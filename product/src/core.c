/* core.c —— 代理主循环（契约 §3）
 *
 * 全流程：
 *   组装请求 → 传输 → SSE 分帧 → LLM 事件 → 文本直推 ui
 *              → 工具调用 → 权限确认 → 执行 → 结果回灌 → 再一轮
 *
 * 三条贯穿始终：
 *   1. 响应文本**边到边显示**，没有「先攒完再显示」这一步
 *   2. 工具轮次有硬上限——模型可以一直调工具，没有上限就是烧钱死循环
 *   3. 权限判定在执行之前，且用 ui 层统一呈现（core 不自己决定「要不要问」）
 */

#include "cagent/core.h"

#include <stdio.h>
#include <string.h>

#include "cagent/json.h"
#include "cagent/llm.h"
#include "cagent/sse.h"

#define CA_CORE_MAX_REQUEST 16384

const char *ca_core_status_text(ca_core_status s) {
    switch (s) {
        case CA_CORE_OK: return "完成";
        case CA_CORE_ERR_ARGS: return "配置不完整";
        case CA_CORE_ERR_TRANSPORT: return "网络或服务不可达";
        case CA_CORE_ERR_PROTOCOL: return "服务返回了无法解析的内容";
        case CA_CORE_ERR_LIMIT: return "触到硬上限，已中止";
        case CA_CORE_ERR_TOOL: return "工具执行失败";
        case CA_CORE_ERR_DENIED: return "用户拒绝了工具调用";
        case CA_CORE_ERR_NOMEM: return "内存不足";
    }
    return "未知状态";
}

static void core_note(ca_core *c, const char *msg) {
    size_t n = strlen(msg);
    if (n >= sizeof(c->last_error)) n = sizeof(c->last_error) - 1;
    memcpy(c->last_error, msg, n);
    c->last_error[n] = '\0';
}

ca_core_status ca_core_init(ca_core *c, const ca_core_config *cfg) {
    if (c == NULL || cfg == NULL) return CA_CORE_ERR_ARGS;
    if (cfg->transport == NULL || cfg->api_base == NULL || cfg->model == NULL ||
        cfg->ui == NULL || cfg->ws == NULL) {
        return CA_CORE_ERR_ARGS;
    }
    memset(c, 0, sizeof(*c));
    c->cfg = *cfg;

    int n = snprintf(c->url_buf, sizeof(c->url_buf), "%s/chat/completions",
                     cfg->api_base);
    if (n <= 0 || (size_t)n >= sizeof(c->url_buf)) {
        core_note(c, "api_base 太长");
        return CA_CORE_ERR_ARGS;
    }
    c->url = c->url_buf;

    /* 系统提示拷进定长缓冲：调用方释放之后不留悬垂指针。
     * 这类悬垂在测试里往往不复现，在长时间运行的进程里会。 */
    if (cfg->system != NULL) {
        size_t sn = strlen(cfg->system);
        if (sn >= sizeof(c->system_buf)) {
            core_note(c, "系统提示过长");
            return CA_CORE_ERR_ARGS;
        }
        memcpy(c->system_buf, cfg->system, sn + 1);
        c->cfg.system = c->system_buf;
    }

    /* 鉴权头在 init 时拼好：key 不会变，没必要每轮重拼。
     *
     * 这里**只做长度检查，不做任何形式化校验**——key 的格式由服务商定义，
     * 我们猜测它合法与否只会误伤。
     *
     * 注意计算的是整个 "Authorization: Bearer <key>" 的长度，
     * 而不是 key 单独的长度：前缀本身占 22 字节，只查 key 会算漏。
     */
    c->auth_header[0] = '\0';
    if (cfg->api_key != NULL && cfg->api_key[0] != '\0') {
        size_t kn = strlen(cfg->api_key);
        if (kn > CA_CORE_MAX_KEY_BYTES) {
            core_note(c, "API key 过长");
            return CA_CORE_ERR_ARGS;
        }
        int an = snprintf(c->auth_header, sizeof(c->auth_header),
                          "Authorization: Bearer %s", cfg->api_key);
        if (an <= 0 || (size_t)an >= sizeof(c->auth_header)) {
            core_note(c, "API key 过长");
            return CA_CORE_ERR_ARGS;
        }
    }
    return CA_CORE_OK;
}

