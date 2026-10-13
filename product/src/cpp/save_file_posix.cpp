// 角色：cpp-implementer
// 产物：src/cpp/save_file_posix.cpp
// 契约：docs/design/save-format-contract.md §5（只读保证、不泄露路径）
//
// POSIX 上路径本就是字节流（通常即 UTF-8），无需宽字符转换。
// 只读保证：用 O_RDONLY 打开，不申请任何写标志。

#if !defined(_WIN32) && !defined(_WIN64)

#include "save_file.h"

#include <errno.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace saveinspector {
namespace {

std::string ErrorFor(int e) {
    switch (e) {
        case ENOENT:
            return "找不到这个存档文件。请确认路径是否正确，或文件是否已被移动。";
        case EACCES:
            return "没有权限读取这个文件。请确认文件权限是否允许。";
        case EISDIR:
            return "这是一个文件夹，不是存档文件。";
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

    // O_RDONLY：唯一的打开标志，没有任何写意图。
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (error != NULL) *error = ErrorFor(errno);
        return false;
    }

    struct stat st;
    if (::fstat(fd, &st) != 0) {
        const int e = errno;
        ::close(fd);
        if (error != NULL) *error = ErrorFor(e);
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        ::close(fd);
        if (error != NULL) *error = "这是一个文件夹，不是存档文件。";
        return false;
    }
    if (st.st_size <= 0) {
        ::close(fd);
        if (error != NULL) *error = "这个存档文件是空的，可能没有保存成功。";
        return false;
    }
    if (static_cast<unsigned long long>(st.st_size) >
            static_cast<unsigned long long>(max_bytes)) {
        ::close(fd);
        if (error != NULL) *error = "这个存档文件太大，无法打开。";
        return false;
    }

    const size_t total = static_cast<size_t>(st.st_size);
    std::string data;
    data.reserve(total);
    char buf[65536];
    while (data.size() < total) {
        const size_t want =
            (total - data.size()) < sizeof(buf) ? (total - data.size()) : sizeof(buf);
        const ssize_t n = ::read(fd, buf, want);
        if (n < 0) {
            const int e = errno;
            ::close(fd);
            if (error != NULL) *error = ErrorFor(e);
            return false;
        }
        if (n == 0) break;  // 文件在读取过程中被截短
        data.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);

    if (data.empty()) {
        if (error != NULL) *error = "这个存档文件是空的，可能没有保存成功。";
        return false;
    }
    *out = data;
    return true;
}

}  // namespace saveinspector

#endif  // !(_WIN32 || _WIN64)
