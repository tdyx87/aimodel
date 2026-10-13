// 角色：cpp-implementer
// 任务：实现 file_transfer 后端
// 产物：src/cpp/file_transfer.cpp
// 契约：docs/design/file-transfer-contract.md（审计 2026-10-12-architect-file-transfer.md）
//
// C++11 合规说明（agents.md 第 6 节）：本项目锁定 C++11，不得使用 <filesystem>
// （C++17）。目录创建、枚举、删除一律走 POSIX/Win32 既有 API
// （stat/mkdir/opendir/remove），与 system_info_posix.cpp 的 <dirent.h> 用法一致。

#include "aimodel/file_transfer.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#if defined(_WIN32) || defined(_WIN64)
#  include <direct.h>
#  include <wchar.h>
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dirent.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif
// MSVC 与 MinGW-w64 均提供 <sys/stat.h>：Windows 侧用 _stat，POSIX 侧用 stat。
// （Windows 上不能写 `struct stat st; ::stat(...)`——全局命名空间里 `stat` 既是
// 结构体标签又可能是宏，`::stat(...)` 会被解析成类型而非函数。）
#include <sys/stat.h>

namespace aimodel {

// 契约 6.4：默认上限 10 MB。
const size_t FileTransfer::kMaxFileSize = 10u * 1024u * 1024u;
const int FileTransfer::kMaxRenames = 1000;

FileTransfer::FileTransfer() : FileTransfer("data/uploads") {}

FileTransfer::FileTransfer(const std::string& uploads_root)
    : uploads_root_(uploads_root.empty() ? "data/uploads" : uploads_root) {}

namespace {

#if defined(_WIN32) || defined(_WIN64)

// UTF-8（本库对外统一编码）→ UTF-16（Win32 宽字符 API 所需）。
//
// 为什么必须转换：Windows 的 ANSI 文件 API（CreateFileA / _mkdir / _stat /
// _findfirst）按**系统代码页**（简中为 GBK/936）解释路径。直接把 UTF-8 字节喂进去，
// 会出现两类错误：
//   1) UTF-8 字节恰是合法 GBK → 落盘文件名乱码（如「硬件报告.txt」→「纭欢鎶ュ憡.txt」）；
//   2) UTF-8 字节不是合法 GBK → 创建直接失败（中文名上传报 WRITE_FAILED）。
// 故路径一律经本函数转宽字符，改走 W 系列 API。
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

// UTF-16 → UTF-8（列目录时把文件名转回对外编码）。
std::string WideToUtf8Local(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                           static_cast<int>(w.size()), NULL, 0,
                                           NULL, NULL);
    if (size <= 0) return std::string();
    std::string out(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                          &out[0], size, NULL, NULL);
    return out;
}

#endif  // _WIN32

bool IsPathSeparator(char c) {
#if defined(_WIN32) || defined(_WIN64)
    return c == '\\' || c == '/';
#else
    return c == '/';
#endif
}

// 可接受的文件名字符。中文等 UTF-8 多字节字符逐字节均 >= 0x80，
// 而 isprint 在 "C" locale 下对这些字节为假，故单独放行，避免误伤中文文件名；
// ASCII 区间则排除控制字符。
bool IsSafeFilenameChar(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u >= 0x80u) return true;
    if (u < 0x20u || u == 0x7fu) return false;
    return std::isprint(u) != 0;
}

// Windows 文件名非法字符（< > : " | ? * 与控制字符）。
bool IsWindowsIllegalChar(char c) {
    switch (c) {
        case '<': case '>': case ':': case '"': case '|':
        case '?': case '*':
            return true;
        default:
            return static_cast<unsigned char>(c) < 0x20u;
    }
}

