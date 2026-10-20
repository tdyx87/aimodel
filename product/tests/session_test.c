/* session_test.c —— 会话历史测试
 * 契约：docs/design/contract.md §6 / §2.1 / §10 T3
 *
 * T3 是契约点名要求的：灌入超限历史后，必须断言
 *   1) 淘汰真的发生了；
 *   2) UI 有明示（本层体现为 notice 被置位并带出数字）。
 * 只测「没崩」是不够的——静默丢掉上下文正是同类工具的毛病，
 * 而本产品与它们的差异点之一就是「丢了会说」。
 */

#include "ca_test.h"

#include "cagent/session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 造一条 n 字节的 'x' 串 */
static char *blob(size_t n) {
    char *p = (char *)malloc(n + 1);
    memset(p, 'x', n);
    p[n] = '\0';
    return p;
}

CA_TEST(session_keeps_messages_in_order) {
    ca_session s;
    CA_ASSERT_EQ_INT(ca_session_init(&s, 4096), 0);
    CA_ASSERT_EQ_INT(ca_session_add(&s, CA_MSG_USER, "你好"), 0);
    CA_ASSERT_EQ_INT(ca_session_add(&s, CA_MSG_ASSISTANT, "你也好"), 0);
    CA_ASSERT_EQ_INT(ca_session_count(&s), 2);
    CA_ASSERT(strcmp(ca_session_text(&s, 0), "你好") == 0);
    CA_ASSERT(strcmp(ca_session_text(&s, 1), "你也好") == 0);
    CA_ASSERT_EQ_INT(ca_session_role(&s, 1), CA_MSG_ASSISTANT);
    ca_session_free(&s);
}

CA_TEST(session_rejects_empty_and_null) {
    ca_session s;
    CA_ASSERT_EQ_INT(ca_session_init(&s, 4096), 0);
    /* 空消息不进历史：白占预算，模型还会看到一条空内容 */
    CA_ASSERT_EQ_INT(ca_session_add(&s, CA_MSG_USER, ""), -2);
    CA_ASSERT_EQ_INT(ca_session_add(&s, CA_MSG_USER, NULL), -1);
    CA_ASSERT_EQ_INT(ca_session_count(&s), 0);
    ca_session_free(&s);
}

CA_TEST(session_append_delta_merges_into_last_assistant) {
    ca_session s;
    CA_ASSERT_EQ_INT(ca_session_init(&s, 4096), 0);
    CA_ASSERT_EQ_INT(ca_session_add(&s, CA_MSG_USER, "问题"), 0);
    CA_ASSERT_EQ_INT(ca_session_append_delta(&s, "逐"), 0);
    CA_ASSERT_EQ_INT(ca_session_append_delta(&s, "字"), 0);
    CA_ASSERT_EQ_INT(ca_session_append_delta(&s, "到达"), 0);
    /* 三段增量必须合成一条消息，否则条数上限会被流式片段耗光 */
    CA_ASSERT_EQ_INT(ca_session_count(&s), 2);
    CA_ASSERT(strcmp(ca_session_text(&s, 1), "逐字到达") == 0);
    ca_session_free(&s);
}

/* ---------------------------------------------------------- T3 淘汰与明示 */

CA_TEST(session_evicts_oldest_turn_and_reports_it) {
    /* 预算刚好装得下两轮，第三轮进来必须挤掉第一轮。 */
    ca_session s;
    CA_ASSERT_EQ_INT(ca_session_init(&s, 1024), 0);
    char *m = blob(250); /* 每条 250 字节 + NUL */

    CA_ASSERT_EQ_INT(ca_session_add(&s, CA_MSG_USER, m), 0);
    CA_ASSERT_EQ_INT(ca_session_add(&s, CA_MSG_ASSISTANT, m), 0);
    CA_ASSERT_EQ_INT(ca_session_add(&s, CA_MSG_USER, m), 0);
    CA_ASSERT_EQ_INT(ca_session_add(&s, CA_MSG_ASSISTANT, m), 0);
    /* 4 条 × 251 = 1004 ≤ 1024，此时不该有淘汰 */
    CA_ASSERT_EQ_INT(ca_session_count(&s), 4);
    CA_ASSERT_EQ_INT(s.evicted_msgs, 0);
    CA_ASSERT_EQ_INT(ca_session_used(&s), 1004);

    /* 第 5 条进来 → 必须淘汰最早的**一整轮**（user+assistant 两条），
     * 而不是只砍一条 —— 拆散一轮会让模型看到孤立的助手回复。 */
    CA_ASSERT_EQ_INT(ca_session_add(&s, CA_MSG_USER, m), 0);
    CA_ASSERT_EQ_INT(ca_session_count(&s), 3);
    CA_ASSERT_EQ_INT(s.evicted_msgs, 2);
    CA_ASSERT(s.evicted_bytes > 0);
    CA_ASSERT(ca_session_used(&s) <= ca_session_budget(&s));

    /* 明示：notice 必须带出「丢了几条、几字节」 */
    int em = 0;
    size_t eb = 0;
    int tm = 0;
    size_t tb = 0;
    CA_ASSERT_EQ_INT(ca_session_take_notice(&s, &em, &eb, &tm, &tb), 1);
    CA_ASSERT_EQ_INT(em, 2);
    CA_ASSERT(eb > 0);
    CA_ASSERT_EQ_INT(tm, 0);
    /* 取过一次就清了：不能每轮都重复提示同一件事 */
    CA_ASSERT_EQ_INT(ca_session_take_notice(&s, &em, &eb, &tm, &tb), 0);
    free(m);
    ca_session_free(&s);
}

