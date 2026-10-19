/* mem_test.c —— mem 层单元测试
 * 契约：docs/design/contract.md §2.2 / §10 T1
 *
 * 重点是**边界**：恰好超限、远超限、解除限制、计数器不撒谎。
 * 「刚好通过」这种用例最容易在实现调整时被无意改坏，所以每条都卡在边界上。
 */

#include "ca_test.h"

#include "cagent/mem.h"

#include <stdio.h>
#include <stdlib.h>

CA_TEST(rss_is_readable_and_plausible) {
    /* 内存计量是 NFR-1 的地基，取不到数字整套承诺就无从谈起 */
    int64_t rss = ca_rss_bytes();
    if (rss <= 0) {
        printf("    当前平台无法读取 RSS（返回 %lld），该用例无法验证\n", (long long)rss);
        return;
    }
    CA_ASSERT(rss > 0);
    /* 一个运行着的进程不可能低于 1 MB，也不可能超过 8 GB */
    CA_ASSERT(rss > 1024 * 1024);
    CA_ASSERT(rss < 8LL * 1024 * 1024 * 1024);
    printf("    当前 RSS = %.2f MB\n", (double)rss / 1024.0 / 1024.0);
}

CA_TEST(peak_rss_not_less_than_current) {
    int64_t cur = ca_rss_bytes();
    int64_t peak = ca_peak_rss_bytes();
    if (cur < 0 || peak < 0) return; /* 平台不支持，忽略 */
    /* 不变量 `peak >= cur` 由**操作系统**保证，不由本项目保证。
     *
     * Linux 的 hiwater_rss 是**延迟更新**的：内核只在缺页、退出等时机
     * 刷新它。刚涨上去的当前 RSS 可以暂时高于记录中的峰值 ——
     * 在 GitHub Actions runner 上实测复现过（peak < cur）。
     *
     * 早先这里是裸断言，于是 CI 偶发红，而它跟被测代码的��错无关。
     * 改成「峰值应在当前值的量级内」，测的是本项目真正依赖的性质：
     * 两个接口都给出了可用的、正的、同一量级的数值。
     */
    if (peak < cur) {
        /* 允许延迟记账造成的偏差，但仍要挡住「接口返回了垃圾值」 */
        CA_ASSERT(cur - peak <= (int64_t)4 * 1024 * 1024);
        printf("    (平台记账延迟: peak < cur，偏差 %lld 字节，在容忍范围内)\n",
               (long long)(cur - peak));
    }
    CA_ASSERT(peak > 0);
}

CA_TEST(rss_over_respects_limit) {
    int64_t rss = ca_rss_bytes();
    if (rss < 0) return;
    /* 上限设为 0 表示不检查，任何情况都不应报超限 */
    CA_ASSERT_EQ_INT(ca_rss_over(0), 0);
    /* 上限设到极小必然超限 */
    CA_ASSERT_EQ_INT(ca_rss_over(1024), 1);
    /* 上限设到极大必然不超限 */
    CA_ASSERT_EQ_INT(ca_rss_over(8LL * 1024 * 1024 * 1024), 0);
    /* 刚好的边界：上限等于当前 RSS 时不应算超限（严格大于才算） */
    CA_ASSERT_EQ_INT(ca_rss_over(rss), 0);
    CA_ASSERT_EQ_INT(ca_rss_over(rss - 1), 1);
}

CA_TEST(arena_basic_alloc_and_free) {
    ca_arena *a = ca_arena_create(4096);
    CA_ASSERT(a != NULL);
    void *p = ca_arena_alloc(a, 100);
    CA_ASSERT(p != NULL);
    /* 返回的内存必须清零 —— 否则解析器会读到未初始化内容，
     * 这类 bug 表现为"偶发的脏数据"，极难查 */
    unsigned char *bytes = (unsigned char *)p;
    int all_zero = 1;
    for (int i = 0; i < 100; i++) {
        if (bytes[i] != 0) all_zero = 0;
    }
    CA_ASSERT(all_zero);
    CA_ASSERT(ca_arena_used(a) >= 100);
    ca_arena_destroy(a);
}

CA_TEST(arena_null_is_safe) {
    /* 空指针无害：清理路径常会无条件调用 destroy */
    ca_arena_destroy(NULL);
    CA_ASSERT(ca_arena_alloc(NULL, 10) == NULL);
    CA_ASSERT_EQ_INT((long long)ca_arena_used(NULL), 0);
}

CA_TEST(arena_zero_size_returns_valid_pointer) {
    ca_arena *a = ca_arena_create(4096);
    /* 分配 0 字节应返回可安全 free 的指针，而不是 NULL
     * （调用方会把它当成失败处理，导致难以定位的行为差异） */
    void *p = ca_arena_alloc(a, 0);
    CA_ASSERT(p != NULL);
    ca_arena_destroy(a);
}

CA_TEST(arena_enforces_hard_limit) {
    /* 契约 C3：超限返回 NULL，不隐式扩容。
     * 这是"内存不会失控"的核心保证，必须在**边界**上验证。 */
    ca_arena *a = ca_arena_create(4096);
    CA_ASSERT(a != NULL);

    /* 逐块分配直到触顶 */
    size_t chunks = 0;
    void *p = NULL;
    while ((p = ca_arena_alloc(a, 64 * 1024)) != NULL) {
        chunks++;
        if (chunks > 1000) break; /* 防御：万一上限失效，不至于死循环 */
    }
    CA_ASSERT(p == NULL);
    CA_ASSERT(chunks > 0);
    CA_ASSERT(ca_arena_used(a) <= ca_arena_limit(a));
    printf("    触顶于第 %u 块，上限 %u 字节\n", (unsigned)chunks,
           (unsigned)ca_arena_limit(a));
    ca_arena_destroy(a);
}

