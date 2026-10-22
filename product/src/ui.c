/* ui.c —— 渲染状态机（契约 §7）
 * 纯逻辑：不碰 termios、不碰 ANSI、不判断 TTY。
 * 所有平台差异都在 ca_term_* 里（契约 §8）。
 */

#include "cagent/ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* 「等待响应」只是占位行：任何真实输出到来时先把它擦掉，
 * 否则用户会看到「（等待响应…）[调用工具 x]」粘在一行里。 */
static void ui_clear_waiting(ca_ui *ui) {
    if (ui == NULL || !ui->waiting) return;
    ui->waiting = 0;
    /* 长度用 sizeof-1："\r\x1b[2K" 是 **5** 个字节（CR + ESC + '[2K'），
     * 写死 4 会只发出 ESC[2，把后面那行内容的开头吃掉。 */
    static const char erase[] = "\r\x1b[2K";
    ui->out.write(ui->out.self, erase, sizeof(erase) - 1);
    ui->last_was_newline = 0;
}

static void ui_puts(ca_ui *ui, const char *s) {
    if (ui == NULL || s == NULL) return;
    size_t n = strlen(s);
    if (n == 0) return;
    ui_clear_waiting(ui);
    ui->out.write(ui->out.self, s, n);
    ui->last_was_newline = (s[n - 1] == '\n');
}

static void ui_putn(ca_ui *ui, const char *s, size_t n) {
    if (ui == NULL || s == NULL || n == 0) return;
    ui_clear_waiting(ui);
    ui->out.write(ui->out.self, s, n);
    ui->last_was_newline = (s[n - 1] == '\n');
}

int ca_ui_init(ca_ui *ui, ca_ui_sink out, ca_ui_src in) {
    if (ui == NULL || out.write == NULL) return -1;
    memset(ui, 0, sizeof(*ui));
    ui->out = out;
    ui->in = in;
    ui->is_tty = ca_term_is_tty();
    ui->last_was_newline = 1;
    /* 原始模式在 TTY 下才尝试启用；失败不是致命错误，
     * 只是退化到行式输出（契约 §7 的降级路径）。 */
    if (ui->is_tty) {
        if (ca_term_raw_enter() == 0) ui->raw_enabled = 1;
        else ui->is_tty = 0;
    }
    return 0;
}

void ca_ui_shutdown(ca_ui *ui) {
    if (ui == NULL) return;
    /* 还原原始模式。这件事必须在任何退出路径上发生，
     * 否则用户会留下一个不能回显的终端——那是**留给用户的问题**。 */
    if (ui->raw_enabled) {
        (void)ca_term_raw_exit();
        ui->raw_enabled = 0;
    }
}

void ca_ui_line(ca_ui *ui, const char *s) {
    ui_puts(ui, s);
    if (!ui->last_was_newline) ui_puts(ui, "\n");
}

void ca_ui_printf(ca_ui *ui, const char *fmt, ...) {
    if (ui == NULL || fmt == NULL) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(ui->scratch, sizeof(ui->scratch), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof(ui->scratch)) {
        ui_putn(ui, ui->scratch, (size_t)n);
    } else {
        /* 放不下就丢弃而不是截断成半句——半句会让用户以为
         * 代理说完了，而它没有。 */
        ui_puts(ui, "\n[输出过长，已省略]\n");
    }
}

/* ---- 视觉增强（仅 TTY）----
 *
 * 目标不是「好看」，而是**让状态一眼可见**：哪一步在跑、哪一步失败了、
 * 哪些是模型的话、哪些是工具的动作。非 TTY 下全部退化成普通文本，
 * CI 消费者看到的仍是干净的一行行文本。
 *
 * 颜色用 ANSI SGR；Windows 10+ 的控制台需要 ENABLE_VIRTUAL_TERMINAL_PROCESSING，
 * 由 ca_term_setup_console() 打开。 */
#define CA_SGR_RESET "\x1b[0m"
#define CA_SGR_DIM "\x1b[2m"
#define CA_SGR_BOLD "\x1b[1m"
#define CA_SGR_RED "\x1b[31m"
#define CA_SGR_GREEN "\x1b[32m"
#define CA_SGR_YELLOW "\x1b[33m"
#define CA_SGR_BLUE "\x1b[36m"

static void ui_colored(ca_ui *ui, const char *color, const char *text) {
    if (ui == NULL) return;
    if (!ui->is_tty) {
        /* 非 TTY：只输出内容本身，不带任何转义序列。 */
        ca_ui_line(ui, text);
        return;
    }
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "%s%s%s", color, text, CA_SGR_RESET);
    if (n > 0 && (size_t)n < sizeof(buf)) {
        ui_putn(ui, buf, (size_t)n);
        if (!ui->last_was_newline) ui_puts(ui, "\n");
    } else {
        ca_ui_line(ui, text);
    }
}

