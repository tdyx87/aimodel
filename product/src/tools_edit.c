/* tools_edit.c —— edit_file（精确匹配替换）
 * 契约：docs/design/contract.md §5.1 / §5.2（P1、P2）
 *
 * 这一层最容易出的不是「写错」，而是**静默破坏**：
 *   - old_string 匹配到多处却全量替换
 *   - 写到一半失败，文件变成半截
 *   - 用户在不知道会改多少行的情况下点了批准
 * 三者都比「明确失败」危险得多，因此都按「宁可拒绝」处理。
 */

/* fopen / MoveFileEx 在 MSVC 上有安全建议但无 C11 可移植等价物，
 * 与 tools.c / tools_shell.c 同一处理。必须定义在 CRT 头文件之前。 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "cagent/json.h"
#include "cagent/tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <io.h>
#  include <windows.h>
#else
#  include <unistd.h>
#endif

#define CA_EDIT_MAX_FILE (4u * 1024u * 1024u)
#define CA_EDIT_MAX_TEXT (256u * 1024u * 1024u)

/* 原子替换：在同目录写临时文件再改名。
 * 直接覆盖写的话，中途失败（磁盘满、进程被杀）会留下半截文件——
 * 那对一个代码编辑工具是灾难性的：源文件被毁，而用户毫不知情。 */
static int write_atomic(const char *path, const char *data, size_t len) {
    char tmp[1200];
    snprintf(tmp, sizeof(tmp), "%s.cagent_tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) return -1;
    size_t wrote = fwrite(data, 1, len, f);
    int ok = (wrote == len);
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        remove(tmp);
        return -1;
    }
#if defined(_WIN32)
    /* MoveFileEx 的替换语义在跨卷时会失败，故要求同目录（已满足） */
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove(tmp);
        return -1;
    }
#else
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
#endif
    return 0;
}

/* 行数：结尾的换行**终止**最后一行、而不是开始新的一行。
 * "alpha\nbeta\n" 是 2 行，不是 3 行。早先无条件 +1，
 * 于是 preview 报出来的行数比编辑器多 1，用户看到的「行数 2 -> 1」
 * 与文件实际对不上——影响面数字不准，批准就没有意义。 */
static int count_lines(const char *s, size_t n) {
    if (n == 0) return 0;
    int lines = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') lines++;
    }
    if (s[n - 1] != '\n') lines++; /* 没有结尾换行时还有一行悬着 */
    return lines;
}

/* 统计子串出现次数（不重叠计数） */
static int count_occurrences(const char *hay, size_t hay_len, const char *needle,
                             size_t needle_len) {
    if (needle_len == 0 || hay_len < needle_len) return 0;
    int n = 0;
    size_t i = 0;
    while (i + needle_len <= hay_len) {
        if (memcmp(hay + i, needle, needle_len) == 0) {
            n++;
            i += needle_len;
        } else {
            i++;
        }
    }
    return n;
}

static int arg_str(const char *json, const char *key, char *out, size_t cap) {
    out[0] = '\0';
    ca_json_cursor root, v;
    ca_json_init(&root, json, strlen(json));
    if (ca_json_enter_object(&root) != 0) return -1;
    if (ca_json_field(&root, key, &v) != 1) return 0;
    if (ca_json_read_string(&v, out, cap, NULL) != 0) return -1;
    return 0;
}

static int arg_bool(const char *json, const char *key, int *out) {
    ca_json_cursor root, v;
    ca_json_init(&root, json, strlen(json));
    if (ca_json_enter_object(&root) != 0) return -1;
    if (ca_json_field(&root, key, &v) != 1) return 0;
    *out = 1;
    return 0;
}

/* 读整个文件。返回 NULL 表示不存在或超限。 */
static char *read_whole(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0 || (unsigned long)size > CA_EDIT_MAX_FILE) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)size + 1);
    if (buf == NULL) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { free(buf); return NULL; }
    buf[got] = '\0';
    *len_out = got;
    return buf;
}

