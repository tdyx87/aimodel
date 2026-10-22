/* tools_shell.c —— search 与 run_shell
 * 契约：docs/design/contract.md §5.1 / §5.2（P5）
 *
 * 从 tools.c 拆出来不是因为它更复杂，而是因为**平台相关代码集中在一起
 * 更好审计**：这一层处理进程创建、管道、超时、目录遍历——任何一处出错
 * 都可能是安全问题或挂死问题，而它们全挤在一个文件里时很难看出边界。
 */

/* fopen 没有 C11 可移植的安全等价物（fopen_s 仅 MSVC 有）。
 * 与 tools.c 同一处理：只对这一条关闭建议。
 * 必须定义在**任何** CRT 头文件之前，否则不生效。 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "cagent/buf.h"
#include "cagent/json.h"
#include "cagent/tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* S_ISDIR / S_ISREG 来自这里。没包含就直接用，在 glibc 下编译期无警告、
 * 链接期才炸（undefined reference）——一个只在非 Windows 平台才暴露的洞。 */
#if !defined(_WIN32)
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <dirent.h>
#endif

#include "cagent/json.h"

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dirent.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <signal.h>
#  include <sys/wait.h>
#  include <time.h>  /* nanosleep：等子进程真正退出（见退出码那段） */
#  include <unistd.h>
#endif

/* ============================================================ 通用工具 */

static int arg_string2(const char *json, const char *key, char *out, size_t cap) {
    out[0] = '\0';
    ca_json_cursor root, v;
    ca_json_init(&root, json, strlen(json));
    if (ca_json_enter_object(&root) != 0) return -1;
    if (ca_json_field(&root, key, &v) != 1) return 0;
    if (ca_json_read_string(&v, out, cap, NULL) != 0) return -1;
    return 0;
}

static int arg_int2(const char *json, const char *key, long long *out) {
    ca_json_cursor root, v;
    ca_json_init(&root, json, strlen(json));
    if (ca_json_enter_object(&root) != 0) return -1;
    if (ca_json_field(&root, key, &v) != 1) return 0;
    if (ca_json_read_int(&v, out) != 0) return -1;
    return 0;
}

/* 判断权限。P1：没有回调即拒绝。
 *
 * 返回 ca_permission 而不是 0/1：**拒绝原因要能传下去**。
 * 「用户拒绝」与「策略拒绝」对模型是不同的信息——
 * 前者换个做法可以再来，后者重试多少次都没用。 */
static ca_permission ask_perm(ca_tool_ctx *ctx, const char *tool) {
    if (ctx == NULL || ctx->perm == NULL) return CA_PERM_DENY;
    return ctx->perm(tool, ctx->perm_user);
}

/* ============================================================ search */

/* 常见噪声目录：搜代码时它们既慢又几乎不会有命中 */
static const char *const k_skip_dirs[] = {
    ".git", ".svn", ".hg", "node_modules", "__pycache__",
    "build", "out", "dist", "cmake-build-debug", ".venv", "venv",
};
static const char *const k_skip_exts[] = {
    ".exe", ".dll", ".so", ".dylib", ".lib", ".a", ".o", ".obj",
    ".png", ".jpg", ".jpeg", ".gif", ".ico", ".pdf", ".zip", ".gz", ".7z",
};

static int should_skip_dir(const char *name) {
    for (size_t i = 0; i < sizeof(k_skip_dirs) / sizeof(k_skip_dirs[0]); i++) {
        if (strcmp(name, k_skip_dirs[i]) == 0) return 1;
    }
    return 0;
}

static int should_skip_file(const char *name) {
    size_t n = strlen(name);
    for (size_t i = 0; i < sizeof(k_skip_exts) / sizeof(k_skip_exts[0]); i++) {
        size_t en = strlen(k_skip_exts[i]);
        if (n > en && strcmp(name + n - en, k_skip_exts[i]) == 0) return 1;
    }
    return 0;
}

typedef struct {
    ca_out *out;
    const char *pattern;
    const char *glob;
    int max_results;
    int found;
    int truncated;
    int files_with_hits;
    /* 跳过的二进制文件数。二进制里按行找字面串必然匹配不到，
     * 静默跳过会让模型以为「这里没有这段代码」——用户实测就撞上了：
     * 在只有 .exe/.lib 的目录里搜 "cmake_minimum_required" 得到 0，
     * 搜 "." 也得到 0，模型据此以为目录里没有源码。必须说清楚。 */
    int skipped_binary;
} search_state;

