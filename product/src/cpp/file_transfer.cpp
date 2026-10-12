// 角色：cpp-implementer
// 任务：实现 file_transfer 后端
// 产物：src/cpp/file_transfer.cpp

#include "aimodel/file_transfer.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace aimodel {

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

bool IsSafeFilenameChar(char c) {
    return std::isprint(static_cast<unsigned char>(c)) != 0;
}

}  // namespace

bool FileTransfer::IsSafeFilename(const std::string& filename) const {
    if (filename.empty()) return false;
    if (filename.size() > 1024) return false;

    // 不能以点开头或以点结尾。
    if (filename[0] == '.' || filename[filename.size() - 1] == '.') return false;

    for (char c : filename) {
        if (!IsSafeFilenameChar(c) || IsPathSeparator(c) || c == '\0') {

std::string FileTransfer::GetUniqueFilename(const std::string& filename) {
    std::string candidate = filename;
    for (int i = 0; i < kMaxRenames; ++i) {
        struct stat st;
        if (stat((uploads_root_ + "/" + candidate).c_str(), &st) != 0) {
            return candidate;
        }

        // 文件已存在，追加时间戳。
        std::time_t now = std::time(nullptr);
#if defined(_WIN32) || defined(_WIN64)
        struct tm tm_info;
        localtime_s(&tm_info, &now);
#else
        struct tm tm_info;
        localtime_r(&now, &tm_info);
#endif

        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), ".%04d%02d%02d_%02d%02d%02d.%d",
                      tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                      tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec, i);
        candidate = filename + buffer;
    }

    return candidate;
}

bool FileTransfer::IsAllowedPath(const std::string& full_path,
                                 const std::string& uploads_root) {
    // 统一路径分隔符，避免 Windows 下反斜杠绕过。
    std::string normalized = full_path;
    for (auto& c : normalized) {
        if (c == '\\') c = '/';
    }

    // 归一化：去掉 /.. 段。
    std::vector<std::string> parts;
    std::string current;
    for (char c : normalized) {
        if (c == '/') {
            if (current == "..") {
                if (!parts.empty()) parts.pop_back();
            } else if (!current.empty() && current != ".") {
                parts.push_back(current);
            }
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty() && current != ".") {
        parts.push_back(current);
    }

    // 检查是否还有 ..。
    for (const auto& part : parts) {
        if (part == "..") return false;
    }

    // 构造绝对路径。
    std::string absolute;
    if (uploads_root.empty()) return false;

    // 去掉末尾分隔符。
    std::string root = uploads_root;
    while (!root.empty() && root.back() == '/') root.pop_back();

    // 对 root 做同样归一化。
    std::vector<std::string> root_parts;

bool FileTransfer::AppendUpload(const std::string& filename,
                                const std::string& content,
                                std::string* error) {
    if (filename.empty()) {
        if (error != nullptr) *error = "filename 不能为空。";
        return false;
    }

    // 强制检查内容大小，避免磁盘耗尽。
    if (content.size() > kMaxFileSize) {
        if (error != nullptr) {
            *error = "文件过大（当前上传的大小已超过 50MB）";
        }
        return false;
    }

    if (!IsSafeFilename(filename)) {
        if (error != nullptr) *error = "文件名包含非法字符。";
        return false;
    }

    if (uploads_root_.empty()) {
        if (error != nullptr) *error = "上传根目录未配置。";
        return false;
    }

    // 保证上传目录存在。
    std::error_code ec;
    std::filesystem::path root_path(uploads_root_, ec);
    if (ec) {
        if (error != nullptr) *error = "上传根目录不可用。";
        return false;
    }

    std::error_code fs_error;
    std::filesystem::create_directories(root_path, fs_error);
    if (fs_error) {
        if (error != nullptr) *error = "创建上传目录失败。";
        return false;
    }

    // 生成唯一安全文件名。
    std::string target = GetUniqueFilename(filename);
    std::filesystem::path full_path = root_path / target;

    // 强制路径归一化：
    // 1. 拒绝路径穿越；
    // 2. 拒绝绝对路径；
    // 3. 拒绝目标与 root 无关。
    if (!IsAllowedPath(full_path.string(), uploads_root_)) {
        if (error != nullptr) *error = "文件路径不在允许目录内。";
        return false;
    }

    // 创建文件失败时返回错误。
    std::ofstream out(full_path.string().c_str(),
                      std::ios::binary | std::ios::app);
    if (!out.is_open()) {
        if (error != nullptr) *error = "无法打开文件。";
        return false;
    }

    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.flush();
    if (!out.good()) {
        if (error != nullptr) *error = "文件写入失败。";
        return false;
    }

    out.close();
    if (!out) {
        if (error != nullptr) *error = "文件写入失败。";
        return false;
    }

    return true;
}

bool FileTransfer::ListUploads(std::string* list) {

bool FileTransfer::ReadUpload(const std::string& filename,
                              std::string* content,
                              std::string* error) {
    if (filename.empty()) {
        if (error != nullptr) *error = "filename 不能为空。";
        return false;
    }

    if (content == nullptr) return false;

    if (uploads_root_.empty()) {
        if (error != nullptr) *error = "上传根目录未配置。";
        return false;
    }

    if (filename.size() > 1024) {
        if (error != nullptr) *error = "文件名过长。";
        return false;
    }

    // 生成安全文件名。
    std::string safe = MakeSafeFilename(filename);
    std::filesystem::path full_path = uploads_root_ + "/" + safe;

    // 校验是否仍在 uploads 目录内。
    if (!IsAllowedPath(full_path.string(), uploads_root_)) {
        if (error != nullptr) *error = "文件路径不在允许目录内。";
        return false;
    }

    std::ifstream in(full_path.string().c_str(), std::ios::binary);
    if (!in.is_open()) {
        if (error != nullptr) *error = "文件不存在或无法打开。";
        return false;
    }

    std::ostringstream ss;
    ss << in.rdbuf();
    *content = ss.str();

    return true;
}

bool FileTransfer::RemoveUpload(const std::string& filename,
                                std::string* error) {
    if (filename.empty()) {
        if (error != nullptr) *error = "filename 不能为空。";
        return false;
    }

    if (uploads_root_.empty()) {
        if (error != nullptr) *error = "上传根目录未配置。";
        return false;
    }

    std::string safe = MakeSafeFilename(filename);
    std::filesystem::path full_path = uploads_root_ + "/" + safe;

    if (!IsAllowedPath(full_path.string(), uploads_root_)) {
        if (error != nullptr) *error = "文件路径不在允许目录内。";
        return false;
    }

    std::error_code ec;
    if (std::filesystem::remove(full_path, ec)) return true;

    if (ec) {
        if (error != nullptr) *error = "删除文件失败。";
        return false;
    }

    return false;
}

bool FileTransfer::GetUploadsRoot(std::string* root) {
    if (root == nullptr) return false;
    *root = uploads_root_;
    return true;
}

}  // namespace aimodel

    if (list == nullptr) return false;

    if (uploads_root_.empty()) return false;

    std::error_code ec;
    std::filesystem::path root(uploads_root_, ec);
    if (ec) return false;

    if (!std::filesystem::exists(root, ec)) return false;
    if (!std::filesystem::is_directory(root, ec)) return false;

    std::vector<std::string> files;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (entry.is_regular_file(ec)) {
                files.push_back(entry.path().filename().string());
            }
        }
    } catch (...) {
        return false;
    }

    std::sort(files.begin(), files.end());

    list->clear();
    for (size_t i = 0; i < files.size(); ++i) {
        if (i != 0) list->append("\n");
        list->append(files[i]);
    }

    return true;
}

    std::string root_current;
    for (char c : root) {
        if (c == '/') {
            if (!root_current.empty() && root_current != ".") {
                root_parts.push_back(root_current);
            }
            root_current.clear();
        } else {
            root_current += c;
        }
    }
    if (!root_current.empty() && root_current != ".") {
        root_parts.push_back(root_current);
    }

    for (const auto& part : root_parts) {
        absolute += "/" + part;
    }

    std::string normalized_root = absolute;
    for (auto& c : normalized_root) {
        if (c == '\\') c = '/';
    }
    while (!normalized_root.empty() && normalized_root.back() == '/') {
        normalized_root.pop_back();
    }

    // 构造完整路径。
    std::string full = normalized;
    while (!full.empty() && full.back() == '/') full.pop_back();

    // 判断 full 是否以 normalized_root 开头。
    if (normalized_root.empty()) return false;

    if (full.size() < normalized_root.size()) return false;

    if (full.compare(0, normalized_root.size(), normalized_root) != 0) {
        return false;
    }

    // 检查下一字符是否为 / 或文件结束。
    size_t next = normalized_root.size();
    if (next < full.size() && full[next] != '/') {
        return false;
    }

    return true;
}

            return false;
        }
    }

    // Windows 保留名，避免跨平台路径歧义。
    std::string upper;
    upper.reserve(filename.size());
    for (char c : filename) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }

    static const char* reserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };

    for (const char* name : reserved) {
        if (upper == name) return false;
    }

    return true;
}

std::string FileTransfer::MakeSafeFilename(const std::string& filename) {
    std::string safe;
    safe.reserve(filename.size());

    for (char c : filename) {
        if (IsSafeFilenameChar(c) && !IsPathSeparator(c) && c != '\0') {
            safe.push_back(c);
        }
    }

    if (safe.empty()) safe = "upload.bin";
    if (safe.size() > 1024) safe.resize(1024);

    // 再次检查保留名。
    std::string upper;
    upper.reserve(safe.size());
    for (char c : safe) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }

    static const char* reserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };

    for (const char* name : reserved) {
        if (upper == name) return "upload.bin";
    }

    return safe;
}
