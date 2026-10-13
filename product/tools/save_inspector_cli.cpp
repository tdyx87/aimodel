// 角色：cpp-implementer
// 产物：tools/save_inspector_cli.cpp
// 用途：命令行查看存档（CI/QA 批量核对用；玩家主用图形界面）
//
// 编码约定：中文文案一律**不进 printf 的格式串**。MSVC 的格式串检查器遇非
// ASCII 会转码失败并报 C4819（即使已加 /utf-8），还会中止扫描而误判占位符
// 数量（C4474）。因此这里全部改为「std::string 拼接 + fputs」输出。

#include "saveinspector/save_inspector.h"

#include <cstdio>
#include <string>

namespace {

void Emit(const std::string& text) {
    std::fputs(text.c_str(), stdout);
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        Emit("用法：save_inspector_cli <存档文件>\n");
        return 2;
    }

    saveinspector::SaveInspector inspector;
    saveinspector::SaveFile save;
    std::string error;
    if (!inspector.OpenFile(argv[1], &save, &error)) {
        std::fputs(("无法打开存档：" + error + "\n").c_str(), stderr);
        return 1;
    }

    Emit("存档版本：v" + std::to_string(static_cast<unsigned>(save.version)) + "\n");
    Emit("创建时间：" + saveinspector::FormatTimestamp(save.created_utc) + "\n");
    Emit("槽位数量：" + std::to_string(static_cast<unsigned>(save.slots.size())) + "\n\n");

    for (size_t i = 0; i < save.diagnostics.size(); ++i) {
        const saveinspector::Diagnostic& d = save.diagnostics[i];
        Emit(std::string("[") + saveinspector::SeverityName(d.severity) + "] " +
             d.title + "\n");
        Emit("  " + d.detail + "\n");
        Emit("  → " + d.advice + "\n");
    }
    if (!save.diagnostics.empty()) Emit("\n");

    for (size_t i = 0; i < save.slots.size(); ++i) {
        Emit(inspector.SlotToTextReport(save.slots[i]));
        Emit("\n");
    }

    Emit("提示：存档查看器只读取存档，从不修改你的原始文件。\n");
    return 0;
}
