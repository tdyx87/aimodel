/* web_net.c —— Web UI 的平台实现（见 web_net.h 的说明） */

#include "web_net.h"

#include <string.h>
#include <time.h>

#if defined(_WIN32)

#  define _CRT_SECURE_NO_WARNINGS 1
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <bcrypt.h>
#  include <process.h>

typedef SOCKET ca_socket_t;
#  define CA_INVALID_SOCKET INVALID_SOCKET
#  define CA_CLOSE(fd) closesocket((ca_socket_t)(fd))
#  define CA_SOCKET_ERRNO WSAGetLastError()

typedef CRITICAL_SECTION ca_mutex_impl;
typedef CONDITION_VARIABLE ca_cond_impl;

struct ca_net_thread_ctx {
    ca_net_thread_fn fn;
    void *arg;
};

static DWORD WINAPI win_thread_proc(LPVOID p) {
    struct ca_net_thread_ctx *ctx = (struct ca_net_thread_ctx *)p;
    ca_net_thread_fn fn = ctx->fn;
    void *arg = ctx->arg;
    free(ctx);
    fn(arg);
    return 0;
}

int ca_net_init(void) {
    WSADATA wsa;
    static int done = 0;
    if (done) return 0;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    done = 1;
    return 0;
}

void ca_net_cleanup(void) { /* 进程退出时由 OS 回收；显式调用也无妨 */ }

int ca_net_listen(int port) {
    ca_socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == CA_INVALID_SOCKET) return -1;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    /* 只绑本机回环：这个服务能让模型执行命令，绝不能暴露到局域网。 */
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(s, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        CA_CLOSE(s);
        return -1;
    }
    if (listen(s, 16) != 0) {
        CA_CLOSE(s);
        return -1;
    }
    return (int)s;
}

int ca_net_accept(int listen_fd) {
    struct sockaddr_in peer;
    int plen = (int)sizeof(peer);
    ca_socket_t c = accept((ca_socket_t)listen_fd, (struct sockaddr *)&peer, &plen);
    if (c == CA_INVALID_SOCKET) return -1;
    return (int)c;
}

long ca_net_send(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = send((ca_socket_t)fd, data + sent, (int)(len - sent), 0);
        if (n <= 0) return (long)sent;
        sent += (size_t)n;
    }
    return (long)sent;
}

long ca_net_recv(int fd, char *buf, size_t cap) {
    int n = recv((ca_socket_t)fd, buf, (int)cap, 0);
    return (long)n;
}

void ca_net_close(int fd) { CA_CLOSE(fd); }

void ca_net_set_send_timeout(int fd, int ms) {
    if (ms <= 0) return;
    DWORD tv = (DWORD)ms;
    (void)setsockopt((ca_socket_t)fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv,
                      sizeof(tv));
}

int ca_net_local_port(int fd) {
    struct sockaddr_in addr;
    int len = (int)sizeof(addr);
    if (getsockname((ca_socket_t)fd, (struct sockaddr *)&addr, &len) != 0) return 0;
    return (int)ntohs(addr.sin_port);
}

int ca_net_thread_start(ca_net_thread_fn fn, void *arg) {
    struct ca_net_thread_ctx *ctx =
        (struct ca_net_thread_ctx *)malloc(sizeof(*ctx));
    if (ctx == NULL) return -1;
    ctx->fn = fn;
    ctx->arg = arg;
    HANDLE h = CreateThread(NULL, 0, win_thread_proc, ctx, 0, NULL);
    if (h == NULL) {
        free(ctx);
        return -1;
    }
    CloseHandle(h);
    return 0;
}

void ca_net_thread_join(long handle) {
    (void)handle; /* CreateThread 的句柄已关闭，线程自己结束 */
}

int ca_net_mutex_init(ca_net_mutex *m) {
    CRITICAL_SECTION *cs = (CRITICAL_SECTION *)malloc(sizeof(CRITICAL_SECTION));
    if (cs == NULL) return -1;
    InitializeCriticalSection(cs);
    m->impl = cs;
    return 0;
}

