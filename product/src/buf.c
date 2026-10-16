/* buf.c —— 定长字节缓冲
 * 契约：docs/design/contract.md §2.3
 */

#include "cagent/buf.h"

#include <stdlib.h>
#include <string.h>

int ca_buf_init(ca_buf *b, size_t cap) {
    if (b == NULL) return -1;
    b->data = NULL;
    b->cap = 0;
    b->len = 0;
    /* 容量为 0 的缓冲一定是调用方算错了，让它当场失败 */
    if (cap == 0) return -1;
    b->data = (unsigned char *)malloc(cap);
    if (b->data == NULL) {
        b->cap = 0;
        return -1;
    }
    b->cap = cap;
    return 0;
}

void ca_buf_free(ca_buf *b) {
    if (b == NULL) return;
    free(b->data);
    b->data = NULL;
    b->cap = 0;
    b->len = 0;
}

void ca_buf_reset(ca_buf *b) {
    if (b == NULL) return;
    b->len = 0;
}

int ca_buf_append(ca_buf *b, const void *src, size_t n) {
    if (b == NULL || b->data == NULL) return -1;
    if (n == 0) return 0;
    if (src == NULL) return -1;
    /* 一次判尽，且不做部分写入：部分写入会让调用方误判进度 */
    if (n > b->cap - b->len) return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

size_t ca_buf_space(const ca_buf *b) {
    if (b == NULL) return 0;
    return b->cap - b->len;
}