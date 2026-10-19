/* llm_test.c —— 服务适配层测试
 * 契约：docs/design/contract.md §4.3 / §10 T1
 *
 * 用例全部基于**真实的服务端流形状**，包括那些容易被忽略的：
 *   - 工具参数分多片到达
 *   - 部分帧没有 index（按数组位置归并）
 *   - usage 出现在最后一帧
 *   - 参数超限（必须报错而非截断）
 *   - [DONE] 之后还有内容（必须忽略）
 */

#include "ca_test.h"

#include "cagent/json.h"
#include "cagent/llm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 尾部字面量 ""}}]}}]}" 共 7 字节，加结尾 NUL = 8。
 * 必须是**编译期常量**：写成运行期变量后 GCC 无法常量传播，
 * -Wformat-truncation 会误判成"输出被截断"。 */
#define LLM_TAIL_BYTES 8

typedef struct {
    int texts;
    int begins;
    int arg_frags;
    int ends;
    int usages;
    int errors;
    int dones;
    char last_text[256];
    char tool_name[64];
    char tool_id[64];
    long long prompt_tokens;
    long long completion_tokens;
} rec;

static void on_ev(const ca_event *ev, void *user) {
    rec *r = (rec *)user;
    switch (ev->kind) {
        case CA_EV_TEXT_DELTA:
            r->texts++;
            if (ev->text != NULL) {
                size_t n = strlen(ev->text);
                if (n > sizeof(r->last_text) - 1) n = sizeof(r->last_text) - 1;
                memcpy(r->last_text, ev->text, n);
                r->last_text[n] = '\0';
            }
            break;
        case CA_EV_TOOL_CALL_BEGIN:
            r->begins++;
            if (ev->tool_name) memcpy(r->tool_name, ev->tool_name, sizeof(r->tool_name) - 1);
            if (ev->tool_id) memcpy(r->tool_id, ev->tool_id, sizeof(r->tool_id) - 1);
            break;
        case CA_EV_TOOL_CALL_ARGS:
            r->arg_frags++;
            break;
        case CA_EV_TOOL_CALL_END:
            r->ends++;
            break;
        case CA_EV_USAGE:
            r->usages++;
            r->prompt_tokens = ev->prompt_tokens;
            r->completion_tokens = ev->completion_tokens;
            break;
        case CA_EV_ERROR:
            r->errors++;
            break;
        case CA_EV_DONE:
            r->dones++;
            break;
    }
}

static void feed(ca_llm *llm, rec *r, const char *json) {
    ca_llm_on_frame(llm, json, strlen(json), on_ev, r);
}

/* ------------------------------------------------------------ 文本增量 */

CA_TEST(llm_parses_text_deltas) {
    ca_llm *llm = ca_llm_create();
    CA_ASSERT(llm != NULL);
    rec r;
    memset(&r, 0, sizeof(r));

    feed(llm, &r, "{\"choices\":[{\"index\":0,\"delta\":{\"content\":\"你\"}}]}");
    feed(llm, &r, "{\"choices\":[{\"index\":0,\"delta\":{\"content\":\"好\"}}]}");
    feed(llm, &r, "[DONE]");

    CA_ASSERT_EQ_INT(r.texts, 2);
    CA_ASSERT(strcmp(r.last_text, "好") == 0);
    CA_ASSERT_EQ_INT(r.dones, 1);
    ca_llm_free(llm);
}

CA_TEST(llm_handles_unicode_content) {
    ca_llm *llm = ca_llm_create();
    rec r;
    memset(&r, 0, sizeof(r));
    /* 中文在 JSON 里通常不转义，但服务可能转义，两种都要过 */
    feed(llm, &r, "{\"choices\":[{\"delta\":{\"content\":\"中文\"}}]}");
    CA_ASSERT_EQ_INT(r.texts, 1);
    CA_ASSERT(strcmp(r.last_text, "中文") == 0);
    ca_llm_free(llm);
}

