// 单元测试：FileTransfer 的文件名清洗、路径白名单与大小上限。
// 安全重点（契约 6.2/6.3）：路径穿越（../、绝对路径、反斜杠）、
// Windows 保留名、危险扩展名、超限大小必须全部被拒绝。
// 每个用例使用独立临时目录，避免相互污染。

#include "aimodel/file_transfer.h"

#include <gtest/gtest.h>

#include <string>

#if defined(_WIN32) || defined(_WIN64)
#  include <direct.h>
#else
#  include <unistd.h>
#endif

namespace {

// 当前工作目录（ctest 在 build 目录运行）下的唯一临时上传根。
std::string MakeTempRoot(const char* tag) {
    return std::string("ft_test_") + tag;
}

}  // namespace

TEST(FileTransferTest, UploadWritesFileAndReportsSavedName) {
    const std::string root = MakeTempRoot("basic");
    aimodel::FileTransfer ft(root);

    std::string saved;
    std::string error;
    EXPECT_TRUE(ft.AppendUpload("report.txt", "hello world", &error, &saved));
    EXPECT_EQ(saved, std::string("report.txt"));
    EXPECT_TRUE(error.empty());

    std::string content;
    EXPECT_TRUE(ft.ReadUpload("report.txt", &content, &error));
    EXPECT_EQ(content, std::string("hello world"));

    // 目录内可列出。
    std::string list;
    EXPECT_TRUE(ft.ListUploads(&list, &error));
    EXPECT_EQ(list, std::string("report.txt"));

    // 删除后读应失败。
    EXPECT_TRUE(ft.RemoveUpload("report.txt", &error));
    EXPECT_FALSE(ft.ReadUpload("report.txt", &content, &error));
}

TEST(FileTransferTest, SameNameDoesNotOverwrite) {
    const std::string root = MakeTempRoot("unique");
    aimodel::FileTransfer ft(root);

    std::string first;
    std::string second;
    std::string error;
    ASSERT_TRUE(ft.AppendUpload("a.txt", "one", &error, &first));
    ASSERT_TRUE(ft.AppendUpload("a.txt", "two", &error, &second));

    // 两次落盘名必须不同，且首份内容不被覆盖。
    EXPECT_NE(first, second);
    std::string c1;
    std::string c2;
    ASSERT_TRUE(ft.ReadUpload(first, &c1, &error));
    ASSERT_TRUE(ft.ReadUpload(second, &c2, &error));
    EXPECT_EQ(c1, std::string("one"));
    EXPECT_EQ(c2, std::string("two"));
}

TEST(FileTransferTest, RejectsPathTraversal) {
    const std::string root = MakeTempRoot("traversal");
    aimodel::FileTransfer ft(root);
    std::string error;
    std::string saved;

    EXPECT_FALSE(ft.AppendUpload("../escape.txt", "x", &error, &saved));
    EXPECT_TRUE(saved.empty());
    EXPECT_FALSE(ft.AppendUpload("..\\escape.txt", "x", &error, &saved));
    EXPECT_FALSE(ft.AppendUpload("a/../../etc/passwd", "x", &error, &saved));
    // 读取/删除同样不能逃出白名单。
    EXPECT_FALSE(ft.ReadUpload("../secret.txt", &saved, &error));
    EXPECT_FALSE(ft.RemoveUpload("../secret.txt", &error));
}

TEST(FileTransferTest, RejectsAbsolutePathNames) {
    const std::string root = MakeTempRoot("absolute");
    aimodel::FileTransfer ft(root);
    std::string error;
    std::string saved;

    // 路径分隔符被清洗掉后不再是绝对路径，但仍须落在白名单内；
    // 关键断言：任何含分隔符的名字都不能写到 root 之外。
    EXPECT_FALSE(ft.AppendUpload("/etc/passwd", "x", &error, &saved));
    EXPECT_FALSE(ft.AppendUpload("C:\\Windows\\system32\\evil.txt", "x", &error,
                                &saved));
}

TEST(FileTransferTest, RejectsDotFilesAndTrailingDot) {
    const std::string root = MakeTempRoot("dots");
    aimodel::FileTransfer ft(root);
    std::string error;
    std::string saved;

    EXPECT_FALSE(ft.AppendUpload(".hidden", "x", &error, &saved));
    EXPECT_FALSE(ft.AppendUpload("trailing.", "x", &error, &saved));
}

