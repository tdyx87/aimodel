/* web.c —— 本地 Web UI：HTTP 解析、SSE 推送、事件/消息通道
 *
 * 结构：
 *   一个监听线程（accept 循环）
 *     └─ 每个连接一个线程（连接数有上限）
 *   一个事件广播：SSE 客户端列表 + 互斥量
 *   两个通道：
 *     用户消息队列（浏览器 → 代理）
 *     批准决定（浏览器 → 代理，槽位式：同一时刻最多一个待批准）
 *
 * 刻意不做的事：
 *   - 不做 keep-alive 复用：每个请求一条连接，行为简单可预测；
 *     SSE 连接是例外（它必须一直挂着）。
 *   - 不做模板/静态文件：页面内嵌在二进制里（单文件交付，与项目风格一致）。
 *   - 不解析复杂 JSON：请求体只用 cagent_json 的零分配游标扫一遍。
 */

/* 禁用 MSVC 的「不安全函数」告警：这里的用法都是安全的
 * （定长缓冲 + 显式长度检查），而 C4996 会在 CI 上直接变红。 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "cagent/web.h"

#include "cagent/json.h"
#include "web_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CA_WEB_MAX_CLIENTS 32
/* 活跃连接上限（每连接一个线程）。超出的连接立刻收到 503 并被断开。 */
#define CA_WEB_MAX_CONNS 64
#define CA_WEB_MAX_BODY (64 * 1024)
#define CA_WEB_TOKEN_LEN 32
#define CA_WEB_QUEUE 32
#define CA_WEB_MSG_MAX 8192

/* 页面（内嵌，避免额外文件依赖）。
 * 以**片段数组**给出：单��字符串字面量超过 4095 字符会触发
 * -Woverlength-strings（ISO C99 只要求支持到 4095），CI 零告警门禁会红。 */
const char *const *ca_web_page_chunks(size_t *count);

struct ca_web {
    int listen_fd;
    int port;
    char token[CA_WEB_TOKEN_LEN + 1];
    char url[128];
    int stop;

    /* SSE 客户端。
     * refs：正在向该 fd 写的线程数（关闭前必须等它归零，
     *        否则 fd 被复用后可能把事件写到别的客户端那里）。
     * dead：该连接已断开，不再接收事件。 */
    struct {
        int fd;
        int refs;
        int dead;
    } clients[CA_WEB_MAX_CLIENTS];
    int client_count;
    /* 活跃连接数。**每个连接一个线程**，不限就是本机任何进程都能
     * 把本进程的钱（内存）花光——哪怕它没有令牌。 */
    int conn_count;

    /* 用户消息队列 */
    char queue[CA_WEB_QUEUE][CA_WEB_MSG_MAX];
    int queue_len;
    int queue_head;

    /* 批准槽位 */
    int approval_ready; /* 0=没有 1=有决定 */
    int approval_allow;

    /* 运行时重配（Web 配置面板用）：
     * 由外部（main.c）设置，调用 ca_core_reconfig 把 URL/模型/密钥
     * 写进 ca_core。为 NULL 时 /api/config 返回 501。 */
    void (*on_config)(const char *api_base, const char *model,
                      const char *api_key, void *user);
    void *config_user;

    ca_net_mutex lock;
    ca_net_cond queue_cond;
    ca_net_cond approval_cond;
};

/* ------------------------------------------------------------------ 工具 */

static void hexify(const unsigned char *raw, size_t n, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = digits[(raw[i] >> 4) & 0x0Fu];
        out[i * 2 + 1] = digits[raw[i] & 0x0Fu];
    }
    out[n * 2] = '\0';
}

static int make_token(char *out, size_t cap) {
    unsigned char raw[CA_WEB_TOKEN_LEN / 2];
    if (ca_net_random(raw, sizeof(raw)) != 0) return -1;
    char hex[CA_WEB_TOKEN_LEN + 1];
    hexify(raw, sizeof(raw), hex);
    if (strlen(hex) >= cap) return -1;
    strcpy(out, hex);
    return 0;
}