/* ------------------------------------------------------------ 工具调用 */

CA_TEST(llm_assembles_tool_call_arguments_from_fragments) {
    /* 参数分三片到达，必须正确拼接。
     * 历史上「拼接错位」这类 bug 在接口测试里看不出来，
     * 因为接口测试不测分片。 */
    ca_llm *llm = ca_llm_create();
    rec r;
    memset(&r, 0, sizeof(r));

    feed(llm, &r,
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
         "\"id\":\"call_a\",\"function\":{\"name\":\"read_file\","
         "\"arguments\":\"{\\\"pa\"}}]}}]}");
    feed(llm, &r,
         "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
         "\"function\":{\"arguments\":\"th\\\":\\\"a.c\\\"}\"}}]}}]}");
    feed(llm, &r, "[DONE]");

    CA_ASSERT_EQ_INT(r.begins, 1);
    CA_ASSERT(strcmp(r.tool_name, "read_file") == 0);
    CA_ASSERT(strcmp(r.tool_id, "call_a") == 0);
    CA_ASSERT_EQ_INT(r.ends, 1);

    size_t len = 0;
    int overflow = 0;
    const char *args = ca_llm_tool_args(llm, 0, &len, &overflow);
    CA_ASSERT(args != NULL);
    CA_ASSERT(strcmp(args, "{\"path\":\"a.c\"}") == 0);
    /* 14 字节：{ " p a t h " : " a . c " } */
    CA_ASSERT_EQ_INT((long long)len, 14);
    CA_ASSERT_EQ_INT(overflow, 0);
    ca_llm_free(llm);
}

CA_TEST(llm_tool_call_without_explicit_index) {
    /* 部分服务不返回 index，应按数组位置归并 */
    ca_llm *llm = ca_llm_create();
    rec r;
    memset(&r, 0, sizeof(r));
    feed(llm, &r,
         "{\"choices\":[{\"delta\":{\"tool_calls\":["
         "{\"id\":\"c1\",\"function\":{\"name\":\"list_dir\",\"arguments\":\"{}\"}}]}}]}");
    feed(llm, &r, "[DONE]");
    CA_ASSERT_EQ_INT(r.begins, 1);
    CA_ASSERT(strcmp(r.tool_name, "list_dir") == 0);
    size_t len = 0;
    int ov = 0;
    const char *a = ca_llm_tool_args(llm, 0, &len, &ov);
    CA_ASSERT(a != NULL && strcmp(a, "{}") == 0);
    ca_llm_free(llm);
}

CA_TEST(llm_handles_multiple_concurrent_tool_calls) {
    ca_llm *llm = ca_llm_create();
    rec r;
    memset(&r, 0, sizeof(r));
    feed(llm, &r,
         "{\"choices\":[{\"delta\":{\"tool_calls\":["
         "{\"index\":0,\"id\":\"a\",\"function\":{\"name\":\"read_file\",\"arguments\":\"{}\"}},"
         "{\"index\":1,\"id\":\"b\",\"function\":{\"name\":\"list_dir\",\"arguments\":\"{}\"}}"
         "]}}]}");
    feed(llm, &r, "[DONE]");
    CA_ASSERT_EQ_INT(r.begins, 2);
    CA_ASSERT_EQ_INT(r.ends, 2);
    const char *a0 = ca_llm_tool_args(llm, 0, NULL, NULL);
    const char *a1 = ca_llm_tool_args(llm, 1, NULL, NULL);
    CA_ASSERT(a0 != NULL && a1 != NULL);
    CA_ASSERT(a0 != a1);
    ca_llm_free(llm);
}