/* 在单个文件里逐行找字面串。返回 -1 表示读失败（跳过该文件）。 */
static void search_file(search_state *st, const char *abs, const char *rel) {
    FILE *f = ca_tool_fopen(abs, "rb");
    if (f == NULL) return;
    {
        /* 二进制判定：前 4 KB 里出现 NUL 就算二进制。
         * 文本文件不该含 NUL；二进制按行读会读出乱码行，匹配毫无意义。 */
        char probe[4096];
        size_t n = fread(probe, 1, sizeof(probe), f);
        int binary = 0;
        for (size_t i = 0; i < n; i++) {
            if (probe[i] == '\0') { binary = 1; break; }
        }
        if (binary) {
            st->skipped_binary++;
            fclose(f);
            return;
        }
        rewind(f);
    }
char line[8192];
        size_t pat_len = strlen(st->pattern);
        long line_no = 0;
        int matched_in_file = 0;

        while (fgets(line, (int)sizeof(line), f) != NULL) {
        line_no++;
        if (pat_len == 0) break;
        /* 二分式朴素查找：先用首字符筛，再 memcmp。 */
        char *hit = NULL;
        for (char *p = line; (p = strchr(p, st->pattern[0])) != NULL; p++) {
            if (memcmp(p, st->pattern, pat_len) == 0) { hit = p; break; }
        }
        if (hit == NULL) continue;

        if (st->found >= st->max_results) {
            st->truncated = 1;
            break;
        }
        if (!matched_in_file) {
            matched_in_file = 1;
            st->files_with_hits++;
            ca_out_printf(st->out, "\n%s\n", rel);
        }
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
        ca_out_printf(st->out, "  %ld: %.*s\n", line_no, (int)len, line);
        st->found++;
    }
    fclose(f);
}

static void search_dir(search_state *st, const char *base, const char *rel, int depth) {
    if (depth > 24) return; /* 防目录炸弹 */
#if defined(_WIN32)
    /* 宽字符 API + UTF-8 转换：窄字符版本（FindFirstFileA）返回的是
     * **系统 ANSI 代码页**字节（中文机器是 GBK），中文目录/文件名会变成
     * 乱码，既搜不到也显示不对。 */
    char pattern[2048];
    snprintf(pattern, sizeof(pattern), "%s\\*", base);
    WCHAR wpattern[2048];
    if (MultiByteToWideChar(CP_UTF8, 0, pattern, -1, wpattern,
                            (int)(sizeof(wpattern) / sizeof(wpattern[0]))) == 0) {
        return;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char name[2048];
        if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name,
                                (int)sizeof(name), NULL, NULL) == 0) {
            continue;
        }
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        char child[2048];
        snprintf(child, sizeof(child), "%s\\%s", base, name);
        char childrel[2048];
        if (rel[0] == '\0') {
            snprintf(childrel, sizeof(childrel), "%s", name);
        } else {
            snprintf(childrel, sizeof(childrel), "%s/%s", rel, name);
        }
        if (is_dir) {
            if (!should_skip_dir(name)) search_dir(st, child, childrel, depth + 1);
        } else {
            if (should_skip_file(name)) continue;
            if (st->glob != NULL && st->glob[0] != '\0' &&
                strstr(name, st->glob) == NULL) continue;
            search_file(st, child, childrel);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(base);
    if (d == NULL) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[2048];
        snprintf(child, sizeof(child), "%s/%s", base, ent->d_name);
        char childrel[2048];
        if (rel[0] == '\0') {
            snprintf(childrel, sizeof(childrel), "%s", ent->d_name);
        } else {
            snprintf(childrel, sizeof(childrel), "%s/%s", rel, ent->d_name);
        }
        struct stat sb;
        if (stat(child, &sb) != 0) continue;
        if (S_ISDIR(sb.st_mode)) {
            if (!should_skip_dir(ent->d_name)) search_dir(st, child, childrel, depth + 1);
        } else if (S_ISREG(sb.st_mode)) {
            if (should_skip_file(ent->d_name)) continue;
            if (st->glob != NULL && st->glob[0] != '\0' &&
                strstr(ent->d_name, st->glob) == NULL) continue;
            search_file(st, child, childrel);
        }
    }
    closedir(d);
#endif
}

