// 角色：cpp-implementer
// 产物：src/cpp/local_http.cpp、include/saveinspector/local_http.h
// 契约：docs/design/save-format-contract.md §4（通信协议）、§5（隐私）
//
// 平台无关：请求解析、闸门、路由、静态资源、线程编排。
// 套接字操作经 HttpServerImpl（平台文件实现），本文件三平台无差别编译。
//
// 本文件承载 aimodel 实战中付出代价换来的四条教训（契约 §7），逐条标注：
//   1. 「读超时/对端关闭」与「请求已完整」必须区分，否则会把截断数据当完整请求处理；
//   2. 失败响应用对应状态码，不一律 200，否则前端无法区分「数据」与「错误」；
//   3. 每个请求过闸门（Host/Origin/Content-Type），防 DNS rebinding 与跨站写入；
//   4. 诊断与响应体不含本机路径。

#include "saveinspector/local_http.h"

#include "local_http_impl.h"
#include "saveinspector/save_inspector.h"

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace saveinspector {
namespace {

const int kAcceptPollMs = 250;       // accept 轮询间隔（兼作停止响应上限）
const int kRequestTimeoutMs = 5000;  // 读取请求的默认等待上限
const int kIoPollMs = 250;           // 单次 Recv 的等待窗口（见 ServeConnection）
const size_t kWorkerCount = 4;
const size_t kMaxQueued = 8;
const size_t kMaxRequestBytes = 512ull * 1024ull * 1024ull;
const size_t kMaxHeaderBytes = 64u * 1024u;

// ---- 通用小工具 ----

std::string ToLowerAscii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        out.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c));
    }
    return out;
}

std::string TrimAscii(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

// JSON 字符串转义（输出 "..." 内部内容）。
std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20u) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

std::string BuildResponse(int status, const char* reason, const char* content_type,
                          const std::string& body) {
    std::ostringstream os;
    os << "HTTP/1.1 " << status << " " << reason << "\r\n"
       << "Content-Type: " << content_type << "\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Cache-Control: no-store\r\n"
       << "X-Content-Type-Options: nosniff\r\n"
       << "Connection: close\r\n\r\n"
       << body;
    return os.str();
}

const char* ReasonPhrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 500: return "Internal Server Error";
        default:  return "Error";
    }
}

// ---- 请求解析 ----

struct RequestHeaders {
    bool has_content_length = false;
    size_t content_length = 0;
    std::string host;
    std::string origin;
    std::string content_type;
};

bool ParseRequestFirstLine(const char* data, size_t len,
                           std::string* method, std::string* path) {
    size_t line_end = std::string::npos;
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == '\n') {
            line_end = i;
            break;
        }
    }
    if (line_end == std::string::npos) return false;
    std::string line(data, line_end);
    if (!line.empty() && line[line.size() - 1] == '\r') {
        line.erase(line.size() - 1);
    }
    const size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos || sp1 == 0) return false;
    const size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;
    *method = line.substr(0, sp1);
    *path = line.substr(sp1 + 1, sp2 - sp1 - 1);
    return !method->empty() && !path->empty();
}

bool ParseRequestHeaders(const std::string& raw, size_t* headers_end,
                         RequestHeaders* out) {
    const size_t sep = raw.find("\r\n\r\n");
    if (sep == std::string::npos) return false;  // 头区尚未收全
    *headers_end = sep + 4;

    out->has_content_length = false;
    out->content_length = 0;
    size_t pos = raw.find("\r\n");
    if (pos == std::string::npos || pos >= sep) return true;
    pos += 2;
    while (pos < sep) {
        size_t eol = raw.find("\r\n", pos);
        if (eol == std::string::npos || eol > sep) eol = sep;
        const std::string line = raw.substr(pos, eol - pos);
        const std::string lower = ToLowerAscii(line);
        const size_t colon = lower.find(':');
        if (colon != std::string::npos) {
            const std::string name = lower.substr(0, colon);
            std::string value = TrimAscii(line.substr(colon + 1));
            if (name == "content-length" && !value.empty()) {
                char* end = NULL;
                const unsigned long long n = std::strtoull(value.c_str(), &end, 10);
                if (end != value.c_str() && *end == '\0') {
                    out->content_length = static_cast<size_t>(n);
                    out->has_content_length = true;
                }
            } else if (name == "host") {
                out->host = value;
            } else if (name == "origin") {
                out->origin = value;
            } else if (name == "content-type") {
                out->content_type = value;
            }
        }
        if (eol >= sep) break;
        pos = eol + 2;
    }
    return true;
}

