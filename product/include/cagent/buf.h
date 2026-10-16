/* buf.h —— 定长字节缓冲
 * 契约：docs/design/contract.md §2.3
 *
 * 只有一种缓冲：容量在创建时定死，写不进去就**报错**。
 *
 * 刻意不提供的能力：
 *   - 不自动扩容（契约 C3：禁止无上限增长）
 *   - 不静默截断（截断会让半个数据看起来像完整数据，
 *     这类 bug 表现为"偶尔解析出奇怪的东西"，极难查）
 *
 * 「宁可报错，不可截断」是本层唯一的原则。
 */

#ifndef CAGENT_BUF_H
#define CAGENT_BUF_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned char *data;
    size_t cap;  /* 容量，创建时定死 */
    size_t len;  /* 当前有效长度 */
} ca_buf;

/* 创建容量为 cap 字节的缓冲。cap 传 0 视为非法（返回失败）——
 * 容量为 0 的缓冲一定是调用方算错了，让它当场失败比留到写入时。 */
int ca_buf_init(ca_buf *b, size_t cap);

/* 释放。传 NULL 或未初始化的 buf 是安全的空操作。 */
void ca_buf_free(ca_buf *b);

/* 清空长度，内容不抹零（抹零是有意义的额外开销）。 */
void ca_buf_reset(ca_buf *b);

/* 追加 n 字节。
 * 返回 0 成功；返回 -1 表示**会超出容量**，此时缓冲**完全未改动**。
 * 不做部分写入——部分写入会让调用方误以为只差一点点。
 */
int ca_buf_append(ca_buf *b, const void *src, size_t n);

/* 剩余可写字节 */
size_t ca_buf_space(const ca_buf *b);

#ifdef __cplusplus
}
#endif

#endif /* CAGENT_BUF_H */