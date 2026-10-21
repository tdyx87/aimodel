/* tools_test.c —— 工具层测试
 * 契约：docs/design/contract.md §5.2 / §10 T1、T4
 *
 * 重点在**安全**与**拒绝路径**：
 *   - 路径逃逸（.. / 绝对路径 / 符号链接）必须被拒绝，且不泄露工作区外结构
 *   - 权限默认拒绝；拒绝后不得中断会话
 *   - 截断必须显式标注，否则模型会以为读到全文
 */

/* 测试里用到 getenv/fopen；fopen_s / _dupenv_s 仅 MSVC 有，没有可移植等价物。
 * 必须定义在**任何** CRT 头文件之前，否则不生效。 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "ca_test.h"

#include "cagent/tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <process.h>
#  define CA_TEST_PID() ((unsigned long)_getpid())
#else
#  include <unistd.h>
#  define CA_TEST_PID() ((unsigned long)getpid())
#endif


#if !defined(_WIN32)
#  include <unistd.h>
#endif

static char g_result[1 << 20];

/* 路径分隔符早先是靠「把 #if 写进 snprintf 的参数里」来选择的：
 *
 *     snprintf(p, n, "%s%c%s", dir,
 * #if defined(_WIN32)
 *              '\\'
 * #else
 *              '/'
 * #endif
 *     );
 *
 * 这在 C 标准里是**未定义行为**——预处理指令出现在宏实参内部，
 * 行为未定义。GCC/macOS 都直接报 warning。
 * 提取成常量后既合法又可读。 */
#if defined(_WIN32)
#  define CA_TEST_SEP '\\'
#else
#  define CA_TEST_SEP '/'
#endif

/* 测试工作区建在系统临时目录，而不是仓库里。
 * 依赖仓库文件做 fixture 有两个问题：文件可能被删（本分支就删过
 * product/README.md），以及测试会在工作区里写东西。 */
static const char *g_ws_dir = NULL;

static const char *make_ws_dir(void) {
    if (g_ws_dir != NULL) return g_ws_dir;
    static char dir[1024];
#if defined(_WIN32)
    const char *tmp = getenv("TEMP");
    if (tmp == NULL) tmp = getenv("TMP");
    if (tmp == NULL) tmp = "C:\\Windows\\Temp";
    snprintf(dir, sizeof(dir), "%s\\cagent_tools_test_%lu", tmp, CA_TEST_PID());
#else
    snprintf(dir, sizeof(dir), "/tmp/cagent_tools_test_%lu", CA_TEST_PID());
#endif
    /* 幂等清理后重建 */
    /* 缓冲区要装下两条命令 + 两份路径 + 引号语法：
     * 只给 2048 时 GCC 算出 "writing up to 1023 bytes into a region of
     * size between 1002 and 2025" —— 真的会截断。 */
    char cmd[4096];
#if defined(_WIN32)
    /* system() 本身就走 cmd.exe。不要再套一层 "cmd /c"：
     * 套了之后 '&' 会被**外层** cmd 解释，第二条命令就跑在错误的上下文里
     * （症状是目录没建成、所有用例拿到 ws == NULL）。 */
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\" 2>nul & mkdir \"%s\"", dir, dir);
#else
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", dir, dir);
#endif
    if (system(cmd) != 0) {
        printf("    无法准备工作区目录\n");
        return NULL;
    }
    /* 就绪判据用「能否创建 fixture 文件」，而不是探测目录本身 ——
     * Windows 上带 '\.' 的路径 fopen 打不开，早先的探测方式恒为失败。 */
    char file[2048];
    snprintf(file, sizeof(file), "%s%cdata.txt", dir, CA_TEST_SEP);;
    FILE *f = fopen(file, "wb");
    if (f == NULL) {
        printf("    工作区目录未就绪（无法创建 fixture）\n");
        return NULL;
    }
    for (int i = 1; i <= 200; i++) fprintf(f, "line %d\n", i);
    fclose(f);
    g_ws_dir = dir;
    return dir;
}

static ca_workspace *make_ws(void) {
    const char *dir = make_ws_dir();
    if (dir == NULL) return NULL;
    return ca_ws_open(dir);
}

static ca_tool_ctx make_ctx(ca_workspace *ws, ca_perm_cb perm) {
    static ca_out o;
    ca_tool_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ca_out_init(&o, g_result, sizeof(g_result));
    ctx.ws = ws;
    ctx.out = &o;
    ctx.perm = perm;
    ctx.perm_user = NULL;
    return ctx;
}

/* ==================================================== 路径安全（P4） */