// 按路径分隔符切分：丢弃空段与 "." 段，保留 ".." 段供上层判定穿越。
std::vector<std::string> SplitNormalizedSegments(const std::string& path) {
    std::vector<std::string> parts;
    std::string current;
    for (size_t i = 0; i <= path.size(); ++i) {
        const char c = (i < path.size()) ? path[i] : '/';
        if (IsPathSeparator(c)) {
            if (current == ".." || (!current.empty() && current != ".")) {
                parts.push_back(current);
            }
            current.clear();
        } else {
            current += c;
        }
    }
    return parts;
}

// 拼接 root 与 name，统一以 '/' 作分隔符，并去掉 root 末尾分隔符。
std::string JoinPath(const std::string& root, const std::string& name) {
    std::string out = root;
    while (!out.empty() && IsPathSeparator(out[out.size() - 1])) {
        out.erase(out.size() - 1);
    }
    if (!out.empty()) out += "/";
    out += name;
    return out;
}

// 平台封装：存在性判断与目录判断。
// Windows 走 GetFileAttributesW（UTF-16 路径），避免 ANSI 代码页误判中文路径；
// POSIX 用 stat（路径本就是 UTF-8 字节流）。
bool PathExists(const std::string& path) {
#if defined(_WIN32) || defined(_WIN64)
    const std::wstring w = Utf8ToWide(path);
    return !w.empty() &&
           ::GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
#endif
}

bool IsDirectory(const std::string& path) {
#if defined(_WIN32) || defined(_WIN64)
    const std::wstring w = Utf8ToWide(path);
    if (w.empty()) return false;
    const DWORD attr = ::GetFileAttributesW(w.c_str());
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
    return (st.st_mode & S_IFMT) == S_IFDIR;
#endif
}

// 单层建目录。
bool MakeSingleDirectory(const std::string& path) {
#if defined(_WIN32) || defined(_WIN64)
    const std::wstring w = Utf8ToWide(path);
    return !w.empty() && ::_wmkdir(w.c_str()) == 0;
#else
    return ::mkdir(path.c_str(), 0755) == 0;
#endif
}

// 以二进制只读打开文件（Windows 走宽字符 API，避免中文路径被按 ANSI 解释）。
std::FILE* OpenReadBinary(const std::string& path) {
#if defined(_WIN32) || defined(_WIN64)
    const std::wstring w = Utf8ToWide(path);
    if (w.empty()) return NULL;
#  if defined(_MSC_VER)
    // MSVC：_wfopen 在 UCRT 下已弃用，/W4 会报 C4996（本项目零警告门禁）。
    std::FILE* fp = NULL;
    return ::_wfopen_s(&fp, w.c_str(), L"rb") == 0 ? fp : NULL;
#  else
    // MinGW-w64 无 _wfopen_s，退回 _wfopen（该编译器不报 C4996）。
    return ::_wfopen(w.c_str(), L"rb");
#  endif
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

// 删除文件（Windows 走宽字符 API）。
int RemoveFile(const std::string& path) {
#if defined(_WIN32) || defined(_WIN64)
    const std::wstring w = Utf8ToWide(path);
    return w.empty() ? -1 : ::_wremove(w.c_str());
#else
    return std::remove(path.c_str());
#endif
}

// 以「排他创建」方式打开文件并写入 content。
// 用 O_EXCL（POSIX）/ CREATE_NEW（Win32）保证：文件已存在则创建失败，
// 从内核层面消除「stat 判空位 → ofstream 打开」之间的 TOCTOU 竞态（CWE-367）。
// 返回：0 = 成功写入；1 = 目标已存在（调用方可换名重试）；-1 = 其他错误。
int CreateFileExclusiveAndWrite(const std::string& path,
                                const std::string& content) {
#if defined(_WIN32) || defined(_WIN64)
    // 宽字符路径（CreateFileW）：中文文件名若走 CreateFileA 会按 ANSI 代码页解释
    // UTF-8 字节，导致乱码或创建失败（WRITE_FAILED）。
    const std::wstring wpath = Utf8ToWide(path);
    if (wpath.empty()) return -1;
    HANDLE h = ::CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = ::GetLastError();
        return (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS) ? 1 : -1;
    }
    DWORD written = 0;
    bool ok = true;
    if (!content.empty()) {
        ok = ::WriteFile(h, content.data(), static_cast<DWORD>(content.size()),
                         &written, NULL) != 0 &&
             written == content.size();
    }
    // 落盘后刷出，语义对齐 POSIX 分支的 fsync（避免「返回成功但数据仍在页缓存」）。
    if (ok && !content.empty() && !::FlushFileBuffers(h)) {
        ok = false;
    }
    ::CloseHandle(h);
    if (!ok) {
        // 写坏的文件不留残file（与 POSIX 分支的 unlink 对齐）。
        ::DeleteFileW(wpath.c_str());
        return -1;
    }
    return 0;
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        return (errno == EEXIST) ? 1 : -1;
    }
    size_t off = 0;
    bool ok = true;
    while (off < content.size()) {
        const ssize_t n = ::write(fd, content.data() + off, content.size() - off);
        if (n <= 0) {
            ok = false;
            break;
        }
        off += static_cast<size_t>(n);
    }
    // fsync 后再 close，避免「返回成功但数据仍在页缓存」的断电丢数据错觉。
    // 但**不能**把 fsync 失败一律判为写失败：部分文件系统（若干 FUSE / 网络挂载）
    // 不支持 fsync 并返回 EINVAL/ENOTSUP，那会让每次上传都 WRITE_FAILED 且被删。
    if (ok && content.size() > 0 && ::fsync(fd) != 0) {
        const int err = errno;
        if (err != EINVAL && err != ENOTSUP && err != EROFS && err != ENOSYS) {
            ok = false;
        }
    }
    ::close(fd);
    if (!ok) {
        ::unlink(path.c_str());  // 写坏的文件不留残file
    }
    return ok ? 0 : -1;
