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
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32) || defined(_WIN64)
#  include <direct.h>
#  include <io.h>
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

// 平台封装：存在性判断与目录判断。Windows 用 _stat 避开 stat 名冲突
// （MinGW 会把 _stat 宏映射到 _stat64i32，故只写 _stat，由平台头自行解析）。
#if defined(_WIN32) || defined(_WIN64)
#  define AIMODEL_STAT(path, out) ::_stat((path), (out))
typedef struct _stat aimodel_stat_t;
#else
#  define AIMODEL_STAT(path, out) ::stat((path), (out))
typedef struct stat aimodel_stat_t;
#endif

bool PathExists(const std::string& path) {
    aimodel_stat_t st;
    return AIMODEL_STAT(path.c_str(), &st) == 0;
}

bool IsDirectory(const std::string& path) {
    aimodel_stat_t st;
    if (AIMODEL_STAT(path.c_str(), &st) != 0) return false;
    return (st.st_mode & S_IFMT) == S_IFDIR;
}

// 单层建目录。
bool MakeSingleDirectory(const std::string& path) {
#if defined(_WIN32) || defined(_WIN64)
    return ::_mkdir(path.c_str()) == 0;
#else
    return ::mkdir(path.c_str(), 0755) == 0;
#endif
}

// 以「排他创建」方式打开文件并写入 content。
// 用 O_EXCL（POSIX）/ CREATE_NEW（Win32）保证：文件已存在则创建失败，
// 从内核层面消除「stat 判空位 → ofstream 打开」之间的 TOCTOU 竞态（CWE-367）。
// 返回：0 = 成功写入；1 = 目标已存在（调用方可换名重试）；-1 = 其他错误。
int CreateFileExclusiveAndWrite(const std::string& path,
                                const std::string& content) {
#if defined(_WIN32) || defined(_WIN64)
    HANDLE h = ::CreateFileA(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW,
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
    ::CloseHandle(h);
    return ok ? 0 : -1;
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
    if (::fsync(fd) != 0) ok = false;
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

std::string FileTransfer::GetUniqueFilename(const std::string& filename) {
    // 唯一性主要由 AppendUpload 的排他创建（O_EXCL）保证；这里只做一次
    // 快速预检，让常见情况（无同名文件）不必走重试循环。
    if (!PathExists(JoinPath(uploads_root_, filename))) {
        return filename;
    }
    // 已存在：直接交给调用方从 attempt=1 开始生成变体。
    return filename;
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
    // 注意 Windows 盘符（"F:"）：SplitNormalizedSegments 会把它切成普通段 "F"，
    // 若从该段开始 _mkdir 会失败（"F" 不是目录）。故遇到盘符段时跳过，
    // 从 "F:/project" 这类真实目录段开始创建。
    const std::vector<std::string> parts = SplitNormalizedSegments(path);
    std::string acc;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (parts[i] == "..") {
            if (error != nullptr) *error = "上传根目录不可用。";
            return false;
        }
        // 盘符段（单字母，如 "F"）：仅作为路径前缀保留，不 mkdir。
        const bool is_drive =
            parts[i].size() == 2 && parts[i][1] == ':';
        if (i == 0) {
            acc = parts[i];
            if (is_drive) continue;  // 盘符本身不是可创建的目录
        } else {
            acc += "/";
            acc += parts[i];
        }
        if (IsDirectory(acc)) continue;
        if (!MakeSingleDirectory(acc) && !IsDirectory(acc)) {
            if (error != nullptr) *error = "创建上传目录失败。";
            return false;
        }
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
        if (code != nullptr) *code = FileError::kInternal;
        return false;
    }

    // 大小校验先于任何磁盘操作（契约 6.4）。
    if (content.size() > kMaxFileSize) {
        if (error != nullptr) *error = "文件过大，请选择小于 10 MB 的文件。";
        if (code != nullptr) *code = FileError::kTooLarge;
        return false;
    }

    // 危险扩展名直接拒绝，不做清洗放行（契约 6.3 第 4 条）。
    if (IsDangerousExtension(filename)) {
        if (error != nullptr) *error = "该文件类型不允许上传。";
        if (code != nullptr) *code = FileError::kInternal;
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
    if (!filename.empty() &&
        (filename[0] == '.' || filename[filename.size() - 1] == '.' ||
         std::isspace(static_cast<unsigned char>(filename[0])) != 0 ||
         std::isspace(static_cast<unsigned char>(
             filename[filename.size() - 1])) != 0)) {
        if (error != nullptr) *error = "文件名不合法。";
        if (code != nullptr) *code = FileError::kInternal;
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
        if (code != nullptr) *code = FileError::kInternal;
        return false;
    }

    const std::string base = GetUniqueFilename(safe);
    if (base.empty()) {
        if (error != nullptr) *error = "无法生成可用的文件名。";
        if (code != nullptr) *code = FileError::kInternal;
        return false;
    }

    // 排他创建 + 撞名重试：O_EXCL/CREATE_NEW 在内核层保证「不存在才创建」，
    // 因此这里不再有 stat 判空位与写盘之间的 TOCTOU 窗口（CWE-367）。
    // 撞名（返回 1）时追加时间戳序号换名再试，最多 kMaxRenames 次。
    std::string target;
    bool wrote = false;
    for (int attempt = 0; attempt < kMaxRenames && !wrote; ++attempt) {
        target = (attempt == 0) ? base : MakeStampVariant(base, attempt);
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
        if (code != nullptr) *code = FileError::kInternal;
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
    const std::string pattern = JoinPath(uploads_root_, "*");
    struct _finddata_t fd;
    const intptr_t handle = ::_findfirst(pattern.c_str(), &fd);
    if (handle == -1) {
        if (error != nullptr) *error = "读取上传目录失败。";
        if (code != nullptr) *code = FileError::kInternal;
        return false;
    }
    do {
        if ((fd.attrib & _A_SUBDIR) != 0) continue;
        if (std::strcmp(fd.name, ".") == 0 || std::strcmp(fd.name, "..") == 0) {
            continue;
        }
        files.push_back(fd.name);
    } while (::_findnext(handle, &fd) == 0);
    ::_findclose(handle);
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
        if (code != nullptr) *code = FileError::kInternal;
        return false;
    }
    if (content == nullptr) {
        if (error != nullptr) *error = "参数无效。";
        if (code != nullptr) *code = FileError::kInternal;
        return false;
    }
    if (filename.size() > kMaxUploadFilenameLength) {
        if (error != nullptr) *error = "文件名过长。";
        if (code != nullptr) *code = FileError::kInternal;
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

    std::ifstream in(full_path.c_str(), std::ios::binary);
    if (!in.is_open()) {
        if (error != nullptr) *error = "文件不存在。";
        if (code != nullptr) *code = FileError::kNotFound;
        return false;
    }

    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) {
        if (error != nullptr) *error = "读取文件失败。";
        if (code != nullptr) *code = FileError::kInternal;
        return false;
    }
    *content = ss.str();
    return true;
}

bool FileTransfer::RemoveUpload(const std::string& filename,
                                std::string* error,
                                FileError* code) {
    if (code != nullptr) *code = FileError::kNone;

    if (filename.empty()) {
        if (error != nullptr) *error = "文件名不能为空。";
        if (code != nullptr) *code = FileError::kInternal;
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

    if (std::remove(full_path.c_str()) != 0) {
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

