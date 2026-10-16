/* sse.c —— SSE 分帧器
 * 契约：docs/design/contract.md §4.2
 *
 * 规则逐条对应契约 §4.2：
 *   - 仅 \n 分隔，容忍 \r\n
 *   - 空行（\n\n）分隔帧
 *   - 以 : 开头的注释行忽略（心跳）
 *   - 字段 event: / data: / id:
 *   - 单帧上限 64 KB，超限报错断开
 *
 * 关键设计：帧内容**就地**在缓冲里解析，用 '\0' 替换行尾、
 * 把多个 data: 行的值紧凑排列在最前面。因此整个过程**零额外分配**，
 * 内存占用恒等于一块帧缓冲，与响应总大小无关。
 */

#include "cagent/sse.h"

#include <stdlib.h>
#include <string.h>

/* 字段值去掉冒号后的前导空格（规范要求恰好去掉一个） */
static const char *value_of(const char *colon) {
    const char *v = colon + 1;
    if (*v == ' ') v++;
    return v;
}

static int name_is(const char *line, size_t len, const char *want) {
    return len == strlen(want) && memcmp(line, want, len) == 0;
}

/* 把一帧（acc.data[0..len)）解析成字段并回调。
 * 会破坏 acc 的内容——调用方随后会立即回收这一帧。
 *
 * 关键：data 的多行合并写到 s->out 这块**独立**的定长缓冲，而不是就地压缩。
 * 就地压缩会覆盖掉 event/id 的值——它们的指针指向同一块内存里的别处，
 * 而压缩区恰好会移动到那里（表现为 data 正确、event 变成空串）。
 * 两块缓冲各自定长，总量恒定，与响应大小无关，仍满足契约 C1。
 */
static void emit_frame(ca_sse *s, unsigned char *buf, size_t len,
                       void (*on_frame)(const ca_sse_frame *, void *), void *user) {
    /* 第一遍：把行尾改成 '\0'，并把每个 data: 值的起点与长度记下 */
    size_t pos = 0;
    char *data_start[64];
    size_t data_len[64];
    size_t data_count = 0;
    size_t total_data = 0;
    const char *event = NULL;
    const char *id = NULL;

    while (pos < len) {
        /* 去掉 \r */
        size_t end = pos;
        while (end < len && buf[end] != '\n') end++;
        if (end > pos && buf[end - 1] == '\r') end--;
        buf[end] = '\0';

        const char *line = (const char *)(buf + pos);
        size_t line_len = end - pos;

        if (line_len > 0 && line[0] != ':') {
            const char *colon = (const char *)memchr(line, ':', line_len);
            size_t nlen = colon ? (size_t)(colon - line) : line_len;
            const char *val = colon ? value_of(colon) : "";

            if (name_is(line, nlen, "data")) {
                if (data_count < 64) {
                    data_start[data_count] = (char *)val;
                    data_len[data_count] = strlen(val);
                    total_data += data_len[data_count];
                    data_count++;
                }
            } else if (name_is(line, nlen, "event")) {
                event = val;
            } else if (name_is(line, nlen, "id")) {
                id = val;
            }
            /* retry: 识别但不消费（契约 §4.2 只要求识别） */
        }
        pos = end + 1;
    }

    /* 只有心跳的帧不产出 */
    if (data_count == 0 && event == NULL && id == NULL) return;

    /* 第二遍：多个 data: 值用 '\n' 连接，写进 out。
     * 单个 data 行是绝大多数情况，此时 out 就是一份拷贝——
     * 不做特判是因为特判会引入"两条路径行为不一致"的风险。 */
    s->out.len = 0;
    if (data_count > 0) {
        size_t need = total_data + (data_count - 1);
        if (need <= s->out.cap) {
            for (size_t i = 0; i < data_count; i++) {
                if (i > 0) s->out.data[s->out.len++] = '\n';
                if (data_len[i] > 0) {
                    memcpy(s->out.data + s->out.len, data_start[i], data_len[i]);
                    s->out.len += data_len[i];
                }
            }
            s->out.data[s->out.len] = '\0';
        } else {
            /* 理论上不可能：帧已受上限约束。保留为防御，
             * 但不能静默给出错误内容，故不产出 data。 */
            s->out.len = 0;
        }
    }

    ca_sse_frame frame;
    frame.event = event;
    frame.id = id;
    frame.data = data_count > 0 && s->out.len > 0 ? (const char *)s->out.data : NULL;
    if (on_frame != NULL) on_frame(&frame, user);
}

