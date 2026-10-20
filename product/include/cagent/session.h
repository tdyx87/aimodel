/* session.h —— 会话历史（契约 §6 / §2.1）
 *
 * 为什么单独成层：历史是本产品里**唯一**会随对话长度增长的东西。
 * 前几轮它涨，第 500 轮它还在涨——如果它没有上限，那么
 * 「内存可预测」这句承诺就只在短会话里成立。所以这里把上限
 * 做成结构的一部分，而不是靠调用方自觉。
 *
 * 三条硬规矩：
 *   1. **字节封顶**（默认 4 MB，见 mem.h 的 CA_LIMIT_HISTORY_BYTES）。
 *      超了就淘汰**最旧的完整轮次**，而不是砍掉半条消息——
 *      半条消息会让模型看到断掉的上下文，比没有上下文更容易胡说。
 *   2. **淘汰必须明示**。丢了多少条、多少字节都记着并由 UI 说出来。
 *      同类工具普遍是自动压缩历史、且不告诉你压掉了什么；
 *      「把取舍摆到明面上」是本产品与它们的差异点（PRD §5.2）。
 *   3. **绝不落盘 API key**（NFR-3）。本层压根没有存放密钥的字段，
 *      所以「忘了排除」这种错误在这里不可能发生。
 *
 * 已知边界（实现期如实记录，不假装支持）：历史只保留**用户消息**
 * 与**助手文本回复**。单次输入内部的工具调用回合（tool_calls / tool
 * 结果）不跨输入保留——见契约 §2.5.6「尚未成立的能力」。
 */

#ifndef CAGENT_SESSION_H
#define CAGENT_SESSION_H

#include <stddef.h>

#include "cagent/llm.h" /* ca_msg_role / ca_llm_msg */
#include "cagent/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 消息条数上限。字节上限之外再设一条，是因为「很多条极短消息」
 * 也能把记录数组塞满——记录数组本身也是内存。 */
#define CA_SESSION_MAX_MSGS 512

#define CA_SESSION_MAX_ID 64

typedef struct {
    unsigned role;  /* ca_msg_role */
    size_t off;     /* 文本在 buf 中的偏移 */
    size_t len;     /* 字节数，不含结尾 NUL */
} ca_session_rec;

typedef struct {
    char *buf;         /* 全部消息文本连续存放 */
    size_t cap;        /* 字节预算 */
    size_t used;       /* 已用字节（每条含结尾 NUL） */
    ca_session_rec recs[CA_SESSION_MAX_MSGS];
    size_t count;

    /* 累计淘汰统计（只增不减） */
    size_t evicted_msgs;
    size_t evicted_bytes;

    /* 最近一次追加/载入时的动作，供 UI 明示后清除 */
    int last_evicted_msgs;
    size_t last_evicted_bytes;
    int last_truncated_msgs;     /* 被截断的消息条数 */
    size_t last_truncated_bytes; /* 其中被丢掉的字节数 */

    char id[CA_SESSION_MAX_ID];
} ca_session;

/* 初始化。budget_bytes 传 0 表示用默认（CA_LIMIT_HISTORY_BYTES）。
 * 返回 0 成功，-1 参数非法，-2 分配失败。 */
int ca_session_init(ca_session *s, size_t budget_bytes);

void ca_session_free(ca_session *s);

/* 追加一条消息（角色 + NUL 结尾文本）。
 * 返回  0 成功；
 *      -1 参数非法；
 *      -2 文本为空（空消息不进历史——它只会白占预算）；
 *      -3 分配失败。
 * 超预算时按轮次淘汰并记录到 last_*，不返回错误：淘汰是**预期行为**。 */
int ca_session_add(ca_session *s, unsigned role, const char *text);

/* 流式追加助手文本：末尾已是助手消息就接上去，否则新开一条。
 * 流式回复是一段段到达的，每段都当成一条消息会把条数上限耗光。 */
int ca_session_append_delta(ca_session *s, const char *text);

size_t ca_session_count(const ca_session *s);
const char *ca_session_text(const ca_session *s, size_t i);
unsigned ca_session_role(const ca_session *s, size_t i);
size_t ca_session_used(const ca_session *s);
size_t ca_session_budget(const ca_session *s);

/* 读出并清除「上一次追加发生了什么」的标记。淘汰或截断过则返回 1。 */
int ca_session_take_notice(ca_session *s, int *evicted_msgs,
                           size_t *evicted_bytes, int *truncated_msgs,
                           size_t *truncated_bytes);

/* 导出为消息数组供 llm 层构造请求。out 至少要有 count 个元素。 */
void ca_session_export(const ca_session *s, ca_llm_msg *out, size_t *out_count);

/* 序列化为单文件自包含 JSON（不含任何密钥字段）。
 * 与 json/llm 一致的两遍语义：buf 为 NULL 只计量；
 * 装不下返回 -2 且不写字节。 */
int ca_session_to_json(const ca_session *s, char *buf, size_t cap,
                       size_t *need_len);

/* 从 JSON 反序列化。返回 0 成功；-1 内容损坏（调用方应跳过并提示，
 * **不得**因此中止启动）；-2 文件超出预算（已按淘汰规则截断后仍载入）。 */
int ca_session_from_json(ca_session *s, const char *text, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CAGENT_SESSION_H */