CA_TEST(arena_single_alloc_over_limit_fails_without_allocating) {
    ca_arena *a = ca_arena_create(4096);
    /* 单笔请求就超过整个上限时，必须直接失败 */
    CA_ASSERT(ca_arena_alloc(a, (size_t)CA_LIMIT_PARSER_BYTES + 1) == NULL);
    CA_ASSERT_EQ_INT((long long)ca_arena_used(a), 0);
    ca_arena_destroy(a);
}

CA_TEST(arena_limit_tightening_allowed_raising_capped) {
    /* 收紧是安全的，放宽才危险 —— 这条用例曾经失败过：
     * 头文件写「只增不减」、实现放行收紧、测试要求拒绝收紧，三者不一致。
     * 最终判定：收紧应当允许（只会让预算更严），放宽才需要设防。
     */
    ca_arena *a = ca_arena_create(4096);
    size_t original = ca_arena_limit(a);
    CA_ASSERT_EQ_INT((long long)original, (long long)CA_LIMIT_PARSER_BYTES);

    /* 收紧：允许，且立即生效 */
    CA_ASSERT(ca_arena_set_limit(a, 8192) == 1);
    CA_ASSERT_EQ_INT((long long)ca_arena_limit(a), 8192);

    /* 收紧后立即生效于分配：超过新上限的请求必须失败 */
    CA_ASSERT(ca_arena_alloc(a, 16384) == NULL);
    CA_ASSERT(ca_arena_alloc(a, 1024) != NULL);

    /* 放宽到超过全局封顶：必须拒绝，且上限保持不变 */
    CA_ASSERT(ca_arena_set_limit(a, (size_t)CA_LIMIT_PARSER_BYTES + 1) == 0);
    CA_ASSERT_EQ_INT((long long)ca_arena_limit(a), 8192);

    /* 合法放宽（不超过封顶）：允许 */
    CA_ASSERT(ca_arena_set_limit(a, (size_t)CA_LIMIT_PARSER_BYTES) == 1);
    CA_ASSERT_EQ_INT((long long)ca_arena_limit(a), (long long)CA_LIMIT_PARSER_BYTES);

    /* NULL 安全 */
    CA_ASSERT(ca_arena_set_limit(NULL, 1024) == 0);
    ca_arena_destroy(a);
}

CA_TEST(arena_allocations_are_distinct_and_stable) {
    ca_arena *a = ca_arena_create(4096);
    /* 连续分配互不重叠：arena 最常见的实现错误是复用同一块尾部空间 */
    void *p1 = ca_arena_alloc(a, 64);
    void *p2 = ca_arena_alloc(a, 64);
    CA_ASSERT(p1 != NULL && p2 != NULL);
    CA_ASSERT(p1 != p2);
    ((char *)p1)[0] = 'A';
    ((char *)p2)[0] = 'B';
    CA_ASSERT(((char *)p1)[0] == 'A');
    CA_ASSERT(((char *)p2)[0] == 'B');
    ca_arena_destroy(a);
}

CA_TEST(arena_survives_many_small_allocs) {
    ca_arena *a = ca_arena_create(1024);
    /* 碎片压力：跨多个块的小额分配，used 必须单调不丢账 */
    size_t total = 0;
    for (int i = 0; i < 5000; i++) {
        void *p = ca_arena_alloc(a, 24);
        if (p == NULL) break;
        total += 24;
    }
    size_t used = ca_arena_used(a);
    CA_ASSERT(used <= ca_arena_limit(a));
    CA_ASSERT(used >= (total > CA_LIMIT_PARSER_BYTES ? CA_LIMIT_PARSER_BYTES : total) - 64);
    ca_arena_destroy(a);
}

int main(void) {
    ca_test_begin("rss_is_readable_and_plausible");
    ca_test_run_rss_is_readable_and_plausible();
    ca_test_end();

    ca_test_begin("peak_rss_not_less_than_current");
    ca_test_run_peak_rss_not_less_than_current();
    ca_test_end();

    ca_test_begin("rss_over_respects_limit");
    ca_test_run_rss_over_respects_limit();
    ca_test_end();

    ca_test_begin("arena_basic_alloc_and_free");
    ca_test_run_arena_basic_alloc_and_free();
    ca_test_end();

    ca_test_begin("arena_null_is_safe");
    ca_test_run_arena_null_is_safe();
    ca_test_end();

    ca_test_begin("arena_zero_size_returns_valid_pointer");
    ca_test_run_arena_zero_size_returns_valid_pointer();
    ca_test_end();

    ca_test_begin("arena_enforces_hard_limit");
    ca_test_run_arena_enforces_hard_limit();
    ca_test_end();

    ca_test_begin("arena_single_alloc_over_limit_fails_without_allocating");
    ca_test_run_arena_single_alloc_over_limit_fails_without_allocating();
    ca_test_end();

    ca_test_begin("arena_limit_tightening_allowed_raising_capped");
    ca_test_run_arena_limit_tightening_allowed_raising_capped();
    ca_test_end();

    ca_test_begin("arena_allocations_are_distinct_and_stable");
    ca_test_run_arena_allocations_are_distinct_and_stable();
    ca_test_end();

    ca_test_begin("arena_survives_many_small_allocs");
    ca_test_run_arena_survives_many_small_allocs();
    ca_test_end();

    return ca_test_summary();
}