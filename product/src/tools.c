/* tools.c —— 工具层
 * 契约：docs/design/contract.md §5
 */

/* MSVC 对 fopen/strcpy 等提出 C4996「可能不安全」。
 * 这里的处理方式不是 blanket 屏蔽：
 *   - strcpy/strncpy 一律换成下面的 ca_copy_str（有界、返回成败）
 *   - fopen 没有可移植的等价物（C11 的 fopen_s 仅 MSVC 有），
 *     故只对这一条关闭建议，并在下方注释说明原因。
 *
 * 注意：必须定义在**任何** CRT 头文件之前，否则不生效。
 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "cagent/tools.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#  include <limits.h>
/* realpath() 的目标缓冲区必须至少是 PATH_MAX。
 * POSIX 规定得很明确，而 realpath **不知道你给的缓冲区有多大**，
 * 它会一路写到解析结果的末尾。早先三处都传 CA_PATH_MAX(1024)，
 * 任何超过 1024 的路径都会越界写栈——在 Ubuntu 上表现为
 * tools_test 直接 abort，而崩溃点离真正的错误点很远。
 *
 * 不能用 realpath(path, NULL)（GNU 扩展会自动分配）：
 * macOS 不支持，而 macOS 恰恰是最需要这个修复的平台。 */
#  ifndef CA_POSIX_PATH_MAX
#    ifdef PATH_MAX
#      define CA_POSIX_PATH_MAX PATH_MAX
#    else
#      define CA_POSIX_PATH_MAX 4096
#    endif
#  endif
#endif

#include "cagent/json.h"

/* 有界字符串拷贝：装不下就**明确失败**，不做截断。
 * 截断出来的半个路径比失败危险得多——它会指向另一个文件。 */
static int ca_copy_str(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n + 1 > cap) return -1;
    memcpy(dst, src, n + 1);
    return 0;
}

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <direct.h>
#  include <windows.h>
#else
#  include <dirent.h>
#  include <limits.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

/* ============================================================ 输出 */

int ca_out_init(ca_out *o, char *buf, size_t cap) {
    if (o == NULL || buf == NULL || cap == 0) return -1;
    o->buf = buf;
    o->cap = cap;
    o->len = 0;
    buf[0] = '\0';
    return 0;
}

/* 返回 0 成功，-1 装不下。装不下时**不静默截断**：
 * 上层据此决定是缩减内容还是报错。 */
int ca_out_try_puts(ca_out *o, const char *s) {
    if (o == NULL || s == NULL) return -1;
    size_t n = strlen(s);
    if (o->len + n >= o->cap) return -1;
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = '\0';
    return 0;
}

void ca_out_puts(ca_out *o, const char *s) {
    (void)ca_out_try_puts(o, s);
}

void ca_out_putc(ca_out *o, char c) {
    if (o == NULL || o->len + 1 >= o->cap) return;
    o->buf[o->len++] = c;
    o->buf[o->len] = '\0';
}

void ca_out_printf(ca_out *o, const char *fmt, ...) {
    if (o == NULL || o->len + 1 >= o->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= o->cap - o->len) {
        o->len = o->cap - 1; /* 只保留装得下的部分，调用方须检查返回值语义 */
        o->buf[o->len] = '\0';
    } else {
        o->len += (size_t)n;
    }
}

/* ============================================================ 权限 */

ca_permission ca_perm_deny_all(const char *tool_name, void *user) {
    (void)tool_name;
    (void)user;
    return CA_PERM_DENY; /* P1：默认全部拒绝，不是默认允许 */
}

const char *ca_tool_status_text(ca_tool_status s) {
    switch (s) {
        case CA_TOOL_OK: return "成功";
        case CA_TOOL_DENIED: return "你拒绝了这次操作";
        case CA_TOOL_ERR_PATH: return "路径不在工作区内";
        case CA_TOOL_ERR_IO: return "读写失败";
        case CA_TOOL_ERR_TOO_LARGE: return "内容超过上限，已拒绝";
        case CA_TOOL_ERR_NOTFOUND: return "找不到该文件或目录";
        case CA_TOOL_ERR_ARGS: return "参数无法解析";
        default: return "未知错误";
    }
}

/* ============================================================ 路径安全 */

#define CA_PATH_MAX 1024

struct ca_workspace {
    char root[CA_PATH_MAX];
};