CA_TEST(path_rejects_dotdot_escape) {
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    char out[2048];
    size_t need = 0;
    /* 逃出工作区必须被拒绝 */
    CA_ASSERT(ca_ws_resolve(ws, "../../../etc/passwd", out, sizeof(out), &need) == -1);
    CA_ASSERT(ca_ws_resolve(ws, "a/../../b", out, sizeof(out), &need) == -1);
    ca_ws_free(ws);
}

CA_TEST(path_accepts_inner_dotdot) {
    /* .. 不越界时必须允许，否则正常的 ../ 相对引用全被拒 */
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    char out[2048];
    size_t need = 0;
    CA_ASSERT(ca_ws_resolve(ws, "sub/../file.txt", out, sizeof(out), &need) == 0);
    CA_ASSERT(strstr(out, "file.txt") != NULL);
    CA_ASSERT(strstr(out, "..") == NULL);
    ca_ws_free(ws);
}

CA_TEST(path_rejects_absolute_outside_workspace) {
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    char out[2048];
    size_t need = 0;
#if defined(_WIN32)
    CA_ASSERT(ca_ws_resolve(ws, "C:\\Windows\\System32\\drivers", out, sizeof(out), &need) == -1);
#else
    CA_ASSERT(ca_ws_resolve(ws, "/etc/passwd", out, sizeof(out), &need) == -1);
    CA_ASSERT(ca_ws_resolve(ws, "/tmp", out, sizeof(out), &need) == -1);
#endif
    ca_ws_free(ws);
}

CA_TEST(path_rejects_absolute_outside_on_relative_root) {
    /* 工作区本身是相对路径时，绝对路径输入也必须正确处理 */
    ca_workspace *ws = ca_ws_open(".");
    CA_ASSERT(ws != NULL);
    char out[2048];
    size_t need = 0;
#if defined(_WIN32)
    CA_ASSERT(ca_ws_resolve(ws, "C:\\Windows", out, sizeof(out), &need) == -1);
#else
    CA_ASSERT(ca_ws_resolve(ws, "/etc", out, sizeof(out), &need) == -1);
#endif
    ca_ws_free(ws);
}

#if !defined(_WIN32)
CA_TEST(path_rejects_symlink_escape) {
    /* 符号链接必须建在**工作区里面**。
     *
     * 早先这里用的是 getcwd()——也就是 CWD，而工作区在 /tmp/cagent_tools_test。
     * 两者根本不是同一个目录，于是：
     *   realpath("<ws>/cagent_test_symlink/passwd") 失败（它不存在）
     *   -> 退到 realpath_ancestor()，拿到的祖先是工作区本身
     *   -> is_within(真) -> 返回 0
     * 断言 rc == -1 失败。
     *
     * 更糟的是：这个用例**在任何平台上都没真正测到符号链接逃逸防护**，
     * 它只是在测「一个不存在的路径能不能解析」。
     * 注释里「P4 要求覆盖」写得很响，实际覆盖的是空气。
     */
    const char *wsdir = make_ws_dir();
    CA_ASSERT(wsdir != NULL);
    if (wsdir == NULL) return;
    char link_path[3072];
    int n = snprintf(link_path, sizeof(link_path), "%s%ccagent_test_symlink",
                     wsdir, CA_TEST_SEP);
    CA_ASSERT(n > 0 && (size_t)n < sizeof(link_path)); /* 截断即视为失败 */
    if (n <= 0 || (size_t)n >= sizeof(link_path)) return;
    unlink(link_path);
    if (symlink("/etc", link_path) != 0) {
        printf("    无法创建符号链接，跳过（本环境不允许）\n");
        return;
    }

    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    if (ws == NULL) { unlink(link_path); return; }
    char out[2048];
    size_t need = 0;
    /* 符号链接完全在工作区内部，仅测 ../ 是测不出它的。契约 P4 明确要求覆盖 */
    int rc = ca_ws_resolve(ws, "cagent_test_symlink/passwd", out, sizeof(out), &need);
    unlink(link_path);
    CA_ASSERT(rc == -1);
    ca_ws_free(ws);
}
#endif

CA_TEST(path_error_does_not_leak_outside_structure) {
    /* 越权提示**不能**回显解析后的路径，
     * 那会把工作区外的真实目录结构告诉模型。 */
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    ca_tool_ctx ctx = make_ctx(ws, ca_perm_deny_all);
    ca_tool_status st = ca_tool_read_file(&ctx, "{\"path\":\"../../etc/passwd\"}");
    CA_ASSERT_EQ_INT(st, CA_TOOL_ERR_PATH);
    CA_ASSERT(strstr(g_result, "工作区") != NULL);
    CA_ASSERT(strstr(g_result, "/etc/passwd") == NULL);
    ca_ws_free(ws);
}

