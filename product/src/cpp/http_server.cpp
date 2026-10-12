// 平台无关：请求解析、路由、静态资源服务、线程编排。
// 套接字操作经 HttpServerImpl（平台文件实现）；本文件三平台无差别编译。

#include "aimodel/chat_suggest.h"
#include "aimodel/file_transfer.h"
#include "aimodel/http_server.h"

#include "aimodel/system_info.h"
#include "http_server_impl.h"

#include <cctype>
#include <cstdlib>
#include <cstdio>
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
    // 解码结果须写入独立变量：UrlDecode 会先 clear 输出，
    // 与输入共用同一对象会把待解码内容清空（曾导致所有静态资源 404）。
    std::string decoded;
    if (!UrlDecode(path, &decoded)) {
        return false;
    }
    path = decoded;
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

// ASCII 小写化（仅用于请求头名比较，不动非 ASCII 字节）。
std::string ToLowerAscii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        out.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c));
    }
    return out;
}

// JSON 字符串转义（输出 "..." 内部内容）：保证文件名等外部输入进 JSON 安全。
std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (c < 0x20u) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

// 从 query string 取参数值（URL 解码）；参数不存在返回空串。
// 仅用于白名单内的参数名（如 file_name），不用于任意路径拼接。
std::string ExtractQueryParam(const std::string& raw_path,
                              const std::string& key) {
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

// 把一个 Unicode 码点编码为 UTF-8 追加到 out（用于解析 JSON 的 \uXXXX 转义）。
void AppendUtf8(unsigned int code_point, std::string* out) {
    if (code_point <= 0x7Fu) {
        out->push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FFu) {
        out->push_back(static_cast<char>(0xC0u | (code_point >> 6)));
        out->push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    } else if (code_point <= 0xFFFFu) {
        out->push_back(static_cast<char>(0xE0u | (code_point >> 12)));
        out->push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
        out->push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    } else {
        out->push_back(static_cast<char>(0xF0u | (code_point >> 18)));
        out->push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu)));
        out->push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
        out->push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    }
}

// 解析 4 位十六进制（\uXXXX 的 XXXX 部分）。
bool ParseHex4(const std::string& s, size_t pos, unsigned int* value) {
    if (pos + 4 > s.size()) return false;
    unsigned int v = 0;
    for (size_t i = 0; i < 4; ++i) {
        const char c = s[pos + i];
        unsigned int d = 0;
        if (c >= '0' && c <= '9') {
            d = static_cast<unsigned int>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = static_cast<unsigned int>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = static_cast<unsigned int>(c - 'A' + 10);
        } else {
            return false;
        }
        v = (v << 4) | d;
    }
    *value = v;
    return true;
}

// 从 JSON 请求体提取字符串字段值（仅覆盖契约内的简单请求体 {"query":"..."}）。
// 找不到字段、值不是字符串、或转义非法时返回空串（由上层按 INVALID_QUERY 处理）。
// 支持 \" \\ \/ \b \f \n \r \t 与 \uXXXX（含 UTF-16 代理对）。
std::string ExtractJsonStringField(const std::string& body,
                                   const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = body.find(needle);
    if (pos == std::string::npos) return std::string();
    pos += needle.size();
    while (pos < body.size() &&
           std::isspace(static_cast<unsigned char>(body[pos])) != 0) {
        ++pos;
    }
    if (pos >= body.size() || body[pos] != ':') return std::string();
    ++pos;
    while (pos < body.size() &&
           std::isspace(static_cast<unsigned char>(body[pos])) != 0) {
        ++pos;
    }
    if (pos >= body.size() || body[pos] != '"') return std::string();
    ++pos;

    std::string out;
    while (pos < body.size()) {
        const char c = body[pos];
        if (c == '"') break;  // 字符串结束
        if (c == '\\') {
            if (pos + 1 >= body.size()) return std::string();
            const char esc = body[pos + 1];
            pos += 2;
            switch (esc) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    unsigned int cp = 0;
                    if (!ParseHex4(body, pos, &cp)) return std::string();
                    pos += 4;
                    // UTF-16 代理对：高位 D800-DBFF 后须跟低位 DC00-DFFF。
                    if (cp >= 0xD800u && cp <= 0xDBFFu) {
                        if (pos + 2 > body.size() || body[pos] != '\\' ||
                            body[pos + 1] != 'u') {
                            return std::string();
                        }
                        pos += 2;
                        unsigned int low = 0;
                        if (!ParseHex4(body, pos, &low)) return std::string();
                        pos += 4;
                        if (low < 0xDC00u || low > 0xDFFFu) return std::string();
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                    }
                    AppendUtf8(cp, &out);
                    break;
                }
                default:
                    return std::string();  // 非法转义
            }
            continue;
        }
        out.push_back(c);
        ++pos;
    }
    return out;
}

