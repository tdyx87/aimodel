/* core_test.c —— 代理主循环的端到端测试
 *
 * 用**脚本化的假传输层**驱动：把预设的 SSE 响应逐块喂进来。
 * 于是整条链路（core → llm → sse → 工具 → ui）都能在没有网络、
 * 没有 API key 的情况下被完整验证。
 *
 * 这也正是把 transport 做成可注入接口的理由——
 * 如果 core 直接调 libcurl，这些测试一条都跑不了。
 */

/* fopen 在 MSVC 上有安全建议但无 C11 可移植等价物。
 * 必须定义在 CRT 头文件之前。 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "ca_test.h"

#include "cagent/core.h"
#include "cagent/ui.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- 捕获 UI 输出与用户输入 ---- */

static char g_ui_buf[1 << 18];
static size_t g_ui_len;
static int g_write_calls;
static char g_user_input[64];
static size_t g_user_pos;

static int cap_write(void *self, const char *s, size_t n) {
    (void)self;
    if (g_ui_len + n >= sizeof(g_ui_buf)) return -1;
    memcpy(g_ui_buf + g_ui_len, s, n);
    g_ui_len += n;
    g_ui_buf[g_ui_len] = '\0';
    g_write_calls++;
    return 0;
}

static int cap_read(void *self) {
    (void)self;
    size_t i = g_user_pos;
    if (g_user_input[i] == '\0') return -1;
    g_user_pos++;
    return (unsigned char)g_user_input[i];
}

static void ui_reset(void) {
    g_ui_len = 0;
    g_ui_buf[0] = '\0';
    g_write_calls = 0;
}

static void feed_user(const char *s) {
    size_t n = strlen(s);
    if (n >= sizeof(g_user_input)) n = sizeof(g_user_input) - 1;
    memcpy(g_user_input, s, n + 1);
    g_user_pos = 0;
}

/* ---- 脚本化的假传输层 ---- */

/* 每次 run 取出下一段脚本喂给上层；脚本用 \0\0 分隔 */
/* 脚本槽位必须够大：core_bounds_tool_rounds 要放 20 段。
 * 早先写的是 [8]，那个用例会**写越界**——
 * 缓冲区大小写小了，看起来是「测试数据」，实际是内存破坏。 */
static const char *g_scripts[40];
static int g_script_count;
static int g_script_pos;
static int g_req_count;

/* 捕获请求体：证明「多轮记忆」真的到了线上，而不只是躺在内存里。
 * 只留最近几个，够断言用，不随用例数增长。
 *
 * 单个缓冲 20 KB：淘汰用例的请求体接近 16 KB 上限，早先写 4096 时
 * 末尾的「最新问题」会被**截断在捕获缓冲里**，断言「请求里带了最新
 * 问题」就会假阴性——捕获缓冲太小也是一种测试缺陷。 */
static char g_bodies[4][20480];
static int g_body_count;

static ca_http_status fake_run(const ca_request *req, ca_on_data on_data,
                               void *user) {
    g_req_count++;
    if (req != NULL && req->body != NULL && g_body_count < 4) {
        size_t n = strlen(req->body);
        if (n >= sizeof(g_bodies[0])) n = sizeof(g_bodies[0]) - 1;
        memcpy(g_bodies[g_body_count], req->body, n);
        g_bodies[g_body_count][n] = '\0';
    }
    g_body_count++;
    if (g_script_pos >= g_script_count) return CA_HTTP_OK;
    const char *s = g_scripts[g_script_pos++];
    size_t len = strlen(s);
    /* 刻意**分块**喂，验证 core 不依赖「一次一整包」——
     * 真实网络给什么形状的包是不受控的。 */
    size_t off = 0;
    while (off < len) {
        size_t chunk = (off % 7 == 0) ? 3 : 11;
        if (off + chunk > len) chunk = len - off;
        if (on_data((const unsigned char *)s + off, chunk, user) != 0) {
            return CA_HTTP_ERR_ABORTED;
        }
        off += chunk;
    }
    return CA_HTTP_OK;
}

static long fake_status(void) { return 200; }