#endif
}

std::string ToUpper(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        out.push_back(static_cast<char>(
            std::toupper(static_cast<unsigned char>(s[i]))));
    }
    return out;
}

// 去掉首尾空白（空格/制表符等）。Windows 落盘时会剥掉文件名首尾的
// 空格与点，故扩展名比对前必须 trim，否则 "a.exe " 会绕过危险扩展名名单。
std::string TrimWhitespace(const std::string& s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end &&
           std::isspace(static_cast<unsigned char>(s[begin])) != 0) {
        ++begin;
    }
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
        --end;
    }
    return s.substr(begin, end - begin);
}

// 取最后一个 '.' 之后的部分作扩展名（不含点），并 trim 空白；无点返回空串。
std::string ExtensionOf(const std::string& filename) {
    const size_t pos = filename.find_last_of('.');
    if (pos == std::string::npos) return std::string();
    return TrimWhitespace(filename.substr(pos + 1));
}

// 名字是否携带路径结构（分隔符，或 ".." 段）。
// 上传接口只接受「单个文件名」，任何路径成分都应整体拒绝而非清洗后放行。
bool ContainsPathStructure(const std::string& filename) {
    if (filename.find("..") != std::string::npos) return true;
    for (size_t i = 0; i < filename.size(); ++i) {
        if (IsPathSeparator(filename[i])) return true;
    }
    return false;
}

