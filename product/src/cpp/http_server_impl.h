// 内部套接字抽象（不安装、不进公共头）。
// 平台分支只用编译期宏（agents.md 6.3）；套接字操作实现于
// http_server_{win32,posix}.cpp，公共逻辑只见 net_socket_t 与 Net* 声明。

#ifndef AIMODEL_HTTP_SERVER_IMPL_H_
#define AIMODEL_HTTP_SERVER_IMPL_H_

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
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
typedef int net_socket_t;
#  define NET_INVALID_SOCKET (-1)
#endif

namespace aimodel {

// 全局网络初始化（Windows: WSAStartup；POSIX: 空操作）。按调用计数，幂等。
bool NetGlobalInit(std::string* error);
void NetGlobalShutdown();

// 平台网络操作集合，实现见 http_server_{win32,posix}.cpp。
struct HttpServerImpl {
    net_socket_t listen_sock;
    std::atomic<bool> stopping;
    int bound_port;

    HttpServerImpl();

    // 监听 host:port（port=0 时由系统分配），成功填 bound_port。
    bool Listen(const std::string& host, int port, std::string* error);
    // 等待连接：1 = 新连接(out 已填)，0 = 超时，-1 = 出错或已请求停止。
    int Accept(net_socket_t* out, int timeout_ms);
    // 读请求：>0 字节数，0 = 对端关闭，-1 = 错误/超时。
    int Recv(net_socket_t s, char* buf, int cap, int timeout_ms);
    // 全量发送，false = 失败。
    bool SendAll(net_socket_t s, const char* data, size_t len);
    void CloseSocket(net_socket_t s);
    void CloseListener();  // 仅在工作线程退出后由 Stop 调用（避免跨线程 close 竞争）
};

}  // namespace aimodel

#endif  // AIMODEL_HTTP_SERVER_IMPL_H_
