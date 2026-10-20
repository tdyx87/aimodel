/* mem.h —— 内存计量与分配预算
 *
 * 契约：docs/design/contract.md §2
 *
 * 本文件是整个程序的最底层，其余所有层都依赖它，且只依赖它。
 * 它不得反向依赖任何上层（契约 §3 依赖方向）。
 *
 * 两条铁律（契约 §2.2）：
 *   C3 禁止无上限的 malloc/realloc 增长 —— 解析大 JSON 时的经典 OOM 来源
 *   C7 除 libcurl 外一切自研 —— 依赖越少越可预测
 */

#ifndef CAGENT_MEM_H
#define CAGENT_MEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 内存计量（契约 §2.4）----
 *
 * 取自操作系统，不是库自己的估算。库估的和系统看到的可能差很远，
 * 而 NFR-1 是对外承诺，必须以系统数字为准。
 */
int64_t ca_rss_bytes(void);       /* 当前 RSS；取不到返回 -1 */
int64_t ca_peak_rss_bytes(void);  /* 峰值 RSS；取不到返回 -1 */

/* 预算检查：当前 RSS 是否已超过给定上限（字节）。
 * 返回 1 表示已超限。用于实现层在关键点自查，而不是等到系统 OOM。
 * limit 传 0 表示不检查（契约 §2.1 的 RSS 硬上限）。
 */
int ca_rss_over(int64_t limit_bytes);

/* ---- 预算常量（契约 §2.1）----
 * 集中定义而非散落各处，避免"上限"在各层被写成不同的数字。
 */
#define CA_LIMIT_FRAME_BYTES (64u * 1024u)        /* 单帧 64 KB */
#define CA_LIMIT_PARSER_BYTES (2u * 1024u * 1024u) /* 解析 arena 2 MB */
#define CA_LIMIT_TOOL_RESULT_BYTES (1u * 1024u * 1024u) /* 工具结果 1 MB */
#define CA_LIMIT_HISTORY_BYTES (4u * 1024u * 1024u)     /* 会话历史 4 MB */
#define CA_LIMIT_FILE_READ_BYTES (4u * 1024u * 1024u)   /* 文件读入 4 MB */

/* 进程 RSS 硬上限 100 MB（PRD NFR-1）。
 *
 * **2026-10-19 由 32 MB 改为 100 MB**（ADR-0003）。原值把 libcurl 的
 * 常驻成本当成 0 —— 实测 dlopen + curl_global_init 就要 6.04 MB，
 * 32 MB 的上限在任何真实配置下都达不成。
 *
 * 注意：**只有这一个数字放宽了**。上面几行的分层预算没有跟着变，
 * 因为它们约束的是「单次操作不得吃掉整个池」，与池子总大小无关。
 */
#define CA_LIMIT_RSS_BYTES (100LL * 1024LL * 1024LL)

/* ---- arena 分配器 ----
 *
 * 用途：请求生命周期内的临时对象（解析出的 JSON、组装的请求体）。
 * 整体释放，不逐个 free —— 既快又不会有"忘了 free"的泄漏，
 * 而泄漏在长驻进程里就是内存缓慢增长的元凶。
 */
typedef struct ca_arena ca_arena;

/* 创建 arena。initial 为初始块字节数，0 表示用默认值。 */
ca_arena *ca_arena_create(size_t initial_bytes);

/* 在 arena 中分配 size 字节。
 * 超出 hard_limit 时返回 NULL 且不推进水位（契约 C3：不隐式扩容）。
 * hard_limit 传 0 表示用 CA_LIMIT_PARSER_BYTES。
 */
void *ca_arena_alloc(ca_arena *a, size_t size);

/* 调整 arena 硬上限。
 *
 * 收紧（降低或等值）是允许的 —— 它只会让预算更严，方向上安全，
 * 且在长会话中逐步收紧是有用的手段。
 *
 * 放松（抬高）只允许抬到全局封顶 CA_LIMIT_PARSER_BYTES 为止 ——
 * 否则「抬高上限」就成了一条绕过 C3 的后门。
 *
 * 返回 1 表示上限已被接受（收紧或合法抬高），0 表示被拒绝
 * （抬得过高）。
 */
int ca_arena_set_limit(ca_arena *a, size_t new_limit);

/* 释放 arena 及其所有块。传 NULL 是安全的空操作。 */
void ca_arena_destroy(ca_arena *a);

/* 查询：已用字节 / 当前硬上限字节 */
size_t ca_arena_used(const ca_arena *a);
size_t ca_arena_limit(const ca_arena *a);

#ifdef __cplusplus
}
#endif

#endif /* CAGENT_MEM_H */