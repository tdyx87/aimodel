// 角色：cpp-implementer
// 任务：本地 HTTP 服务（承载界面 ↔ 核心库通信）
// 产物：include/saveinspector/local_http.h、src/cpp/local_http*.cpp
// 契约：docs/design/save-format-contract.md §4（通信协议）、§5（隐私）
//
// 隐私红线（PRD NFR-3）：只绑定 127.0.0.1；请求闸门挡跨站与 DNS rebinding；
// 响应体不含用户本机路径。

#ifndef SAVEINSPECTOR_LOCAL_HTTP_H_
#define SAVEINSPECTOR_LOCAL_HTTP_H_

#include <string>

#include "saveinspector/export.h"

namespace saveinspector {

struct ServerConfig {
    std::string bind_host;    // 仅接受 "127.0.0.1"（Start 内强校验）
    int port = 0;             // 0 = 系统分配；Start 成功后读 bound_port()
    std::string static_root;  // 静态资源目录（web/）；空 = 仅提供 API

    // 单次读取的等待上限（毫秒），按「距上次收到字节的间隔」计。
    // 0 = 使用内置默认 5000。
    int request_timeout_ms = 0;
};

// 本地 HTTP 服务：1 个 accept 线程 + 4 个工作线程 + 有界队列。
// 串行处理会让一个慢连接阻塞全服务（浏览器对回环默认开 6 条并发连接）。
// RAII：析构即 Stop；不可拷贝。可重复 Start/Stop。
class SAVEINSPECTOR_API LocalHttpServer {
public:
    LocalHttpServer();
    ~LocalHttpServer();

    LocalHttpServer(const LocalHttpServer&) = delete;
    LocalHttpServer& operator=(const LocalHttpServer&) = delete;

    bool Start(const ServerConfig& config, std::string* error);
    void Stop();
    bool IsRunning() const;
    int bound_port() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace saveinspector

#endif  // SAVEINSPECTOR_LOCAL_HTTP_H_
