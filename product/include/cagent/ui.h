/* ui.h —— 终端界面层（契约 §7）
 *
 * 设计要点：**渲染逻辑与终端能力分离**。
 *
 * ca_ui 只知道「写到哪儿去」和「从哪儿读」，它不碰 termios、
 * 不碰 ANSI 转义、不判断是不是 TTY。所有平台差异收拢在
 * ca_term_* 里（契约 §8 要求「差异不得泄漏到业务逻辑」）。
 *
 * 这么分不是为了好看，而是因为**非 TTY 路径必须在 CI 里测**：
 * CI runner 就是非 TTY。如果渲染逻辑和终端能力缠在一起，
 * 非 TTY 那条路径就只能靠人工点，而它恰恰是最容易坏、
 * 且坏了会直接炸 CI 的那条。
 *
 * 由此得到的结构：
 *   ca_ui          —— 状态机与渲染决策（纯逻辑，可测）
 *   ca_term_tty    —— ANSI + 原始模式 + 按键
 *   ca_term_plain  —— 行式输出，CI 跑的就是这条
 */
#ifndef CAGENT_UI_H
#define CAGENT_UI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 输出端（由 ca_term_* 提供；测试可注入自定义实现）----
 * 约定：write 返回实际写入字节数，失败返回负数。 */

typedef struct {
    int (*write)(void *self, const char *data, size_t len);
    void *self;
} ca_ui_sink;

/* ---- 输入端 ---- */

typedef struct {
    /* 读一个字节；返回 1=有数据, 0=EOF, -1=错误 */
    int (*read_byte)(void *self);
    void *self;
} ca_ui_src;

/* ---- 权限询问的结果 ---- */

typedef enum {
    CA_UI_ALLOW = 0,
    CA_UI_DENY = 1,
    CA_UI_ABORT = 2 /* Ctrl-C 或读入失败 */,
} ca_ui_decision;

/* ---- ui 状态 ---- */

typedef struct {
    ca_ui_sink out;
    ca_ui_src in;

    int is_tty;        /* 0 时走行式降级（契约 §7） */
    int raw_enabled;   /* 原始模式是否已启用，退出时需还原 */

    int spinner;       /* 是否有进行中的流式输出需要收尾 */
    int waiting;       /* 是否已打出「等待响应」且尚未被新输出顶掉（仅 TTY） */
    int last_was_newline; /* 上一行是否已闭合，决定要不要补换行 */
    char scratch[512]; /* 组合渲染行时用，避免每次都堆内存 */
} ca_ui;

/* ---- 生命周期 ---- */

int  ca_ui_init(ca_ui *ui, ca_ui_sink out, ca_ui_src in);
void ca_ui_shutdown(ca_ui *ui);

/* ---- 普通输出 ---- */

void ca_ui_line(ca_ui *ui, const char *s);   /* 一行，自动补 \n */
void ca_ui_printf(ca_ui *ui, const char *fmt, ...);

/* ---- 视觉增强（仅 TTY 生效）----
 *
 * 非 TTY 路径必须保持纯文本：CI 消费者会逐行比对，装饰性输出会变成噪音。
 * 所以下面这些函数在 is_tty == 0 时退化成普通文本输出。
 *
 * 颜色用 ANSI SGR，TTY 之外不写任何转义序列。 */
void ca_ui_heading(ca_ui *ui, const char *title);  /* 章节标题（带下划线） */
void ca_ui_panel(ca_ui *ui, const char *title, const char *body); /* 盒面板 */
void ca_ui_kv(ca_ui *ui, const char *key, const char *value);    /* 键值行 */
void ca_ui_ok(ca_ui *ui, const char *text);        /* 成功（绿） */
void ca_ui_warn(ca_ui *ui, const char *text);      /* 警告（黄） */
void ca_ui_error(ca_ui *ui, const char *text);     /* 错误（红） */
void ca_ui_note(ca_ui *ui, const char *text);      /* 次要信息（灰） */

/* ---- 流式输出（契约 §7：逐 token 追加，不做全量重绘）----
 *
 * begin/token/end 三个动作。TUI 下 token 会覆盖同一行并重绘光标，
 * 行式降级下则是直接追加换行——两条路径各自正确，
 * 上层不需要知道自己跑在哪种终端上。 */
/* 请求已发出、首个 token 还没到：TTY 下给一行等待提示。
 * 非 TTY 下什么都不做——管道输出必须干净（契约 §7 降级）。 */
void ca_ui_wait_begin(ca_ui *ui);
void ca_ui_wait_end(ca_ui *ui);

void ca_ui_stream_begin(ca_ui *ui);
void ca_ui_stream_token(ca_ui *ui, const char *text);
void ca_ui_stream_end(ca_ui *ui);

/* ---- 权限确认（契约 §5.2 P1/P5）----
 *
 * 这是整个 ui 层最关键的一处：
 * 待批准内容必须在**等输入之前**完整写出去，
 * 并且**不清屏**。折叠命令等于让用户盲批一件事他没看清的事。
 *
 * 注意 text 是**原文**，本函数不做任何截断或省略。 */
ca_ui_decision ca_ui_confirm(ca_ui *ui, const char *tool, const char *text);

/* ---- 终端能力（平台层）---- */

/* 探测当前输出是否为 TTY。实现上直接问系统，不做猜测。 */
int  ca_term_is_tty(void);

/* 进入/退出原始模式。非 TTY 下两者都是**安全的空操作**，
 * 而不是失败——契约 §7 要求「原始模式不可用时必须能降级」。
 *
 * ca_term_raw_exit 必须能在**信号处理函数**里调用：用户按 Ctrl-C 时
 * 进程会立刻结束，此时若不还原控制台模式，用户的终端会留在
 * 「无回显」状态——之后敲什么都不显示，终端看起来坏掉了。 */
int  ca_term_raw_enter(void);
int  ca_term_raw_exit(void);

/* 控制台准备：Windows 上把输出代码页设为 UTF-8 并打开 ANSI 支持。
 * 不设的话，中文在简体中文控制台（代码页 936）上是乱码——
 * 用户看到的现象是「界面什么都不显示」，与程序逻辑无关。 */
void ca_term_setup_console(void);

/* 默认 sink / src，供 main 使用。 */
void ca_term_default_sink(ca_ui_sink *out);
void ca_term_default_src(ca_ui_src *in);

#ifdef __cplusplus
}
#endif

#endif /* CAGENT_UI_H */