CA_TEST(session_never_exceeds_budget_under_flood) {
    /* 灌 500 轮：契约 T3 的「灌入超限历史」。 */
    ca_session s;
    CA_ASSERT_EQ_INT(ca_session_init(&s, 4096), 0);
    char *q = blob(300);
    for (int i = 0; i < 500; i++) {
        CA_ASSERT(ca_session_add(&s, CA_MSG_USER, q) == 0);
        CA_ASSERT_EQ_INT(ca_session_append_delta(&s, q), 0);
        CA_ASSERT(ca_session_used(&s) <= ca_session_budget(&s));
    }
    CA_ASSERT(s.evicted_msgs > 0);
    CA_ASSERT(ca_session_count(&s) < 500);
    /* 淘汰必须成整轮发生：留下的第一条仍应是 user */
    CA_ASSERT_EQ_INT(ca_session_role(&s, 0), CA_MSG_USER);
    free(q);
    ca_session_free(&s);
}

CA_TEST(session_truncates_single_oversized_message_explicitly) {
    ca_session s;
    CA_ASSERT_EQ_INT(ca_session_init(&s, 1024), 0);
    char *big = blob(5000); /* 单条远超整个预算 */
    CA_ASSERT_EQ_INT(ca_session_add(&s, CA_MSG_USER, big), 0);
    CA_ASSERT(ca_session_used(&s) <= ca_session_budget(&s));
    CA_ASSERT_EQ_INT(ca_session_count(&s), 1);

    int em = 0;
    size_t eb = 0;
    int tm = 0;
    size_t tb = 0;
    /* 截断必须显式记账：静默截断的上下文会让模型基于半句话作答 */
    CA_ASSERT_EQ_INT(ca_session_take_notice(&s, &em, &eb, &tm, &tb), 1);
    CA_ASSERT_EQ_INT(tm, 1);
    CA_ASSERT_EQ_INT(tb, 5000 - 1023);
    free(big);
    ca_session_free(&s);
}

/* ---------------------------------------------------------- 导出给 llm */

CA_TEST(session_exports_llm_messages) {
    ca_session s;
    CA_ASSERT_EQ_INT(ca_session_init(&s, 4096), 0);
    CA_ASSERT_EQ_INT(ca_session_add(&s, CA_MSG_USER, "问"), 0);
    CA_ASSERT_EQ_INT(ca_session_add(&s, CA_MSG_ASSISTANT, "答"), 0);

    ca_llm_msg msgs[8];
    size_t n = 0;
    ca_session_export(&s, msgs, &n);
    CA_ASSERT_EQ_INT(n, 2);
    CA_ASSERT_EQ_INT(msgs[0].role, CA_MSG_USER);
    CA_ASSERT(strcmp(msgs[0].text, "问") == 0);
    CA_ASSERT_EQ_INT(msgs[1].role, CA_MSG_ASSISTANT);
    CA_ASSERT(strcmp(msgs[1].text, "答") == 0);
    ca_session_free(&s);
}

/* ---------------------------------------------------------- JSON 往返 */

