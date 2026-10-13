// 平台无关：请求解析、路由、静态资源服务、线程编排。
// 套接字操作经 HttpServerImpl（平台文件实现）；本文件三平台无差别编译。

#include "aimodel/chat_suggest.h"
#include "aimodel/file_transfer.h"
#include "aimodel/http_server.h"

#include "aimodel/system_info.h"
#include "http_server_impl.h"

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace aimodel {
namespace {

const int kAcceptPollMs = 250;       // accept 轮询间隔（兼作停止响应上限）
const int kRequestTimeoutMs = 5000;  // 读取请求的默认等待上限
const int kIoPollMs = 250;           // 单次 Recv 的等待窗口（见 ServeConnection）
const size_t kWorkerCount = 4;       // 并发处理连接的工作线程数
const size_t kMaxQueued = 8;         // 待处理连接排队上限，超出直接拒绝（背压）

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

// RFC 5987 的 filename* 值编码：保留 unreserved 字符，其余按 UTF-8 字节百分号编码。
std::string Rfc5987Encode(const std::string& s) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const bool unreserved =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0Fu]);
            out.push_back(kHex[c & 0x0Fu]);
        }
    }
    return out;
}

// 传统 filename= 的 ASCII 回退值：非 ASCII 与引号/分隔符替换为 '_'（仅供老客户端）。
std::string AsciiFilenameFallback(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20u || c >= 0x7Fu || c == '"' || c == '\\' || c == ';') {
            out.push_back('_');
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    if (out.empty()) out = "download";
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
        case FileError::kInvalidName:    return "INVALID_FILENAME";
        case FileError::kPathNotAllowed: return "PATH_NOT_ALLOWED";
        case FileError::kNotFound:       return "NOT_FOUND";
        case FileError::kTooLarge:       return "TOO_LARGE";
        case FileError::kWriteFailed:    return "WRITE_FAILED";
        case FileError::kNone:           return "NONE";
        case FileError::kInternal:
        default:                         return "INTERNAL_ERROR";
    }
}

// FileError → HTTP 状态码（契约 §4.3）。
// 此前一律回 200：语义错误，且任何按状态码判别的中间层/客户端都会失效
// （例如前端下载失败时会把这段 JSON 当正文导航过去）。
int FileErrorStatus(FileError code) {
    switch (code) {
        case FileError::kNone:           return 200;
        case FileError::kInvalidName:    return 400;
        case FileError::kPathNotAllowed: return 400;
        case FileError::kNotFound:       return 404;
        case FileError::kTooLarge:       return 413;
        case FileError::kWriteFailed:    return 500;
        case FileError::kInternal:
        default:                         return 500;
    }
}

const char* ReasonPhrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default:  return "Error";
    }
}

// 契约 §4.1 的 saved_path：只给「授权目录名/落盘名」的相对形式。
//
// 为什么不给完整 uploads_root：那是服务端绝对路径，属安全审查明令不得外泄
// （security-reviewer-file-transfer.md §路径处理）。绝对根目录时退化为末段目录名，
// 相对根目录（如默认的 data/uploads）则原样给出，便于定位。
std::string RelativeSavedPath(const std::string& uploads_root,
                              const std::string& name) {
    std::string root = uploads_root;
    while (!root.empty() && (root[root.size() - 1] == '/' ||
                             root[root.size() - 1] == '\\')) {
        root.erase(root.size() - 1);
    }
    const bool absolute = !root.empty() &&
                          (root[0] == '/' || root[0] == '\\' ||
                           (root.size() >= 2 && root[1] == ':'));
    std::string dir;
    if (absolute) {
        const size_t sep = root.find_last_of("/\\");
        dir = (sep == std::string::npos) ? std::string() : root.substr(sep + 1);
    } else {
        dir = root;
    }
    if (dir.empty()) {
        dir = "uploads";
    }
    return dir + "/" + name;
}

