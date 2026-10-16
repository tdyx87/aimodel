/* sse.h —— SSE 分帧器
 * 契约：docs/design/contract.md §4.2
 *
 * 纯逻辑，不碰网络。这样「绝不整块缓冲」这条才能被真实测试覆盖，
 * 而不用依赖网络抖动 —— 把传输和分帧混在一起时，
 * 出了问题分不清是网络还是解析。
 *
 * 增量接口：喂多少处理多少，内部只留「未成帧的残片」。
 * 残片长度受帧上限约束（默认 64 KB），因此**内存占用与响应总大小无关**。
 */

#ifndef CAGENT_SSE_H
#define CAGENT_SSE_H

#include <stddef.h>

#include "cagent/buf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 单帧上限：协议帧不应更大，超出即协议异常（契约 §2.1）。 */
#define CA_SSE_MAX_FRAME_BYTES (64u * 1024u)

/* 一帧解析后的字段。指针指向分帧器内部缓冲，
 * **仅在 on_frame 回调期间有效**，回调返回后即失效。
 * 不想拷就别在回调外存指针。
 */
typedef struct {
    const char *event; /* 无则为 NULL */
    const char *data;  /* 无则为 NULL（多个 data: 行以 \n 连接） */
    const char *id;    /* 无则为 NULL */
} ca_sse_frame;

/* 分帧器。内部只有两块定长缓冲，容量在 init 时确定。 */
typedef struct {
    ca_buf acc;  /* 累积中的帧 */
    ca_buf out;  /* 组装好的 data 值 */
    size_t start;   /* acc 中已消费字节数（读游标，避免每帧 memmove 整个缓冲） */
    size_t scan_pos;/* 扫描游标：已知 [start, scan_pos) 内无帧边界 */
    int in_frame;   /* 是否已开始累积一个帧 */
    int failed;     /* 一旦置位，后续 feed 直接拒绝，不再产出帧 */
} ca_sse;

/* 创建。frame_limit 传 0 用 CA_SSE_MAX_FRAME_BYTES；
 * 超过 CA_SSE_MAX_FRAME_BYTES 会被拒绝（上限本身也被封顶，
 * 否则"传入一个很大的上限"就成了绕过封顶的后门）。
 * 分配失败返回 -1。
 */
int ca_sse_init(ca_sse *s, size_t frame_limit);
void ca_sse_free(ca_sse *s);

/* 喂入 n 字节。每凑成一帧就回调 on_frame（可为 NULL 表示丢弃帧内容）。
 * 返回值：
 *    0  正常
 *   -1  帧超限或缓冲写不进去（协议异常，调用方应断开并报错）
 */
int ca_sse_feed(ca_sse *s, const unsigned char *data, size_t n,
                void (*on_frame)(const ca_sse_frame *f, void *user), void *user);

/* 当前累积中的残片字节数——用于测试断言内存受控。
 * 实现上界即帧上限，这是本文件最重要的可观测性。 */
size_t ca_sse_pending(const ca_sse *s);

#ifdef __cplusplus
}
#endif

#endif /* CAGENT_SSE_H */