/* ca_test.c —— 极简测试框架实现
 * 契约 §C7：依赖固定为 libcurl，其余自研。
 */

#include "ca_test.h"

#include <stdio.h>

static int g_failures = 0;
static int g_checks = 0;
static const char *g_current = "";

void ca_test_count(void) {
    /* 每次断言都要计数。否则汇总会报"0 次检查"——
     * 一个会说谎的测试框架比没有框架更糟，它让人以为覆盖是已知的。 */
    g_checks++;
}

void ca_test_fail(const char *file, int line, const char *expr) {
    if (g_failures == 0) {
        printf("  [FAIL] %s\n", g_current);
    }
    g_failures++;
    printf("    %s:%d\n      断言失败: %s\n", file, line, expr);
}

void ca_test_note_int(const char *fmt, long long a, long long b) {
    printf("    ");
    printf(fmt, a, b);
    printf("\n");
}

void ca_test_begin(const char *name) {
    g_current = name;
    printf("[ RUN ] %s\n", name);
}

void ca_test_end(void) {
    if (g_failures == 0) {
        printf("[  ok ] %s\n", g_current);
    }
}

int ca_test_summary(void) {
    printf("\n%d 次检查，%d 次失败\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}