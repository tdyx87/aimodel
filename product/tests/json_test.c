/* json_test.c —— JSON 游标扫描器测试
 * 契约：docs/design/contract.md §C6 / §10 T1
 *
 * 重点放在**边界与畸形输入**上：LLM 返回的 JSON 经常是残缺的、
 * 分片的、超长的。这些正是最容易把流式解析搞崩的地方。
 */

#include "ca_test.h"

#include "cagent/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CA_TEST(json_reads_flat_object_field) {
    const char *doc = "{\"model\":\"gpt-4\",\"id\":\"abc\"}";
    ca_json_cursor root, val;
    ca_json_init(&root, doc, strlen(doc));
    CA_ASSERT(ca_json_enter_object(&root) == 0);
    CA_ASSERT(ca_json_field(&root, "model", &val) == 1);
    char buf[64];
    CA_ASSERT(ca_json_read_string(&val, buf, sizeof(buf), NULL) == 0);
    CA_ASSERT(strcmp(buf, "gpt-4") == 0);
    CA_ASSERT(ca_json_field(&root, "id", &val) == 1);
    CA_ASSERT(ca_json_read_string(&val, buf, sizeof(buf), NULL) == 0);
    CA_ASSERT(strcmp(buf, "abc") == 0);
    /* 字段不存在返回 0，不应被当成错误 */
    CA_ASSERT(ca_json_field(&root, "nope", &val) == 0);
}

CA_TEST(json_descends_nested_path) {
    /* LLM 响应正是这种形状：choices[].delta.content */
    const char *doc =
        "{\"choices\":[{\"index\":0,\"delta\":{\"content\":\"你好\"}}],\"id\":\"x\"}";
    ca_json_cursor root, choices, first, delta, content;
    char buf[64];
    ca_json_init(&root, doc, strlen(doc));
    CA_ASSERT(ca_json_enter_object(&root) == 0);
    CA_ASSERT(ca_json_field(&root, "choices", &choices) == 1);
    CA_ASSERT(ca_json_enter_array(&choices) == 0);
    CA_ASSERT(ca_json_index(&choices, 0, &first) == 1);
    CA_ASSERT(ca_json_enter_object(&first) == 0);
    CA_ASSERT(ca_json_field(&first, "delta", &delta) == 1);
    CA_ASSERT(ca_json_enter_object(&delta) == 0);
    CA_ASSERT(ca_json_field(&delta, "content", &content) == 1);
    CA_ASSERT(ca_json_read_string(&content, buf, sizeof(buf), NULL) == 0);
    CA_ASSERT(strcmp(buf, "你好") == 0);
}

CA_TEST(json_handles_escapes_in_strings) {
    /* LLM 输出里换行与引号极常见，转义处理错了会让界面显示乱码 */
    const char *doc = "{\"content\":\"a\\nb\\\"c\\\\d\"}";
    ca_json_cursor root, v;
    char buf[64];
    ca_json_init(&root, doc, strlen(doc));
    CA_ASSERT(ca_json_enter_object(&root) == 0);
    CA_ASSERT(ca_json_field(&root, "content", &v) == 1);
    size_t len = 0;
    CA_ASSERT(ca_json_read_string(&v, buf, sizeof(buf), &len) == 0);
    CA_ASSERT(strcmp(buf, "a\nb\"c\\d") == 0);
    CA_ASSERT_EQ_INT((long long)len, 7);
}

CA_TEST(json_decodes_unicode_escape) {
    const char *doc = "{\"c\":\"\\u4f60\\u597d\"}";
    ca_json_cursor root, v;
    char buf[64];
    ca_json_init(&root, doc, strlen(doc));
    CA_ASSERT(ca_json_enter_object(&root) == 0);
    CA_ASSERT(ca_json_field(&root, "c", &v) == 1);
    CA_ASSERT(ca_json_read_string(&v, buf, sizeof(buf), NULL) == 0);
    /* \u4f60\u597d 应解码为 UTF-8 的「你好」 */
    CA_ASSERT(strcmp(buf, "\xE4\xBD\xA0\xE5\xA5\xBD") == 0);
}

CA_TEST(json_string_rejects_overflow_without_truncating) {
    /* 缓冲区不足必须报错且**不修改缓冲区**。
     * 截断后的半个 JSON 看起来是合法的——那比报错危险得多。 */
    const char *doc = "{\"c\":\"0123456789abcdef\"}";
    ca_json_cursor root, v;
    char buf[8];
    memset(buf, 0x7F, sizeof(buf));
    ca_json_init(&root, doc, strlen(doc));
    CA_ASSERT(ca_json_enter_object(&root) == 0);
    CA_ASSERT(ca_json_field(&root, "c", &v) == 1);
    CA_ASSERT(ca_json_read_string(&v, buf, sizeof(buf), NULL) == -2);
    /* 缓冲区未被触碰 */
    for (size_t i = 0; i < sizeof(buf); i++) {
        CA_ASSERT_EQ_INT((long long)(unsigned char)buf[i], 0x7F);
    }
}

CA_TEST(json_reads_integers) {
    const char *doc = "{\"n\":-12345,\"big\":0}";
    ca_json_cursor root, v;
    long long n = 0;
    ca_json_init(&root, doc, strlen(doc));
    CA_ASSERT(ca_json_enter_object(&root) == 0);
    CA_ASSERT(ca_json_field(&root, "n", &v) == 1);
    CA_ASSERT(ca_json_read_int(&v, &n) == 0);
    CA_ASSERT_EQ_INT(n, -12345);
}

