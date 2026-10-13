// POSIX 套接字实现（Linux/macOS）。
// 本文件只做套接字；请求解析、闸门、路由在 local_http.cpp（平台无关）。

#include "local_http_impl.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <mutex>

namespace saveinspector {
namespace {

// SIGPIPE：写入已断开的连接时若不忽略，进程会被信号终止。
bool g_pipe_ignored = false;
std::mutex g_pipe_mutex;

}  // namespace

bool NetGlobalInit(std::string* error) {
    (void)error;
    std::lock_guard<std::mutex> lock(g_pipe_mutex);
    if (!g_pipe_ignored) {
        ::signal(SIGPIPE, SIG_IGN);
        g_pipe_ignored = true;
    }
    return true;
}

void NetGlobalShutdown() {
    // POSIX 无 WSA 引用计数，空操作。
}

HttpServerImpl::HttpServerImpl()
    : listen_sock(NET_INVALID_SOCKET), stopping(false), bound_port(0) {}

bool HttpServerImpl::Listen(const std::string& host, int port, std::string* error) {
    // 隐私红线（PRD NFR-3）：只允许本机回环。
    if (host != "127.0.0.1") {
        if (error != NULL) *error = "出于隐私保护，仅支持本机访问（127.0.0.1）";
        return false;
    }
    listen_sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock == NET_INVALID_SOCKET) {
        if (error != NULL) *error = "创建网络连接失败。";
        return false;
    }
    int opt = 1;
    ::setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int e = errno;
        if (error != NULL) {
            *error = (e == EADDRINUSE) ? "端口已被占用，请稍后重试。"
                                        : "绑定本机端口失败。";
        }
        ::close(listen_sock);  // 失败路径回收，避免每次重试泄漏一个 fd
        listen_sock = NET_INVALID_SOCKET;
        return false;
    }
    if (::listen(listen_sock, SOMAXCONN) != 0) {
        if (error != NULL) *error = "监听失败。";
        ::close(listen_sock);
        listen_sock = NET_INVALID_SOCKET;
        return false;
    }

    // 端口回读：第三个参数是 socklen_t*，必须指向独立的 socklen_t。
    // 曾误写成 (socklen_t*)&addr —— 那读到的是 sin_family，port=0 时拿到随机值。
    sockaddr_in bound;
    socklen_t bound_len = sizeof(bound);
    if (::getsockname(listen_sock, reinterpret_cast<sockaddr*>(&bound),
                      &bound_len) == 0) {
        bound_port = ntohs(bound.sin_port);
    }
    return true;
}

int HttpServerImpl::Accept(net_socket_t* out, int timeout_ms) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(listen_sock, &readfds);
    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    const int sel = ::select(listen_sock + 1, &readfds, NULL, NULL, &tv);
    if (sel <= 0) {
        return sel;
    }
    const net_socket_t client = ::accept(listen_sock, NULL, NULL);
    if (client == NET_INVALID_SOCKET) {
        return -1;
    }
    *out = client;
    return 1;
}

int HttpServerImpl::Recv(net_socket_t s, char* buf, int cap, int timeout_ms) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(s, &readfds);
    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    const int sel = ::select(s + 1, &readfds, NULL, NULL, &tv);
    if (sel <= 0) {
        return -1;
    }
    return static_cast<int>(::recv(s, buf, static_cast<size_t>(cap), 0));
}

bool HttpServerImpl::SendAll(net_socket_t s, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        // MSG_NOSIGNAL 是 **Linux 特有**的（Darwin 没有这个宏），无条件使用会让
        // macOS 直接**编译失败**。macOS 侧改走「进程级忽略 SIGPIPE」这条路：
        // NetGlobalInit 已 signal(SIGPIPE, SIG_IGN)，写入已断开的连接只会返回
        // -1/EPIPE，由下面的 n <= 0 处理，进程不会被信号杀掉。
        // （另一种写法是给每个已接受连接设 SO_NOSIGPIPE，但进程级忽略已足够，
        //   且不必依赖 accept 的返回值。）
#if defined(MSG_NOSIGNAL)
        const ssize_t n = ::send(s, data + sent, len - sent, MSG_NOSIGNAL);
#else
        const ssize_t n = ::send(s, data + sent, len - sent, 0);
#endif
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

void HttpServerImpl::CloseSocket(net_socket_t s) {
    if (s != NET_INVALID_SOCKET) {
        ::close(s);
    }
}

void HttpServerImpl::CloseListener() {
    if (listen_sock != NET_INVALID_SOCKET) {
        ::close(listen_sock);
        listen_sock = NET_INVALID_SOCKET;
    }
}

}  // namespace saveinspector
