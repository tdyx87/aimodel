/* transport_mem.c —— 内存传输后端
 * 契约：docs/design/contract.md §4.1
 *
 * 存在的理由不是「没网时的降级」，而是让**「绝不整块缓冲」这条约束
 * 能在没有网络的情况下被测试**。把它绑死在真实网络上，等于让它
 * 只能靠「线上恰好没出问题」来间接保证——那不是保证。
 *
 * 它按固定小块把预设数据喂给上层，与 libcurl 后端的分块行为一致。
 */

#include "cagent/http.h"

#include <string.h>

/* 每次喂给上层的数据块大小。刻意做得比真实网络分块小得多，
 * 因为分块越小越容易暴露「上层偷偷攒数据」的倾向。 */
#define MEM_CHUNK 997u /* 质数，避免与缓冲大小形成整除关系 */

static int mem_init(void) { return 0; }
static void mem_cleanup(void) { (void)0; }

/* 测试通过这两个函数预设要返回的数据。
 * 上层 API 里**没有**「设置响应体」这类东西——数据只经 run 的回调流出，
 * 与真实后端的形状一致。
 */
static const unsigned char *g_payload = NULL;
static size_t g_payload_len = 0;
static long g_status = 0;

void ca_mem_transport_set_payload(const unsigned char *data, size_t len, long status) {
    g_payload = data;
    g_payload_len = len;
    g_status = status;
}

static ca_http_status mem_run(const ca_request *req, ca_on_data on_data, void *user) {
    (void)req;
    if (g_payload == NULL || g_payload_len == 0) {
        if (on_data != NULL) {
            static const unsigned char empty[] = "";
            if (on_data(empty, 0, user) != 0) return CA_HTTP_ERR_ABORTED;
        }
    }
    size_t off = 0;
    while (off < g_payload_len) {
        size_t n = g_payload_len - off;
        if (n > MEM_CHUNK) n = MEM_CHUNK;
        if (on_data != NULL && on_data(g_payload + off, n, user) != 0) {
            return CA_HTTP_ERR_ABORTED;
        }
        off += n;
    }
    return (g_status >= 400) ? CA_HTTP_ERR_PROTOCOL : CA_HTTP_OK;
}

static long mem_last_status(void) { return g_status; }

const ca_transport_ops *ca_transport_mem(void) {
    static const ca_transport_ops ops = {mem_init, mem_cleanup, mem_run, mem_last_status};
    return &ops;
}

const char *ca_transport_mem_name(void) { return "memory"; }