/* ASCII 大小写无关比较。
 *
 * 不用 strncasecmp：MSVC 没有它，POSIX 还得专门 include <strings.h>
 * （而本项目刻意关掉了编译器扩展，隐式声明会变成「返回 int 的指针」，
 * 在 64 位上被截断）。自己写只有十行，还没有 locale 依赖。 */
static int ca_ieq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        if (ca == '\0') return 1;
    }
    return 1;
}

/* JSON 字符串转义（用于 SSE 载荷）。输出到 out，返回长度。 */
static size_t json_escape(const char *s, char *out, size_t cap) {
    size_t n = 0;
    if (s == NULL) s = "";
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        char tmp[8];
        size_t add = 0;
        if (*p == '"' || *p == '\\') {
            tmp[0] = '\\';
            tmp[1] = (char)*p;
            add = 2;
        } else if (*p == '\n') {
            tmp[0] = '\\';
            tmp[1] = 'n';
            add = 2;
        } else if (*p == '\r') {
            tmp[0] = '\\';
            tmp[1] = 'r';
            add = 2;
        } else if (*p == '\t') {
            tmp[0] = '\\';
            tmp[1] = 't';
            add = 2;
        } else if (*p < 0x20) {
            /* 控制字符用 \u00XX */
            static const char d[] = "0123456789abcdef";
            tmp[0] = '\\';
            tmp[1] = 'u';
            tmp[2] = '0';
            tmp[3] = '0';
            tmp[4] = d[(*p >> 4) & 0x0F];
            tmp[5] = d[*p & 0x0F];
            add = 6;
        } else {
            tmp[0] = (char)*p;
            add = 1;
        }
        if (n + add + 1 > cap) break;
        memcpy(out + n, tmp, add);
        n += add;
    }
    out[n] = '\0';
    return n;
}

/* ------------------------------------------------------------------ 广播 */

/* 发一条 SSE 事件。
 *
 * 分两次发（事件名 / 数据），**不要**把它们拼进一个定长缓冲：
 * 早先用 128 字节的缓冲拼 "event: x\ndata: <json>\n\n"，
 * 而 json 常常有几百字节——snprintf 截断后返回「需要的长度」，
 * 代码把它当成失败直接 return，于是**所有稍长的消息都被静默丢弃**
 * （页面只看得见最初那条 info）。
 * 现在按段发，不存在这个问题。 */
static int sse_send(int fd, const char *event, const char *json_body) {
    char head[64];
    int n = snprintf(head, sizeof(head), "event: %s\n", event != NULL ? event : "message");
    if (n <= 0 || (size_t)n >= sizeof(head)) return -1;
    if (ca_net_send(fd, head, (size_t)n) <= 0) return -1;
    if (ca_net_send(fd, "data: ", 6) <= 0) return -1;
    if (json_body != NULL && ca_net_send(fd, json_body, strlen(json_body)) <= 0) {
        return -1;
    }
    if (ca_net_send(fd, "\n\n", 2) <= 0) return -1;
    return 0;
}

