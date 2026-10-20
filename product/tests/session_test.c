/* session_test.c —— 会话历史测试
 * 契约：docs/design/contract.md §6 / §2.1 / §10 T3
 *
 * T3 是契约点名要求的：灌入超限历史后，必须断言
 *   1) 淘汰真的发生了；
 *   2) UI 有明示（本层体现为 notice 被置位并带出数字）。
 * 只测「没崩」是不够的——静默丢掉上下文正是同类工具的毛病，
 * 而本产品与它们的差异点之一就是「丢了会说」。
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#if defined(_WIN32)
#  define SEP_CHAR '\\'
#else
#  define SEP_CHAR '/'
#endif

#include "ca_test.h"

#include "cagent/session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* ---------------------------------------------------------- 落盘 */

/* 每轮用一个独立的临时目录，绝不去碰用户真实的会话目录。 */
static void make_test_dir(char *buf, size_t cap) {
    const char *base = getenv("TEMP");
    if (base == NULL) base = getenv("TMPDIR");
    if (base == NULL) base = ".";
    snprintf(buf, cap, "%s%ccagent_sess_test_%lu",
             base, (char)SEP_CHAR, (unsigned long)time(NULL));
}

CA_TEST(session_saves_lists_loads_and_deletes) {
    char dir[CA_SESSION_MAX_PATH];
    make_test_dir(dir, sizeof(dir));
    CA_ASSERT_EQ_INT(ca_session_dir_ensure(dir), 0);

    ca_session s;
    CA_ASSERT_EQ_INT(ca_session_init(&s, 8192), 0);
    CA_ASSERT_EQ_INT(ca_session_add(&s, CA_MSG_USER, "落盘的问题"), 0);
    CA_ASSERT_EQ_INT(ca_session_append_delta(&s, "落盘的答案"), 0);
    CA_ASSERT_EQ_INT(ca_session_save(&s, dir), 0);
    /* id 由保存时生成：调用方不该被要求先想好名字 */
    CA_ASSERT(s.id[0] != '\0');

    ca_session_info list[8];
    size_t n = 0;
    CA_ASSERT_EQ_INT(ca_session_list(dir, list, 8, &n), 1);
    CA_ASSERT_EQ_INT(n, 1);
    CA_ASSERT(strcmp(list[0].id, s.id) == 0);
    CA_ASSERT_EQ_INT(list[0].corrupt, 0);
    CA_ASSERT_EQ_INT(list[0].msgs, 2);

    ca_session back;
    CA_ASSERT_EQ_INT(ca_session_init(&back, 8192), 0);
    CA_ASSERT_EQ_INT(ca_session_load(&back, list[0].path), 0);
    CA_ASSERT_EQ_INT(ca_session_count(&back), 2);
    CA_ASSERT(strcmp(ca_session_text(&back, 0), "落盘的问题") == 0);
    CA_ASSERT(strcmp(ca_session_text(&back, 1), "落盘的答案") == 0);
    /* 载入后接着问，历史必须延续——这才是 US-6「继续其中之一」 */
    CA_ASSERT_EQ_INT(ca_session_add(&back, CA_MSG_USER, "接着问"), 0);
    CA_ASSERT_EQ_INT(ca_session_count(&back), 3);

    CA_ASSERT_EQ_INT(ca_session_delete(dir, s.id), 0);
    CA_ASSERT_EQ_INT(ca_session_list(dir, list, 8, &n), 0);
    CA_ASSERT_EQ_INT(n, 0);

    ca_session_free(&back);
    ca_session_free(&s);
}

CA_TEST(session_saved_file_has_no_key) {
    char dir[CA_SESSION_MAX_PATH];
    make_test_dir(dir, sizeof(dir));
    CA_ASSERT_EQ_INT(ca_session_dir_ensure(dir), 0);

    ca_session s;
    CA_ASSERT_EQ_INT(ca_session_init(&s, 8192), 0);
    CA_ASSERT_EQ_INT(ca_session_add(&s, CA_MSG_USER, "sk-do-not-persist"), 0);
    CA_ASSERT_EQ_INT(ca_session_save(&s, dir), 0);

    /* 用产品自己的路径拼接，而不是在测试里再拼一次：
     * 手写 "%s%c%s" 时 GCC 的 -Wformat-truncation 会（在这个已知长度的
     * 局部数组上）算不出结果一定放得下而告警，而零告警是硬门禁。 */
    char path[CA_SESSION_MAX_PATH];
    CA_ASSERT_EQ_INT(ca_session_path_for(dir, s.id, path, sizeof(path)), 0);

    FILE *f = fopen(path, "rb");
    CA_ASSERT(f != NULL);
    if (f != NULL) {
        char buf[8192];
        size_t got = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[got] = '\0';
        CA_ASSERT(strstr(buf, "api_key") == NULL);
        CA_ASSERT(strstr(buf, "Authorization") == NULL);
        CA_ASSERT(strstr(buf, "sk-do-not-persist") != NULL); /* 用户原话照存 */
    }
    ca_session_delete(dir, s.id);
    ca_session_free(&s);
}