/* 从模型参数直接执行 search。
 *
 * 为什么需要这一层：`ca_tool_search` 收的是结构体，而模型给的是 JSON。
 * core 的分发表按 JSON 调工具，没有这个入口，search 就**永远走不到**
 * ——函数和单测都在，用户却用不上（契约 §5.1 的六工具只剩五个）。
 * 参数解析放在这里，与 run_shell 用同一套 arg_* 辅助函数。 */
ca_tool_status ca_tool_search_json(ca_tool_ctx *ctx, const char *args_json) {
    if (ctx == NULL || args_json == NULL) return CA_TOOL_ERR_ARGS;
    char pattern[2048];
    char glob[256];
    long long max_results = 0;
    if (arg_string2(args_json, "pattern", pattern, sizeof(pattern)) != 0) {
        ca_out_puts(ctx->out,
                    "<result status=\"error\" error=\"缺少 pattern 参数\">"
                    "未执行搜索。</result>");
        return CA_TOOL_ERR_ARGS;
    }
    glob[0] = '\0';
    (void)arg_string2(args_json, "glob", glob, sizeof(glob));
    (void)arg_int2(args_json, "max_results", &max_results);

    ca_search_opts opts;
    opts.pattern = pattern;
    opts.glob = (glob[0] != '\0') ? glob : NULL;
    opts.max_results = (max_results > 0 && max_results < 100000)
                           ? (int)max_results : 0;
    return ca_tool_search(ctx, &opts);
}

ca_tool_status ca_tool_search(ca_tool_ctx *ctx, const ca_search_opts *opts) {
    if (ctx == NULL || ctx->out == NULL || opts == NULL) return CA_TOOL_ERR_ARGS;
    if (opts->pattern == NULL || opts->pattern[0] == '\0') {
        ca_out_puts(ctx->out,
                    "<result status=\"error\" error=\"搜索内容为空\">"
                    "请给出要查找的字面内容。</result>");
        return CA_TOOL_ERR_ARGS;
    }
    /* max_results <=0 用默认值。不在调用点构造临时复合字面量：
     * 那种写法容易写出「字段数与结构体不符」的初始化（C2078）。 */
    int max_results = opts->max_results;
    if (max_results <= 0) max_results = 200;

    /* 搜索只读，不惊动用户 */
    char root[2048];
    size_t need = 0;
    if (ca_ws_resolve(ctx->ws, ".", root, sizeof(root), &need) != 0) {
        ca_out_puts(ctx->out, "<result status=\"error\" error=\"无法定位工作区\"></result>");
        return CA_TOOL_ERR_PATH;
    }

    /* 命中数在遍历完才知道，所以**先收集再输出**。
     * 早先在 <result> 里先写 matches="0" 占位、遍历完再补一个 summary ——
     * 于是同一份结果里出现两个互相矛盾的数字，模型读到哪个都可能出错。
     *
     * 收集缓冲必须**堆分配**：1 MB 放栈上会直接爆栈
     * （MSVC 默认栈就是 1 MB）。 */
    char *body = (char *)malloc(CA_TOOL_RESULT_LIMIT);
    if (body == NULL) return CA_TOOL_ERR_TOO_LARGE;
    ca_out body_out;
    ca_out_init(&body_out, body, CA_TOOL_RESULT_LIMIT);

    search_state st;
    st.out = &body_out;
    st.pattern = opts->pattern;
    st.glob = opts->glob;
    st.max_results = max_results;
    st.found = 0;
    st.truncated = 0;
    st.files_with_hits = 0;
    st.skipped_binary = 0;

    search_dir(&st, root, "", 0);

    ca_out_printf(ctx->out,
                  "<result tool=\"search\" query=\"%s\" matches=\"%d\" "
                  "files=\"%d\" truncated=\"%s\" skipped_binary=\"%d\">\n",
                  opts->pattern, st.found, st.files_with_hits,
                  st.truncated ? "true" : "false", st.skipped_binary);
    if (body_out.len > 0) ca_out_puts(ctx->out, body);
    if (st.truncated) {
        ca_out_puts(ctx->out,
                    "\n<!-- 结果已按上限截断，以上不是全部命中。"
                    "请缩小搜索范围或换一个更具体的关键词。 -->\n");
    }
    if (st.skipped_binary > 0) {
        /* 二进制文件被跳过了，必须说出来：
         * 否则「0 匹配」会被读成「这里没有这段代码」——
         * 而真相是「这些文件是二进制，搜不了」。 */
        ca_out_printf(ctx->out,
                      "\n<!-- 已跳过 %d 个二进制文件（.exe/.lib/.o 等），"
                      "它们不参与字面搜索；要找源码请在源码目录里搜。 -->\n",
                      st.skipped_binary);
    }
    ca_out_puts(ctx->out, "</result>\n");
    free(body);
    return CA_TOOL_OK;
}

