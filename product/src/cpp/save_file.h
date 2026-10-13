// 内部文件读取抽象（不安装、不进公共头）。
// 平台分支只用编译期宏（agents.md §7.3）；实现于 save_file_{win32,posix}.cpp。
//
// 关键约束：这是**只读**入口。契约 §5 / PRD NFR-5 要求工具绝不修改用户的
// 原始存档，因此这里只有 Open-read / Close，不提供任何写入、截断、删除、
// 重命名接口——不存在写路径，就是最强的保证（security-reviewer 可据此核验）。

#ifndef SAVEINSPECTOR_SAVE_FILE_H_
#define SAVEINSPECTOR_SAVE_FILE_H_

#include <string>

namespace saveinspector {

// 只读读入整个文件。
//
// - 只以只读方式打开，任何失败都不修改文件；
// - 超过 max_bytes 时返回 false 并在 error 中说明，避免超大文件耗尽内存
//   （PRD NFR-2 要求 100 MB 存档可用，故默认值取 512 MB）；
// - error 为**面向用户的中文**，不含本机路径（契约 §5）。
bool ReadFileBytesReadOnly(const std::string& path, std::string* out,
                           std::string* error, size_t max_bytes = 512ull * 1024ull * 1024ull);

}  // namespace saveinspector

#endif  // SAVEINSPECTOR_SAVE_FILE_H_