// URL 解码 %XX；非法序列或 %00 返回 false。
bool UrlDecode(const std::string& in, std::string* out) {
    out->clear();
    out->reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') {
            out->push_back(in[i]);
            continue;
        }
        if (i + 2 >= in.size()) return false;
        char hex[3] = {in[i + 1], in[i + 2], '\0'};
        char* end = NULL;
        const long value = std::strtol(hex, &end, 16);
        if (end == hex || value <= 0) return false;
        out->push_back(static_cast<char>(value));
        i += 2;
    }
    return true;
}

std::string ExtractQueryParam(const std::string& raw_path, const std::string& key) {
    const size_t q = raw_path.find('?');
    if (q == std::string::npos) return std::string();
    const std::string query = raw_path.substr(q + 1);
    const std::string prefix = key + "=";
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        const std::string pair = query.substr(pos, amp - pos);
        if (pair.compare(0, prefix.size(), prefix) == 0) {
            std::string decoded;
            if (UrlDecode(pair.substr(prefix.size()), &decoded)) {
                return decoded;
            }
            return std::string();
        }
        pos = amp + 1;
    }
    return std::string();
}

// ---- 静态路径消毒 ----

bool SanitizeStaticPath(const std::string& raw, std::string* rel) {
    std::string path = raw;
    const size_t q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);
    std::string decoded;
    if (!UrlDecode(path, &decoded)) return false;
    path = decoded;
    if (path.empty() || path[0] != '/' || path.size() > 1024) return false;
    if (path.find("..") != std::string::npos || path.find('\\') != std::string::npos) {
        return false;
    }
    if (path == "/") path = "/index.html";
    *rel = path.substr(1);
    return true;
}

const char* MimeForPath(const std::string& rel) {
    const size_t dot = rel.rfind('.');
    if (dot == std::string::npos) return NULL;
    const std::string ext = rel.substr(dot + 1);
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

bool ReadFileBytes(const std::string& path, std::string* out) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

// ---- 请求闸门（契约 §4.6）----
//
// 仅绑定回环**不足以**防跨站访问：任何网页都能向 127.0.0.1 发请求，而 DNS
// rebinding 能让对方的同源校验指向本机。三层校验，任一不过一律 403 且不
// 说明原因（避免给探测方反馈）：
//   1. Host 必须是回环名/IP 且端口匹配 —— 挡 rebinding
//   2. Origin 若存在必须同源 —— 挡跨站 fetch/form 写入
//   3. 写接口的 Content-Type 必须匹配 —— 跨站「简单请求」发不出这些类型，
//      必须先过 CORS 预检，而本服务不响应预检、不返回任何 Access-Control-* 头
bool IsAllowedHostHeader(const std::string& host, int bound_port) {
    if (host.empty()) return true;  // HTTP/1.0 允许无 Host
    const std::string h = ToLowerAscii(TrimAscii(host));
    const std::string port = (bound_port > 0) ? std::to_string(bound_port)
                                               : std::string();
    static const char* kNames[] = {"127.0.0.1", "localhost", "[::1]"};
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
        if (h == kNames[i]) return true;
        if (!port.empty() && h == std::string(kNames[i]) + ":" + port) return true;
    }
    return false;
}

bool IsAllowedOriginHeader(const std::string& origin, int bound_port) {
    if (origin.empty()) return true;  // 非浏览器 / 同源导航不带 Origin
    const std::string o = ToLowerAscii(TrimAscii(origin));
    if (bound_port <= 0) return false;
    const std::string port = std::to_string(bound_port);
    return o == "http://127.0.0.1:" + port || o == "http://localhost:" + port;
}

std::string MediaTypeOf(const std::string& content_type) {
    const size_t semi = content_type.find(';');
    return ToLowerAscii(content_type.substr(0, semi));
}

}  // namespace

// ---------------------------------------------------------------------------
// LocalHttpServer
// ---------------------------------------------------------------------------

struct LocalHttpServer::Impl {
    HttpServerImpl net;
    ServerConfig config;
    std::thread acceptor;
    std::vector<std::thread> pool;
    std::deque<net_socket_t> queue;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> running;
    int bound_port;

