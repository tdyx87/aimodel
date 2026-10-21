/* transport_curl.c —— libcurl 传输后端
 * 契约：docs/design/contract.md §4.1
 *
 * 整个后端的核心就是 `write_cb`：libcurl 每收到一块就调它，
 * 我们**立刻**把数据转交给上层，一块都不留。
 *
 * 刻意不用的东西（每一条都是内存失控的入口）：
 *   - CURLOPT_WRITEDATA 指向大缓冲（不缓冲）
 *   - 任何形如 "get whole body" 的辅助用法
 *   - CURLOPT_BUFFERSIZE 调大（默认足够，且我们不依赖它）
 *
 * 若未检测到 libcurl，本文件不参与编译，由 CMake 控制。
 */

#include "cagent/http.h"

#include <stdlib.h>
#include <string.h>

#if defined(CAGENT_HAVE_CURL)

#include <curl/curl.h>

/* 当前线程的请求上下文。libcurl 的回调没有 userdata 之外的通道，
 * 用线程局部变量传递；C99 的 _Thread_local 在 MSVC 2015+ 可用。 */
#if defined(_MSC_VER)
#  define CA_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
/* 本项目是 **C99**，而 `_Thread_local` 是 C11 才有的。直接用它，
 * GCC 会报 \"ISO C99 does not support '_Thread_local'\"。
 * `__thread` 是 GCC/Clang 的等价扩展，不触发该告警。 */
#  define CA_TLS __thread
#else
/* 不支持线程局部存储时退化为普通 static。
 * 本项目的使用场景是单线程的请求-回调配对，不会并发。 */
#  define CA_TLS
#endif

typedef struct {
    ca_on_data on_data;
    void *user;
    long status_code;
    int aborted;
    int transport_error;
} curl_ctx;

static CA_TLS curl_ctx g_ctx;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)userdata;
    size_t n = size * nmemb;
    if (n == 0) return 0;
    if (g_ctx.on_data == NULL) return n;
    if (g_ctx.on_data((const unsigned char *)ptr, n, g_ctx.user) != 0) {
        g_ctx.aborted = 1;
        /* 返回 0 表示"我不要更多了"，libcurl 会以 CURLE_WRITE_ERROR 结束。
         * 这是中止的标准做法，不用设超时也不必杀掉连接。 */
        return 0;
    }
    return n; /* 必须原样返回，否则 libcurl 视为错误 */
}

static size_t header_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)userdata;
    size_t n = size * nmemb;
    /* 状态行形如 "HTTP/1.1 429 Too Many Requests\r\n"。
     *
     * 早先这里写的是 `g_ctx.status_code = size * nmemb`——把「这一行的
     * 字节长度」当成了状态码。于是 `last_status_code()` 永远返回几十
     * 这样的小数，上层既判不出 4xx、也没法把 429 的额度/过载原因
     * 转告用户（真实故障现场：界面只说「请求失败」）。 */
    if (n > 9 && strncmp(ptr, "HTTP/", 5) == 0) {
        const char *sp = strchr(ptr + 5, ' ');
        if (sp != NULL) {
            long code = strtol(sp + 1, NULL, 10);
            if (code >= 100 && code < 600) g_ctx.status_code = code;
        }
    }
    return n;
}

static int curl_init(void) {
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? 0 : -1;
}

static void curl_cleanup(void) {
    curl_global_cleanup();
}

static ca_http_status curl_run(const ca_request *req, ca_on_data on_data, void *user) {
    if (req == NULL || req->url == NULL || req->method == NULL) return CA_HTTP_ERR_TRANSPORT;

    CURL *curl = curl_easy_init();
    if (curl == NULL) return CA_HTTP_ERR_OOM;

    g_ctx.on_data = on_data;
    g_ctx.user = user;
    g_ctx.status_code = 0;
    g_ctx.aborted = 0;
    g_ctx.transport_error = 0;

    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    /* 关键：不设 CURLOPT_WRITEDATA。数据只经 write_cb 流过，不落任何缓冲。 */

    if (req->headers != NULL) {
        struct curl_slist *hl = NULL;
        for (const char *const *h = req->headers; *h != NULL; h++) {
            hl = curl_slist_append(hl, *h);
        }
        if (hl != NULL) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hl);
        /* hl 的所有权已交给 curl，不要再 free */
    }

    if (strcmp(req->method, "POST") == 0 || strcmp(req->method, "PUT") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body ? req->body : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req->body_len);
    } else if (strcmp(req->method, "GET") == 0) {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    /* 三类超时分开设（契约 §4.1）：流式下设总超时没有意义——
     * 一个正常的长回答会被自己的总超时掐断。 */
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                     req->connect_timeout_ms > 0 ? req->connect_timeout_ms : 10000L);
    if (req->first_byte_timeout_ms > 0) {
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    }
    if (req->idle_timeout_ms > 0) {
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,
                         (long)(req->idle_timeout_ms / 1000) > 0
                             ? (long)(req->idle_timeout_ms / 1000) : 1L);
    }

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    /* 以 libcurl 的权威状态码为准，而不是信任 header 回调的解析：
     * 重定向链、HTTP/2 的 ":status" 都可能让逐行解析漏掉最后一次。 */
    if (status > 0) g_ctx.status_code = status;

    /* 上层主动中止优先于一切传输层错误——on_data 返回非 0 时
     * libcurl 会报 WRITE_ERROR，那是我们的选择，不是网络故障。 */
    if (g_ctx.aborted) return CA_HTTP_ERR_ABORTED;

    switch (rc) {
        case CURLE_OK:
            if (status >= 400) return CA_HTTP_ERR_PROTOCOL;
            return CA_HTTP_OK;
        case CURLE_COULDNT_CONNECT:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY:
            return CA_HTTP_ERR_CONNECT;
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_PEER_FAILED_VERIFICATION:
            return CA_HTTP_ERR_TLS;
        case CURLE_OPERATION_TIMEDOUT:
            return CA_HTTP_ERR_TIMEOUT;
        default:
            return CA_HTTP_ERR_TRANSPORT;
    }
}

static long curl_last_status(void) {
    return g_ctx.status_code;
}

const ca_transport_ops *ca_transport_default(void) {
    static const ca_transport_ops ops = {curl_init, curl_cleanup, curl_run, curl_last_status};
    return &ops;
}

const char *ca_transport_name(void) {
    return "libcurl";
}

#else /* 没有 libcurl 时的占位 */

const ca_transport_ops *ca_transport_default(void) { return NULL; }
const char *ca_transport_name(void) { return "none"; }

#endif /* CAGENT_HAVE_CURL */

const char *ca_http_status_text(ca_http_status s) {
    switch (s) {
        case CA_HTTP_OK: return "成功";
        case CA_HTTP_ERR_CONNECT: return "无法连接到服务端";
        case CA_HTTP_ERR_TLS: return "TLS 握手或证书校验失败";
        case CA_HTTP_ERR_TIMEOUT: return "超时";
        case CA_HTTP_ERR_PROTOCOL: return "服务端返回错误状态码";
        case CA_HTTP_ERR_TRANSPORT: return "传输失败";
        case CA_HTTP_ERR_ABORTED: return "已被中止";
        case CA_HTTP_ERR_OOM: return "内存不足";
        default: return "未知状态";
    }
}