/* web_net.h —— Web UI 的平台层（内部头，不对外暴露）
 *
 * 为什么单独一层：契约 §8 要求「平台差异不得泄漏到业务逻辑」。
 * Web UI 需要的东西全是平台相关的：
 *   socket（Windows 是 winsock2，POSIX 是 BSD socket）
 *   线程（Windows 是 CreateThread，POSIX 是 pthread）
 *   互斥/条件变量（Windows 是 CRITICAL_SECTION/CONDITION_VARIABLE）
 *   随机数（用来生成访问 token）
 * 全部收在这里，web.c 只看到「打开一个本地监听、收一个连接、发一段字节」。
 *
 * 设计取舍：
 *   - **只监听 127.0.0.1**：这个服务能让模型执行命令，绝不能暴露到外网。
 *   - **阻塞 socket + 每连接一线程**：实现简单、行为可预测；
 *     连接数有上限（见 CA_WEB_MAX_CLIENTS），不会被打爆。
 *   - **不用任何第三方库**：与项目「依赖只有 libcurl」的约定一致。
 */
#ifndef CAGENT_WEB_NET_H
#define CAGENT_WEB_NET_H

#include <stddef.h>

/* ---- socket ---- */

/* 初始化平台网络栈（Windows 需要 WSAStartup）。可重复调用。 */
int ca_net_init(void);
void ca_net_cleanup(void);

/* 在 127.0.0.1:port 上监听。port=0 表示由系统分配。
 * 成功返回监听套接字（>=0），失败返回 -1。 */
int ca_net_listen(int port);

/* 接受一个连接。返回客户端套接字，失败返回 -1。 */
int ca_net_accept(int listen_fd);

/* 收发。send/recv 返回字节数，<=0 表示对端关闭或出错。 */
long ca_net_send(int fd, const char *data, size_t len);
long ca_net_recv(int fd, char *buf, size_t cap);

void ca_net_close(int fd);

/* 给套接字设置发送超时（毫秒）。
 * SSE 客户端连上却不读时，代理不能被它拖住——阻塞写必须有上限。 */
void ca_net_set_send_timeout(int fd, int ms);

/* 实际监听端口（port=0 时由系统分配后要查回来）。 */
int ca_net_local_port(int fd);

/* ---- 线程 ---- */

typedef void (*ca_net_thread_fn)(void *arg);

/* 起一个线程。返回 0 成功。 */
int ca_net_thread_start(ca_net_thread_fn fn, void *arg);
/* 等待线程结束（用于停机时收尾）。 */
void ca_net_thread_join(long handle);
/* 线程 ID 句柄类型（内部用 long 承载指针）。 */
typedef long ca_net_thread;

/* ---- 互斥与条件变量 ---- */

typedef struct {
    void *impl; /* CRITICAL_SECTION* 或 pthread_mutex_t* */
} ca_net_mutex;

typedef struct {
    void *impl; /* CONDITION_VARIABLE* 或 pthread_cond_t* */
} ca_net_cond;

int  ca_net_mutex_init(ca_net_mutex *m);
void ca_net_mutex_destroy(ca_net_mutex *m);
void ca_net_mutex_lock(ca_net_mutex *m);
void ca_net_mutex_unlock(ca_net_mutex *m);

int  ca_net_cond_init(ca_net_cond *c);
void ca_net_cond_destroy(ca_net_cond *c);
/* 等待。返回 0 被唤醒，1 超时。 */
int  ca_net_cond_wait(ca_net_cond *c, ca_net_mutex *m, int timeout_ms);
void ca_net_cond_signal(ca_net_cond *c);
void ca_net_cond_broadcast(ca_net_cond *c);

/* ---- 随机 ---- */

/* 填 n 字节随机数据。返回 0 成功。 */
int ca_net_random(void *buf, size_t n);

#endif /* CAGENT_WEB_NET_H */
