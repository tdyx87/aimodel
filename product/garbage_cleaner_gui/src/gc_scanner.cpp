#include "gc_scanner.h"

#include <windows.h>

#include <cwctype>

namespace gc {

const wchar_t* const kCategoryBuild = L"构建产物";
const wchar_t* const kCategoryTemp = L"临时缓存";
const wchar_t* const kCategoryLog = L"日志文件";

namespace {

std::wstring ToLower(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        out.push_back(static_cast<wchar_t>(::towlower(c)));
    }
    return out;
}

// 取扩展名（含点）；以点开头的文件名（如 .DS_Store）不算扩展名。
std::wstring ExtensionOf(const std::wstring& name_lower) {
    const std::size_t dot = name_lower.rfind(L'.');
    if (dot == std::wstring::npos || dot == 0) return std::wstring();
    return name_lower.substr(dot);
}

bool InSet(const std::wstring& value, const wchar_t* const* set, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (value == set[i]) return true;
    }
    return false;
}

template <std::size_t N>
bool InSet(const std::wstring& value, const wchar_t* const (&set)[N]) {
    return InSet(value, set, N);
}

} // namespace

Scanner::Scanner(ScanOptions options) : options_(std::move(options)) {}

std::wstring Scanner::CategoryOf(const std::wstring& name_lower, const std::wstring& path_lower) {
    (void)path_lower; // 刻意不使用路径规则，见头文件说明

    // 明确由编译器/构建系统生成的中间产物
    static const wchar_t* const kBuildExt[] = {
        L".obj", L".lib", L".pdb", L".ilk", L".exp", L".idb", L".ipdb", L".iobj",
        L".tlog", L".lastbuildstate", L".pch", L".gch", L".o", L".a", L".sbr", L".bsc",
    };
    // 临时文件与解释器缓存
    static const wchar_t* const kTempExt[] = {
        L".tmp", L".temp", L".cache", L".bak", L".old", L".orig", L".swp", L".pyc", L".pyo",
    };
    // 构建系统写在任何目录下都不该长期保留的清单文件
    static const wchar_t* const kBuildNames[] = {
        L"cmakecache.txt", L".ninja_deps", L".ninja_log", L"compile_commands.json",
    };

    const std::wstring ext = ExtensionOf(name_lower);
    if (!ext.empty()) {
        if (InSet(ext, kBuildExt)) return kCategoryBuild;
        if (InSet(ext, kTempExt)) return kCategoryTemp;
        if (ext == L".log") return kCategoryLog;
    }

    if (InSet(name_lower, kBuildNames)) return kCategoryBuild;
    if (name_lower == L".ds_store") return kCategoryTemp;
    // Office 临时锁文件，正常退出后会自己消失，残留下来就可以清掉
    if (name_lower.size() > 2 && name_lower[0] == L'~' && name_lower[1] == L'$') return kCategoryTemp;

    return std::wstring();
}

bool Scanner::IsProtected(const std::wstring& path_lower) {
    static const wchar_t* const kProtected[] = {
        L"\\windows\\",
        L"\\program files\\",
        L"\\program files (x86)\\",
        L"\\programdata\\",
        L"\\$recycle.bin\\",
        L"\\system volume information\\",
        L"\\recovery\\",
        L"\\boot\\",
        L"\\system32\\",
        L"\\winsxs\\",
        L"\\appdata\\local\\microsoft\\windows\\",
        L"\\appdata\\roaming\\microsoft\\",
    };

    for (const wchar_t* const marker : kProtected) {
        if (path_lower.find(marker) != std::wstring::npos) return true;
    }
    return false;
}

bool Scanner::DeleteFileSafely(const std::wstring& path, const std::wstring& root_lower) {
    if (path.empty()) return false;

    const std::wstring lower = ToLower(path);
    if (IsProtected(lower)) return false; // 二次校验，防止界面传入意外路径
    if (!root_lower.empty() && lower.compare(0, root_lower.size(), root_lower) != 0) return false;

    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) return false; // 只删文件，不删目录
    if ((attrs & FILE_ATTRIBUTE_READONLY) != 0) {
        ::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    }
    return ::DeleteFileW(path.c_str()) != FALSE;
}