CA_TEST(llm_rejects_tool_calls_beyond_slot_limit) {
    /* 超过槽位上限必须报错，**不能**只取前 8 个然后假装没事——
     * 那样会执行错误的工具。 */
    ca_llm *llm = ca_llm_create();
    rec r;
    memset(&r, 0, sizeof(r));
    char frame[8192];
    int n = snprintf(frame, sizeof(frame), "{\"choices\":[{\"delta\":{\"tool_calls\":[");
    for (int i = 0; i < CA_LLM_MAX_TOOL_CALLS + 3; i++) {
        n += snprintf(frame + n, sizeof(frame) - (size_t)n,
                      "%s{\"index\":%d,\"id\":\"c%d\",\"function\":"
                      "{\"name\":\"list_dir\",\"arguments\":\"{}\"}}",
                      i ? "," : "", i, i);
    }
    n += snprintf(frame + n, sizeof(frame) - (size_t)n, "]}}]}");

    int rc = ca_llm_on_frame(llm, frame, (size_t)n, on_ev, &r);
    CA_ASSERT_EQ_INT(rc, -2);
    ca_llm_free(llm);
}

CA_TEST(llm_accepts_large_single_argument_fragment) {
    /* 单片参数就超过 1 KB —— 一次 read_file 的参数很容易到这个量级。
     * 早先经由 1024 字节临时缓冲读取，会把它误判成超限并拒绝，
     * 那会把本来合法的调用拒掉。 */
    ca_llm *llm = ca_llm_create();
    rec r;
    memset(&r, 0, sizeof(r));
    size_t big_len = 20000;
    /* 缓冲区必须按**实际写入量**算，不能拍脑袋给 512。
     * 早先 malloc(big_len + 512)，却从 payload+at（≈ 20115）起再写 512 字节，
     * 末端到 20627，而分配只有 20512 —— 真实越界。
     * GCC 报 `specified bound 512 exceeds destination size 412` 不是误报。
     * 改为：先在小缓冲区里量出前缀长度，再按前缀实际长度精确分配。 */
    char head[160];
    int hn = snprintf(head, sizeof(head),
                      "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                      "\"id\":\"c\",\"function\":{\"name\":\"read_file\",\"arguments\":\"");
    CA_ASSERT(hn > 0 && (size_t)hn < sizeof(head));
    if (hn <= 0 || (size_t)hn >= sizeof(head)) return;
    const size_t tail = LLM_TAIL_BYTES; /* "\"}}]}}]}" 加结尾 NUL */
    char *payload = (char *)malloc((size_t)hn + big_len + tail);
    CA_ASSERT(payload != NULL);
    if (payload == NULL) return;
    memcpy(payload, head, (size_t)hn);
    size_t at = (size_t)hn;
    for (size_t i = 0; i < big_len; i++) payload[at + i] = 'x';
    at += big_len;
    int tn = snprintf(payload + at, LLM_TAIL_BYTES, "\"}}]}}]}");
    int rc = ca_llm_on_frame(llm, payload, at + (size_t)tn, on_ev, &r);
    CA_ASSERT_EQ_INT(rc, 0);
    size_t got = 0;
    int ov = 0;
    const char *a = ca_llm_tool_args(llm, 0, &got, &ov);
    CA_ASSERT(a != NULL);
    CA_ASSERT_EQ_INT((long long)got, (long long)big_len);
    CA_ASSERT_EQ_INT(ov, 0);
    free(payload);
    ca_llm_free(llm);
}