#if defined(_WIN32)
#  define CA_SEP '\\'
static int ca_is_sep(char c) { return c == '/' || c == '\\'; }

/* 输入是否表示绝对路径。
 * Windows 上**必须**单独识别盘符：`C:\Windows` 的首字符是 'C' 不是分隔符，
 * 早先只用 ca_is_sep 判断，于是它被当成相对路径拼到工作区底下，
 * 结果 `C:\Users\..\work\C:\Windows` 仍然「在工作区内」——
 * 路径逃逸检查被完全绕过。 */
static int ca_is_absolute(const char *p) {
    if (p == NULL || p[0] == '\0') return 0;
    if (ca_is_sep(p[0])) return 1;
#if defined(_WIN32)
    if (p[1] == ':' && ca_is_sep(p[2])) return 1; /* C:\ 或 C:/ */
    if (p[1] == ':' && p[2] == '\0') return 1;     /* C: */
#endif
    return 0;
}
#else
#  define CA_SEP '/'
static int ca_is_sep(char c) { return c == '/'; }
/* 这一个函数早先只在 Windows 分支里有定义，POSIX 分支漏了 ——
 * 而它是被**无条件调用**的。结果是整个 POSIX 侧从未编译成功过：
 * macOS 报 "call to undeclared function"，Linux 报 "undefined reference"。
 * 本地只有 MSVC，这个洞一直看不见。 */
static int ca_is_absolute(const char *p) {
    return p != NULL && ca_is_sep(p[0]);
}
#endif

/* 词法规范化：消解 '.' 与 '..'，不访问文件系统。
 * 返回 0 成功；-1 试图越过根（.. 数量超过已有层级）。 */
static int lexical_normalize(const char *in, char *out, size_t cap) {
    /* 第一步：把 '/' 与 '\\' 统一成 CA_SEP。
     * 早先的写法是「先写一个分隔符，再判断当前字符是不是分隔符」，
     * 于是**每个字符前面都被插入分隔符**——`..` 被拆成两个独立的 `.` 段，
     * 逃逸检测整个失效（C:\Windows 被规约成 \C\W\i\n\d\o\w\s）。
     * 规范化只能改分隔符本身，不能凭空插入。
     */
    char tmp[CA_PATH_MAX * 2];
    size_t tn = 0;
    int absolute = 0;
    if (in[0] == '/' || in[0] == '\\') {
        absolute = 1;
        if (tn + 1 < sizeof(tmp)) tmp[tn++] = CA_SEP;
    }
    for (const char *p = in; *p != '\0'; p++) {
        if (tn + 2 >= sizeof(tmp)) return -1; /* 超长即非法 */
        if (*p == '/' || *p == '\\') {
            if (tn > 0 && tmp[tn - 1] == CA_SEP) continue; /* 连续分隔符压成一个 */
            tmp[tn++] = CA_SEP;
        } else {
            tmp[tn++] = *p;
        }
    }
    tmp[tn] = '\0';

    /* 栈式消解 */
    char *segs[128];
    size_t seglen[128];
    int depth = 0;
    int overflow = 0;

    size_t i = 0;
    while (i < tn) {
        size_t start = i;
        while (i < tn && tmp[i] != CA_SEP) i++;
        size_t len = i - start;
        while (i < tn && tmp[i] == CA_SEP) i++;
        if (len == 0) continue;
        if (len == 1 && tmp[start] == '.') continue;
        if (len == 2 && tmp[start] == '.' && tmp[start + 1] == '.') {
            if (depth > 0) {
                depth--;
            } else if (absolute) {
                /* 绝对路径里越过根：这是非法路径，不是「回到根」 */
                overflow = 1;
                break;
            } else {
                /* 相对路径的前导 ..：保留（稍后交给包含性检查判定） */
                if (depth >= 128) { overflow = 1; break; }
                segs[depth] = NULL;
                seglen[depth] = 3; /* ".." */
                depth++;
            }
            continue;
        }
        if (depth >= 128) { overflow = 1; break; }
        segs[depth] = tmp + start;
        seglen[depth] = len;
        depth++;
    }
    if (overflow) return -1;

    size_t pos = 0;
    if (absolute && pos + 1 < cap) out[pos++] = CA_SEP;
    for (int k = 0; k < depth; k++) {
        if (k > 0 && pos + 1 < cap) out[pos++] = CA_SEP;
        size_t n = (segs[k] == NULL) ? 2 : seglen[k];
        if (segs[k] == NULL) {
            if (pos + 2 < cap) { out[pos++] = '.'; out[pos++] = '.'; }
        } else {
            for (size_t j = 0; j < n && pos + 1 < cap; j++) out[pos++] = segs[k][j];
        }
    }
    if (pos == 0 && pos + 1 < cap) out[pos++] = '.';
    out[pos < cap ? pos : cap - 1] = '\0';
    return 0;
}

