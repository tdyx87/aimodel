// 角色：cpp-implementer
// 产物：tests/local_http_test.cpp
// 契约：docs/design/save-format-contract.md §4
//
// 这些断言无法在库层做：闸门（Host/Origin/Content-Type）、请求体完整性、
// HTTP 状态码全都发生在「请求 → 响应」这一层。

#include "saveinspector/local_http.h"
#include "saveinspector/save_inspector.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>

#if defined(_WIN32) || defined(_WIN64)
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace {

// 服务端每条响应都以 Connection: close 收尾并主动关闭，
// 因此「一直读到 0」即可拿到完整响应，无需解析 Content-Length。
int ClientSocket() {
#if defined(_WIN32) || defined(_WIN64)
    static bool ready = false;
    if (!ready) {
        WSADATA wsa;
        ::WSAStartup(MAKEWORD(2, 2), &wsa);
        ready = true;
    }
#endif
    const int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    return (fd < 0) ? -1 : fd;
}

bool ConnectLoopback(int fd, int port) {
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
}

void CloseFd(int fd) {
    if (fd >= 0) {
#if defined(_WIN32) || defined(_WIN64)
        ::closesocket(static_cast<SOCKET>(fd));
#else
        ::close(fd);
#endif
    }
}

bool SendBytes(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
#if defined(_WIN32) || defined(_WIN64)
        const int n = ::send(static_cast<SOCKET>(fd), data + sent,
                             static_cast<int>(len - sent), 0);
#else
        const ssize_t n = ::send(fd, data + sent, len - sent, 0);
#endif
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

std::string ReadAll(int fd) {
    std::string out;
    char buf[4096];
    for (;;) {
#if defined(_WIN32) || defined(_WIN64)
        const int n = ::recv(static_cast<SOCKET>(fd), buf, sizeof(buf), 0);
#else
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
#endif
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

int StatusOf(const std::string& response) {
    if (response.size() < 12 || response.compare(0, 5, "HTTP/") != 0) return 0;
    return std::atoi(response.substr(9, 3).c_str());
}

std::string BodyOf(const std::string& response) {
    const size_t sep = response.find("\r\n\r\n");
    return (sep == std::string::npos) ? std::string() : response.substr(sep + 4);
}

// 构造一个最小的合法 v3 存档（单槽位）。
std::string MakeMinimalSave() {
    const char kMagic[8] = {'S', 'A', 'V', 'E', 'F', 'I', 'L', 'E'};
    const char kFooter[8] = {'E', 'N', 'D', 'S', 'L', 'O', 'T', '\0'};
    std::string slot;
    const uint32_t level = 12;
    const uint64_t coins = 999;
    const uint64_t playtime = 3600 * 5;
    for (int i = 0; i < 4; ++i) slot.push_back(static_cast<char>((level >> (8 * i)) & 0xFF));
    for (int i = 0; i < 8; ++i) slot.push_back(static_cast<char>((coins >> (8 * i)) & 0xFF));
    for (int i = 0; i < 8; ++i) slot.push_back(static_cast<char>((playtime >> (8 * i)) & 0xFF));
    slot.push_back(0); slot.push_back(0); slot.push_back(0); slot.push_back(0);  // items=0
    slot.push_back(0); slot.push_back(0); slot.push_back(0); slot.push_back(0);  // achs=0
    for (int i = 0; i < 8; ++i) slot.push_back(0);   // last_played
    for (int i = 0; i < 8; ++i) slot.push_back(0);   // signature

    std::string header(kMagic, 8);
    const uint32_t version = 3, flags = 0, count = 1, reserved = 0;
    const uint64_t created = 1773000000;
    for (int i = 0; i < 4; ++i) header.push_back(static_cast<char>((version >> (8 * i)) & 0xFF));
    for (int i = 0; i < 4; ++i) header.push_back(static_cast<char>((flags >> (8 * i)) & 0xFF));
    for (int i = 0; i < 4; ++i) header.push_back(static_cast<char>((count >> (8 * i)) & 0xFF));
    for (int i = 0; i < 4; ++i) header.push_back(static_cast<char>((reserved >> (8 * i)) & 0xFF));
    for (int i = 0; i < 8; ++i) header.push_back(static_cast<char>((created >> (8 * i)) & 0xFF));

    std::string table;
    const uint64_t offset = static_cast<uint64_t>(header.size() + 24);
    const uint64_t length = static_cast<uint64_t>(slot.size());
    const uint64_t crc = saveinspector::Crc64Ecma182(slot);
    for (int i = 0; i < 8; ++i) table.push_back(static_cast<char>((offset >> (8 * i)) & 0xFF));
    for (int i = 0; i < 8; ++i) table.push_back(static_cast<char>((length >> (8 * i)) & 0xFF));
    for (int i = 0; i < 8; ++i) table.push_back(static_cast<char>((crc >> (8 * i)) & 0xFF));

    return header + table + slot + std::string(kFooter, 8);
}

class LocalHttpTest : public ::testing::Test {
protected:
    void SetUp() {
        saveinspector::ServerConfig config;
        config.bind_host = "127.0.0.1";
        config.port = 0;
        config.static_root.clear();
        // 缩短读超时：请求体截断用例要靠它触发，不能等满 5 秒。
        config.request_timeout_ms = 250;
        ASSERT_TRUE(server_.Start(config, &error_)) << error_;
        ASSERT_GT(server_.bound_port(), 0);
    }

    void TearDown() { server_.Stop(); }

    // 发一个请求，返回原始响应。body 为空表示不带请求体。
    std::string Send(const std::string& request, const std::string& body = std::string()) {
        const int fd = ClientSocket();
        if (fd < 0) return std::string();
        std::string out;
        if (ConnectLoopback(fd, server_.bound_port())) {
            const bool head_ok = SendBytes(fd, request.data(), request.size());
            const bool body_ok =
                body.empty() || SendBytes(fd, body.data(), body.size());
            if (head_ok && body_ok) out = ReadAll(fd);
        }
        CloseFd(fd);
        return out;
    }

    // 发请求头后只补 body_bytes 个占位字节后停手：模拟请求体中途超时/断连。
    std::string SendTruncated(const std::string& request, int body_bytes) {
        const std::string filler(static_cast<size_t>(body_bytes), 'x');
        return Send(request, filler);
    }

    // 组装一次「上传存档字节」的请求。
    std::string OpenRequest(const std::string& save) const {
        return "POST /api/save/open HTTP/1.1\r\n" + Host() +
               "Content-Type: application/octet-stream\r\n"
               "Content-Length: " + std::to_string(save.size()) + "\r\n\r\n";
    }

    std::string Host() const {
        return "Host: 127.0.0.1:" + std::to_string(server_.bound_port()) + "\r\n";
    }

    saveinspector::LocalHttpServer server_;
    std::string error_;
};

}  // namespace

// ---------------------------------------------------------------------------
// 请求闸门（契约 §4.6）
// ---------------------------------------------------------------------------

TEST_F(LocalHttpTest, RejectsNonLoopbackHostHeader) {
    // DNS rebinding：恶意域名解析到 127.0.0.1 后，浏览器仍会带原 Host 发来。
    EXPECT_EQ(StatusOf(Send("GET /api/save/slots HTTP/1.1\r\nHost: evil.example.com\r\n\r\n")),
              403);
}

TEST_F(LocalHttpTest, AcceptsLoopbackHost) {
    // 本夹具刻意不配 static_root，故 `/` 返回 404 而非 200。
    // 这里要断言的是：**回环 Host 不会被闸门拦下**（不是 403）。
    const int status = StatusOf(Send("GET / HTTP/1.1\r\n" + Host() + "\r\n"));
    EXPECT_NE(status, 403) << "回环 Host 被闸门误拦";
    EXPECT_EQ(status, 404) << "未配 static_root 时根路径应为 404";
}

TEST_F(LocalHttpTest, RejectsCrossOriginWrite) {
    EXPECT_EQ(StatusOf(Send("POST /api/save/open HTTP/1.1\r\n" + Host() +
                            "Origin: https://evil.example.com\r\n"
                            "Content-Type: application/octet-stream\r\n"
                            "Content-Length: 1\r\n\r\nx")),
              403);
}

TEST_F(LocalHttpTest, AcceptsLoopbackOrigin) {
    const std::string save = MakeMinimalSave();
    std::string head = OpenRequest(save);
    head = "POST /api/save/open HTTP/1.1\r\n" + Host() +
           "Origin: http://127.0.0.1:" + std::to_string(server_.bound_port()) + "\r\n"
           "Content-Type: application/octet-stream\r\n"
           "Content-Length: " + std::to_string(save.size()) + "\r\n\r\n";
    EXPECT_EQ(StatusOf(Send(head, save)), 200);
}

TEST_F(LocalHttpTest, RejectsFormContentTypeUpload) {
    // HTML <form> 的跨站简单请求只能发 text/plain 这类 CORS 安全类型；
    // 要求 octet-stream 等于强制预检，而本服务不响应预检。
    EXPECT_EQ(StatusOf(Send("POST /api/save/open HTTP/1.1\r\n" + Host() +
                            "Content-Type: text/plain\r\n"
                            "Content-Length: 1\r\n\r\nx")),
              415);
}

// ---------------------------------------------------------------------------
// 请求体完整性（契约 §4.7）
// ---------------------------------------------------------------------------

TEST_F(LocalHttpTest, TruncatedBodyIsRejected) {
    // 声明 4096 字节，只发 16 字节后静默：必须 400，绝不能把截断数据当存档解析。
    const std::string head = "POST /api/save/open HTTP/1.1\r\n" + Host() +
                             "Content-Type: application/octet-stream\r\n"
                             "Content-Length: 4096\r\n\r\n";
    EXPECT_EQ(StatusOf(SendTruncated(head, 16)), 400);
}

TEST_F(LocalHttpTest, MissingBodyEntirelyIsRejected) {
    // 声明了 Content-Length 却一个字节都不发：同样不得派发。
    const std::string head = "POST /api/save/open HTTP/1.1\r\n" + Host() +
                             "Content-Type: application/octet-stream\r\n"
                             "Content-Length: 4096\r\n\r\n";
    EXPECT_EQ(StatusOf(Send(head)), 400);
}

// ---------------------------------------------------------------------------
// 业务结果与状态码（契约 §4）
// ---------------------------------------------------------------------------

TEST_F(LocalHttpTest, OpeningGarbageReturnsBusinessErrorWithDiagnostics) {
    // 「这不是存档」是可预期的业务结果 → 200 + {ok:false,diagnostics}，
    // 不是协议错误。界面需要结构化诊断数组来渲染 diagnosis 视图。
    const std::string garbage("this is definitely not a save file");
    const std::string response = Send(OpenRequest(garbage), garbage);
    EXPECT_EQ(StatusOf(response), 200);
    const std::string body = BodyOf(response);
    EXPECT_NE(body.find("\"ok\":false"), std::string::npos) << body;
    EXPECT_NE(body.find("\"diagnostics\""), std::string::npos) << body;
    // 三段式不可省（PRD US-5）
    EXPECT_NE(body.find("\"advice\""), std::string::npos) << body;
}

TEST_F(LocalHttpTest, SlotEndpointRequiresPriorOpen) {
    EXPECT_EQ(StatusOf(Send("GET /api/save/slots HTTP/1.1\r\n" + Host() + "\r\n")), 409);
    EXPECT_EQ(StatusOf(Send("GET /api/save/slot?index=0 HTTP/1.1\r\n" + Host() + "\r\n")), 409);
}

TEST_F(LocalHttpTest, OpenThenListSlots) {
    const std::string save = MakeMinimalSave();
    const std::string opened = Send(OpenRequest(save), save);
    ASSERT_EQ(StatusOf(opened), 200) << BodyOf(opened);
    // 成功与业务失败都是 200，必须断言响应体而不是状态码
    ASSERT_NE(BodyOf(opened).find("\"ok\":true"), std::string::npos) << BodyOf(opened);

    const std::string slots = Send("GET /api/save/slots HTTP/1.1\r\n" + Host() + "\r\n");
    EXPECT_EQ(StatusOf(slots), 200) << slots;
    EXPECT_NE(BodyOf(slots).find("\"index\":0"), std::string::npos) << BodyOf(slots);
}

TEST_F(LocalHttpTest, ExportCarriesContentDispositionAndNoPath) {
    const std::string save = MakeMinimalSave();
    ASSERT_EQ(StatusOf(Send(OpenRequest(save), save)), 200);

    const std::string response =
        Send("GET /api/save/export?index=0&format=json HTTP/1.1\r\n" + Host() + "\r\n");
    EXPECT_EQ(StatusOf(response), 200) << response;
    EXPECT_NE(response.find("Content-Disposition: attachment; filename=\"save-slot-1.json\""),
              std::string::npos) << response;
    // 导出文件名不得含本机路径（契约 §5）
    EXPECT_EQ(response.find("\\"), std::string::npos);
}

TEST_F(LocalHttpTest, UnknownApiReturnsNotFound) {
    EXPECT_EQ(StatusOf(Send("GET /api/nope HTTP/1.1\r\n" + Host() + "\r\n")), 404);
}

TEST_F(LocalHttpTest, PathTraversalOnStaticIsRejected) {
    EXPECT_EQ(StatusOf(Send("GET /../../../Windows/win.ini HTTP/1.1\r\n" + Host() + "\r\n")),
              404);
}

// ---------------------------------------------------------------------------
// 并发与停止
// ---------------------------------------------------------------------------

TEST_F(LocalHttpTest, SlowConnectionDoesNotBlockOtherRequests) {
    const int stuck = ClientSocket();
    ASSERT_GE(stuck, 0);
    ASSERT_TRUE(ConnectLoopback(stuck, server_.bound_port()));
    const std::string head = "GET /api/save/slots HTTP/1.1\r\n" + Host() + "\r\n";
    ASSERT_TRUE(SendBytes(stuck, head.data(), head.size()));

    // 串行实现下，这个请求会被卡住连接的读等待拖满超时。
    const int status = StatusOf(Send("GET / HTTP/1.1\r\n" + Host() + "\r\n"));
    EXPECT_NE(status, 403) << "回环 Host 被闸门误拦";
    EXPECT_NE(status, 400) << "请求被误判为不合法";
    CloseFd(stuck);
}

TEST_F(LocalHttpTest, StopIsPromptWithConnectionInFlight) {
    const int stuck = ClientSocket();
    ASSERT_GE(stuck, 0);
    ASSERT_TRUE(ConnectLoopback(stuck, server_.bound_port()));
    const std::string head = "GET /api/save/slots HTTP/1.1\r\n" + Host() + "\r\n";
    ASSERT_TRUE(SendBytes(stuck, head.data(), head.size()));

    const std::clock_t start = std::clock();
    server_.Stop();
    const double seconds = static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;

    EXPECT_LT(seconds, 2.0) << "Stop() 耗时 " << seconds << "s，说明被卡住的连接拖住了";
    EXPECT_FALSE(server_.IsRunning());
    CloseFd(stuck);
}