CA_TEST(session_list_sorts_newest_first) {
    char dir[CA_SESSION_MAX_PATH];
    make_test_dir(dir, sizeof(dir));
    CA_ASSERT_EQ_INT(ca_session_dir_ensure(dir), 0);

    for (int i = 0; i < 3; i++) {
        ca_session s;
        CA_ASSERT_EQ_INT(ca_session_init(&s, 4096), 0);
        char msg[32];
        snprintf(msg, sizeof(msg), "第 %d 次", i);
        CA_ASSERT_EQ_INT(ca_session_add(&s, CA_MSG_USER, msg), 0);
        CA_ASSERT_EQ_INT(ca_session_save(&s, dir), 0);
        ca_session_free(&s);
    }

    ca_session_info list[8];
    size_t n = 0;
    CA_ASSERT_EQ_INT(ca_session_list(dir, list, 8, &n), 3);
    CA_ASSERT_EQ_INT(n, 3);
    /* 断言排序不变量：时间戳不递增 */
    for (size_t i = 1; i < n; i++) {
        CA_ASSERT(list[i - 1].mtime >= list[i].mtime);
    }
    for (size_t i = 0; i < n; i++) ca_session_delete(dir, list[i].id);
    ca_session_free(NULL); /* 空指针是安全的空操作 */
}

CA_TEST(session_corrupt_file_is_listed_not_fatal) {
    char dir[CA_SESSION_MAX_PATH];
    make_test_dir(dir, sizeof(dir));
    CA_ASSERT_EQ_INT(ca_session_dir_ensure(dir), 0);

    /* 手写一个坏文件：截断的 JSON（路径同样交给产品去拼，见上） */
    char bad[CA_SESSION_MAX_PATH];
    CA_ASSERT_EQ_INT(ca_session_path_for(dir, "bad", bad, sizeof(bad)), 0);
    FILE *f = fopen(bad, "wb");
    CA_ASSERT(f != NULL);
    if (f != NULL) {
        fputs("{\"v\":1,\"messages\":[{\"role\":\"user\",", f);
        fclose(f);
    }

    ca_session s;
    CA_ASSERT_EQ_INT(ca_session_init(&s, 4096), 0);
    CA_ASSERT_EQ_INT(ca_session_add(&s, CA_MSG_USER, "好文件"), 0);
    CA_ASSERT_EQ_INT(ca_session_save(&s, dir), 0);

    ca_session_info list[8];
    size_t n = 0;
    /* 关键：坏文件不能让「列出会话」整体失败——用户的工具不该因为
     * 一个坏文件就打不开。 */
    CA_ASSERT_EQ_INT(ca_session_list(dir, list, 8, &n), 2);
    CA_ASSERT_EQ_INT(n, 2);
    int corrupt_seen = 0;
    for (size_t i = 0; i < n; i++) {
        if (list[i].corrupt) {
            corrupt_seen = 1;
            CA_ASSERT(strcmp(list[i].id, "bad") == 0);
            CA_ASSERT_EQ_INT(list[i].msgs, 0);
        }
    }
    CA_ASSERT(corrupt_seen); /* 必须**被标出来**，而不是悄悄消失 */

    ca_session bad_sess;
    CA_ASSERT_EQ_INT(ca_session_init(&bad_sess, 4096), 0);
    CA_ASSERT_EQ_INT(ca_session_load(&bad_sess, bad), -1);
    CA_ASSERT_EQ_INT(ca_session_count(&bad_sess), 0);

    ca_session_delete(dir, s.id);
    remove(bad);
    ca_session_free(&bad_sess);
    ca_session_free(&s);
}

CA_TEST(session_delete_refuses_path_traversal) {
    char dir[CA_SESSION_MAX_PATH];
    make_test_dir(dir, sizeof(dir));
    CA_ASSERT_EQ_INT(ca_session_dir_ensure(dir), 0);
    /* id 由 main 透传进来，必须挡住越出会话目录的写法 */
    CA_ASSERT_EQ_INT(ca_session_delete(dir, ".."), -1);
    CA_ASSERT_EQ_INT(ca_session_delete(dir, "../evil"), -1);
    CA_ASSERT_EQ_INT(ca_session_delete(dir, ""), -1);
}

CA_TEST(session_dir_is_under_user_config) {
    char dir[CA_SESSION_MAX_PATH];
    CA_ASSERT_EQ_INT(ca_session_dir(dir, sizeof(dir)), 0);
    /* 会话不该写进工作区：写进工作区就会被提交、被误删、
     * 也会污染用户的仓库。 */
    CA_ASSERT(strstr(dir, "cagent") != NULL);
    CA_ASSERT(strstr(dir, "sessions") != NULL);
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
        {"session_saves_lists_loads_and_deletes", ca_test_run_session_saves_lists_loads_and_deletes},
        {"session_saved_file_has_no_key", ca_test_run_session_saved_file_has_no_key},
        {"session_list_sorts_newest_first", ca_test_run_session_list_sorts_newest_first},
        {"session_corrupt_file_is_listed_not_fatal", ca_test_run_session_corrupt_file_is_listed_not_fatal},
        {"session_delete_refuses_path_traversal", ca_test_run_session_delete_refuses_path_traversal},
        {"session_dir_is_under_user_config", ca_test_run_session_dir_is_under_user_config},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ca_test_begin(cases[i].name);
        cases[i].fn();
        ca_test_end();
    }
    return ca_test_summary();
}