/* 路径 p 是否在 root 之内（含 root 本身） */
ca_workspace *ca_ws_open(const char *root) {
    if (root == NULL || root[0] == '\0') return NULL;
    ca_workspace *ws = (ca_workspace *)calloc(1, sizeof(ca_workspace));
    if (ws == NULL) return NULL;

    /* 启动时 ws->root 还是空的，**不能**走 ca_ws_resolve ——
     * 它的包含性检查要拿 ws->root 和候选路径比较，root 为空时必然失败，
     * 于是 ca_ws_open 永远返回 NULL。早先这么写，整个工具层直接不可用。
     * 这里只做词法规范化，不做包含性检查（此时还没有"工作区"可言）。 */
    char combined[CA_PATH_MAX * 2];
    if (ca_is_absolute(root)) {
        if (ca_copy_str(combined, sizeof(combined), root) != 0) {
            free(ws);
            return NULL;
        }
    } else {
        char cwd[CA_PATH_MAX];
#if defined(_WIN32)
        if (_getcwd(cwd, sizeof(cwd)) == NULL) { free(ws); return NULL; }
#else
        if (getcwd(cwd, sizeof(cwd)) == NULL) { free(ws); return NULL; }
#endif
        if (ca_copy_str(combined, sizeof(combined), cwd) != 0) { free(ws); return NULL; }
        size_t n = strlen(combined);
        combined[n] = CA_SEP;
        if (ca_copy_str(combined + n + 1, sizeof(combined) - n - 1, root) != 0) {
            free(ws);
            return NULL;
        }
    }

    if (lexical_normalize(combined, ws->root, sizeof(ws->root)) != 0) {
        free(ws);
        return NULL;
    }
    /* 去掉末尾分隔符，规范形式便于比较 */
    size_t n = strlen(ws->root);
    while (n > 1 && (ws->root[n - 1] == '/' || ws->root[n - 1] == '\\')) {
        ws->root[--n] = '\0';
    }

#if !defined(_WIN32)
    /* 关键：root 必须自己也规范化（消解符号链接），否则沙箱形同虚设。
     *
     * macOS 上 /tmp 是指向 /private/tmp 的**符号链接**。于是：
     *     ws->root            = /tmp/cagent_tools_test      （词法规范化的结果）
     *     realpath(候选路径)  = /private/tmp/cagent_tools_test/...
     * is_within(ws->root, real) 永远为假 —— 于是 POSIX 上**每一次**
     * 路径解析都被判成「逃出工作区」，工具层在该平台上直接不可用。
     *
     * Windows 上 GetFullPathName 已经做了等价的事，不需要额外处理。
     * realpath 失败（目录还不存在）时保留词法结果，属可接受的降级。
     */
    char real_root[CA_POSIX_PATH_MAX];
    if (realpath(ws->root, real_root) != NULL) {
        size_t rn = strlen(real_root);
        while (rn > 1 && (real_root[rn - 1] == '/' || real_root[rn - 1] == '\\')) {
            real_root[--rn] = '\0';
        }
        if (rn < sizeof(ws->root)) ca_copy_str(ws->root, sizeof(ws->root), real_root);
    }
#endif
    return ws;
}

void ca_ws_free(ca_workspace *ws) {
    if (ws == NULL) return;
    free(ws);
}

/* 路径 p 是否在 root 之内（含 root 本身） */
static int is_within(const char *root, const char *p) {
    size_t rn = strlen(root);
    if (rn == 0) return 0;
    if (strncmp(p, root, rn) != 0) return 0;
    if (p[rn] == '\0') return 1; /* 就是 root 本身 */
#if defined(_WIN32)
    /* Windows 上两种分隔符都算边界 */
    char c1 = p[rn];
    if (c1 == '/' || c1 == '\\') return 1;
    return 0;
#else
    return p[rn] == CA_SEP;
#endif
}