void ca_web_emit(ca_web *w, const ca_web_event *ev) {
    if (w == NULL || ev == NULL) return;
    char esc_text[8192];
    char esc_name[256];
    char esc_status[64];
    char body[9216];
    json_escape(ev->text, esc_text, sizeof(esc_text));
    json_escape(ev->name, esc_name, sizeof(esc_name));
    json_escape(ev->status, esc_status, sizeof(esc_status));

    const char *kind = "text";
    switch (ev->kind) {
        case CA_WEB_EV_TEXT: kind = "text"; break;
        case CA_WEB_EV_TOOL: kind = "tool"; break;
        case CA_WEB_EV_APPROVAL: kind = "approval"; break;
        case CA_WEB_EV_INFO: kind = "info"; break;
        case CA_WEB_EV_ERROR: kind = "error"; break;
        case CA_WEB_EV_DONE: kind = "done"; break;
        default: kind = "text"; break;
    }
    int bn = snprintf(body, sizeof(body),
                      "{\"kind\":\"%s\",\"text\":\"%s\",\"name\":\"%s\",\"status\":\"%s\"}",
                      kind, esc_text, esc_name, esc_status);
    if (bn <= 0 || (size_t)bn >= sizeof(body)) {
        /* 载荷装不下：发一条短的，别发半个 JSON——
         * 半个 JSON 在浏览器里是 JSON.parse 报错，页面直接卡住。 */
        snprintf(body, sizeof(body),
                 "{\"kind\":\"%s\",\"text\":\"\",\"name\":\"\",\"status\":\"\"}",
                 kind);
    }

    /* 快照 + 引用计数，然后**放锁再写**：
     *   - 持锁写会拖住所有其他事件（甚至代理本身）；
     *   - 不计引用就 close(fd)，fd 被复用后事件会写去别的客户端。 */
    int fds[CA_WEB_MAX_CLIENTS];
    int nfd = 0;
    ca_net_mutex_lock(&w->lock);
    for (int i = 0; i < w->client_count; i++) {
        if (w->clients[i].dead) continue;
        w->clients[i].refs++;
        fds[nfd++] = w->clients[i].fd;
    }
    ca_net_mutex_unlock(&w->lock);

    for (int i = 0; i < nfd; i++) {
        if (sse_send(fds[i], kind, body) != 0) {
            /* 写失败（对端已断）：标记为 dead，让它被回收。 */
            ca_net_mutex_lock(&w->lock);
            for (int k = 0; k < w->client_count; k++) {
                if (w->clients[k].fd == fds[i] && !w->clients[k].dead) {
                    w->clients[k].dead = 1;
                    break;
                }
            }
            ca_net_mutex_unlock(&w->lock);
        }
    }

    ca_net_mutex_lock(&w->lock);
    for (int i = 0; i < nfd; i++) {
        for (int k = 0; k < w->client_count; k++) {
            if (w->clients[k].fd == fds[i] && w->clients[k].refs > 0) {
                w->clients[k].refs--;
                break;
            }
        }
    }
    ca_net_cond_broadcast(&w->approval_cond);
    ca_net_mutex_unlock(&w->lock);
}

/* ------------------------------------------------------------------ 通道 */

int ca_web_push_user(ca_web *w, const char *text) {
    if (w == NULL || text == NULL) return -1;
    size_t n = strlen(text);
    if (n >= CA_WEB_MSG_MAX) n = CA_WEB_MSG_MAX - 1;

    ca_net_mutex_lock(&w->lock);
    int ok = 0;
    if (w->queue_len < CA_WEB_QUEUE) {
        int slot = (w->queue_head + w->queue_len) % CA_WEB_QUEUE;
        memcpy(w->queue[slot], text, n);
        w->queue[slot][n] = '\0';
        w->queue_len++;
        ok = 1;
    }
    ca_net_cond_signal(&w->queue_cond);
    ca_net_mutex_unlock(&w->lock);
    return ok ? 0 : -1;
}

int ca_web_pop_user(ca_web *w, char *buf, size_t cap, int timeout_ms) {
    if (w == NULL || buf == NULL) return -1;
    ca_net_mutex_lock(&w->lock);
    while (w->queue_len == 0 && !w->stop) {
        if (ca_net_cond_wait(&w->queue_cond, &w->lock, timeout_ms) == 1) {
            ca_net_mutex_unlock(&w->lock);
            return 0; /* 超时 */
        }
    }
    if (w->queue_len == 0) {
        ca_net_mutex_unlock(&w->lock);
        return -1; /* 已停止 */
    }
    int slot = w->queue_head;
    size_t n = strlen(w->queue[slot]);
    if (n >= cap) n = cap - 1;
    memcpy(buf, w->queue[slot], n);
    buf[n] = '\0';
    w->queue_head = (w->queue_head + 1) % CA_WEB_QUEUE;
    w->queue_len--;
    ca_net_mutex_unlock(&w->lock);
    return 1;
}