// 路径的「根前缀」：Windows 盘符（"C:"）或 UNC 前缀（"\\\\"）、POSIX 前导 "/"。
// 它是路径的组成部分而非可创建的目录，必须在逐段 mkdir 之前先接上。
//
// 为什么必须单独处理：SplitNormalizedSegments 会丢弃前导分隔符产生的空段，
// 于是 "/data/uploads" 的分段结果是 ["data", "uploads"]——与相对路径
// "data/uploads" 完全同形。若据此逐段 mkdir，"/data/uploads" 会在**当前工作
// 目录**下建出 data/uploads，落盘位置与授权目录不符（路径白名单仍会通过，
// 因为它同样按段比较，但文件落在了意外的位置）。
std::string RootPrefixOf(const std::string& path) {
    if (path.empty()) {
        return std::string();
    }
#if defined(_WIN32) || defined(_WIN64)
    // 盘符形式："C:" 或 "C:/..." / "C:\..."
    if (path.size() >= 2 && path[1] == ':' &&
            std::isalpha(static_cast<unsigned char>(path[0]))) {
        return path.substr(0, 2);
    }
    // UNC 前缀 "\\\\server\\share"：首段本身已是真实目录，交由后续 mkdir 处理，
    // 这里只保证拼接时不丢掉前导两个反斜杠。
    if (path.size() >= 2 && path[0] == '\\' && path[1] == '\\') {
        return "\\\\";
    }
#else
    if (path[0] == '/') {
        return "/";
    }
#endif
    return std::string();
}

}  // namespace

// ---------------------------------------------------------------------------
// 文件名安全校验（契约 6.3）
// ---------------------------------------------------------------------------

bool FileTransfer::IsDangerousExtension(const std::string& filename) const {
    const std::string ext = ToUpper(ExtensionOf(filename));
    if (ext.empty()) return false;
    // 可执行/脚本类：上传后若被用户误运行，风险最高。
    static const char* kDangerous[] = {
        "EXE", "DLL", "BAT", "CMD", "COM", "SCR", "MSI", "MSC",
        "PS1", "PSM1", "VBS", "VBE", "JS", "JSE", "WSF", "WSH",
        "SH", "BASH", "ZSH", "CPL", "HTA", "REG", "LNK", "PIF", "JAR"
    };
    for (size_t i = 0; i < sizeof(kDangerous) / sizeof(kDangerous[0]); ++i) {
        if (ext == kDangerous[i]) return true;
    }
    return false;
}

bool FileTransfer::IsReservedBaseName(const std::string& upper_base) const {
    static const char* kReserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };
    for (size_t i = 0; i < sizeof(kReserved) / sizeof(kReserved[0]); ++i) {
        if (upper_base == kReserved[i]) return true;
    }
    return false;
}

bool FileTransfer::IsSafeFilename(const std::string& filename) const {
    if (filename.empty()) return false;
    if (filename.size() > kMaxUploadFilenameLength) return false;

    // 点文件（隐藏文件）与尾点（Windows 会剥掉尾点导致歧义）一律拒绝。
    if (filename[0] == '.') return false;
    if (filename[filename.size() - 1] == '.') return false;

    // 首尾空白同样拒绝：Windows 落盘会剥掉，"a.exe " 与 "a.exe" 等价，
    // 若此处放行，落盘名与校验名就不一致（CWE-178 大小写/空白规范化不当）。
    if (std::isspace(static_cast<unsigned char>(filename[0])) != 0) return false;
    if (std::isspace(static_cast<unsigned char>(
            filename[filename.size() - 1])) != 0) {
        return false;
    }

    for (size_t i = 0; i < filename.size(); ++i) {
        const char c = filename[i];
        if (c == '\0') return false;
        if (IsPathSeparator(c)) return false;
        if (IsWindowsIllegalChar(c)) return false;
        if (!IsSafeFilenameChar(c)) return false;
    }

    // Windows 保留名需比对主名（CON.txt 同样非法）。
    const size_t dot = filename.find_first_of('.');
    const std::string base =
        ToUpper((dot == std::string::npos) ? filename : filename.substr(0, dot));
    if (IsReservedBaseName(base)) return false;

    // 危险扩展名（契约 6.3 第 4 条）。
    if (IsDangerousExtension(filename)) return false;

    return true;
}