#if !defined(_WIN32)
/* 取最深的已存在祖先做 realpath，消解符号链接后再判包含性。
 * 只做词法检查是不够的：工作区内一个指向外部的符号链接是最常见的逃逸方式。 */
static int realpath_ancestor(const char *path, char *out, size_t cap) {
    /* 两个缓冲都必须是 CA_POSIX_PATH_MAX：realpath 不知道它们的容量。
     * 早先这里用 CA_PATH_MAX(1024)，超过 1024 的路径就越界写栈。 */
    char tmp[CA_POSIX_PATH_MAX];
    if (strlen(path) >= sizeof(tmp)) return -1;
    ca_copy_str(tmp, sizeof(tmp), path);

    /* 逐级去掉末尾分量直到某一级存在 */
    for (;;) {
        char resolved[CA_POSIX_PATH_MAX];
        if (realpath(tmp, resolved) != NULL) {
            if (strlen(resolved) >= cap) return -1;
ca_copy_str(out, cap, resolved);
            return 0;
        }
        char *slash = strrchr(tmp, '/');
        if (slash == NULL) return -1; /* 根都不存在，无从判断 */
        if (slash == tmp) {
            tmp[1] = '\0';
        } else {
            *slash = '\0';
        }
    }
}
#endif

int ca_ws_resolve(ca_workspace *ws, const char *input, char *out, size_t cap,
                  size_t *need_len) {
    if (ws == NULL || input == NULL || out == NULL) return -2;
    if (cap == 0) {
        if (need_len != NULL) *need_len = CA_PATH_MAX;
        return -2;
    }

    char combined[CA_PATH_MAX * 2];
    if (ca_is_absolute(input)) {
        if (strlen(input) >= sizeof(combined)) return -2;
        ca_copy_str(combined, sizeof(combined), input);
} else {
        size_t room = sizeof(combined) - strlen(ws->root) - 1;
        if (strlen(input) + 1 > room) return -2;
        if (ca_copy_str(combined, sizeof(combined), ws->root) != 0) return -2;
        size_t n = strlen(combined);
        combined[n] = CA_SEP;
        if (ca_copy_str(combined + n + 1, room, input) != 0) return -2;
    }

    char norm[CA_PATH_MAX];
    if (lexical_normalize(combined, norm, sizeof(norm)) != 0) {
        if (need_len != NULL) *need_len = CA_PATH_MAX;
        return -1;
    }
    /* 第一次包含性检查（词法） */
    if (!is_within(ws->root, norm)) {
        if (need_len != NULL) *need_len = strlen(norm) + 1;
        return -1;
    }

#if !defined(_WIN32)
    /* 第二次检查：消解符号链接后再判。
     * 存在的文件直接判；不存在的（写入前）判其最深的已存在祖先。 */
    char real[CA_POSIX_PATH_MAX];
    if (realpath(norm, real) != NULL) {
        if (!is_within(ws->root, real)) {
            if (need_len != NULL) *need_len = strlen(real) + 1;
            return -1;
        }
        if (strlen(real) + 1 > cap) {
            if (need_len != NULL) *need_len = strlen(real) + 1;
            return -2;
        }
ca_copy_str(out, cap, real);
        return 0;
    }
    char anc[CA_PATH_MAX];
    if (realpath_ancestor(norm, anc, sizeof(anc)) == 0) {
        if (!is_within(ws->root, anc)) {
            if (need_len != NULL) *need_len = strlen(anc) + 1;
            return -1;
        }
    }
#endif

    size_t n = strlen(norm);
    if (n + 1 > cap) {
        if (need_len != NULL) *need_len = n + 1;
        return -2;
    }
ca_copy_str(out, cap, norm);
    return 0;
}

const char *ca_tool_pending_display(const ca_tool_ctx *ctx, const char *tool_name) {
    if (ctx == NULL || ctx->pending_display == NULL) return NULL;
    /* 白名单：只有破坏性工具需要展示待批准内容。
     * 漏掉任何一个，就等于该工具在**盲批**状态下被批准。 */
    if (tool_name != NULL && strcmp(tool_name, "run_shell") != 0 &&
        strcmp(tool_name, "write_file") != 0 &&
        strcmp(tool_name, "edit_file") != 0) {
        return NULL;
    }
    return ctx->pending_display;
}

/* ============================================================ 工具 */

