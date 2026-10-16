/* mem.c —— 内存计量与 arena 分配器
 * 契约：docs/design/contract.md §2
 */

#include "cagent/mem.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <psapi.h>
#else
#  include <stdio.h>
#  include <unistd.h>
#  include <sys/resource.h>
#endif

/* ---------------------------------------------------------------- 内存计量 */

#if defined(_WIN32)

/* GetProcessMemoryInfo 来自 psapi.lib。
 * 它只是一个导入表项（现代 Windows 上该 API 实现在 kernel32 内），
 * 运行时不增加额外负担，因此直接静态链接，不做动态加载那套复杂化。
 * 真正的内存大头是缓冲区，不是这一个 import。
 */
static int query_process_memory(PROCESS_MEMORY_COUNTERS *out) {
    out->cb = (DWORD)sizeof(*out);
    return GetProcessMemoryInfo(GetCurrentProcess(), out, sizeof(*out)) ? 1 : 0;
}

int64_t ca_rss_bytes(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    if (!query_process_memory(&pmc)) return -1;
    return (int64_t)pmc.WorkingSetSize;
}

int64_t ca_peak_rss_bytes(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    if (!query_process_memory(&pmc)) return -1;
    return (int64_t)pmc.PeakWorkingSetSize;
}

#else /* POSIX */

int64_t ca_rss_bytes(void) {
    /* /proc/self/statm 第 2 个字段是常驻页数，乘页大小即 RSS。
     * 比 getrusage 更准确（后者是历史最大值口径，不是当前值）。
     */
    FILE *f = fopen("/proc/self/statm", "r");
    if (f == NULL) return -1;
    unsigned long total = 0;
    unsigned long resident = 0;
    int got = fscanf(f, "%lu %lu", &total, &resident);
    fclose(f);
    if (got != 2) return -1;
    long page = 4096;
#  if defined(_SC_PAGESIZE)
    long p = sysconf(_SC_PAGESIZE);
    if (p > 0) page = p;
#  endif
    return (int64_t)resident * (int64_t)page;
}

int64_t ca_peak_rss_bytes(void) {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return -1;
    /* ru_maxrss 在 Linux 上是 KB，在 macOS/BSD 上是字节 */
#  if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    return (int64_t)ru.ru_maxrss;
#  else
    return (int64_t)ru.ru_maxrss * 1024;
#  endif
}

#endif

int ca_rss_over(int64_t limit_bytes) {
    if (limit_bytes <= 0) return 0;
    int64_t rss = ca_rss_bytes();
    if (rss < 0) return 0; /* 取不到内存数字就不误报 */
    return rss > limit_bytes ? 1 : 0;
}

/* ------------------------------------------------------------------- arena */

struct ca_block {
    struct ca_block *next;
    size_t cap;
    size_t used;
    /* 数据紧随其后，省一次分配与一次指针跳转 */
    unsigned char data[1];
};

struct ca_arena {
    struct ca_block *head;
    size_t limit; /* 硬上限 */
};

#define CA_ARENA_DEFAULT_LIMIT ((size_t)CA_LIMIT_PARSER_BYTES)
#define CA_ARENA_ALIGN 16u
#define CA_ARENA_MIN_BLOCK 65536u

static size_t align_up(size_t n) {
    return (n + (CA_ARENA_ALIGN - 1)) & ~(size_t)(CA_ARENA_ALIGN - 1);
}

/* 统计用量是 O(块数)。块数增长受限于硬上限，不会无界增长。 */
static size_t arena_used_of(const ca_arena *a) {
    size_t total = 0;
    for (struct ca_block *b = a->head; b != NULL; b = b->next) total += b->used;
    return total;
}

static size_t arena_cap_of(const ca_arena *a) {
    size_t total = 0;
    for (struct ca_block *b = a->head; b != NULL; b = b->next) total += b->cap;
    return total;
}

ca_arena *ca_arena_create(size_t initial_bytes) {
    ca_arena *a = (ca_arena *)calloc(1, sizeof(ca_arena));
    if (a == NULL) return NULL;
    if (initial_bytes < 4096) initial_bytes = 4096;
    /* 初始块也不超过上限：否则"创建即超限"会变成常态而不是错误 */
    if (initial_bytes > CA_ARENA_DEFAULT_LIMIT) initial_bytes = CA_ARENA_DEFAULT_LIMIT;
    struct ca_block *b = (struct ca_block *)malloc(sizeof(struct ca_block) + initial_bytes);
    if (b == NULL) {
        free(a);
        return NULL;
    }
    b->next = NULL;
    b->cap = initial_bytes;
    b->used = 0;
    a->head = b;
    a->limit = CA_ARENA_DEFAULT_LIMIT;
    return a;
}

void *ca_arena_alloc(ca_arena *a, size_t size) {
    if (a == NULL) return NULL;
    size_t want = align_up(size == 0 ? 1 : size);
    size_t limit = a->limit ? a->limit : CA_ARENA_DEFAULT_LIMIT;

    /* 契约 C3：超限返回 NULL，不隐式扩容。
     * 这里刻意不去 malloc 一块更大的——那正是内存失控的入口。
     */
    if (want > limit - arena_used_of(a)) return NULL;

    struct ca_block *last_block = a->head;
    while (last_block != NULL && last_block->next != NULL) last_block = last_block->next;

    /* 尾块还有空间就就地分配，避免块数膨胀 */
    if (last_block != NULL && last_block->cap - last_block->used >= want) {
        void *p = last_block->data + last_block->used;
        last_block->used += want;
        memset(p, 0, want);
        return p;
    }

    size_t cap = want > CA_ARENA_MIN_BLOCK ? want : CA_ARENA_MIN_BLOCK;
    size_t room = limit - arena_cap_of(a);
    if (cap > room) cap = want; /* 只为这一笔开刚好够大的块 */
    if (cap > room) return NULL;

    struct ca_block *b = (struct ca_block *)malloc(sizeof(struct ca_block) + cap);
    if (b == NULL) return NULL;
    b->next = NULL;
    b->cap = cap;
    b->used = want;
    if (last_block != NULL) {
        last_block->next = b;
    } else {
        a->head = b;
    }
    void *p = b->data;
    memset(p, 0, want);
    return p;
}

int ca_arena_set_limit(ca_arena *a, size_t new_limit) {
    if (a == NULL) return 0;
    /* 收紧安全，放宽才危险 */
    if (new_limit <= a->limit) {
        a->limit = new_limit;
        return 1;
    }
    /* 上限本身也必须封顶：否则"抬高上限"就成了绕过 C3 的后门 */
    if (new_limit > CA_ARENA_DEFAULT_LIMIT) return 0;
    a->limit = new_limit;
    return 1;
}

void ca_arena_destroy(ca_arena *a) {
    if (a == NULL) return;
    struct ca_block *b = a->head;
    while (b != NULL) {
        struct ca_block *next = b->next;
        free(b);
        b = next;
    }
    free(a);
}

size_t ca_arena_used(const ca_arena *a) {
    if (a == NULL) return 0;
    return arena_used_of(a);
}

size_t ca_arena_limit(const ca_arena *a) {
    if (a == NULL) return 0;
    return a->limit ? a->limit : CA_ARENA_DEFAULT_LIMIT;
}