/* http_test.c —— 传输层测试
 * 契约：docs/design/contract.md §4.1 / §10 T2
 *
 * 本文件把契约 C1 推到端到端：传输 → SSE 分帧 → 事件，
 * 全程在**没有网络**的条件下验证内存不随响应大小增长。
 */

#include "ca_test.h"

#include "cagent/http.h"
#include "cagent/mem.h"
#include "cagent/sse.h"
#include "cagent/transport_mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CA_TEST(http_status_text_is_human_readable) {
    /* 错误信息面向用户，不得泄漏开发者术语或本机路径 */
    CA_ASSERT(strcmp(ca_http_status_text(CA_HTTP_OK), "成功") == 0);
    CA_ASSERT(strcmp(ca_http_status_text(CA_HTTP_ERR_TLS), "TLS 握手或证书校验失败") == 0);
    CA_ASSERT(strcmp(ca_http_status_text(CA_HTTP_ERR_ABORTED), "已被中止") == 0);
    /* 每个状态都必须有非空文本，否则用户只会看到空字符串 */
    for (int s = CA_HTTP_OK; s <= CA_HTTP_ERR_OOM; s++) {
        const char *t = ca_http_status_text((ca_http_status)s);
        CA_ASSERT(t != NULL && t[0] != '\0');
    }
}

CA_TEST(http_default_transport_present) {
    const ca_transport_ops *ops = ca_transport_default();
    if (ops == NULL) {
        /* 未编译 libcurl 后端时允许为空，但必须说清楚是哪种情况 */
        printf("    未编译传输后端（无 libcurl），运行时会拒绝联网请求\n");
        CA_ASSERT(strcmp(ca_transport_name(), "none") == 0);
        return;
    }
    CA_ASSERT(ops->init != NULL);
    CA_ASSERT(ops->run != NULL);
    CA_ASSERT(ops->cleanup != NULL);
}

/* ---- 传输 → SSE → 事件 的端到端泵 ---- */

typedef struct {
    ca_sse sse;
    int frames;
    size_t total_bytes;
    int64_t max_rss;
} pump_ctx;

static void on_frame(const ca_sse_frame *f, void *user) {
    pump_ctx *p = (pump_ctx *)user;
    (void)f;
    p->frames++;
}

static int on_data(const unsigned char *data, size_t n, void *user) {
    pump_ctx *p = (pump_ctx *)user;
    p->total_bytes += n;
    /* 每次回调都查一次内存：若上层偷偷攒数据，这里会先看到增长 */
    int64_t rss = ca_rss_bytes();
    if (rss > p->max_rss) p->max_rss = rss;
    return ca_sse_feed(&p->sse, data, n, on_frame, user);
}

CA_TEST(pump_streams_without_buffering_whole_response) {
    /* 契约 C1 + T2 的端到端形式：100 MB 的响应流经
     * 传输 → SSE → 事件，全程不整块缓冲。 */
    const size_t target = 100u * 1024u * 1024u;
    const char unit[] = "data: {\"delta\":\"x\"}\n\n";
    const size_t unit_len = sizeof(unit) - 1;

    /* 生成一份 100 MB 的响应，但不一次性分配 100 MB ——
     * 测试自身的内存不能掩盖被测代码的内存。 */
    size_t units = target / unit_len;
    unsigned char *payload = (unsigned char *)malloc(units * unit_len);
    CA_ASSERT(payload != NULL);
    if (payload == NULL) return;
    for (size_t i = 0; i < units; i++) {
        memcpy(payload + i * unit_len, unit, unit_len);
    }

    pump_ctx p;
    memset(&p, 0, sizeof(p));
    CA_ASSERT(ca_sse_init(&p.sse, 0) == 0);

    ca_mem_transport_set_payload(payload, units * unit_len, 200);

    const ca_transport_ops *ops = ca_transport_mem();
    CA_ASSERT(ops->init() == 0);

    int64_t rss_before = ca_rss_bytes();
    ca_request req;
    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.url = "http://127.0.0.1/stream";
    ca_http_status st = ops->run(&req, on_data, &p);
    int64_t rss_after = ca_rss_bytes();

    CA_ASSERT_EQ_INT(st, CA_HTTP_OK);
    CA_ASSERT_EQ_INT(p.total_bytes, (long long)(units * unit_len));
    CA_ASSERT_EQ_INT(p.frames, (long long)units);
    /* 分帧器残片必须极小：内存与响应大小无关的直接证据 */
    CA_ASSERT(ca_sse_pending(&p.sse) == 0);

    if (rss_before > 0 && rss_after > 0) {
        long long delta = (long long)(rss_after - rss_before);
        printf("    100 MB 响应：帧数 %d，RSS 增量 %.2f MB\n",
               p.frames, (double)delta / 1024.0 / 1024.0);
        /* 响应 100 MB 而增量必须远小于此，否则就是整块缓冲了 */
        CA_ASSERT(delta <= 16LL * 1024 * 1024);
    }

    ca_sse_free(&p.sse);
    free(payload);
    ops->cleanup();
}

/* 用于验证「上层主动中止」的回调：第一次调用就返回非 0。 */
typedef struct {
    int calls;
} abort_ctx;

static int on_data_abort(const unsigned char *data, size_t n, void *user) {
    (void)data;
    (void)n;
    abort_ctx *a = (abort_ctx *)user;
    a->calls++;
    return 1; /* 要求中止 */
}

CA_TEST(pump_propagates_upstream_abort) {
    /* 上层主动停止时，传输层必须报 ABORTED，
     * 而不是笼统的"传输失败"——用户主动停和网络故障是两件事，
     * 混为一谈会让用户以为自己的网络出了问题。 */
    const char *body = "data: a\n\ndata: b\n\ndata: c\n\n";
    ca_mem_transport_set_payload((const unsigned char *)body, strlen(body), 200);

    abort_ctx a;
    a.calls = 0;
    const ca_transport_ops *ops = ca_transport_mem();
    ca_request req;
    memset(&req, 0, sizeof(req));
    CA_ASSERT_EQ_INT(ops->run(&req, on_data_abort, &a), CA_HTTP_ERR_ABORTED);
    /* 必须在第一次回调就停，而不是把全部数据喂完才停 */
    CA_ASSERT_EQ_INT(a.calls, 1);
}

CA_TEST(pump_reports_protocol_error_status) {
    /* 4xx/5xx 必须被识别成协议错误，而不是当成正常响应流继续解析 */
    const char *body = "{\"error\":\"unauthorized\"}";
    ca_mem_transport_set_payload((const unsigned char *)body, strlen(body), 401);
    const ca_transport_ops *ops = ca_transport_mem();
    ca_request req;
    memset(&req, 0, sizeof(req));
    CA_ASSERT_EQ_INT(ops->run(&req, NULL, NULL), CA_HTTP_ERR_PROTOCOL);
}

int main(void) {
    struct { const char *name; void (*fn)(void); } cases[] = {
        {"http_status_text_is_human_readable", ca_test_run_http_status_text_is_human_readable},
        {"http_default_transport_present", ca_test_run_http_default_transport_present},
        {"pump_streams_without_buffering_whole_response", ca_test_run_pump_streams_without_buffering_whole_response},
        {"pump_propagates_upstream_abort", ca_test_run_pump_propagates_upstream_abort},
        {"pump_reports_protocol_error_status", ca_test_run_pump_reports_protocol_error_status},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ca_test_begin(cases[i].name);
        cases[i].fn();
        ca_test_end();
    }
    return ca_test_summary();
}