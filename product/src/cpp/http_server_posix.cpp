// POSIX 套接字实现（Linux/macOS）。

#include "http_server_impl.h"

#include <csignal>
#include <cstring>
#include <mutex>

namespace aimodel {

namespace {
// SIGPIPE 在断开连接上写时被阻断（避免单次写崩溃）。
bool g_pipe_ignored = false;
std::mutex g_pipe_mutex;
}

bool NetGlobalInit(std::string* error) {
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
    if (host != "127.0.0.1") {
        if (error != NULL) *error = "出于隐私保护，仅支持 127.0.0.1";
        return false;
    }
    listen_sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock == NET_INVALID_SOCKET) {
        if (error != NULL) *error = "创建套接字失败";
        return false;
    }
    int opt = 1;
    ::setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listen_sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        int e = errno;
        if (error != NULL) *error = (e == EADDRINUSE) ? "端口已被占用" : "绑定失败";
        return false;
    }
    if (::listen(listen_sock, SOMAXCONN) != 0) {
        if (error != NULL) *error = "监听失败";
        return false;
    }
    if (::getsockname(listen_sock, (sockaddr*)&addr, (socklen_t*)&addr) == 0) {
        bound_port = ntohs(addr.sin_port);
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
    int sel = ::select(listen_sock + 1, &readfds, NULL, NULL, &tv);
    if (sel <= 0) {
        return sel;  // 0=超时，-1=错误
    }
    sockaddr_in src;
    socklen_t slen = sizeof(src);
    net_socket_t client = ::accept(listen_sock, (sockaddr*)&src, &slen);
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
    int sel = ::select(s + 1, &readfds, NULL, NULL, &tv);
    if (sel <= 0) {
        return -1;
    }
    int n = ::recv(s, buf, cap, 0);
    return n;
}

bool HttpServerImpl::SendAll(net_socket_t s, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(s, data + sent, static_cast<size_t>(len - sent), MSG_NOSIGNAL);
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
        s = NET_INVALID_SOCKET;
    }
}

void HttpServerImpl::CloseListener() {
    if (listen_sock != NET_INVALID_SOCKET) {
        ::close(listen_sock);
        listen_sock = NET_INVALID_SOCKET;
    }
}

}  // namespace aimodel