CA_TEST(session_json_round_trip) {
    ca_session s;
    CA_ASSERT_EQ_INT(ca_session_init(&s, 4096), 0);
    snprintf(s.id, sizeof(s.id), "s1234");
    CA_ASSERT_EQ_INT(ca_session_add(&s, CA_MSG_USER, "引号\"与\\反斜杠"), 0);
    CA_ASSERT_EQ_INT(ca_session_add(&s, CA_MSG_ASSISTANT, "换行\n制表\t"), 0);

    size_t need = 0;
    CA_ASSERT_EQ_INT(ca_session_to_json(&s, NULL, 0, &need), 0);
    CA_ASSERT(need > 1);
    char *doc = (char *)malloc(need);
    CA_ASSERT_EQ_INT(ca_session_to_json(&s, doc, need, NULL), 0);
    /* 自包含：内容真的在里面 */
    CA_ASSERT(strstr(doc, "\\\"与\\\\反斜杠") != NULL);

    ca_session back;
    CA_ASSERT_EQ_INT(ca_session_init(&back, 4096), 0);
    CA_ASSERT_EQ_INT(ca_session_from_json(&back, doc, strlen(doc)), 0);
    CA_ASSERT_EQ_INT(ca_session_count(&back), 2);
    CA_ASSERT(strcmp(ca_session_text(&back, 0), "引号\"与\\反斜杠") == 0);
    CA_ASSERT(strcmp(ca_session_text(&back, 1), "换行\n制表\t") == 0);
    CA_ASSERT(strcmp(back.id, "s1234") == 0);

    free(doc);
    ca_session_free(&back);
    ca_session_free(&s);
}

CA_TEST(session_json_never_contains_key_field) {
    /* NFR-3：密钥绝不进会话文件。本层结构上就没有存放它的地方，
     * 这条测试是把这个事实钉住——防止将来有人「顺手」加进去。 */
    ca_session s;
    CA_ASSERT_EQ_INT(ca_session_init(&s, 4096), 0);
    CA_ASSERT_EQ_INT(ca_session_add(&s, CA_MSG_USER, "sk-should-not-leak"), 0);
    size_t need = 0;
    CA_ASSERT_EQ_INT(ca_session_to_json(&s, NULL, 0, &need), 0);
    char *doc = (char *)malloc(need);
    CA_ASSERT_EQ_INT(ca_session_to_json(&s, doc, need, NULL), 0);
    CA_ASSERT(strstr(doc, "api_key") == NULL);
    CA_ASSERT(strstr(doc, "Authorization") == NULL);
    free(doc);
    ca_session_free(&s);
}

CA_TEST(session_json_rejects_corrupt_without_crashing) {
    const char *bad[] = {
        "",
        "{",
        "[]",
        "{\"messages\":}",
        "{\"messages\":[{\"role\":\"user\"}]}",
        "{\"messages\":[{\"content\":\"x\"}]}",
        "{\"messages\":123}",
        "not json at all",
    };
    /* 刻意不测「孤立代理对」（\uD800）：本层不做 Unicode 代理对校验，
     * 那属于解码层的职责。这里断言的是**结构性**损坏能被识别。 */
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ca_session s;
        CA_ASSERT_EQ_INT(ca_session_init(&s, 4096), 0);
        int rc = ca_session_from_json(&s, bad[i], strlen(bad[i]));
        /* 坏文件必须被识别成坏文件（-1），而不是崩溃或当成空会话 */
        CA_ASSERT_EQ_INT(rc, -1);
        ca_session_free(&s);
    }
}

CA_TEST(session_json_load_evicts_when_over_budget) {
    /* 契约 §6：读取同样受上限约束，超出按轮次淘汰规则截断。
     * 单条装得下、总长装不下时，走的是**淘汰**而不是截断。 */
    ca_session src;
    CA_ASSERT_EQ_INT(ca_session_init(&src, 65536), 0);
    char *m = blob(2000);
    for (int i = 0; i < 10; i++) {
        CA_ASSERT_EQ_INT(ca_session_add(&src, CA_MSG_USER, m), 0);
        CA_ASSERT_EQ_INT(ca_session_add(&src, CA_MSG_ASSISTANT, m), 0);
    }
    size_t need = 0;
    CA_ASSERT_EQ_INT(ca_session_to_json(&src, NULL, 0, &need), 0);
    char *doc = (char *)malloc(need);
    CA_ASSERT_EQ_INT(ca_session_to_json(&src, doc, need, NULL), 0);

    ca_session dst;
    CA_ASSERT_EQ_INT(ca_session_init(&dst, 8192), 0);
    /* 返回值 0：能装下，只是丢了旧轮次。丢了多少由 notice 说。 */
    CA_ASSERT_EQ_INT(ca_session_from_json(&dst, doc, strlen(doc)), 0);
    CA_ASSERT(ca_session_used(&dst) <= ca_session_budget(&dst));
    CA_ASSERT(ca_session_count(&dst) > 0);
    CA_ASSERT(ca_session_count(&dst) < 20);
    CA_ASSERT(dst.evicted_msgs > 0);
    CA_ASSERT_EQ_INT(ca_session_role(&dst, 0), CA_MSG_USER);

    free(doc);
    free(m);
    ca_session_free(&dst);
    ca_session_free(&src);
}

