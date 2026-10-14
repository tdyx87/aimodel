#include "gc_log.h"

#include <windows.h>

#include <cstdio>

namespace gc {

namespace {

// 每条日志都是一次「打开 → 追加 → 关闭」，而不是常驻句柄：
// 这样即使用户用编辑器开着日志文件，也不会因为占用而丢日志；
// 清理工具本身操作不频繁，这点开销无关紧要。
void Append(const std::wstring& text) {
    const std::wstring path = LogFilePath();
    HANDLE file = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    // 统一写 UTF-8：Windows 记事本、VS Code、git diff 都能正确显示中文
    std::string utf8;
    if (!text.empty()) {
        const int need = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
                                               nullptr, nullptr);
        utf8.resize(static_cast<std::size_t>(need));
        ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &utf8[0], need, nullptr,
                              nullptr);
    }
    utf8 += "\r\n";

    DWORD written = 0;
    ::WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    ::CloseHandle(file);
}

// 未处理异常过滤器：日志落盘后返回 EXCEPTION_EXECUTE_HANDLER，
// 系统随即按默认行为终止进程（不会把异常吞掉，也不会改变崩溃语义）。
LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info) {
    const DWORD code = (info && info->ExceptionRecord) ? info->ExceptionRecord->ExceptionCode : 0;
    const void* address = (info && info->ExceptionRecord) ? info->ExceptionRecord->ExceptionAddress : nullptr;

    wchar_t buf[256] = {0};
    ::swprintf_s(buf, L"!! 未处理异常 code=0x%08X 地址=%p 线程=%lu（进程已终止）",
                 static_cast<unsigned>(code), address, ::GetCurrentThreadId());
    Append(buf);
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

std::wstring LogFilePath() {
    wchar_t dir[MAX_PATH + 1] = {0};
    const DWORD len = ::GetTempPathW(MAX_PATH, dir);
    if (len == 0 || len > MAX_PATH) return L"garbage_cleaner_gui.log";

    std::wstring path(dir);
    if (path.empty() || path.back() != L'\\') path += L'\\';
    path += L"garbage_cleaner_gui.log";
    return path;
}

void LogLine(const std::wstring& text) {
    SYSTEMTIME now = {};
    ::GetLocalTime(&now);

    wchar_t stamp[64] = {0};
    ::swprintf_s(stamp, L"[%04u-%02u-%02u %02u:%02u:%02u] ", static_cast<unsigned>(now.wYear),
                 static_cast<unsigned>(now.wMonth), static_cast<unsigned>(now.wDay), static_cast<unsigned>(now.wHour),
                 static_cast<unsigned>(now.wMinute), static_cast<unsigned>(now.wSecond));
    Append(std::wstring(stamp) + text);
}

void InstallCrashLogger() {
    ::SetUnhandledExceptionFilter(OnUnhandledException);
}

} // namespace gc