/* 从参数 JSON 里取字符串字段。找不到时 out 置空。 */
static int arg_string(const char *json, const char *key, char *out, size_t cap) {
    out[0] = '\0';
    ca_json_cursor root, v;
    ca_json_init(&root, json, strlen(json));
    if (ca_json_enter_object(&root) != 0) return -1;
    if (ca_json_field(&root, key, &v) != 1) return 0; /* 可选字段 */
    if (ca_json_read_string(&v, out, cap, NULL) != 0) return -1;
    return 0;
}

static int arg_int(const char *json, const char *key, long long *out) {
    ca_json_cursor root, v;
    ca_json_init(&root, json, strlen(json));
    if (ca_json_enter_object(&root) != 0) return -1;
    if (ca_json_field(&root, key, &v) != 1) return 0;
    if (ca_json_read_int(&v, out) != 0) return -1;
    return 0;
}

/* 权限检查（P1）。没有回调时按默认拒绝处理。 */
static int check_perm(ca_tool_ctx *ctx, const char *tool) {
    if (ctx == NULL || ctx->perm == NULL) return CA_PERM_DENY;
    return ctx->perm(tool, ctx->perm_user) == CA_PERM_ALLOW ? 0 : 1;
}

ca_tool_status ca_tool_read_file(ca_tool_ctx *ctx, const char *args_json) {
    if (ctx == NULL || ctx->out == NULL) return CA_TOOL_ERR_ARGS;
    char path_in[CA_PATH_MAX];
    if (arg_string(args_json, "path", path_in, sizeof(path_in)) != 0) {
        return CA_TOOL_ERR_ARGS;
    }
    if (path_in[0] == '\0') {
        ca_out_puts(ctx->out, "<result error=\"路径为空\"></result>");
        return CA_TOOL_ERR_ARGS;
    }

    char resolved[CA_PATH_MAX];
    size_t need = 0;
    int rc = ca_ws_resolve(ctx->ws, path_in, resolved, sizeof(resolved), &need);
    if (rc == -1) {
        /* P4：越权必须明确拒绝，且**不回显解析后的路径**——
         * 那会把工作区外的真实目录结构泄露给模型。 */
        ca_out_puts(ctx->out,
                    "<result error=\"路径不在工作区内，已拒绝\">"
                    "为安全起见不显示解析后的路径。</result>");
        return CA_TOOL_ERR_PATH;
    }
    if (rc != 0) {
        ca_out_puts(ctx->out, "<result error=\"路径过长，已拒绝\"></result>");
        return CA_TOOL_ERR_ARGS;
    }

    FILE *f = fopen(resolved, "rb");
    if (f == NULL) {
        ca_out_printf(ctx->out, "<result error=\"%s\" path=\"%s\"></result>",
                      "找不到该文件", path_in);
        return CA_TOOL_ERR_NOTFOUND;
    }

    long long offset = 0, limit = 0;
    (void)arg_int(args_json, "offset", &offset);
    (void)arg_int(args_json, "limit", &limit);
    if (offset < 0) offset = 0;

    /* 先读上限 +1 字节，用来判断「是否被截断」——
     * 少读一个字节就无法区分「刚好等于上限」与「超过上限」。 */
    size_t cap = CA_FILE_READ_LIMIT;
    if (limit > 0 && (size_t)limit < cap) cap = (size_t)limit;
    char *data = (char *)malloc(cap + 1);
    if (data == NULL) {
        fclose(f);
        return CA_TOOL_ERR_TOO_LARGE;
    }
    if (offset > 0) {
        if (fseek(f, (long)offset, SEEK_SET) != 0) {
            free(data);
            fclose(f);
            ca_out_puts(ctx->out, "<result error=\"无法定位到指定位置\"></result>");
            return CA_TOOL_ERR_IO;
        }
    }
    size_t got = fread(data, 1, cap + 1, f);
    int truncated = (got > cap);
    if (truncated) got = cap;
    fclose(f);

    /* 统计行号与总行数（只统计已读部分即可：截断时总行数未知，
     * 如实标注而不是猜一个） */
    size_t lines = 0;
    for (size_t i = 0; i < got; i++) {
        if (data[i] == '\n') lines++;
    }
    size_t line_start = 1, line_end = lines;
    if (offset > 0) {
        /* 粗略折算起始行：够用且不撒谎（从第 1 行开始数） */
        line_start = 1;
        line_end = lines;
    }

    ca_out_printf(ctx->out,
                  "<result path=\"%s\" lines=\"%llu-%llu\" truncated=\"%s\">\n",
                  path_in, (unsigned long long)line_start,
                  (unsigned long long)(line_end + 1),
                  truncated ? "true" : "false");

    /* 按行输出并带行号 */
    size_t line_no = (size_t)line_start;
    size_t i = 0;
    while (i < got) {
        size_t eol = i;
        while (eol < got && data[eol] != '\n') eol++;
        size_t seg = eol - i;
        ca_out_printf(ctx->out, "%6llu| ", (unsigned long long)line_no);
        for (size_t j = 0; j < seg; j++) ca_out_putc(ctx->out, data[i + j]);
        ca_out_putc(ctx->out, '\n');
        line_no++;
        i = eol + 1;
    }

    if (truncated) {
        /* 截断必须**显式告知**，否则模型会以为读到的是全文。
         * 且给出继续读取的方法。 */
        ca_out_printf(ctx->out,
                      "\n<!-- 已在 %llu 字节处截断（上限 %llu 字节）。"
                      "以上不是全文。如需更多，请用 offset=%llu 继续读取。 -->\n",
                      (unsigned long long)cap, (unsigned long long)CA_FILE_READ_LIMIT,
                      (unsigned long long)(offset + cap));
    }
    ca_out_puts(ctx->out, "</result>\n");

    free(data);
    /* 超过硬上限时拒绝而非静默截断 */
    return truncated ? CA_TOOL_ERR_TOO_LARGE : CA_TOOL_OK;
}

