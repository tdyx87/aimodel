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
#include <stdlib.h>
#include <string.h>

#include "cagent/json.h"
#include "cagent/llm.h"
#include "cagent/sse.h"

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

size_t ca_core_context_bytes(const ca_core *c) {
    return (c == NULL) ? 0 : c->body_cap;
}

void ca_core_free(ca_core *c) {
    if (c == NULL) return;
    free(c->body);
    c->body = NULL;
    c->body_cap = 0;
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

    /* 请求体缓冲放在**所有校验之后**：这样失败路径不会留下已分配的
     * 缓冲，也不用在每个 return 前面加一次 free。
     *
     * 钳制到 [16 KB, 4 MB]，0 取默认。钳制而不是报错——配置写大了
     * 不该让代理起不来，但也不能悄悄分配一个远超内存预算的缓冲。
     * 实际值由 ca_core_context_bytes() 暴露。 */
    size_t cap = cfg->context_bytes;
    if (cap == 0) cap = CA_CORE_DEFAULT_CONTEXT_BYTES;
    if (cap < CA_CORE_MIN_CONTEXT_BYTES) cap = CA_CORE_MIN_CONTEXT_BYTES;
    if (cap > CA_CORE_MAX_CONTEXT_BYTES) cap = CA_CORE_MAX_CONTEXT_BYTES;
    c->body = (char *)malloc(cap);
    if (c->body == NULL) {
        core_note(c, "请求缓冲分配失败");
        return CA_CORE_ERR_NOMEM;
    }
    c->body_cap = cap;
    /* cfg 里的原值不再有意义：后面只认 body_cap。 */
    c->cfg.context_bytes = cap;
    return CA_CORE_OK;
}

/* ---- 一次请求-响应 ---- */

typedef struct {
    ca_core *core;
    ca_llm *llm;
    ca_sse *sse;
    int frame_error;
    /* 服务端用 4xx 回普通 JSON 错误体时，SSE 分帧器会判「无法解析」，
     * 真正的原因（额度不足、模型过载……）就在这段字节里。
     * 只留前 255 字节：**有界**，不违反契约 C1（不缓存整段响应）。 */
    char raw[256];
    size_t raw_len;
} turn_ctx;

