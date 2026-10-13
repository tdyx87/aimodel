// 角色：cpp-implementer
// 任务：文件传输服务端接口
// 产物：include/aimodel/file_transfer.h
// 契约：docs/design/file-transfer-contract.md
// 审计：docs/audit/2026-10-12-architect-file-transfer.md

#ifndef AIMODEL_FILE_TRANSFER_H_
#define AIMODEL_FILE_TRANSFER_H_

#include <cstddef>
#include <string>

#include "aimodel/export.h"

namespace aimodel {

// 传输失败的错误码（契约第 5 节）。HTTP 层据此映射 JSON 响应体的 code 字段，
// 并按错误码选择 HTTP 状态码（见 file-transfer-contract.md §4.3）。
enum class FileError {
    kNone = 0,        // 成功
    kInvalidName,     // 文件名非法（空、超长、危险扩展名、点文件、清洗后仍不合法）
    kPathNotAllowed,  // 路径不在授权目录内（名字里带路径成分）
    kNotFound,        // 文件不存在
    kTooLarge,        // 超过大小上限
    kWriteFailed,     // 写入/删除失败
    kInternal         // 内部错误（参数非法、根目录不可用等）
};

// 文件名长度上限（字符数），见 IsSafeFilename。
const size_t kMaxUploadFilenameLength = 1024;

// 服务端文件传输模块。
// 职责：
//   - 接收上传内容；
//   - 校验文件名与目标路径；
//   - 限制文件大小；
//   - 将文件写入服务端授权目录；
//   - 支持列出、读取、删除已上传文件。
//
// 安全边界：
//   - 不允许路径穿越（../、绝对路径、反斜杠等）；
//   - 文件名必须合法；
//   - 只允许访问服务端授权目录（默认 data/uploads）；
//   - 不向调用方暴露内部绝对路径。
//
// 说明：error 为面向用户的中文提示（可直接进 JSON 的 message），
// code 为机器可判定的错误码，两者可同时为 nullptr。
class AIMODEL_API FileTransfer {
public:
    // 默认上传根目录为 data/uploads。
    explicit FileTransfer();

    // 显式指定上传根目录。
    explicit FileTransfer(const std::string& uploads_root);

    // 上传一个文件：清洗文件名 → 校验 → 生成唯一名 → 写入授权目录。
    // 返回 true 表示写入成功。
    // saved_name（可选）输出服务端实际落盘的文件名（不含目录），
    // 供 HTTP 层填充契约 4.1 的 saved_path / file_name 字段；
    // 该值不泄露内部绝对路径。
    bool AppendUpload(const std::string& filename,
                      const std::string& content,
                      std::string* error,
                      std::string* saved_name = nullptr,
                      FileError* code = nullptr);

    // 列出上传目录下的文件名（换行分隔，已排序）。
    // 返回 true 表示成功，list 可能为空。
    bool ListUploads(std::string* list,
                     std::string* error = nullptr,
                     FileError* code = nullptr);

    // 读取一个已上传文件。
    bool ReadUpload(const std::string& filename,
                    std::string* content,
                    std::string* error,
                    FileError* code = nullptr);

    // 删除一个已上传文件。
    bool RemoveUpload(const std::string& filename,
                      std::string* error,
                      FileError* code = nullptr);

    // 获取上传根目录（仅 root 为 nullptr 时返回 false）。
    bool GetUploadsRoot(std::string* root);

private:
    // 检查文件名是否安全：
    //   - 非空；
    //   - 长度 ≤ kMaxUploadFilenameLength；
    //   - 不能包含路径分隔符、NUL、控制字符、Windows 非法字符；
    //   - 不能以点开头（点文件）或以点结尾；
    //   - 不能使用 Windows 保留名（含带扩展名形式，如 CON.txt）；
    //   - 扩展名不在危险扩展名名单内。
    bool IsSafeFilename(const std::string& filename) const;

    // 生成安全文件名，去掉非法字符（保留扩展名语义）。
    std::string MakeSafeFilename(const std::string& filename);

    // 在扩展名前插入「时间戳 + 序号」，用于排他创建撞名后的重试。
    std::string MakeStampVariant(const std::string& filename, int index);

    // 校验完整路径是否仍在 uploads_root_ 内部。
    bool IsAllowedPath(const std::string& full_path,
                       const std::string& uploads_root);

    // 递归创建目录（C++11 无 <filesystem>，逐段 mkdir/create_directory）。
    bool EnsureDirectory(const std::string& path, std::string* error);

    // 判断扩展名是否为危险可执行类型（契约 6.3 第 4 条）。
    bool IsDangerousExtension(const std::string& filename) const;

    // 判断文件名主名是否为 Windows 保留名（CON/PRN/COM1…，含 CON.txt 形式）。
    bool IsReservedBaseName(const std::string& upper_base) const;

    // 上传根目录。
    std::string uploads_root_;

    // 最大文件大小：10 MB（契约 6.4 默认上限）。
    static const size_t kMaxFileSize;

    // 最大重命名尝试次数，防止死循环。
    static const int kMaxRenames;
};

}  // namespace aimodel

#endif  // AIMODEL_FILE_TRANSFER_H_
