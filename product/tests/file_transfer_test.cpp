// 单元测试：FileTransfer 的文件名清洗、路径白名单与大小上限。
// 安全重点（契约 6.2/6.3）：路径穿越（../、绝对路径、反斜杠）、
// Windows 保留名、危险扩展名、超限大小必须全部被拒绝。
// 每个用例使用独立临时目录，避免相互污染。

#include "aimodel/file_transfer.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <string>

#if defined(_WIN32) || defined(_WIN64)
#  include <direct.h>
#else
#  include <unistd.h>
#endif

namespace {

// 当前工作目录（ctest 在 build 目录运行）下的唯一临时上传根。
// 加入「时间戳 + 进程内自增序号」保证每次运行互不污染：否则第二次运行会读到
// 上次残留的文件，使「重名换名」「空目录列表」等用例偶发失败（非幂等）。
std::string MakeTempRoot(const char* tag) {
    static unsigned int counter = 0;
    const std::time_t now = std::time(NULL);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "ft_test_%s_%ld_%u", tag,
                  static_cast<long>(now), ++counter);
    return std::string(buf);
}

// 文件是否存在。用 ifstream 而非平台 API：窄路径在 Windows 上按 ANSI 代码页
// 解释，与 GetCwd 返回的编码一致；POSIX 上本就是字节流。
bool TestFileExists(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    return in.is_open();
}

// 当前工作目录。MSVC 上 getcwd 已弃用（/W4 报 C4996），改用 _getcwd；
// 其余编译器（MinGW-w64 / POSIX）用 getcwd。
std::string GetCwd() {
    char buf[1024];
#if defined(_MSC_VER)
    if (::_getcwd(buf, sizeof(buf)) == NULL) {
        return std::string();
    }
#else
    if (::getcwd(buf, sizeof(buf)) == NULL) {
        return std::string();
    }
#endif
    return std::string(buf);
}

// 读环境变量。MSVC 上 getenv 已弃用（/W4 报 C4996），改用 _dupenv_s。
std::string EnvOrEmpty(const char* name) {
#if defined(_MSC_VER)
    char* value = NULL;
    size_t len = 0;
    if (::_dupenv_s(&value, &len, name) != 0 || value == NULL) {
        if (value != NULL) std::free(value);
        return std::string();
    }
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* v = std::getenv(name);
    return (v == NULL) ? std::string() : std::string(v);
#endif
}