/* ---- 一次请求-响应 ---- */

typedef struct {
    ca_core *core;
    ca_llm *llm;
    ca_sse *sse;
    int frame_error;
} turn_ctx;

static void on_event(const ca_event *ev, void *user) {
    turn_ctx *t = (turn_ctx *)user;
    ca_core *c = t->core;
    if (ev == NULL) return;

    switch (ev->kind) {
        case CA_EV_TEXT_DELTA:
            /* 第一次有文本才开流——纯工具调用的响应不该先打一个空行。 */
            if (!c->saw_text) {
                ca_ui_stream_begin(c->cfg.ui);
                c->saw_text = 1;
            }
            ca_ui_stream_token(c->cfg.ui, ev->text != NULL ? ev->text : "");
            break;

        case CA_EV_TOOL_CALL_BEGIN:
            /* 有工具调用意味着这轮不是纯文本。先收尾已经流出的文本，
             * 否则后面显示的工具信息会接在同一行里。 */
            if (c->saw_text) ca_ui_stream_end(c->cfg.ui);
            c->saw_text = 0;
            c->saw_tool = 1;
            break;

        case CA_EV_ERROR:
            core_note(c, ev->text != NULL ? ev->text : "服务返回了错误");
            break;

        default:
            break;
    }
}

static void on_frame(const ca_sse_frame *f, void *user) {
    turn_ctx *t = (turn_ctx *)user;
    if (f == NULL || f->data == NULL) return;
    int rc = ca_llm_on_frame(t->llm, f->data, strlen(f->data), on_event, t);
    /* 非 0：协议无法解析或触到硬上限。带着半截数据继续执行工具是危险的，
     * 标记后由外层决定中止。 */
    if (rc != 0) t->frame_error = rc;
}

/* 传输层的数据回调：**只喂分帧器，不做任何缓冲**。
 * 这是契约 C1 在 core 层的落点——这里若开始缓存，内存就等于响应大小。 */
static int on_data(const unsigned char *data, size_t n, void *user) {
    turn_ctx *t = (turn_ctx *)user;
    if (t->frame_error != 0) return 1; /* 已在出错 → 要求传输中止 */
    if (ca_sse_feed(t->sse, data, n, on_frame, t) != 0) {
        t->frame_error = -1;
        return 1;
    }
    return 0;
}