static void on_event(const ca_event *ev, void *user) {
    turn_ctx *t = (turn_ctx *)user;
    ca_core *c = t->core;
    if (ev == NULL) return;

    switch (ev->kind) {
        case CA_EV_TEXT_DELTA: {
            const char *piece = ev->text != NULL ? ev->text : "";
            /* 第一次有文本才开流——纯工具调用的响应不该先打一个空行。 */
            if (!c->saw_text) {
                ca_ui_stream_begin(c->cfg.ui);
                c->saw_text = 1;
            }
            ca_ui_stream_token(c->cfg.ui, piece);
            /* 富客户端（Web UI）需要知道「这是模型说的」，
             * 而不是从终端文本里猜。 */
            if (c->cfg.on_event != NULL) {
                ca_core_event cev;
                cev.kind = CA_CORE_EV_TEXT;
                cev.text = piece;
                cev.name = NULL;
                cev.status = NULL;
                c->cfg.on_event(&cev, c->cfg.on_event_user);
            }
            /* 同一段文本同时进历史。**只在这一处**记录助手输出，
             * 免得出现「显示的是 A、记下的是 B」这种两套真相。 */
            if (c->cfg.session != NULL) {
                (void)ca_session_append_delta(c->cfg.session, piece);
            }
            break;
        }

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

/* 从服务端的错误体里取出可读原因。
 * 覆盖 {"error":{"code":"…","message":"…"}} 这种常见形状；
 * 取不到 JSON 就原样给一小段（换行压成空格，别把界面刷乱）。
 * 返回 0 表示「没拿到有用信息」，由调用方用通用文案。 */
static int core_error_reason(const char *raw, char *out, size_t cap) {
    if (raw == NULL || cap < 2) return 0;
    const char *p = strstr(raw, "\"message\"");
    if (p != NULL) {
        p = strchr(p, ':');
        if (p != NULL) {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '"') {
                p++;
                size_t i = 0;
                while (*p != '\0' && *p != '"' && i + 1 < cap) {
                    if (*p == '\\' && p[1] != '\0') {
                        p++;
                        if (*p == 'n' || *p == 't' || *p == 'r') { out[i++] = ' '; p++; continue; }
                    }
                    out[i++] = *p++;
                }
                out[i] = '\0';
                return i > 0;
            }
        }
    }
    size_t i = 0;
    while (raw[i] != '\0' && i + 1 < cap) {
        char ch = raw[i];
        out[i] = (ch == '\n' || ch == '\r' || ch == '\t') ? ' ' : ch;
        i++;
    }
    out[i] = '\0';
    return i > 0;
}

/* 传输层的数据回调：**只喂分帧器，不做任何缓冲**。
 * 这是契约 C1 在 core 层的落点——这里若开始缓存，内存就等于响应大小。
 *
 * 唯一的例外是下面那 255 字节的 `raw`：定长、够用即停，
 * 目的是让 4xx 错误体里的原因能被说出来（真实案例：服务端返回
 * 429 + {"message":"模型过载"}，界面却只说「无法解析的内容」）。 */
static int on_data(const unsigned char *data, size_t n, void *user) {
    turn_ctx *t = (turn_ctx *)user;
    if (t->frame_error != 0) return 1; /* 已在出错 → 要求传输中止 */
    if (t->raw_len + n <= sizeof(t->raw) - 1) {
        memcpy(t->raw + t->raw_len, data, n);
        t->raw_len += n;
        t->raw[t->raw_len] = '\0';
    }
    if (ca_sse_feed(t->sse, data, n, on_frame, t) != 0) {
        t->frame_error = -1;
        return 1;
    }
    return 0;
}

/* 构造本轮请求体。
 *
 * user_in_history=1 表示 user_text **已经在会话历史里**（本轮的首次请求，
 * 我们刚把它记进去），此时只带历史；
 * =0 表示 user_text 是历史之外的一条（工具结果回灌），追加在历史之后。
 *
 * 这个区分是必要的：把同一条用户输入既放进历史又当作尾随消息，
 * 模型会看到重复的问题，回答也会开始重复。
 *
 * 历史放不下时不在这里处理——core_one_turn 会从**最旧的一端**丢弃
 * 完整轮次后重试。构造器只负责如实报告「需要多少字节」。
 */
static void core_collect_msgs(ca_core *c, const char *user_text,
                              int user_in_history, ca_llm_msg *msgs,
                              size_t *out_n) {
    /* +1 给尾随的工具结果消息。栈上约 8 KB，不放进 ca_core
     * ——ca_core 已经很大，再塞进去只会让「别把 ca_core 放栈上」更危险。 */
    static const size_t kMaxMsgs = CA_SESSION_MAX_MSGS + 1;
    size_t n = 0;
    if (c->cfg.session != NULL) {
        ca_session_export(c->cfg.session, msgs, &n);
        if (n > CA_SESSION_MAX_MSGS) n = CA_SESSION_MAX_MSGS; /* 防御，不可能发生 */
    }
    if (n == 0) {
        /* 无会话（单轮模式），或历史为空（例如空输入没被记进历史）：
         * 这一条就是全部上下文。**不能**因为 user_in_history=1 就什么都不放——
         * 空请求体会让每一轮都报「超出上下文上限」。 */
        msgs[0].role = CA_MSG_USER;
        msgs[0].text = user_text;
        n = 1;
    } else if (!user_in_history) {
        if (n >= kMaxMsgs) n = kMaxMsgs - 1;
        msgs[n].role = CA_MSG_USER;
        msgs[n].text = user_text;
        n++;
    }
    *out_n = n;
}

/* 从 drop 开始往后找到**下一个完整轮次的起点**（user 消息）。
 * 直接丢一条 assistant 回复会留下「问而不答」的残局，模型会接着
 * 自言自语；按轮次对齐才是安全的淘汰单位。 */
static size_t core_next_turn(const ca_llm_msg *msgs, size_t n, size_t drop) {
    size_t i = drop + 1;
    while (i < n && msgs[i].role != CA_MSG_USER) i++;
    return i;
}

/* 传输层记下的 HTTP 状态码（拿不到就当 0）。 */
static long core_http_status(const ca_core *c) {
    if (c->cfg.transport == NULL || c->cfg.transport->last_status_code == NULL) {
        return 0;
    }
    return c->cfg.transport->last_status_code();
}

/* 失败原因必须带出服务端说的话。
 * 只报 fallback（「请求失败」「无法解析的内容」）等于把服务端唯一
 * 有用的信息丢掉：用户分不清是额度不足、模型过载还是 key 过期。
 *
 * 格式串保持纯 ASCII：MSVC 对含中文的格式串会误报 C4474（参数过多），
 * 而零告警是硬门禁——中文一律作为参数传。 */
static void core_note_http_error(ca_core *c, long status, const char *raw,
                                 const char *fallback) {
    char reason[160];
    if (status >= 400 && core_error_reason(raw, reason, sizeof(reason))) {
        char msg[256];
        snprintf(msg, sizeof(msg), "HTTP %ld: %s", status, reason);
        core_note(c, msg);
        return;
    }
    core_note(c, fallback);
}

/* 传输层失败（连不上 / TLS 不对 / 超时 / 被中止）时给出**可行动**的说明。
 *
 * 真实故障现场：域名拼错或网络不通时，界面只显示「请求失败」——
 * 用户完全不知道该改地址、改代理还是改证书。必须把原因和目标地址一起
 * 说出来（地址只含主机部分，不含 key）。
 *
 * ca_http_status_text() 已经把这些情形翻译成人话，这里再补上主机名。 */
static void core_note_transport_error(ca_core *c, ca_http_status hs) {
    const char *base = ca_http_status_text(hs);
    char host[128];
    host[0] = '\0';
    if (c->url != NULL) {
        const char *p = strstr(c->url, "://");
        p = (p != NULL) ? p + 3 : c->url;
        size_t i = 0;
        while (p[i] != '\0' && p[i] != '/' && p[i] != '?' && i + 1 < sizeof(host)) {
            host[i] = p[i];
            i++;
        }
        host[i] = '\0';
    }
    if (host[0] != '\0') {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s（目标：%s）", base, host);
        core_note(c, msg);
    } else {
        core_note(c, base);
    }
}

static ca_core_status core_one_turn(ca_core *c, const char *user_text,
                                    int user_in_history) {
    ca_llm_msg msgs[CA_SESSION_MAX_MSGS + 1];
    size_t nmsg = 0;
    core_collect_msgs(c, user_text, user_in_history, msgs, &nmsg);

    size_t drop = 0;
    size_t need = 0;
    for (;;) {
        /* 工具声明随请求一起发。没有它，真实服务上的模型只会聊天，
         * 永远不会发出 tool_calls（§4.3 / US-3）。 */
        int rc = ca_llm_build_request_full(c->body, c->body_cap, c->cfg.model,
                                           c->cfg.system, msgs + drop,
                                           nmsg - drop, ca_tools_schema_json(),
                                           &need);
        if (rc != -2) {
            if (rc == -1) { core_note(c, "请求参数非法"); return CA_CORE_ERR_ARGS; }
            break;
        }
        /* 缓冲不足：丢掉最旧的完整轮次再试。
         * 没有会话（单轮模式）时无可丢弃，直接报错。 */
        if (c->cfg.session == NULL) {
            drop = nmsg;
            break;
        }
        size_t next = core_next_turn(msgs, nmsg, drop);
        if (next >= nmsg) {
            /* 只剩最后一轮（或只有一条消息）也放不下：这是**单轮内容**
             * 超限，不是历史太长。此时截断请求体只会让模型看到半截输入，
             * 明确报错并给出可操作的建议才对。 */
            drop = nmsg;
            break;
        }
        drop = next;
    }
    if (drop >= nmsg) {
        char msg[192];
        /* 建议必须和当前模式对得上：单轮模式（`--prompt`）里没有 `/new`，
         * 告诉用户敲一个不存在的命令等于把人往死路上指。 */
        if (c->cfg.session != NULL) {
            snprintf(msg, sizeof(msg),
                     "单轮内容超出上下文上限 %lu 字节（需要 %lu 字节），"
                     "请缩短输入，或用 /new 开新会话。",
                     (unsigned long)c->body_cap, (unsigned long)need);
        } else {
            snprintf(msg, sizeof(msg),
                     "单轮内容超出上下文上限 %lu 字节（需要 %lu 字节），"
                     "请缩短输入。",
                     (unsigned long)c->body_cap, (unsigned long)need);
        }
        core_note(c, msg);
        return CA_CORE_ERR_LIMIT;
    }
    if (drop > 0 && !c->context_notified) {
        /* 与契约 T3 一致：淘汰必须说出来。只报一次（本轮第一次需要淘汰时），
         * 之后继续淘汰不再重复刷屏；会话层自己的淘汰另有提示。 */
        c->context_notified = 1;
        ca_ui_printf(c->cfg.ui,
                     "（上下文超出 %lu 字节上限，已丢弃最早的 %lu 条消息）\n",
                     (unsigned long)c->body_cap, (unsigned long)drop);
    }

    const char *body = c->body;

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
    t.raw[0] = '\0'; t.raw_len = 0;

    /* llm 状态必须跨轮次保留：工具调用的参数是分片累积的，
     * 每一轮都是新响应，重新 create 才对。 */
    ca_request hreq;
    memset(&hreq, 0, sizeof(hreq));
    hreq.method = "POST";
    hreq.url = c->url;
    hreq.body = body;
    hreq.body_len = strlen(body);
    hreq.headers = (const char *const *)hdr;

    /* 光标先在「等待响应」上，收到第一段输出时自动让位（仅 TTY）。
     * 管道/重定向下两者都是空操作，输出照样能被脚本消费。 */
    ca_ui_wait_begin(c->cfg.ui);
    ca_http_status hs = c->cfg.transport->run(&hreq, on_data, &t);
    ca_ui_wait_end(c->cfg.ui);

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
        core_note_http_error(c, core_http_status(c), t.raw,
                             "服务返回了无法解析的内容");
        return CA_CORE_ERR_PROTOCOL;
    }
    if (hs != CA_HTTP_OK) {
        /* 真实传输层在 4xx 时返回 ERR_PROTOCOL，**不会**置 frame_error
         * （错误体不像 SSE，分帧器直接跳过）——所以这条分支才是
         * 「额度不足/模型过载」真正走到的地方。 */
        if (c->last_error[0] == '\0') {
            if (hs == CA_HTTP_ERR_PROTOCOL) {
                long status = core_http_status(c);
                char reason[160];
                if (status >= 400 &&
                    core_error_reason(t.raw, reason, sizeof(reason))) {
                    /* 服务端给了可读原因（额度不足、模型过载、key 过期…） */
                    char msg[256];
                    snprintf(msg, sizeof(msg), "HTTP %ld: %s", status, reason);
                    core_note(c, msg);
                } else if (status >= 400) {
                    /* 4xx 但没有可读原因（常见于网关错误页）：
                     * 至少把状态码说出来，别只剩一句「请求失败」。 */
                    char msg[128];
                    snprintf(msg, sizeof(msg), "HTTP %ld %s", status,
                             "（服务端没有返回可读原因）");
                    core_note(c, msg);
                } else {
                    core_note(c, "服务返回了无法解析的内容");
                }
            } else {
                /* 连接/TLS/超时/中止：说清楚是哪一种，并带上目标主机。 */
                core_note_transport_error(c, hs);
            }
        }
        return (hs == CA_HTTP_ERR_PROTOCOL) ? CA_CORE_ERR_PROTOCOL
                                             : CA_CORE_ERR_TRANSPORT;
    }
    return CA_CORE_OK;
}