void ca_ui_heading(ca_ui *ui, const char *title) {
    if (ui == NULL || title == NULL) return;
    if (!ui->is_tty) {
        ca_ui_line(ui, title);
        return;
    }
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "%s%s%s\n", CA_SGR_BOLD CA_SGR_BLUE, title,
                     CA_SGR_RESET);
    if (n > 0 && (size_t)n < sizeof(buf)) ui_putn(ui, buf, (size_t)n);
}

void ca_ui_panel(ca_ui *ui, const char *title, const char *body) {
    if (ui == NULL) return;
    if (!ui->is_tty) {
        if (title != NULL && title[0] != '\0') ca_ui_line(ui, title);
        if (body != NULL && body[0] != '\0') ca_ui_line(ui, body);
        return;
    }
    /* 盒面板：标题在上，内容缩进两格，下边框线。
     * 不清屏、不折叠——契约 P2 要求批准前看到完整内容。 */
    /* 盒子字符不能直接写在格式串里：MSVC 把多字节字符当成格式说明符，
     * 报 C4474（参数过多）。一律用 %s 传。 */
    static const char k_box_top[] = "\xe2\x94\x8c\xe2\x94\x80 ";  /* ┌─ */
    static const char k_box_bar[] = "\xe2\x94\x82";                 /* │ */
    static const char k_box_bottom[] =
        "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\x80";  /* └──────────────────── */
    char line[1024];
    int n = snprintf(line, sizeof(line), "%s%s%s %s%s\n", CA_SGR_DIM, k_box_top,
                     title != NULL ? title : "", k_box_top + 3, CA_SGR_RESET);
    if (n > 0 && (size_t)n < sizeof(line)) ui_putn(ui, line, (size_t)n);

    if (body != NULL && body[0] != '\0') {
        const char *p = body;
        while (*p != '\0') {
            const char *nl = strchr(p, '\n');
            size_t len = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
            if (len > 0) {
                char row[1024];
                int m = snprintf(row, sizeof(row), "%s%s%s ", CA_SGR_DIM, k_box_bar,
                                 CA_SGR_RESET);
                size_t head = (m > 0 && (size_t)m < sizeof(row)) ? (size_t)m : 0;
                if (head + len + 2 < sizeof(row)) {
                    memcpy(row + head, p, len);
                    size_t rest = sizeof(row) - head - len - 1;
                    if (rest > sizeof(CA_SGR_RESET)) {
                        memcpy(row + head + len, CA_SGR_RESET, sizeof(CA_SGR_RESET));
                        ui_putn(ui, row, head + len + sizeof(CA_SGR_RESET) - 1);
                    } else {
                        ui_putn(ui, p, len);
                    }
                } else {
                    ui_putn(ui, p, len);
                }
                ui_puts(ui, "\n");
            }
            if (nl == NULL) break;
            p = nl + 1;
        }
    }
    n = snprintf(line, sizeof(line), "%s%s%s\n", CA_SGR_DIM, k_box_bottom,
                 CA_SGR_RESET);
    if (n > 0 && (size_t)n < sizeof(line)) ui_putn(ui, line, (size_t)n);
}

void ca_ui_kv(ca_ui *ui, const char *key, const char *value) {
    if (ui == NULL) return;
    if (!ui->is_tty) {
        ca_ui_printf(ui, "%s: %s\n", key != NULL ? key : "", value != NULL ? value : "");
        return;
    }
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "%s%s%s  %s", CA_SGR_DIM,
                     key != NULL ? key : "", CA_SGR_RESET, value != NULL ? value : "");
    if (n > 0 && (size_t)n < sizeof(buf)) {
        ui_putn(ui, buf, (size_t)n);
        if (!ui->last_was_newline) ui_puts(ui, "\n");
    }
}

void ca_ui_ok(ca_ui *ui, const char *text) { ui_colored(ui, CA_SGR_GREEN, text); }
void ca_ui_warn(ca_ui *ui, const char *text) { ui_colored(ui, CA_SGR_YELLOW, text); }
void ca_ui_error(ca_ui *ui, const char *text) { ui_colored(ui, CA_SGR_RED, text); }
void ca_ui_note(ca_ui *ui, const char *text) { ui_colored(ui, CA_SGR_DIM, text); }

/* ---- 等待响应 ---- */

