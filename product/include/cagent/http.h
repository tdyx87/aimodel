/* http.h —— 传输层
 * 契约：docs/design/contract.md §4.1
 *
 * 本层只做一件事：把响应体**逐块**交给上层。
 *
 * 【最重要的一条】这里**不存在**「读回完整响应体」的函数。
 * 不是「不提供」而是「不存在」——只要有这样一个函数，实现就会走捷径，
 * 而一旦它被调用，内存立刻等于响应大小。让犯错在 API 层就不可能，
 * 比写在注释里提醒要可靠得多（契约 C1 / V1）。
 *
 * 因此接口是「泵」式的：调一次 run，上层在 on_data 回调里逐块拿到数据。
 * 上层无需也不能缓存整包。
 */

#ifndef CAGENT_HTTP_H
#define CAGENT_HTTP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 分块回调。返回非 0 表示上层要求中止（返回码会向上传播）。
 * data 在回调期间有效，**不得**保存指针——它指向传输层内部缓冲，
 * 下一次回调即被覆盖。 */
typedef int (*ca_on_data)(const unsigned char *data, size_t n, void *user);

/* 请求参数。只包含流式所必需的项。 */
typedef struct {
    const char *method;   /* 必需，例如 "POST" */
    const char *url;      /* 必需 */
    const char *body;     /* 请求体，可为 NULL */
    size_t body_len;      /* 请求体长度 */
    /* 请求头。name 与 value 均以 '\0' 结尾，以连续两个 '\0' 收尾。 */
    const char *const *headers; /* 可为 NULL */
    long connect_timeout_ms;   /* 连接超时；<=0 用默认值 */
    long first_byte_timeout_ms;/* 首字节超时；<=0 用默认值 */
    long idle_timeout_ms;      /* 两次数据之间的空闲超时；<=0 不限 */
} ca_request;

/* 结果状态。刻意区分「传输失败」与「上层中止」——
 * 二者对用户是不同的事：前者要去查网络，后者是用户自己停的。 */
typedef enum {
    CA_HTTP_OK = 0,
    CA_HTTP_ERR_CONNECT,
    CA_HTTP_ERR_TLS,
    CA_HTTP_ERR_TIMEOUT,
    CA_HTTP_ERR_PROTOCOL,   /* HTTP 层返回了非 2xx */
    CA_HTTP_ERR_TRANSPORT,  /* 传输层内部错误 */
    CA_HTTP_ERR_ABORTED,    /* on_data 返回非 0，上层主动中止 */
    CA_HTTP_ERR_OOM         /* 分配失败 */
} ca_http_status;

/* 传输实现。不同后端（libcurl / 内存）填这张表。
 *
 * 之所以做成表而不是直接调 libcurl：
 *   1. 契约 C1 要求「只有流式回调」，做成接口等于结构上强制；
 *   2. 传输逻辑可在没有网络的情况下被完整测试——
 *      「绝不整块缓冲」这条用内存后端就能验证，不必依赖网络抖动。
 */
typedef struct ca_transport ca_transport;
typedef struct {
    /* 全局初始化（curl_global_init 等）。返回 0 成功。 */
    int (*init)(void);
    void (*cleanup)(void);
    /* 执行一次请求。响应体逐块喂给 on_data。
     * 返回 CA_HTTP_OK 或具体错误码。 */
    ca_http_status (*run)(const ca_request *req, ca_on_data on_data, void *user);
    /* 最近一次 HTTP 状态码（用于区分协议错误）；无则 0。 */
    long (*last_status_code)(void);
} ca_transport_ops;

/* 取内置后端。
 * 后端由编译期决定：有 libcurl 时为 curl，否则为内存后端
 * （内存后端不能真联网，仅供测试与无网络环境降级）。
 */
const ca_transport_ops *ca_transport_default(void);
const char *ca_transport_name(void);

/* 状态码转可读文本（不含本机路径、不含凭据）。 */
const char *ca_http_status_text(ca_http_status s);

#ifdef __cplusplus
}
#endif

#endif /* CAGENT_HTTP_H */