ca_tool_status ca_tool_edit_preview(ca_tool_ctx *ctx, const char *args_json,
                                   ca_edit_preview *out) {
    if (ctx == NULL || ctx->ws == NULL || out == NULL) return CA_TOOL_ERR_ARGS;
    memset(out, 0, sizeof(*out));

    char path_in[1024];
    if (arg_str(args_json, "path", path_in, sizeof(path_in)) != 0) {
        return CA_TOOL_ERR_ARGS;
    }
    if (path_in[0] == '\0') return CA_TOOL_ERR_ARGS;

    char *old_s = NULL;
    char *new_s = NULL;
    ca_tool_status st = CA_TOOL_ERR_ARGS;
    size_t old_len = 0, new_len = 0;
    old_s = (char *)malloc(CA_EDIT_MAX_TEXT);
    new_s = (char *)malloc(CA_EDIT_MAX_TEXT);
    if (old_s == NULL || new_s == NULL) goto done;
    if (arg_str(args_json, "old_string", old_s, CA_EDIT_MAX_TEXT) != 0) goto done;
    if (arg_str(args_json, "new_string", new_s, CA_EDIT_MAX_TEXT) != 0) goto done;
    old_len = strlen(old_s);
    new_len = strlen(new_s);

    char resolved[1024];
    size_t need = 0;
    if (ca_ws_resolve(ctx->ws, path_in, resolved, sizeof(resolved), &need) == -1) {
        st = CA_TOOL_ERR_PATH;
        goto done;
    }

    size_t file_len = 0;
    char *content = read_whole(resolved, &file_len);
    if (content == NULL) {
        ca_out_printf(ctx->out,
                      "<result status=\"error\" error=\"找不到 %s 或它超出编辑上限\">"
                      "未做任何修改。</result>", path_in);
        st = CA_TOOL_ERR_NOTFOUND;
        goto done;
    }

    out->occurrences = count_occurrences(content, file_len, old_s, old_len);
    out->lines_before = count_lines(content, file_len);
    out->bytes_before = (long long)file_len;

    /* 估算替换后的规模（仅统计，不真正构造） */
    int replaced = out->occurrences;
    if (out->occurrences == 0) {
        out->lines_after = out->lines_before;
        out->bytes_after = (long long)file_len;
    } else {
        long long delta = (long long)replaced * ((long long)new_len - (long long)old_len);
        long long after = (long long)file_len + delta;
        out->bytes_after = after < 0 ? 0 : after;
        /* 行数变化：新内容含换行的次数决定 */
        int nl_new = count_lines(new_s, new_len) - 1;
        int nl_old = count_lines(old_s, old_len) - 1;
        out->lines_after = out->lines_before + replaced * (nl_new - nl_old);
        if (out->lines_after < 0) out->lines_after = 0;
    }
    free(content);
    st = CA_TOOL_OK;

done:
    free(old_s);
    free(new_s);
    return st;
}