void ca_net_mutex_destroy(ca_net_mutex *m) {
    if (m->impl == NULL) return;
    DeleteCriticalSection((CRITICAL_SECTION *)m->impl);
    free(m->impl);
    m->impl = NULL;
}

void ca_net_mutex_lock(ca_net_mutex *m) {
    EnterCriticalSection((CRITICAL_SECTION *)m->impl);
}
void ca_net_mutex_unlock(ca_net_mutex *m) {
    LeaveCriticalSection((CRITICAL_SECTION *)m->impl);
}

int ca_net_cond_init(ca_net_cond *c) {
    CONDITION_VARIABLE *cv = (CONDITION_VARIABLE *)malloc(sizeof(CONDITION_VARIABLE));
    if (cv == NULL) return -1;
    InitializeConditionVariable(cv);
    c->impl = cv;
    return 0;
}

void ca_net_cond_destroy(ca_net_cond *c) {
    if (c->impl == NULL) return;
    free(c->impl);
    c->impl = NULL;
}

int ca_net_cond_wait(ca_net_cond *c, ca_net_mutex *m, int timeout_ms) {
    DWORD ms = (timeout_ms < 0) ? INFINITE : (DWORD)timeout_ms;
    if (SleepConditionVariableCS((CONDITION_VARIABLE *)c->impl,
                                 (CRITICAL_SECTION *)m->impl, ms)) {
        return 0;
    }
    return 1; /* 超时 */
}

void ca_net_cond_signal(ca_net_cond *c) {
    WakeConditionVariable((CONDITION_VARIABLE *)c->impl);
}
void ca_net_cond_broadcast(ca_net_cond *c) {
    WakeAllConditionVariable((CONDITION_VARIABLE *)c->impl);
}

int ca_net_random(void *buf, size_t n) {
    /* Windows 的系统熵源：BCryptGenRandom。
     *
     * 不用 rand_s：它在严格 C 标准模式下会被隐藏（MSVC 把 rand_s 放在
     * 自己的扩展里），一隐藏就变成「隐式声明返回 int」——正好踩中本项目
     * 关掉编译器扩展后最危险的那种坑。
     * BCrypt 是系统 DLL，导入表多一个条目，运行时不增加负担。 */
    if (BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)n,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) {
        return 0;
    }
    /* 拿不到系统熵就退化：时间 + 进程号 + 地址混合。
     * token 只用于挡住「本机其它进程随手访问」，强度要求不高，
     * 但仍然不该是可预测的常量。 */
    unsigned char *p = (unsigned char *)buf;
    unsigned long seed = (unsigned long)time(NULL) ^
                         (unsigned long)GetCurrentProcessId() ^
                         (unsigned long)(size_t)p;
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1103515245UL + 12345UL;
        p[i] = (unsigned char)((seed >> 16) & 0xFFu);
    }
    return 0;
}

#else /* POSIX */

#  include <arpa/inet.h>
#  include <errno.h>
#  include <netinet/in.h>
#  include <pthread.h>
#  include <stdlib.h>
#  include <sys/socket.h>
#  include <unistd.h>

typedef int ca_socket_t;
#  define CA_INVALID_SOCKET (-1)
#  define CA_CLOSE(fd) close(fd)

typedef pthread_mutex_t ca_mutex_impl;
typedef pthread_cond_t ca_cond_impl;

int ca_net_init(void) { return 0; }
void ca_net_cleanup(void) { }

int ca_net_listen(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 只绑回环 */

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        CA_CLOSE(s);
        return -1;
    }
    if (listen(s, 16) != 0) {
        CA_CLOSE(s);
        return -1;
    }
    return s;
}

int ca_net_accept(int listen_fd) {
    struct sockaddr_in peer;
    socklen_t plen = (socklen_t)sizeof(peer);
    int c = accept(listen_fd, (struct sockaddr *)&peer, &plen);
    return c;
}

long ca_net_send(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return (long)sent;
        }
        sent += (size_t)n;
    }
    return (long)sent;
}

long ca_net_recv(int fd, char *buf, size_t cap) {
    ssize_t n;
    do {
        n = recv(fd, buf, cap, 0);
    } while (n < 0 && errno == EINTR);
    return (long)n;
}

