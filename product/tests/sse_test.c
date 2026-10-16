/* sse_test.c —— buf + sse 层单元测试
 * 契约：docs/design/contract.md §4.2 / §10 T1 / T2
 *
 * 本文件最重要的一条是 test_huge_stream_keeps_memory_bounded：
 * 它把契约 C1「绝不整块缓冲」从一句设计口号变成可测的数字。
 */

#include "ca_test.h"

#include "cagent/buf.h"
#include "cagent/mem.h"
#include "cagent/sse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 安全的定长拷贝：strncpy 在 MSVC 上是弃用 API，且不保证 NUL 终止。
 * 两者都容易在测试里埋下"读越界"或"读未初始化"的问题。 */
static void copy_str(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ------------------------------------------------------------------- buf */

CA_TEST(buf_append_and_reset) {
    ca_buf b;
    CA_ASSERT(ca_buf_init(&b, 16) == 0);
    CA_ASSERT_EQ_INT((long long)ca_buf_space(&b), 16);
    CA_ASSERT(ca_buf_append(&b, "abc", 3) == 0);
    CA_ASSERT_EQ_INT((long long)b.len, 3);
    CA_ASSERT(memcmp(b.data, "abc", 3) == 0);
    ca_buf_reset(&b);
    CA_ASSERT_EQ_INT((long long)b.len, 0);
    CA_ASSERT_EQ_INT((long long)ca_buf_space(&b), 16);
    ca_buf_free(&b);
}

CA_TEST(buf_rejects_overflow_without_partial_write) {
    ca_buf b;
    CA_ASSERT(ca_buf_init(&b, 8) == 0);
    CA_ASSERT(ca_buf_append(&b, "12345678", 8) == 0);
    /* 满后再写必须失败，且**缓冲内容完全不变**——
     * 部分写入会让调用方误以为只差一点点，是最难查的一类 bug */
    CA_ASSERT(ca_buf_append(&b, "9", 1) == -1);
    CA_ASSERT_EQ_INT((long long)b.len, 8);
    CA_ASSERT(memcmp(b.data, "12345678", 8) == 0);
    ca_buf_free(&b);
}

CA_TEST(buf_zero_capacity_rejected) {
    /* 容量为 0 一定是调用方算错了，让它当场失败而不是留到写入时 */
    ca_buf b;
    CA_ASSERT(ca_buf_init(&b, 0) == -1);
    CA_ASSERT(b.data == NULL);
}

CA_TEST(buf_null_is_safe) {
    ca_buf_free(NULL);
    ca_buf_reset(NULL);
    CA_ASSERT_EQ_INT((long long)ca_buf_space(NULL), 0);
}

/* ------------------------------------------------------------------- sse */

typedef struct {
    int frames;
    char last_data[512];
    char last_event[64];
} sse_collector;

static void collect(const ca_sse_frame *f, void *user) {
    sse_collector *c = (sse_collector *)user;
    c->frames++;
    copy_str(c->last_data, sizeof(c->last_data), f->data);
    copy_str(c->last_event, sizeof(c->last_event), f->event);
}

CA_TEST(sse_parses_simple_frame) {
    ca_sse s;
    sse_collector c;
    memset(&c, 0, sizeof(c));
    CA_ASSERT(ca_sse_init(&s, 0) == 0);
    const char *raw = "event: delta\ndata: hello\n\n";
    CA_ASSERT(ca_sse_feed(&s, (const unsigned char *)raw, strlen(raw), collect, &c) == 0);
    CA_ASSERT_EQ_INT(c.frames, 1);
    CA_ASSERT(strcmp(c.last_data, "hello") == 0);
    CA_ASSERT(strcmp(c.last_event, "delta") == 0);
    ca_sse_free(&s);
}

CA_TEST(sse_ignores_comment_heartbeats) {
    /* 注释行（以 : 开头）是心跳，必须忽略且不产生帧 */
    ca_sse s;
    sse_collector c;
    memset(&c, 0, sizeof(c));
    CA_ASSERT(ca_sse_init(&s, 0) == 0);
    const char *raw = ": ping\n\n: ping\n\n";
    CA_ASSERT(ca_sse_feed(&s, (const unsigned char *)raw, strlen(raw), collect, &c) == 0);
    CA_ASSERT_EQ_INT(c.frames, 0);
    ca_sse_free(&s);
}

CA_TEST(sse_joins_multiple_data_lines_with_newline) {
    ca_sse s;
    sse_collector c;
    memset(&c, 0, sizeof(c));
    CA_ASSERT(ca_sse_init(&s, 0) == 0);
    const char *raw = "data: line1\ndata: line2\ndata: line3\n\n";
    CA_ASSERT(ca_sse_feed(&s, (const unsigned char *)raw, strlen(raw), collect, &c) == 0);
    CA_ASSERT_EQ_INT(c.frames, 1);
    CA_ASSERT(strcmp(c.last_data, "line1\nline2\nline3") == 0);
    ca_sse_free(&s);
}

CA_TEST(sse_tolerates_crlf) {
    ca_sse s;
    sse_collector c;
    memset(&c, 0, sizeof(c));
    CA_ASSERT(ca_sse_init(&s, 0) == 0);
    const char *raw = "data: win\r\n\r\n";
    CA_ASSERT(ca_sse_feed(&s, (const unsigned char *)raw, strlen(raw), collect, &c) == 0);
    CA_ASSERT_EQ_INT(c.frames, 1);
    CA_ASSERT(strcmp(c.last_data, "win") == 0);
    ca_sse_free(&s);
}

CA_TEST(sse_handles_frames_split_across_feeds) {
    /* 真实网络会把一帧切成任意位置。边界落在帧中间是最容易出错的情况。 */
    ca_sse s;
    sse_collector c;
    memset(&c, 0, sizeof(c));
    CA_ASSERT(ca_sse_init(&s, 0) == 0);
    const char *raw = "event: delta\ndata: split-me\n\n";
    for (size_t i = 0; i < strlen(raw); i++) {
        CA_ASSERT(ca_sse_feed(&s, (const unsigned char *)raw + i, 1, collect, &c) == 0);
    }
    CA_ASSERT_EQ_INT(c.frames, 1);
    CA_ASSERT(strcmp(c.last_data, "split-me") == 0);
    CA_ASSERT(strcmp(c.last_event, "delta") == 0);
    ca_sse_free(&s);
}

CA_TEST(sse_parses_back_to_back_frames_in_one_feed) {
    ca_sse s;
    sse_collector c;
    memset(&c, 0, sizeof(c));
    CA_ASSERT(ca_sse_init(&s, 0) == 0);
    const char *raw = "data: a\n\ndata: b\n\ndata: c\n\n";
    CA_ASSERT(ca_sse_feed(&s, (const unsigned char *)raw, strlen(raw), collect, &c) == 0);
    CA_ASSERT_EQ_INT(c.frames, 3);
    CA_ASSERT(strcmp(c.last_data, "c") == 0);
    ca_sse_free(&s);
}

CA_TEST(sse_rejects_oversized_frame) {
    /* 契约 §4.2：单帧超限即报错断开，不截断。
     * 截断出的半个帧会被当成完整帧解析——那比报错危险得多。 */
    ca_sse s;
    sse_collector c;
    memset(&c, 0, sizeof(c));
    CA_ASSERT(ca_sse_init(&s, 0) == 0); /* 默认 64 KB */
    size_t big = CA_SSE_MAX_FRAME_BYTES + 1024;
    unsigned char *blob = (unsigned char *)malloc(big);
    CA_ASSERT(blob != NULL);
    memset(blob, 'x', big);
    memcpy(blob, "data: ", 6);
    blob[big - 1] = '\n';
    blob[big - 2] = '\n'; /* 试图结束帧，但整帧已超限 */
    int rc = ca_sse_feed(&s, blob, big, collect, &c);
    CA_ASSERT_EQ_INT(rc, -1);
    CA_ASSERT_EQ_INT(c.frames, 0);
    /* 失败后必须持续拒绝，而不是恢复后继续解析 */
    CA_ASSERT(ca_sse_feed(&s, (const unsigned char *)"data: x\n\n", 10, collect, &c) == -1);
    CA_ASSERT_EQ_INT(c.frames, 0);
    free(blob);
    ca_sse_free(&s);
}

CA_TEST(sse_rejects_limit_above_cap) {
    /* 上限本身也封顶，否则"传入一个很大的上限"就成了绕过封顶的后门 */
    ca_sse s;
    CA_ASSERT(ca_sse_init(&s, (size_t)CA_SSE_MAX_FRAME_BYTES + 1) == -1);
    CA_ASSERT(ca_sse_init(&s, 1024) == 0);
    ca_sse_free(&s);
}

/* ---- 契约 T2：绝不整块缓冲 ---- */

CA_TEST(huge_stream_keeps_memory_bounded) {
    /* 灌入 100 MB 的 SSE 流，断言：
     *   1. 分帧器内部残片始终远小于帧上限（内存与响应大小无关）
     *   2. 进程 RSS 增量受控
     *   3. 帧数正确
     *
     * 这一条是契约 C1 的可测形式。若将来有人在解析里偷偷整块缓冲，
     * 本用例会先红。测试自身也**不**预先分配 100 MB ——
     * 否则测试的内存就会掩盖被测代码的内存。
     */
    ca_sse s;
    sse_collector c;
    memset(&c, 0, sizeof(c));
    CA_ASSERT(ca_sse_init(&s, 0) == 0);

    const size_t total_target = 100u * 1024u * 1024u;
    const size_t chunk = 64u * 1024u;
    unsigned char *payload = (unsigned char *)malloc(chunk);
    CA_ASSERT(payload != NULL);

    /* 一个 chunk 里放**多个**小帧，而不是一个巨型帧。
     * 早先的写法是「一整块 x + \n\n」构成单个 64KB 以上的帧——
     * 那会被 64 KB 帧上限正确拒绝（这是上限在起作用），
     * 于是测出来 0 帧。帧上限约束的是单帧，不是单次 feed。
     */
    const char unit[] = "data: x\n\n"; /* 9 字节一帧 */
    const size_t unit_len = sizeof(unit) - 1;
    size_t fill = 0;
    size_t units = 0;
    while (fill + unit_len <= chunk) {
        memcpy(payload + fill, unit, unit_len);
        fill += unit_len;
        units++;
    }
    size_t usable = fill;
    CA_ASSERT(usable == units * unit_len);
    CA_ASSERT(units > 1000);

    int64_t rss_before = ca_rss_bytes();
    size_t fed = 0;
    size_t max_pending = 0;
    while (fed < total_target) {
        size_t n = usable;
        if (n > total_target - fed) n = total_target - fed;
        if (ca_sse_feed(&s, payload, n, collect, &c) != 0) {
            CA_ASSERT(0);
            break;
        }
        size_t p = ca_sse_pending(&s);
        if (p > max_pending) max_pending = p;
        fed += n;
    }
    int64_t rss_after = ca_rss_bytes();

    /* 内部残片不得超过帧上限 */
    CA_ASSERT(max_pending <= CA_SSE_MAX_FRAME_BYTES);
    /* 帧数 = 总喂入字节 / 单帧字节数（向下取整，末尾残留是未成帧的残片）。
     * 早先的公式假定末次 feed 也是整块，实际它会被 total_target 截断，
     * 于是期望值偏小。 */
    CA_ASSERT_EQ_INT((long long)c.frames, (long long)(fed / unit_len));
    /* 内存与响应大小无关：残片上限恒为 64 KB 而已 */
    CA_ASSERT(max_pending <= 64u * 1024u);

    if (rss_before > 0 && rss_after > 0) {
        long long delta = (long long)(rss_after - rss_before);
        printf("    灌入 %u MB，帧数 %d，最大残片 %u 字节，RSS 增量 %.2f MB\n",
               (unsigned)(total_target / 1024u / 1024u), c.frames,
               (unsigned)max_pending, (double)delta / 1024.0 / 1024.0);
        /* 100 MB 响应，RSS 增量不应超过 8 MB（远低于 100 MB 即达标） */
        CA_ASSERT(delta <= 8LL * 1024 * 1024);
    }

    free(payload);
    ca_sse_free(&s);
}

int main(void) {
    struct { const char *name; void (*fn)(void); } cases[] = {
        {"buf_append_and_reset", ca_test_run_buf_append_and_reset},
        {"buf_rejects_overflow_without_partial_write", ca_test_run_buf_rejects_overflow_without_partial_write},
        {"buf_zero_capacity_rejected", ca_test_run_buf_zero_capacity_rejected},
        {"buf_null_is_safe", ca_test_run_buf_null_is_safe},
        {"sse_parses_simple_frame", ca_test_run_sse_parses_simple_frame},
        {"sse_ignores_comment_heartbeats", ca_test_run_sse_ignores_comment_heartbeats},
        {"sse_joins_multiple_data_lines_with_newline", ca_test_run_sse_joins_multiple_data_lines_with_newline},
        {"sse_tolerates_crlf", ca_test_run_sse_tolerates_crlf},
        {"sse_handles_frames_split_across_feeds", ca_test_run_sse_handles_frames_split_across_feeds},
        {"sse_parses_back_to_back_frames_in_one_feed", ca_test_run_sse_parses_back_to_back_frames_in_one_feed},
        {"sse_rejects_oversized_frame", ca_test_run_sse_rejects_oversized_frame},
        {"sse_rejects_limit_above_cap", ca_test_run_sse_rejects_limit_above_cap},
        {"huge_stream_keeps_memory_bounded", ca_test_run_huge_stream_keeps_memory_bounded},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ca_test_begin(cases[i].name);
        cases[i].fn();
        ca_test_end();
    }
    return ca_test_summary();
}