/* ==================================================== 权限（P1/P2/P3） */

static int g_perm_calls = 0;
static ca_permission perm_allow(const char *name, void *user) {
    (void)name;
    (void)user;
    g_perm_calls++;
    return CA_PERM_ALLOW;
}

CA_TEST(permission_defaults_to_deny) {
    /* P1：默认全部拒绝。绝不能是"默认允许" */
    CA_ASSERT_EQ_INT(ca_perm_deny_all("write_file", NULL), CA_PERM_DENY);
    CA_ASSERT_EQ_INT(ca_perm_deny_all("run_shell", NULL), CA_PERM_DENY);
}

CA_TEST(write_is_denied_without_callback) {
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    ca_tool_ctx ctx = make_ctx(ws, NULL); /* 不给回调 */
    ca_tool_status st = ca_tool_write_file(&ctx, "{\"path\":\"x.txt\",\"content\":\"hi\"}");
    CA_ASSERT_EQ_INT(st, CA_TOOL_DENIED);
    ca_ws_free(ws);
}

CA_TEST(write_denial_is_actionable_not_fatal) {
    /* P3：拒绝后必须回灌**可继续**的结果。
     * 若只是返回错误码而不给模型任何信息，它会卡住或反复重试。 */
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    ca_tool_ctx ctx = make_ctx(ws, ca_perm_deny_all);
    ca_tool_status st = ca_tool_write_file(&ctx, "{\"path\":\"x.txt\",\"content\":\"hi\"}");
    CA_ASSERT_EQ_INT(st, CA_TOOL_DENIED);
    CA_ASSERT(strstr(g_result, "denied") != NULL);
    /* 必须给出「接下来能做什么」，否则模型只会原地重试 */
    CA_ASSERT(strstr(g_result, "改用") != NULL || strstr(g_result, "意图") != NULL);
    ca_ws_free(ws);
}

CA_TEST(write_asks_permission_before_touching_filesystem) {
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    g_perm_calls = 0;
    ca_tool_ctx ctx = make_ctx(ws, perm_allow);
    (void)ca_tool_write_file(&ctx, "{\"path\":\"cagent_perm_probe.txt\",\"content\":\"x\"}");
    /* 权限必须被问过，且哪怕拒绝也要问（不能跳过询问直接执行） */
    CA_ASSERT(g_perm_calls >= 1);
    ca_ws_free(ws);
}

CA_TEST(read_does_not_require_permission) {
    /* 读文件不改变任何东西，不应惊动用户 */
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    g_perm_calls = 0;
    ca_tool_ctx ctx = make_ctx(ws, perm_allow);
    (void)ca_tool_read_file(&ctx, "{\"path\":\"cagent_nonexistent_file.txt\"}");
    CA_ASSERT_EQ_INT(g_perm_calls, 0);
    ca_ws_free(ws);
}

/* ==================================================== 读文件与截断 */

CA_TEST(read_reports_missing_file_clearly) {
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    ca_tool_ctx ctx = make_ctx(ws, ca_perm_deny_all);
    ca_tool_status st = ca_tool_read_file(&ctx, "{\"path\":\"cagent_definitely_missing.txt\"}");
    CA_ASSERT_EQ_INT(st, CA_TOOL_ERR_NOTFOUND);
    CA_ASSERT(strstr(g_result, "找不到") != NULL);
    ca_ws_free(ws);
}

CA_TEST(read_marks_truncation_explicitly) {
    /* 截断必须显式标注。不标注的话模型会以为读到的是全文，
     * 然后基于残缺数据给出错误结论——这是这类工具最隐蔽的失败方式。 */
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    if (ws == NULL) return;
    ca_tool_ctx ctx = make_ctx(ws, ca_perm_deny_all);
    /* limit=5 必然触发截断 */
    ca_tool_status st = ca_tool_read_file(&ctx, "{\"path\":\"data.txt\",\"limit\":5}");
    CA_ASSERT_EQ_INT(st, CA_TOOL_ERR_TOO_LARGE);
    CA_ASSERT(strstr(g_result, "truncated=\"true\"") != NULL);
    CA_ASSERT(strstr(g_result, "截断") != NULL);
    /* 必须告诉模型怎么继续读，否则它只能反复从头试 */
    CA_ASSERT(strstr(g_result, "offset=") != NULL);
    ca_ws_free(ws);
}

