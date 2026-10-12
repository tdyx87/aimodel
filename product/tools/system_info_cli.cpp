// system_info_cli：命令行输出本机系统信息（JSON）。
// 用法：system_info_cli [--compact]（默认多行缩进；--compact 输出单行）

#include "aimodel/system_info.h"

#include <cstdio>
#include <string>

namespace {

void PrintUsage() {
    std::printf("Usage: system_info_cli [options]\n"
                "Options:\n"
                "  -h, --help    Show this help\n"
                "  --compact     Output single-line JSON\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    bool compact = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            PrintUsage();
            return 0;
        } else if (arg == "--compact") {
            compact = true;
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            PrintUsage();
            return 2;
        }
    }

    aimodel::SystemInfo info = aimodel::GetSystemInfo();
    std::string json = aimodel::ToJson(info);
    if (compact) {
        // 压缩：去掉换行（字符串内的 \n 已转义为字面 \n，安全）
        std::string flat;
        flat.reserve(json.size());
        for (size_t i = 0; i < json.size(); ++i) {
            if (json[i] != '\n') {
                flat.push_back(json[i]);
            }
        }
        std::printf("%s\n", flat.c_str());
    } else {
        std::printf("%s", json.c_str());
    }
    return 0;
}
