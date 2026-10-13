// 角色：cpp-implementer
// 产物：src/cpp/save_file_win32.cpp
// 契约：docs/design/save-format-contract.md §5（只读保证、不泄露路径）
//
// Windows 的窄字符文件 API 按**系统 ANSI 代码页**解释路径，简中环境为 GBK/936。
// 直接把 UTF-8 字节喂进去会出现两类错误：UTF-8 字节恰是合法 GBK 时路径乱码；
// 字节非合法 GBK 时直接打不开。因此路径一律转 UTF-16 后走 W 系列 API。

#if defined(_WIN32) || defined(_WIN64)

#define WIN32_LEAN_AND_MEAN
#include "save_file.h"

#include <wchar.h>

#include <windows.h>

namespace saveinspector {
namespace {

// UTF-8 → UTF-16（Win32 宽字符 API 所需）。
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                           static_cast<int>(s.size()), NULL, 0);
    if (size <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                          &out[0], size);
    return out;
}

// 区分「文件不存在」与「其他 IO 失败」：前者是用户最常见的情况，文案要具体。
std::string ErrorFor(DWORD code) {
    switch (code) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return "找不到这个存档文件。请确认路径是否正确，或文件是否已被移动。";
        case ERROR_ACCESS_DENIED:
            return "没有权限读取这个文件。请确认文件没有被其他程序占用。";
        case ERROR_SHARING_VIOLATION:
            return "这个文件正被游戏占用。请先退出游戏再试。";
        default:
            return "读取存档文件失败。请确认文件是否完整。";
    }
}

}  // namespace

bool ReadFileBytesReadOnly(const std::string& path, std::string* out,
                           std::string* error, size_t max_bytes) {
    if (out == NULL || path.empty()) {
        if (error != NULL) *error = "请先选择一个存档文件。";
        return false;
    }
    out->clear();

    const std::wstring wpath = Utf8ToWide(path);
    if (wpath.empty()) {
        if (error != NULL) *error = "文件路径无效，无法读取。";
        return false;
    }

    // GENERIC_READ + FILE_SHARE_READ：只申请读权限，且允许其他程序同时读。
    // 没有任何写入标志 —— 这是只读保证在代码层的落点。
    HANDLE h = ::CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                             NULL, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                             NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (error != NULL) *error = ErrorFor(::GetLastError());
        return false;
    }

    LARGE_INTEGER size_info;
    if (!::GetFileSizeEx(h, &size_info)) {
        ::CloseHandle(h);
        if (error != NULL) *error = "无法获取存档文件大小。";
        return false;
    }
    const LONGLONG total = size_info.QuadPart;
    if (total <= 0) {
        ::CloseHandle(h);
        if (error != NULL) *error = "这个存档文件是空的，可能没有保存成功。";
        return false;
    }
    if (static_cast<unsigned long long>(total) >
            static_cast<unsigned long long>(max_bytes)) {
        ::CloseHandle(h);
        if (error != NULL) *error = "这个存档文件太大，无法打开。";
        return false;
    }

    std::string data;
    data.reserve(static_cast<size_t>(total));
    const DWORD kChunk = 64u * 1024u;
    char buf[65536];
    for (;;) {
        DWORD want = static_cast<DWORD>(
            (static_cast<size_t>(total) - data.size()) < kChunk
                ? (static_cast<size_t>(total) - data.size())
                : kChunk);
        if (want == 0) break;
        DWORD got = 0;
        if (!::ReadFile(h, buf, want, &got, NULL)) {
            ::CloseHandle(h);
            if (error != NULL) *error = "读取存档文件时出错，文件可能已损坏。";
            return false;
        }
        if (got == 0) break;
        data.append(buf, got);
    }
    ::CloseHandle(h);

    if (data.empty()) {
        if (error != NULL) *error = "这个存档文件是空的，可能没有保存成功。";
        return false;
    }
    *out = data;
    return true;
}

}  // namespace saveinspector

#endif  // _WIN32 || _WIN64