std::string FileTransfer::MakeSafeFilename(const std::string& filename) {
    std::string safe;
    safe.reserve(filename.size());

    for (size_t i = 0; i < filename.size(); ++i) {
        const char c = filename[i];
        if (c == '\0') continue;
        if (IsPathSeparator(c)) continue;
        if (IsWindowsIllegalChar(c)) continue;
        if (!IsSafeFilenameChar(c)) continue;
        safe.push_back(c);
    }

    // 去掉首尾点与首尾空白，避免生成隐藏文件、尾点名，
    // 或 "a.exe " 这类落盘后被 Windows 剥成 a.exe 的名字。
    while (!safe.empty() && safe[0] == '.') safe.erase(0, 1);
    while (!safe.empty() && safe[safe.size() - 1] == '.') {
        safe.erase(safe.size() - 1);
    }
    safe = TrimWhitespace(safe);
    while (!safe.empty() && safe[safe.size() - 1] == '.') {
        safe.erase(safe.size() - 1);
    }
    if (safe.empty()) safe = "upload.bin";
    if (safe.size() > kMaxUploadFilenameLength) {
        safe.resize(kMaxUploadFilenameLength);
    }
    return safe;
}

// 在扩展名之前插入「时间戳 + 序号」的变体名，用于撞名重试。
std::string FileTransfer::MakeStampVariant(const std::string& filename,
                                           int index) {
    const std::time_t now = std::time(nullptr);
    struct tm tm_info;
#if defined(_WIN32) || defined(_WIN64)
    localtime_s(&tm_info, &now);
#else
    localtime_r(&now, &tm_info);
#endif
    char stamp[64];
    std::snprintf(stamp, sizeof(stamp), ".%04d%02d%02d_%02d%02d%02d_%d",
                  tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                  tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec, index);

    const std::string ext = ExtensionOf(filename);
    const size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos || ext.empty()) {
        return filename + stamp;
    }
    return filename.substr(0, dot) + stamp + "." + ext;
}

bool FileTransfer::IsAllowedPath(const std::string& full_path,
                                 const std::string& uploads_root) {
    if (uploads_root.empty() || full_path.empty()) return false;

    // 注意：full_path 由调用方用 JoinPath(root, name) 构造，root 自身可以是
    // 绝对路径（含 Windows 盘符），故此处**不能**再按「绝对路径一律拒绝」处理——
    // 那会把合法的 F:/.../uploads/a.txt 也拒掉。绝对路径检查属于 IsSafeFilename
    // 对**用户输入名**的职责，不在此重复。
    const std::vector<std::string> target =
        SplitNormalizedSegments(full_path);
    const std::vector<std::string> root =
        SplitNormalizedSegments(uploads_root);

    // 目标必须严格位于 root 内部（段数更多且前缀逐段相同）。
    if (target.size() <= root.size()) return false;
    for (size_t i = 0; i < root.size(); ++i) {
        if (target[i] != root[i]) return false;
    }
    for (size_t i = 0; i < target.size(); ++i) {
        if (target[i] == "..") return false;
    }
    return true;
}

