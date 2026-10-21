/* main.c —— 可执行入口（PRD US-1 / US-7）
 *
 * 这一层刻意薄：**配置解析 + 读一行 + 交给 core**。
 * 业务逻辑一概不在这里 —— 一旦 main 里出现「判断要不要批准」
 * 或「怎么拼请求」，那些逻辑就再也测不到了。
 *
 * 配置取自环境变量：
 *   CAGENT_API_BASE  必填，例如 https://api.example.com/v1
 *   CAGENT_MODEL     必填，服务端模型名
 *   CAGENT_API_KEY   可选（本地无鉴权服务可不填）
 *   CAGENT_SYSTEM    可选，系统提示
 *   CAGENT_ALLOW_SHELL  可选，设为 1 才允许执行命令
 *   CAGENT_CONTEXT_BYTES 可选，单次请求上限（默认 256 KB，超限丢弃最旧轮次）
 *
 * 密钥只存在于进程内存：**不打印、不落盘**（PRD NFR-3）。
 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "cagent/core.h"
#include "cagent/http.h"
#include "cagent/mem.h"
#include "cagent/tools.h"
#include "cagent/transport_mem.h"
#include "cagent/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <direct.h>
#  define ca_getcwd _getcwd
#else
#  include <unistd.h>
#  define ca_getcwd getcwd
#endif

#define CA_MAIN_INPUT_MAX 4096
#define CA_MAIN_CWD_MAX 4096

/* ca_core 内含 1 MB tool_out + 256 KB results。
 * **放在栈上会直接爆栈**（core_test 曾因这个崩溃），必须是静态存储。 */
static ca_core g_core;

/* 会话（US-6）。历史预算 4 MB，与契约 §2.1 一致。
 * 会话目录取不到时（没有 HOME/APPDATA 之类）**降级**为不落盘：
 * 内存里的多轮记忆照常工作，只是关掉进程就没了。 */
static ca_session g_session;
static char g_sess_dir[CA_SESSION_MAX_PATH];
static int g_sess_dir_ok;

static const char *k_banner =
    "cagent —— 低内存终端代理（C99）\n"
    "输入 /help 查看命令，/exit 退出。\n";

static void print_usage(FILE *f) {
    fprintf(f,
            "用法：设置环境变量后直接运行 cagent\n"
            "\n"
            "  CAGENT_API_BASE   必填，服务地址，如 https://api.example.com/v1\n"
            "  CAGENT_MODEL      必填，模型名\n"
            "  CAGENT_API_KEY    可选，鉴权用；不填则不发送 Authorization\n"
              "  CAGENT_SYSTEM    可选，系统提示\n"
            "  CAGENT_ALLOW_SHELL  可选，设为 1 才允许模型执行命令（默认拒绝）\n"
            "  CAGENT_CONTEXT_BYTES  可选，单次请求上限（字节，默认 262144，范围 16K~4M）\n"
              "\n"
              "模型要写文件或执行命令时，会先把具体内容列出来并问 [y/N]，\n"
              "只有明确输入 y 才会执行。\n"
            "\n"
            "示例（PowerShell）：\n"
            "  $env:CAGENT_API_BASE=\"https://api.example.com/v1\"\n"
            "  $env:CAGENT_MODEL=\"your-model\"\n"
            "  $env:CAGENT_API_KEY=\"sk-...\"\n"
            "  .\\cagent\n");
}

/* ---- 内置命令 ---- */

static void print_mem(ca_ui *ui, const char *transport_name) {
    int64_t cur = ca_rss_bytes();
    int64_t peak = ca_peak_rss_bytes();
    /* US-7：给**具体数字**而不是「大约几十兆」。
     * 取不到就明说取不到 —— 编一个数字比不报更糟。 */
    if (cur < 0) {
        ca_ui_line(ui, "当前 RSS：无法获取（该平台未提供）");
    } else {
        ca_ui_printf(ui, "当前 RSS：%.2f MB\n", (double)cur / 1048576.0);
    }
    if (peak < 0) {
        ca_ui_line(ui, "峰值 RSS：无法获取（该平台未提供）");
    } else {
        ca_ui_printf(ui, "峰值 RSS：%.2f MB\n", (double)peak / 1048576.0);
    }
    /* 报告**实际在用**的后端，而不是编译期默认值：
     * 离线自检时 ca_transport_name() 仍返回 "none"，
     * 照抄它会让 /mem 显示一个与事实不符的后端名。 */
    ca_ui_printf(ui, "传输后端：%s\n", transport_name);
}