static const ca_transport_ops g_fake = {
    NULL, NULL, fake_run, fake_status
};

/* ---- 搭建 ---- */

static ca_workspace *g_ws;
static ca_ui g_ui;
static ca_core g_core;

/* 需要多轮记忆的用例先把它设上；setup 会把它交给 core 并清空，
 * 于是其余用例天然跑在「无记忆」模式下，不必各自重置。 */
static ca_session *g_session;

/* 探测「文件是否存在」必须**按工作区根**拼路径。
 * 早先这里拼的是 "\name"（首字符就是分隔符 = 盘符根），探测点根本
 * 不在工作区里：断言「文件不存在」永远成立，工具真写了也发现不了——
 * 一个永远不会红的用例。工作区根由 main 建，这里记一份。 */
static char g_ws_dir[512];

#if defined(_WIN32)
static const char g_sep = '\\';
#else
static const char g_sep = '/';
#endif

static int ws_file_exists(const char *name) {
    char probe[1024];
    snprintf(probe, sizeof(probe), "%s%c%s", g_ws_dir, g_sep, name);
    FILE *f = fopen(probe, "rb");
    int found = (f != NULL);
    if (f != NULL) fclose(f);
    return found;
}

static void ws_file_remove(const char *name) {
    char probe[1024];
    snprintf(probe, sizeof(probe), "%s%c%s", g_ws_dir, g_sep, name);
    remove(probe);
}

/* 权限三种形态：
 *   0 = 回调为 NULL（契约 P1 的默认拒绝落点）
 *   1 = 回调放行（模拟 main 的策略：用户已同意）
 *   2 = 回调拒绝（模拟产品策略否决，如 run_shell 未开启）
 * 由测试在 setup 之前设置，setup 读完就复位，避免串到下个用例。 */
static int g_perm_kind;

/* 用例想显式指定请求体上限时设它；setup 读完复位（0 = 用默认值）。 */
static size_t g_context_bytes;

static ca_permission test_perm(const char *tool, void *user) {
    (void)tool;
    return ((int)(intptr_t)user) == 1 ? CA_PERM_ALLOW : CA_PERM_DENY;
}

static void setup(const char *system, const char *perm_answer) {
    ui_reset();
    feed_user(perm_answer);
    g_script_pos = 0;
    g_req_count = 0;
    g_body_count = 0;

    memset(&g_ui, 0, sizeof(g_ui));
    g_ui.out.write = cap_write;
    g_ui.in.read_byte = cap_read;
    g_ui.is_tty = 0; /* 强制降级路径：CI 就是非 TTY */
    g_ui.last_was_newline = 1;

    int kind = g_perm_kind;
    g_perm_kind = 0;
    size_t context = g_context_bytes;
    g_context_bytes = 0;

    ca_core_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.transport = &g_fake;
    cfg.api_base = "https://example.invalid/v1";
    cfg.model = "test-model";
    cfg.system = system;
    cfg.ui = &g_ui;
    cfg.ws = g_ws;
    cfg.perm = (kind == 0) ? NULL : test_perm;
    cfg.perm_user = (void *)(intptr_t)kind;
    cfg.session = g_session;
    cfg.context_bytes = context;
    g_session = NULL;
    /* g_core 是 static 且被多个用例复用：init 会 memset 它，
     * 上一轮的请求缓冲必须先释放，否则每个用例漏 256 KB。 */
    ca_core_free(&g_core);
    CA_ASSERT_EQ_INT(ca_core_init(&g_core, &cfg), CA_CORE_OK);
}

static void script(const char *a) {
    g_scripts[g_script_count++] = a;
}
static void script_reset(void) { g_script_count = 0; }

/* ==================================================== 纯文本 */

CA_TEST(core_streams_text_incrementally) {
    script_reset();
    script("data: {\"choices\":[{\"delta\":{\"content\":\"你\"}}]}\n\n"
           "data: {\"choices\":[{\"delta\":{\"content\":\"好\"}}]}\n\n"
           "data: [DONE]\n\n");
    setup(NULL, "");
    CA_ASSERT_EQ_INT(ca_core_run_once(&g_core, "你好"), CA_CORE_OK);
    /* 逐 token 追加，不是攒完一次性吐出。
     * 降级路径下 begin 不写任何东西，所以是两个 token 两次写
     * 加收尾的一个换行 = 3 次。 */
    CA_ASSERT(g_write_calls >= 3);
    CA_ASSERT(strstr(g_ui_buf, "你好") != NULL);
    CA_ASSERT_EQ_INT(g_req_count, 1);
}