ca_tool_status ca_tool_write_file(ca_tool_ctx *ctx, const char *args_json) {
    if (ctx == NULL || ctx->out == NULL) return CA_TOOL_ERR_ARGS;

    /* 解析参数放在问权限**之前**：契约 P2 要求确认提示展示「完整参数」。
     * 解析没有副作用（不碰文件系统），先解析才可能把「写到哪、写什么」
     * 摆到用户面前。run_shell 也是这样做的（先取出命令原文再问）。
     * 真正有副作用的写盘仍然在权限之后。 */
    char path_in[CA_PATH_MAX];
    if (arg_string(args_json, "path", path_in, sizeof(path_in)) != 0) {
        return CA_TOOL_ERR_ARGS;
    }
    /* 内容可能很大，放大缓冲 */
    size_t content_cap = CA_TOOL_RESULT_LIMIT;
    char *content = (char *)malloc(content_cap);
    if (content == NULL) return CA_TOOL_ERR_TOO_LARGE;
    if (arg_string(args_json, "content", content, content_cap) != 0) {
        free(content);
        ca_out_puts(ctx->out, "<result error=\"内容超出上限或无法解析\"></result>");
        return CA_TOOL_ERR_TOO_LARGE;
    }
    /* arg_string 已写入 NUL，用 strlen 取长度——JSON 转义已解码 */
    size_t content_len = strlen(content);

    /* P2/P5：把目标路径与要写的内容交给权限界面。
     * 早先这里**什么都没设**（pending_display 为 NULL），用户看到的是
     * 一句「write_file 请求。允许执行？」，既不知道写哪个文件也不知道
     * 写什么——等于盲批。内容上限 1 MB，全量刷屏没用，所以只展示前
     * 一段并**显式标注**总长度与省略量：不标注的截断才是欺骗。 */
    {
        const size_t preview_cap = 2048;
        static char pending[4096];
        size_t show = content_len < preview_cap ? content_len : preview_cap;
        char *p = pending;
        size_t cap = sizeof(pending);
        int w = snprintf(p, cap, "将写入文件：%s\n内容 %llu 字节：\n\n",
                         path_in[0] ? path_in : "(空路径)",
                         (unsigned long long)content_len);
        if (w < 0) w = 0;
        if ((size_t)w < cap) {
            p += w;
            cap -= (size_t)w;
            size_t n = show < cap - 1 ? show : cap - 1;
            memcpy(p, content, n);
            p += n;
            cap -= n;
        }
        if (show < content_len) {
            snprintf(p, cap, "\n[以上仅前 %lu 字节，共 %llu 字节]",
                     (unsigned long)show, (unsigned long long)content_len);
        } else if (cap > 1) {
            snprintf(p, cap, "\n");
        }
        ctx->pending_display = pending;
    }

    /* P1/P2：破坏性操作必须**先问权限**，拿到了同意才动文件系统。 */
    if (check_perm(ctx, "write_file") != 0) {
        ctx->pending_display = NULL;
        free(content);
        /* P3：拒绝要回灌结构化结果，模型才能换个做法继续 */
        ca_out_puts(ctx->out,
                    "<result status=\"denied\" tool=\"write_file\">"
                    "用户拒绝了这次写入操作。可以先说明你的意图，"
                    "或改用其他方式。</result>");
        return CA_TOOL_DENIED;
    }
    ctx->pending_display = NULL;

    char resolved[CA_PATH_MAX];
    size_t need = 0;
    int rc = ca_ws_resolve(ctx->ws, path_in, resolved, sizeof(resolved), &need);
    if (rc != 0) {
        free(content);
        ca_out_puts(ctx->out,
                    "<result status=\"error\" error=\"路径不在工作区内，已拒绝\">"
                    "为安全起见不显示解析后的路径。</result>");
        return CA_TOOL_ERR_PATH;
    }

    FILE *f = fopen(resolved, "wb");
    if (f == NULL) {
        free(content);
        ca_out_printf(ctx->out, "<result status=\"error\" error=\"无法写入 %s\">"
                                 "请确认目标目录存在且可写。</result>", path_in);
        return CA_TOOL_ERR_IO;
    }
    size_t wrote = fwrite(content, 1, content_len, f);
    int ok = (wrote == content_len);
    if (fclose(f) != 0) ok = 0;
    free(content);

    if (!ok) {
        ca_out_puts(ctx->out, "<result status=\"error\" error=\"写入未完成\">"
                               "文件可能已被部分修改，请先读取确认。</result>");
        return CA_TOOL_ERR_IO;
    }
    ca_out_printf(ctx->out,
                  "<result status=\"ok\" path=\"%s\" bytes=\"%llu\">"
                  "已写入。</result>\n", path_in, (unsigned long long)content_len);
    return CA_TOOL_OK;
}

