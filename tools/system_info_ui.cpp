// 产品入口：本地 HTTP 服务 + 打开默认浏览器。
// 产品形态：用户双击启动后在本地浏览器打开硬件报告界面（隐私：仅 127.0.0.1）。

#include "aimodel/http_server.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#if defined(_WIN32) || defined(_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <cstdlib>
#endif

namespace {

volatile std::sig_atomic_t g_quit = 0;

void SignalHandler(int) {
    g_quit = 1;
}

void PrintHelp(const char* prog) {
    std::printf("用法：%s [选项]\n", prog);
    std::printf("产品入口：在本地浏览器中启动用户界面，并提供硬件报告 HTTP 服务。\n"
                "隐私：服务仅监听 127.0.0.1；数据不出本机。\n"
                "选项：\n"
                "  --port N       监听端口（默认 0 = 系统分配，启动后打印真实端口）\n"
                "  --no-open      启动后不自动打开浏览器\n"
                "  --web-root DIR 静态资源目录（默认：与可执行文件同级的 web）\n"
                "  --help         显示本帮助\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    int port = 0;
    bool no_open = false;
    std::string web_root = "web";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            PrintHelp(argv[0]);
            return 0;
        } else if (arg == "--port") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "错误：--port 需要一个数值参数\n");
                return 2;
            }
            port = std::atoi(argv[++i]);
        } else if (arg == "--no-open") {
            no_open = true;
        } else if (arg == "--web-root") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "错误：--web-root 需要一个目录参数\n");
                return 2;
            }
            web_root = argv[++i];
        } else {
            std::fprintf(stderr, "未知选项：%s\n", argv[i]);
            return 2;
        }
    }

    aimodel::ServerConfig config;
    config.bind_host = "127.0.0.1";
    config.port = port;
    config.static_root = web_root;

    aimodel::LocalServer server;
    std::string error;
    if (!server.Start(config, &error)) {
        std::fprintf(stderr, "启动服务失败：%s\n", error.c_str());
        return 1;
    }

    const int bound = server.bound_port();
    std::printf("aimodel 已启动。\n");
    std::printf("  界面：http://127.0.0.1:%d/\n", bound);
    std::printf("  硬件报告：http://127.0.0.1:%d/api/system-info\n", bound);
    std::printf("  按 Ctrl+C 退出。\n", bound);

    if (!no_open) {
#if defined(_WIN32) || defined(_WIN64)
        std::string cmd = std::string("start \"aimodel\" \"") + "http://127.0.0.1:" + std::to_string(bound) + "\"";
        std::system(cmd.c_str());
#elif defined(__APPLE__) || defined(__linux__)
        std::string cmd = "xdg-open \"http://127.0.0.1:" + std::to_string(bound) + "\"";
        std::system(cmd.c_str());
#endif
    }

    // 等待 Ctrl+C（进程终止时服务器自动释放）
    signal(SIGINT, SignalHandler);
    while (!g_quit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::printf("\naimodel 已停止。\n");
    return 0;
}
