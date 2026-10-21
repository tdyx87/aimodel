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

/* ---- Windows 控制台：UTF-8 输出 ----
 *
 * 不设代码页的话，中文会以控制台默认代码页（简体中文机器通常是 936/GBK）
 * 去解释 UTF-8 字节，用户看到的是**乱码**——表现为「界面什么都不显示」。
 * 这是 Windows 上最常见的一条「界面坏了」的原因，而它跟程序逻辑毫无关系。
 *
 * SetConsoleOutputCP 只影响控制台，**重定向到文件时无效**（那时写出的仍是
 * UTF-8 字节，工具链按 UTF-8 读即可），所以这里不做 _setmode。
 * 失败不算致命：老系统或非控制台句柄都会返回 FALSE，静默忽略。 */
#if defined(_WIN32)
void ca_term_setup_console(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        /* 顺带打开 ANSI 支持：没有它，界面里的颜色与边框会变成乱码。 */
        mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        (void)SetConsoleMode(h, mode);
    }
    (void)SetConsoleOutputCP(CP_UTF8);
    HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    if (hin != INVALID_HANDLE_VALUE) (void)SetConsoleCP(CP_UTF8);
}
#else
void ca_term_setup_console(void) { /* POSIX 控制台本来就是 UTF-8 环境 */ }
#endif

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
    /* **不要**清 ENABLE_PROCESSED_INPUT：清掉它，Ctrl-C 就不会再产生
     * 中断，进程也没法退出——用户只能去关窗口，而强杀不会还原控制台
     * 模式，终端就留在「无回显」状态，之后敲什么都不显示。
     * 这两件事都是用户视角的「程序把我的终端搞坏了」。 */
    raw &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
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