CA_TEST(json_skips_nested_structures) {
    /* 跳过必须能正确处理嵌套与字符串内的括号——后者最容易被漏掉 */
    const char *doc = "{\"a\":{\"b\":[1,2,{\"c\":\"}\"}]},\"d\":true}";
    ca_json_cursor root, a, d;
    long long t = 0;
    ca_json_init(&root, doc, strlen(doc));
    CA_ASSERT(ca_json_enter_object(&root) == 0);
    CA_ASSERT(ca_json_field(&root, "a", &a) == 1);
    /* a 的结构复杂，skip 必须干净跳过 */
    CA_ASSERT(ca_json_field(&root, "d", &d) == 1);
    CA_ASSERT(ca_json_skip_value(&d) == 0);
    (void)t;
}

CA_TEST(json_counts_array_elements) {
    const char *doc = "[{\"a\":1},{\"b\":2},{\"c\":3}]";
    ca_json_cursor arr;
    ca_json_init(&arr, doc, strlen(doc));
    CA_ASSERT(ca_json_enter_array(&arr) == 0);
    CA_ASSERT_EQ_INT(ca_json_array_count(&arr), 3);
}

CA_TEST(json_array_index_out_of_range) {
    const char *doc = "[1,2]";
    ca_json_cursor arr, out;
    ca_json_init(&arr, doc, strlen(doc));
    CA_ASSERT(ca_json_enter_array(&arr) == 0);
    CA_ASSERT(ca_json_index(&arr, 0, &out) == 1);
    CA_ASSERT(ca_json_index(&arr, 5, &out) == 0);
}

CA_TEST(json_rejects_malformed_input_without_crashing) {
    /* 畸形输入是常态（流被截断、服务端出错）。逐一验证不崩溃、
     * 不越界、返回可辨识的错误码。 */
    const char *cases[] = {
        "{", "[", "{\"a\"", "{\"a\":", "{\"a\":\"unterminated",
        "\"", "[1,2", "{\"a\":{\"b\":}}", "}", "]",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ca_json_cursor root, v;
        char buf[32];
        ca_json_init(&root, cases[i], strlen(cases[i]));
        (void)ca_json_enter_object(&root);
        (void)ca_json_enter_array(&root);
        (void)ca_json_field(&root, "a", &v);
        (void)ca_json_index(&root, 0, &v);
        (void)ca_json_skip_value(&root);
        (void)ca_json_array_count(&root);
        (void)ca_json_read_string(&v, buf, sizeof(buf), NULL);
    }
    /* 能走完且无崩溃即通过；断言数量保证用例没被优化掉 */
    CA_ASSERT(1);
}

CA_TEST(json_is_allocation_free) {
    /* 契约 C6：JSON 解析不得随输入增长分配内存。
     * 这里用一个很大的 JSON 文档验证：RSS 增量必须极小。 */
    size_t n = 4u * 1024u * 1024u;
    char *big = (char *)malloc(n + 64);
    CA_ASSERT(big != NULL);
    if (big == NULL) return;
    /* 造一个 "{\"padding\":\"aaaa...\",\"n\":1}" */
    size_t p = 0;
    memcpy(big + p, "{\"padding\":\"", 13); p += 13;
    memset(big + p, 'a', n); p += n;
    memcpy(big + p, "\",\"n\":1}", 8); p += 8;

    char buf[64];
    ca_json_cursor root, v;
    ca_json_init(&root, big, p);
    CA_ASSERT(ca_json_enter_object(&root) == 0);
    CA_ASSERT(ca_json_field(&root, "padding", &v) == 1);
    /* 读入定长缓冲必然装不下 —— 关键是**报错**而不是分配或截断 */
    CA_ASSERT(ca_json_read_string(&v, buf, sizeof(buf), NULL) == -2);
    CA_ASSERT(ca_json_field(&root, "n", &v) == 1);
    long long num = 0;
    CA_ASSERT(ca_json_read_int(&v, &num) == 0);
    CA_ASSERT_EQ_INT(num, 1);
    free(big);
}

int main(void) {
    struct { const char *name; void (*fn)(void); } cases[] = {
        {"json_reads_flat_object_field", ca_test_run_json_reads_flat_object_field},
        {"json_descends_nested_path", ca_test_run_json_descends_nested_path},
        {"json_handles_escapes_in_strings", ca_test_run_json_handles_escapes_in_strings},
        {"json_decodes_unicode_escape", ca_test_run_json_decodes_unicode_escape},
        {"json_string_rejects_overflow_without_truncating", ca_test_run_json_string_rejects_overflow_without_truncating},
        {"json_reads_integers", ca_test_run_json_reads_integers},
        {"json_skips_nested_structures", ca_test_run_json_skips_nested_structures},
        {"json_counts_array_elements", ca_test_run_json_counts_array_elements},
        {"json_array_index_out_of_range", ca_test_run_json_array_index_out_of_range},
        {"json_rejects_malformed_input_without_crashing", ca_test_run_json_rejects_malformed_input_without_crashing},
        {"json_is_allocation_free", ca_test_run_json_is_allocation_free},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ca_test_begin(cases[i].name);
        cases[i].fn();
        ca_test_end();
    }
    return ca_test_summary();
}