void ca_web_set_approval(ca_web *w, int allow) {
    if (w == NULL) return;
    ca_net_mutex_lock(&w->lock);
    w->approval_ready = 1;
    w->approval_allow = allow ? 1 : 0;
    ca_net_cond_broadcast(&w->approval_cond);
    ca_net_mutex_unlock(&w->lock);
}

int ca_web_wait_approval(ca_web *w, int timeout_ms) {
    if (w == NULL) return 0;
    ca_net_mutex_lock(&w->lock);
    while (!w->approval_ready && !w->stop) {
        if (ca_net_cond_wait(&w->approval_cond, &w->lock, timeout_ms) == 1) {
            ca_net_mutex_unlock(&w->lock);
            return -1; /* 超时：按拒绝处理 */
        }
    }
    int allow = w->approval_ready ? w->approval_allow : 0;
    w->approval_ready = 0;
    ca_net_mutex_unlock(&w->lock);
    return allow;
}

/* ------------------------------------------------------------------ HTTP */

typedef struct {
    char method[8];
    char path[256];
    char query[256];
    char token[CA_WEB_TOKEN_LEN + 1];
    char host[128];
    int has_token_header;
} web_request;

static int header_value(const char *headers, const char *name, char *out,
                         size_t cap) {
    size_t nlen = strlen(name);
    const char *p = headers;
    while (p != NULL && *p != '\0') {
        const char *eol = strstr(p, "\r\n");
        size_t line_len = (eol != NULL) ? (size_t)(eol - p) : strlen(p);
        if (line_len > nlen && ca_ieq(p, name, nlen) && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (v < p + line_len && (*v == ' ' || *v == '\t')) v++;
            size_t vlen = (size_t)(p + line_len - v);
            if (vlen >= cap) vlen = cap - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return 0;
        }
        p = (eol != NULL) ? eol + 2 : NULL;
    }
    return -1;
}

/* 从请求行里取出路径与查询串。 */
static void parse_target(const char *target, char *path, size_t pcap,
                         char *query, size_t qcap) {
    const char *q = strchr(target, '?');
    if (q != NULL) {
        size_t n = (size_t)(q - target);
        if (n >= pcap) n = pcap - 1;
        memcpy(path, target, n);
        path[n] = '\0';
        size_t qn = strlen(q + 1);
        if (qn >= qcap) qn = qcap - 1;
        memcpy(query, q + 1, qn);
        query[qn] = '\0';
    } else {
        size_t n = strlen(target);
        if (n >= pcap) n = pcap - 1;
        memcpy(path, target, n);
        path[n] = '\0';
        if (qcap > 0) query[0] = '\0';
    }
}

static int token_ok(const ca_web *w, const web_request *req) {
    if (req->has_token_header) {
        return strcmp(req->token, w->token) == 0;
    }
    /* 也接受 URL 上的 ?t=（页面首次加载用得上） */
    if (req->query[0] != '\0' && strncmp(req->query, "t=", 2) == 0) {
        return strcmp(req->query + 2, w->token) == 0;
    }
    return 0;
}

/* Host 头必须是本机回环 + 端口，挡住 DNS rebinding。 */
static int host_ok(const ca_web *w, const web_request *req) {
    if (req->host[0] == '\0') return 0;
    char expect[64];
    snprintf(expect, sizeof(expect), "127.0.0.1:%d", w->port);
    if (strcmp(req->host, expect) == 0) return 1;
    snprintf(expect, sizeof(expect), "localhost:%d", w->port);
    if (strcmp(req->host, expect) == 0) return 1;
    return 0;
}

