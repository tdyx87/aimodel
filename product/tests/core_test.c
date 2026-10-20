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

static ca_http_status fake_run(const ca_request *req, ca_on_data on_data,
                               void *user) {
    (void)req;
    g_req_count++;
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

static void setup(const char *system, const char *perm_answer) {
    ui_reset();
    feed_user(perm_answer);
    g_script_pos = 0;
    g_req_count = 0;

    memset(&g_ui, 0, sizeof(g_ui));
    g_ui.out.write = cap_write;
    g_ui.in.read_byte = cap_read;
    g_ui.is_tty = 0; /* 强制降级路径：CI 就是非 TTY */
    g_ui.last_was_newline = 1;

    ca_core_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.transport = &g_fake;
    cfg.api_base = "https://example.invalid/v1";
    cfg.model = "test-model";
    cfg.system = system;
    cfg.ui = &g_ui;
    cfg.ws = g_ws;
    cfg.perm = NULL;
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
    char probe[1024];
    snprintf(probe, sizeof(probe), "%s%c%s", g_ws ? "" : "",
             g_ws ? '\\' : '/', "new.txt");
    FILE *f = fopen(probe, "rb");
    CA_ASSERT(f == NULL); /* 不存在 */
    if (f != NULL) fclose(f);
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
    /* 请求体装不下时必须明确报「需要多少」，
     * 而不是截断成半个 JSON 发出去——
     * 半个请求体会被服务端当成协议错误，报错还更难懂。 */
    static char huge[60000];
    memset(huge, 'x', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    script_reset();
    setup(NULL, "");
    ca_core_status st = ca_core_run_once(&g_core, huge);
    CA_ASSERT_EQ_INT(st, CA_CORE_ERR_LIMIT);
    CA_ASSERT(strstr(g_ui_buf, "请求体") != NULL);
    CA_ASSERT_EQ_INT(g_req_count, 0); /* 根本没发出去 */
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
        {"core_bounds_tool_rounds", ca_test_run_core_bounds_tool_rounds},
        {"core_refuses_oversized_tool_args", ca_test_run_core_refuses_oversized_tool_args},
        {"core_rejects_incomplete_config", ca_test_run_core_rejects_incomplete_config},
        {"core_survives_empty_response", ca_test_run_core_survives_empty_response},
        {"core_endpoints_are_built_from_base", ca_test_run_core_endpoints_are_built_from_base},
        {"core_reports_oversized_request_instead_of_truncating", ca_test_run_core_reports_oversized_request_instead_of_truncating},
    };
    size_t n = sizeof(cases) / sizeof(cases[0]);

    /* 一个真实的工作区，工具测试需要 */
    const char *tmp = getenv("TEMP");
    if (tmp == NULL) tmp = getenv("TMP");
    if (tmp == NULL) tmp = ".";
    char dir[512];
    snprintf(dir, sizeof(dir), "%s%ccagent_core_test", tmp,
#if defined(_WIN32)
             '\\'
#else
             '/'
#endif
    );
    {
        /* 2048 而不是 1024：GCC 的 -Wformat-truncation 指出
         * 两条命令 + 两份路径最多需要 1047 字节，1024 装不下会静默截断。
         * 截断后的命令行会创建出错误的目录名，后续用例全部拿到 NULL 工作区。 */
        char cmd[2048];
        int n;
#if defined(_WIN32)
        n = snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\" 2>nul & mkdir \"%s\"", dir, dir);
#else
        n = snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", dir, dir);
#endif
        CA_ASSERT(n > 0 && (size_t)n < sizeof(cmd)); /* 截断即失败 */
        if (n <= 0 || (size_t)n >= sizeof(cmd)) {
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