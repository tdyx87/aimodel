// 平台无关：请求解析、路由、静态资源服务、线程编排。
// 套接字操作经 HttpServerImpl（平台文件实现）；本文件三平台无差别编译。

#include "aimodel/http_server.h"

#include "aimodel/system_info.h"
#include "http_server_impl.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

namespace aimodel {
namespace {

const int kAcceptPollMs = 250;      // accept 轮询间隔（兼作停止响应上限）
const int kRequestTimeoutMs = 5000; // 读取请求行超时

// URL 解码 %XX；非法序列或 %00 返回 false。
bool UrlDecode(const std::string& in, std::string* out) {
    out->clear();
    out->reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') {
            out->push_back(in[i]);
            continue;
        }
        if (i + 2 >= in.size()) {
            return false;
        }
        char hex[3] = {in[i + 1], in[i + 2], '\0'};
        char* end = NULL;
        long value = std::strtol(hex, &end, 16);
        if (end == hex || value <= 0) {
            return false;
        }
        out->push_back(static_cast<char>(value));
        i += 2;
    }
    return true;
}

// 静态路径消毒：去 query、URL 解码、拒绝 `..`/反斜杠/NUL；根路径 → index.html。
bool SanitizeStaticPath(const std::string& raw, std::string* rel) {
    std::string path = raw;
    size_t q = path.find('?');
    if (q != std::string::npos) {
        path = path.substr(0, q);
    }
    if (!UrlDecode(path, &path)) {
        return false;
    }
    if (path.empty() || path[0] != '/' || path.size() > 1024) {
        return false;
    }
    if (path.find("..") != std::string::npos || path.find('\\') != std::string::npos ||
            path.find('\0') != std::string::npos) {
        return false;
    }
    if (path == "/") {
        path = "/index.html";
    }
    *rel = path.substr(1);
    return true;
}

// 扩展名白名单 → Content-Type；不在白名单返回 NULL。
const char* MimeForPath(const std::string& rel) {
    size_t dot = rel.rfind('.');
    if (dot == std::string::npos) {
        return NULL;
    }
    std::string ext = rel.substr(dot + 1);
    if (ext == "html") return "text/html; charset=utf-8";
    if (ext == "css") return "text/css; charset=utf-8";
    if (ext == "js") return "text/javascript; charset=utf-8";
    if (ext == "json") return "application/json; charset=utf-8";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "png") return "image/png";
    if (ext == "ico") return "image/x-icon";
    if (ext == "woff2") return "font/woff2";
    return NULL;
}

std::string BuildResponse(int status, const char* reason, const char* content_type,
                          const std::string& body) {
    std::ostringstream os;
    os << "HTTP/1.1 " << status << " " << reason << "\r\n"
       << "Content-Type: " << content_type << "\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Cache-Control: no-store\r\n"
       << "Connection: close\r\n\r\n"
       << body;
    return os.str();
}

}  // namespace


namespace {

// 用户语言错误页（不暴露技术细节，agents.md 设计原则）。
std::string ErrorPage(int status, const std::string& title, const std::string& message) {
    (void)status;  // 错误页对用户只呈现语义文案，不展示状态码等技术细节
    std::ostringstream os;
    os << "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
       << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
       << "<title>" << title << " - aimodel</title>"
       << "<style>body{font-family:system-ui,'PingFang SC','Microsoft YaHei';"
       << "background:#0E1116;color:#E6EDF3;display:flex;align-items:center;"
       << "justify-content:center;margin:0;min-height:100vh}"
       << "main{text-align:center;max-width:480px;padding:24px}"
       << "h1{font-size:20px}p{color:#8B949E;line-height:1.6}"
       << "a{color:#2F81F7}</style></head><body><main>"
       << "<h1>" << title << "</h1><p>" << message << "</p>"
       << "<p><a href=\"/\">返回首页</a></p></main></body></html>";
    return os.str();
}

// 解析请求首行：METHOD SP PATH SP VERSION。
bool ParseRequestFirstLine(const char* data, size_t len, std::string* method, std::string* path) {
    size_t line_end = std::string::npos;
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == '\n') {
            line_end = i;
            break;
        }
    }
    if (line_end == std::string::npos) {
        return false;
    }
    std::string line(data, line_end);
    if (!line.empty() && line[line.size() - 1] == '\r') {
        line.erase(line.size() - 1);
    }
    size_t sp1 = line.find(' ');
    size_t sp2 = line.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos || sp1 == 0) {
        return false;
    }
    *method = line.substr(0, sp1);
    *path = line.substr(sp1 + 1, sp2 - sp1 - 1);
    return !method->empty() && !path->empty();
}

bool ReadFileBytes(const std::string& path, std::string* out) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in.is_open()) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