    // 已加载的存档：全部工作线程共享。
    //
    // **必须加锁**（security-reviewer MED-1 / CWE-362）：/api/save/open 对它做写，
    // /api/save/slot 与 /api/save/export 对它做读。浏览器对回环默认开 6 条并发
    // 连接，「用户快速连续打开两个存档」就会让两个 open 真正并发——此时不加锁
    // 可能读到赋值过程中的 std::string/vector 中间状态（未定义行为，最坏崩溃）。
    //
    // 锁的范围是单个请求的全部处理，耗时为毫秒级解析，串行化不影响体验。
    // 注意本锁与 mutex（连接队列用）**刻意分开**：队列锁不得跨到业务处理，
    // 否则一个慢请求会堵住 accept 循环。
    mutable std::mutex inspector_mutex;
    SaveInspector inspector;

    Impl() : running(false), bound_port(0) {}

    void AcceptLoop();
    void WorkerLoop();
    void ServeConnection(net_socket_t client);
    void Enqueue(net_socket_t client);
    void DrainQueue();
};

namespace {

// 路由：返回完整 HTTP 响应字节。
std::string HandleRoute(const std::string& method, const std::string& raw_path,
                        const std::string& body, const RequestHeaders& headers,
                        SaveInspector* inspector, const ServerConfig& config) {
    std::string path = raw_path;
    const size_t q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);

    // ---- /api/save/open ----
    if (path == "/api/save/open" && method == "POST") {
        // 二进制上传：契约 §4.6 要求 octet-stream（挡 HTML form 跨站 CSRF）。
        if (MediaTypeOf(headers.content_type) != "application/octet-stream") {
            return BuildResponse(
                415, "Unsupported Media Type", "application/json; charset=utf-8",
                "{\"ok\":false,\"message\":\"请以文件方式选择存档。\"}");
        }
        // 契约 §5 的隐私约束：不接收本机路径。保存目录由用户在界面上选定，
        // 文件由浏览器读出字节后以 body 上传，服务端只见到字节、见不到路径。
        SaveInspector temp;
        SaveFile save;
        std::string error;
        if (!temp.OpenBytes(body, &save, &error)) {
            // 打开失败必须**清空**本连接已加载的存档：否则界面上已切到诊断视图，
            // 而 /api/save/slots 仍会返回上一个存档的槽位，服务端状态与界面
            // 所见不一致（用户选错文件后会看到不相干的旧数据）。
            inspector->Clear();
            // 「存档损坏/无法解析」是**可预期的业务结果**，不是协议错误 →
            // 200 + {ok:false,diagnostics}；传输层错误才用 4xx/5xx。
            std::ostringstream os;
            os << "{\"ok\":false,\"diagnostics\":[{"
               << "\"severity\":\"error\","
               << "\"title\":\"" << JsonEscape(error) << "\","
               << "\"detail\":\"这个文件无法作为存档读取。\","
               << "\"advice\":\"请确认选择的是游戏的存档文件，而不是存档的备份或截图。\""
               << "}]}";
            return BuildResponse(200, "OK", "application/json; charset=utf-8", os.str());
        }
        // 解析成功：把结果交给本连接的 inspector 供后续 /slot 请求使用。
        *inspector = temp;
        return BuildResponse(200, "OK", "application/json; charset=utf-8",
                             inspector->SaveToJson(inspector->save()));
    }
    // ---- /api/save/slots ----
    if (path == "/api/save/slots" && method == "GET") {
        if (!inspector->has_data()) {
            return BuildResponse(409, "Conflict", "application/json; charset=utf-8",
                                 "{\"ok\":false,\"message\":\"请先打开一个存档文件。\"}");
        }
        std::ostringstream os;
        os << "{\"ok\":true,\"slots\":[";
        const std::vector<SlotSummary> slots = inspector->ListSlots();
        for (size_t i = 0; i < slots.size(); ++i) {
            if (i != 0) os << ",";
            os << "{\"index\":" << slots[i].index
               << ",\"level\":" << slots[i].level
               << ",\"coins\":" << slots[i].coins
               << ",\"playtime\":\"" << JsonEscape(FormatDuration(slots[i].playtime_sec))
               << "\",\"last_played\":\"" << JsonEscape(FormatTimestamp(slots[i].last_played_utc))
               << "\",\"healthy\":" << (slots[i].healthy ? "true" : "false")
               << "}";
        }
        os << "]}";
        return BuildResponse(200, "OK", "application/json; charset=utf-8", os.str());
    }

