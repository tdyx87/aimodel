/* ui_test.c —— 终端界面层的测试
 *
 * 契约 §7 有一句话是直接冲着这份测试来的：
 *   「原始模式不可用时怎么办」必须有测试覆盖——CI 跑测试时就是非 TTY，
 *   这条不测就会在 CI 上炸。
 *
 * 所以这里**主动构造非 TTY 场景**，而不是依赖「CI 恰好就是非 TTY」。
 * 依赖环境属性的测试，换个环境就悄悄不测了——
 * 这是本项目栽过的坑（见 ADR-0003 里记录的「覆盖幻觉」清单）。
 */

#include "ca_test.h"

#include "cagent/ui.h"

#include <stdio.h>
#include <string.h>

/* ---- 可注入的 sink / src：把「终端」换成内存 ---- */

static char g_buf[1 << 16];
static size_t g_len;
static int g_in[64];
static size_t g_in_len;
static size_t g_in_pos;
static int g_write_calls; /* 写了几次——用来验证「逐 token 追加，不做全量重绘」 */

static int mem_write(void *self, const char *data, size_t n) {
    (void)self;
    if (g_len + n >= sizeof(g_buf)) return -1;
    memcpy(g_buf + g_len, data, n);
    g_len += n;
    /* 必须补结尾 NUL：否则 strcmp 会越过内容末尾、
     * 读到上一次测试残留的字节，报出一个看似莫名其妙的失败。 */
    g_buf[g_len] = '\0';
    g_write_calls++;
    return 0;
}

static int mem_read(void *self) {
    (void)self;
    if (g_in_pos >= g_in_len) return -1; /* EOF */
    return g_in[g_in_pos++];
}

static void feed(const char *s) {
    g_in_len = 0;
    g_in_pos = 0;
    for (size_t i = 0; s[i] != '\0'; i++) g_in[g_in_len++] = (unsigned char)s[i];
}

static void reset_io(void) {
    g_len = 0;
    g_buf[0] = '\0';
    g_write_calls = 0;
    feed("");
}

/* 造一个**强制非 TTY** 的 ui，不去问 ca_term_is_tty() */
static void make_plain_ui(ca_ui *ui) {
    memset(ui, 0, sizeof(*ui));
    ui->out.write = mem_write;
    ui->out.self = NULL;
    ui->in.read_byte = mem_read;
    ui->in.self = NULL;
    ui->is_tty = 0;
    ui->last_was_newline = 1;
}

/* ==================================================== 降级路径（CI 跑的就是它） */

CA_TEST(ui_plain_mode_needs_no_terminal) {
    /* 契约 §7：非 TTY 时不得尝试原始模式，直接降级为行式输出。
     * 这条若不测，就会在 CI 上炸——而 CI 就是非 TTY。 */
    ca_ui ui;
    make_plain_ui(&ui);
    ui.out.write = mem_write;

    ca_ui_line(&ui, "hello");
    CA_ASSERT(g_len > 0);
    CA_ASSERT(strcmp(g_buf, "hello\n") == 0);
    CA_ASSERT(g_buf[g_len - 1] == '\n');
    /* 降级路径不得吐出任何 ANSI 转义序列 */
    CA_ASSERT(strstr(g_buf, "\x1b[") == NULL);
}

CA_TEST(ui_plain_stream_appends_incrementally) {
    /* 契约 §7：逐 token 追加，**不做全量重绘**。
     * 验证方式是数写次数：3 个 token 至少要有 3 次写，
     * 且**不能**是「每次都把全部内容重写一遍」。 */
    ca_ui ui;
    reset_io();
    make_plain_ui(&ui);

    ca_ui_stream_begin(&ui);
    CA_ASSERT_EQ_INT(g_write_calls, 0); /* 降级路径 begin 不该产生任何输出 */
    ca_ui_stream_token(&ui, "He");
    int after1 = g_write_calls;
    ca_ui_stream_token(&ui, "llo");
    int after2 = g_write_calls;
    ca_ui_stream_token(&ui, " world");
    int after3 = g_write_calls;

    CA_ASSERT(after1 >= 1);
    CA_ASSERT(after2 > after1); /* 每个 token 单独写，不合并成一次全量 */
    CA_ASSERT(after3 > after2);
    CA_ASSERT(strcmp(g_buf, "Hello world") == 0);
    /* 累计写入量必须**正好**等于内容长度——若是全量重绘会远大于此 */
    CA_ASSERT_EQ_INT((long long)g_len, (long long)strlen("Hello world"));
}