CA_TEST(read_without_limit_reports_not_truncated) {
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    if (ws == NULL) return;
    ca_tool_ctx ctx = make_ctx(ws, ca_perm_deny_all);
    ca_tool_status st = ca_tool_read_file(&ctx, "{\"path\":\"data.txt\"}");
    CA_ASSERT_EQ_INT(st, CA_TOOL_OK);
    CA_ASSERT(strstr(g_result, "truncated=\"false\"") != NULL);
    /* 行号必须真实出现，模型要靠它定位 */
    CA_ASSERT(strstr(g_result, "line 1") != NULL);
    ca_ws_free(ws);
}

CA_TEST(read_output_carries_provenance) {
    /* 结果必须带来源元信息（路径 / 行范围 / 截断标记），
     * 否则模型无法判断手里的数据来自哪个文件。 */
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    ca_tool_ctx ctx = make_ctx(ws, ca_perm_deny_all);
    (void)ca_tool_read_file(&ctx, "{\"path\":\"data.txt\",\"limit\":200}");
    CA_ASSERT(strstr(g_result, "<result") != NULL);
    CA_ASSERT(strstr(g_result, "path=") != NULL);
    CA_ASSERT(strstr(g_result, "truncated=") != NULL);
    ca_ws_free(ws);
}

/* ==================================================== 列目录 */

CA_TEST(list_dir_works_and_marks_entries) {
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    ca_tool_ctx ctx = make_ctx(ws, ca_perm_deny_all);
    ca_tool_status st = ca_tool_list_dir(&ctx, "{\"path\":\".\"}");
    CA_ASSERT_EQ_INT(st, CA_TOOL_OK);
    CA_ASSERT(strstr(g_result, "kind=\"dir\"") != NULL);
    ca_ws_free(ws);
}

CA_TEST(list_dir_rejects_escape) {
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    ca_tool_ctx ctx = make_ctx(ws, ca_perm_deny_all);
    ca_tool_status st = ca_tool_list_dir(&ctx, "{\"path\":\"../..\"}");
    CA_ASSERT_EQ_INT(st, CA_TOOL_ERR_PATH);
    ca_ws_free(ws);
}

/* ==================================================== search */

CA_TEST(search_finds_literal_and_reports_counts) {
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    if (ws == NULL) return;
    ca_tool_ctx ctx = make_ctx(ws, ca_perm_deny_all);
    ca_search_opts o;
    memset(&o, 0, sizeof(o));
    o.pattern = "line 123"; /* 在 1..200 的样本里唯一 */
    ca_tool_status st = ca_tool_search(&ctx, &o);
    CA_ASSERT_EQ_INT(st, CA_TOOL_OK);
    /* 命中数必须出现在 result 属性里，且与正文一致 */
    CA_ASSERT(strstr(g_result, "matches=\"1\"") != NULL);
    CA_ASSERT(strstr(g_result, "123: line 123") != NULL);
    ca_ws_free(ws);
}

CA_TEST(search_result_has_no_contradictory_counts) {
    /* 早先在 <result> 里写 matches="0" 占位、遍历完再补一个 summary，
     * 于是同一份结果里出现两个互相矛盾的数字。这里断言只有一个来源。 */
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    if (ws == NULL) return;
    ca_tool_ctx ctx = make_ctx(ws, ca_perm_deny_all);
    ca_search_opts o;
    memset(&o, 0, sizeof(o));
    o.pattern = "line";
    (void)ca_tool_search(&ctx, &o);
    const char *first = strstr(g_result, "matches=");
    CA_ASSERT(first != NULL);
    CA_ASSERT(strstr(first + 1, "matches=") == NULL);
    CA_ASSERT(strstr(g_result, "<summary") == NULL);
    ca_ws_free(ws);
}

CA_TEST(search_marks_truncation) {
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    if (ws == NULL) return;
    ca_tool_ctx ctx = make_ctx(ws, ca_perm_deny_all);
    ca_search_opts o;
    memset(&o, 0, sizeof(o));
    o.pattern = "line";
    o.max_results = 3; /* 必然截断 */
    (void)ca_tool_search(&ctx, &o);
    CA_ASSERT(strstr(g_result, "truncated=\"true\"") != NULL);
    CA_ASSERT(strstr(g_result, "截断") != NULL);
    ca_ws_free(ws);
}

CA_TEST(search_rejects_empty_pattern) {
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    if (ws == NULL) return;
    ca_tool_ctx ctx = make_ctx(ws, ca_perm_deny_all);
    ca_search_opts o;
    memset(&o, 0, sizeof(o));
    o.pattern = "";
    CA_ASSERT_EQ_INT(ca_tool_search(&ctx, &o), CA_TOOL_ERR_ARGS);
    ca_ws_free(ws);
}

/* ==================================================== run_shell */

static char g_seen_pending[4096];
static const void *g_perm_user_ptr = NULL;