// FileError → 契约第 5 节的错误码字符串。
const char* FileErrorCodeString(FileError code) {
    switch (code) {
        case FileError::kPathNotAllowed: return "PATH_NOT_ALLOWED";
        case FileError::kNotFound:       return "NOT_FOUND";
        case FileError::kTooLarge:       return "TOO_LARGE";
        case FileError::kWriteFailed:    return "WRITE_FAILED";
        case FileError::kNone:           return "NONE";
        case FileError::kInternal:
        default:                         return "INTERNAL_ERROR";
    }
}

// 文件传输失败响应：契约 4.1/4.2 的 {ok:false, code, message} 结构。
// message 直接用库层给出的用户语言文案，不暴露内部路径与技术细节。
std::string BuildFileErrorResponse(FileError code, const std::string& message) {
    std::ostringstream os;
    os << "{\"ok\":false,\"code\":\"" << FileErrorCodeString(code)
       << "\",\"message\":\"" << JsonEscape(message) << "\"}";
    return BuildResponse(200, "OK", "application/json; charset=utf-8", os.str());
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

// 解析完整请求：首行 + 请求头 + 请求体。
// 返回 false 表示请求行不合法（已读取到的部分不足以判定时由调用方继续收包）。
struct ParsedRequest {
    std::string method;
    std::string path;
    std::string body;
};

// 从已收到的字节中切出请求头区，并取出 Content-Length 与 body 起始偏移。
// headers_end 指向 body 第一个字节；complete 表示 body 是否已收全。
bool ParseRequestHeaders(const std::string& raw, size_t* headers_end,
                         size_t* content_length, bool* has_content_length) {
    // 请求头与 body 以 "\r\n\r\n" 分隔。
    const size_t sep = raw.find("\r\n\r\n");
    if (sep == std::string::npos) {
        return false;  // 头区尚未收全
    }
    *headers_end = sep + 4;

    *content_length = 0;
    *has_content_length = false;
    // 在头区内逐行找 Content-Length（大小写不敏感）。
    size_t pos = 0;
    while (pos < sep) {
        size_t eol = raw.find("\r\n", pos);
        if (eol == std::string::npos || eol > sep) {
            eol = sep;
        }
        const std::string line = raw.substr(pos, eol - pos);
        const std::string lower = ToLowerAscii(line);
        const std::string key = "content-length:";
        if (lower.compare(0, key.size(), key) == 0) {
            std::string value = line.substr(key.size());
            // 去掉前导空格。
            size_t v = 0;
            while (v < value.size() && value[v] == ' ') ++v;
            value = value.substr(v);
            if (!value.empty()) {
                char* end = NULL;
                const unsigned long long n = std::strtoull(value.c_str(), &end, 10);
                if (end != value.c_str() && *end == '\0') {
                    *content_length = static_cast<size_t>(n);
                    *has_content_length = true;
                }
            }
        }
        if (eol >= sep) break;
        pos = eol + 2;
    }
    return true;
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
                          const std::string& body,
                          const std::string& uploads_root,
                          const ServerConfig& config) {
    std::string path = raw_path;
    size_t q = path.find('?');
    if (q != std::string::npos) {
        path = path.substr(0, q);
    }

    // ---- 文件传输 API（契约 docs/design/file-transfer-contract.md）----
    if (path == "/api/file/upload" && method == "POST") {
        FileTransfer ft(uploads_root);
        std::string error;
        std::string saved;
        FileError code = FileError::kNone;
        if (!ft.AppendUpload(ExtractQueryParam(raw_path, "file_name"), body,
                             &error, &saved, &code)) {
            return BuildFileErrorResponse(code, error);
        }
        std::ostringstream os;
        os << "{\"ok\":true,\"file_name\":\"" << JsonEscape(saved)
           << "\",\"size\":" << body.size() << "}";
        return BuildResponse(200, "OK", "application/json; charset=utf-8", os.str());
    }
    if (path == "/api/file/download" && method == "GET") {
        FileTransfer ft(uploads_root);
        std::string error;
        std::string content;
        FileError code = FileError::kNone;
        if (!ft.ReadUpload(ExtractQueryParam(raw_path, "file_name"), &content,
                           &error, &code)) {
            return BuildFileErrorResponse(code, error);
        }
        std::ostringstream os;
        os << "Content-Type: application/octet-stream\r\n"
           << "Content-Disposition: attachment; filename=\""
           << JsonEscape(ExtractQueryParam(raw_path, "file_name")) << "\"\r\n"
           << "Content-Length: " << content.size() << "\r\n"
           << "Cache-Control: no-store\r\n"
           << "Connection: close\r\n\r\n"
           << content;
        return os.str();
    }
    if (path.compare(0, 10, "/api/file/") == 0) {
        return BuildResponse(405, "Method Not Allowed",
                             "application/json; charset=utf-8",
                             "{\"ok\":false,\"code\":\"METHOD_NOT_ALLOWED\","
                             "\"message\":\"不支持的请求方法。\"}");
    }
    // ---- 聊天/推荐 API（契约 docs/design/chat-system-contract.md）----
    // 本地规则式建议：GET 能力说明 + POST 建议；数据不出本机（隐私红线）。
    if (path == "/api/chat/capability" && method == "GET") {
        return BuildResponse(200, "OK", "application/json; charset=utf-8",
                             ChatCapabilityJson());
    }
    if (path == "/api/chat/suggest" && method == "POST") {
        const std::string query = ExtractJsonStringField(body, "query");
        try {
            const SystemInfo info = GetSystemInfo();
            const ChatAdvice advice = BuildChatAdvice(info, query);
            return BuildResponse(200, "OK", "application/json; charset=utf-8",
                                 ChatAdviceToJson(info, query, advice));
        } catch (...) {
            // 采集/序列化异常：按契约 §4 返回用户语言错误，不暴露技术细节。
            return BuildResponse(
                200, "OK", "application/json; charset=utf-8",
                "{\"ok\":false,\"error\":{\"code\":\"UNEXPECTED_ERROR\","
                "\"message\":\"请求失败，请稍后重试。\"}}");
        }
    }
    if (path.compare(0, 10, "/api/chat/") == 0) {
        return BuildResponse(405, "Method Not Allowed",
                             "application/json; charset=utf-8",
                             "{\"ok\":false,\"code\":\"METHOD_NOT_ALLOWED\","
                             "\"message\":\"不支持的请求方法。\"}");
    }
    // ---- 硬件报告 API（原有）----
    if (method != "GET") {
        return BuildResponse(405, "Method Not Allowed", "text/plain; charset=utf-8",
                             "Method Not Allowed");
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
    std::string body_out;
    if (!ReadFileBytes(config.static_root + "/" + rel, &body_out)) {
        if (rel == "index.html") {
            return BuildResponse(
                404, "Not Found", "text/html; charset=utf-8",
                ErrorPage(404, "界面资源缺失",
                          "未找到界面资源，可能是安装不完整，请重新安装 aimodel。"
                          "（开发者提示：界面资源目录 web 需与可执行文件同级）"));
        }
        return BuildResponse(404, "Not Found", "text/html; charset=utf-8",
                             ErrorPage(404, "页面不存在", "未找到请求的页面。"));
    }
    return BuildResponse(200, "OK", mime, body_out);
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
    // 请求体上限：与 FileTransfer 的 10 MB 一致，另留余量给请求头，
    // 避免超大 Content-Length 造成内存耗尽（DoS 防护）。
    const size_t kMaxRequestBytes = 11u * 1024u * 1024u;

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

        std::string raw;
        raw.reserve(4096);
        char buf[4096];
        bool headers_done = false;
        size_t body_start = 0;
        size_t content_length = 0;
        bool has_content_length = false;

        // 先收请求头；再按 Content-Length 收全请求体（支持上传大文件）。
        for (;;) {
            const int n = net.Recv(client, buf, sizeof(buf) - 1, kRequestTimeoutMs);
            if (n <= 0) {
                break;  // 超时或连接关闭
            }
            raw.append(buf, static_cast<size_t>(n));

            if (!headers_done) {
                if (!ParseRequestHeaders(raw, &body_start, &content_length,
                                         &has_content_length)) {
                    if (raw.size() > 64u * 1024u) {
                        // 头区异常膨胀，直接断开。
                        raw.clear();
                        break;
                    }
                    continue;  // 头区尚未收全
                }
                headers_done = true;
                if (!has_content_length) {
                    break;  // 无 body（GET 等），读完头即可
                }
                if (content_length > kMaxRequestBytes) {
                    // 声明体积超限：不接收，直接拒绝。
                    raw.clear();
                    break;
                }
            }
            if (raw.size() - body_start >= content_length) {
                break;  // body 已收全
            }
        }

        std::string response;
        if (!raw.empty()) {
            std::string method, path;
            // 首行解析：把请求行（含结尾 \n）整段传入，避免二次查找失败。
            size_t line_end = raw.find('\n');
            if (line_end == std::string::npos) {
                line_end = raw.size() - 1;
            }
            const std::string first_line = raw.substr(0, line_end + 1);
            if (ParseRequestFirstLine(first_line.data(), first_line.size(),
                                      &method, &path)) {
                std::string body;
                if (headers_done && body_start <= raw.size()) {
                    body = raw.substr(body_start);
                }
                response = HandleRequest(method, path, body,
                                         config.uploads_root, config);
            } else {
                response = BuildResponse(400, "Bad Request",
                                         "text/plain; charset=utf-8", "Bad Request");
            }
        }
        if (response.empty()) {
            response = BuildResponse(400, "Bad Request",
                                     "text/plain; charset=utf-8", "Bad Request");
        }
        net.SendAll(client, response.data(), response.size());
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