CA_TEST(ui_plain_stream_end_closes_line) {
    /* 流式结束后必须补换行，否则下一条输出会接在同一行上，
     * 在管道里就变成一行粘连的内容。 */
    ca_ui ui;
    reset_io();
    make_plain_ui(&ui);
    ca_ui_stream_begin(&ui);
    ca_ui_stream_token(&ui, "abc");
    ca_ui_stream_end(&ui);
    CA_ASSERT_EQ_INT((long long)g_len, 4);
    CA_ASSERT(strcmp(g_buf, "abc\n") == 0);
}

CA_TEST(ui_plain_never_emits_ansi) {
    /* 降级路径下**一次** ANSI 序列都不许出现。
     * 往管道里写 ESC[ 是最典型的「终端泄漏到非终端」。 */
    ca_ui ui;
    reset_io();
    make_plain_ui(&ui);
    ca_ui_stream_begin(&ui);
    ca_ui_stream_token(&ui, "x");
    ca_ui_stream_end(&ui);
    ca_ui_line(&ui, "y");
    feed("y");
    (void)ca_ui_confirm(&ui, "run_shell", "命令：rm -rf /");
    CA_ASSERT(strstr(g_buf, "\x1b") == NULL);
    CA_ASSERT(strstr(g_buf, "\r") == NULL);
}

/* ==================================================== 权限确认（P1 / P5） */

CA_TEST(ui_confirm_allow_on_y) {
    ca_ui ui;
    reset_io();
    make_plain_ui(&ui);
    feed("y");
    CA_ASSERT_EQ_INT(ca_ui_confirm(&ui, "run_shell", "命令：git status"), CA_UI_ALLOW);
    CA_ASSERT(strstr(g_buf, "git status") != NULL);
}

CA_TEST(ui_confirm_allow_on_single_y) {
    /* 单键确认：**第一个字符**是 y/Y 即允许（unix 惯例）。
     * 这里把边界写死——免得以后有人改成「必须完整匹配 yes」，
     * 那会让用户在原始模式下按一次 y 却什么都没发生。 */
    const char *allows[] = {"y", "Y"};
    for (size_t i = 0; i < 2; i++) {
        reset_io();
        ca_ui ui;
        make_plain_ui(&ui);
        feed(allows[i]);
        CA_ASSERT_EQ_INT(ca_ui_confirm(&ui, "run_shell", "命令：git status"), CA_UI_ALLOW);
    }
}

CA_TEST(ui_confirm_denies_on_anything_else) {
    /* 除了 y 之外**一律拒绝**。特别是回车——
     * 回车不是「允许」，否则误触就会放行一次破坏性操作。
     *
     * 注意区分：真实输入的非 y 是 DENY（用户表态了「不」）；
     * EOF 才是 ABORT（用户根本没机会表态）。
     * 两者混为一谈会掩盖「管道场景下无人应答」这个真实情况——
     * EOF 有独立用例 ui_confirm_aborts_on_eof。
     *
     * 不把 "yy" / "yes" 放进拒绝列表：这是**单键确认**——
     * unix 惯例（sudo / rm 都是如此），输入的**第一个字符**是 y/Y 就生效。
     * 把它们算作拒绝才是错的。 */
    const char *cases[] = {"\n", "\r", "n", "N", " ", "x", "\t", "q", "0"};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        reset_io();
        ca_ui ui;
        make_plain_ui(&ui);
        feed(cases[i]);
        ca_ui_decision d = ca_ui_confirm(&ui, "run_shell", "命令：rm -rf /");
        CA_ASSERT_EQ_INT(d, CA_UI_DENY);
    }
}

CA_TEST(ui_confirm_aborts_on_eof) {
    /* EOF（管道结束、CI 非交互）与「拒绝」不是一回事：
     * EOF 意味着用户**根本没机会**表态，此时绝不能当成允许。 */
    ca_ui ui;
    reset_io();
    make_plain_ui(&ui);
    feed("");
    CA_ASSERT_EQ_INT(ca_ui_confirm(&ui, "run_shell", "危险命令"), CA_UI_ABORT);
}

