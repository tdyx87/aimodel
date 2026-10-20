/* session_store.c —— 会话落盘（契约 §6 / §8）
 *
 * 本文件是 session 层的**平台边界**：配置目录在哪、目录怎么建、
 * 目录怎么遍历、修改时间怎么取，全都关在这里。
 * 上层（session.c / core / main）只看到「保存 / 载入 / 列出 / 删除」，
 * 一个 #ifdef 都碰不到。
 *
 * 三条必须守住的约定：
 *   1. 读文件**带上限**。会话文件是外部输入，可以被人为放大；
 *      没有上限的读取就是把「内存可预测」交给别人决定。
 *   2. 损坏文件不得导致启动失败。用户不该因为一个坏文件用不了工具。
 *   3. 写文件走临时名 + 改名。直接覆盖时若中途失败，留下的是半个文件，
 *      而它下次会被当成损坏会话——自己把自己的数据弄丢。
 */

/* MSVC 把 fopen/getenv/stat 一类的 C 标准函数标成「不安全」，
 * 建议换成 _s 变体。但那些变体不是 C99 标准的一部分，
 * 换过去就等于把可移植代码绑死在 MSVC 上。
 *
 * 必须定义在**所有** include 之前——包括本项目自己的头文件：
 * MSVC 的 <stddef.h>/<stdint.h> 会拉进 vcruntime.h，而
 * 「要不要告警」这个判断在那时就定下来了，之后再定义也没有用。
 * 放在 #include "cagent/session.h" 之后实测无效。
 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "cagent/session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  include <direct.h>
#  include <io.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  define CA_MKDIR(p) _mkdir(p)
#  define CA_SEP '\\'
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#  define CA_MKDIR(p) mkdir(p, 0755)
#  define CA_SEP '/'
#endif

/* 会话文件大小硬上限：与历史预算一致。超过它就不该被整个读进来。 */
#define CA_SESSION_FILE_MAX CA_LIMIT_HISTORY_BYTES
#define CA_SESSION_SUFFIX ".json"

/* MSVC 的 <sys/stat.h> 只给 S_IFDIR/S_IFMT，不给 S_ISDIR 这个宏
 * （POSIX 两种都有）。与其在调用处写 #ifdef，不如补齐一次。 */
#if !defined(S_ISDIR)
#  define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif

static int path_join(char *buf, size_t cap, const char *dir, const char *name) {
    if (buf == NULL || dir == NULL || name == NULL) return -1;
    int n = snprintf(buf, cap, "%s%c%s", dir, CA_SEP, name);
    if (n <= 0 || (size_t)n >= cap) return -1;
    return 0;
}

static int env_dir(char *buf, size_t cap, const char *name) {
    const char *v = getenv(name);
    if (v == NULL || v[0] == '\0') return -1;
    int n = snprintf(buf, cap, "%s", v);
    if (n <= 0 || (size_t)n >= cap) return -1;
    return 0;
}

int ca_session_dir(char *buf, size_t cap) {
    if (buf == NULL || cap == 0) return -1;
    char base[CA_SESSION_MAX_PATH];
#if defined(_WIN32)
    if (env_dir(base, sizeof(base), "APPDATA") != 0 &&
        env_dir(base, sizeof(base), "USERPROFILE") != 0) {
        return -1;
    }
    int n = snprintf(buf, cap, "%s%ccagent%csessions", base, CA_SEP, CA_SEP);
#else
    char xdg[CA_SESSION_MAX_PATH];
    if (env_dir(xdg, sizeof(xdg), "XDG_CONFIG_HOME") == 0) {
        int n = snprintf(base, sizeof(base), "%s", xdg);
        if (n <= 0 || (size_t)n >= sizeof(base)) return -1;
    } else if (env_dir(base, sizeof(base), "HOME") == 0) {
        size_t used = strlen(base);
        int n = snprintf(base + used, sizeof(base) - used, "/.config");
        if (n <= 0 || (size_t)n >= sizeof(base) - used) return -1;
    } else {
        return -1;
    }
    int n = snprintf(buf, cap, "%s/cagent/sessions", base);
#endif
    if (n <= 0 || (size_t)n >= cap) return -1;
    return 0;
}

