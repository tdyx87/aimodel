#pragma once

// 垃圾文件扫描器 —— 纯 C++17 + Win32，不依赖界面库。
// 这样扫描逻辑可以单独测试，也便于以后换掉界面层。

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gc {

// 类别名（也是界面上显示的文案）
extern const wchar_t* const kCategoryBuild;   // 构建产物
extern const wchar_t* const kCategoryTemp;    // 临时缓存
extern const wchar_t* const kCategoryLog;     // 日志文件

struct ScanItem {
    std::wstring path;      // 完整路径
    std::uint64_t size = 0; // 字节
    std::wstring category;  // kCategoryXxx
};

struct ScanProgress {
    std::uint64_t dirs = 0;
    std::uint64_t files = 0;
    std::uint64_t matched = 0;
    std::uint64_t bytes = 0;
};

struct ScanResult {
    std::vector<ScanItem> items;
    std::uint64_t total_bytes = 0;
    std::uint64_t dirs = 0;
    std::uint64_t files = 0;
    std::uint64_t skipped_protected = 0;
    bool truncated = false; // 命中条目达到上限，结果不完整
    bool aborted = false;   // 用户中途取消
};

struct ScanOptions {
    std::wstring root;                 // 扫描起始目录
    int max_depth = 12;                // 递归深度上限，防止超深目录
    std::size_t max_items = 5000;      // 结果条数上限，防止内存与界面被撑爆
};

class Scanner {
public:
    using ProgressFn = std::function<void(const ScanProgress&)>;

    explicit Scanner(ScanOptions options);

    void SetProgress(ProgressFn fn) { progress_ = std::move(fn); }

    // 从其他线程请求中止；Scan() 会在下一个检查点返回，aborted 置位。
    void RequestAbort() { abort_.store(true, std::memory_order_relaxed); }

    ScanResult Scan();

    // 类别判定：只依据扩展名与少数固定文件名，绝不用「位于 build 目录」这类路径规则，
    // 避免把构建目录里用户自己放进去的源码也当成垃圾。
    static std::wstring CategoryOf(const std::wstring& name_lower, const std::wstring& path_lower);

    // 系统目录与用户数据目录的保护名单（传入小写路径）。
    static bool IsProtected(const std::wstring& path_lower);

    // 删除前的二次校验：受保护路径、必须在扫描根目录之内、目录本身不删。
    // root_lower 为空表示不校验根目录归属。
    static bool DeleteFileSafely(const std::wstring& path, const std::wstring& root_lower);

    static std::wstring FormatSize(std::uint64_t bytes);

private:
    void Walk(const std::wstring& dir, int depth, ScanResult& result, ScanProgress& progress);
    void ReportProgress(const ScanProgress& progress);

    ScanOptions options_;
    ProgressFn progress_;
    std::atomic<bool> abort_{false};
    std::uint64_t last_report_tick_ = 0;
};

} // namespace gc