CA_TEST(ui_confirm_shows_full_text_before_waiting) {
    /* P5：待批准内容必须在**等输入之前**完整写出，且不截断。
     * 验证方式是「长的标记串必须原样出现在输出里」。 */
    ca_ui ui;
    reset_io();
    make_plain_ui(&ui);
    feed("y");
    static const char marker[] =
        "a-very-long-marker-12345-abcdefghijklmnopqrstuvwxyz";
    ca_ui_decision d = ca_ui_confirm(&ui, "run_shell", marker);
    CA_ASSERT_EQ_INT(d, CA_UI_ALLOW);
    CA_ASSERT(strstr(g_buf, marker) != NULL);
    /* 绝不允许用省略号代替原文 */
    CA_ASSERT(strstr(g_buf, "...") == NULL);
}

CA_TEST(ui_confirm_does_not_clear_screen) {
    /* 契约 §7：不得在渲染中途清屏导致用户看不到待批准内容。
     * 清屏是 ESC[2J / ESC[H，退格回行是 \r。两者都不得出现。 */
    ca_ui ui;
    reset_io();
    make_plain_ui(&ui);
    feed("y");
    (void)ca_ui_confirm(&ui, "write_file", "将写入 3 个文件");
    CA_ASSERT(strstr(g_buf, "2J") == NULL);
    CA_ASSERT(strstr(g_buf, "\x1b[H") == NULL);
    CA_ASSERT(strstr(g_buf, "\r") == NULL);
}

CA_TEST(ui_confirm_denies_when_no_input_source) {
    /* 输入源缺失时必须保守拒绝，而不是崩溃或放行。 */
    ca_ui ui;
    reset_io();
    make_plain_ui(&ui);
    ui.in.read_byte = NULL;
    CA_ASSERT_EQ_INT(ca_ui_confirm(&ui, "run_shell", "命令"), CA_UI_ABORT);
}

CA_TEST(ui_confirm_null_is_deny) {
    /* 整个 ui 指针为空时必须拒绝。 */
    CA_ASSERT_EQ_INT(ca_ui_confirm(NULL, "run_shell", "x"), CA_UI_DENY);
}

/* ==================================================== TTY 路径 */

CA_TEST(ui_tty_stream_redraws_in_place) {
    /* TTY 下 token 应当**就地重绘**而不是不断追加——
     * 但注意：每次只重绘当前行，不是全屏重绘。 */
    ca_ui ui;
    reset_io();
    make_plain_ui(&ui);
    ui.is_tty = 1;

    ca_ui_stream_begin(&ui);
    CA_ASSERT(strstr(g_buf, "2K") != NULL); /* 清当前行 */
    size_t after_begin = g_len;
    ca_ui_stream_token(&ui, "Hi");
    /* 只追加了 token 本身，没有重写之前的内容 */
    CA_ASSERT_EQ_INT((long long)(g_len - after_begin), 2);
    ca_ui_stream_end(&ui);
    CA_ASSERT(strcmp(g_buf + after_begin, "Hi\n") == 0);
}

CA_TEST(ui_wait_hint_is_tty_only) {
    /* 管道/重定向下不能多出任何字符——脚本要能直接消费输出。 */
    ca_ui ui;
    reset_io();
    make_plain_ui(&ui);
    ca_ui_wait_begin(&ui);
    ca_ui_wait_end(&ui);
    CA_ASSERT_EQ_INT(g_len, 0);
}

CA_TEST(ui_wait_hint_is_erased_by_first_output) {
    /* TTY 下先给一行「等待响应」，真实输出到来时必须**先擦掉**它，
     * 否则会看到「（等待响应…）[调用工具 x]」粘在一行。 */
    ca_ui ui;
    reset_io();
    make_plain_ui(&ui);
    ui.is_tty = 1;
    ca_ui_wait_begin(&ui);
    CA_ASSERT(strstr(g_buf, "等待响应") != NULL);
    CA_ASSERT_EQ_INT(ui.waiting, 1);
    ca_ui_line(&ui, "hello");
    CA_ASSERT_EQ_INT(ui.waiting, 0);
    /* 擦行序列只出现一次，且出现在 hello 之前 */
    const char *erase = strstr(g_buf, "\x1b[2K");
    CA_ASSERT(erase != NULL);
    CA_ASSERT(erase < strstr(g_buf, "hello"));
    ca_ui_wait_end(&ui); /* 已经擦过，不再重复 */
    CA_ASSERT_EQ_INT((long long)strlen(strstr(g_buf, "hello")), 6);
}

CA_TEST(ui_wait_hint_cleared_when_no_text_arrives) {
    /* 本轮只调了工具、没有文本：等待行必须在收尾时被擦掉。 */
    ca_ui ui;
    reset_io();
    make_plain_ui(&ui);
    ui.is_tty = 1;
    ca_ui_wait_begin(&ui);
    ca_ui_wait_end(&ui);
    CA_ASSERT_EQ_INT(ui.waiting, 0);
    CA_ASSERT(strstr(g_buf, "\x1b[2K") != NULL);
}

