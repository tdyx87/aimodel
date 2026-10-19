/* ui_term.c —— 终端能力层（契约 §8：平台差异收拢于此）
 *
 * 三条路径：
 *   TTY   —— ANSI + 原始模式，可单键确认
 *   非TTY —— 行式输出（**CI 跑的就是这条**）
 *
 * 「非 TTY 时原始模式是安全的空操作」而不是失败：
 * 契约 §7 要求降级，若把「不是 TTY」当成错误，
 * CI 和所有管道场景都会直接炸掉。
 */

#include "cagent/ui.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  include <io.h>
#  include <windows.h>
#else
#  include <termios.h>
#  include <unistd.h>
#endif

/* ---- 默认 sink / src ---- */

static int term_write(void *self, const char *data, size_t len) {
    (void)self;
    FILE *f = stdout;
    size_t n = fwrite(data, 1, len, f);
    fflush(f); /* 不 flush 的话管道消费者要等缓冲区满才看到内容 */
    return (n == len) ? 0 : -1;
}

static int term_read(void *self) {
    (void)self;
    int c = fgetc(stdin);
    return (c == EOF) ? -1 : c;
}

void ca_term_default_sink(ca_ui_sink *out) {
    out->write = term_write;
    out->self = NULL;
}

void ca_term_default_src(ca_ui_src *in) {
    in->read_byte = term_read;
    in->self = NULL;
}

/* ---- TTY 探测 ---- */

int ca_term_is_tty(void) {
#if defined(_WIN32)
    return _isatty(_fileno(stdout)) ? 1 : 0;
#else
    /* 用 STDOUT_FILENO 而不是 fileno(stdout)：
     * 严格 -std=c99 下（不开 _POSIX_C_SOURCE / _GNU_SOURCE）
     * <stdio.h> **不声明** fileno，于是它是隐式声明 ——
     * 在 C99 里这是约束违反，不是警告那么轻。
     * STDOUT_FILENO 由 <unistd.h> 提供，且就是常量 1。 */
    return isatty(STDOUT_FILENO) ? 1 : 0;
#endif
}

/* ---- 原始模式 ---- */

#if defined(_WIN32)

/* Windows 的「原始模式」要按**输入句柄**分别设置：
 * 关掉行缓冲（要逐键读）与回显（要自己回显），
 * 并保留 ENABLE_VIRTUAL_TERMINAL_PROCESSING 以便 ANSI 生效。 */
static DWORD g_saved_in = 0;
static int g_have_in = 0;

int ca_term_raw_enter(void) {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode)) return -1;
    g_saved_in = mode;
    g_have_in = 1;
    DWORD raw = mode;
    raw &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    if (!SetConsoleMode(h, raw)) { g_have_in = 0; return -1; }
    HANDLE o = GetStdHandle(STD_OUTPUT_HANDLE);
    if (o != INVALID_HANDLE_VALUE) {
        DWORD om = 0;
        if (GetConsoleMode(o, &om)) {
            om |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            (void)SetConsoleMode(o, om);
        }
    }
    return 0;
}

int ca_term_raw_exit(void) {
    if (!g_have_in) return 0;
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h != INVALID_HANDLE_VALUE) (void)SetConsoleMode(h, g_saved_in);
    g_have_in = 0;
    return 0;
}

#else /* POSIX */

static struct termios g_saved;
static int g_have_saved = 0;

int ca_term_raw_enter(void) {
    if (!isatty(STDIN_FILENO)) return -1;
    if (tcgetattr(STDIN_FILENO, &g_saved) != 0) return -1;
    g_have_saved = 1;
    struct termios raw = g_saved;
    /* 非规范模式：不等回车即可读到按键 */
    raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        g_have_saved = 0;
        return -1;
    }
    return 0;
}

int ca_term_raw_exit(void) {
    if (!g_have_saved) return 0;
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
    g_have_saved = 0;
    return 0;
}

#endif

/* 非 TTY 时上面两个函数返回 -1（进不去 / 不该进）。
 * ca_ui_init 会据此把 is_tty 清零并走行式路径——
 * **降级而不是报错**，这正是契约 §7 要求的。 */