int ca_session_dir_ensure(const char *dir) {
    if (dir == NULL) return -1;
    char work[CA_SESSION_MAX_PATH];
    int n = snprintf(work, sizeof(work), "%s", dir);
    if (n <= 0 || (size_t)n >= sizeof(work)) return -1;

    /* 逐级创建：只建最后一级会失败，因为父目录可能也不存在。 */
    for (char *p = work + 1; *p != '\0'; p++) {
        if (*p != CA_SEP && *p != '/') continue;
        char save = *p;
        *p = '\0';
        (void)CA_MKDIR(work); /* 已存在会失败，那不是错误 */
        *p = save;
    }
    if (CA_MKDIR(work) != 0) {
        /* 已存在也算成功：并发或重复调用都应幂等。
         * 用 S_ISDIR 而不是 S_IFDIR——后者要 _DEFAULT_SOURCE，
         * 在 CI 的 Linux 上（严格 C99 + 无扩展宏）根本不存在。 */
        struct stat st;
        if (stat(work, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- 保存 */

static void make_id(char *buf, size_t cap) {
    /* 时间 + 进程内序号：同秒内连续保存两次也不撞名。
     * 不用 localtime/strftime —— 它们在 MSVC 上会触发 C4996，
     * 而本仓库的 CI 把告警当失败。 */
    static unsigned seq = 0;
    unsigned long t = (unsigned long)time(NULL);
    seq++;
    snprintf(buf, cap, "s%08lx%03x", t, seq & 0xFFFu);
}

int ca_session_save(ca_session *s, const char *dir) {
    if (s == NULL || dir == NULL) return -1;
    if (ca_session_dir_ensure(dir) != 0) return -1;

    if (s->id[0] == '\0') make_id(s->id, sizeof(s->id));

    size_t need = 0;
    if (ca_session_to_json(s, NULL, 0, &need) != 0) return -1;
    char *doc = (char *)malloc(need);
    if (doc == NULL) return -1;
    if (ca_session_to_json(s, doc, need, NULL) != 0) { free(doc); return -1; }

    char name[CA_SESSION_MAX_ID + 8];
    snprintf(name, sizeof(name), "%s%s", s->id, CA_SESSION_SUFFIX);
    char path[CA_SESSION_MAX_PATH];
    char tmp[CA_SESSION_MAX_PATH];
    if (path_join(path, sizeof(path), dir, name) != 0 ||
        path_join(tmp, sizeof(tmp), dir, name) != 0) {
        free(doc);
        return -1;
    }

    /* 临时名 + 改名：中途失败留下的临时文件，不会顶掉一份好的会话。
     * 直接写目标文件的话，写坏时用户的会话就没了。 */
    size_t tmp_len = strlen(tmp);
    if (tmp_len + 5 < sizeof(tmp)) {
        memcpy(tmp + tmp_len, ".tmp", 5);
    } else {
        free(doc);
        return -1;
    }

    FILE *f = fopen(tmp, "wb");
    if (f == NULL) { free(doc); return -1; }
    size_t len = strlen(doc);
    size_t wrote = fwrite(doc, 1, len, f);
    int close_rc = fclose(f);
    free(doc);
    if (wrote != len || close_rc != 0) {
        remove(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- 读取 */

/* 把整个文件读进定长缓冲。超过上限返回 -2 而不是继续增长。 */
static int read_file_capped(const char *path, char *buf, size_t cap,
                            size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    int extra = 0;
    if (n == cap - 1) {
        /* 可能还有更多：探一个字节，确认是「刚好装满」还是「被截断」 */
        extra = (fgetc(f) != EOF);
    }
    fclose(f);
    buf[n] = '\0';
    *out_len = n;
    return extra ? -2 : 0;
}

int ca_session_load(ca_session *s, const char *path) {
    if (s == NULL || path == NULL) return -1;
    size_t cap = s->cap > 0 ? s->cap : CA_SESSION_FILE_MAX;
    if (cap < 1024) cap = 1024;

    /* 文件按**预算**量级读取，而不是按「文件多大就读多大」。
     * 会话文件是外部输入，可以被放大到任意尺寸。 */
    char *text = (char *)malloc(cap);
    if (text == NULL) return -1;
    size_t len = 0;
    int rr = read_file_capped(path, text, cap, &len);
    if (rr == -1) { free(text); return -1; }

    /* 载入前清空：不然会被解读成「在已有历史上追加」。 */
    s->count = 0;
    s->used = 0;
    s->buf[0] = '\0';
    int rc = ca_session_from_json(s, text, len);
    free(text);
    if (rc == -1) return -1;
    if (rc == -2 || rr == -2) return -2;
    return 0;
}

int ca_session_path_for(const char *dir, const char *id, char *buf, size_t cap) {
    if (dir == NULL || id == NULL || buf == NULL || id[0] == '\0') return -1;
    /* id 由我们自己生成，但 main 会把它直接透传进来：
     * 带分隔符（或 ".."）就是试图越出会话目录，一律拒绝。 */
    if (strchr(id, '/') != NULL || strchr(id, CA_SEP) != NULL ||
        strstr(id, "..") != NULL) {
        return -1;
    }
    char name[CA_SESSION_MAX_ID + 8];
    if (snprintf(name, sizeof(name), "%s%s", id, CA_SESSION_SUFFIX) <= 0) return -1;
    return path_join(buf, cap, dir, name);
}

int ca_session_delete(const char *dir, const char *id) {
    char path[CA_SESSION_MAX_PATH];
    if (ca_session_path_for(dir, id, path, sizeof(path)) != 0) return -1;
    return remove(path) == 0 ? 0 : -1;
}

/* ---------------------------------------------------------------- 列表 */

typedef struct {
    char name[CA_SESSION_MAX_PATH];
    long long mtime;
    long long bytes;
} store_ent;

static int has_json_suffix(const char *name) {
    size_t n = strlen(name);
    size_t s = strlen(CA_SESSION_SUFFIX);
    return n > s && strcmp(name + n - s, CA_SESSION_SUFFIX) == 0;
}

static int ent_cmp_desc(const void *a, const void *b) {
    const store_ent *x = (const store_ent *)a;
    const store_ent *y = (const store_ent *)b;
    if (x->mtime != y->mtime) return (x->mtime < y->mtime) ? 1 : -1; /* 新的在前 */
    return strcmp(y->name, x->name);
}

int ca_session_list(const char *dir, ca_session_info *out, size_t cap,
                    size_t *count) {
    if (count != NULL) *count = 0;
    if (dir == NULL || out == NULL) return -1;

    store_ent *ents = (store_ent *)calloc(cap, sizeof(*ents));
    if (ents == NULL) return -1;
    size_t n = 0;

#if defined(_WIN32)
    char pattern[CA_SESSION_MAX_PATH];
    if (snprintf(pattern, sizeof(pattern), "%s%c*", dir, CA_SEP) <= 0) {
        free(ents);
        return -1;
    }
    struct _finddata_t fd;
    intptr_t h = _findfirst(pattern, &fd);
    if (h == -1) { free(ents); return -1; } /* 目录不存在 */
    do {
        if ((fd.attrib & _A_SUBDIR) == 0 && has_json_suffix(fd.name) && n < cap) {
            snprintf(ents[n].name, sizeof(ents[n].name), "%s", fd.name);
            ents[n].mtime = (long long)fd.time_write;
            ents[n].bytes = (long long)fd.size;
            n++;
        }
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    DIR *d = opendir(dir);
    if (d == NULL) { free(ents); return -1; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL && n < cap) {
        if (!has_json_suffix(de->d_name)) continue;
        char path[CA_SESSION_MAX_PATH];
        if (path_join(path, sizeof(path), dir, de->d_name) != 0) continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;
        snprintf(ents[n].name, sizeof(ents[n].name), "%s", de->d_name);
        ents[n].mtime = (long long)st.st_mtime;
        ents[n].bytes = (long long)st.st_size;
        n++;
    }
    closedir(d);
#endif

    qsort(ents, n, sizeof(*ents), ent_cmp_desc);

    /* 逐个校验内容：损坏文件必须**在列表里**被标出来。
     * 悄悄跳过会让用户看到「会话不见了」，而原因其实可查。
     * 校验用一个复用的临时会话，于是额外的内存是常数份。 */
    ca_session scratch;
    int have_scratch = (ca_session_init(&scratch, 0) == 0);

    size_t written = 0;
    for (size_t i = 0; i < n; i++) {
        ca_session_info *info = &out[written];
        memset(info, 0, sizeof(*info));
        snprintf(info->id, sizeof(info->id), "%s", ents[i].name);
        char *dot = strrchr(info->id, '.');
        if (dot != NULL) *dot = '\0';
        if (path_join(info->path, sizeof(info->path), dir, ents[i].name) != 0) {
            snprintf(info->path, sizeof(info->path), "%s", ents[i].name);
        }
        info->mtime = ents[i].mtime;
        info->bytes = ents[i].bytes;

        if (have_scratch) {
            int rc = ca_session_load(&scratch, info->path);
            if (rc == -1) info->corrupt = 1;
            else info->msgs = (int)ca_session_count(&scratch);
        }
        written++;
    }
    if (have_scratch) ca_session_free(&scratch);
    free(ents);

    if (count != NULL) *count = written;
    return (int)written;
}