// 系统临时目录（绝对路径）。构造「不在当前工作目录下」的授权根时需要它——
// 若授权根本来就是 cwd + "/" + name，那么「根前缀丢失」与「未丢失」解析到
// 同一个文件，反向断言就恒真、什么也证明不了。
std::string AbsTempRoot() {
    static const char* kVars[] = {"TMPDIR", "TEMP", "TMP"};
    for (size_t i = 0; i < sizeof(kVars) / sizeof(kVars[0]); ++i) {
        const std::string v = EnvOrEmpty(kVars[i]);
        if (!v.empty()) {
            return v;
        }
    }
    return std::string();
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

TEST(FileTransferTest, HandlesNonAsciiFilenamesOnDisk) {
    // 回归：Windows 曾用 ANSI 文件 API（CreateFileA/_mkdir/_findfirst）处理 UTF-8 路径，
    // 被按系统代码页（简中 GBK/936）解释，导致：
    //   - 部分中文名（UTF-8 字节非合法 GBK）上传直接失败（WRITE_FAILED），如「中文名.txt」；
    //   - 其余中文名虽成功但落盘乱码（「硬件报告.txt」→「纭欢鎶ュ憡.txt」）。
    // 修复后统一走宽字符 API，以下断言应全部成立。
    const std::string root = MakeTempRoot("cn");
    aimodel::FileTransfer ft(root);
    std::string error;
    std::string saved;

    const char* names[] = {"中文名.txt", "硬件报告.txt", "测试.txt", "模型推荐.txt"};
    const size_t count = sizeof(names) / sizeof(names[0]);

    for (size_t i = 0; i < count; ++i) {
        saved.clear();
        ASSERT_TRUE(ft.AppendUpload(names[i], "content", &error, &saved)) << names[i];
        EXPECT_EQ(saved, std::string(names[i])) << names[i];
    }

    // 列目录应原样返回中文名（旧实现经 ANSI 枚举会乱码）。
    std::string list;
    ASSERT_TRUE(ft.ListUploads(&list, &error));
    for (size_t i = 0; i < count; ++i) {
        EXPECT_NE(list.find(names[i]), std::string::npos) << names[i];
    }

    // 读回内容一致。
    for (size_t i = 0; i < count; ++i) {
        std::string content;
        ASSERT_TRUE(ft.ReadUpload(names[i], &content, &error)) << names[i];
        EXPECT_EQ(content, std::string("content"));
    }
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
    // EnsureDirectory 必须先接上它再逐段 mkdir——否则 mkdir("F") 必然失败，
    // 绝对路径的上传根目录会创建失败（端到端实测曾报 WRITE_FAILED/创建上传目录失败）。
    // 用当前工作目录的盘符拼绝对路径，避免硬编码盘符导致跨机器不可移植。
    // （文件实际落盘位置的断言见 AbsoluteUploadRootKeepsRootPrefix。）
    const std::string cwd = GetCwd();
    ASSERT_FALSE(cwd.empty());
    const std::string abs = cwd + "/" + MakeTempRoot("driveprefix");

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
    // （含盘符 / POSIX 前导 /），故不能把「绝对路径一律拒绝」套到 full_path 上
    // ——那会让合法的绝对根目录下所有文件都被拒（端到端实测曾报 PATH_NOT_ALLOWED）。
    // 绝对路径防护属于对用户输入名的职责，已由 ContainsPathStructure 承担。
    const std::string cwd = GetCwd();
    ASSERT_FALSE(cwd.empty());
    const std::string abs = cwd + "/" + MakeTempRoot("absroot");

    aimodel::FileTransfer ft(abs);
    std::string error;
    std::string saved;

    ASSERT_TRUE(ft.AppendUpload("inside.txt", "data", &error, &saved));
    // 绝对路径形式的穿越尝试仍必须被拒。
    EXPECT_FALSE(ft.AppendUpload("../outside.txt", "x", &error, &saved));
    EXPECT_FALSE(ft.ReadUpload("..\\outside.txt", &saved, &error));
}

TEST(FileTransferTest, AbsoluteUploadRootKeepsRootPrefix) {
    // 回归：EnsureDirectory 逐段建目录时，若把根前缀（POSIX 的前导 "/"、
    // Windows 的盘符 "C:"）当成普通段丢掉，绝对路径会退化成当前工作目录下的
    // 相对路径 —— 文件落在与授权目录不同的位置。
    //
    // 用例原先是**假阳性**：它只断言 AppendUpload/ReadUpload 返回 true，
    // 而 IsAllowedPath 也是按段比较的（前缀两边都被丢掉），所以即使建到了错误
    // 位置照样通过；且授权根取自 cwd + "/" + name，两种解析指向同一个文件，
    // 反向断言恒真。这里把授权根放在系统临时目录（cwd 之外），差异才可观测。
    const std::string cwd = GetCwd();
    const std::string base = AbsTempRoot();
    ASSERT_FALSE(cwd.empty());
    ASSERT_FALSE(base.empty());

    const std::string name = MakeTempRoot("rootprefix");
    const std::string abs = base + "/" + name;

    aimodel::FileTransfer ft(abs);
    std::string error;
    std::string saved;
    ASSERT_TRUE(ft.AppendUpload("probe.txt", "payload", &error, &saved)) << error;

    // 服务端声明的根就是那个绝对路径。
    std::string root;
    ASSERT_TRUE(ft.GetUploadsRoot(&root));
    EXPECT_EQ(root, abs);

    // 文件必须真的落在绝对路径下。
    EXPECT_TRUE(TestFileExists(abs + "/probe.txt")) << error;

    // 反向断言：根前缀若被丢弃，会在当前工作目录下另建一份同名相对目录。
    // （临时目录恰在 cwd 下时两者同形，无法区分，故跳过。）
    if (base != cwd) {
        EXPECT_FALSE(TestFileExists(cwd + "/" + name + "/probe.txt"))
            << "根前缀丢失：文件同时出现在当前工作目录下的相对路径 " << name;
    }

    // 清理（写在 build 目录之外，用完即删）。
    std::remove((abs + "/probe.txt").c_str());
#if defined(_WIN32) || defined(_WIN64)
    ::_rmdir(abs.c_str());
#else
    ::rmdir(abs.c_str());
#endif
}

TEST(FileTransferTest, NestedRelativeUploadRootIsCreatedUnderCwd) {
    // 相对根（含多级）同样要逐段建出，且目录内容可枚举。
    const std::string rel = "a1/b2/" + MakeTempRoot("nested");
    aimodel::FileTransfer ft(rel);
    std::string error;
    std::string saved;

    ASSERT_TRUE(ft.AppendUpload("deep.txt", "d", &error, &saved));
    EXPECT_TRUE(TestFileExists(rel + "/deep.txt"));

    std::string listing;
    ASSERT_TRUE(ft.ListUploads(&listing, &error));
    EXPECT_EQ(listing, std::string("deep.txt"));
}

TEST(FileTransferTest, GetUploadsRootRoundTrips) {
    aimodel::FileTransfer ft("custom/root");
    std::string root;
    ASSERT_TRUE(ft.GetUploadsRoot(&root));
    EXPECT_EQ(root, std::string("custom/root"));

    // nullptr 输出参数应返回 false 而不是崩溃。
    EXPECT_FALSE(ft.GetUploadsRoot(nullptr));
}