CA_TEST(llm_marks_argument_overflow_without_truncating) {
    /* 超过 32 KB 上限：必须标记 overflow，执行前必须拒绝。
     * 绝不截断——半截 JSON 会被当成合法参数执行，那非常危险。 */
    ca_llm *llm = ca_llm_create();
    rec r;
    memset(&r, 0, sizeof(r));
    size_t huge = CA_LLM_MAX_ARGS_BYTES + 5000;
    char head[160];
    int hn = snprintf(head, sizeof(head),
                      "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                      "\"id\":\"c\",\"function\":{\"name\":\"read_file\",\"arguments\":\"");
    CA_ASSERT(hn > 0 && (size_t)hn < sizeof(head));
    if (hn <= 0 || (size_t)hn >= sizeof(head)) return;
    const size_t tail = LLM_TAIL_BYTES;
    char *payload = (char *)malloc((size_t)hn + huge + tail);
    CA_ASSERT(payload != NULL);
    if (payload == NULL) return;
    memcpy(payload, head, (size_t)hn);
    size_t at = (size_t)hn;
    for (size_t i = 0; i < huge; i++) payload[at + i] = 'x';
    at += huge;
    int tn = snprintf(payload + at, LLM_TAIL_BYTES, "\"}}]}}]}");
    (void)ca_llm_on_frame(llm, payload, at + (size_t)tn, on_ev, &r);

    size_t got = 0;
    int ov = 1;
    const char *a = ca_llm_tool_args(llm, 0, &got, &ov);
    CA_ASSERT(a != NULL);
    /* 关键：溢出标记必须为真，否则半截参数会被当合法参数执行 */
    CA_ASSERT_EQ_INT(ov, 1);
    /* 已写入的长度不得超过上限 */
    CA_ASSERT(got <= CA_LLM_MAX_ARGS_BYTES);
    free(payload);
    ca_llm_free(llm);
}

/* ------------------------------------------------------------ 其它事件 */

CA_TEST(llm_reads_usage) {
    ca_llm *llm = ca_llm_create();
    rec r;
    memset(&r, 0, sizeof(r));
    feed(llm, &r,
         "{\"choices\":[{\"delta\":{}}],"
         "\"usage\":{\"prompt_tokens\":123,\"completion_tokens\":45}}");
    CA_ASSERT_EQ_INT(r.usages, 1);
    CA_ASSERT_EQ_INT(r.prompt_tokens, 123);
    CA_ASSERT_EQ_INT(r.completion_tokens, 45);
    ca_llm_free(llm);
}

CA_TEST(llm_reports_server_error) {
    ca_llm *llm = ca_llm_create();
    rec r;
    memset(&r, 0, sizeof(r));
    feed(llm, &r, "{\"error\":{\"message\":\"rate limit exceeded\"}}");
    CA_ASSERT_EQ_INT(r.errors, 1);
    ca_llm_free(llm);
}

CA_TEST(llm_ignores_content_after_done) {
    ca_llm *llm = ca_llm_create();
    rec r;
    memset(&r, 0, sizeof(r));
    feed(llm, &r, "{\"choices\":[{\"delta\":{\"content\":\"a\"}}]}");
    feed(llm, &r, "[DONE]");
    CA_ASSERT_EQ_INT(r.dones, 1);
    /* DONE 之后再来内容必须被忽略，否则界面会显示重复内容 */
    feed(llm, &r, "{\"choices\":[{\"delta\":{\"content\":\"b\"}}]}");
    CA_ASSERT_EQ_INT(r.texts, 1);
    CA_ASSERT_EQ_INT(r.dones, 1);
    ca_llm_free(llm);
}

CA_TEST(llm_tolerates_malformed_frames) {
    ca_llm *llm = ca_llm_create();
    rec r;
    memset(&r, 0, sizeof(r));
    const char *bad[] = {"", "{", "{}", "not json", "{\"choices\":", "[]",
                         "{\"choices\":[{\"delta\":{\"content\":}}]}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        (void)ca_llm_on_frame(llm, bad[i], strlen(bad[i]), on_ev, &r);
    }
    /* 畸形输入不得崩溃，也不得产生文本/工具事件 */
    CA_ASSERT_EQ_INT(r.texts, 0);
    CA_ASSERT_EQ_INT(r.begins, 0);
    ca_llm_free(llm);
}

/* ------------------------------------------------------------ 请求构造 */