// 文件传输失败响应：契约 4.1/4.2 的 {ok:false, code, message} 结构，状态码随错误码。
// message 直接用库层给出的用户语言文案，不暴露内部路径与技术细节。
std::string BuildFileErrorResponse(FileError code, const std::string& message) {
    std::ostringstream os;
    os << "{\"ok\":false,\"code\":\"" << FileErrorCodeString(code)
       << "\",\"message\":\"" << JsonEscape(message) << "\"}";
    const int status = FileErrorStatus(code);
    return BuildResponse(status, ReasonPhrase(status),
                         "application/json; charset=utf-8", os.str());
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

// 解析请求头区中本服务关心的字段。字段名大小写不敏感。
struct RequestHeaders {
    bool has_content_length = false;
    size_t content_length = 0;
    std::string host;
    std::string origin;
    std::string content_type;
};

// 从已收到的字节中切出请求头区并取出关心的字段。
// headers_end 指向 body 第一个字节；返回 false 表示头区尚未收全。
bool ParseRequestHeaders(const std::string& raw, size_t* headers_end,
                         RequestHeaders* out) {
    // 请求头与 body 以 "\r\n\r\n" 分隔。
    const size_t sep = raw.find("\r\n\r\n");
    if (sep == std::string::npos) {
        return false;  // 头区尚未收全
    }
    *headers_end = sep + 4;

    out->has_content_length = false;
    out->content_length = 0;
    // 逐行匹配：跳过首行（请求行），命中则按名字取值。
    size_t pos = raw.find("\r\n");
    if (pos == std::string::npos || pos >= sep) {
        return true;
    }
    pos += 2;
    while (pos < sep) {
        size_t eol = raw.find("\r\n", pos);
        if (eol == std::string::npos || eol > sep) {
            eol = sep;
        }
        const std::string line = raw.substr(pos, eol - pos);
        const std::string lower = ToLowerAscii(line);
        const size_t colon = lower.find(':');
        if (colon != std::string::npos) {
            const std::string name = lower.substr(0, colon);
            std::string value = line.substr(colon + 1);
            size_t v = 0;
            while (v < value.size() && value[v] == ' ') ++v;  // 去前导空格
            value = value.substr(v);
            while (!value.empty() && value[value.size() - 1] == ' ') {
                value.erase(value.size() - 1);  // 去尾随空格
            }
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

// Content-Type 的媒体类型部分：小写、去掉 "; charset=..." 之后的参数。
std::string MediaTypeOf(const std::string& content_type) {
    const size_t semi = content_type.find(';');
    return ToLowerAscii(content_type.substr(0, semi));
}

// 去掉首尾 ASCII 空白（头字段值不做 unicode 折叠）。
std::string TrimAscii(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

// ---- 请求闸门：Host / Origin / Content-Type ----
//
// 仅绑定回环地址**不足以**防跨站攻击：任何网页都能向 127.0.0.1:port 发请求，
// 而 DNS rebinding 还能让对方的同源校验指向本机。这里按三层拒绝：
//
//   1) Host 必须是回环名/回环 IP —— 挡住 rebinding
//      （浏览器会把 evil.com 解析到 127.0.0.1 后再带 Host: evil.com 发来）；
//   2) Origin 若存在必须是本机 —— 挡住跨站 fetch / form 的写操作；
//   3) 写接口要求非 CORS「安全方法」的内容类型 —— 跨站简单请求
//      只可能发出 text/plain / x-www-form-urlencoded / multipart/form-data，
//      要发出 application/octet-stream / application/json 就必须先过预检，
//      而本服务不响应预检，于是请求根本到不了这里。
//
// 三层任一不通过都返回 403，且不透露失败的具体原因（避免给探测方反馈）。
bool IsAllowedHostHeader(const std::string& host, int bound_port) {
    if (host.empty()) {
        return true;  // HTTP/1.0 允许无 Host
    }
    const std::string h = ToLowerAscii(TrimAscii(host));
    // 端口必须与实际监听端口一致：反代/端口扫描场景下端口不符同样可疑。
    const std::string port = (bound_port > 0) ? std::to_string(bound_port) : std::string();
    static const char* kNames[] = {"127.0.0.1", "localhost", "[::1]"};
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
        if (h == kNames[i]) {
            return true;
        }
        if (!port.empty() && h == std::string(kNames[i]) + ":" + port) {
            return true;
        }
    }
    return false;
}

bool IsAllowedOriginHeader(const std::string& origin, int bound_port) {
    if (origin.empty()) {
        return true;  // 非浏览器客户端 / 同源导航不会带 Origin
    }
    const std::string o = ToLowerAscii(TrimAscii(origin));
    if (bound_port <= 0) {
        return false;
    }
    const std::string port = std::to_string(bound_port);
    // 只认 http scheme 的回环来源；"null"（sandbox iframe / data:）一律拒绝。
    return o == "http://127.0.0.1:" + port || o == "http://localhost:" + port;
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
// headers 传入已解析的头字段（写接口据此校验 Content-Type）。
std::string HandleRequest(const std::string& method, const std::string& raw_path,
                          const std::string& body,
                          const std::string& uploads_root,
                          const ServerConfig& config,
                          const RequestHeaders& headers) {
    std::string path = raw_path;
    size_t q = path.find('?');
    if (q != std::string::npos) {
        path = path.substr(0, q);
    }

    // ---- 文件传输 API（契约 docs/design/file-transfer-contract.md）----
    // 上传要求 Content-Type: application/octet-stream：跨站简单请求发不出这个
    // 类型（必须先过 CORS 预检，而本服务不响应预检），是 CSRF 写操作的第三道闸。
    if (path == "/api/file/upload" && method == "POST") {
        if (MediaTypeOf(headers.content_type) != "application/octet-stream") {
            return BuildResponse(
                415, "Unsupported Media Type", "application/json; charset=utf-8",
                "{\"ok\":false,\"code\":\"UNSUPPORTED_MEDIA_TYPE\","
                "\"message\":\"请以二进制方式上传文件。\"}");
        }
        FileTransfer ft(uploads_root);
        std::string error;
        std::string saved;
        FileError code = FileError::kNone;
        if (!ft.AppendUpload(ExtractQueryParam(raw_path, "file_name"), body,
                             &error, &saved, &code)) {
            return BuildFileErrorResponse(code, error);
        }
        std::ostringstream os;
        os << "{\"ok\":true,\"saved_path\":\""
           << JsonEscape(RelativeSavedPath(uploads_root, saved))
           << "\",\"file_name\":\"" << JsonEscape(saved)
           << "\",\"size\":" << body.size() << "}";
        return BuildResponse(200, "OK", "application/json; charset=utf-8", os.str());
    }
    if (path == "/api/file/download" && method == "GET") {
        FileTransfer ft(uploads_root);
        std::string error;
        std::string content;
        FileError code = FileError::kNone;
        const std::string file_name = ExtractQueryParam(raw_path, "file_name");
        if (!ft.ReadUpload(file_name, &content, &error, &code)) {
            return BuildFileErrorResponse(code, error);
        }
        std::ostringstream os;
        // 下载响应需带 Content-Disposition（BuildResponse 不支持），故手工拼接；
        // 但**必须**以状态行开头——曾漏掉 "HTTP/1.1 200 OK"，导致客户端把它当成
        // HTTP/0.9（curl 报 "Received HTTP/0.9 when not allowed"）、浏览器下载失败。
        //
        // 文件名编码：HTTP 头是 latin-1/ASCII，直接把 UTF-8 字节写进去会被浏览器
        // 拒绝或乱码。按 RFC 6266 同时给出 ASCII 回退（filename=）与 UTF-8 编码值
        // （filename*=UTF-8''），中文名才能正确落为下载文件名。
        os << "HTTP/1.1 200 OK\r\n"
           << "Content-Type: application/octet-stream\r\n"
           << "Content-Disposition: attachment; filename=\""
           << AsciiFilenameFallback(file_name) << "\"; filename*=UTF-8''"
           << Rfc5987Encode(file_name) << "\r\n"
           << "Content-Length: " << content.size() << "\r\n"
           << "Cache-Control: no-store\r\n"
           << "Connection: close\r\n\r\n"
           << content;
        return os.str();
    }
    // 列出已上传文件（契约 §4.4）。此前该能力只在库层存在、HTTP 层无路由，
    // 导致界面下载列表只能显示「本次会话」上传的文件，重启后旧文件再也取不回来。
    if (path == "/api/file/list" && method == "GET") {
        FileTransfer ft(uploads_root);
        std::string error;
        std::string listing;
        FileError code = FileError::kNone;
        if (!ft.ListUploads(&listing, &error, &code)) {
            return BuildFileErrorResponse(code, error);
        }
        std::ostringstream os;
        os << "{\"ok\":true,\"files\":[";
        size_t begin = 0;
        bool first = true;
        while (begin <= listing.size()) {
            const size_t nl = listing.find('\n', begin);
            const std::string name = listing.substr(
                begin, (nl == std::string::npos ? listing.size() : nl) - begin);
            if (!name.empty()) {
                if (!first) os << ",";
                os << "\"" << JsonEscape(name) << "\"";
                first = false;
            }
            if (nl == std::string::npos) break;
            begin = nl + 1;
        }
        os << "]}";
        return BuildResponse(200, "OK", "application/json; charset=utf-8", os.str());
    }
    // 删除已上传文件（契约 §4.5）。没有它，上传目录只增不减，磁盘无法回收。
    if (path == "/api/file/remove" && method == "POST") {
        FileTransfer ft(uploads_root);
        std::string error;
        FileError code = FileError::kNone;
        const std::string file_name = ExtractQueryParam(raw_path, "file_name");
        if (!ft.RemoveUpload(file_name, &error, &code)) {
            return BuildFileErrorResponse(code, error);
        }
        std::ostringstream os;
        os << "{\"ok\":true,\"file_name\":\"" << JsonEscape(file_name) << "\"}";
        return BuildResponse(200, "OK", "application/json; charset=utf-8", os.str());
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
        // 同上传接口：application/json 非 CORS 安全方法，跨站必须先过预检。
        if (MediaTypeOf(headers.content_type) != "application/json") {
            return BuildResponse(
                415, "Unsupported Media Type", "application/json; charset=utf-8",
                "{\"ok\":false,\"error\":{\"code\":\"UNSUPPORTED_MEDIA_TYPE\","
                "\"message\":\"请求格式不正确。\"}}");
        }
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
            // 变量名不叫 body：与 HandleRequest 的 body 形参同名会触发 C4457
            // （遮蔽）——本项目零警告门禁，且遮蔽后极易在后续改动中误用请求体。
            const std::string payload = ToJson(GetSystemInfo());
            return BuildResponse(200, "OK", "application/json; charset=utf-8", payload);
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


// 连接调度：1 个 accept 线程 + kWorkerCount 个工作线程。
//
// 为什么改线程池：原先是「accept 循环里串行处理完一条连接再收下一条」，
// 于是一个慢客户端能把整个服务按住 —— 普通请求最多 5 秒（读等待上限），
// 超限请求的丢弃分支最多 32 秒。浏览器对 127.0.0.1 默认开 6 条并发连接，
// 只要有一条卡住，整页就僵住。
//
// 队列有界（kMaxQueued）：超出直接关闭新连接，而不是无限堆积线程/连接。
struct LocalServer::Impl {
    HttpServerImpl net;
    ServerConfig config;
    std::thread acceptor;
    std::vector<std::thread> pool;
    std::deque<net_socket_t> queue;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> running;
    int bound_port;

    Impl() : running(false), bound_port(0) {}

    void AcceptLoop();
    void WorkerLoop();
    void ServeConnection(net_socket_t client);
    void Enqueue(net_socket_t client);
    void DrainQueue();
};

void LocalServer::Impl::AcceptLoop() {
    while (!net.stopping.load()) {
        net_socket_t client = NET_INVALID_SOCKET;
        const int r = net.Accept(&client, kAcceptPollMs);
        if (r == 0) {
            continue;
        }
        if (r < 0) {
            if (net.stopping.load()) {
                break;
            }
            continue;  // 单次接受失败不停服
        }
        Enqueue(client);
    }
    running = false;
}

void LocalServer::Impl::Enqueue(net_socket_t client) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        // 队列满 = 背压：直接关掉新连接，避免无限累积内存与文件描述符。
        if (net.stopping.load() || queue.size() >= kMaxQueued) {
            net.CloseSocket(client);
            return;
        }
        queue.push_back(client);
    }
    cv.notify_one();
}

void LocalServer::Impl::WorkerLoop() {
    for (;;) {
        net_socket_t client = NET_INVALID_SOCKET;
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [this] { return net.stopping.load() || !queue.empty(); });
            if (net.stopping.load()) {
                break;  // 停服：不再取新任务，剩余队列由 DrainQueue 统一关闭
            }
            if (queue.empty()) {
                continue;
            }
            client = queue.front();
            queue.pop_front();
        }
        ServeConnection(client);
    }
}

void LocalServer::Impl::DrainQueue() {
    std::deque<net_socket_t> pending;
    {
        std::lock_guard<std::mutex> lock(mutex);
        pending.swap(queue);
    }
    for (size_t i = 0; i < pending.size(); ++i) {
        net.CloseSocket(pending[i]);
    }
}

void LocalServer::Impl::ServeConnection(net_socket_t client) {
    // 请求体上限：与 FileTransfer 的 10 MB 一致，另留余量给请求头，
    // 避免超大 Content-Length 造成内存耗尽（DoS 防护）。
    const size_t kMaxRequestBytes = 11u * 1024u * 1024u;

    const int io_timeout =
        (config.request_timeout_ms > 0) ? config.request_timeout_ms : kRequestTimeoutMs;
    // 单次 Recv 的等待窗口收敛到轮询周期。读超时按「距上次收到字节的间隔」
    // 累计（见 last_progress），这样既限制慢连接占用的时长，又让 Stop() 能在
    // 约一个周期内收敛 —— 否则一个卡住的连接会把优雅停止拖满整个读超时。
    const int poll_ms = (io_timeout < kIoPollMs) ? io_timeout : kIoPollMs;

    std::string raw;
    raw.reserve(4096);
    char buf[4096];
    bool headers_done = false;
    bool too_large = false;
    // 请求体是否已收全。**关键**：超时/断连与「收全」必须区分，
    // 否则会把截断的 body 当完整请求派发，上传出一个静默损坏的文件
    // （实测：声明 Content-Length: 1000 只发 100 字节，服务端仍回 200 OK）。
    bool body_complete = false;
    size_t body_start = 0;
    size_t content_length = 0;
    bool has_content_length = false;
    RequestHeaders headers;
    std::chrono::steady_clock::time_point last_progress =
        std::chrono::steady_clock::now();

    // 先收请求头；再按 Content-Length 收全请求体（支持上传大文件）。
    for (;;) {
        if (net.stopping.load()) {
            break;  // 停服中：不再等待剩余数据
        }
        const int n = net.Recv(client, buf, sizeof(buf) - 1, poll_ms);
        if (n > 0) {
            raw.append(buf, static_cast<size_t>(n));
            last_progress = std::chrono::steady_clock::now();
        } else if (n == 0) {
            break;  // 对端关闭
        } else {
            // 超时或错误：按累计无进展时长判定，尚未超限就继续等。
            const long long idle_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - last_progress).count();
            if (idle_ms >= io_timeout) {
                break;
            }
            continue;
        }

        if (!headers_done) {
            if (!ParseRequestHeaders(raw, &body_start, &headers)) {
                if (raw.size() > 64u * 1024u) {
                    // 头区异常膨胀，直接断开。
                    raw.clear();
                    break;
                }
                continue;  // 头区尚未收全
            }
            headers_done = true;
            if (!headers.has_content_length) {
                body_complete = true;  // 无 body（GET 等），读完头即可
                break;
            }
            content_length = headers.content_length;
            has_content_length = true;
            if (content_length > kMaxRequestBytes) {
                // 声明体积超限：不接收请求体，置标志后跳出。
                // 刻意不清空 raw——保留已收到的请求头，供下方按路径返回对应错误体。
                too_large = true;
                break;
            }
        }
        if (raw.size() - body_start >= content_length) {
            body_complete = true;  // body 已收全
            break;
        }
    }

    // 声明超限时，先把已声明的请求体有界地丢弃，再回错误响应：
    // 若直接关闭，客户端可能仍在发送而收到 RST，读不到错误体（表现为偶发
    // 「网络错误」而非「文件过大」）。丢弃不缓冲，故不违背内存防护初衷；
    // 上限 32 MB + 每次 1s 超时，避免对超大声明值长时间阻塞。
    if (too_large) {
        size_t drained = (raw.size() > body_start) ? (raw.size() - body_start) : 0;
        const size_t kMaxDrain = 32u * 1024u * 1024u;
        char sink[8192];
        while (drained < content_length && drained < kMaxDrain &&
               !net.stopping.load()) {
            const int n = net.Recv(client, sink, sizeof(sink), 1000);
            if (n <= 0) break;
            drained += static_cast<size_t>(n);
        }
    }

    std::string response;
    if (too_large) {
        // 请求体超过服务端接收上限（11 MB）：按 JSON 契约返回用户语言错误。
        // 此前上传路由回裸 200，前端 json 解析失败只能显示笼统提示
        // （文件传输契约 §6.4 要求超限返回 TOO_LARGE 的用户可读文案）。
        std::string method, path;
        size_t line_end = raw.find('\n');
        if (line_end == std::string::npos) line_end = raw.size();
        const std::string first_line = raw.substr(0, line_end + 1);
        bool is_upload = false;
        if (ParseRequestFirstLine(first_line.data(), first_line.size(),
                                  &method, &path)) {
            // 路由比较前先去掉 query string（与 HandleRequest 的路由口径一致）。
            const size_t q = path.find('?');
            const std::string route =
                (q == std::string::npos) ? path : path.substr(0, q);
            is_upload = (route == "/api/file/upload");
        }
        if (is_upload) {
            response = BuildResponse(
                413, "Payload Too Large", "application/json; charset=utf-8",
                "{\"ok\":false,\"code\":\"TOO_LARGE\","
                "\"message\":\"文件过大，请选择小于 10 MB 的文件。\"}");
        } else {
            response = BuildResponse(
                413, "Payload Too Large", "application/json; charset=utf-8",
                "{\"ok\":false,\"code\":\"REQUEST_TOO_LARGE\","
                "\"message\":\"请求体过大。\"}");
        }
    } else if (!raw.empty()) {
        std::string method, path;
        // 首行解析：把请求行（含结尾 \n）整段传入，避免二次查找失败。
        size_t line_end = raw.find('\n');
        if (line_end == std::string::npos) {
            line_end = raw.size() - 1;
        }
        const std::string first_line = raw.substr(0, line_end + 1);
        if (!ParseRequestFirstLine(first_line.data(), first_line.size(),
                                   &method, &path)) {
            response = BuildResponse(400, "Bad Request",
                                     "text/plain; charset=utf-8", "Bad Request");
        } else if (!IsAllowedHostHeader(headers.host, bound_port)) {
            // 挡住 DNS rebinding：浏览器会把恶意域名解析到 127.0.0.1 后再带
            // 非回环 Host 发来。不透露具体原因，避免给探测方反馈。
            response = BuildResponse(403, "Forbidden", "text/plain; charset=utf-8",
                                     "Forbidden");
        } else if (!IsAllowedOriginHeader(headers.origin, bound_port)) {
            // 挡住跨站 fetch / form 的写操作（同源请求不带 Origin）。
            response = BuildResponse(403, "Forbidden", "text/plain; charset=utf-8",
                                     "Forbidden");
        } else if (headers_done && has_content_length && !body_complete) {
            // 声明了 Content-Length 却没有收全（中途超时或对端断开）：
            // 绝不能把已收到的片段当完整请求处理，否则会写出截断的文件。
            response = BuildResponse(400, "Bad Request", "text/plain; charset=utf-8",
                                     "Bad Request");
        } else {
            std::string body;
            if (headers_done && body_start <= raw.size()) {
                // 按 Content-Length 精确截取：一次 read 里可能夹带多余字节，
                // 全量取会把它们当请求体（"length" 超长时应截断）。
                const size_t avail = raw.size() - body_start;
                body = raw.substr(body_start,
                                  (avail < content_length) ? avail : content_length);
            }
            response = HandleRequest(method, path, body, config.uploads_root,
                                     config, headers);
        }
    }
    if (response.empty()) {
        response = BuildResponse(400, "Bad Request",
                                 "text/plain; charset=utf-8", "Bad Request");
    }
    net.SendAll(client, response.data(), response.size());
    net.CloseSocket(client);
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
        // 引用计数的成对释放只由这里负责：HttpServerImpl::Listen 失败路径里
        // **不得**再调 NetGlobalShutdown，否则两个 LocalServer 时，
        // 第二个启动失败会把第一个正在运行的服务的 Winsock 拆掉。
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

void LocalServer::Stop() {
    if (!impl_->acceptor.joinable()) {
        impl_->running = false;
        return;
    }
    impl_->net.stopping = true;
    impl_->cv.notify_all();
    impl_->acceptor.join();
    // 必须 join 完所有工作线程再析构 Impl：工作线程持有 this，
    // 未 join 就 delete 即 use-after-free（joinable 的 std::thread 未 join
    // 析构会直接 std::terminate）。
    for (size_t i = 0; i < impl_->pool.size(); ++i) {
        impl_->pool[i].join();
    }
    impl_->pool.clear();
    impl_->DrainQueue();
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
