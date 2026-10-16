/* ca_test.h —— 极简测试框架
 * 契约 §C7：依赖固定为 libcurl，其余自研。
 * 引入 gtest 会给这个小工具拖进一整套依赖，与"极低内存"的出发点相悖。
 * 用不上 C++ 的 assert/参数化/死亡测试，几十行足够。
 */

#ifndef CAGENT_CA_TEST_H
#define CAGENT_CA_TEST_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 定义一个测试用例。 */
#define CA_TEST(name)                                                       \
    static void ca_test_##name(void);                                       \
    void ca_test_run_##name(void);                                          \
    void ca_test_run_##name(void) { ca_test_##name(); }                     \
    static void ca_test_##name(void)

/* 断言。失败时打印文件、行号与表达式，但不中止进程（便于一次跑完看全部失败）。 */
#define CA_ASSERT(cond)                                                     \
    do {                                                                    \
        ca_test_count();                                                    \
        if (!(cond)) {                                                      \
            ca_test_fail(__FILE__, __LINE__, #cond);                        \
        }                                                                   \
    } while (0)

#define CA_ASSERT_EQ_INT(actual, expected)                                  \
    do {                                                                    \
        ca_test_count();                                                    \
        long long a_ = (long long)(actual);                                 \
        long long e_ = (long long)(expected);                               \
        if (a_ != e_) {                                                     \
            ca_test_fail(__FILE__, __LINE__, #actual " == " #expected);     \
            ca_test_note_int("  实际=%lld 期望=%lld", a_, e_);              \
        }                                                                   \
    } while (0)

void ca_test_fail(const char *file, int line, const char *expr);
void ca_test_note_int(const char *fmt, long long a, long long b);
void ca_test_count(void);
void ca_test_begin(const char *name);
void ca_test_end(void);
int ca_test_summary(void);

#ifdef __cplusplus
}
#endif

#endif /* CAGENT_CA_TEST_H */