TEST(FileTransferTest, RejectsWindowsReservedNames) {
    const std::string root = MakeTempRoot("reserved");
    aimodel::FileTransfer ft(root);
    std::string error;
    std::string saved;

    EXPECT_FALSE(ft.AppendUpload("CON", "x", &error, &saved));
    EXPECT_FALSE(ft.AppendUpload("CON.txt", "x", &error, &saved));
    EXPECT_FALSE(ft.AppendUpload("NUL", "x", &error, &saved));
    EXPECT_FALSE(ft.AppendUpload("com1", "x", &error, &saved));
    EXPECT_FALSE(ft.AppendUpload("LPT9", "x", &error, &saved));
}

TEST(FileTransferTest, RejectsDangerousExtensions) {
    const std::string root = MakeTempRoot("danger");
    aimodel::FileTransfer ft(root);
    std::string error;
    std::string saved;

    EXPECT_FALSE(ft.AppendUpload("payload.exe", "x", &error, &saved));
    EXPECT_FALSE(ft.AppendUpload("script.bat", "x", &error, &saved));
    EXPECT_FALSE(ft.AppendUpload("hook.dll", "x", &error, &saved));
    EXPECT_FALSE(ft.AppendUpload("run.ps1", "x", &error, &saved));
    EXPECT_FALSE(ft.AppendUpload("setup.msi", "x", &error, &saved));

    // 普通数据文件必须放行。
    EXPECT_TRUE(ft.AppendUpload("notes.txt", "ok", &error, &saved));
    EXPECT_TRUE(ft.AppendUpload("data.json", "{}", &error, &saved));
    EXPECT_TRUE(ft.AppendUpload("image.png", "PNG", &error, &saved));
}

TEST(FileTransferTest, EnforcesSizeLimitAtTenMegabytes) {
    const std::string root = MakeTempRoot("size");
    aimodel::FileTransfer ft(root);
    std::string error;
    std::string saved;
    aimodel::FileError code = aimodel::FileError::kNone;

    const size_t limit = 10u * 1024u * 1024u;

    // 恰好在限内（10 MB）应成功。
    EXPECT_TRUE(ft.AppendUpload("exact.bin", std::string(limit, 'a'), &error,
                                &saved, &code));
    EXPECT_EQ(code, aimodel::FileError::kNone);

    // 超 1 字节即拒绝，且错误码为 TOO_LARGE。
    EXPECT_FALSE(ft.AppendUpload("over.bin", std::string(limit + 1, 'a'), &error,
                                 &saved, &code));
    EXPECT_EQ(code, aimodel::FileError::kTooLarge);
    EXPECT_FALSE(ft.ReadUpload("over.bin", &saved, &error));
}

TEST(FileTransferTest, SanitizesIllegalCharactersInName) {
    const std::string root = MakeTempRoot("sanitize");
    aimodel::FileTransfer ft(root);
    std::string error;
    std::string saved;

    // Windows 非法字符被清洗后仍是一个合法名字，且不含原非法字符。
    EXPECT_TRUE(ft.AppendUpload("my<file>:name.txt", "x", &error, &saved));
    EXPECT_EQ(saved.find('<'), std::string::npos);
    EXPECT_EQ(saved.find('>'), std::string::npos);
    EXPECT_EQ(saved.find(':'), std::string::npos);
}

TEST(FileTransferTest, AcceptsNonAsciiFilenames) {
    const std::string root = MakeTempRoot("utf8");
    aimodel::FileTransfer ft(root);
    std::string error;
    std::string saved;

    // 中文文件名不应被 isprint 误杀。
    EXPECT_TRUE(ft.AppendUpload("硬件报告.txt", "x", &error, &saved));
    std::string content;
    EXPECT_TRUE(ft.ReadUpload(saved, &content, &error));
    EXPECT_EQ(content, std::string("x"));
}

TEST(FileTransferTest, MissingDirectoryListsAsEmpty) {
    const std::string root = MakeTempRoot("empty");
    aimodel::FileTransfer ft(root);
    std::string error;

    // 尚未上传过：目录不存在时列表应为空而非失败。
    std::string list;
    EXPECT_TRUE(ft.ListUploads(&list, &error));
    EXPECT_TRUE(list.empty());
}