ca_tool_status ca_tool_list_dir(ca_tool_ctx *ctx, const char *args_json) {
    if (ctx == NULL || ctx->out == NULL) return CA_TOOL_ERR_ARGS;
    char path_in[CA_PATH_MAX];
    if (arg_string(args_json, "path", path_in, sizeof(path_in)) != 0) {
        return CA_TOOL_ERR_ARGS;
    }
    if (path_in[0] == '\0') {
#if defined(_WIN32)
        path_in[0] = '.'; path_in[1] = '\0';
#else
        path_in[0] = '.'; path_in[1] = '\0';
#endif
    }

    char resolved[CA_PATH_MAX];
    size_t need = 0;
    int rc = ca_ws_resolve(ctx->ws, path_in, resolved, sizeof(resolved), &need);
    if (rc != 0) {
        ca_out_puts(ctx->out,
                    "<result error=\"路径不在工作区内，已拒绝\">"
                    "为安全起见不显示解析后的路径。</result>");
        return CA_TOOL_ERR_PATH;
    }

#if defined(_WIN32)
    char pattern[CA_PATH_MAX];
    snprintf(pattern, sizeof(pattern), "%s\\*", resolved);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        ca_out_printf(ctx->out, "<result error=\"无法列出 %s\"></result>\n", path_in);
        return CA_TOOL_ERR_NOTFOUND;
    }
    ca_out_printf(ctx->out, "<result path=\"%s\" kind=\"dir\">\n", path_in);
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        ca_out_printf(ctx->out, "%s %s\n",
                      (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "[dir] " : "      ",
                      fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(resolved);
    if (d == NULL) {
        ca_out_printf(ctx->out, "<result error=\"无法列出 %s\"></result>\n", path_in);
        return CA_TOOL_ERR_NOTFOUND;
    }
    ca_out_printf(ctx->out, "<result path=\"%s\" kind=\"dir\">\n", path_in);
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char full[CA_PATH_MAX * 2];
        snprintf(full, sizeof(full), "%s/%s", resolved, ent->d_name);
        struct stat st;
        int is_dir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));
        ca_out_printf(ctx->out, "%s %s\n", is_dir ? "[dir] " : "      ", ent->d_name);
    }
    closedir(d);
#endif
    ca_out_puts(ctx->out, "</result>\n");
    return CA_TOOL_OK;
}