/* ============================================================ run_shell */

#if defined(_WIN32)

ca_tool_status ca_tool_run_shell(ca_tool_ctx *ctx, const char *args_json) {
    char command[4096];
    if (arg_string2(args_json, "command", command, sizeof(command)) != 0) {
        return CA_TOOL_ERR_ARGS;
    }
    if (command[0] == '\0') {
        ca_out_puts(ctx->out, "<result status=\"error\" error=\"命令为空\"></result>");
        return CA_TOOL_ERR_ARGS;
    }
    long long timeout = 0;
    (void)arg_int2(args_json, "timeout_ms", &timeout);
    if (timeout <= 0) timeout = CA_SHELL_TIMEOUT_MS;

    /* P5：把**完整命令原文**交给权限界面。不截断、不加省略号——
     * 折叠命令等于让用户盲批一件他没看清的事。 */
    static char pending[4200];
    snprintf(pending, sizeof(pending),
             "将执行以下命令（原文完整展示）：\n\n%s\n", command);
    ctx->pending_display = pending;

    /* P1 + P5：先问权限，且必须在解析之前 —— 不能先执行了才问。 */
    ca_permission verdict = ask_perm(ctx, "run_shell");
    if (verdict != CA_PERM_ALLOW) {
        ctx->pending_display = NULL;
        /* 策略拒绝 ≠ 用户拒绝：策略拒绝时用户按 y 也没用，
         * 继续重试只是白费一次批准。 */
        if (verdict == CA_PERM_DENY_POLICY) {
            ca_out_puts(ctx->out,
                        "<result status=\"denied_by_policy\" tool=\"run_shell\">"
                        "策略不允许执行命令（用户即使批准也不会执行）。"
                        "可以先说明这条命令做什么，"
                        "或改用不执行命令的方式。</result>");
        } else {
            ca_out_puts(ctx->out,
                        "<result status=\"denied\" tool=\"run_shell\">"
                        "用户拒绝了执行命令。可以先说明这条命令做什么，"
                        "或改用不执行命令的方式。</result>");
        }
        return CA_TOOL_DENIED;
    }
    ctx->pending_display = NULL;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return CA_TOOL_ERR_IO;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    char cmdline[4200];
    snprintf(cmdline, sizeof(cmdline), "cmd.exe /c %s", command);

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        ca_out_puts(ctx->out,
                    "<result status=\"error\" error=\"无法启动命令\">"
                    "系统拒绝创建该进程。</result>");
        return CA_TOOL_ERR_IO;
    }
    CloseHandle(wr);

    /* 读取输出到上限为止，并按超时轮询。
     * 挂死的命令会拖住整个代理，必须有硬超时。 */
    char buf[8192];
    char *out_buf = (char *)malloc(CA_TOOL_RESULT_LIMIT + 1);
    size_t out_len = 0;
    int overflow = 0;
    int timed_out = 0;
    DWORD waited = 0;
    const DWORD slice = 50;

    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            DWORD want = avail;
            if (want > sizeof(buf)) want = (DWORD)sizeof(buf);
            DWORD got = 0;
            if (ReadFile(rd, buf, want, &got, NULL) && got > 0) {
                for (DWORD i = 0; i < got; i++) {
                    if (out_len < CA_TOOL_RESULT_LIMIT) out_buf[out_len++] = buf[i];
                    else overflow = 1;
                }
            }
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, slice) == WAIT_OBJECT_0) {
            /* 进程结束，再把管道里剩下的读干净 */
            DWORD rest = 0;
            if (PeekNamedPipe(rd, NULL, 0, NULL, &rest, NULL) && rest > 0) {
                DWORD want = rest;
                if (want > sizeof(buf)) want = (DWORD)sizeof(buf);
                DWORD got = 0;
                if (ReadFile(rd, buf, want, &got, NULL) && got > 0) {
                    for (DWORD i = 0; i < got; i++) {
                        if (out_len < CA_TOOL_RESULT_LIMIT) out_buf[out_len++] = buf[i];
                        else overflow = 1;
                    }
                }
                continue;
            }
            break;
        }
        waited += slice;
        if ((long long)waited >= timeout) {
            timed_out = 1;
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            break;
        }
    }
    CloseHandle(rd);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    out_buf[out_len] = '\0';

    ca_out_printf(ctx->out,
                  "<result tool=\"run_shell\" status=\"%s\" exit_code=\"%d\" "
                  "timeout=\"%s\" truncated=\"%s\">\n",
                  timed_out ? "timeout" : "ok", (int)code,
                  timed_out ? "true" : "false", overflow ? "true" : "false");
    if (out_len > 0) ca_out_puts(ctx->out, out_buf);
    if (overflow) {
        ca_out_puts(ctx->out,
                    "\n<!-- 输出超出上限已截断。如需完整输出，"
                    "请把命令改得更精确，或把输出写入文件再读。 -->\n");
    }
    if (timed_out) {
        ca_out_printf(ctx->out,
                      "\n<!-- 命令超过 %lld 毫秒上限，已终止。"
                      "这不是成功——请检查命令是否在等待输入。 -->\n", timeout);
    }
    ca_out_puts(ctx->out, "</result>\n");
    free(out_buf);
    return timed_out ? CA_TOOL_ERR_IO : CA_TOOL_OK;
}