ca_tool_status ca_tool_edit_file(ca_tool_ctx *ctx, const char *args_json) {
    if (ctx == NULL || ctx->ws == NULL || ctx->out == NULL) return CA_TOOL_ERR_ARGS;

    /* 第一步：算影响面（P2）。必须在请求权限**之前**——
     * 否则用户是在「不知道会改多少行」的情况下批准。 */
    ca_edit_preview pv;
    ca_tool_status st = ca_tool_edit_preview(ctx, args_json, &pv);
    if (st != CA_TOOL_OK) return st;

    int replace_all = 0;
    (void)arg_bool(args_json, "replace_all", &replace_all);

    if (pv.occurrences == 0) {
        ca_out_puts(ctx->out,
                    "<result status=\"error\" error=\"未找到要替换的内容\">"
                    "文件未被修改。请核对 old_string 是否与文件内容完全一致"
                    "（含空格与换行）。</result>");
        return CA_TOOL_ERR_ARGS;
    }
    if (pv.occurrences > 1 && !replace_all) {
        /* 宁可拒绝，也不替用户猜 */
        ca_out_printf(ctx->out,
                      "<result status=\"error\" occurrences=\"%d\">"
                      "要替换的内容在文件中出现多次，已拒绝执行。"
                      "请提供包含上下文的更长 old_string 以唯一确定位置；"
                      "确实需要全部替换时才使用 replace_all。</result>",
                      pv.occurrences);
        return CA_TOOL_ERR_ARGS;
    }

    /* 第二步：把影响面交给权限界面（P2/P5） */
    static char pending[2048];
    snprintf(pending, sizeof(pending),
             "将修改文件，影响面如下：\n"
             "  匹配处数 : %d\n"
             "  行数     : %d -> %d\n"
             "  字节     : %lld -> %lld\n"
             "文件未被修改，直到你批准。",
             pv.occurrences, pv.lines_before, pv.lines_after,
             pv.bytes_before, pv.bytes_after);
    ctx->pending_display = pending;

    if (ctx->perm == NULL ||
        ctx->perm("edit_file", ctx->perm_user) != CA_PERM_ALLOW) {
        ctx->pending_display = NULL;
        ca_out_puts(ctx->out,
                    "<result status=\"denied\" tool=\"edit_file\">"
                    "用户拒绝了这次修改。可以先说明修改理由，"
                    "或提出别的方案。</result>");
        return CA_TOOL_DENIED;
    }
    ctx->pending_display = NULL;

    /* 第三步：真正执行 */
    char path_in[1024];
    char *old_s = (char *)malloc(CA_EDIT_MAX_TEXT);
    char *new_s = (char *)malloc(CA_EDIT_MAX_TEXT);
    if (old_s == NULL || new_s == NULL) {
        free(old_s);
        free(new_s);
        return CA_TOOL_ERR_TOO_LARGE;
    }
    ca_tool_status rc = CA_TOOL_ERR_ARGS;
    char *content = NULL;
    char *result = NULL;
    size_t file_len = 0;

    if (arg_str(args_json, "path", path_in, sizeof(path_in)) != 0) goto out;
    if (arg_str(args_json, "old_string", old_s, CA_EDIT_MAX_TEXT) != 0) goto out;
    if (arg_str(args_json, "new_string", new_s, CA_EDIT_MAX_TEXT) != 0) goto out;

    char resolved[1024];
    size_t need = 0;
    if (ca_ws_resolve(ctx->ws, path_in, resolved, sizeof(resolved), &need) != 0) {
        ca_out_puts(ctx->out,
                    "<result status=\"error\" error=\"路径不在工作区内，已拒绝\">"
                    "为安全起见不显示解析后的路径。</result>");
        rc = CA_TOOL_ERR_PATH;
        goto out;
    }

    content = read_whole(resolved, &file_len);
    if (content == NULL) {
        ca_out_puts(ctx->out, "<result status=\"error\" error=\"无法读取目标文件\">"
                               "文件未被修改。</result>");
        rc = CA_TOOL_ERR_NOTFOUND;
        goto out;
    }

    size_t old_len = strlen(old_s);
    size_t new_len = strlen(new_s);
    long long delta = (long long)pv.occurrences * ((long long)new_len - (long long)old_len);
    long long new_size = (long long)file_len + delta;
    if (new_size < 0 || new_size > (long long)CA_EDIT_MAX_FILE) {
        ca_out_puts(ctx->out,
                    "<result status=\"error\" error=\"替换后文件会超出上限\">"
                    "文件未被修改。</result>");
        rc = CA_TOOL_ERR_TOO_LARGE;
        goto out;
    }

    result = (char *)malloc((size_t)new_size + 1);
    if (result == NULL) { rc = CA_TOOL_ERR_TOO_LARGE; goto out; }

    /* 逐段拼接。复制方向是从前向后，结果区总是落后于源区，安全。
     * 注意不要把「已替换过」写进匹配条件——那会让 replace_all 只替换第一处。 */
    size_t ri = 0, si = 0;
    int count = 0;
    while (si < file_len) {
        if (old_len > 0 && si + old_len <= file_len &&
            memcmp(content + si, old_s, old_len) == 0) {
            memcpy(result + ri, new_s, new_len);
            ri += new_len;
            si += old_len;
            count++;
            if (!replace_all) {
                /* 只替换第一处，剩余原样复制 */
                if (si < file_len) memcpy(result + ri, content + si, file_len - si);
                ri += file_len - si;
                break;
            }
            continue;
        }
        result[ri++] = content[si++];
    }
    result[ri] = '\0';

    if (count != pv.occurrences) {
        /* preview 与实际执行不一致：宁可不改，也不能在用户不知情的情况下
         * 写入与「已批准的内容」不同的结果。 */
        ca_out_puts(ctx->out,
                    "<result status=\"error\" error=\"内部不一致\">"
                    "预期替换处数与实际不符，文件未被修改。请重试。</result>");
        rc = CA_TOOL_ERR_IO;
        goto out;
    }

    if (write_atomic(resolved, result, ri) != 0) {
        /* 原子写失败 = 文件保持原样。如实说明，不要含糊。 */
        ca_out_puts(ctx->out,
                    "<result status=\"error\" error=\"写入失败\">"
                    "原文件未被修改（采用原子替换，不会留下半截文件）。"
                    "请检查磁盘空间与目录权限。</result>");
        rc = CA_TOOL_ERR_IO;
        goto out;
    }

    ca_out_printf(ctx->out,
                  "<result status=\"ok\" tool=\"edit_file\" path=\"%s\" "
                  "replaced=\"%d\" lines_before=\"%d\" lines_after=\"%d\">"
                  "已修改。</result>\n",
                  path_in, pv.occurrences, pv.lines_before, pv.lines_after);
    rc = CA_TOOL_OK;

out:
    free(content);
    free(result);
    free(old_s);
    free(new_s);
    return rc;
}