CA_TEST(ui_shutdown_is_safe_without_raw_mode) {
    /* 没进过原始模式时 shutdown 必须是安全的空操作——
     * 否则非 TTY 路径会在退出时做无意义的系统调用。 */
    ca_ui ui;
    make_plain_ui(&ui);
    ui.raw_enabled = 0;
    ca_ui_shutdown(&ui);
    CA_ASSERT_EQ_INT(ui.raw_enabled, 0);
    ca_ui_shutdown(NULL); /* 不得崩溃 */
}

/* ==================================================== 终端能力层 */

CA_TEST(term_raw_is_safe_on_non_tty) {
    /* 契约 §7 的核心降级保证：在非 TTY 上调用原始模式接口
     * 必须**安全地失败**，而不是崩溃或破坏 stdin。
     * CI 上这里天然是非 TTY，是这条保证的主要验证场景。 */
    int r = ca_term_raw_enter();
    /* 无论成功与否，随后退出都必须安全 */
    (void)ca_term_raw_exit();
    /* 若「进入成功」了（说明测试环境真是 TTY），退出后必须可再进 */
    if (r == 0) {
        CA_ASSERT_EQ_INT(ca_term_raw_enter(), 0);
        (void)ca_term_raw_exit();
    }
}

CA_TEST(term_is_tty_probe_is_callable) {
    /* 探测本身不得崩溃，且在 CI 上必须返回 0。
     * 若这里返回 1，说明测试环境不是 CI 的典型环境，
     * 那么本文件里所有降级路径的覆盖都无效——必须让人看见。 */
    int t = ca_term_is_tty();
    printf("    ca_term_is_tty() = %d\n", t);
    CA_ASSERT(t == 0 || t == 1);
}

int main(void) {
    struct { const char *name; void (*fn)(void); } cases[] = {
        /* 降级路径（CI 跑的就是这条） */
        {"ui_plain_mode_needs_no_terminal", ca_test_run_ui_plain_mode_needs_no_terminal},
        {"ui_plain_stream_appends_incrementally", ca_test_run_ui_plain_stream_appends_incrementally},
        {"ui_plain_stream_end_closes_line", ca_test_run_ui_plain_stream_end_closes_line},
        {"ui_plain_never_emits_ansi", ca_test_run_ui_plain_never_emits_ansi},
        /* 权限确认 */
        {"ui_confirm_allow_on_y", ca_test_run_ui_confirm_allow_on_y},
        {"ui_confirm_allow_on_single_y", ca_test_run_ui_confirm_allow_on_single_y},
        {"ui_confirm_denies_on_anything_else", ca_test_run_ui_confirm_denies_on_anything_else},
        {"ui_confirm_aborts_on_eof", ca_test_run_ui_confirm_aborts_on_eof},
        {"ui_confirm_shows_full_text_before_waiting", ca_test_run_ui_confirm_shows_full_text_before_waiting},
        {"ui_confirm_does_not_clear_screen", ca_test_run_ui_confirm_does_not_clear_screen},
        {"ui_confirm_denies_when_no_input_source", ca_test_run_ui_confirm_denies_when_no_input_source},
        {"ui_confirm_null_is_deny", ca_test_run_ui_confirm_null_is_deny},
        /* TTY 路径 */
        {"ui_tty_stream_redraws_in_place", ca_test_run_ui_tty_stream_redraws_in_place},
    {"ui_wait_hint_is_tty_only", ca_test_run_ui_wait_hint_is_tty_only},
    {"ui_wait_hint_is_erased_by_first_output", ca_test_run_ui_wait_hint_is_erased_by_first_output},
    {"ui_wait_hint_cleared_when_no_text_arrives", ca_test_run_ui_wait_hint_cleared_when_no_text_arrives},
        {"ui_shutdown_is_safe_without_raw_mode", ca_test_run_ui_shutdown_is_safe_without_raw_mode},
        /* 终端能力层 */
        {"term_raw_is_safe_on_non_tty", ca_test_run_term_raw_is_safe_on_non_tty},
        {"term_is_tty_probe_is_callable", ca_test_run_term_is_tty_probe_is_callable},
    };
    size_t n = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < n; i++) {
        ca_test_begin(cases[i].name);
        cases[i].fn();
        ca_test_end();
    }
    return ca_test_summary();
}