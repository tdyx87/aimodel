/* mem_probe.c —— 空闲内存基线探针
 * 契约：docs/design/contract.md §2.5.3
 *
 * 为什么需要它：PRD NFR-1 承诺的是「**空闲基线** RSS」。
 * 而单元测试跑完就退出，测不出常驻占用——那才是用户真正感受到的成本：
 * 终端里开着不关的代理，每天吃掉多少 RAM。
 *
 * 因此需要一个**会待命**的进程：初始化各层、报告一次内存、然后静默等待。
 * 它刻意不包含业务逻辑——这里测的是「地基有多重」，不是功能有多少。
 *
 * 用法：
 *     mem_probe [保持秒数]
 * 保持期间可被外部读取 RSS。默认 30 秒。
 */

#include <stdio.h>
#include <stdlib.h>

#include "cagent/buf.h"
#include "cagent/http.h"
#include "cagent/json.h"
#include "cagent/llm.h"
#include "cagent/mem.h"
#include "cagent/sse.h"
#include "cagent/transport_mem.h"

#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#endif

static void sleep_seconds(int seconds) {
#ifdef _WIN32
    Sleep((DWORD)seconds * 1000);
#else
    sleep((unsigned)seconds);
#endif
}

int main(int argc, char **argv) {
    int hold = 30;
    if (argc > 1) {
        int v = atoi(argv[1]);
        if (v > 0) hold = v;
    }

    /* 启动瞬间的基线：还没初始化任何层 */
    int64_t rss_start = ca_rss_bytes();

    /* 初始化各层 —— 与真实程序的启动路径一致的部分。
     * 传输后端只 init 不发请求：内存承诺与联网与否无关。 */
    const ca_transport_ops *ops = ca_transport_default();
    if (ops != NULL && ops->init != NULL) ops->init();

    ca_buf b;
    ca_buf_init(&b, 65536);
    ca_sse s;
    ca_sse_init(&s, 0);
    /* llm 层持有 8 个 32 KB 的工具调用槽位（共 256 KB 定长），
     * 必须计入基线——它是常驻开销，不是按需分配。 */
    ca_llm *llm = ca_llm_create();
    (void)ca_transport_mem();

    int64_t rss_init = ca_rss_bytes();

    printf("传输后端: %s\n", ca_transport_name());
    printf("启动 RSS : %.2f MB\n", (double)rss_start / 1048576.0);
    printf("初始化后 : %.2f MB\n", (double)rss_init / 1048576.0);
    printf("保持 %d 秒，可从外部读取 RSS\n", hold);
    fflush(stdout);

    sleep_seconds(hold);

    int64_t rss_end = ca_rss_bytes();
    printf("结束时   : %.2f MB\n", (double)rss_end / 1048576.0);
    printf("峰值     : %.2f MB\n", (double)ca_peak_rss_bytes() / 1048576.0);

    ca_sse_free(&s);
    ca_buf_free(&b);
    ca_llm_free(llm);
    if (ops != NULL && ops->cleanup != NULL) ops->cleanup();
    return 0;
}