CA_TEST(session_json_load_truncates_oversized_message) {
    /* 单条消息本身就超过载入方的预算：这才是 -2（已截断）的场合。 */
    ca_session src;
    CA_ASSERT_EQ_INT(ca_session_init(&src, 65536), 0);
    char *m = blob(9000);
    CA_ASSERT_EQ_INT(ca_session_add(&src, CA_MSG_USER, m), 0);
    size_t need = 0;
    CA_ASSERT_EQ_INT(ca_session_to_json(&src, NULL, 0, &need), 0);
    char *doc = (char *)malloc(need);
    CA_ASSERT_EQ_INT(ca_session_to_json(&src, doc, need, NULL), 0);

    ca_session dst;
    CA_ASSERT_EQ_INT(ca_session_init(&dst, 4096), 0);
    CA_ASSERT_EQ_INT(ca_session_from_json(&dst, doc, strlen(doc)), -2);
    CA_ASSERT(ca_session_used(&dst) <= ca_session_budget(&dst));

    int em = 0;
    size_t eb = 0;
    int tm = 0;
    size_t tb = 0;
    CA_ASSERT_EQ_INT(ca_session_take_notice(&dst, &em, &eb, &tm, &tb), 1);
    CA_ASSERT_EQ_INT(tm, 1);

    free(doc);
    free(m);
    ca_session_free(&dst);
    ca_session_free(&src);
}

CA_TEST(session_json_to_json_rejects_small_buffer) {
    ca_session s;
    CA_ASSERT_EQ_INT(ca_session_init(&s, 4096), 0);
    CA_ASSERT_EQ_INT(ca_session_add(&s, CA_MSG_USER, "一段需要不少字节的内容"), 0);
    size_t need = 0;
    CA_ASSERT_EQ_INT(ca_session_to_json(&s, NULL, 0, &need), 0);

    char small[16];
    memset(small, 'Z', sizeof(small));
    CA_ASSERT_EQ_INT(ca_session_to_json(&s, small, sizeof(small), &need), -2);
    /* 失败时不留半截产物 */
    CA_ASSERT(small[0] == 'Z');
    ca_session_free(&s);
}

int main(void) {
    struct {
        const char *name;
        void (*fn)(void);
    } cases[] = {
        {"session_keeps_messages_in_order", ca_test_run_session_keeps_messages_in_order},
        {"session_rejects_empty_and_null", ca_test_run_session_rejects_empty_and_null},
        {"session_append_delta_merges_into_last_assistant", ca_test_run_session_append_delta_merges_into_last_assistant},
        {"session_evicts_oldest_turn_and_reports_it", ca_test_run_session_evicts_oldest_turn_and_reports_it},
        {"session_never_exceeds_budget_under_flood", ca_test_run_session_never_exceeds_budget_under_flood},
        {"session_truncates_single_oversized_message_explicitly", ca_test_run_session_truncates_single_oversized_message_explicitly},
        {"session_exports_llm_messages", ca_test_run_session_exports_llm_messages},
        {"session_json_round_trip", ca_test_run_session_json_round_trip},
        {"session_json_never_contains_key_field", ca_test_run_session_json_never_contains_key_field},
        {"session_json_rejects_corrupt_without_crashing", ca_test_run_session_json_rejects_corrupt_without_crashing},
        {"session_json_load_evicts_when_over_budget", ca_test_run_session_json_load_evicts_when_over_budget},
        {"session_json_load_truncates_oversized_message", ca_test_run_session_json_load_truncates_oversized_message},
        {"session_json_to_json_rejects_small_buffer", ca_test_run_session_json_to_json_rejects_small_buffer},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ca_test_begin(cases[i].name);
        cases[i].fn();
        ca_test_end();
    }
    return ca_test_summary();
}