std::wstring Scanner::FormatSize(std::uint64_t bytes) {
    static const wchar_t* const kUnits[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    wchar_t buf[64] = {0};

    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }

    if (unit == 0) {
        ::swprintf_s(buf, L"%llu %s", static_cast<unsigned long long>(bytes), kUnits[0]);
    } else {
        ::swprintf_s(buf, L"%.1f %s", value, kUnits[unit]);
    }
    return buf;
}

void Scanner::ReportProgress(const ScanProgress& progress) {
    if (!progress_) return;
    const std::uint64_t now = ::GetTickCount64();
    // 扫描大目录时文件可能很密，这里限流到 100ms 一次，避免刷爆界面消息队列
    if (last_report_tick_ != 0 && now - last_report_tick_ < 100) return;
    last_report_tick_ = now;
    progress_(progress);
}

void Scanner::Walk(const std::wstring& dir, int depth, ScanResult& result, ScanProgress& progress) {
    if (abort_.load(std::memory_order_relaxed)) return;

    std::wstring pattern = dir;
    if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') pattern += L'\\';
    pattern += L'*';

    WIN32_FIND_DATAW fd = {};
    HANDLE handle = ::FindFirstFileW(pattern.c_str(), &fd);
    if (handle == INVALID_HANDLE_VALUE) return;

    std::vector<std::wstring> sub_dirs;
    do {
        if (abort_.load(std::memory_order_relaxed)) break;

        const std::wstring name(fd.cFileName);
        if (name == L"." || name == L"..") continue;

        std::wstring full = dir;
        if (!full.empty() && full.back() != L'\\' && full.back() != L'/') full += L'\\';
        full += name;
        const std::wstring full_lower = ToLower(full);

        // 不跟随符号链接与目录联接点，避免死循环和跨盘越界
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;

        if (IsProtected(full_lower)) {
            ++result.skipped_protected;
            continue;
        }

        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            sub_dirs.push_back(full);
            continue;
        }

        ++progress.files;

        const std::wstring category = CategoryOf(ToLower(name), full_lower);
        if (category.empty()) continue;

        const std::uint64_t size =
            (static_cast<std::uint64_t>(fd.nFileSizeHigh) << 32) | static_cast<std::uint64_t>(fd.nFileSizeLow);

        if (result.items.size() >= options_.max_items) {
            result.truncated = true;
            continue;
        }

        result.items.push_back(ScanItem{full, size, category});
        result.total_bytes += size;
        ++progress.matched;
        progress.bytes = result.total_bytes;
        ReportProgress(progress);
    } while (::FindNextFileW(handle, &fd));

    ::FindClose(handle);

    ++progress.dirs;
    ReportProgress(progress);

    if (depth >= options_.max_depth) return; // 到深度上限就不再往里走
    for (const std::wstring& sub : sub_dirs) {
        if (abort_.load(std::memory_order_relaxed)) break;
        Walk(sub, depth + 1, result, progress);
    }
}

ScanResult Scanner::Scan() {
    ScanResult result;
    ScanProgress progress;
    abort_.store(false, std::memory_order_relaxed);
    last_report_tick_ = 0;

    std::wstring root = options_.root;
    // 保留 "C:\" 这种三字符根，其余情况去掉末尾分隔符，便于和子路径做前缀比较
    while (root.size() > 3 && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();

    if (!root.empty()) {
        Walk(root, 0, result, progress);
    }

    result.dirs = progress.dirs;
    result.files = progress.files;
    result.aborted = abort_.load(std::memory_order_relaxed);

    if (progress_) progress_(progress); // 收尾时无条件报一次最终进度

    return result;
}

} // namespace gc