TEST(FileTransferTest, ErrorCodesAreReported) {
    const std::string root = MakeTempRoot("codes");
    aimodel::FileTransfer ft(root);
    std::string error;
    std::string content;
    aimodel::FileError code = aimodel::FileError::kNone;

    // 读取不存在文件 → NOT_FOUND。
    EXPECT_FALSE(ft.ReadUpload("missing.txt", &content, &error, &code));
    EXPECT_EQ(code, aimodel::FileError::kNotFound);

    // 删除不存在文件 → NOT_FOUND。
    EXPECT_FALSE(ft.RemoveUpload("missing.txt", &error, &code));
    EXPECT_EQ(code, aimodel::FileError::kNotFound);
}

TEST(FileTransferTest, TrailingSpaceExtensionBypassIsRejected) {
    const std::string root = MakeTempRoot("extspace");
    aimodel::FileTransfer ft(root);
    std::string error;
    std::string saved;

    // 尾随空格：Windows 落盘会剥掉空格，使 "a.exe " 实际成为 a.exe。
    // 扩展名比对必须先把空白 trim 掉，否则危险扩展名可被空格绕过。
    EXPECT_FALSE(ft.AppendUpload("a.exe ", "x", &error, &saved));
    EXPECT_FALSE(ft.AppendUpload("b.bat ", "x", &error, &saved));
    EXPECT_FALSE(ft.AppendUpload("c.dll\t", "x", &error, &saved));
}

TEST(FileTransferTest, EnsureDirectoryHandlesWindowsDrivePrefix) {
    // Windows 盘符前缀（如 "F:"）会被 SplitNormalizedSegments 切成普通段，
    // EnsureDirectory 必须跳过盘符段而非尝试 mkdir("F")——否则绝对路径的
    // 上传根目录会创建失败（端到端实测曾报 WRITE_FAILED/创建上传目录失败）。
    // 用当前工作目录的盘符拼绝对路径，避免硬编码盘符导致跨机器不可移植。
    char cwd[1024];
    ASSERT_NE(::getcwd(cwd, sizeof(cwd)), static_cast<char*>(nullptr));
    const std::string abs = std::string(cwd) + "/" + MakeTempRoot("driveprefix");

    aimodel::FileTransfer ft(abs);
    std::string error;
    std::string saved;

    EXPECT_TRUE(ft.AppendUpload("probe.txt", "x", &error, &saved));
    EXPECT_EQ(saved, std::string("probe.txt"));

    std::string content;
    EXPECT_TRUE(ft.ReadUpload("probe.txt", &content, &error));
    EXPECT_EQ(content, std::string("x"));
}

TEST(FileTransferTest, IsAllowedPathAcceptsAbsoluteUploadRoot) {
    // IsAllowedPath 校验的是 JoinPath(root, name)。root 自身可以是绝对路径
    // （含盘符），故不能把「绝对路径一律拒绝」套到 full_path 上——那会让
    // 合法的绝对根目录下所有文件都被拒（端到端实测曾报 PATH_NOT_ALLOWED）。
    // 绝对路径防护属于对用户输入名的职责，已由 ContainsPathStructure 承担。
    char cwd[1024];
    ASSERT_NE(::getcwd(cwd, sizeof(cwd)), static_cast<char*>(nullptr));
    const std::string abs = std::string(cwd) + "/" + MakeTempRoot("absroot");

    aimodel::FileTransfer ft(abs);
    std::string error;
    std::string saved;

    ASSERT_TRUE(ft.AppendUpload("inside.txt", "data", &error, &saved));
    // 绝对路径形式的穿越尝试仍必须被拒。
    EXPECT_FALSE(ft.AppendUpload("../outside.txt", "x", &error, &saved));
    EXPECT_FALSE(ft.ReadUpload("..\\outside.txt", &saved, &error));
}

TEST(FileTransferTest, GetUploadsRootRoundTrips) {
    aimodel::FileTransfer ft("custom/root");
    std::string root;
    ASSERT_TRUE(ft.GetUploadsRoot(&root));
    EXPECT_EQ(root, std::string("custom/root"));

    // nullptr 输出参数应返回 false 而不是崩溃。
    EXPECT_FALSE(ft.GetUploadsRoot(nullptr));
}