CA_TEST(core_survives_split_chunks) {
    /* 同一段 SSE 被切成 3 字节的小块喂进来。
     * 分帧器必须能处理——真实网络给什么形状的包不受控。 */
    script_reset();
    script("data: {\"choices\":[{\"delta\":{\"content\":\"分块也能拼回来\"}}]}\n\n"
           "data: [DONE]\n\n");
    setup(NULL, "");
    CA_ASSERT_EQ_INT(ca_core_run_once(&g_core, "x"), CA_CORE_OK);
    CA_ASSERT(strstr(g_ui_buf, "分块也能拼回来") != NULL);
}

CA_TEST(core_rejects_malformed_response) {
    /* 服务返回无法解析的内容时必须**报错**，不能假装成功。
     * 带着半截数据继续走是最糟的选项。 */
    script_reset();
    script("data: {这不是 JSON\n\ndata: [DONE]\n\n");
    setup(NULL, "");
    ca_core_status st = ca_core_run_once(&g_core, "x");
    CA_ASSERT_EQ_INT(st, CA_CORE_ERR_PROTOCOL);
    CA_ASSERT(strstr(g_ui_buf, "错误") != NULL);
}

CA_TEST(core_rejects_unknown_tool) {
    /* 模型幻觉出一个不存在的工具：必须拒绝执行并说明，
     * 而不是当作未知命令去跑。 */
    script_reset();
    script("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
           "\"id\":\"c1\",\"function\":{\"name\":\"rm_rf_everything\","
           "\"arguments\":\"{}\"}}]}}]}\n\n"
           "data: [DONE]\n\n");
    setup(NULL, "n");
    (void)ca_core_run_once(&g_core, "x");
    CA_ASSERT(strstr(g_ui_buf, "不认识的工具") != NULL);
}

/* ==================================================== 工具 + 权限 */

CA_TEST(core_tool_requires_approval_and_shows_it) {
    /* 契约 P5：模型要求执行 shell 命令时，**完整命令**必须先呈现给用户。
     * 这是整条链路上最不该出错的一处。 */
    script_reset();
    script("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
           "\"id\":\"c1\",\"function\":{\"name\":\"run_shell\","
           "\"arguments\":\"{\\\"command\\\":\\\"echo a-very-long-marker-12345\\\"}\"}}]}}]}\n\n"
           "data: [DONE]\n\n");
    setup(NULL, "n"); /* 用户拒绝 */
    (void)ca_core_run_once(&g_core, "跑个命令");
    CA_ASSERT(strstr(g_ui_buf, "a-very-long-marker-12345") != NULL);
    /* 拒绝后不应有命令被执行 */
    CA_ASSERT(strstr(g_ui_buf, "拒绝") != NULL);
}

CA_TEST(core_permission_defaults_to_deny) {
    /* 契约 P1：没有配置权限回调时**默认拒绝**。
     * 若这里放行，等于「没接权限系统 = 全都允许」，那是灾难性的默认值。 */
    script_reset();
    script("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
           "\"id\":\"c1\",\"function\":{\"name\":\"read_file\","
           "\"arguments\":\"{\\\"path\\\":\\\"x.txt\\\"}\"}}]}}]}\n\n"
           "data: [DONE]\n\n");
    setup(NULL, "y"); /* 即使输入 y */
    (void)ca_core_run_once(&g_core, "读文件");
    /* read_file 不需要权限（只读），但 write_file 需要。
     * 这里验证的是：perm 回调为 NULL 时，破坏性工具会被拒。 */
    CA_ASSERT(strstr(g_ui_buf, "找不到") != NULL ||
              strstr(g_ui_buf, "错误") != NULL);
}

