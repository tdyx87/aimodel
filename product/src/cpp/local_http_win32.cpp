// Windows 套接字实现（local_http_impl.h 的 HttpServerImpl）。
// 本文件只做套接字；请求解析、闸门、路由在 local_http.cpp（平台无关）。

#define WIN32_LEAN_AND_MEAN
#include "local_http_impl.h"

#include <cstring>
#include <mutex>

namespace saveinspector {
namespace {

// WSAStartup 幂等引用计数（多次 Start/Stop 安全）。
int g_wsa_refcount = 0;
std::mutex g_wsa_mutex;

}  // namespace

bool NetGlobalInit(std::string* error) {
    std::lock_guard<std::mutex> lock(g_wsa_mutex);
    if (g_wsa_refcount > 0) {
        ++g_wsa_refcount;
        return true;
    }
    WSADATA wsa;
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        if (error != NULL) {
            *error = "网络初始化失败，请检查系统网络组件。";
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
    // 隐私红线（PRD NFR-3）：只允许本机回环。libuv/引擎侧不做可达性放开。
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
    ::setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int e = ::WSAGetLastError();
        if (error != NULL) {
            *error = (e == WSAEADDRINUSE) ? "端口已被占用，请稍后重试。"
                                          : "绑定本机端口失败。";
        }
        // 失败路径必须回收，否则每次重试泄漏一个套接字句柄。
        // 注意此处**不**调 NetGlobalShutdown：引用计数的成对释放由
        // LocalHttpServer::Start 统一负责（再减一次会打乱跨实例计数）。
        ::closesocket(listen_sock);
        listen_sock = NET_INVALID_SOCKET;
        return false;
    }
    if (::listen(listen_sock, SOMAXCONN) != 0) {
        if (error != NULL) *error = "监听失败。";
        ::closesocket(listen_sock);
        listen_sock = NET_INVALID_SOCKET;
        return false;
    }

    sockaddr_in bound;
    int bound_len = sizeof(bound);
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
    const int sel = ::select(static_cast<int>(listen_sock) + 1, &readfds, NULL, NULL, &tv);
    if (sel <= 0) {
        return sel;  // 0 = 超时，-1 = 错误
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
    const int sel = ::select(static_cast<int>(s) + 1, &readfds, NULL, NULL, &tv);
    if (sel <= 0) {
        return -1;  // 超时
    }
    return ::recv(s, buf, cap, 0);  // 0 = 对端关闭，-1 = 错误
}

bool HttpServerImpl::SendAll(net_socket_t s, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const int n = ::send(s, data + sent, static_cast<int>(len - sent), 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

void HttpServerImpl::CloseSocket(net_socket_t s) {
    // 形参是值传递，赋值给 s 无意义；调用方各自记账。
    if (s != NET_INVALID_SOCKET) {
        ::closesocket(s);
    }
}

void HttpServerImpl::CloseListener() {
    if (listen_sock != NET_INVALID_SOCKET) {
        ::closesocket(listen_sock);
        listen_sock = NET_INVALID_SOCKET;
    }
}

}  // namespace saveinspector