void ca_net_close(int fd) { CA_CLOSE(fd); }

void ca_net_set_send_timeout(int fd, int ms) {
    if (ms <= 0) return;
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

int ca_net_local_port(int fd) {
    struct sockaddr_in addr;
    socklen_t len = (socklen_t)sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) return 0;
    return (int)ntohs(addr.sin_port);
}

static void *posix_thread_proc(void *p) {
    struct ca_net_thread_ctx {
        ca_net_thread_fn fn;
        void *arg;
    } *ctx = (struct ca_net_thread_ctx *)p;
    ca_net_thread_fn fn = ctx->fn;
    void *arg = ctx->arg;
    free(ctx);
    fn(arg);
    return NULL;
}

int ca_net_thread_start(ca_net_thread_fn fn, void *arg) {
    struct ca_net_thread_ctx *ctx =
        (struct ca_net_thread_ctx *)malloc(sizeof(*ctx));
    if (ctx == NULL) return -1;
    ctx->fn = fn;
    ctx->arg = arg;
    pthread_t tid;
    if (pthread_create(&tid, NULL, posix_thread_proc, ctx) != 0) {
        free(ctx);
        return -1;
    }
    pthread_detach(tid);
    return 0;
}

void ca_net_thread_join(long handle) { (void)handle; }

int ca_net_mutex_init(ca_net_mutex *m) {
    pthread_mutex_t *mu = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    if (mu == NULL) return -1;
    if (pthread_mutex_init(mu, NULL) != 0) {
        free(mu);
        return -1;
    }
    m->impl = mu;
    return 0;
}

void ca_net_mutex_destroy(ca_net_mutex *m) {
    if (m->impl == NULL) return;
    pthread_mutex_destroy((pthread_mutex_t *)m->impl);
    free(m->impl);
    m->impl = NULL;
}

void ca_net_mutex_lock(ca_net_mutex *m) { pthread_mutex_lock((pthread_mutex_t *)m->impl); }
void ca_net_mutex_unlock(ca_net_mutex *m) { pthread_mutex_unlock((pthread_mutex_t *)m->impl); }

int ca_net_cond_init(ca_net_cond *c) {
    pthread_cond_t *cv = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
    if (cv == NULL) return -1;
    if (pthread_cond_init(cv, NULL) != 0) {
        free(cv);
        return -1;
    }
    c->impl = cv;
    return 0;
}

void ca_net_cond_destroy(ca_net_cond *c) {
    if (c->impl == NULL) return;
    pthread_cond_destroy((pthread_cond_t *)c->impl);
    free(c->impl);
    c->impl = NULL;
}

int ca_net_cond_wait(ca_net_cond *c, ca_net_mutex *m, int timeout_ms) {
    if (timeout_ms < 0) {
        pthread_cond_wait((pthread_cond_t *)c->impl, (pthread_mutex_t *)m->impl);
        return 0;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    int rc = pthread_cond_timedwait((pthread_cond_t *)c->impl,
                                    (pthread_mutex_t *)m->impl, &ts);
    return (rc == ETIMEDOUT) ? 1 : 0;
}

void ca_net_cond_signal(ca_net_cond *c) { pthread_cond_signal((pthread_cond_t *)c->impl); }
void ca_net_cond_broadcast(ca_net_cond *c) { pthread_cond_cond_broadcast((pthread_cond_t *)c->impl); }

int ca_net_random(void *buf, size_t n) {
    /* /dev/urandom：POSIX 上的加密级随机源。
     * 读不到就退化到时间+地址混合（本地 token 场景可接受）。 */
    FILE *f = fopen("/dev/urandom", "rb");
    if (f != NULL) {
        size_t got = fread(buf, 1, n, f);
        fclose(f);
        if (got == n) return 0;
    }
    unsigned char *p = (unsigned char *)buf;
    unsigned long seed = (unsigned long)time(NULL) ^ (unsigned long)(size_t)buf;
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1103515245UL + 12345UL;
        p[i] = (unsigned char)((seed >> 16) & 0xFFu);
    }
    return 0;
}

#endif /* _WIN32 */