int ca_sse_init(ca_sse *s, size_t frame_limit) {
    if (s == NULL) return -1;
    s->in_frame = 0;
    s->failed = 0;
    if (frame_limit == 0) frame_limit = CA_SSE_MAX_FRAME_BYTES;
    /* 上限本身也封顶：否则"传入一个很大的上限"就成了绕过封顶的后门 */
    if (frame_limit > CA_SSE_MAX_FRAME_BYTES) return -1;
    if (ca_buf_init(&s->acc, frame_limit) != 0) return -1;
    /* out 比 acc 多留 1 字节用于 NUL 终止。总量仍是常数，与响应大小无关。 */
    if (ca_buf_init(&s->out, frame_limit + 1) != 0) {
        ca_buf_free(&s->acc);
        return -1;
    }
    return 0;
}

void ca_sse_free(ca_sse *s) {
    if (s == NULL) return;
    ca_buf_free(&s->acc);
    ca_buf_free(&s->out);
}

size_t ca_sse_pending(const ca_sse *s) {
    if (s == NULL) return 0;
    return s->acc.len;
}

/* 找帧边界：空行结束帧。必须容忍 CRLF —— 契约 §4.2 明确要求。
 * 边界的字节形态有两种：
 *     ...\n\n      （LF）
 *     ...\n\r\n    （CRLF）
 * 早先只匹配相邻的 \n\n，于是 CRLF 流一帧都解析不出来。
 *
 * frame_len  = 帧内容长度（不含分隔符）
 * *consumed  = 需要跳过的总字节数（含分隔符）
 * 未找到时返回 0。
 */
static size_t find_frame_end(const ca_buf *acc, size_t *consumed) {
    for (size_t i = 0; i + 1 < acc->len; i++) {
        if (acc->data[i] != '\n') continue;
        /* \n\n */
        if (acc->data[i + 1] == '\n') {
            *consumed = i + 2;
            return i;
        }
        /* \n\r\n */
        if (acc->data[i + 1] == '\r' && i + 2 < acc->len && acc->data[i + 2] == '\n') {
            *consumed = i + 3;
            return i;
        }
    }
    return 0;
}

static void consume_frame(ca_sse *s, size_t flen, size_t consumed,
                          void (*on_frame)(const ca_sse_frame *, void *), void *user) {
    emit_frame(s, s->acc.data, flen, on_frame, user);
    if (consumed >= s->acc.len) {
        s->acc.len = 0;
    } else {
        memmove(s->acc.data, s->acc.data + consumed, s->acc.len - consumed);
        s->acc.len -= consumed;
    }
}

int ca_sse_feed(ca_sse *s, const unsigned char *data, size_t n,
                void (*on_frame)(const ca_sse_frame *, void *), void *user) {
    if (s == NULL) return -1;
    if (s->failed) return -1;
    if (n == 0) return 0;
    if (data == NULL) return -1;

    /* 先消化缓冲里已有的完整帧，再追加。
     * 顺序反了会把"缓冲里有半帧 + 新数据"的边界埋掉。 */
    for (;;) {
        size_t consumed = 0;
        size_t flen = find_frame_end(&s->acc, &consumed);
        if (flen == 0) break;
        consume_frame(s, flen, consumed, on_frame, user);
    }

    if (ca_buf_append(&s->acc, data, n) != 0) {
        /* 写不进去 = 超过帧上限 = 协议异常，报错断开（契约 §4.2）。
         * 刻意不截断：截断出的半个帧会被当成完整帧解析。 */
        s->failed = 1;
        return -1;
    }

    for (;;) {
        size_t consumed = 0;
        size_t flen = find_frame_end(&s->acc, &consumed);
        if (flen == 0) break;
        consume_frame(s, flen, consumed, on_frame, user);
    }
    return 0;
}