static void http_send(int fd, const char *status, const char *ctype,
                      const char *body, size_t body_len, int keep_alive) {
    char head[512];
    int n = snprintf(head, sizeof(head),
                      "HTTP/1.1 %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %lu\r\n"
                      "Cache-Control: no-store\r\n"
                      "X-Content-Type-Options: nosniff\r\n"
                      "Connection: %s\r\n\r\n",
                      status, ctype, (unsigned long)body_len,
                      keep_alive ? "keep-alive" : "close");
    if (n <= 0 || (size_t)n >= sizeof(head)) return;
    (void)ca_net_send(fd, head, (size_t)n);
    if (body != NULL && body_len > 0) (void)ca_net_send(fd, body, body_len);
}

/* 从请求体里取一个字符串字段（零分配游标扫描）。 */
static int json_get_string(const char *body, const char *key, char *out,
                            size_t cap) {
    ca_json_cursor root, v;
    ca_json_init(&root, body, strlen(body));
    if (ca_json_enter_object(&root) != 0) return -1;
    if (ca_json_field(&root, key, &v) != 1) return -1;
    if (ca_json_read_string(&v, out, cap, NULL) != 0) return -1;
    return 0;
}

static void handle_sse(ca_web *w, int fd) {
    static const char kHead[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: keep-alive\r\n\r\n";
    (void)ca_net_send(fd, kHead, sizeof(kHead) - 1);

    /* 先发一条 info，让页面立刻拿到会话信息 */
    char info[512];
    snprintf(info, sizeof(info),
             "{\"kind\":\"info\",\"text\":\"已连接\",\"name\":\"\",\"status\":\"\"}");
    sse_send(fd, "info", info);

    /* 发送超时：客户端连上却不读时，代理不能被它拖住。 */
    ca_net_set_send_timeout(fd, 2000);

    ca_net_mutex_lock(&w->lock);
    if (w->client_count < CA_WEB_MAX_CLIENTS) {
        w->clients[w->client_count].fd = fd;
        w->clients[w->client_count].refs = 0;
        w->clients[w->client_count].dead = 0;
        w->client_count++;
        ca_net_mutex_unlock(&w->lock);
        /* 一直挂着，直到对端关闭 */
        char buf[512];
        for (;;) {
            long n = ca_net_recv(fd, buf, sizeof(buf));
            if (n <= 0) break;
        }
        /* 注销：**必须等引用归零**才能 close(fd)——
         * 否则 fd 释放后可能被新连接复用，而另一个线程还在往它写，
         * 事件就泄露给别的客户端了。 */
        ca_net_mutex_lock(&w->lock);
        for (int i = 0; i < w->client_count; i++) {
            if (w->clients[i].fd == fd) {
                while (w->clients[i].refs > 0) {
                    ca_net_cond_broadcast(&w->approval_cond);
                    (void)ca_net_cond_wait(&w->approval_cond, &w->lock, 100);
                }
                w->clients[i] = w->clients[w->client_count - 1];
                w->client_count--;
                break;
            }
        }
        ca_net_mutex_unlock(&w->lock);
    } else {
        ca_net_mutex_unlock(&w->lock);
        (void)ca_net_send(fd, "event: error\ndata: {\"kind\":\"error\",\"text\":\"连接数已达上限\"}\n\n",
                           65);
    }
    ca_net_close(fd);
}

struct conn_arg {
    ca_web *w;
    int fd;
};

static void conn_thread(void *p) {
    struct conn_arg *ca = (struct conn_arg *)p;
    ca_web *w = ca->w;
    int fd = ca->fd;

    char buf[8192];
    size_t used = 0;
    char headers[4096];
    headers[0] = '\0';
    size_t header_len = 0;
    char body[CA_WEB_MAX_BODY];
    size_t body_len = 0;
    int content_length = -1;
    int have_head = 0;

    /* 读请求头 */
    while (!have_head) {
        long n = ca_net_recv(fd, buf + used, sizeof(buf) - used);
        if (n <= 0) {
            ca_net_close(fd);
            free(ca);
            return;
        }
        used += (size_t)n;
        char *end = strstr(buf, "\r\n\r\n");
        if (end != NULL) {
            header_len = (size_t)(end - buf) + 4;
            have_head = 1;
        } else if (used >= sizeof(buf)) {
            ca_net_close(fd);
            free(ca);
            return;
        }
    }
    memcpy(headers, buf, header_len > sizeof(headers) ? sizeof(headers) : header_len);
    headers[sizeof(headers) - 1] = '\0';

    /* 解析请求行与头 */
    web_request req;
    memset(&req, 0, sizeof(req));
    {
        /* 请求行单独取一份副本。
         *
         * 早先直接在 headers 里把第一个 CRLF 换成 '\0'，结果整个头部块
         * 就被截断到只剩请求行——后面的头一个都扫不到，
         * 于是「带了令牌也 401」。头部必须**原样保留**给 header_value 扫。 */
        char first_line[512];
        const char *eol = strstr(headers, "\r\n");
        size_t flen = (eol != NULL) ? (size_t)(eol - headers) : strlen(headers);
        if (flen >= sizeof(first_line)) flen = sizeof(first_line) - 1;
        memcpy(first_line, headers, flen);
        first_line[flen] = '\0';

        char method[16] = {0};
        char target[512] = {0};
        if (sscanf(first_line, "%15s %511s", method, target) == 2) {
            /* 不用 strncpy：它可能截断，GCC 会报 -Wstringop-truncation，
             * 而 CI 把告警当失败。这里显式判长度再拷贝。 */
            size_t mlen = strlen(method);
            if (mlen >= sizeof(req.method)) mlen = sizeof(req.method) - 1;
            memcpy(req.method, method, mlen);
            req.method[mlen] = '\0';
            parse_target(target, req.path, sizeof(req.path), req.query,
                         sizeof(req.query));
        }
        (void)header_value(headers, "Host", req.host, sizeof(req.host));
        if (header_value(headers, "X-Cagent-Token", req.token,
                         sizeof(req.token)) == 0) {
            req.has_token_header = 1;
        }
        char cl[32];
        if (header_value(headers, "Content-Length", cl, sizeof(cl)) == 0) {
            content_length = atoi(cl);
            if (content_length < 0 || content_length > CA_WEB_MAX_BODY) {
                content_length = -1;
            }
        }
    }

    /* 读请求体 */
    if (content_length > 0) {
        size_t have = used - header_len;
        if (have > (size_t)content_length) have = (size_t)content_length;
        memcpy(body, buf + header_len, have);
        body_len = have;
        while (body_len < (size_t)content_length) {
            long n = ca_net_recv(fd, body + body_len,
                                 (size_t)content_length - body_len);
            if (n <= 0) break;
            body_len += (size_t)n;
        }
        body[body_len] = '\0';
    } else {
        body[0] = '\0';
    }

    /* 路由 */
    if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/") == 0) {
        /* 页面按片段发送：长度不超单个字符串字面量的限制。 */
        size_t nchunks = 0;
        const char *const *chunks = ca_web_page_chunks(&nchunks);
        size_t total = 0;
        for (size_t i = 0; i < nchunks; i++) total += strlen(chunks[i]);
        char head[256];
        int hn = snprintf(head, sizeof(head),
                          "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/html; charset=utf-8\r\n"
                          "Content-Length: %lu\r\n"
                          "Cache-Control: no-store\r\n"
                          "X-Content-Type-Options: nosniff\r\n"
                          "Connection: close\r\n\r\n",
                          (unsigned long)total);
        if (hn > 0 && (size_t)hn < sizeof(head)) {
            (void)ca_net_send(fd, head, (size_t)hn);
            for (size_t i = 0; i < nchunks; i++) {
                (void)ca_net_send(fd, chunks[i], strlen(chunks[i]));
            }
        }
    } else if (strcmp(req.method, "GET") == 0 &&
               strcmp(req.path, "/api/events") == 0) {
        if (!host_ok(w, &req) || !token_ok(w, &req)) {
            http_send(fd, "401 Unauthorized", "text/plain; charset=utf-8",
                      "unauthorized", 12, 0);
        } else {
            handle_sse(w, fd);
            free(ca);
            ca_net_mutex_lock(&w->lock);
            w->conn_count--;
            ca_net_mutex_unlock(&w->lock);
            return; /* handle_sse 已关闭 fd */
        }
    } else if (strcmp(req.method, "GET") == 0 &&
               strcmp(req.path, "/api/info") == 0) {
        if (!host_ok(w, &req) || !token_ok(w, &req)) {
            http_send(fd, "401 Unauthorized", "text/plain; charset=utf-8",
                      "unauthorized", 12, 0);
        } else {
            http_send(fd, "200 OK", "application/json; charset=utf-8",
                      "{\"ok\":true}", 11, 0);
        }
    } else if (strcmp(req.method, "POST") == 0 &&
               strcmp(req.path, "/api/send") == 0) {
        if (!host_ok(w, &req) || !token_ok(w, &req)) {
            http_send(fd, "401 Unauthorized", "text/plain; charset=utf-8",
                      "unauthorized", 12, 0);
        } else {
            char text[CA_WEB_MSG_MAX];
            if (json_get_string(body, "text", text, sizeof(text)) != 0) {
                http_send(fd, "400 Bad Request", "text/plain; charset=utf-8",
                          "bad request", 11, 0);
            } else if (ca_web_push_user(w, text) != 0) {
                http_send(fd, "503 Service Unavailable",
                          "text/plain; charset=utf-8", "queue full", 10, 0);
            } else {
                http_send(fd, "200 OK", "application/json; charset=utf-8",
                          "{\"ok\":true}", 11, 0);
            }
        }
    } else if (strcmp(req.method, "POST") == 0 &&
               strcmp(req.path, "/api/approve") == 0) {
        if (!host_ok(w, &req) || !token_ok(w, &req)) {
            http_send(fd, "401 Unauthorized", "text/plain; charset=utf-8",
                      "unauthorized", 12, 0);
        } else {
            char decision[32];
            if (json_get_string(body, "decision", decision,
                                sizeof(decision)) != 0) {
                http_send(fd, "400 Bad Request", "text/plain; charset=utf-8",
                          "bad request", 11, 0);
            } else {
                int allow = (strcmp(decision, "allow") == 0) ? 1 : 0;
                ca_web_set_approval(w, allow);
                http_send(fd, "200 OK", "application/json; charset=utf-8",
                          "{\"ok\":true}", 11, 0);
            }
        }
    } else if (strcmp(req.method, "POST") == 0 &&
               strcmp(req.path, "/api/config") == 0) {
        if (!host_ok(w, &req) || !token_ok(w, &req)) {
            http_send(fd, "401 Unauthorized", "text/plain; charset=utf-8",
                      "unauthorized", 12, 0);
        } else if (w->on_config == NULL) {
            http_send(fd, "501 Not Implemented", "text/plain; charset=utf-8",
                      "config not supported", 19, 0);
        } else {
            char api_base[256] = {0};
            char model[256] = {0};
            char api_key[512] = {0};
            int have_base = json_get_string(body, "api_base", api_base,
                                            sizeof(api_base)) == 0;
            int have_model = json_get_string(body, "model", model,
                                             sizeof(model)) == 0;
            int have_key = json_get_string(body, "api_key", api_key,
                                           sizeof(api_key)) == 0;
            if (!have_base && !have_model && !have_key) {
                http_send(fd, "400 Bad Request", "text/plain; charset=utf-8",
                          "need api_base/model/api_key", 26, 0);
            } else {
                w->on_config(have_base ? api_base : NULL,
                             have_model ? model : NULL,
                             have_key ? api_key : NULL,
                             w->config_user);
                http_send(fd, "200 OK", "application/json; charset=utf-8",
                          "{\"ok\":true}", 11, 0);
            }
        }
    } else {
        http_send(fd, "404 Not Found", "text/plain; charset=utf-8", "not found",
                  9, 0);
    }

    ca_net_close(fd);
    free(ca);
    ca_net_mutex_lock(&w->lock);
    w->conn_count--;
    ca_net_mutex_unlock(&w->lock);
}

