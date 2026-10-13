// 单元测试：本地 HTTP 服务的请求闸门、请求体完整性、错误状态码与文件传输接口。
//
// 为什么这些断言不能放在库层单测里：它们全部发生在「HTTP 请求 → 响应」这一层，
// 库层（FileTransfer / chat_suggest）根本看不到 Host 头、请求体是否收全、状态码。
//
// 覆盖重点（对应历史实测复现过的缺陷）：
//   1. 截断上传必须 400，且**不得**落盘半个文件（静默数据损坏）；
//   2. 非回环 Host 必须 403（DNS rebinding）；
//   3. 跨站 Origin 的写操作必须 403；
//   4. 写接口缺少约定 Content-Type 必须 415（HTML form 跨站 CSRF）；
//   5. 错误响应用对应状态码，不再一律 200（否则前端会把 JSON 当页面导航）。

#include "aimodel/http_server.h"

#include <gtest/gtest.h>

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

// ---------------------------------------------------------------------------
// 裸 socket 客户端：服务端每条响应都以 Connection: close 收尾并主动关闭，
// 因此「一直读到 0」即可拿到完整响应，不需要解析 Content-Length。
// ---------------------------------------------------------------------------

int ClientSocket() {
#if defined(_WIN32) || defined(_WIN64)
    static bool wsa_ready = false;
    if (!wsa_ready) {
        WSADATA wsa;
        ::WSAStartup(MAKEWORD(2, 2), &wsa);
        wsa_ready = true;
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

void CloseSocketFd(int fd) {
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
        if (n <= 0) {
            return false;
        }
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
        if (n <= 0) {
            break;
        }
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

// 取响应状态码（首行的三位数字）。无响应时返回 0。
int StatusOf(const std::string& response) {
    if (response.size() < 12 || response.compare(0, 5, "HTTP/") != 0) {
        return 0;
    }
    return std::atoi(response.substr(9, 3).c_str());
}

std::string BodyOf(const std::string& response) {
    const size_t sep = response.find("\r\n\r\n");
    return (sep == std::string::npos) ? std::string() : response.substr(sep + 4);
}

// ---------------------------------------------------------------------------
// 测试夹具：起一个真实服务（随机端口），用独立的临时上传目录。
// ---------------------------------------------------------------------------

std::string MakeTempRoot(const char* tag) {
    static unsigned int counter = 0;
    const std::time_t now = std::time(NULL);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "http_test_%s_%ld_%u", tag,
                  static_cast<long>(now), ++counter);
    return std::string(buf);
}

bool UploadedFileExists(const std::string& root, const std::string& name) {
    std::ifstream probe((root + "/" + name).c_str(), std::ios::binary);
    return probe.is_open();
}

class HttpServerTest : public ::testing::Test {
protected:
    void SetUp() {
        uploads_root_ = MakeTempRoot("upload");

        aimodel::ServerConfig config;
        config.bind_host = "127.0.0.1";
        config.port = 0;                 // 系统分配端口
        config.static_root.clear();      // 这些用例不需要静态资源
        config.uploads_root = uploads_root_;
        // 缩短读超时：截断上传用例要靠它触发，不能让用例等满 5 秒。
        config.request_timeout_ms = 250;

        ASSERT_TRUE(server_.Start(config, &error_)) << error_;
        ASSERT_GT(server_.bound_port(), 0);
    }

    void TearDown() { server_.Stop(); }

    // 发起一次完整请求并返回原始响应。
    std::string Send(const std::string& request) {
        const int fd = ClientSocket();
        if (fd < 0) {
            return std::string();
        }
        std::string out;
        if (ConnectLoopback(fd, server_.bound_port())) {
            if (SendBytes(fd, request.data(), request.size())) {
                out = ReadAll(fd);
            }
        }
        CloseSocketFd(fd);
        return out;
    }

    // 发出请求头 + 部分 body 后停手，模拟上传中途超时/断连。
    std::string SendTruncated(const std::string& request_head, int body_bytes) {
        const int fd = ClientSocket();
        if (fd < 0) {
            return std::string();
        }
        std::string out;
        if (ConnectLoopback(fd, server_.bound_port())) {
            const bool head_ok = SendBytes(fd, request_head.data(), request_head.size());
            const std::string partial(body_bytes, 'a');
            const bool body_ok =
                (body_bytes <= 0) ||
                SendBytes(fd, partial.data(), partial.size());
            if (head_ok && body_ok) {
                out = ReadAll(fd);
            }
        }
        CloseSocketFd(fd);
        return out;
    }

    std::string Host() const {
        return "Host: 127.0.0.1:" + std::to_string(server_.bound_port()) + "\r\n";
    }

    aimodel::LocalServer server_;
    std::string uploads_root_;
    std::string error_;
};

// ---------------------------------------------------------------------------
// 1. 请求体完整性
// ---------------------------------------------------------------------------

TEST_F(HttpServerTest, TruncatedUploadIsRejectedAndWritesNothing) {
    // 声明 1000 字节，只发 100 字节后静默。
    const std::string head =
        "POST /api/file/upload?file_name=trunc.bin HTTP/1.1\r\n" + Host() +
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: 1000\r\n\r\n";
    const std::string response = SendTruncated(head, 100);

    // 必须拒绝。
    EXPECT_EQ(StatusOf(response), 400) << response;
    // 关键：不能把截断的内容当完整文件落盘（否则静默数据损坏）。
    EXPECT_FALSE(UploadedFileExists(uploads_root_, "trunc.bin"));
}

TEST_F(HttpServerTest, CompleteUploadSucceedsAndIsListed) {
    const std::string head =
        "POST /api/file/upload?file_name=ok.txt HTTP/1.1\r\n" + Host() +
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: 5\r\n\r\n";
    const std::string response = SendTruncated(head, 5);

    EXPECT_EQ(StatusOf(response), 200) << response;
    const std::string body = BodyOf(response);
    EXPECT_NE(body.find("\"ok\":true"), std::string::npos) << body;
    EXPECT_NE(body.find("\"file_name\":\"ok.txt\""), std::string::npos) << body;
    // 契约 §4.1 要求 saved_path；且不得泄露服务端绝对路径。
    EXPECT_NE(body.find("\"saved_path\":"), std::string::npos) << body;
    EXPECT_EQ(body.find(":" + uploads_root_), std::string::npos) << body;
    EXPECT_TRUE(UploadedFileExists(uploads_root_, "ok.txt"));
}

// ---------------------------------------------------------------------------
// 2/3/4. 请求闸门：Host / Origin / Content-Type
// ---------------------------------------------------------------------------

TEST_F(HttpServerTest, RejectsNonLoopbackHostHeader) {
    // DNS rebinding：恶意域名解析到 127.0.0.1 后浏览器仍会带原 Host 发来。
    const std::string response = Send(
        "GET /api/system-info HTTP/1.1\r\nHost: evil.example.com\r\n\r\n");
    EXPECT_EQ(StatusOf(response), 403) << response;
}

TEST_F(HttpServerTest, RejectsCrossOriginWrite) {
    // 跨站 fetch 写操作：Origin 非本机必须 403。
    const std::string response = Send(
        "POST /api/file/upload?file_name=csrf.txt HTTP/1.1\r\n" + Host() +
        "Origin: https://evil.example.com\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: 5\r\n\r\nhello");
    EXPECT_EQ(StatusOf(response), 403) << response;
    EXPECT_FALSE(UploadedFileExists(uploads_root_, "csrf.txt"));
}

TEST_F(HttpServerTest, AcceptsLoopbackOrigin) {
    const std::string response = Send(
        "POST /api/file/upload?file_name=same.txt HTTP/1.1\r\n" + Host() +
        "Origin: http://127.0.0.1:" + std::to_string(server_.bound_port()) + "\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: 5\r\n\r\nhello");
    EXPECT_EQ(StatusOf(response), 200) << response;
    EXPECT_TRUE(UploadedFileExists(uploads_root_, "same.txt"));
}

TEST_F(HttpServerTest, RejectsFormContentTypeUpload) {
    // HTML <form> 的跨站简单请求只能发出 text/plain 这类 CORS 安全类型；
    // 要求 octet-stream 等于强制预检，而本服务不响应预检。
    const std::string response = Send(
        "POST /api/file/upload?file_name=form.txt HTTP/1.1\r\n" + Host() +
        "Content-Type: text/plain\r\n"
        "Content-Length: 5\r\n\r\nhello");
    EXPECT_EQ(StatusOf(response), 415) << response;
    EXPECT_FALSE(UploadedFileExists(uploads_root_, "form.txt"));
}

TEST_F(HttpServerTest, AcceptsLoopbackHost) {
    const std::string response = Send("GET /api/system-info HTTP/1.1\r\n" + Host() + "\r\n");
    EXPECT_EQ(StatusOf(response), 200) << response;
}

// ---------------------------------------------------------------------------
// 5. 错误响应的状态码
// ---------------------------------------------------------------------------

TEST_F(HttpServerTest, DownloadMissingFileReturnsNotFound) {
    // 此前一律回 200 + JSON，前端用 window.location.href 下载时会把这段
    // JSON 当页面渲染，把用户踢出整个界面。
    const std::string response = Send(
        "GET /api/file/download?file_name=missing.bin HTTP/1.1\r\n" + Host() + "\r\n");
    EXPECT_EQ(StatusOf(response), 404) << response;
    const std::string body = BodyOf(response);
    EXPECT_NE(body.find("\"ok\":false"), std::string::npos) << body;
    EXPECT_NE(body.find("\"code\":\"NOT_FOUND\""), std::string::npos) << body;
}

TEST_F(HttpServerTest, DownloadTraversalIsRejected) {
    const std::string response = Send(
        "GET /api/file/download?file_name=..%2F..%2Fsecret.txt HTTP/1.1\r\n" +
        Host() + "\r\n");
    EXPECT_EQ(StatusOf(response), 400) << response;
    EXPECT_NE(BodyOf(response).find("PATH_NOT_ALLOWED"), std::string::npos);
}

TEST_F(HttpServerTest, DangerousExtensionIsRejectedAsBadRequest) {
    // 危险扩展名此前报 INTERNAL_ERROR（语义错，用户只会看到笼统的「操作失败」）。
    const std::string response = Send(
        "POST /api/file/upload?file_name=payload.exe HTTP/1.1\r\n" + Host() +
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: 1\r\n\r\nx");
    EXPECT_EQ(StatusOf(response), 400) << response;
    EXPECT_NE(BodyOf(response).find("INVALID_FILENAME"), std::string::npos);
    EXPECT_FALSE(UploadedFileExists(uploads_root_, "payload.exe"));
}

TEST_F(HttpServerTest, DownloadSuccessCarriesContentDisposition) {
    ASSERT_EQ(StatusOf(Send(
        "POST /api/file/upload?file_name=dl.txt HTTP/1.1\r\n" + Host() +
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: 11\r\n\r\nhello world")), 200);

    const std::string response = Send(
        "GET /api/file/download?file_name=dl.txt HTTP/1.1\r\n" + Host() + "\r\n");
    EXPECT_EQ(StatusOf(response), 200) << response;
    EXPECT_NE(response.find("Content-Disposition: attachment; filename=\"dl.txt\""),
              std::string::npos) << response;
    EXPECT_NE(response.find("Content-Type: application/octet-stream"),
              std::string::npos) << response;
    EXPECT_EQ(BodyOf(response), std::string("hello world"));
}

// ---------------------------------------------------------------------------
// 6. 新增的 list / remove 接口（契约 §4.4 / §4.5）
// ---------------------------------------------------------------------------

TEST_F(HttpServerTest, ListReturnsUploadedFiles) {
    ASSERT_EQ(StatusOf(Send(
        "POST /api/file/upload?file_name=a.txt HTTP/1.1\r\n" + Host() +
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: 1\r\n\r\nx")), 200);

    const std::string response = Send("GET /api/file/list HTTP/1.1\r\n" + Host() + "\r\n");
    EXPECT_EQ(StatusOf(response), 200) << response;
    const std::string body = BodyOf(response);
    EXPECT_NE(body.find("\"ok\":true"), std::string::npos) << body;
    EXPECT_NE(body.find("\"a.txt\""), std::string::npos) << body;
}

TEST_F(HttpServerTest, RemoveDeletesUploadedFile) {
    ASSERT_EQ(StatusOf(Send(
        "POST /api/file/upload?file_name=gone.txt HTTP/1.1\r\n" + Host() +
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: 1\r\n\r\nx")), 200);
    ASSERT_TRUE(UploadedFileExists(uploads_root_, "gone.txt"));

    EXPECT_EQ(StatusOf(Send(
        "POST /api/file/remove?file_name=gone.txt HTTP/1.1\r\n" + Host() + "\r\n")),
        200);
    EXPECT_FALSE(UploadedFileExists(uploads_root_, "gone.txt"));

    // 再删一次应为 404。
    EXPECT_EQ(StatusOf(Send(
        "POST /api/file/remove?file_name=gone.txt HTTP/1.1\r\n" + Host() + "\r\n")),
        404);
}

// ---------------------------------------------------------------------------
// 7. 并发：一个慢连接不应阻塞其他请求
// ---------------------------------------------------------------------------

TEST_F(HttpServerTest, SlowConnectionDoesNotBlockOtherRequests) {
    // 占用一个连接：只发请求头后停手（服务端会等读超时）。
    const int stuck = ClientSocket();
    ASSERT_GE(stuck, 0);
    ASSERT_TRUE(ConnectLoopback(stuck, server_.bound_port()));
    const std::string head = "GET /api/system-info HTTP/1.1\r\n" + Host() + "\r\n";
    ASSERT_TRUE(SendBytes(stuck, head.data(), head.size()));

    // 串行实现下，这个请求会被卡住连接的读等待拖到超时才返回。
    const std::string response = Send("GET /api/system-info HTTP/1.1\r\n" + Host() + "\r\n");
    EXPECT_EQ(StatusOf(response), 200) << response;

    CloseSocketFd(stuck);
}

TEST_F(HttpServerTest, StopIsPromptWithConnectionInFlight) {
    // 有连接卡在读等待时，Stop() 必须仍能及时收敛（工作线程按轮询周期检查停止标志）。
    const int stuck = ClientSocket();
    ASSERT_GE(stuck, 0);
    ASSERT_TRUE(ConnectLoopback(stuck, server_.bound_port()));
    const std::string head = "GET /api/system-info HTTP/1.1\r\n" + Host() + "\r\n";
    ASSERT_TRUE(SendBytes(stuck, head.data(), head.size()));

    const std::clock_t start = std::clock();
    server_.Stop();
    const double seconds = static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;

    EXPECT_LT(seconds, 2.0) << "Stop() 耗时 " << seconds << "s，说明被卡住的连接拖住了";
    EXPECT_FALSE(server_.IsRunning());
    CloseSocketFd(stuck);
}

}  // namespace