CA_TEST(llm_builds_request_with_escaping) {
    char buf[1024];
    ca_llm_request req;
    memset(&req, 0, sizeof(req));
    req.api_base = "https://api.openai.com/v1";
    req.model = "gpt-4";
    req.system = "你是助手";
    req.user = "他说\"你好\"\n换行";
    size_t need = 0;
    int rc = ca_llm_build_request(buf, sizeof(buf), &req, &need);
    CA_ASSERT_EQ_INT(rc, 0);

    /* 产物必须是合法 JSON */
    ca_json_cursor root;
    ca_json_init(&root, buf, strlen(buf));
    CA_ASSERT(ca_json_enter_object(&root) == 0);
    CA_ASSERT(strstr(buf, "\"stream\":true") != NULL);
    CA_ASSERT(strstr(buf, "\\\"你好\\\"") != NULL);
    CA_ASSERT(strstr(buf, "\\n") != NULL);
}

CA_TEST(llm_request_rejects_small_buffer_without_truncating) {
    char tiny[16];
    memset(tiny, 0x7F, sizeof(tiny));
    ca_llm_request req;
    memset(&req, 0, sizeof(req));
    req.api_base = "https://x/v1";
    req.model = "gpt-4";
    req.user = "hello";
    size_t need = 0;
    CA_ASSERT(ca_llm_build_request(tiny, sizeof(tiny), &req, &need) == -2);
    /* 缓冲必须保持原样 */
    for (size_t i = 0; i < sizeof(tiny); i++) {
        CA_ASSERT_EQ_INT((long long)(unsigned char)tiny[i], 0x7F);
    }
}

CA_TEST(llm_request_requires_mandatory_fields) {
    char buf[1024];
    ca_llm_request req;
    memset(&req, 0, sizeof(req));
    req.api_base = "https://x/v1";
    req.user = "hi";
    CA_ASSERT(ca_llm_build_request(buf, sizeof(buf), &req, NULL) == -1); /* 缺 model */
    memset(&req, 0, sizeof(req));
    req.model = "m";
    req.user = "hi";
    CA_ASSERT(ca_llm_build_request(buf, sizeof(buf), &req, NULL) == -1); /* 缺 api_base */
}

int main(void) {
    struct { const char *name; void (*fn)(void); } cases[] = {
        {"llm_parses_text_deltas", ca_test_run_llm_parses_text_deltas},
        {"llm_handles_unicode_content", ca_test_run_llm_handles_unicode_content},
        {"llm_assembles_tool_call_arguments_from_fragments", ca_test_run_llm_assembles_tool_call_arguments_from_fragments},
        {"llm_tool_call_without_explicit_index", ca_test_run_llm_tool_call_without_explicit_index},
        {"llm_handles_multiple_concurrent_tool_calls", ca_test_run_llm_handles_multiple_concurrent_tool_calls},
        {"llm_rejects_tool_calls_beyond_slot_limit", ca_test_run_llm_rejects_tool_calls_beyond_slot_limit},
        {"llm_accepts_large_single_argument_fragment", ca_test_run_llm_accepts_large_single_argument_fragment},
        {"llm_marks_argument_overflow_without_truncating", ca_test_run_llm_marks_argument_overflow_without_truncating},
        {"llm_reads_usage", ca_test_run_llm_reads_usage},
        {"llm_reports_server_error", ca_test_run_llm_reports_server_error},
        {"llm_ignores_content_after_done", ca_test_run_llm_ignores_content_after_done},
        {"llm_tolerates_malformed_frames", ca_test_run_llm_tolerates_malformed_frames},
        {"llm_builds_request_with_escaping", ca_test_run_llm_builds_request_with_escaping},
        {"llm_request_rejects_small_buffer_without_truncating", ca_test_run_llm_request_rejects_small_buffer_without_truncating},
        {"llm_request_requires_mandatory_fields", ca_test_run_llm_request_requires_mandatory_fields},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ca_test_begin(cases[i].name);
        cases[i].fn();
        ca_test_end();
    }
    return ca_test_summary();
}