static void accept_thread(void *arg) {
    ca_web *w = (ca_web *)arg;
    for (;;) {
        int fd = ca_net_accept(w->listen_fd);
        if (fd < 0) {
            if (w->stop) break;
            continue;
        }
        /* 连接数上限：超了就立刻回一句 503 并断开，不给线程。 */
        ca_net_mutex_lock(&w->lock);
        int too_many = (w->conn_count >= CA_WEB_MAX_CONNS);
        if (!too_many) w->conn_count++;
        ca_net_mutex_unlock(&w->lock);
        if (too_many) {
            static const char kBusy[] =
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: 12\r\n"
                "Connection: close\r\n\r\n"
                "too many conn";
            (void)ca_net_send(fd, kBusy, sizeof(kBusy) - 1);
            ca_net_close(fd);
            continue;
        }
        struct conn_arg *ca = (struct conn_arg *)malloc(sizeof(*ca));
        if (ca == NULL) {
            ca_net_mutex_lock(&w->lock);
            w->conn_count--;
            ca_net_mutex_unlock(&w->lock);
            ca_net_close(fd);
            continue;
        }
        ca->w = w;
        ca->fd = fd;
        if (ca_net_thread_start(conn_thread, ca) != 0) {
            ca_net_mutex_lock(&w->lock);
            w->conn_count--;
            ca_net_mutex_unlock(&w->lock);
            ca_net_close(fd);
            free(ca);
        }
    }
}

