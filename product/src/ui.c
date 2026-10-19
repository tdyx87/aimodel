/* ui.c —— 渲染状态机（契约 §7）
 * 纯逻辑：不碰 termios、不碰 ANSI、不判断 TTY。
 * 所有平台差异都在 ca_term_* 里（契约 §8）。
 */

#include "cagent/ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void ui_puts(ca_ui *ui, const char *s) {
    if (ui == NULL || s == NULL) return;
    size_t n = strlen(s);
    if (n == 0) return;
    ui->out.write(ui->out.self, s, n);
    ui->last_was_newline = (s[n - 1] == '\n');
}

static void ui_putn(ca_ui *ui, const char *s, size_t n) {
    if (ui == NULL || s == NULL || n == 0) return;
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

ca_ui_decision ca_ui_confirm(ca_ui *ui, const char *tool, const char *text) {
    if (ui == NULL) return CA_UI_DENY; /* 失败时必须保守：拒绝，而不是放行 */

    /* 第一步：把待批准内容**原样**写出去。
     * 必须在等输入之前，且绝不清屏（契约 §7 / §5.2 P5）。 */
    if (text != NULL && text[0] != '\0') {
        ui_puts(ui, text);
        if (!ui->last_was_newline) ui_puts(ui, "\n");
    }

    /* 第二步：给一个不会与内容混淆的分隔。 */
    if (ui->is_tty) ui_puts(ui, "\n");
    ui_puts(ui, "--- ");

    /* 第三步：提示。y=允许 其他一律拒绝。
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