static ca_core_status core_one_turn(ca_core *c, const char *user_text) {
    char body[CA_CORE_MAX_REQUEST];
    size_t need = 0;

    ca_llm_request req;
    req.api_base = c->cfg.api_base;
    req.model = c->cfg.model;
    req.system = c->cfg.system;
    req.user = user_text;

    int rc = ca_llm_build_request(body, sizeof(body), &req, &need);
    if (rc == -1) { core_note(c, "请求参数非法"); return CA_CORE_ERR_ARGS; }
    if (rc == -2) {
        /* 缓冲不足：明确报需要多少，**不截断**。
         * 半个请求体会被服务端当成协议错误，报错还更难懂。 */
        char msg[128];
        snprintf(msg, sizeof(msg), "请求体需要 %lu 字节，上限 %u。",
                 (unsigned long)need, (unsigned)CA_CORE_MAX_REQUEST);
        core_note(c, msg);
        return CA_CORE_ERR_LIMIT;
    }

    /* 鉴权头由 init 拼好，可能为空。
     * 空时把这一项写成 NULL —— 传输层按「遇到 NULL 即结束」遍历，
     * 所以它必须排在最后。
     *
     * 逐项赋值而不是写成一个初始化列表：列表里含 `c->auth_header`
     * 这样的非常量表达式时，MSVC 在 C 模式下报 C4204
     * （「使用了非标准扩展：非常量聚合初始化」）。
     * 末尾的转换是安全的：接口声明为只读，传进去不会被修改。 */
    const char *hdr[4];
    hdr[0] = "Content-Type: application/json";
    hdr[1] = "Accept: text/event-stream";
    hdr[2] = (c->auth_header[0] != '\0') ? c->auth_header : NULL;
    hdr[3] = NULL;

    ca_llm *llm = ca_llm_create();
    if (llm == NULL) return CA_CORE_ERR_NOMEM;
    ca_sse sse;
    if (ca_sse_init(&sse, 0) != 0) { ca_llm_free(llm); return CA_CORE_ERR_NOMEM; }

    turn_ctx t;
    t.core = c; t.llm = llm; t.sse = &sse; t.frame_error = 0;

    /* llm 状态必须跨轮次保留：工具调用的参数是分片累积的，
     * 每一轮都是新响应，重新 create 才对。 */
    ca_request hreq;
    memset(&hreq, 0, sizeof(hreq));
    hreq.method = "POST";
    hreq.url = c->url;
    hreq.body = body;
    hreq.body_len = strlen(body);
    hreq.headers = (const char *const *)hdr;

    ca_http_status hs = c->cfg.transport->run(&hreq, on_data, &t);

    /* 保存本轮的工具槽位，供执行阶段读取。
     * 注意**必须计数**——早先拷了槽位却忘了 slot_count，
     * 于是 slot_count 恒为 0，下面的「本轮有工具调用吗」永远答否，
     * 工具执行整段被静默跳过。 */
    for (unsigned i = 0; i < CA_LLM_MAX_TOOL_CALLS; i++) {
        if (llm->tools[i].active) {
            c->slots[c->slot_count++] = llm->tools[i];
        }
    }

    ca_sse_free(&sse);
    ca_llm_free(llm);
    if (c->saw_text) ca_ui_stream_end(c->cfg.ui);

    if (t.frame_error != 0) {
        core_note(c, "服务返回了无法解析的内容");
        return CA_CORE_ERR_PROTOCOL;
    }
    if (hs != CA_HTTP_OK) {
        if (c->last_error[0] == '\0') core_note(c, "请求失败");
        return (hs == CA_HTTP_ERR_PROTOCOL) ? CA_CORE_ERR_PROTOCOL
                                             : CA_CORE_ERR_TRANSPORT;
    }
    return CA_CORE_OK;
}

/* ---- 工具执行 ---- */

/* 权限回调：把 tools 层给的 pending_display 原样交给 ui 呈现。
 * cfg->perm 为 NULL 时（未配置权限回调）**一律拒绝**（契约 P1 默认拒绝）。 */
static ca_permission core_perm(const char *tool_name, void *user) {
    ca_core *c = (ca_core *)user;
    const char *display = ca_tool_pending_display(&c->tctx, tool_name);
    if (c->cfg.perm == NULL) {
        ca_ui_confirm(c->cfg.ui, tool_name, display);
        return CA_PERM_DENY;
    }
    /* 有自定义回调时，先把待批准内容呈现出来——
     * 否则自定义回调很容易绕过 P5。 */
    ca_ui_decision d = ca_ui_confirm(c->cfg.ui, tool_name, display);
    if (d != CA_UI_ALLOW) return CA_PERM_DENY;
    return c->cfg.perm(tool_name, c->cfg.perm_user);
}

static void results_append(ca_core *c, const char *s) {
    size_t n = strlen(s);
    if (c->results_len + n + 1 > sizeof(c->results)) {
        /* 静默截断会让模型基于半截结果下结论。
         * 明确标注，与工具结果的截断约定一致。 */
        static const char tail[] = "\n[工具结果已截断：超过上限]";
        size_t keep = sizeof(c->results) - sizeof(tail);
        if (c->results_len < keep) {
            c->results_len = keep;
            memcpy(c->results + keep - sizeof(tail) + 1, tail, sizeof(tail) - 1);
        }
        return;
    }
    memcpy(c->results + c->results_len, s, n + 1);
    c->results_len += n;
}

/* 把工具名映射到具体的执行函数。未知的工具名一律拒绝执行——
 * 模型可能幻觉出一个不存在的工具，那是它的错，不是我们的。 */