/* 取命令的第一个参数（跳过一个以上的空白）。没有参数返回 NULL。 */
static const char *cmd_arg(const char *line) {
    const char *p = line;
    while (*p != '\0' && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    return (*p == '\0') ? NULL : p;
}

/* 落盘失败必须说出来。早先这里 `(void)` 掉了返回值，于是 Windows 上
 * rename() 因为目标已存在而失败时，每一轮都静默丢弃保存：
 * 文件永远停在第一轮，用户以为会话还在（本缺陷就是这样被压了整轮开发）。 */
static int g_save_warned;

static void save_session_quietly(void) {
    if (!g_sess_dir_ok || ca_session_count(&g_session) == 0) return;
    if (ca_session_save(&g_session, g_sess_dir) != 0 && !g_save_warned) {
        g_save_warned = 1;
        fprintf(stderr, "警告：会话未能写入 %s，本次对话退出后不会保留。\n",
                g_sess_dir);
    }
}

/* ---- 工具权限策略（契约 §5.2 P1 默认拒绝） ----
 *
 * 分层要分清：**问不问用户**由 core 决定（`core_perm` 一定先把待批准内容
 * 完整呈现出来，P5），**允不允许**由这里的策略决定。
 *
 * 早先 main 传的是 `perm = NULL`，core 的语义是「仍然提示、但一律拒绝」——
 * 于是用户在提示处按了 y 也没用，读写文件/执行命令**全部不可用**。
 * 那是把「默认拒绝」错当成了「默认不可用」。
 *
 * 现在的策略：
 *   - 用户在提示处明确同意（y）后，读/写/改/列目录放行；
 *   - `run_shell` 额外要求 `CAGENT_ALLOW_SHELL=1`：执行任意命令的风险
 *     与文件读写不是一个量级，值得多加一道**显式**开关，而不是随默认放行。
 */
static int g_allow_shell;

static ca_permission main_perm(const char *tool_name, void *user) {
    if (tool_name != NULL && strcmp(tool_name, "run_shell") == 0 && !g_allow_shell) {
        ca_ui_line((ca_ui *)user,
                   "（按策略拒绝执行命令：需要显式开启，设置 CAGENT_ALLOW_SHELL=1 后重试）");
        return CA_PERM_DENY;
    }
    /* 走到这里说明用户在提示处已经明确同意过一次。 */
    return CA_PERM_ALLOW;
}

/* 返回 1 表示要求退出。 */
static int run_command(ca_ui *ui, const char *line, const char *transport_name) {
    if (strcmp(line, "/exit") == 0 || strcmp(line, "/quit") == 0) return 1;
    if (strcmp(line, "/help") == 0) {
        ca_ui_line(ui, "命令：");
        ca_ui_line(ui, "  /help          显示本帮助");
        ca_ui_line(ui, "  /mem           显示当前与峰值 RSS");
        ca_ui_line(ui, "  /new           开始新会话（当前对话从记忆中清除）");
        ca_ui_line(ui, "  /list          列出历史会话（新到旧）");
        ca_ui_line(ui, "  /resume <id>   继续指定的会话");
        ca_ui_line(ui, "  /delete <id>   删除指定的会话");
        ca_ui_line(ui, "  /exit          退出");
        ca_ui_line(ui, "");
        ca_ui_line(ui, "模型要改文件或执行命令时会先列出具体内容并问 [y/N]，");
        ca_ui_line(ui, "只有输入 y 才执行。执行命令需 CAGENT_ALLOW_SHELL=1。");
        return 0;
    }
    if (strcmp(line, "/mem") == 0) {
        print_mem(ui, transport_name);
        ca_ui_printf(ui, "会话历史：%lu / %lu 字节，%lu 条消息\n",
                     (unsigned long)ca_session_used(&g_session),
                     (unsigned long)ca_session_budget(&g_session),
                     (unsigned long)ca_session_count(&g_session));
        return 0;
    }
    if (strcmp(line, "/new") == 0) {
        /* 只清内存，不动磁盘：开启新会话不该顺手删掉旧会话。 */
        save_session_quietly();
        g_session.count = 0;
        g_session.used = 0;
        g_session.buf[0] = '\0';
        g_session.id[0] = '\0';
        ca_ui_line(ui, "已开始新会话。");
        return 0;
    }
    if (strcmp(line, "/list") == 0) {
        if (!g_sess_dir_ok) {
            ca_ui_line(ui, "无法确定会话目录（缺少 HOME/APPDATA），本会话不会落盘。");
            return 0;
        }
        ca_session_info infos[32];
        size_t n = 0;
        int rc = ca_session_list(g_sess_dir, infos, 32, &n);
        if (rc < 0) {
            ca_ui_printf(ui, "会话目录不可读：%s\n", g_sess_dir);
            return 0;
        }
        if (n == 0) {
            ca_ui_line(ui, "（还没有历史会话）");
            return 0;
        }
        ca_ui_printf(ui, "会话（新到旧，共 %lu 个）：\n", (unsigned long)n);
        for (size_t i = 0; i < n; i++) {
            if (infos[i].corrupt) {
                /* 损坏要**标出来**：悄悄不显示会让用户以为会话丢了 */
                ca_ui_printf(ui, "  %-24s [损坏，已跳过]\n", infos[i].id);
            } else {
                ca_ui_printf(ui, "  %-24s %d 条消息\n", infos[i].id, infos[i].msgs);
            }
        }
        return 0;
    }
    if (strncmp(line, "/resume", 7) == 0) {
        const char *id = cmd_arg(line);
        if (id == NULL) { ca_ui_line(ui, "用法：/resume <id>（id 见 /list）"); return 0; }
        if (!g_sess_dir_ok) { ca_ui_line(ui, "会话目录不可用，无法恢复。"); return 0; }
        /* 先保存当前会话，再切换 —— 不然用户会丢掉正在进行的对话。 */
        save_session_quietly();
        char path[CA_SESSION_MAX_PATH];
        if (ca_session_path_for(g_sess_dir, id, path, sizeof(path)) != 0) {
            ca_ui_line(ui, "无效的会话 id（id 见 /list）。");
            return 0;
        }
        int rc = ca_session_load(&g_session, path);
        if (rc == -1) {
            /* 坏文件不该让程序退出，也不该被顺手删掉：用户可能还想自己看看。 */
            ca_ui_line(ui, "无法恢复：该会话文件损坏或不存在（未做任何改动）。");
            return 0;
        }
        if (rc == -2) {
            ca_ui_line(ui, "该会话超出历史上限，已按轮次淘汰后恢复（最早的轮次被丢弃）。");
        }
        ca_ui_printf(ui, "已恢复会话 %s：%lu 条消息。\n", id,
                     (unsigned long)ca_session_count(&g_session));
        return 0;
    }
    if (strncmp(line, "/delete", 7) == 0) {
        const char *id = cmd_arg(line);
        if (id == NULL) { ca_ui_line(ui, "用法：/delete <id>（id 见 /list）"); return 0; }
        if (!g_sess_dir_ok) { ca_ui_line(ui, "会话目录不可用，无法删除。"); return 0; }
        if (ca_session_delete(g_sess_dir, id) != 0) {
            ca_ui_printf(ui, "删除失败：%s（id 是否正确？）\n", id);
        } else {
            ca_ui_printf(ui, "已删除会话 %s。\n", id);
        }
        return 0;
    }
    /* 不认识的命令**不当成对话内容**发出去：
     * 那会让模型去回答一个用户本来只想打命令的问题。 */
    ca_ui_printf(ui, "未知命令：%s（/help 查看可用命令）\n", line);
    return 0;
}

/* ---- 读取一行 ----
 *
 * 不用 fgets：ui 在 TTY 下已进入原始模式（关掉行缓冲与回显），
 * fgets 会一直阻塞到换行且用户看不到自己打的字。
 * 这里按字节读自己回显，两条路径（TTY / 管道）行为一致，
 * 于是同一套代码在 CI 的管道里也能跑。
 *
 * 返回：1 取到一行；0 EOF；-1 行过长；-2 用户中断（Ctrl-C）。
 */
static int read_line(ca_ui *ui, char *buf, size_t cap, size_t *out_len) {
    size_t n = 0;
    int too_long = 0;
    *out_len = 0;
    if (buf == NULL || cap == 0) return -1;

    for (;;) {
        int c = (ui->in.read_byte != NULL) ? ui->in.read_byte(ui->in.self) : -1;

        if (c < 0) {
            /* EOF：已输入的内容仍算一行（管道最后一行常没有换行）。 */
            if (too_long) return -1;
            if (n == 0) return 0;
            buf[n] = '\0';
            *out_len = n;
            return 1;
        }
        if (c == 3) { /* Ctrl-C */
            if (ui->is_tty) ca_ui_printf(ui, "\n");
            return -2;
        }
        if (c == '\n' || c == '\r') {
            if (ui->is_tty) ca_ui_printf(ui, "\n");
            if (too_long) return -1;
            buf[n] = '\0';
            *out_len = n;
            return 1;
        }
        if (c == 127 || c == 8) { /* Backspace */
            if (n > 0) {
                n--;
                if (ui->is_tty) ca_ui_printf(ui, "\b \b");
            }
            continue;
        }
        if (c == 4) { /* Ctrl-D：行首表示 EOF，行内忽略 */
            if (n == 0) return 0;
            continue;
        }
        if (too_long) continue; /* 丢弃剩余部分，直到换行 */

        if (n + 1 >= cap) {
            /* 超长**不截断**：截断后发出去是一句用户没说过的半句话。 */
            too_long = 1;
            continue;
        }
        buf[n++] = (char)c;
        if (ui->is_tty) ca_ui_printf(ui, "%c", c);
    }
}

/* ---- 入口 ---- */

/* 版本自报。发布出来的二进制必须能说清自己是什么：
 * 版本、编译器标准、以及**本构建有没有链接 libcurl**——
 * 最后这条是最容易出错也最需要一眼看到的（"为什么连不上网"多半是构建问题）。
 * 刻意不做网络初始化：--version 在离线机器上也必须能跑。 */
static void print_version(void) {
    const char *backend = ca_transport_default() != NULL
                              ? "libcurl"
                              : "memory（本构建未链接 libcurl，无法联网）";
    /* 格式串一律保持**纯 ASCII**，中文作为参数传入。
     * 实测：MSVC 的格式串分析器遇到含 UTF-8 中文的 printf 格式串时，
     * 会误报 C4474「参数太多」（同样的串在 gcc/clang 上无告警）。
     * 这跟本仓库早先记过的 C4819 是同一类工具链问题——
     * 与其抑制编号，不如写成两种编译器都认的形式。 */
    printf("%s %s (C99)\n", "cagent", CAGENT_VERSION);
    printf("%s%s\n", "传输后端：", backend);
}

static void print_cli_help(void) {
    printf("%s",
           "用法：cagent [选项]\n"
           "\n"
           "  -h, --help     显示本帮助\n"
           "  -v, --version  显示版本与传输后端\n"
           "  （无选项）      进入交互会话；配置取自环境变量，见下\n"
           "\n");
    print_usage(stdout);
}

int main(int argc, char **argv) {
    /* 选项必须在读环境变量之前处理：--version / --help 不该要求配置。 */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a == NULL) continue;
        if (strcmp(a, "--version") == 0 || strcmp(a, "-v") == 0) {
            print_version();
            return 0;
        }
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            print_cli_help();
            return 0;
        }
        fprintf(stderr, "%s%s%s\n", "未知选项：", a, "（试试 --help）");
        return 2;
    }

    const char *base = getenv("CAGENT_API_BASE");
    const char *model = getenv("CAGENT_MODEL");
    const char *key = getenv("CAGENT_API_KEY");
    const char *sys = getenv("CAGENT_SYSTEM");
    const char *transport_env = getenv("CAGENT_TRANSPORT");

    /* 命令执行默认关（见 main_perm）。"0"/空串都算没开，
     * 免得 `CAGENT_ALLOW_SHELL=` 这种写法被误读成开启。 */
    const char *allow_shell = getenv("CAGENT_ALLOW_SHELL");
    g_allow_shell = (allow_shell != NULL && allow_shell[0] != '\0' &&
                     strcmp(allow_shell, "0") != 0);

    /* 请求上限（字节）。不填用默认值；写了非数字就明说并按默认走——
     * 静默忽略一个配置项，用户会一直以为它生效了。 */
    size_t context_bytes = 0;
    const char *ctx_env = getenv("CAGENT_CONTEXT_BYTES");
    if (ctx_env != NULL && ctx_env[0] != '\0') {
        char *end = NULL;
        unsigned long v = strtoul(ctx_env, &end, 10);
        if (end == ctx_env || *end != '\0' || v == 0) {
            fprintf(stderr, "%s\n",
                    "CAGENT_CONTEXT_BYTES 不是正整数，已改用默认上限（262144）。");
        } else {
            context_bytes = (size_t)v;
        }
    }

    if (base == NULL || base[0] == '\0' || model == NULL || model[0] == '\0') {
        print_usage(stderr);
        return 2;
    }

    /* 离线自检模式（CAGENT_TRANSPORT=mem）：
     * 用预设的 SSE 走完整链路（sse → llm → core → ui），不联网。
     *
     * 存在的理由不是「方便」：
     *   1. US-1/US-2 的端到端行为需要**可重复**的验证，
     *      真实服务给什么内容不受控，测不了「逐字上屏」这类性质；
     *   2. 没有 libcurl 开发包的机器也能确认这个二进制是好的，
     *      而不是只能看到一个「无法联网」的退出码。
     * 它**不是**假装联网：界面会明确标注当前是离线模式。 */
    const ca_transport_ops *tr = NULL;
    int offline = 0;
    if (transport_env != NULL && strcmp(transport_env, "mem") == 0) {
        static const char canned[] =
            "data: {\"choices\":[{\"delta\":{\"content\":\"（离线自检模式：\"}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{\"content\":\"本构建未联网，这是预设回复。）\"}}]}\n\n"
            "data: [DONE]\n\n";
        ca_mem_transport_set_payload((const unsigned char *)canned,
                                     sizeof(canned) - 1, 200);
        tr = ca_transport_mem();
        offline = 1;
    } else {
        tr = ca_transport_default();
        if (tr == NULL) {
            /* 如实说：这不是配置问题，是这个二进制没法联网。
             * 含糊地说「连接失败」会让人去查网络，而问题在构建。
             * 同时给出**能立刻用**的替代：离线自检模式。 */
            fprintf(stderr,
                    "本构建未链接 libcurl，无法发起真实请求。\n"
                    "请在有 libcurl 开发包的环境下构建（CI 三平台均已覆盖），\n"
                    "或设置 CAGENT_TRANSPORT=mem 以离线自检模式运行（不发网络请求）。\n");
            return 3;
        }
    }

    char cwd[CA_MAIN_CWD_MAX];
    if (ca_getcwd(cwd, sizeof(cwd)) == NULL) {
        fprintf(stderr, "无法取得当前目录，工具的工作区无从确定。\n");
        return 4;
    }

    ca_workspace *ws = ca_ws_open(cwd);
    if (ws == NULL) {
        fprintf(stderr, "无法打开工作区：%s\n", cwd);
        return 4;
    }

    ca_ui ui;
    ca_ui_sink sink;
    ca_ui_src src;
    ca_term_default_sink(&sink);
    ca_term_default_src(&src);
    if (ca_ui_init(&ui, sink, src) != 0) {
        fprintf(stderr, "界面初始化失败。\n");
        ca_ws_free(ws);
        return 5;
    }

    if (tr->init != NULL && tr->init() != 0) {
        ca_ui_line(&ui, "传输层初始化失败。");
        ca_ui_shutdown(&ui);
        ca_ws_free(ws);
        return 3;
    }

    ca_core_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.transport = tr;
    cfg.api_base = base;
    cfg.model = model;
    cfg.api_key = key;
    cfg.system = sys;
    cfg.ui = &ui;
    cfg.ws = ws;
    /* 权限策略：模型每次要动文件/跑命令，core 都会先把**具体内容**
     * 摆给用户看并等一个明确的 y；这里决定「用户同意后放不放行」。
     * 传 NULL 会让所有工具无条件被拒(提示照出、y 无效)，所以必须接上。 */
    cfg.perm = main_perm;
    cfg.perm_user = &ui;
    /* 单次请求上限：0 表示用默认（256 KB）。core 会把它钳制到
     * [16 KB, 4 MB]，钳制后的真实值从 ca_core_context_bytes() 取。 */
    cfg.context_bytes = context_bytes;

    /* 会话（US-6）：预算传 0 → 用契约规定的历史上限。
     * 拿不到会话目录不算致命：降级为「只在内存里记得」，
     * 并在启动时如实说明，而不是假装能恢复。 */
    if (ca_session_init(&g_session, 0) != 0) {
        fprintf(stderr, "会话初始化失败（内存不足）。\n");
        if (tr->cleanup != NULL) tr->cleanup();
        ca_ui_shutdown(&ui);
        ca_ws_free(ws);
        return 2;
    }
    g_sess_dir_ok = (ca_session_dir(g_sess_dir, sizeof(g_sess_dir)) == 0 &&
                     ca_session_dir_ensure(g_sess_dir) == 0);
    cfg.session = &g_session;

    ca_core_status st = ca_core_init(&g_core, &cfg);
    if (st != CA_CORE_OK) {
        ca_ui_printf(&ui, "初始化失败：%s\n", ca_core_status_text(st));
        if (tr->cleanup != NULL) tr->cleanup();
        ca_ui_shutdown(&ui);
        ca_ws_free(ws);
        return 2;
    }

    if (ui.is_tty) {
        ca_ui_line(&ui, k_banner);
        /* 上下文上限决定「聊多久会开始丢历史」。用户有权在开始前知道，
         * 而不是在历史被丢之后才从提示里猜。 */
        ca_ui_printf(&ui, "上下文上限：%lu KB\n",
                     (unsigned long)(ca_core_context_bytes(&g_core) / 1024u));
        /* 工具能写到哪里、会话存在哪里，用户一进来就该看得见：
         * 批准写入时若不知道工作区是哪个目录，那个「同意」是盲目的。 */
        ca_ui_printf(&ui, "工作区：%s\n", cwd);
        ca_ui_printf(&ui, "会话：%s\n",
                     g_sess_dir_ok ? g_sess_dir
                                   : "（目录不可用，仅保存在内存中，退出即丢失）");
    }
    static char line[CA_MAIN_INPUT_MAX];
    int exit_code = 0;

    if (offline) {
        /* 离线模式必须在界面上**明说**，否则用户会以为
         * 模型真的这样回答 —— 那是把假的当真。 */
        ca_ui_line(&ui, "离线自检模式：不发起任何网络请求，回复是预设内容。");
    }

    for (;;) {
        ca_ui_printf(&ui, "> ");

        size_t len = 0;
        int r = read_line(&ui, line, sizeof(line), &len);

        if (r == 0 || r == -2) break; /* EOF 或 Ctrl-C：正常退出 */
        if (r < 0) {
            ca_ui_line(&ui, "输入过长，已丢弃该行（上限 4095 字节）。");
            continue;
        }
        if (len == 0) continue; /* 空行不发请求：省一次计费调用 */

        if (line[0] == '/') {
            if (run_command(&ui, line, offline ? "memory（离线自检）" : ca_transport_name())) break;
            continue;
        }

        st = ca_core_run_once(&g_core, line);
        if (st != CA_CORE_OK) {
            /* 错误**已经由 core 打印**（它知道 last_error）。
             * 这里再打一次会造成同样的「错误：…」连出两行——
             * 真实故障现场里正是这样，用户以为失败了两回。 */
        } else {
            /* 每轮结束就落盘：崩溃/断电至多丢一轮，而不是整段对话。 */
            save_session_quietly();
        }
    }

    /* 退出时把 id 报出来，否则用户下次无从 /resume——
     * 会话存了却没人知道它叫什么，等于没存。 */
    save_session_quietly();
    if (g_sess_dir_ok && g_session.id[0] != '\0') {
        ca_ui_printf(&ui, "本次会话已保存：%s（用 /resume %s 继续，/list 查看全部）\n",
                     g_session.id, g_session.id);
    } else if (!g_sess_dir_ok) {
        ca_ui_line(&ui, "会话未落盘（无法确定会话目录），退出后不保留。");
    }
    ca_session_free(&g_session);
    ca_core_free(&g_core);

    if (tr->cleanup != NULL) tr->cleanup();
    ca_ui_shutdown(&ui); /* 必须在任何退出路径上还原终端 */
    ca_ws_free(ws);
    return exit_code;
}
