/* llm.h —— 服务适配层
 * 契约：docs/design/contract.md §4.3
 *
 * 本层把「各家协议长得不一样」这件事**完全封在里面**，
 * 上层只看到一种统一事件流。US-8「多服务兼容」的实现基础就在这。
 *
 * 【内存上的关键取舍】工具调用的 arguments 是**分片累积**的：
 * 服务端每来一帧就追加几字节，全部到齐才能执行。因此它天然是
 * 「随响应增长」的地方——本层用**定长槽位 + 显式上限**把它压住：
 *   - 槽位数量固定（超出即拒绝并报错，不动态扩容）
 *   - 每个槽位的参数长度有上限（超出即标记 overflow 并报错，不静默截断）
 * 静默截断的参数会被当成合法 JSON 去执行，那是极危险的行为。
 */

#ifndef CAGENT_LLM_H
#define CAGENT_LLM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 统一事件类型（契约 §4.3）。上层不得知道底层是哪家协议。 */
typedef enum {
    CA_EV_TEXT_DELTA,      /* 一段增量文本 */
    CA_EV_TOOL_CALL_BEGIN, /* 工具调用开始（拿到 id 与名字） */
    CA_EV_TOOL_CALL_ARGS,  /* 工具参数增量片段（可能分多片） */
    CA_EV_TOOL_CALL_END,   /* 参数收齐 */
    CA_EV_USAGE,           /* token 用量（如服务提供） */
    CA_EV_ERROR,           /* 服务端返回的错误 */
    CA_EV_DONE             /* 响应结束 */
} ca_event_kind;

/* 一次事件。指针字段仅在回调期间有效，不得保存。 */
typedef struct {
    ca_event_kind kind;
    const char *text;       /* TEXT_DELTA：增量文本；ERROR：错误信息 */
    size_t tool_index;      /* 工具调用序号（对应协议里的 index） */
    const char *tool_id;    /* TOOL_CALL_BEGIN：调用 id */
    const char *tool_name;  /* TOOL_CALL_BEGIN：工具名 */
    const char *args;       /* TOOL_CALL_ARGS：本次追加的参数片段 */
    size_t args_total;      /* TOOL_CALL_ARGS：累积后总长 */
    long long prompt_tokens;
    long long completion_tokens;
} ca_event;

typedef void (*ca_llm_on_event)(const ca_event *ev, void *user);

/* 定长限制。全部在编译期固定，运行期不可调——可调的预算等于没有预算。 */
#define CA_LLM_MAX_TOOL_CALLS 8
#define CA_LLM_MAX_ARGS_BYTES (32u * 1024u)
#define CA_LLM_MAX_ID 64
#define CA_LLM_MAX_NAME 64
#define CA_LLM_MAX_TEXT 256

typedef struct {
    int active;
    unsigned index;
    char id[CA_LLM_MAX_ID];
    char name[CA_LLM_MAX_NAME];
    char args[CA_LLM_MAX_ARGS_BYTES];
    size_t args_len;
    int args_overflow; /* 已超过上限；**不截断**，执行前必须拒绝 */
    int announced;     /* 是否已发出 BEGIN */
} ca_llm_tool_slot;

typedef struct {
    ca_llm_tool_slot tools[CA_LLM_MAX_TOOL_CALLS];
    int finished;  /* 是否已见到 [DONE] */
} ca_llm;

/* 创建/销毁。 */
ca_llm *ca_llm_create(void);
void ca_llm_free(ca_llm *llm);

/* 喂入一帧的 data 部分（不含 "data:" 前缀）。
 * 返回  0 正常；
 *      -1 协议无法解析；
 *      -2 触到硬上限（工具调用过多 / 参数超长 / id 或名字过长）。
 *         此时该次响应应当**中止**，不能带着半截数据继续执行工具。
 */
int ca_llm_on_frame(ca_llm *llm, const char *data, size_t len,
                    ca_llm_on_event on_event, void *user);

/* 取出某个工具调用累积到的完整参数。返回 NULL 表示序号不存在。 */
const char *ca_llm_tool_args(const ca_llm *llm, unsigned index, size_t *len,
                             int *overflow);

/* ---- 请求构造 ---- */

typedef struct {
    const char *api_base; /* 例 "https://api.openai.com/v1"，必需 */
    const char *model;    /* 必需 */
    const char *system;   /* 系统提示，可为 NULL */
    const char *user;     /* 用户输入，必需 */
} ca_llm_request;

/* 把请求体写进定长缓冲。
 * 返回  0 成功；
 *      -1 参数非法（缺必需字段）；
 *      -2 缓冲不足（**不截断**，所需的完整长度写入 need_len）。
 */
int ca_llm_build_request(char *buf, size_t cap, const ca_llm_request *req,
                         size_t *need_len);

#ifdef __cplusplus
}
#endif

#endif /* CAGENT_LLM_H */