    // ---- /api/save/slot ----
    if (path == "/api/save/slot" && method == "GET") {
        if (!inspector->has_data()) {
            return BuildResponse(409, "Conflict", "application/json; charset=utf-8",
                                 "{\"ok\":false,\"message\":\"请先打开一个存档文件。\"}");
        }
        const std::string index_text = ExtractQueryParam(raw_path, "index");
        const uint32_t index =
            static_cast<uint32_t>(std::atoi(index_text.c_str()));
        SlotDetail slot;
        if (index_text.empty() || !inspector->ReadSlot(index, &slot)) {
            return BuildResponse(404, "Not Found", "application/json; charset=utf-8",
                                 "{\"ok\":false,\"message\":\"找不到这个槽位。\"}");
        }
        // 契约 §4：{"ok":true,"slot":{...}}。槽位对象必须**嵌在** "slot" 下——
        // 直接拼会得到 {"ok":true,{"slot":1,...}}，那是非法 JSON，
        // 前端 response.json() 抛异常，槽位详情页完全打不开
        // （该缺陷由 E2E 抓出：库层用例与 /api/save/slots 用例都覆盖不到它）。
        return BuildResponse(200, "OK", "application/json; charset=utf-8",
                             "{\"ok\":true,\"slot\":" + inspector->SlotToJson(slot) + "}");
    }

    // ---- /api/save/export ----
    if (path == "/api/save/export" && method == "GET") {
        if (!inspector->has_data()) {
            return BuildResponse(409, "Conflict", "application/json; charset=utf-8",
                                 "{\"ok\":false,\"message\":\"请先打开一个存档文件。\"}");
        }
        const std::string index_text = ExtractQueryParam(raw_path, "index");
        const uint32_t index =
            static_cast<uint32_t>(std::atoi(index_text.c_str()));
        const std::string format = ExtractQueryParam(raw_path, "format");
        SlotDetail slot;
        if (index_text.empty() || !inspector->ReadSlot(index, &slot)) {
            return BuildResponse(404, "Not Found", "application/json; charset=utf-8",
                                 "{\"ok\":false,\"message\":\"找不到这个槽位。\"}");
        }

        std::string content;
        const char* content_type;
        const char* ext;
        if (format == "text") {
            content = inspector->SlotToTextReport(slot);
            content_type = "text/plain; charset=utf-8";
            ext = "txt";
        } else {
            content = inspector->SlotToJson(slot);
            content_type = "application/json; charset=utf-8";
            ext = "json";
        }
        // 文件名只用槽位号，不含用户本机路径（契约 §5）。
        std::ostringstream name;
        name << "save-slot-" << (index + 1) << "." << ext;

        std::ostringstream os;
        os << "HTTP/1.1 200 OK\r\n"
           << "Content-Type: " << content_type << "\r\n"
           << "Content-Disposition: attachment; filename=\"" << name.str() << "\"\r\n"
           << "Content-Length: " << content.size() << "\r\n"
           << "Cache-Control: no-store\r\n"
           << "X-Content-Type-Options: nosniff\r\n"
           << "Connection: close\r\n\r\n"
           << content;
        return os.str();
    }

    if (path.compare(0, 5, "/api/") == 0) {
        if (method != "GET") {
            return BuildResponse(405, "Method Not Allowed",
                                 "application/json; charset=utf-8",
                                 "{\"ok\":false,\"message\":\"不支持的请求方法。\"}");
        }
        return BuildResponse(404, "Not Found", "application/json; charset=utf-8",
                             "{\"ok\":false,\"message\":\"找不到这个接口。\"}");
    }