/* ------------------------------------------------------------------ 生命周期 */

int ca_web_start(ca_web **out, int port) {
    if (out == NULL) return -1;
    *out = NULL;
    if (ca_net_init() != 0) return -1;

    ca_web *w = (ca_web *)calloc(1, sizeof(ca_web));
    if (w == NULL) return -1;
    w->listen_fd = -1;
    if (ca_net_mutex_init(&w->lock) != 0) {
        free(w);
        return -1;
    }
    if (ca_net_cond_init(&w->queue_cond) != 0 ||
        ca_net_cond_init(&w->approval_cond) != 0) {
        free(w);
        return -1;
    }
    if (make_token(w->token, sizeof(w->token)) != 0) {
        free(w);
        return -1;
    }
    w->listen_fd = ca_net_listen(port);
    if (w->listen_fd < 0) {
        free(w);
        return -1;
    }
    w->port = ca_net_local_port(w->listen_fd);
    if (w->port <= 0) w->port = port;
    snprintf(w->url, sizeof(w->url), "http://127.0.0.1:%d/?t=%s", w->port,
             w->token);

    if (ca_net_thread_start(accept_thread, w) != 0) {
        ca_net_close(w->listen_fd);
        free(w);
        return -1;
    }
    *out = w;
    return 0;
}

void ca_web_stop(ca_web *w) {
    if (w == NULL) return;
    ca_net_mutex_lock(&w->lock);
    w->stop = 1;
    ca_net_cond_broadcast(&w->queue_cond);
    ca_net_cond_broadcast(&w->approval_cond);
    ca_net_mutex_unlock(&w->lock);
    if (w->listen_fd >= 0) ca_net_close(w->listen_fd);
    ca_net_mutex_destroy(&w->lock);
    ca_net_cond_destroy(&w->queue_cond);
    ca_net_cond_destroy(&w->approval_cond);
    free(w);
}

const char *ca_web_url(const ca_web *w) { return (w != NULL) ? w->url : ""; }
const char *ca_web_token(const ca_web *w) { return (w != NULL) ? w->token : ""; }
int ca_web_port(const ca_web *w) { return (w != NULL) ? w->port : 0; }

void ca_web_set_config_cb(ca_web *w,
                          void (*on_config)(const char *api_base,
                                            const char *model,
                                            const char *api_key,
                                            void *user),
                          void *user) {
    if (w == NULL) return;
    w->on_config = on_config;
    w->config_user = user;
}