void ca_ui_wait_begin(ca_ui *ui) {
    if (ui == NULL || !ui->is_tty) return;
    static const char hint[] = "（等待响应…）";
    ui->waiting = 1;
    ui->out.write(ui->out.self, hint, sizeof(hint) - 1);
    ui->last_was_newline = 0;
}

void ca_ui_wait_end(ca_ui *ui) {
    if (ui == NULL || !ui->waiting) return;
    /* 到这一轮结束还没被真实输出顶掉，说明本轮没有可见文本
     * （例如只调了工具）。把占位行擦掉，让后续内容从行首开始。 */
    ui_clear_waiting(ui);
}

/* ---- 流式输出 ---- */

void ca_ui_stream_begin(ca_ui *ui) {
    if (ui == NULL) return;
    ui->spinner = 1;
    /* 行式降级下不加任何提示符：管道里的输出应该可以直接被脚本消费。 */
    if (!ui->is_tty) return;
    ui_puts(ui, "\r\x1b[2K"); /* 回行首 + 清行，为逐 token 重绘做准备 */
}

void ca_ui_stream_token(ca_ui *ui, const char *text) {
    if (ui == NULL || text == NULL) return;
    /* 契约 §7：逐 token 追加，**不做全量重绘**。
     * 重绘是卡顿的主因——每来一个 token 就把整屏重排一遍。 */
    ui_puts(ui, text);
}

void ca_ui_stream_end(ca_ui *ui) {
    if (ui == NULL) return;
    ui->spinner = 0;
    if (!ui->last_was_newline) ui_puts(ui, "\n");
    /* 有尚未闭合的行就补一个换行，否则下一条输出会接在同一行上 */
}

/* ---- 权限确认 ---- */

/* 读一个字符，返回 -1 表示 EOF/错误 */
static int ui_getch(ca_ui *ui) {
    if (ui->in.read_byte == NULL) return -1;
    return ui->in.read_byte(ui->in.self);
}

void ca_ui_show_pending(ca_ui *ui, const char *tool, const char *text) {
    if (ui == NULL) return;
    /* 把待批准内容**原样**写出去。必须在（可能的）提问之前，
     * 且绝不清屏（契约 §7 / §5.2 P5）。
     *
     * TTY 下放进盒面板：内容与「这是什么操作」的标题绑在一起，
     * 用户不会把待批准的内容当成上一条消息的尾巴。
     * 非 TTY 保持裸文本（管道里不要装饰）。 */
    if (text == NULL || text[0] == '\0') return;
    if (ui->is_tty) {
        char title[160];
        snprintf(title, sizeof(title), "调用工具 %s · 等待批准",
                 tool != NULL ? tool : "");
        ca_ui_panel(ui, title, text);
    } else {
        ui_puts(ui, text);
        if (!ui->last_was_newline) ui_puts(ui, "\n");
    }
}

ca_ui_decision ca_ui_ask(ca_ui *ui, const char *tool) {
    if (ui == NULL) return CA_UI_DENY; /* 失败时必须保守：拒绝，而不是放行 */

    /* 给一个不会与内容混淆的分隔。 */
    if (ui->is_tty) ui_puts(ui, "\n");
    ui_puts(ui, "--- ");

    /* 提示。y=允许 其他一律拒绝。
     * 注意这里**没有默认值**：回车不是「允许」，
     * 否则误触回车就会放行一次破坏性操作。 */
    ui_puts(ui, tool != NULL ? tool : "工具");
    ui_puts(ui, " 请求。允许执行？[y/N] ");

    int c = ui_getch(ui);
    /* 把用户的按键回显出来：TTY 下按键不可见，
     * 不回显的话用户不知道自己的输入被收到了。 */
    if (c == 'y' || c == 'Y') {
        ui_puts(ui, "y\n");
        ui_puts(ui, "--- \n");
        return CA_UI_ALLOW;
    }
    if (c == '\r' || c == '\n') {
        ui_puts(ui, "\n");
    } else if (c > 0) {
        char echo[2];
        echo[0] = (char)c;
        echo[1] = '\n';
        ui_putn(ui, echo, 2);
    }
    ui_puts(ui, "--- \n");
    /* 任何非 y 的输入（含 EOF、错误）都拒绝。 */
    return (c < 0) ? CA_UI_ABORT : CA_UI_DENY;
}

ca_ui_decision ca_ui_confirm(ca_ui *ui, const char *tool, const char *text) {
    if (ui == NULL) return CA_UI_DENY;
    ca_ui_show_pending(ui, tool, text);
    return ca_ui_ask(ui, tool);
}