static ca_tool_status run_tool(ca_core *c, const char *name, const char *args) {
    if (strcmp(name, "read_file") == 0) return ca_tool_read_file(&c->tctx, args);
    if (strcmp(name, "write_file") == 0) return ca_tool_write_file(&c->tctx, args);
    if (strcmp(name, "list_dir") == 0) return ca_tool_list_dir(&c->tctx, args);
    if (strcmp(name, "run_shell") == 0) return ca_tool_run_shell(&c->tctx, args);
    if (strcmp(name, "edit_file") == 0) return ca_tool_edit_file(&c->tctx, args);
    ca_out_puts(&c->tout, "<result status=\"error\" error=\"不认识的工具\">"
                           "本代理没有这个工具。请只用已有工具。</result>");
    return CA_TOOL_ERR_ARGS;
}

ca_core_status ca_core_run_once(ca_core *c, const char *user_input) {
    if (c == NULL || user_input == NULL) return CA_CORE_ERR_ARGS;
    c->results_len = 0;
    c->results[0] = '\0';
    c->rounds = 0;
    c->slot_count = 0;
    c->last_error[0] = '\0';

    /* 工具输出缓冲：1 MB（与契约 §2.1 一致） */
    if (ca_out_init(&c->tout, c->tool_out, sizeof(c->tool_out)) != 0) {
        return CA_CORE_ERR_NOMEM;
    }
    c->tctx.ws = c->cfg.ws;
    c->tctx.out = &c->tout;
    c->tctx.perm = core_perm;
    c->tctx.perm_user = c;
    c->tctx.pending_display = NULL;

    const char *turn_input = user_input;
    int denied = 0;

    for (;;) {
        c->saw_text = 0;
        c->saw_tool = 0;
        c->slot_count = 0;

        ca_core_status st = core_one_turn(c, turn_input);
        if (st != CA_CORE_OK) {
            ca_ui_line(c->cfg.ui, "");
            ca_ui_printf(c->cfg.ui, "错误：%s\n",
                         c->last_error[0] ? c->last_error : ca_core_status_text(st));
            return st;
        }
        ca_ui_line(c->cfg.ui, "");

        if (c->slot_count == 0) return CA_CORE_OK; /* 纯文本回答，本轮结束 */

        /* 工具轮次有硬上限 */
        if (c->rounds >= CA_CORE_MAX_TOOL_ROUNDS) {
            ca_ui_line(c->cfg.ui, "已达到工具轮次上限，停止执行更多工具。");
            return CA_CORE_ERR_LIMIT;
        }
        c->rounds++;

        c->results_len = 0;
        c->results[0] = '\0';

        for (int i = 0; i < c->slot_count; i++) {
            const ca_llm_tool_slot *s = &c->slots[i];

            /* 参数超限时**拒绝执行**：执行半截参数等于做模型没要求的事。 */
            if (s->args_overflow) {
                ca_ui_line(c->cfg.ui, "工具参数超过上限，已拒绝执行。");
                denied = 1;
                continue;
            }
            if (s->args_len == 0 || s->name[0] == '\0') continue;

            ca_ui_printf(c->cfg.ui, "\n[调用工具 %s]\n", s->name);
            c->tout.len = 0;
            ca_tool_status ts = run_tool(c, s->name, s->args);

            /* 结果**也要显示给用户**。只回灌给模型的话，
             * 用户就是在一个黑盒里看着代理改他的文件。
             * 「代理做了什么」必须是可见的，不只是事后可查。 */
            if (c->tout.len > 0) ca_ui_line(c->cfg.ui, c->tool_out);

            if (ts == CA_TOOL_DENIED) {
                denied = 1;
                /* 明确告知。用户按下 n 之后必须有反馈，
                 * 否则他无法确认自己到底批准了没有。 */
                ca_ui_line(c->cfg.ui, "已拒绝该工具调用，未做任何改动。");
            } else if (ts != CA_TOOL_OK) {
                ca_ui_line(c->cfg.ui, "（该工具未成功，结果见上）");
            }
            if (c->tout.len > 0) results_append(c, c->tool_out);
        }

        ca_ui_line(c->cfg.ui, "");
        turn_input = c->results;
        if (c->results_len == 0) {
            /* 没有结果可回灌，硬转下一轮只会让模型重复要同样的工具。 */
            return denied ? CA_CORE_ERR_DENIED : CA_CORE_OK;
        }
    }
}