/* 把「历史被淘汰/被截断」这件事明确告诉用户。
 * 数字必须给出：只说「历史过长已截断」等于没说清楚丢了什么。 */
static void core_report_session_notice(ca_core *c) {
    if (c->cfg.session == NULL) return;
    int em = 0, tm = 0;
    size_t eb = 0, tb = 0;
    if (!ca_session_take_notice(c->cfg.session, &em, &eb, &tm, &tb)) return;
    if (em > 0) {
        ca_ui_printf(c->cfg.ui,
                     "（会话历史已达上限，丢弃最早的 %d 条消息，共 %lu 字节）\n",
                     em, (unsigned long)eb);
    }
    if (tm > 0) {
        ca_ui_printf(c->cfg.ui, "（有 %d 条消息过长被截断，共丢弃 %lu 字节）\n",
                     tm, (unsigned long)tb);
    }
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
    /* 顺序：**展示内容 → 查策略 → 才问用户**。
     *
     * 早先的顺序是「展示 → 问用户 → 查策略」，于是策略没开时
     * 用户会被问一个注定被拒的问题：按下 y 之后仍然不执行，
     * 界面上还写着「用户拒绝了执行命令」——其实用户同意了，
     * 是规则不允许。两处都在骗人。
     *
     * 策略拒绝时**不再问**：问了也是白问，还白白消耗用户一次按键
     * （而这个 y 会被当成「我批准过了」的错觉）。
     * 但内容照样展示：用户有权知道代理想干什么。 */
    ca_ui_show_pending(c->cfg.ui, tool_name, display);
    if (c->cfg.on_event != NULL) {
        ca_core_event cev;
        cev.kind = CA_CORE_EV_APPROVAL;
        cev.text = display;
        cev.name = tool_name;
        cev.status = "pending";
        c->cfg.on_event(&cev, c->cfg.on_event_user);
    }
    ca_permission policy = c->cfg.perm(tool_name, c->cfg.perm_user);
    if (policy != CA_PERM_ALLOW) {
        c->deny_by_policy = (policy == CA_PERM_DENY_POLICY) ? 1 : 0;
        return policy;
    }
    c->deny_by_policy = 0;
    ca_ui_decision d = ca_ui_ask(c->cfg.ui, tool_name);
    if (d != CA_UI_ALLOW) return CA_PERM_DENY;
    return CA_PERM_ALLOW;
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

/* 工具结果回灌时必须说明「工具已经跑完了」。
 *
 * 真实模型上的现场：GLM 收到工具结果（以 user 消息回灌）后，
 * 会**原样再要一次同一个工具**——因为它看到的是「用户又发了一段话」，
 * 而不是「你刚才那次调用的结果」。于是同一个写文件请求被反复请求，
 * 用户要一次次批准。
 *
 * 明确告诉它「工具已执行、这是结果、请据此继续」，重复调用明显减少。
 * 这不是万能药：OpenAI 规范里的 `role:"tool"` + `tool_call_id` 握手
 * 才是根本解（见 audit 的遗留项），但在当前实现下这句提示是必需的。
 *
 * 措辞必须区分「执行了」和「被拒了」：被拒的工具**没有执行**，
 * 说成「已执行」会让模型以为文件已经写好了——那正是最坏的情况。 */
static void results_append_notice(ca_core *c, const char *tool_name,
                                  ca_tool_status ts) {
    char note[320];
    /* 格式串纯 ASCII：MSVC 对含中文的格式串会误报 C4474，零告警是硬门禁。 */
    if (ts == CA_TOOL_DENIED) {
        snprintf(note, sizeof(note),
                 "<tool_result tool=\"%s\" status=\"denied\">"
                 "The user declined this call, so the tool did NOT run. "
                 "Do not retry the same call; explain what you would do "
                 "instead or ask the user how to proceed.</tool_result>\n",
                 tool_name != NULL ? tool_name : "");
    } else {
        snprintf(note, sizeof(note),
                 "<tool_result tool=\"%s\" status=\"done\">"
                 "The tool has already been executed. "
                 "Use this result to continue; do not call the same tool again "
                 "unless the result shows it failed.</tool_result>\n",
                 tool_name != NULL ? tool_name : "");
    }
    results_append(c, note);
}

/* 把工具名映射到具体的执行函数。未知的工具名一律拒绝执行——
 * 模型可能幻觉出一个不存在的工具，那是它的错，不是我们的。 */
static ca_tool_status run_tool(ca_core *c, const char *name, const char *args) {
    if (strcmp(name, "read_file") == 0) return ca_tool_read_file(&c->tctx, args);
    if (strcmp(name, "write_file") == 0) return ca_tool_write_file(&c->tctx, args);
    if (strcmp(name, "list_dir") == 0) return ca_tool_list_dir(&c->tctx, args);
    if (strcmp(name, "run_shell") == 0) return ca_tool_run_shell(&c->tctx, args);
    if (strcmp(name, "edit_file") == 0) return ca_tool_edit_file(&c->tctx, args);
    if (strcmp(name, "search") == 0) return ca_tool_search_json(&c->tctx, args);
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
    c->context_notified = 0;

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
    /* 本轮首次请求：用户输入已经记进历史，请求体里不再重复追加。 */
    int user_in_history = 1;

    if (c->cfg.session != NULL) {
        /* 记不进历史（空串）不影响本轮问答本身，照常继续。 */
        (void)ca_session_add(c->cfg.session, CA_MSG_USER, user_input);
    }

    for (;;) {
        c->saw_text = 0;
        c->saw_tool = 0;
        c->slot_count = 0;

        ca_core_status st = core_one_turn(c, turn_input, user_in_history);
        if (st != CA_CORE_OK) {
            ca_ui_line(c->cfg.ui, "");
            {
                /* 格式串纯 ASCII：MSVC 对含中文的格式串会误报 C4474，
                 * 而「零告警」是 CI 硬门禁。中文一律作为参数传。 */
                char msg[320];
                snprintf(msg, sizeof(msg), "%s%s", "错误：",
                         c->last_error[0] ? c->last_error : ca_core_status_text(st));
                ca_ui_error(c->cfg.ui, msg);
                if (c->cfg.on_event != NULL) {
                    ca_core_event cev;
                    cev.kind = CA_CORE_EV_ERROR;
                    cev.text = c->last_error[0] ? c->last_error
                                                : ca_core_status_text(st);
                    cev.name = NULL;
                    cev.status = NULL;
                    c->cfg.on_event(&cev, c->cfg.on_event_user);
                }
            }
            return st;
        }
        ca_ui_line(c->cfg.ui, "");
        /* 历史满了必须说出来。同类工具普遍在背后悄悄压缩历史，
         * 用户以为上下文还在——「丢了会说」是本产品的差异点。 */
        core_report_session_notice(c);

        if (c->slot_count == 0) {
            if (c->cfg.on_event != NULL) {
                ca_core_event cev;
                cev.kind = CA_CORE_EV_DONE;
                cev.text = NULL;
                cev.name = NULL;
                cev.status = NULL;
                c->cfg.on_event(&cev, c->cfg.on_event_user);
            }
            return CA_CORE_OK; /* 纯文本回答，本轮结束 */
        }

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

            if (!c->cfg.ui->is_tty) {
                /* 非 TTY 保持原来的一行文本（CI 友好）。 */
                ca_ui_printf(c->cfg.ui, "\n[调用工具 %s]\n", s->name);
            }
            if (c->cfg.on_event != NULL) {
                ca_core_event cev;
                cev.kind = CA_CORE_EV_TOOL;
                cev.text = NULL;
                cev.name = s->name;
                cev.status = "calling";
                c->cfg.on_event(&cev, c->cfg.on_event_user);
            }
            c->tout.len = 0;
            ca_tool_status ts = run_tool(c, s->name, s->args);

            /* 结果**也要显示给用户**。只回灌给模型的话，
             * 用户就是在一个黑盒里看着代理改他的文件。
             * 「代理做了什么」必须是可见的，不只是事后可查。 */
            if (c->tout.len > 0) {
                /* 结果**放进面板**：先前是「空盒子 + 框外一段文字」，
                 * 看起来像渲染坏了。标题写明是哪个工具的结果。 */
                if (c->cfg.ui->is_tty) {
                    char title[160];
                    snprintf(title, sizeof(title), "调用工具 %s · 结果", s->name);
                    ca_ui_panel(c->cfg.ui, title, c->tool_out);
                } else {
                    ca_ui_line(c->cfg.ui, c->tool_out);
                }
            }
            if (c->cfg.on_event != NULL) {
                ca_core_event cev;
                cev.kind = CA_CORE_EV_TOOL;
                cev.text = (c->tout.len > 0) ? c->tool_out : NULL;
                cev.name = s->name;
                cev.status = (ts == CA_TOOL_OK)
                                 ? "ok"
                                 : ((ts == CA_TOOL_DENIED) ? "denied" : "error");
                c->cfg.on_event(&cev, c->cfg.on_event_user);
            }

            if (ts == CA_TOOL_DENIED) {
                denied = 1;
                /* 明确告知。用户按下 n 之后必须有反馈，
                 * 否则他无法确认自己到底批准了没有。 */
                ca_ui_warn(c->cfg.ui,
                           c->deny_by_policy
                               ? "按策略拒绝该工具调用（用户即使批准也不会执行），未做任何改动。"
                               : "已拒绝该工具调用，未做任何改动。");
            } else if (ts != CA_TOOL_OK) {
                ca_ui_warn(c->cfg.ui, "（该工具未成功，结果见上）");
            }
            if (c->tout.len > 0) results_append(c, c->tool_out);
            /* 附一句说明工具的状态：真实模型收到裸结果会原样再要一次
             * 同一个工具，用户被迫反复批准。措辞区分「已执行」与「被拒绝」。 */
            results_append_notice(c, s->name, ts);
        }

        ca_ui_line(c->cfg.ui, "");
        turn_input = c->results;
        /* 工具结果不在会话历史里，下一轮作为**尾随消息**追加。 */
        user_in_history = 0;
        if (c->results_len == 0) {
            /* 没有结果可回灌，硬转下一轮只会让模型重复要同样的工具。 */
            return denied ? CA_CORE_ERR_DENIED : CA_CORE_OK;
        }
    }
}