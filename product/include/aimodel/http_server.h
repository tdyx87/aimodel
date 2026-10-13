// 本地 HTTP 服务：为产品 UI（web/）提供静态页面与 /api/system-info 数据接口。
// 隐私红线：仅绑定 127.0.0.1，数据不出本机（docs/design/http-ui.md 契约）。

#ifndef AIMODEL_HTTP_SERVER_H_
#define AIMODEL_HTTP_SERVER_H_

#include <string>

#include "aimodel/export.h"

namespace aimodel {

struct ServerConfig {
    std::string bind_host;    // 仅接受 "127.0.0.1"（Start 内强校验，防暴露到局域网）
    int port;                 // 0 = 系统分配；Start 成功后读 bound_port()
    std::string static_root;  // 静态资源目录（web/）；空 = 仅提供 API
    std::string uploads_root; // 文件上传授权目录（默认 data/uploads）；空 = 用库层默认值

    // 单次读取的等待上限（毫秒），按「距上次收到字节的间隔」计。
    // 0 = 使用内置默认 5000。调小可加快慢连接的回收与优雅停止。
    int request_timeout_ms = 0;
};

// 单线程接受循环的本地服务器（localhost 单用户场景足够）。
// RAII：析构即 Stop；不可拷贝。可重复 Start/Stop。
class AIMODEL_API LocalServer {
public:
    LocalServer();
    ~LocalServer();

    LocalServer(const LocalServer&) = delete;
    LocalServer& operator=(const LocalServer&) = delete;

    // 启动后台线程。失败返回 false 并填充 error（用户可呈现信息）。
    bool Start(const ServerConfig& config, std::string* error);

    // 优雅停止（等待工作线程退出），可再次 Start。
    void Stop();

    bool IsRunning() const;

    // Start 成功后的真实监听端口（config.port=0 时由系统分配）。
    int bound_port() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace aimodel

#endif  // AIMODEL_HTTP_SERVER_H_