    // ---- 静态资源 ----
    if (method != "GET") {
        return BuildResponse(405, "Method Not Allowed", "text/plain; charset=utf-8",
                             "Method Not Allowed");
    }
    if (config.static_root.empty()) {
        return BuildResponse(404, "Not Found", "text/html; charset=utf-8",
                             "<!doctype html><meta charset=\"utf-8\">"
                             "<p>界面资源缺失。请在 web-root 参数中指定 web 目录。</p>");
    }
    std::string rel;
    if (!SanitizeStaticPath(raw_path, &rel) || MimeForPath(rel) == NULL) {
        return BuildResponse(404, "Not Found", "text/html; charset=utf-8",
                             "<!doctype html><meta charset=\"utf-8\">"
                             "<p>未找到请求的页面。</p>");
    }
    // 变量不叫 body：与 HandleRoute 的 body 形参同名会触发遮蔽（C4457 家族），
    // 且后续改动极易误用成请求体。
    std::string content;
    if (!ReadFileBytes(config.static_root + "/" + rel, &content)) {
        return BuildResponse(404, "Not Found", "text/html; charset=utf-8",
                             "<!doctype html><meta charset=\"utf-8\">"
                             "<p>未找到请求的页面。</p>");
    }
    return BuildResponse(200, "OK", MimeForPath(rel), content);
}

}  // namespace

void LocalHttpServer::Impl::AcceptLoop() {
    while (!net.stopping.load()) {
        net_socket_t client = NET_INVALID_SOCKET;
        const int r = net.Accept(&client, kAcceptPollMs);
        if (r == 0) continue;
        if (r < 0) {
            if (net.stopping.load()) break;
            continue;
        }
        Enqueue(client);
    }
    running = false;
}

void LocalHttpServer::Impl::Enqueue(net_socket_t client) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        // 队列满 = 背压：直接关掉新连接，避免无限累积。
        if (net.stopping.load() || queue.size() >= kMaxQueued) {
            net.CloseSocket(client);
            return;
        }
        queue.push_back(client);
    }
    cv.notify_one();
}

void LocalHttpServer::Impl::WorkerLoop() {
    for (;;) {
        net_socket_t client = NET_INVALID_SOCKET;
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [this] { return net.stopping.load() || !queue.empty(); });
            if (net.stopping.load()) break;
            if (queue.empty()) continue;
            client = queue.front();
            queue.pop_front();
        }
        ServeConnection(client);
    }
}

void LocalHttpServer::Impl::DrainQueue() {
    std::deque<net_socket_t> pending;
    {
        std::lock_guard<std::mutex> lock(mutex);
        pending.swap(queue);
    }
    for (size_t i = 0; i < pending.size(); ++i) {
        net.CloseSocket(pending[i]);
    }
}