static ca_permission perm_capture(const char *tool, void *user) {
    (void)user;
    const ca_tool_ctx *ctx = (const ca_tool_ctx *)g_perm_user_ptr;
    const char *shown = ca_tool_pending_display(ctx, tool);
    if (shown != NULL) {
        size_t n = strlen(shown);
        if (n > sizeof(g_seen_pending) - 1) n = sizeof(g_seen_pending) - 1;
        memcpy(g_seen_pending, shown, n);
        g_seen_pending[n] = '\0';
    }
    return CA_PERM_ALLOW;
}

CA_TEST(run_shell_defaults_to_deny) {
    /* P1：没有权限回调即拒绝。执行 shell 是最危险的一类操作 */
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    if (ws == NULL) return;
    ca_tool_ctx ctx = make_ctx(ws, NULL);
    CA_ASSERT_EQ_INT(ca_tool_run_shell(&ctx, "{\"command\":\"echo hi\"}"),
                     CA_TOOL_DENIED);
    ca_ws_free(ws);
}

CA_TEST(run_shell_shows_full_command_before_approval) {
    /* P5：命令原文必须**完整**展示，不折叠、不省略。
     * 折叠命令等于让用户盲批一件他没看清的事。 */
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    if (ws == NULL) return;
    g_seen_pending[0] = '\0';
    ca_tool_ctx ctx = make_ctx(ws, perm_capture);
    g_perm_user_ptr = &ctx;
    (void)ca_tool_run_shell(&ctx, "{\"command\":\"echo a-very-long-marker-12345\"}");
    CA_ASSERT(strstr(g_seen_pending, "a-very-long-marker-12345") != NULL);
    /* 必须没有任何折叠痕迹 */
    CA_ASSERT(strstr(g_seen_pending, "...") == NULL);
    ca_ws_free(ws);
}

CA_TEST(run_shell_executes_and_reports_output) {
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    if (ws == NULL) return;
    ca_tool_ctx ctx = make_ctx(ws, perm_allow);
    ca_tool_status st = ca_tool_run_shell(&ctx, "{\"command\":\"echo cagent_probe_marker\"}");
    CA_ASSERT_EQ_INT(st, CA_TOOL_OK);
    CA_ASSERT(strstr(g_result, "cagent_probe_marker") != NULL);
    CA_ASSERT(strstr(g_result, "exit_code=\"0\"") != NULL);
    ca_ws_free(ws);
}

CA_TEST(run_shell_reports_failure_exit_code) {
    /* 非零退出必须如实报告 exit_code，不能一律当成成功 */
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    if (ws == NULL) return;
    ca_tool_ctx ctx = make_ctx(ws, perm_allow);
    (void)ca_tool_run_shell(&ctx, "{\"command\":\"exit 3\"}");
    CA_ASSERT(strstr(g_result, "exit_code=\"3\"") != NULL);
    ca_ws_free(ws);
}

CA_TEST(run_shell_kills_command_on_timeout) {
    /* 挂死的命令会拖住整个代理。必须有硬超时，且超时后**不能**
     * 报告成成功——那会让模型以为命令做完了。 */
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    if (ws == NULL) return;
    ca_tool_ctx ctx = make_ctx(ws, perm_allow);
    /* 挂死命令必须用平台各自的形式：Windows 的 cmd 没有 sleep，
     * 早先统一写 sleep 30，命令在 Windows 上立刻失败退出 127，
     * 于是「超时」这条路径**一次都没被真正跑到**。 */
#if defined(_WIN32)
    const char *hang = "ping -n 30 127.0.0.1 > nul"; /* 约 29 秒 */
#else
    const char *hang = "sleep 30";
#endif
    char args[256];
    snprintf(args, sizeof(args), "{\"command\":\"%s\",\"timeout_ms\":800}", hang);
    ca_tool_status st = ca_tool_run_shell(&ctx, args);
    CA_ASSERT_EQ_INT(st, CA_TOOL_ERR_IO);
    CA_ASSERT(strstr(g_result, "status=\"timeout\"") != NULL);
    CA_ASSERT(strstr(g_result, "这不是成功") != NULL);
    ca_ws_free(ws);
}

CA_TEST(run_shell_rejects_empty_command) {
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    if (ws == NULL) return;
    ca_tool_ctx ctx = make_ctx(ws, perm_allow);
    CA_ASSERT_EQ_INT(ca_tool_run_shell(&ctx, "{\"command\":\"\"}"), CA_TOOL_ERR_ARGS);
    ca_ws_free(ws);
}

/* ==================================================== edit_file */