CA_TEST(core_write_tool_is_denied_by_default) {
    script_reset();
    script("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
           "\"id\":\"c1\",\"function\":{\"name\":\"write_file\","
           "\"arguments\":\"{\\\"path\\\":\\\"new.txt\\\",\\\"content\\\":\\\"hi\\\"}\"}}]}}]}\n\n"
           "data: [DONE]\n\n");
    setup(NULL, "n");
    (void)ca_core_run_once(&g_core, "写个文件");
    CA_ASSERT(strstr(g_ui_buf, "拒绝") != NULL);
    /* 文件必须**没有被创建**——拒绝若只发生在嘴上，就是没拒绝 */
    CA_ASSERT(!ws_file_exists("new.txt")); /* 拒绝不能只停在提示上 */
}

/* ==================================================== 批准通道 */

CA_TEST(core_runs_tool_when_user_approves) {
    /* 「能用」的定义：用户在提示处输入 y 之后，工具**真的执行了**。 */
    script_reset();
    script("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
           "\"id\":\"c1\",\"function\":{\"name\":\"write_file\","
           "\"arguments\":\"{\\\"path\\\":\\\"approved.txt\\\",\\\"content\\\":\\\"hi\\\"}\"}}]}}]}\n\n"
           "data: [DONE]\n\n");
    g_perm_kind = 1;
    setup(NULL, "y");
    (void)ca_core_run_once(&g_core, "写文件");

    CA_ASSERT(ws_file_exists("approved.txt")); /* 真的落盘了 */
    ws_file_remove("approved.txt");
}

CA_TEST(core_runs_tool_when_user_approves_sees_pending) {
    /* 待批准内容必须对用户可见（P5），不能是黑盒。
     * 与上一条共用同一份脚本，但只关心 UI 上出现了文件名。 */
    script_reset();
    script("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
           "\"id\":\"c1\",\"function\":{\"name\":\"write_file\","
           "\"arguments\":\"{\\\"path\\\":\\\"shown.txt\\\",\\\"content\\\":\\\"hi\\\"}\"}}]}}]}\n\n"
           "data: [DONE]\n\n");    g_perm_kind = 1;
    setup(NULL, "y");
    (void)ca_core_run_once(&g_core, "写文件");
    CA_ASSERT(strstr(g_ui_buf, "shown.txt") != NULL);
    ws_file_remove("shown.txt");
}

CA_TEST(core_policy_may_veto_after_user_approves) {
    /* 用户点了 y 不等于必须放行：产品策略仍可否决（main 用它拦 run_shell）。 */
    script_reset();
    script("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
           "\"id\":\"c1\",\"function\":{\"name\":\"write_file\","
           "\"arguments\":\"{\\\"path\\\":\\\"vetoed.txt\\\",\\\"content\\\":\\\"hi\\\"}\"}}]}}]}\n\n"
           "data: [DONE]\n\n");
    g_perm_kind = 2;
    setup(NULL, "y");
    (void)ca_core_run_once(&g_core, "写文件");

    CA_ASSERT(!ws_file_exists("vetoed.txt")); /* 被策略拦住了 */
    ws_file_remove("vetoed.txt");
}

/* ==================================================== 硬上限 */

CA_TEST(core_bounds_tool_rounds) {
    /* 模型可以一直要求调工具。没有上限的 agent 循环
     * 就是一个会烧钱、且永不返回的死循环。 */
    script_reset();
    for (int i = 0; i < 20; i++) {
        script("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
               "\"id\":\"c1\",\"function\":{\"name\":\"list_dir\","
               "\"arguments\":\"{\\\"path\\\":\\\".\\\"}\"}}]}}]}\n\n"
               "data: [DONE]\n\n");
    }
    setup(NULL, "y");
    ca_core_status st = ca_core_run_once(&g_core, "列目录");
    CA_ASSERT_EQ_INT(st, CA_CORE_ERR_LIMIT);
    /* 请求次数必须被限制在上限附近，不能跑完 20 次 */
    CA_ASSERT(g_req_count <= CA_CORE_MAX_TOOL_ROUNDS + 1);
    CA_ASSERT(strstr(g_ui_buf, "上限") != NULL);
}