void LocalHttpServer::Impl::ServeConnection(net_socket_t client) {
    const int io_timeout = (config.request_timeout_ms > 0)
                               ? config.request_timeout_ms
                               : kRequestTimeoutMs;
    // 单次 Recv 的等待窗口收敛到轮询周期：读超时按「距上次收到字节的间隔」
    // 累计，既限制慢连接占用，也让 Stop() 能在约一个周期内收敛。
    const int poll_ms = (io_timeout < kIoPollMs) ? io_timeout : kIoPollMs;

    std::string raw;
    raw.reserve(4096);
    char buf[4096];
    bool headers_done = false;
    bool too_large = false;
    // 教训 1：必须区分「收满」与「超时/断连」，否则会把截断 body 当完整请求
    // 处理——aimodel 曾因此把声明 1000 字节只发 100 字节的上传落盘并回 200 OK。
    bool body_complete = false;
    size_t body_start = 0;
    size_t content_length = 0;
    RequestHeaders headers;
    std::chrono::steady_clock::time_point last_progress =
        std::chrono::steady_clock::now();

    for (;;) {
        if (net.stopping.load()) break;
        const int n = net.Recv(client, buf, sizeof(buf) - 1, poll_ms);
        if (n > 0) {
            raw.append(buf, static_cast<size_t>(n));
            last_progress = std::chrono::steady_clock::now();
        } else if (n == 0) {
            break;  // 对端关闭
        } else {
            const long long idle_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - last_progress).count();
            if (idle_ms >= io_timeout) break;
            continue;
        }

        if (!headers_done) {
            if (!ParseRequestHeaders(raw, &body_start, &headers)) {
                if (raw.size() > kMaxHeaderBytes) {
                    raw.clear();
                    break;
                }
                continue;
            }
            headers_done = true;
            if (!headers.has_content_length) {
                body_complete = true;
                break;
            }
            content_length = headers.content_length;
            if (content_length > kMaxRequestBytes) {
                too_large = true;
                break;
            }
        }
        if (raw.size() - body_start >= content_length) {
            body_complete = true;
            break;
        }
    }

    if (too_large) {
        size_t drained = (raw.size() > body_start) ? (raw.size() - body_start) : 0;
        char sink[8192];
        while (drained < content_length && !net.stopping.load()) {
            const int n = net.Recv(client, sink, sizeof(sink), 1000);
            if (n <= 0) break;
            drained += static_cast<size_t>(n);
        }
    }

    std::string response;
    if (too_large) {
        response = BuildResponse(413, "Payload Too Large",
                                 "application/json; charset=utf-8",
                                 "{\"ok\":false,\"message\":\"这个存档文件太大，无法打开。\"}");
    } else if (!raw.empty()) {
        std::string method;
        std::string path;
        size_t line_end = raw.find('\n');
        if (line_end == std::string::npos) line_end = raw.size() - 1;
        const std::string first_line = raw.substr(0, line_end + 1);

        if (!ParseRequestFirstLine(first_line.data(), first_line.size(),
                                   &method, &path)) {
            response = BuildResponse(400, "Bad Request",
                                     "text/plain; charset=utf-8", "Bad Request");
        } else if (!IsAllowedHostHeader(headers.host, bound_port)) {
            // 教训 3：挡 DNS rebinding
            response = BuildResponse(403, "Forbidden", "text/plain; charset=utf-8",
                                     "Forbidden");
        } else if (!IsAllowedOriginHeader(headers.origin, bound_port)) {
            // 教训 3：挡跨站写入
            response = BuildResponse(403, "Forbidden", "text/plain; charset=utf-8",
                                     "Forbidden");
        } else if (headers_done && headers.has_content_length && !body_complete) {
            // 教训 1：请求体未收全绝不派发
            response = BuildResponse(400, "Bad Request",
                                     "text/plain; charset=utf-8", "Bad Request");
        } else {
            std::string body;
            if (headers_done && body_start <= raw.size()) {
                // 按 Content-Length 精确截取：一次 read 中夹带的超量字节
                // 会被误当成存档正文。
                const size_t avail = raw.size() - body_start;
                body = raw.substr(body_start,
                                  (avail < content_length) ? avail : content_length);
            }
            // MED-1：inspector 被所有工作线程共享，路由全程持锁。
            // 锁的范围不含上面的收包与下面的发送——网络 I/O 不占锁。
            {
                std::lock_guard<std::mutex> guard(inspector_mutex);
                response = HandleRoute(method, path, body, headers, &inspector, config);
            }
        }
    }
    if (response.empty()) {
        response = BuildResponse(400, "Bad Request",
                                 "text/plain; charset=utf-8", "Bad Request");
    }
    net.SendAll(client, response.data(), response.size());
    net.CloseSocket(client);
}
LocalHttpServer::LocalHttpServer() : impl_(new Impl) {}

LocalHttpServer::~LocalHttpServer() {
    Stop();
    delete impl_;
}

bool LocalHttpServer::Start(const ServerConfig& config, std::string* error) {
    if (IsRunning()) {
        if (error != NULL) *error = "服务已在运行。";
        return false;
    }
    // 隐私红线（PRD NFR-3）：只允许本机回环
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
    impl_->acceptor = std::thread(&Impl::AcceptLoop, impl_);
    for (size_t i = 0; i < kWorkerCount; ++i) {
        impl_->pool.push_back(std::thread(&Impl::WorkerLoop, impl_));
    }
    return true;
}

void LocalHttpServer::Stop() {
    if (!impl_->acceptor.joinable()) {
        impl_->running = false;
        return;
    }
    impl_->net.stopping = true;
    impl_->cv.notify_all();
    impl_->acceptor.join();
    // 必须 join 完所有工作线程再析构 Impl：线程持有 this，
    // joinable 的 std::thread 未 join 就析构会直接 std::terminate。
    for (size_t i = 0; i < impl_->pool.size(); ++i) {
        impl_->pool[i].join();
    }
    impl_->pool.clear();
    impl_->DrainQueue();
    impl_->net.CloseListener();
    NetGlobalShutdown();
    impl_->running = false;
}

bool LocalHttpServer::IsRunning() const {
    return impl_->running.load();
}

int LocalHttpServer::bound_port() const {
    return impl_->bound_port;
}

}  // namespace saveinspector
