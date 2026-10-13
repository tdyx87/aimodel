// 内部套接字抽象（不安装、不进公共头）。
// 平台分支只用编译期宏（agents.md §7.3）；套接字操作实现于
// local_http_{win32,posix}.cpp，公共逻辑只见 net_socket_t 与 Net* 声明。

#ifndef SAVEINSPECTOR_LOCAL_HTTP_IMPL_H_
#define SAVEINSPECTOR_LOCAL_HTTP_IMPL_H_

#include <atomic>
#include <string>

#if defined(_WIN32) || defined(_WIN64)
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET net_socket_t;
#  define NET_INVALID_SOCKET INVALID_SOCKET
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
typedef int net_socket_t;
#  define NET_INVALID_SOCKET (-1)
#endif

namespace saveinspector {

// 全局网络初始化（Windows: WSAStartup；POSIX: 忽略 SIGPIPE）。按调用计数，幂等。
bool NetGlobalInit(std::string* error);
void NetGlobalShutdown();

// 平台网络操作集合。
struct HttpServerImpl {
    net_socket_t listen_sock;
    std::atomic<bool> stopping;
    int bound_port;

    HttpServerImpl();

    bool Listen(const std::string& host, int port, std::string* error);
    // 1 = 新连接，0 = 超时，-1 = 出错或已请求停止
    int Accept(net_socket_t* out, int timeout_ms);
    // >0 字节数，0 = 对端关闭，-1 = 超时或错误
    int Recv(net_socket_t s, char* buf, int cap, int timeout_ms);
    bool SendAll(net_socket_t s, const char* data, size_t len);
    void CloseSocket(net_socket_t s);
    void CloseListener();  // 仅在工作线程退出后调用（避免跨线程 close 竞争）
};

}  // namespace saveinspector

#endif  // SAVEINSPECTOR_LOCAL_HTTP_IMPL_H_