// 路由：返回完整 HTTP 响应字节。
std::string HandleRequest(const std::string& method, const std::string& raw_path,
                          const ServerConfig& config) {
    if (method != "GET") {
        return BuildResponse(405, "Method Not Allowed", "text/plain; charset=utf-8",
                             "Method Not Allowed");
    }
    std::string path = raw_path;
    size_t q = path.find('?');
    if (q != std::string::npos) {
        path = path.substr(0, q);
    }
    if (path == "/api/system-info") {
        try {
            std::string body = ToJson(GetSystemInfo());
            return BuildResponse(200, "OK", "application/json; charset=utf-8", body);
        } catch (...) {
            return BuildResponse(500, "Internal Server Error",
                                 "application/json; charset=utf-8",
                                 "{\"error\":\"hardware report unavailable\"}");
        }
    }
    if (path.compare(0, 5, "/api/") == 0) {
        return BuildResponse(404, "Not Found", "application/json; charset=utf-8",
                             "{\"error\":\"not found\"}");
    }
    // 静态资源
    std::string rel;
    if (config.static_root.empty()) {
        return BuildResponse(404, "Not Found", "text/html; charset=utf-8",
                             ErrorPage(404, "页面不存在", "该服务仅提供硬件数据接口。"));
    }
    if (!SanitizeStaticPath(raw_path, &rel) || MimeForPath(rel) == NULL) {
        return BuildResponse(404, "Not Found", "text/html; charset=utf-8",
                             ErrorPage(404, "页面不存在", "未找到请求的页面。"));
    }
    const char* mime = MimeForPath(rel);
    std::string body;
    if (!ReadFileBytes(config.static_root + "/" + rel, &body)) {
        if (rel == "index.html") {
            return BuildResponse(
                404, "Not Found", "text/html; charset=utf-8",
                ErrorPage(404, "界面资源缺失",
                          "未找到界面资源，可能是安装不完整，请重新安装 aimodel。"
                          "（开发者提示：请先构建前端 cd web 然后 npm install 与 npm run build）"));
        }
        return BuildResponse(404, "Not Found", "text/html; charset=utf-8",
                             ErrorPage(404, "页面不存在", "未找到请求的页面。"));
    }
    return BuildResponse(200, "OK", mime, body);
}

}  // namespace


struct LocalServer::Impl {
    HttpServerImpl net;
    ServerConfig config;
    std::thread worker;
    std::atomic<bool> running;
    int bound_port;

    Impl() : running(false), bound_port(0) {}

    void Run();  // accept 循环（平台无关，仅调用 Net* 抽象）
};

void LocalServer::Impl::Run() {
    while (!net.stopping.load()) {
        net_socket_t client = NET_INVALID_SOCKET;
        int r = net.Accept(&client, kAcceptPollMs);
        if (r == 0) {
            continue;
        }
        if (r < 0) {
            if (net.stopping.load()) {
                break;
            }
            continue;  // 单次接受失败不停服
        }
        char buf[4096];
        int n = net.Recv(client, buf, sizeof(buf) - 1, kRequestTimeoutMs);
        if (n > 0) {
            buf[n] = '\0';
            std::string method, path;
            std::string response;
            if (ParseRequestFirstLine(buf, static_cast<size_t>(n), &method, &path)) {
                response = HandleRequest(method, path, config);
            } else {
                response = BuildResponse(400, "Bad Request", "text/plain; charset=utf-8",
                                         "Bad Request");
            }
            net.SendAll(client, response.data(), response.size());
        }
        net.CloseSocket(client);
    }
    running = false;
}

LocalServer::LocalServer() : impl_(new Impl) {}

LocalServer::~LocalServer() {
    Stop();
    delete impl_;
}

bool LocalServer::Start(const ServerConfig& config, std::string* error) {
    if (IsRunning()) {
        if (error != NULL) *error = "服务已在运行";
        return false;
    }
    // 隐私红线：仅允许本机回环地址
    if (!config.bind_host.empty() && config.bind_host != "127.0.0.1") {
        if (error != NULL) *error = "出于隐私保护，仅支持本机访问（127.0.0.1）";
        return false;
    }
    std::string local_error;
    if (!NetGlobalInit(&local_error)) {
        if (error != NULL) *error = local_error;
        return false;
    }
    impl_->config = config;
    if (impl_->config.bind_host.empty()) impl_->config.bind_host = "127.0.0.1";
    impl_->net.stopping = false;
    if (!impl_->net.Listen(impl_->config.bind_host, impl_->config.port, &local_error)) {
        NetGlobalShutdown();
        if (error != NULL) *error = local_error;
        return false;
    }
    impl_->bound_port = impl_->net.bound_port;
    impl_->running = true;
    impl_->worker = std::thread(&Impl::Run, impl_);
    return true;
}

void LocalServer::Stop() {
    if (!impl_->worker.joinable()) {
        impl_->running = false;
        return;
    }
    impl_->net.stopping = true;
    impl_->worker.join();
    impl_->net.CloseListener();
    NetGlobalShutdown();
    impl_->running = false;
}

bool LocalServer::IsRunning() const {
    return impl_->running.load();
}

int LocalServer::bound_port() const {
    return impl_->bound_port;
}

}  // namespace aimodel