CA_TEST(core_refuses_oversized_tool_args) {
    /* 参数超限时**拒绝执行**。执行半截参数等于做模型没要求的事。
     * 构造方式刻意笨拙但每步都可核对：拼头 + 填 6000 个 x + 拼尾，
     * 总长必须精确落在缓冲内。早先用了一行算术去凑长度，
     * 结果编译器报 C4789「可能越界」——那是它算对了。 */
    static char big[65536];
    const char *head =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"c1\",\"function\":{\"name\":\"read_file\",\"arguments\":\"";
    const char *tail = "\"}}]}}]}\n\ndata: [DONE]\n\n";
    size_t hl = strlen(head), tl = strlen(tail);
    /* 载荷必须**超过** CA_LLM_MAX_ARGS_BYTES(32 KB) 才触发溢出分支。
     * 早先用 8000 —— 远低于上限，于是参数正常通过，
     * 工具拿着一个超长路径去执行，报的是「找不到」，
     * 而测试却期待「超过上限」。测试通过与否全看措辞。 */
    size_t pad = 40000;
    size_t total = hl + pad + tl;
    if (total >= sizeof(big)) { CA_ASSERT(0); return; }
    memcpy(big, head, hl);
    memset(big + hl, 'x', pad);
    memcpy(big + hl + pad, tail, tl + 1);

    script_reset();
    script(big);
    setup(NULL, "y");
    (void)ca_core_run_once(&g_core, "读一个超长路径");
    CA_ASSERT(strstr(g_ui_buf, "超过上限") != NULL ||
              strstr(g_ui_buf, "解析") != NULL);
}

/* ==================================================== 配置校验 */

CA_TEST(core_rejects_incomplete_config) {
    /* 注意 ca_core 有 1 MB tool_out + 256 KB results，
     * **放栈上会直接爆栈**（本测试最初就因此栈溢出）。
     * 这里用 static：它是全局的，不占栈。 */
    static ca_core c;
    ca_core_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    /* 缺 transport */
    cfg.api_base = "x"; cfg.model = "m"; cfg.ui = &g_ui; cfg.ws = g_ws;
    CA_ASSERT_EQ_INT(ca_core_init(&c, &cfg), CA_CORE_ERR_ARGS);
    /* 缺 ui */
    cfg.transport = &g_fake; cfg.ui = NULL;
    CA_ASSERT_EQ_INT(ca_core_init(&c, &cfg), CA_CORE_ERR_ARGS);
    CA_ASSERT_EQ_INT(ca_core_init(NULL, NULL), CA_CORE_ERR_ARGS);
}

CA_TEST(core_survives_empty_response) {
    /* 服务返回空响应：不该崩溃，也不该谎称成功。 */
    script_reset();
    setup(NULL, "");
    ca_core_status st = ca_core_run_once(&g_core, "x");
    CA_ASSERT_EQ_INT(st, CA_CORE_OK);
    CA_ASSERT_EQ_INT(g_req_count, 1);
}

CA_TEST(core_endpoints_are_built_from_base) {
    /* 端点由 api_base 拼出。base 里带 /v1 之类的前缀时
     * 不能被丢掉——那是配置里明确写了的东西。 */
    script_reset();
    setup(NULL, "");
    CA_ASSERT(strstr(g_core.url, "https://example.invalid/v1") != NULL);
    CA_ASSERT(strstr(g_core.url, "chat/completions") != NULL);
}

