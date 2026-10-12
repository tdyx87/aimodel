// 角色：cpp-implementer
// 任务：文件传输服务端接口
// 产物：include/aimodel/file_transfer.h

#ifndef AIMODEL_FILE_TRANSFER_H_
#define AIMODEL_FILE_TRANSFER_H_

#include <string>

#include "aimodel/export.h"

namespace aimodel {

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
class AIMODEL_API FileTransfer {
public:
    // 默认上传根目录为 data/uploads。
    explicit FileTransfer();

    // 显式指定上传根目录。
    explicit FileTransfer(const std::string& uploads_root);

    // 追加上传一个文件。
    // 返回 true 表示写入成功。
    bool AppendUpload(const std::string& filename,
                      const std::string& content,
                      std::string* error);

    // 列出上传目录下的文件名。
    // 返回 true 表示成功，list 可能为空。
    bool ListUploads(std::string* list);

    // 读取一个已上传文件。
    bool ReadUpload(const std::string& filename,
                    std::string* content,
                    std::string* error);

    // 删除一个已上传文件。
    bool RemoveUpload(const std::string& filename,
                      std::string* error);

    // 获取上传根目录。
    bool GetUploadsRoot(std::string* root);

private:
    // 检查文件名是否安全：
    //   - 非空；
    //   - 长度 ≤ 1024；
    //   - 不能包含路径分隔符、NUL；
    //   - 不能为点文件；
    //   - 不能使用 Windows 保留名。
    bool IsSafeFilename(const std::string& filename) const;

    // 生成安全文件名，去掉非法字符。
    std::string MakeSafeFilename(const std::string& filename);

    // 生成唯一文件名，避免覆盖已有文件。
    std::string GetUniqueFilename(const std::string& filename);

    // 校验完整路径是否仍在 uploads_root_ 内部。
    bool IsAllowedPath(const std::string& full_path,
                       const std::string& uploads_root);

    // 上传根目录。
    std::string uploads_root_;

    // 最大文件大小：50 MB。
    static constexpr size_t kMaxFileSize = 50u * 1024u * 1024u;

    // 最大重命名尝试次数，防止死循环。
    static constexpr int kMaxRenames = 1000;
};

}  // namespace aimodel

#endif  // AIMODEL_FILE_TRANSFER_H_
