#pragma once

// 极简运行日志。
//
// 为什么需要它：GUI 程序出问题时用户很难描述（「点了没反应」「一闪就没了」），
// 而 Windows 的默认崩溃表现对用户是零信息。有了这一个文件，
// 用户只要把 %TEMP%\garbage_cleaner_gui.log 发过来就能定位到哪一步断了。
//
// 刻意不引入日志库：只需要「追加一行、带时间戳」，spdlog 之类反而增加依赖面。

#include <string>

namespace gc {

// 追加一行到 %TEMP%\garbage_cleaner_gui.log（UTF-8，带本地时间戳）。
// 任何线程都可以调用；打开失败就静默放弃——写日志失败不该影响清理垃圾这件正事。
void LogLine(const std::wstring& text);

// 安装进程级未处理异常过滤器：崩溃时先把异常码与地址写进日志再让进程退出。
void InstallCrashLogger();

// 日志文件路径（供界面或测试显示/读取）
std::wstring LogFilePath();

} // namespace gc