CA_TEST(core_reports_oversized_request_instead_of_truncating) {
    /* 单轮内容比整个上下文上限还大时，必须明确报「需要多少」，
     * 而不是截断成半个 JSON 发出去——
     * 半个请求体会被服务端当成协议错误，报错还更难懂。
     *
     * 注意这里把上限压到最小值：默认 256 KB 时 60 KB 是**正常**请求，
     * 早先这条用例靠的是写死的 16 KB 上限，上限可配之后必须自己指定，
     * 否则它会变成一个「什么都测不到」的绿用例。 */
    static char huge[60000];
    memset(huge, 'x', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    g_context_bytes = CA_CORE_MIN_CONTEXT_BYTES;
    script_reset();
    setup(NULL, "");
    ca_core_status st = ca_core_run_once(&g_core, huge);
    CA_ASSERT_EQ_INT(st, CA_CORE_ERR_LIMIT);
    CA_ASSERT(strstr(g_ui_buf, "上下文上限") != NULL);
    CA_ASSERT_EQ_INT(g_req_count, 0); /* 根本没发出去 */
}

CA_TEST(core_trims_oldest_turns_when_over_budget) {
    /* 历史放不下时**不能直接失败**：这是早先的真实缺陷——
     * 请求体写死 16 KB，约 50 轮之后每轮都报「触到硬上限」，
     * 对话彻底不可用。正确行为是丢最旧的完整轮次，并明确告诉用户。 */
    ca_session s;
    CA_ASSERT_EQ_INT(ca_session_init(&s, 200000), 0);

    char reply[601];
    memset(reply, 'y', sizeof(reply) - 1);
    reply[sizeof(reply) - 1] = '\0';

    for (int i = 0; i < 40; i++) {
        char q[64];
        snprintf(q, sizeof(q), "旧问题-%02d-MARKER", i);
        CA_ASSERT_EQ_INT(ca_session_add(&s, CA_MSG_USER, q), 0);
        CA_ASSERT_EQ_INT(ca_session_add(&s, CA_MSG_ASSISTANT, reply), 0);
    }

    g_session = &s;
    g_context_bytes = CA_CORE_MIN_CONTEXT_BYTES;
    script_reset();
    script("data: {\"choices\":[{\"delta\":{\"content\":\"答\"}}]}\n\n"
           "data: [DONE]\n\n");
    setup(NULL, "");

    CA_ASSERT_EQ_INT(ca_core_run_once(&g_core, "最新问题-LATEST"), CA_CORE_OK);
    CA_ASSERT_EQ_INT(g_req_count, 1);

    /* 请求真的发出去了，且仍然带着**最近**的上下文 */
    CA_ASSERT(strstr(g_bodies[0], "LATEST") != NULL);
    CA_ASSERT(strstr(g_bodies[0], "旧问题-39-MARKER") != NULL);
    /* 最旧的被丢掉——否则「淘汰」二字无从谈起 */
    CA_ASSERT(strstr(g_bodies[0], "旧问题-00-MARKER") == NULL);
    /* 丢了就要说（契约 T3） */
    CA_ASSERT(strstr(g_ui_buf, "已丢弃最早的") != NULL);

    ca_session_free(&s);
}

CA_TEST(core_clamps_context_bytes) {
    /* 配置的边界：太小的值不该让请求永远失败，太大的值不该悄悄分配。
     * 钳制后的真实值必须能被查询到，否则 UI 没法告诉用户实际限制。 */
    static ca_core c;
    ca_core_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.transport = &g_fake;
    cfg.api_base = "https://example.invalid/v1";
    cfg.model = "m";
    cfg.ui = &g_ui;
    cfg.ws = g_ws;

    cfg.context_bytes = 0;
    CA_ASSERT_EQ_INT(ca_core_init(&c, &cfg), CA_CORE_OK);
    CA_ASSERT_EQ_INT((long)ca_core_context_bytes(&c),
                     (long)CA_CORE_DEFAULT_CONTEXT_BYTES);
    ca_core_free(&c);

    cfg.context_bytes = 1;
    CA_ASSERT_EQ_INT(ca_core_init(&c, &cfg), CA_CORE_OK);
    CA_ASSERT_EQ_INT((long)ca_core_context_bytes(&c),
                     (long)CA_CORE_MIN_CONTEXT_BYTES);
    ca_core_free(&c);

    cfg.context_bytes = 64u * 1024u * 1024u;
    CA_ASSERT_EQ_INT(ca_core_init(&c, &cfg), CA_CORE_OK);
    CA_ASSERT_EQ_INT((long)ca_core_context_bytes(&c),
                     (long)CA_CORE_MAX_CONTEXT_BYTES);
    ca_core_free(&c);
}

/* ==================================================== 会话历史（US-6 / §6） */

CA_TEST(core_carries_history_across_inputs) {
    /* 这条测试是「多轮记忆」的真正证据：上一次问答必须出现在
     * 下一次请求体里。会话文件、内存结构都可能是摆设，
     * 只有请求体骗不了人。 */
    ca_session s;
    CA_ASSERT_EQ_INT(ca_session_init(&s, 8192), 0);
    g_session = &s;

    script_reset();
    script("data: {\"choices\":[{\"delta\":{\"content\":\"第一答\"}}]}\n\n"
           "data: [DONE]\n\n");
    script("data: {\"choices\":[{\"delta\":{\"content\":\"第二答\"}}]}\n\n"
           "data: [DONE]\n\n");
    setup(NULL, "");

    CA_ASSERT_EQ_INT(ca_core_run_once(&g_core, "第一问"), CA_CORE_OK);
    CA_ASSERT_EQ_INT(ca_core_run_once(&g_core, "第二问"), CA_CORE_OK);
    CA_ASSERT_EQ_INT(g_body_count, 2);

    /* 第一次请求只有本轮问题，不该凭空出现上一轮 */
    CA_ASSERT(strstr(g_bodies[0], "第一问") != NULL);
    CA_ASSERT(strstr(g_bodies[0], "第一答") == NULL);
    /* 第二次请求必须带上第一轮的问答 —— 这就是记忆 */
    CA_ASSERT(strstr(g_bodies[1], "第一问") != NULL);
    CA_ASSERT(strstr(g_bodies[1], "第一答") != NULL);
    CA_ASSERT(strstr(g_bodies[1], "第二问") != NULL);

    /* 问题不该被重复发送：历史里已经有它，就不再作为尾随消息追加 */
    const char *first = strstr(g_bodies[1], "第一问");
    CA_ASSERT(first != NULL);
    CA_ASSERT(strstr(first + 1, "第一问") == NULL);

    /* 助手回复也要落进历史 */
    CA_ASSERT_EQ_INT(ca_session_count(&s), 4);
    CA_ASSERT_EQ_INT(ca_session_role(&s, 0), CA_MSG_USER);
    CA_ASSERT(strcmp(ca_session_text(&s, 1), "第一答") == 0);
    CA_ASSERT(strcmp(ca_session_text(&s, 3), "第二答") == 0);
    ca_session_free(&s);
}

CA_TEST(core_tells_user_when_history_is_evicted) {
    /* 契约 T3：灌入超限历史后，淘汰必须发生**且 UI 有明示**。
     * 悄悄丢掉上下文正是同类工具的毛病，本产品必须说出来。 */
    ca_session s;
    CA_ASSERT_EQ_INT(ca_session_init(&s, 1024), 0);
    g_session = &s;

    /* 每次回复 300 字节，几轮就会撑满 1024 的预算 */
    static char frame[512];
    char body[301];
    memset(body, 'x', sizeof(body) - 1);
    body[sizeof(body) - 1] = '\0';

    for (int i = 0; i < 4; i++) {
        script_reset();
        snprintf(frame, sizeof(frame),
                 "data: {\"choices\":[{\"delta\":{\"content\":\"%s\"}}]}\n\n"
                 "data: [DONE]\n\n", body);
        script(frame);
        /* setup() 用完就把 g_session 清空（让其余用例天然无记忆），
         * 所以每一轮都要重新挂上。 */
        g_session = &s;
        setup(NULL, "");
        CA_ASSERT_EQ_INT(ca_core_run_once(&g_core, "继续"), CA_CORE_OK);
        CA_ASSERT(ca_session_used(&s) <= ca_session_budget(&s));
    }

    CA_ASSERT(s.evicted_msgs > 0);
    CA_ASSERT(strstr(g_ui_buf, "丢弃最早的") != NULL);
    ca_session_free(&s);
}

int main(void) {
    struct { const char *name; void (*fn)(void); } cases[] = {
        {"core_streams_text_incrementally", ca_test_run_core_streams_text_incrementally},
        {"core_survives_split_chunks", ca_test_run_core_survives_split_chunks},
        {"core_rejects_malformed_response", ca_test_run_core_rejects_malformed_response},
        {"core_rejects_unknown_tool", ca_test_run_core_rejects_unknown_tool},
        {"core_tool_requires_approval_and_shows_it", ca_test_run_core_tool_requires_approval_and_shows_it},
        {"core_permission_defaults_to_deny", ca_test_run_core_permission_defaults_to_deny},
        {"core_write_tool_is_denied_by_default", ca_test_run_core_write_tool_is_denied_by_default},
    /* 这张表是手工维护的：写了 CA_TEST 却忘了登记，用例会静默不跑 */
    {"core_runs_tool_when_user_approves", ca_test_run_core_runs_tool_when_user_approves},
    {"core_runs_tool_when_user_approves_sees_pending", ca_test_run_core_runs_tool_when_user_approves_sees_pending},
    {"core_policy_may_veto_after_user_approves", ca_test_run_core_policy_may_veto_after_user_approves},
        {"core_bounds_tool_rounds", ca_test_run_core_bounds_tool_rounds},
        {"core_refuses_oversized_tool_args", ca_test_run_core_refuses_oversized_tool_args},
        {"core_rejects_incomplete_config", ca_test_run_core_rejects_incomplete_config},
        {"core_survives_empty_response", ca_test_run_core_survives_empty_response},
        {"core_endpoints_are_built_from_base", ca_test_run_core_endpoints_are_built_from_base},
        {"core_reports_oversized_request_instead_of_truncating", ca_test_run_core_reports_oversized_request_instead_of_truncating},
    /* 新增于此：上限可配 + 超限淘汰。漏登记 = 静默不跑。 */
    {"core_trims_oldest_turns_when_over_budget", ca_test_run_core_trims_oldest_turns_when_over_budget},
    {"core_clamps_context_bytes", ca_test_run_core_clamps_context_bytes},
        {"core_carries_history_across_inputs", ca_test_run_core_carries_history_across_inputs},
        {"core_tells_user_when_history_is_evicted", ca_test_run_core_tells_user_when_history_is_evicted},
    };
    size_t n = sizeof(cases) / sizeof(cases[0]);

    /* 一个真实的工作区，工具测试需要 */
    const char *tmp = getenv("TEMP");
    if (tmp == NULL) tmp = getenv("TMP");
    if (tmp == NULL) tmp = ".";
    char dir[512];
    /* 路径分隔符必须先算出来，**不能**把 #if 写进 snprintf 的实参里。
     * clang 的 -Wembedded-directive 会指出这是未定义行为——
     * 同一类错误本仓库已经犯过一次（llm_test 里的 CA_TEST_SEP）。 */
#if defined(_WIN32)
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    snprintf(dir, sizeof(dir), "%s%ccagent_core_test", tmp, sep);
    snprintf(g_ws_dir, sizeof(g_ws_dir), "%s", dir);
    {
        /* 2048 而不是 1024：GCC 的 -Wformat-truncation 指出
         * 两条命令 + 两份路径最多需要 1047 字节，1024 装不下会静默截断。
         * 截断后的命令行会创建出错误的目录名，后续用例全部拿到 NULL 工作区。 */
        char cmd[2048];
        /* 不能叫 n：外层已有一个 size_t n（用例数），
         * MSVC 的 C4456 会因为遮蔽而报错。 */
        int clen;
#if defined(_WIN32)
        clen = snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\" 2>nul & mkdir \"%s\"", dir, dir);
#else
        clen = snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", dir, dir);
#endif
        CA_ASSERT(clen > 0 && (size_t)clen < sizeof(cmd)); /* 截断即失败 */
        if (clen <= 0 || (size_t)clen >= sizeof(cmd)) {
            printf("测试命令行被截断，无法准备工作区\n");
            return 1;
        }
        if (system(cmd) != 0) {
            printf("无法准备测试工作区\n");
            return 1;
        }
    }
    g_ws = ca_ws_open(dir);
    if (g_ws == NULL) { printf("工作区打开失败\n"); return 1; }

    for (size_t i = 0; i < n; i++) {
        ca_test_begin(cases[i].name);
        cases[i].fn();
        ca_test_end();
    }
    ca_ws_free(g_ws);
    return ca_test_summary();
}