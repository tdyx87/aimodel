// 角色：cpp-implementer
// 产物：tools/save_inspector_ui.cpp
// 产品入口：启动本地服务并打开浏览器（PRD NFR-1 首屏 ≤ 2 秒）

#include "saveinspector/local_http.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#if defined(_WIN32) || defined(_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

volatile std::sig_atomic_t g_quit = 0;

void SignalHandler(int) {
    g_quit = 1;
}

void PrintHelp(const char* prog) {
    // 中文文案不进 printf 格式串：MSVC 的格式串检查器遇非 ASCII 会转码失败
    // 并误判占位符数量。静态文案直接 fputs。
    std::fputs("存档查看器 —— 在本地查看游戏存档，不修改原始文件。\n", stdout);
    std::fputs("隐私：仅在本机运行，数据不出你的设备。\n", stdout);
    std::fputs("选项：\n", stdout);
    std::fputs("  --port N       监听端口（默认 0 = 系统分配）\n", stdout);
    std::fputs("  --no-open      启动后不自动打开浏览器\n", stdout);
    std::fputs("  --web-root DIR 界面资源目录（默认：与可执行文件同级的 web）\n", stdout);
    std::fputs("  --help         显示本帮助\n", stdout);
    (void)prog;
}

}  // namespace

int main(int argc, char* argv[]) {
    int port = 0;
    bool no_open = false;
    std::string web_root = "web";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            PrintHelp(argv[0]);
            return 0;
        } else if (arg == "--port") {
            if (i + 1 >= argc) {
                std::fputs("错误：--port 需要一个数值参数\n", stderr);
                return 2;
            }
            port = std::atoi(argv[++i]);
        } else if (arg == "--no-open") {
            no_open = true;
        } else if (arg == "--web-root") {
            if (i + 1 >= argc) {
                std::fputs("错误：--web-root 需要一个目录参数\n", stderr);
                return 2;
            }
            web_root = argv[++i];
        } else {
            std::fputs((std::string("未知选项：") + argv[i] + "\n").c_str(), stderr);
            return 2;
        }
    }

    saveinspector::ServerConfig config;
    config.bind_host = "127.0.0.1";
    config.port = port;
    config.static_root = web_root;

    saveinspector::LocalHttpServer server;
    std::string error;
    if (!server.Start(config, &error)) {
        std::fputs((std::string("启动服务失败：") + error + "\n").c_str(), stderr);
        return 1;
    }

    const int bound = server.bound_port();
    std::fputs("存档查看器已启动。\n", stdout);
    std::printf("  界面：http://127.0.0.1:%d/\n", bound);
    std::fputs("  提示：工具只读取存档，绝不修改你的原始文件。\n", stdout);
    std::fputs("  按 Ctrl+C 退出。\n", stdout);

    if (!no_open) {
#if defined(_WIN32) || defined(_WIN64)
        const std::string cmd = std::string("start \"save-inspector\" \"") +
                                "http://127.0.0.1:" + std::to_string(bound) + "\"";
        std::system(cmd.c_str());
#elif defined(__APPLE__)
        const std::string cmd = "open \"http://127.0.0.1:" +
                                std::to_string(bound) + "\"";
        std::system(cmd.c_str());
#else
        const std::string cmd = "xdg-open \"http://127.0.0.1:" +
                                std::to_string(bound) + "\"";
        std::system(cmd.c_str());
#endif
    }

    signal(SIGINT, SignalHandler);
    while (!g_quit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::fputs("\n存档查看器已停止。\n", stdout);
    return 0;
}
