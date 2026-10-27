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

void ca_core_set_trace(ca_core *c, int on) {
    if (c == NULL) return;
    c->cfg.trace = on ? 1 : 0;
}

int ca_core_get_trace(const ca_core *c) {
    if (c == NULL) return 0;
    return c->cfg.trace ? 1 : 0;
}

void ca_core_set_trace_raw(ca_core *c, int on) {
    if (c == NULL) return;
    c->cfg.trace_raw = on ? 1 : 0;
}

int ca_core_get_trace_raw(const ca_core *c) {
    if (c == NULL) return 0;
    return c->cfg.trace_raw ? 1 : 0;
}

ca_core_status ca_core_init(ca_core *c, const ca_core_config *cfg) {
    if (c == NULL || cfg == NULL) return CA_CORE_ERR_ARGS;
    if (cfg->transport == NULL || cfg->ui == NULL || cfg->ws == NULL) {
        return CA_CORE_ERR_ARGS;
    }
    memset(c, 0, sizeof(*c));
    c->cfg = *cfg;

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

    /* Web 模式下 api_base/model 可先为空，由 /api/config 后续设置；
     * 非 Web 模式下两者必填，否则无从发请求。 */
    if (cfg->api_base != NULL && cfg->api_base[0] != '\0') {
        if (core_rebuild_url(c, cfg->api_base) != 0) {
            core_note(c, "api_base 太长");
            return CA_CORE_ERR_ARGS;
        }
    }
    if (cfg->model != NULL && cfg->model[0] != '\0') {
        size_t n = strlen(cfg->model);
        if (n >= sizeof(c->model_buf)) {
            core_note(c, "模型名过长");
            return CA_CORE_ERR_ARGS;
        }
        memcpy(c->model_buf, cfg->model, n + 1);
        c->cfg.model = c->model_buf;
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

    /* 工具结果的回灌上限必须**由请求体容量倒推**，不能是固定值。
     *
     * 真实故障：回灌缓冲固定 256 KB，而请求体默认也是 256 KB
     * （还可能被配到 16 KB 下限）。于是「工具结果填满回灌缓冲」
     * 必然意味着「请求体装不下」——不是截断，而是**整轮失败**，
     * 用户看到的是「单轮内容超出上下文上限」。
     * 把 context 配小一点，代理就彻底不可用了。
     *
     * 正确做法：给「非工具结果」的部分预留空间，剩下的才是结果配额。
     * 预留 = 工具声明 + 系统提示 + 一段余量（JSON 包装、用户输入、
     * assistant 的 tool_calls 回带）。
     *
     * 截断器（契约 C4）的作用是「保证请求还发得出去」，
     * 不是「尽量多塞」——塞不下就发不出去。 */
    size_t reserve = 8u * 1024u; /* JSON 包装 + 用户输入 + tool_calls 回带的余量 */
    reserve += strlen(ca_tools_schema_json());
    reserve += strlen(c->system_buf);
    if (reserve + 1024u > cap) {
        /* 空间小到连一份工具结果都放不下：配额取 0，
         * 工具结果会被完整标注为「已截断」，而不是让请求失败。 */
        c->results_cap = 0;
    } else {
        c->results_cap = cap - reserve;
        if (c->results_cap > sizeof(c->results)) {
            c->results_cap = sizeof(c->results);
        }
    }
    return CA_CORE_OK;
}

/* 把 api_base 拼成完整的请求地址（url_buf），供 ca_core_run_once 用。
 * 空串表示“不发请求”（Web 模式尚未配置）。 */
static int core_rebuild_url(ca_core *c, const char *api_base) {
    if (api_base == NULL || api_base[0] == '\0') {
        c->url = NULL;
        c->url_buf[0] = '\0';
        return 0;
    }
    int n = snprintf(c->url_buf, sizeof(c->url_buf), "%s/chat/completions",
                     api_base);
    if (n <= 0 || (size_t)n >= sizeof(c->url_buf)) return -1;
    c->url = c->url_buf;
    return 0;
}

/* 运行时重配服务端地址 / 模型 / 密钥（Web UI 配置用）。
 *
 * 调用后下一轮 ca_core_run_once 会使用新的地址与密钥发请求。
 * 会话历史、传输层、权限策略等保持不变。
 *
 * api_base 为 NULL 或空串时不改 url；model 为 NULL 或空串时不改模型；
 * api_key 为 NULL 时不改鉴权头（这样可以只改模型而不改密钥）。
 *
 * 返回 0 成功；-1 参数非法（c 为 NULL 或 api_base 太长）。 */
int ca_core_reconfig(ca_core *c, const char *api_base, const char *model,
                     const char *api_key) {
    if (c == NULL) return -1;

    if (api_base != NULL && api_base[0] != '\0') {
        if (core_rebuild_url(c, api_base) != 0) return -1;
        c->cfg.api_base = c->url_buf; /* 指向自己持有的缓冲，避免悬垂 */
    }

    if (model != NULL && model[0] != '\0') {
        size_t n = strlen(model);
        if (n >= sizeof(c->model_buf)) return -1;
        memcpy(c->model_buf, model, n + 1);
        c->cfg.model = c->model_buf;
    }

    if (api_key != NULL && api_key[0] != '\0') {
        size_t kn = strlen(api_key);
        if (kn > CA_CORE_MAX_KEY_BYTES) return -1;
        int an = snprintf(c->auth_header, sizeof(c->auth_header),
                          "Authorization: Bearer %s", api_key);
        if (an <= 0 || (size_t)an >= sizeof(c->auth_header)) return -1;
    } else if (api_key != NULL && api_key[0] == '\0') {
        /* 显式传空串：取消鉴权。 */
        c->auth_header[0] = '\0';
    }

    return 0;
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
            /* 同时攒一份**完整文本**：流式输出是「正在打字」的效果，
             * 屏幕上没法整段复制。用户要的是一份可复制的完整输出。 */
            if (c->cfg.trace) {
                size_t n = strlen(piece);
                if (c->answer_len + n + 1 <= sizeof(c->answer)) {
                    memcpy(c->answer + c->answer_len, piece, n + 1);
                    c->answer_len += n;
                } else if (!c->answer_truncated) {
                    /* 显式标注，不静默截断（与工具结果的约定一致）。 */
                    c->answer_truncated = 1;
                }
            }
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
    /* 原样转储服务端返回的每一帧。
     *
     * 为什么打印**原始帧**而不是「解析后的文本」：
     * 模型说的话已经被流式打印到界面上了（ca_ui_stream_token），
     * 再打印一遍是噪音。真正看不到的是**协议层**的东西——
     * tool_calls 的 id、arguments 的分片、服务端的错误体。
     * 那些只有原始帧里有。 */
    if (t->core->cfg.trace_raw) {
        char title[96];
        /* 格式串纯 ASCII：MSVC 对含中文的格式串会误报 C4474，
         * 而零告警是 CI 硬门禁。中文一律作为参数传。 */
        snprintf(title, sizeof(title), "%s %s [%lu %s]", "<-", "服务端帧",
                 (unsigned long)strlen(f->data), "字节");
        ca_ui_dump(t->core->cfg.ui, title, f->data);
    }
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
 * =0 表示 user_text 是历史之外的一条（仅用于无会话单轮模式的兜底）。
 *
 * 这个区分是必要的：把同一条用户输入既放进历史又当作尾随消息，
 * 模型会看到重复的问题，回答也会开始重复。
 *
 * 历史之后追加的是**标准工具握手**的回带消息（若处于工具循环中）：
 * 一条 assistant（带 tool_calls 原文）+ 若干 role:"tool" 的结果。
 * 这些不在会话历史里（契约 §2.5.6：工具回合不跨输入保留），
 * 只在当前这次输入的循环内有效。
 *
 * 历史放不下时不在这里处理——core_one_turn 会从**最旧的一端**丢弃
 * 完整轮次后重试。构造器只负责如实报告「需要多少字节」。
 */
static void core_collect_msgs(ca_core *c, const char *user_text,
                              int user_in_history, ca_llm_msg *msgs,
                              size_t *out_n) {
    /* 上限：会话消息 + assistant tool_calls + 每条结果 + 可能的尾随 user。 */
    static const size_t kMaxMsgs =
        CA_SESSION_MAX_MSGS + 1 + CA_LLM_MAX_TOOL_CALLS + 1;
    size_t n = 0;
    if (c->cfg.session != NULL) {
        ca_session_export(c->cfg.session, msgs, &n);
        if (n > CA_SESSION_MAX_MSGS) n = CA_SESSION_MAX_MSGS; /* 防御，不可能发生 */
    }
    if (n == 0 && !c->replay_open) {
        /* 无会话（单轮模式），或历史为空（例如空输入没被记进历史）：
         * 这一条就是全部上下文。**不能**因为 user_in_history=1 就什么都不放——
         * 空请求体会让每一轮都报「超出上下文上限」。 */
        msgs[0].role = CA_MSG_USER;
        msgs[0].text = user_text;
        msgs[0].tool_calls_json = NULL;
        msgs[0].tool_call_id = NULL;
        n = 1;
    } else if (n == 0 && c->replay_open) {
        /* 单轮模式的工具握手：请求里仍要先带上**本轮的原始用户输入**，
         * 否则只剩「assistant 调工具 + tool 结果」，没有问题可答。 */
        msgs[0].role = CA_MSG_USER;
        msgs[0].text = (c->turn_user != NULL) ? c->turn_user : "";
        msgs[0].tool_calls_json = NULL;
        msgs[0].tool_call_id = NULL;
        n = 1;
    } else if (!user_in_history && !c->replay_open && n > 0) {
        /* 历史之外的一条**用户**消息。工具循环走的是 replay 分支，
         * 不会再走到这里——避免把工具结果当用户消息发出去。 */
        if (n >= kMaxMsgs) n = kMaxMsgs - 1;
        msgs[n].role = CA_MSG_USER;
        msgs[n].text = user_text;
        msgs[n].tool_calls_json = NULL;
        msgs[n].tool_call_id = NULL;
        n++;
    }

    /* 标准工具握手回带：assistant(tool_calls) + 逐条 tool 结果。 */
    if (c->replay_open && n < kMaxMsgs) {
        msgs[n].role = CA_MSG_ASSISTANT;
        /* assistant 的文本部分：工具轮次里通常为空（模型直接调工具）。
         * 用空串而非 NULL——序列化器据「text 为空」决定写 content:null。 */
        msgs[n].text = "";
        msgs[n].tool_calls_json = c->replay_tool_calls;
        msgs[n].tool_call_id = NULL;
        n++;
    }
    for (size_t i = 0; i < c->tool_msg_count && n < kMaxMsgs; i++) {
        msgs[n].role = CA_MSG_TOOL;
        msgs[n].text = c->results + c->tool_msgs[i].off;
        msgs[n].tool_calls_json = NULL;
        msgs[n].tool_call_id = c->tool_msgs[i].id;
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
    /* 会话上限 + assistant tool_calls + 各条 tool 结果 + 尾随 user。 */
    ca_llm_msg msgs[CA_SESSION_MAX_MSGS + 1 + CA_LLM_MAX_TOOL_CALLS + 1];
    size_t nmsg = 0;
    core_collect_msgs(c, user_text, user_in_history, msgs, &nmsg);

    /* 回带块（assistant tool_calls + tool 结果）不允许被部分丢弃：
     * 拆散它们会让服务端看到「有 tool 结果却没有对应调用」而 400。
     * 因此可丢弃的只到 hist_count 为止，回带块要么整块保留，要么整轮报错。 */
    size_t replay_n = (c->replay_open ? 1 : 0) + c->tool_msg_count;
    size_t hist_count = (nmsg >= replay_n) ? (nmsg - replay_n) : 0;

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
        if (c->cfg.session == NULL || drop >= hist_count) {
            drop = nmsg;
            break;
        }
        size_t next = core_next_turn(msgs, nmsg, drop);
        /* 只允许丢到回带块之前；越过它就没有可丢的历史了。 */
        if (next >= hist_count) {
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

    /* 转储**真正发出去的**请求体。
     *
     * 为什么在这一刻、而不是构造完就打印：请求体可能被历史淘汰机制
     * 改写（丢掉最旧的完整轮次后重试），只有**最终**这一份才是
     * 服务端真正收到的。打印早于它的版本就是在骗人。
     *
     * 请求体里**没有** API key（key 在 Authorization 头里），
     * 所以原样打印不会泄露密钥。 */
    if (c->cfg.trace) {
        char title[192];
        /* 格式串纯 ASCII：MSVC 对含中文的格式串会误报 C4474，
         * 而零告警是 CI 硬门禁。中文一律作为参数传。 */
        snprintf(title, sizeof(title), "%s %s %s %lu %s", "->", "请求", c->url,
                 (unsigned long)strlen(body), "字节");
        ca_ui_dump(c->cfg.ui, title, body);
    }

    /* 本轮的模型输出从零开始攒。 */
    c->answer_len = 0;
    c->answer[0] = '\0';
    c->answer_truncated = 0;

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
    /* Web 模式下尚未配置服务端地址时，url 为 NULL：
     * 不得发请求，返回明确错误，提示用户先在 Web UI 设置。 */
    if (c->url == NULL) {
        ca_ui_wait_end(c->cfg.ui);
        core_note(c, "未配置服务端地址：请在 Web UI 的设置里填写 URL、模型与密钥后再发送消息");
        return CA_CORE_ERR_ARGS;
    }

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

    /* 本轮模型输出的**完整文本**。
     *
     * 为什么在这里而不是流式打印时：流式是「正在打字」的效果，
     * 屏幕上留不下可复制的完整文本。用户要的是「把输出全部打印出来」——
     * 一份完整、能整段拷走的副本。
     *
     * 纯工具调用的一轮没有文本，这时**不打印**（避免刷一串空标题）。 */
    if (c->cfg.trace && c->answer_len > 0) {
        char title[160];
        /* 格式串纯 ASCII：MSVC 对含中文的格式串会误报 C4474，
         * 而零告警是 CI 硬门禁。中文一律作为参数传。 */
        snprintf(title, sizeof(title), "%s %s [%lu %s]", "<-", "模型输出",
                 (unsigned long)c->answer_len, "字节");
        ca_ui_dump(c->cfg.ui, title, c->answer);
        if (c->answer_truncated) {
            /* 超出上限必须**说出来**：一份被悄悄截断的「完整输出」
             * 比没有更糟——用户会以为看到了全部。 */
            ca_ui_line(c->cfg.ui, "（上面的模型输出超过展示上限，已截断）");
        }
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

/* ---- 标准工具握手的结果消息构造 ---- */

/* 开始一条新的 role:"tool" 结果消息：登记它的 tool_call_id 与文本起点。
 * 返回该消息在 c->tool_msgs 中的下标，满了返回 -1。 */
static int tool_msg_begin(ca_core *c, const char *id) {
    if (c->tool_msg_count >= CA_LLM_MAX_TOOL_CALLS) return -1;
    size_t i = c->tool_msg_count++;
    /* id 为空时给一个占位：服务端要求 tool 消息必须有 tool_call_id，
     * 而某些服务不发 id（或我们没拿到）。空 id 会让整条请求 400，
     * 不如用一个稳定的占位，至少语义上「对应这个序号」。 */
    if (id != NULL && id[0] != '\0') {
        size_t n = strlen(id);
        if (n >= sizeof(c->tool_msgs[i].id)) n = sizeof(c->tool_msgs[i].id) - 1;
        memcpy(c->tool_msgs[i].id, id, n);
        c->tool_msgs[i].id[n] = '\0';
    } else {
        snprintf(c->tool_msgs[i].id, sizeof(c->tool_msgs[i].id), "call_%lu",
                 (unsigned long)i);
    }
    c->tool_msgs[i].off = c->results_len;
    c->tool_msgs[i].len = 0;
    return (int)i;
}

/* 截断标注。**格式串纯 ASCII**：MSVC 对含中文的格式串会误报 C4474，
 * 而零告警是 CI 硬门禁——中文一律作为参数传。 */
#define CA_TRUNC_NOTE "工具结果已截断：超过上限"

/* 往当前工具结果消息尾追加文本。超上限时**显式标注**（不静默截断）。
 *
 * 两条硬规矩：
 *   1. 结果缓冲里的每条消息都必须**NUL 结尾**——序列化器按 C 字符串读它，
 *      没有 NUL 就会一路读出缓冲区（越界读）。
 *   2. `results_len` **只增不减**：它是各条消息的偏移基准，
 *      一旦被回缩到某条消息起点之前，那条消息的长度就会下溢成天文数字。
 *      早先的截断逻辑正是这么写的（既没写 NUL，也可能回缩），
 *      这里一并修掉。 */
static void tool_msg_append(ca_core *c, size_t idx, const char *s) {
    if (idx >= c->tool_msg_count) return;
    size_t n = strlen(s);
    size_t avail = c->results_cap;
    if (avail > sizeof(c->results)) avail = sizeof(c->results);

    char tail[128];
    int tn = snprintf(tail, sizeof(tail), "\n[%s %lu 字节]", CA_TRUNC_NOTE,
                      (unsigned long)avail);
    if (tn < 0 || (size_t)tn >= sizeof(tail)) {
        tail[0] = '\0';
        tn = 0;
    }
    size_t tlen = (size_t)tn;

    if (avail == 0) {
        /* 配额为 0（请求体小到放不下任何结果）：只留标注，不写正文。
         * 标注必须写在**本条消息自己的**位置上——否则多条消息会互相覆盖，
         * 后一条的指针会指到前一条的内容上。 */
        if (c->results_len + tlen + 1 <= sizeof(c->results)) {
            memcpy(c->results + c->results_len, tail, tlen + 1);
            c->results_len += tlen;
            c->tool_msgs[idx].len = tlen;
        }
        return;
    }
    if (c->results_len + n + 1 > avail) {
        /* 静默截断会让模型基于半截结果下结论。
         * 明确标注，与工具结果的截断约定一致。 */
        /* 还能写多少字节（给结尾 NUL 留一个）。 */
        size_t room = avail - c->results_len - 1;
        if (room > tlen) {
            /* 放得下标注：正文砍掉一部分，末尾写标注。 */
            size_t keep_text = room - tlen;
            if (keep_text < n) n = keep_text;
            memcpy(c->results + c->results_len, s, n);
            c->results_len += n;
            memcpy(c->results + c->results_len, tail, tlen);
            c->results_len += tlen;
        } else if (room > 0) {
            /* 连标注都放不下：只写能写的部分，并**显式**留一个省略标记。 */
            memcpy(c->results + c->results_len, s, room);
            c->results_len += room;
        }
        c->results[c->results_len] = '\0';
        c->tool_msgs[idx].len = c->results_len - c->tool_msgs[idx].off;
        return;
    }
    memcpy(c->results + c->results_len, s, n + 1);
    c->results_len += n;
    c->tool_msgs[idx].len = c->results_len - c->tool_msgs[idx].off;
}

/* 工具结果回灌时必须说明「工具已经跑完了」。
 *
 * 标准握手（role:"tool" + tool_call_id）已经从结构上解决了大部分
 * 「模型不知道这是结果」的困惑，但措辞依然有用：它明确区分
 * 「执行了」与「被拒了」——被拒的工具**没有执行**，
 * 说成「已执行」会让模型以为文件已经写好了，那正是最坏的情况。
 *
 * 措辞必须区分「执行了」和「被拒了」。 */
static void tool_msg_append_notice(ca_core *c, size_t idx, const char *tool_name,
                                   ca_tool_status ts) {
    char note[320];
    /* 格式串纯 ASCII：MSVC 对含中文的格式串会误报 C4474，零告警是硬门禁。 */
    if (ts == CA_TOOL_DENIED) {
        snprintf(note, sizeof(note),
                 "\n<tool_result tool=\"%s\" status=\"denied\">"
                 "The user declined this call, so the tool did NOT run. "
                 "Do not retry the same call; explain what you would do "
                 "instead or ask the user how to proceed.</tool_result>\n",
                 tool_name != NULL ? tool_name : "");
    } else {
        snprintf(note, sizeof(note),
                 "\n<tool_result tool=\"%s\" status=\"done\">"
                 "The tool has already been executed. "
                 "Use this result to continue; do not call the same tool again "
                 "unless the result shows it failed.</tool_result>\n",
                 tool_name != NULL ? tool_name : "");
    }
    tool_msg_append(c, idx, note);
}

/* 用本轮的工具槽位拼出 assistant 消息的 tool_calls 数组原文。
 *
 * 逐字回带 id / name / arguments —— 服务端会拿它跟当初发来的做一致性
 * 校验，任何「重新序列化」导致的变化（键序、转义）都可能被拒。
 * 只写规范要求的三个键：id / type / function{name,arguments}。 */
static int build_replay_tool_calls(ca_core *c) {
    char *buf = c->replay_tool_calls;
    /* 留一个字节给结尾的 NUL：ca_json_write_raw 只保证不写出 cap，
     * 若把 cap 原样传进去，len 可以正好等于 cap，
     * 随后的 buf[len]='\0' 就是**越界写一个字节**。 */
    size_t cap = sizeof(c->replay_tool_calls) - 1;
    size_t len = 0;
    int first = 1;
    buf[0] = '\0';
    /* **必须是数组**：[{"id":...},{"id":...}]
     * 早先漏了方括号，产出的是 {"id":...} —— 那是个**对象**，
     * 服务端会直接 400。端到端断言（看真实请求体）才抓得到：
     * 手写带方括号的单元测试永远看不出这个错。 */
    if (ca_json_write_raw(buf, cap, &len, "[") != 0) return -1;
    for (int i = 0; i < c->slot_count; i++) {
        const ca_llm_tool_slot *s = &c->slots[i];
        /* 只回带**真的执行过**的调用（有名字、参数不超限）：
         * 参数超限的压根没执行，回带它等于告诉模型「这是你的调用结果」，
         * 但下面并没有对应的 tool 消息，服务端会因缺结果而 400。 */
        if (s->args_overflow || s->name[0] == '\0' || s->args_len == 0) continue;
        if (!first) {
            if (ca_json_write_raw(buf, cap, &len, ",") != 0) return -1;
        }
        first = 0;
        if (ca_json_write_raw(buf, cap, &len, "{\"id\":") != 0) return -1;
        if (ca_json_write_string(buf, cap, &len, s->id) != 0) return -1;
        if (ca_json_write_raw(buf, cap, &len, ",\"type\":\"function\",\"function\":{\"name\":") != 0)
            return -1;
        if (ca_json_write_string(buf, cap, &len, s->name) != 0) return -1;
        if (ca_json_write_raw(buf, cap, &len, ",\"arguments\":") != 0) return -1;
        if (ca_json_write_string(buf, cap, &len, s->args) != 0) return -1;
        if (ca_json_write_raw(buf, cap, &len, "}}") != 0) return -1;
    }
    if (len == 0 || first) return -1; /* 没有任何可回带的调用 */
    if (ca_json_write_raw(buf, cap, &len, "]") != 0) return -1;
    buf[len] = '\0';
    return 0;
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

    /* 无会话（单轮）模式下，工具握手轮次的请求里必须**仍带这一轮的
     * 用户输入**——否则请求会变成「assistant 调工具 + tool 结果」，
     * 完全没有问题可答。会话模式下它已在历史里，不需要这里兜底。 */
    c->turn_user = user_input;

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
        c->tool_msg_count = 0;
        c->replay_open = 0;

        for (int i = 0; i < c->slot_count; i++) {
            const ca_llm_tool_slot *s = &c->slots[i];

            /* 参数超限时**拒绝执行**：执行半截参数等于做模型没要求的事。
             * 必须补一条 tool 结果回灌，否则模型会反复用同一个调用重试。 */
            if (s->args_overflow) {
                ca_ui_line(c->cfg.ui, "工具参数超过上限，已拒绝执行。");
                denied = 1;
                int mi = tool_msg_begin(c, s->id);
                if (mi >= 0) {
                    tool_msg_append(c, (size_t)mi,
                                    "<result status=\"error\" error=\"参数超过上限\">"
                                    "工具参数超过上限，已拒绝执行。</result>");
                    tool_msg_append_notice(c, (size_t)mi, s->name, CA_TOOL_DENIED);
                }
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

            /* 标准握手：本次调用的结果成为一条 role:"tool" 消息，
             * 带 tool_call_id 指向它回应的那次调用。 */
            int mi = tool_msg_begin(c, s->id);
            if (mi < 0) continue; /* 结果消息数达上限（不可能，槽位已封顶） */
            if (c->tout.len > 0) tool_msg_append(c, (size_t)mi, c->tool_out);
            /* 附一句状态说明：措辞区分「已执行」与「被拒绝」。 */
            tool_msg_append_notice(c, (size_t)mi, s->name, ts);
        }

        ca_ui_line(c->cfg.ui, "");

        /* 没有任何结果可回灌，硬转下一轮只会让模型重复要同样的工具。 */
        if (c->tool_msg_count == 0) {
            return denied ? CA_CORE_ERR_DENIED : CA_CORE_OK;
        }

        /* 拼出助手那条 tool_calls 消息。拼不出来（理论上不会发生）
         * 就退回「结束本轮」，绝不发一条没有对应调用的 tool 结果——
         * 服务端会因缺 tool_calls 而 400，而界面上只看到「请求失败」。 */
        if (build_replay_tool_calls(c) != 0) {
            ca_ui_line(c->cfg.ui, "（工具回灌失败：无法生成 tool_calls，跳过本轮回带）");
            return denied ? CA_CORE_ERR_DENIED : CA_CORE_OK;
        }
        c->replay_open = 1;
        /* 下一步是工具握手，不再有尾随用户消息。 */
        turn_input = "";
        user_in_history = 0;
    }
}