#else /* POSIX */

ca_tool_status ca_tool_run_shell(ca_tool_ctx *ctx, const char *args_json) {
    char command[4096];
    if (arg_string2(args_json, "command", command, sizeof(command)) != 0) {
        return CA_TOOL_ERR_ARGS;
    }
    if (command[0] == '\0') {
        ca_out_puts(ctx->out, "<result status=\"error\" error=\"命令为空\"></result>");
        return CA_TOOL_ERR_ARGS;
    }
    long long timeout = 0;
    (void)arg_int2(args_json, "timeout_ms", &timeout);
    if (timeout <= 0) timeout = CA_SHELL_TIMEOUT_MS;

    /* P5：命令**原文**必须在请求权限**之前**完整展示。
     * 这一段早先整段缺失 —— POSIX 版直接 ask_perm()，
     * 于是 Linux/macOS 上用户是在**什么都没看到**的情况下
     * 批准一条将要执行的 shell 命令。盲批。
     *
     * 与我给 edit_file 的 ca_tool_pending_display 白名单是同一类洞：
     * 一个平台实现漏了这一步，而本地只有 MSVC，永远看不见。
     */
    static char pending[4200];
    snprintf(pending, sizeof(pending),
             "将执行以下命令（原文完整展示，不折叠）：\n\n%s\n", command);
    ctx->pending_display = pending;

    ca_permission verdict = ask_perm(ctx, "run_shell");
    if (verdict != CA_PERM_ALLOW) {
        ctx->pending_display = NULL;
        /* 策略拒绝 ≠ 用户拒绝：策略拒绝时用户按 y 也没用，
         * 继续重试只是白费一次批准。 */
        if (verdict == CA_PERM_DENY_POLICY) {
            ca_out_puts(ctx->out,
                        "<result status=\"denied_by_policy\" tool=\"run_shell\">"
                        "策略不允许执行命令（用户即使批准也不会执行）。"
                        "可以先说明这条命令做什么，"
                        "或改用不执行命令的方式。</result>");
        } else {
            ca_out_puts(ctx->out,
                        "<result status=\"denied\" tool=\"run_shell\">"
                        "用户拒绝了执行命令。可以先说明这条命令做什么，"
                        "或改用不执行命令的方式。</result>");
        }
        return CA_TOOL_DENIED;
    }
    ctx->pending_display = NULL;

    int fds[2];
    if (pipe(fds) != 0) return CA_TOOL_ERR_IO;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return CA_TOOL_ERR_IO;
    }
    if (pid == 0) {
        /* 子进程：重定向输出后交给 /bin/sh */
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);

    char *out_buf = (char *)malloc(CA_TOOL_RESULT_LIMIT + 1);
    size_t out_len = 0;
    int overflow = 0;
    int timed_out = 0;
    long long waited = 0;
    /* 退出状态必须声明在**循环外**。
     *
     * 早先它声明在循环体内，于是「子进程在循环里被 waitpid 回收」这一条
     * 路径上，退出码随循环一起消失；循环后又去 waitpid 一次，
     * 而已经被回收的 pid 再 waitpid 只会得到 ECHILD（返回 -1），
     * 局部 status 保持初值 0 → **命令失败了却报 exit_code="0"**。
     *
     * 这是一条**竞态**：命令是否在循环内被回收，取决于管道 EOF 与
     * 进程退出谁先被观察到。macOS CI 上正是这条路径偶发命中，
     * 于是 `exit 3` 偶发被报成 exit_code="0"（用例失败）。
     * 「如实报告命令的退出码」是硬要求，不能看运气。 */
    int status = 0;
    int reaped = 0; /* 子进程是否已被 waitpid 回收 */

    for (;;) {
        struct pollfd pfd;
        pfd.fd = fds[0];
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 50);
        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            char buf[8192];
            ssize_t n = read(fds[0], buf, sizeof(buf));
            if (n > 0) {
                for (ssize_t i = 0; i < n; i++) {
                    if (out_len < CA_TOOL_RESULT_LIMIT) out_buf[out_len++] = buf[i];
                    else overflow = 1;
                }
                continue;
            }
            if (n == 0) break; /* EOF */
        }
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            reaped = 1;
            /* 进程已退出，再把管道读干净 */
            char buf[8192];
            ssize_t n;
            while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
                for (ssize_t i = 0; i < n; i++) {
                    if (out_len < CA_TOOL_RESULT_LIMIT) out_buf[out_len++] = buf[i];
                    else overflow = 1;
                }
            }
            break;
        }
        waited += 50;
        if (waited >= timeout) {
            timed_out = 1;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            reaped = 1;
            break;
        }
    }
    close(fds[0]);
    out_buf[out_len] = '\0';

    int exit_code = -1;
    if (!timed_out) {
        if (reaped) {
            exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        } else {
            /* 走到这里说明是「管道先 EOF」那条路。
             *
             * **EOF 不等于进程已退出**：写端关闭与进程真正死掉之间
             * 有一个小窗口 —— 有的 shell 会先关掉 fd 1，再走完退出流程
             * （收尾、跑 trap）。macOS 的 /bin/sh 是 bash，这个窗口更明显。
             *
             * 早先这里只 WNOHANG 取一次：窗口内就取不到退出码，
             * 于是退出码退化成「未知」（-1），命令明明返回 0 却报不出来。
             * （更早的版本把 status 留在初值 0，于是同一个窗口里
             *  「返回 3」被报成「0」——失败被说成成功。）
             *
             * 现在**有界地等**一小会儿：正常情况下进程在微秒级退出，
             * 立刻就能拿到真实退出码；万一它真的赖着不走，
             * 就如实报「未知」（-1），不猜。
             *
             * 上界 200 ms：超时就放弃，不能让工具挂住。 */
            for (int i = 0; i < 200; i++) {
                pid_t r = waitpid(pid, &status, WNOHANG);
                if (r == pid) {
                    exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                    break;
                }
                if (r < 0) break; /* 出错了，别再等 */
                struct timespec ts;
                ts.tv_sec = 0;
                ts.tv_nsec = 1000L * 1000L; /* 1 ms */
                nanosleep(&ts, NULL);
            }
        }
    }

    ca_out_printf(ctx->out,
                  "<result tool=\"run_shell\" status=\"%s\" exit_code=\"%d\" "
                  "timeout=\"%s\" truncated=\"%s\">\n",
                  timed_out ? "timeout" : "ok", exit_code,
                  timed_out ? "true" : "false", overflow ? "true" : "false");
    if (out_len > 0) ca_out_puts(ctx->out, out_buf);
    if (overflow) {
        ca_out_puts(ctx->out,
                    "\n<!-- 输出超出上限已截断。如需完整输出，"
                    "请把命令改得更精确，或把输出写入文件再读。 -->\n");
    }
    if (timed_out) {
        ca_out_printf(ctx->out,
                      "\n<!-- 命令超过 %lld 毫秒上限，已终止。"
                      "这不是成功——请检查命令是否在等待输入。 -->\n", timeout);
    }
    ca_out_puts(ctx->out, "</result>\n");
    free(out_buf);
    return timed_out ? CA_TOOL_ERR_IO : CA_TOOL_OK;
}

#endif /* _WIN32 */