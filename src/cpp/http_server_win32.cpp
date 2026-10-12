// Windows 套接字实现（http_server_impl.h 的 HttpServerImpl）。

#define WIN32_LEAN_AND_MEAN
#include "http_server_impl.h"

#include <mutex>

namespace aimodel {

namespace {
// WSAStartup 幂等引用计数（多次 Start/Stop 安全）。
int g_wsa_refcount = 0;
std::mutex g_wsa_mutex;
}

bool NetGlobalInit(std::string* error) {
    std::lock_guard<std::mutex> lock(g_wsa_mutex);
    if (g_wsa_refcount > 0) {
        ++g_wsa_refcount;
        return true;
    }
    WSADATA wsa;
    int rc = ::WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        if (error != NULL) {
            *error = std::to_string(WSAGetLastError()) + "：网络初始化失败";
        }
        return false;
    }
    g_wsa_refcount = 1;
    return true;
}

void NetGlobalShutdown() {
    std::lock_guard<std::mutex> lock(g_wsa_mutex);
    if (g_wsa_refcount > 0 && --g_wsa_refcount == 0) {
        ::WSACleanup();
    }
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
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listen_sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        int e = ::WSAGetLastError();
        if (error != NULL) *error = (e == WSAEADDRINUSE) ? "端口已被占用" : "绑定失败";
        NetGlobalShutdown();
        return false;
    }
    if (::listen(listen_sock, SOMAXCONN) != 0) {
        if (error != NULL) *error = "监听失败";
        NetGlobalShutdown();
        return false;
    }
    sockaddr_in name;
    int namelen = sizeof(addr);
    if (::getsockname(listen_sock, (sockaddr*)&name, &namelen) == 0) {
        bound_port = ntohs(name.sin_port);
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
    int sel = ::select((int)listen_sock + 1, &readfds, NULL, NULL, &tv);
    if (sel <= 0) {
        return sel;  // 0=超时，-1=错误
    }
    net_socket_t client = ::accept(listen_sock, NULL, NULL);
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
    int sel = ::select(static_cast<int>(s) + 1, &readfds, NULL, NULL, &tv);
    if (sel <= 0) {
        return -1;
    }
    int n = ::recv(s, buf, cap, 0);
    if (n <= 0) {
        return n;
    }
    return n;
}

bool HttpServerImpl::SendAll(net_socket_t s, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = ::send(s, data + static_cast<size_t>(sent), (int)(len - sent), 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

void HttpServerImpl::CloseSocket(net_socket_t s) {
    if (s != NET_INVALID_SOCKET) {
        ::closesocket(s);
        s = NET_INVALID_SOCKET;
    }
}

void HttpServerImpl::CloseListener() {
    if (listen_sock != NET_INVALID_SOCKET) {
        ::closesocket(listen_sock);
        listen_sock = NET_INVALID_SOCKET;
    }
}

}  // namespace aimodel
