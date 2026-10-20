/* core.h —— 代理主循环（契约 §3）
 *
 * 职责：组装请求 → 解析响应 → 执行工具 → 回灌结果 → 再来一轮。
 *
 * 依赖方向（契约 §3）：core → {llm, tools, ui} → {http, sse, json, buf} → mem。
 * core 不反向依赖任何下层，也不直接碰 termios / socket / 文件。
 *
 * 三条贯穿始终的原则：
 *
 * 1. **工具执行轮次有硬上限。** 模型可以一直要求调工具；
 *    没有上限的 agent 循环就是一个会烧钱的死循环。
 *
 * 2. **权限判定在执行之前，且用 ui 层统一呈现。**
 *    core 不自己决定「要不要问」，它只负责把
 *    ca_tool_pending_display() 的内容交给用户看。
 *
 * 3. **不做整块缓冲。** 响应经 sse 逐帧、llm 逐事件流上来，
 *    文本直接推给 ui，没有「先攒完再显示」这一步（契约 C1）。
 */
#ifndef CAGENT_CORE_H
#define CAGENT_CORE_H

#include <stddef.h>

#include "cagent/http.h"
#include "cagent/llm.h"
#include "cagent/tools.h"
#include "cagent/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 单次用户输入内，工具执行与回灌的**最大轮次**。
 * 定长且编译期固定——可调的预算等于没有预算。 */
#define CA_CORE_MAX_TOOL_ROUNDS 8

/* 回灌给模型的工具结果上限（契约 §2.1「工具结果单次上限 1 MB」的呼应）。 */
#define CA_CORE_MAX_RESULT_BYTES (256u * 1024u)

/* API key 长度上限。超长一律拒绝，**不截断**——
 * 截断后的 key 只会在服务端换来一个 401，而用户会以为是自己填错了。 */
#define CA_CORE_MAX_KEY_BYTES 512

typedef enum {
    CA_CORE_OK = 0,
    CA_CORE_ERR_ARGS,        /* 配置缺失 */
    CA_CORE_ERR_TRANSPORT,   /* 网络/TLS/超时 */
    CA_CORE_ERR_PROTOCOL,    /* 服务返回非 2xx 或帧无法解析 */
    CA_CORE_ERR_LIMIT,       /* 触到硬上限（工具轮次 / 结果长度 / 参数超限） */
    CA_CORE_ERR_TOOL,        /* 工具执行失败（已在 UI 上向用户说明） */
    CA_CORE_ERR_DENIED,      /* 用户拒绝了工具调用 */
    CA_CORE_ERR_NOMEM
} ca_core_status;

typedef struct {
    const ca_transport_ops *transport; /* 必填；注入而非硬编码，测试才跑得起来 */
    const char *api_base;             /* 必填 */
    const char *model;                /* 必填 */
    /* API key。可为 NULL —— 指向本地无鉴权的服务（如自建代理）。
     * **绝不写进日志、绝不落盘**（PRD NFR-3）。 */
    const char *api_key;
    const char *system;               /* 系统提示，可为 NULL */
    ca_ui *ui;                        /* 必填 */
    ca_workspace *ws;                 /* 工具工作区，必填 */
    ca_perm_cb perm;                  /* 可为 NULL —— 此时按「默认拒绝」处理 */
    void *perm_user;
} ca_core_config;

/* 运行期状态。全部定长，不做无界增长。 */
typedef struct {
    ca_core_config cfg;
    const char *url;        /* 由 api_base 拼出并缓存 */
    char url_buf[512];
    char system_buf[4096];
    /* 预先拼好的 Authorization 头。空串表示不带鉴权。
     * 定长存放，且只在内存里存在——不打印、不落盘。 */
    char auth_header[CA_CORE_MAX_KEY_BYTES + 32];

    /* 本轮累积到的工具调用（由 llm 层填，执行阶段读） */
    ca_llm_tool_slot slots[CA_LLM_MAX_TOOL_CALLS];
    int slot_count;

    /* 工具执行用的上下文。perm 固定为 core_perm：
     * core 必须自己把待批准内容送到用户面前，不能让上层绕过。 */
    ca_tool_ctx tctx;
    ca_out tout;
    char tool_out[CA_TOOL_RESULT_LIMIT];

    /* 本轮工具结果累积（回灌用） */
    char results[CA_CORE_MAX_RESULT_BYTES];
    size_t results_len;

    int rounds;             /* 已用的工具轮次 */
    int saw_text;           /* 本轮是否有增量文本（决定要不要开流） */
    int saw_tool;           /* 本轮是否有工具调用 */
    char last_error[256];
} ca_core;

const char *ca_core_status_text(ca_core_status s);

/* 初始化。不发起任何网络请求。 */
ca_core_status ca_core_init(ca_core *c, const ca_core_config *cfg);

/* 处理一次用户输入，含完整的工具执行循环。
 *
 * 这是主循环的**全部**对外入口：一次调用 = 一轮对话。
 * 返回 OK 不代表模型没调工具，只代表流程走完了。 */
ca_core_status ca_core_run_once(ca_core *c, const char *user_input);

#ifdef __cplusplus
}
#endif

#endif /* CAGENT_CORE_H */