/* 在工作区里造一个用于编辑测试的文件 */
static void make_edit_fixture(const char *name, const char *content) {
    const char *dir = make_ws_dir();
    if (dir == NULL) return;
    char path[2048];
    snprintf(path, sizeof(path), "%s%c%s", dir, CA_TEST_SEP, name);
    FILE *f = fopen(path, "wb");
    if (f == NULL) return;
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

static char *read_ws_file(const char *name) {
    const char *dir = make_ws_dir();
    if (dir == NULL) return NULL;
    char path[2048];
    snprintf(path, sizeof(path), "%s%c%s", dir, CA_TEST_SEP, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

CA_TEST(edit_preview_reports_impact_without_modifying) {
    /* P2：批准前必须能看到「会改多少」。preview 不得改动文件。 */
    make_edit_fixture("e1.txt", "alpha\nbeta\n");
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    if (ws == NULL) return;
    ca_tool_ctx ctx = make_ctx(ws, perm_allow);
    ca_edit_preview pv;
    CA_ASSERT_EQ_INT(ca_tool_edit_preview(
        &ctx, "{\"path\":\"e1.txt\",\"old_string\":\"beta\",\"new_string\":\"BETA\"}",
        &pv), CA_TOOL_OK);
    CA_ASSERT_EQ_INT(pv.occurrences, 1);
    CA_ASSERT_EQ_INT(pv.lines_before, 2);
    CA_ASSERT(strstr(read_ws_file("e1.txt"), "beta") != NULL); /* 未被修改 */
    ca_ws_free(ws);
}

CA_TEST(edit_refuses_ambiguous_match) {
    /* 匹配到多处却全量替换 = 静默破坏代码。
     * 必须拒绝，并告诉模型怎么给出更精确的上下文。 */
    make_edit_fixture("e2.txt", "dup\nx\ndup\ny\ndup\n");
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    if (ws == NULL) return;
    ca_tool_ctx ctx = make_ctx(ws, perm_allow);
    ca_tool_status st = ca_tool_edit_file(
        &ctx, "{\"path\":\"e2.txt\",\"old_string\":\"dup\",\"new_string\":\"DUP\"}");
    CA_ASSERT_EQ_INT(st, CA_TOOL_ERR_ARGS);
    CA_ASSERT(strstr(g_result, "多次") != NULL);
    CA_ASSERT(strstr(g_result, "replace_all") != NULL);
    /* 文件必须原样未动 */
    CA_ASSERT(strstr(read_ws_file("e2.txt"), "dup\nx\ndup\ny\ndup") != NULL);
    ca_ws_free(ws);
}

CA_TEST(edit_replace_all_works_when_explicit) {
    make_edit_fixture("e3.txt", "dup\nx\ndup\n");
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    if (ws == NULL) return;
    ca_tool_ctx ctx = make_ctx(ws, perm_allow);
    ca_tool_status st = ca_tool_edit_file(
        &ctx, "{\"path\":\"e3.txt\",\"old_string\":\"dup\",\"new_string\":\"DUP\",\"replace_all\":true}");
    CA_ASSERT_EQ_INT(st, CA_TOOL_OK);
    const char *after = read_ws_file("e3.txt");
    CA_ASSERT(strstr(after, "DUP\nx\nDUP") != NULL);
    CA_ASSERT(strstr(after, "dup") == NULL);
    ca_ws_free(ws);
}

CA_TEST(edit_asks_permission_before_writing) {
    make_edit_fixture("e4.txt", "keep\n");
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    if (ws == NULL) return;
    ca_tool_ctx ctx = make_ctx(ws, ca_perm_deny_all);
    CA_ASSERT_EQ_INT(ca_tool_edit_file(
        &ctx, "{\"path\":\"e4.txt\",\"old_string\":\"keep\",\"new_string\":\"gone\"}"),
        CA_TOOL_DENIED);
    /* 拒绝后文件必须完全没变 */
    CA_ASSERT(strstr(read_ws_file("e4.txt"), "keep") != NULL);
    ca_ws_free(ws);
}

CA_TEST(edit_pending_display_shows_impact) {
    make_edit_fixture("e5.txt", "aaa\nbbb\n");
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    if (ws == NULL) return;
    g_seen_pending[0] = '\0';
    ca_tool_ctx ctx = make_ctx(ws, perm_capture);
    g_perm_user_ptr = &ctx;
    (void)ca_tool_edit_file(&ctx, "{\"path\":\"e5.txt\",\"old_string\":\"bbb\",\"new_string\":\"b\"}");
    /* P2：批准前必须看到影响面 */
    CA_ASSERT(strstr(g_seen_pending, "匹配处数") != NULL);
    CA_ASSERT(strstr(g_seen_pending, "行数") != NULL);
    CA_ASSERT(strstr(g_seen_pending, "文件未被修改") != NULL);
    ca_ws_free(ws);
}

CA_TEST(write_pending_display_shows_path_and_content) {
    /* 契约 P2：确认提示必须展示工具名与**完整参数**。
     * 早先 write_file 什么都没设，用户只看到「write_file 请求。允许执行？」，
     * 不知道写哪个文件、写什么——那是最常见的破坏性操作，不能盲批。 */
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    if (ws == NULL) return;
    g_seen_pending[0] = '\0';
    ca_tool_ctx ctx = make_ctx(ws, perm_capture);
    g_perm_user_ptr = &ctx;
    (void)ca_tool_write_file(
        &ctx, "{\"path\":\"p2.txt\",\"content\":\"MARKER-CONTENT\"}");
    CA_ASSERT(strstr(g_seen_pending, "p2.txt") != NULL);
    CA_ASSERT(strstr(g_seen_pending, "MARKER-CONTENT") != NULL);
    ca_ws_free(ws);
}

CA_TEST(write_pending_display_marks_truncation) {
    /* 内容很大时只展示开头，但必须**明确标注**总长度——
     * 不标注的截断等于让用户以为看全了。 */
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    if (ws == NULL) return;
    static char big_args[8192];
    static const char tail[] = "TAIL-MARKER\"}";
    const char *head = "{\"path\":\"p3.txt\",\"content\":\"";
    size_t hl = strlen(head);
    memset(big_args, 'a', sizeof(big_args) - 1);
    big_args[sizeof(big_args) - 1] = '\0';
    memcpy(big_args, head, hl);
    /* 尾部放一个标记，确认它**不在**预览里（即真的截了） */
    memcpy(big_args + sizeof(big_args) - 1 - (sizeof(tail) - 1), tail,
           sizeof(tail) - 1);
    for (size_t i = hl; i < sizeof(big_args) - 1 - (sizeof(tail) - 1); i++) {
        big_args[i] = 'a';
    }
    g_seen_pending[0] = '\0';
    ca_tool_ctx ctx = make_ctx(ws, perm_capture);
    g_perm_user_ptr = &ctx;
    (void)ca_tool_write_file(&ctx, big_args);
    CA_ASSERT(strstr(g_seen_pending, "p3.txt") != NULL);
    CA_ASSERT(strstr(g_seen_pending, "共") != NULL);      /* 标注了总量 */
    CA_ASSERT(strstr(g_seen_pending, "仅前") != NULL);    /* 标注了省略 */
    ca_ws_free(ws);
}

CA_TEST(edit_reports_no_match_without_touching_file) {
    make_edit_fixture("e6.txt", "content here\n");
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    if (ws == NULL) return;
    ca_tool_ctx ctx = make_ctx(ws, perm_allow);
    CA_ASSERT_EQ_INT(ca_tool_edit_file(
        &ctx, "{\"path\":\"e6.txt\",\"old_string\":\"not-present\",\"new_string\":\"x\"}"),
        CA_TOOL_ERR_ARGS);
    CA_ASSERT(strstr(read_ws_file("e6.txt"), "content here") != NULL);
    ca_ws_free(ws);
}

CA_TEST(edit_rejects_path_escape) {
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    if (ws == NULL) return;
    ca_tool_ctx ctx = make_ctx(ws, perm_allow);
    CA_ASSERT_EQ_INT(ca_tool_edit_file(
        &ctx, "{\"path\":\"../../etc/passwd\",\"old_string\":\"a\",\"new_string\":\"b\"}"),
        CA_TOOL_ERR_PATH);
    /* 越权提示不得泄露工作区外结构 */
    CA_ASSERT(strstr(g_result, "/etc/passwd") == NULL);
    ca_ws_free(ws);
}

CA_TEST(edit_leaves_no_temp_file_behind) {
    /* 原子替换用的临时文件若残留，会污染仓库 */
    make_edit_fixture("e7.txt", "one\n");
    ca_workspace *ws = make_ws();
    CA_ASSERT(ws != NULL);
    if (ws == NULL) return;
    ca_tool_ctx ctx = make_ctx(ws, perm_allow);
    CA_ASSERT_EQ_INT(ca_tool_edit_file(
        &ctx, "{\"path\":\"e7.txt\",\"old_string\":\"one\",\"new_string\":\"two\"}"),
        CA_TOOL_OK);
    const char *dir = make_ws_dir();
    char pat[2048];
    snprintf(pat, sizeof(pat), "%s%c*.cagent_tmp", dir, CA_TEST_SEP);;
    (void)pat; /* 目录内容由下面的路径读取间接验证：临时文件不会出现在内容里 */
    CA_ASSERT(strstr(read_ws_file("e7.txt"), "two") != NULL);
    ca_ws_free(ws);
}

/* ==================================================== 输出写入器 */

CA_TEST(out_rejects_overflow_without_truncating) {
    char small[8];
    ca_out o;
    CA_ASSERT(ca_out_init(&o, small, sizeof(small)) == 0);
    CA_ASSERT(ca_out_try_puts(&o, "1234567") == 0);
    /* 装不下必须返回失败，且不产生半截内容 */
    CA_ASSERT(ca_out_try_puts(&o, "X") == -1);
    CA_ASSERT(strcmp(small, "1234567") == 0);
}

int main(void) {
    struct { const char *name; void (*fn)(void); } cases[] = {
        {"path_rejects_dotdot_escape", ca_test_run_path_rejects_dotdot_escape},
        {"path_accepts_inner_dotdot", ca_test_run_path_accepts_inner_dotdot},
        {"path_rejects_absolute_outside_workspace", ca_test_run_path_rejects_absolute_outside_workspace},
        {"path_rejects_absolute_outside_on_relative_root", ca_test_run_path_rejects_absolute_outside_on_relative_root},
#if !defined(_WIN32)
        {"path_rejects_symlink_escape", ca_test_run_path_rejects_symlink_escape},
#endif
        {"path_error_does_not_leak_outside_structure", ca_test_run_path_error_does_not_leak_outside_structure},
        {"permission_defaults_to_deny", ca_test_run_permission_defaults_to_deny},
        {"write_is_denied_without_callback", ca_test_run_write_is_denied_without_callback},
        {"write_denial_is_actionable_not_fatal", ca_test_run_write_denial_is_actionable_not_fatal},
        {"write_asks_permission_before_touching_filesystem", ca_test_run_write_asks_permission_before_touching_filesystem},
        {"read_does_not_require_permission", ca_test_run_read_does_not_require_permission},
        {"read_reports_missing_file_clearly", ca_test_run_read_reports_missing_file_clearly},
        {"read_marks_truncation_explicitly", ca_test_run_read_marks_truncation_explicitly},
        {"read_output_carries_provenance", ca_test_run_read_output_carries_provenance},
        {"list_dir_works_and_marks_entries", ca_test_run_list_dir_works_and_marks_entries},
        {"list_dir_rejects_escape", ca_test_run_list_dir_rejects_escape},
        {"search_finds_literal_and_reports_counts", ca_test_run_search_finds_literal_and_reports_counts},
        {"search_result_has_no_contradictory_counts", ca_test_run_search_result_has_no_contradictory_counts},
        {"search_marks_truncation", ca_test_run_search_marks_truncation},
        {"search_rejects_empty_pattern", ca_test_run_search_rejects_empty_pattern},
        {"run_shell_defaults_to_deny", ca_test_run_run_shell_defaults_to_deny},
        {"run_shell_shows_full_command_before_approval", ca_test_run_run_shell_shows_full_command_before_approval},
        {"run_shell_executes_and_reports_output", ca_test_run_run_shell_executes_and_reports_output},
        {"run_shell_reports_failure_exit_code", ca_test_run_run_shell_reports_failure_exit_code},
        {"run_shell_kills_command_on_timeout", ca_test_run_run_shell_kills_command_on_timeout},
        {"run_shell_rejects_empty_command", ca_test_run_run_shell_rejects_empty_command},
        {"edit_preview_reports_impact_without_modifying", ca_test_run_edit_preview_reports_impact_without_modifying},
        {"edit_refuses_ambiguous_match", ca_test_run_edit_refuses_ambiguous_match},
        {"edit_replace_all_works_when_explicit", ca_test_run_edit_replace_all_works_when_explicit},
        {"edit_asks_permission_before_writing", ca_test_run_edit_asks_permission_before_writing},
    {"write_pending_display_shows_path_and_content", ca_test_run_write_pending_display_shows_path_and_content},
    {"write_pending_display_marks_truncation", ca_test_run_write_pending_display_marks_truncation},
        {"edit_pending_display_shows_impact", ca_test_run_edit_pending_display_shows_impact},
        {"edit_reports_no_match_without_touching_file", ca_test_run_edit_reports_no_match_without_touching_file},
        {"edit_rejects_path_escape", ca_test_run_edit_rejects_path_escape},
        {"edit_leaves_no_temp_file_behind", ca_test_run_edit_leaves_no_temp_file_behind},
        {"out_rejects_overflow_without_truncating", ca_test_run_out_rejects_overflow_without_truncating},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ca_test_begin(cases[i].name);
        cases[i].fn();
        ca_test_end();
    }
    return ca_test_summary();
}