bool FileTransfer::EnsureDirectory(const std::string& path, std::string* error) {
    if (path.empty()) {
        if (error != nullptr) *error = "上传根目录未配置。";
        return false;
    }
    if (IsDirectory(path)) return true;

    // 逐段创建，替代 C++17 的 std::filesystem::create_directories。
    //
    // 两个平台细节必须同时照顾到：
    //   - POSIX 前导 "/" 是路径前缀而非可创建目录，且分段时被丢弃
    //     （"/a/b" → ["a","b"]），不补回就会在**当前工作目录**下建出 a/b；
    //   - Windows 盘符 "C:" 因 ':' 不是路径分隔符，会原样留在首段里
    //     （"C:/a" → ["C:","a"]），已由 RootPrefixOf 接住，不能再拼一次
    //     （否则得到 "C:/C:" 这种非法路径）。
    const std::vector<std::string> parts = SplitNormalizedSegments(path);
    std::string acc = RootPrefixOf(path);
    size_t start = 0;
    if (!acc.empty() && !parts.empty() && parts[0] == acc) {
        start = 1;  // 盘符段：它已在 acc 里，跳过不重复拼接
    }
    for (size_t i = start; i < parts.size(); ++i) {
        if (parts[i] == "..") {
            if (error != nullptr) *error = "上传根目录不可用。";
            return false;
        }
        if (!acc.empty() && !IsPathSeparator(acc[acc.size() - 1])) {
            acc += "/";
        }
        acc += parts[i];
        if (IsDirectory(acc)) continue;
        if (!MakeSingleDirectory(acc) && !IsDirectory(acc)) {
            if (error != nullptr) *error = "创建上传目录失败。";
            return false;
        }
    }
    if (acc.empty() || !IsDirectory(acc)) {
        if (error != nullptr) *error = "上传根目录不可用。";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 公开接口（契约第 4 节）
// ---------------------------------------------------------------------------

bool FileTransfer::AppendUpload(const std::string& filename,
                                const std::string& content,
                                std::string* error,
                                std::string* saved_name,
                                FileError* code) {
    if (saved_name != nullptr) saved_name->clear();
    if (code != nullptr) *code = FileError::kNone;

    if (filename.empty()) {
        if (error != nullptr) *error = "文件名不能为空。";
        if (code != nullptr) *code = FileError::kInvalidName;
        return false;
    }

    // 大小校验先于任何磁盘操作（契约 6.4）。
    if (content.size() > kMaxFileSize) {
        if (error != nullptr) *error = "文件过大，请选择小于 10 MB 的文件。";
        if (code != nullptr) *code = FileError::kTooLarge;
        return false;
    }

    if (filename.size() > kMaxUploadFilenameLength) {
        if (error != nullptr) *error = "文件名过长。";
        if (code != nullptr) *code = FileError::kInvalidName;
        return false;
    }

    // 危险扩展名直接拒绝，不做清洗放行（契约 6.3 第 4 条）。
    if (IsDangerousExtension(filename)) {
        if (error != nullptr) *error = "该文件类型不允许上传。";
        if (code != nullptr) *code = FileError::kInvalidName;
        return false;
    }

    // 名字里出现路径分隔符或 ".." 段，说明调用方想指定子路径——直接拒绝，
    // 不做「剥掉分隔符」式清洗（那会把 ../escape.txt 变成合法的 ..escape.txt，
    // 语义上等于接受了路径输入）。契约 6.2 第 1、2、6 条。
    if (ContainsPathStructure(filename)) {
        if (error != nullptr) *error = "文件名不能包含路径。";
        if (code != nullptr) *code = FileError::kPathNotAllowed;
        return false;
    }

    // 点文件、尾点与首尾空白同样整体拒绝：MakeSafeFilename 会把 .hidden 清洗成
    // 合法的 "hidden"，若在此之后才校验，就相当于接受了隐藏文件；Windows 落盘
    // 又会剥掉首尾空白，导致落盘名与校验名不一致。契约 6.3 第 3 条。
    if (filename[0] == '.' || filename[filename.size() - 1] == '.' ||
            std::isspace(static_cast<unsigned char>(filename[0])) != 0 ||
            std::isspace(static_cast<unsigned char>(
                filename[filename.size() - 1])) != 0) {
        if (error != nullptr) *error = "文件名不合法。";
        if (code != nullptr) *code = FileError::kInvalidName;
        return false;
    }

    if (!EnsureDirectory(uploads_root_, error)) {
        if (code != nullptr) *code = FileError::kWriteFailed;
        return false;
    }

    // 清洗 → 校验：清洗结果仍须通过安全校验，否则说明原名整体不可接受。
    const std::string safe = MakeSafeFilename(filename);
    if (!IsSafeFilename(safe)) {
        if (error != nullptr) *error = "文件名不合法。";
        if (code != nullptr) *code = FileError::kInvalidName;
        return false;
    }

    // 排他创建 + 撞名重试：O_EXCL/CREATE_NEW 在内核层保证「不存在才创建」，
    // 因此这里不再有 stat 判空位与写盘之间的 TOCTOU 窗口（CWE-367）。
    // 撞名（返回 1）时追加时间戳序号换名再试，最多 kMaxRenames 次。
    std::string target;
    bool wrote = false;
    for (int attempt = 0; attempt < kMaxRenames && !wrote; ++attempt) {
        target = (attempt == 0) ? safe : MakeStampVariant(safe, attempt);
        const std::string full_path = JoinPath(uploads_root_, target);

        // 纵深防御：落盘前仍确认路径在白名单内。
        if (!IsAllowedPath(full_path, uploads_root_)) {
            if (error != nullptr) *error = "文件路径不在允许目录内。";
            if (code != nullptr) *code = FileError::kPathNotAllowed;
            return false;
        }

        const int rc = CreateFileExclusiveAndWrite(full_path, content);
        if (rc == 0) {
            wrote = true;
            break;
        }
        if (rc == 1) {
            continue;  // 撞名：换名重试
        }
        if (error != nullptr) *error = "文件写入失败。";
        if (code != nullptr) *code = FileError::kWriteFailed;
        return false;
    }

    if (!wrote) {
        if (error != nullptr) *error = "无法生成可用的文件名。";
        if (code != nullptr) *code = FileError::kWriteFailed;
        return false;
    }

    if (saved_name != nullptr) *saved_name = target;
    return true;
}

bool FileTransfer::ListUploads(std::string* list,
                               std::string* error,
                               FileError* code) {
    if (code != nullptr) *code = FileError::kNone;
    if (list != nullptr) list->clear();

    if (list == nullptr) {
        if (error != nullptr) *error = "参数无效。";
        if (code != nullptr) *code = FileError::kInternal;
        return false;
    }
    // 目录尚未创建视为空列表（上传前不强制建目录）。
    if (uploads_root_.empty() || !IsDirectory(uploads_root_)) {
        return true;
    }

    std::vector<std::string> files;
#if defined(_WIN32) || defined(_WIN64)
    // 宽字符枚举（FindFirstFileW）：中文文件名经 ANSI 版 _findfirst 会乱码。
    const std::wstring pattern = Utf8ToWide(JoinPath(uploads_root_, "*"));
    if (pattern.empty()) {
        if (error != nullptr) *error = "读取上传目录失败。";
        if (code != nullptr) *code = FileError::kInternal;
        return false;
    }
    WIN32_FIND_DATAW fd;
    HANDLE handle = ::FindFirstFileW(pattern.c_str(), &fd);
    if (handle == INVALID_HANDLE_VALUE) {
        // 目录存在但为空时 FindFirstFileW 报 ERROR_FILE_NOT_FOUND：视为空列表而非错误。
        if (::GetLastError() == ERROR_FILE_NOT_FOUND) return true;
        if (error != nullptr) *error = "读取上传目录失败。";
        if (code != nullptr) *code = FileError::kInternal;
        return false;
    }
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        files.push_back(WideToUtf8Local(fd.cFileName));
    } while (::FindNextFileW(handle, &fd) != 0);
    ::FindClose(handle);
#else
    DIR* dir = ::opendir(uploads_root_.c_str());
    if (dir == nullptr) {
        if (error != nullptr) *error = "读取上传目录失败。";
        if (code != nullptr) *code = FileError::kInternal;
        return false;
    }
    struct dirent* ent = nullptr;
    while ((ent = ::readdir(dir)) != nullptr) {
        if (std::strcmp(ent->d_name, ".") == 0 ||
            std::strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        const std::string full = JoinPath(uploads_root_, ent->d_name);
        if (!PathExists(full) || IsDirectory(full)) continue;
        files.push_back(ent->d_name);
    }
    ::closedir(dir);
#endif

    std::sort(files.begin(), files.end());

    for (size_t i = 0; i < files.size(); ++i) {
        if (i != 0) list->append("\n");
        list->append(files[i]);
    }
    return true;
}

bool FileTransfer::ReadUpload(const std::string& filename,
                              std::string* content,
                              std::string* error,
                              FileError* code) {
    if (code != nullptr) *code = FileError::kNone;

    if (filename.empty()) {
        if (error != nullptr) *error = "文件名不能为空。";
        if (code != nullptr) *code = FileError::kInvalidName;
        return false;
    }
    if (content == nullptr) {
        if (error != nullptr) *error = "参数无效。";
        if (code != nullptr) *code = FileError::kInternal;
        return false;
    }
    if (filename.size() > kMaxUploadFilenameLength) {
        if (error != nullptr) *error = "文件名过长。";
        if (code != nullptr) *code = FileError::kInvalidName;
        return false;
    }

    // 与上传侧一致：携带路径成分的名字整体拒绝（契约 6.2）。
    if (ContainsPathStructure(filename)) {
        if (error != nullptr) *error = "文件名不能包含路径。";
        if (code != nullptr) *code = FileError::kPathNotAllowed;
        return false;
    }

    const std::string safe = MakeSafeFilename(filename);
    const std::string full_path = JoinPath(uploads_root_, safe);

    if (!IsAllowedPath(full_path, uploads_root_)) {
        if (error != nullptr) *error = "文件路径不在允许目录内。";
        if (code != nullptr) *code = FileError::kPathNotAllowed;
        return false;
    }

    // 二进制读取（Windows 走 _wfopen，避免中文路径被按 ANSI 代码页解释而读不到）。
    std::FILE* fp = OpenReadBinary(full_path);
    if (fp == NULL) {
        if (error != nullptr) *error = "文件不存在。";
        if (code != nullptr) *code = FileError::kNotFound;
        return false;
    }
    std::string data;
    char buf[8192];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) {
        data.append(buf, n);
    }
    const bool read_error = std::ferror(fp) != 0;
    std::fclose(fp);
    if (read_error) {
        if (error != nullptr) *error = "读取文件失败。";
        if (code != nullptr) *code = FileError::kInternal;
        return false;
    }
    *content = data;
    return true;
}

bool FileTransfer::RemoveUpload(const std::string& filename,
                                std::string* error,
                                FileError* code) {
    if (code != nullptr) *code = FileError::kNone;

    if (filename.empty()) {
        if (error != nullptr) *error = "文件名不能为空。";
        if (code != nullptr) *code = FileError::kInvalidName;
        return false;
    }

    // 与上传/读取侧一致：携带路径成分的名字整体拒绝（契约 6.2）。
    if (ContainsPathStructure(filename)) {
        if (error != nullptr) *error = "文件名不能包含路径。";
        if (code != nullptr) *code = FileError::kPathNotAllowed;
        return false;
    }

    const std::string safe = MakeSafeFilename(filename);
    const std::string full_path = JoinPath(uploads_root_, safe);

    if (!IsAllowedPath(full_path, uploads_root_)) {
        if (error != nullptr) *error = "文件路径不在允许目录内。";
        if (code != nullptr) *code = FileError::kPathNotAllowed;
        return false;
    }

    if (!PathExists(full_path)) {
        if (error != nullptr) *error = "文件不存在。";
        if (code != nullptr) *code = FileError::kNotFound;
        return false;
    }

    if (RemoveFile(full_path) != 0) {
        if (error != nullptr) *error = "删除文件失败。";
        if (code != nullptr) *code = FileError::kWriteFailed;
        return false;
    }
    return true;
}

bool FileTransfer::GetUploadsRoot(std::string* root) {
    if (root == nullptr) return false;
    *root = uploads_root_;
    return true;
}

}  // namespace aimodel

