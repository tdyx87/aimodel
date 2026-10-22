/* session.c —— 会话历史（契约 §6 / §2.1）
 *
 * 本文件只做**内存中的历史**：字节封顶、按轮次淘汰、明示丢弃、JSON 读写。
 * 落盘与目录扫描在 session_store.c —— 那里才有平台差异，
 * 刻意与这里分开，免得上层逻辑被 #ifdef _WIN32 淹没。
 *
 * 核心不变量：`used <= cap` 永远成立。任何会破坏它的路径都必须先淘汰或先拒绝。
 */

#include "cagent/session.h"

#include <stdlib.h>
#include <string.h>

#include "cagent/json.h"

static unsigned role_from_name(const char *name) {
    if (name != NULL) {
        if (strcmp(name, "assistant") == 0) return CA_MSG_ASSISTANT;
        if (strcmp(name, "system") == 0) return CA_MSG_SYSTEM;
    }
    return CA_MSG_USER;
}

static const char *role_to_name(unsigned role) {
    switch (role) {
        case CA_MSG_ASSISTANT: return "assistant";
        case CA_MSG_SYSTEM: return "system";
        case CA_MSG_USER:
        default: return "user";
    }
}

int ca_session_init(ca_session *s, size_t budget_bytes) {
    if (s == NULL) return -1;
    memset(s, 0, sizeof(*s));
    if (budget_bytes == 0) budget_bytes = CA_LIMIT_HISTORY_BYTES;
    /* 太小到装不下一条消息的预算没有意义，反而让淘汰逻辑失去腾挪空间。 */
    if (budget_bytes < 1024) budget_bytes = 1024;
    s->buf = (char *)malloc(budget_bytes);
    if (s->buf == NULL) return -2;
    s->buf[0] = '\0';
    s->cap = budget_bytes;
    return 0;
}

void ca_session_free(ca_session *s) {
    if (s == NULL) return;
    free(s->buf);
    s->buf = NULL;
    s->cap = 0;
    s->used = 0;
    s->count = 0;
}

size_t ca_session_count(const ca_session *s) { return s ? s->count : 0; }
size_t ca_session_used(const ca_session *s) { return s ? s->used : 0; }
size_t ca_session_budget(const ca_session *s) { return s ? s->cap : 0; }

const char *ca_session_text(const ca_session *s, size_t i) {
    if (s == NULL || i >= s->count) return NULL;
    return s->buf + s->recs[i].off;
}

unsigned ca_session_role(const ca_session *s, size_t i) {
    if (s == NULL || i >= s->count) return CA_MSG_USER;
    return s->recs[i].role;
}

/* 一轮 = 从一条 user 消息起，到**下一条 user 消息之前**。
 * 淘汰的粒度必须是整轮：把一轮拆开淘汰，模型会看到孤立的助手回复，
 * 那比少一轮更糟——它会以为用户说过没说过的话。 */
static size_t session_turn_end(const ca_session *s) {
    for (size_t i = 1; i < s->count; i++) {
        if (s->recs[i].role == CA_MSG_USER) return i;
    }
    return s->count;
}

/* 删除 recs[from, to) 并把剩余文本前移、修正偏移。 */
static void session_compact(ca_session *s, size_t from, size_t to) {
    size_t start_off = s->recs[from].off;
    size_t end_off = (to < s->count) ? s->recs[to].off : s->used;
    size_t shift = end_off - start_off;
    size_t n = to - from;

    memmove(s->buf + start_off, s->buf + end_off, s->used - end_off);
    for (size_t i = to; i < s->count; i++) {
        s->recs[i - n] = s->recs[i];
        s->recs[i - n].off -= shift;
    }
    s->count -= n;
    s->used -= shift;
}

/* 腾出 need 字节。protect_last=1 时绝不删除最后一条
 * （流式追加时，正在写的那条就是最后一条）。 */
static void session_make_room(ca_session *s, size_t need, int protect_last) {
    while (s->used + need > s->cap || s->count >= CA_SESSION_MAX_MSGS) {
        size_t k = session_turn_end(s);
        if (k == 0) break;                              /* 已空 */
        if (protect_last && k >= s->count) break;       /* 只剩当前这一轮，不能再删 */
        size_t before = s->used;
        session_compact(s, 0, k);
        s->last_evicted_msgs += (int)k;
        s->last_evicted_bytes += before - s->used;
        s->evicted_msgs += k;
        s->evicted_bytes += before - s->used;
    }
}

static void session_reset_notice(ca_session *s) {
    s->last_evicted_msgs = 0;
    s->last_evicted_bytes = 0;
    s->last_truncated_msgs = 0;
    s->last_truncated_bytes = 0;
}

int ca_session_add(ca_session *s, unsigned role, const char *text) {
    if (s == NULL || text == NULL || s->buf == NULL) return -1;
    size_t n = strlen(text);
    /* 空消息不进历史：白占预算，还会让模型看到一行空内容。 */
    if (n == 0) return -2;

    session_reset_notice(s);

    size_t need = n + 1; /* 含结尾 NUL */
    session_make_room(s, need, 0);

    /* 淘汰完仍装不下 = 单条消息本身就超过整个预算。
     * 截断，但**记下截掉了多少**，由 UI 说出来。 */
    size_t keep = n;
    if (keep + 1 > s->cap) {
        keep = (s->cap > 1) ? (s->cap - 1) : 0;
        s->last_truncated_msgs += 1;
        s->last_truncated_bytes += n - keep;
    }

    memcpy(s->buf + s->used, text, keep);
    s->buf[s->used + keep] = '\0';
    s->recs[s->count].role = role;
    s->recs[s->count].off = s->used;
    s->recs[s->count].len = keep;
    s->count++;
    s->used += keep + 1;
    return 0;
}

int ca_session_append_delta(ca_session *s, const char *text) {
    if (s == NULL || text == NULL || s->buf == NULL) return -1;
    size_t n = strlen(text);
    if (n == 0) return -2;

    session_reset_notice(s);

    if (s->count == 0 || s->recs[s->count - 1].role != CA_MSG_ASSISTANT) {
        int rc = ca_session_add(s, CA_MSG_ASSISTANT, text);
        return rc;
    }

    /* 末尾就是助手消息：接上去。它的文本一定紧贴 used 的尾部
     * （它就是最后一条），所以覆盖掉它结尾的 NUL 即可，不必搬数据。 */
    ca_session_rec *r = &s->recs[s->count - 1];
    size_t tail = r->off + r->len; /* 指向它结尾的 NUL */

    session_make_room(s, n, 1);

    size_t keep = n;
    /* 必须给结尾的 NUL 留一格：本条的存储是 [off, tail+keep] 含 NUL，
     * 于是 keep <= cap - tail - 1。少减这个 1，NUL 会写到 buf[cap] 上
     * （越界 1 字节），且 used 会变成 cap+1 —— 破坏 used<=cap，
     * 而症状只是静默的堆损坏。 */
    if (tail + n + 1 > s->cap) {
        keep = (s->cap > tail + 1) ? (s->cap - tail - 1) : 0;
        s->last_truncated_msgs += 1;
        s->last_truncated_bytes += n - keep;
    }
    memcpy(s->buf + tail, text, keep);
    s->buf[tail + keep] = '\0';
    r->len += keep;
    s->used = tail + keep + 1;
    return 0;
}

int ca_session_take_notice(ca_session *s, int *evicted_msgs,
                           size_t *evicted_bytes, int *truncated_msgs,
                           size_t *truncated_bytes) {
    if (s == NULL) return 0;
    int any = 0;
    if (evicted_msgs != NULL) *evicted_msgs = s->last_evicted_msgs;
    if (evicted_bytes != NULL) *evicted_bytes = s->last_evicted_bytes;
    if (truncated_msgs != NULL) *truncated_msgs = s->last_truncated_msgs;
    if (truncated_bytes != NULL) *truncated_bytes = s->last_truncated_bytes;
    if (s->last_evicted_msgs > 0 || s->last_truncated_msgs > 0) any = 1;
    session_reset_notice(s);
    return any;
}

void ca_session_export(const ca_session *s, ca_llm_msg *out, size_t *out_count) {
    if (out == NULL || out_count == NULL) return;
    *out_count = 0;
    if (s == NULL) return;
    for (size_t i = 0; i < s->count; i++) {
        out[i].role = (ca_msg_role)s->recs[i].role;
        out[i].text = s->buf + s->recs[i].off;
        /* 历史消息**绝不**携带工具握手字段。会话层不保留工具回合
         * （契约 §2.5.6），把它俩显式置 NULL 是必须的：结构体多了
         * 字段而生产者没更新，序列化器就会读到未初始化的指针并按
         * 「非空 = 是 tool_calls」去解引用——一个立刻段错误的坑。 */
        out[i].tool_calls_json = NULL;
        out[i].tool_call_id = NULL;
    }
    *out_count = s->count;
}

/* ---------------------------------------------------------------- JSON */

/* 渲染成 JSON。dst 为 NULL 时只计量（用于两遍构造）。
 * 返回 0 成功，-1 容量不足。 */
static int session_render(const ca_session *s, char *dst, size_t cap, size_t *len) {
    if (ca_json_write_raw(dst, cap, len, "{\"v\":1,\"id\":") != 0) return -1;
    if (ca_json_write_string(dst, cap, len, s->id) != 0) return -1;
    if (ca_json_write_raw(dst, cap, len, ",\"messages\":[") != 0) return -1;
    for (size_t i = 0; i < s->count; i++) {
        if (i > 0 && ca_json_write_raw(dst, cap, len, ",") != 0) return -1;
        if (ca_json_write_raw(dst, cap, len, "{\"role\":") != 0) return -1;
        if (ca_json_write_string(dst, cap, len, role_to_name(s->recs[i].role)) != 0) return -1;
        if (ca_json_write_raw(dst, cap, len, ",\"content\":") != 0) return -1;
        if (ca_json_write_string(dst, cap, len, s->buf + s->recs[i].off) != 0) return -1;
        if (ca_json_write_raw(dst, cap, len, "}") != 0) return -1;
    }
    if (ca_json_write_raw(dst, cap, len, "]}") != 0) return -1;
    return 0;
}

int ca_session_to_json(const ca_session *s, char *buf, size_t cap,
                       size_t *need_len) {
    if (s == NULL) return -1;

    /* 先只计量，再决定写不写：容量不足时一个字节都不碰 buf。
     * 早先写成「两遍循环 + 在只计量那遍里也判容量」，
     * 于是 buf=NULL, cap=0（标准的「先问要多长」用法）永远返回 -2——
     * 只计量这一遍本来就不该有容量概念。 */
    size_t need = 0;
    if (session_render(s, NULL, 0, &need) != 0) return -1;
    if (need_len != NULL) *need_len = need + 1; /* 含 NUL */
    if (buf == NULL) return 0;
    if (need + 1 > cap) return -2;

    size_t len = 0;
    if (session_render(s, buf, cap, &len) != 0) return -2;
    buf[len] = '\0';
    return 0;
}

int ca_session_from_json(ca_session *s, const char *text, size_t len) {
    if (s == NULL || text == NULL || s->buf == NULL) return -1;
    session_reset_notice(s);

    ca_json_cursor root;
    ca_json_init(&root, text, len);
    ca_json_skip_ws(&root);
    if (ca_json_enter_object(&root) != 0) return -1;

    /* id 可选：缺了不影响历史本身，坏文件判定不靠它。 */
    ca_json_cursor idv;
    if (ca_json_field(&root, "id", &idv) == 1) {
        (void)ca_json_read_string(&idv, s->id, sizeof(s->id), NULL);
    }

    ca_json_cursor mv;
    if (ca_json_field(&root, "messages", &mv) != 1) return -1;

    ca_json_cursor marr = mv;
    if (ca_json_enter_array(&marr) != 0) return -1;
    long n = ca_json_array_count(&marr);
    if (n < 0) return -1;

    /* 内容按预算读取。临时缓冲与预算同量级，因此整体内存
     * 仍是「历史预算 + 一份临时」，不随文件大小增长。 */
    char *tmp = (char *)malloc(s->cap);
    if (tmp == NULL) return -1;
    int truncated_msgs = 0;

    for (long i = 0; i < n; i++) {
        ca_json_cursor el;
        if (ca_json_index(&marr, (size_t)i, &el) != 1) { free(tmp); return -1; }
        ca_json_cursor eo = el;
        if (ca_json_enter_object(&eo) != 0) { free(tmp); return -1; }

        char role[16];
        ca_json_cursor rv;
        if (ca_json_field(&eo, "role", &rv) != 1) { free(tmp); return -1; }
        if (ca_json_read_string(&rv, role, sizeof(role), NULL) != 0) { free(tmp); return -1; }

        ca_json_cursor cv;
        if (ca_json_field(&eo, "content", &cv) != 1) { free(tmp); return -1; }
        size_t clen = 0;
        int rr = ca_json_read_string(&cv, tmp, s->cap, &clen);
        if (rr == -2) {
            /* 单条超过预算：截断载入并记下条数，而不是拒绝整个会话。
             * 一个坏掉的超长消息不该让用户丢掉整个会话。 */
            truncated_msgs++;
            size_t keep = s->cap > 1 ? s->cap - 1 : 0;
            tmp[keep] = '\0';
        } else if (rr != 0) {
            free(tmp);
            return -1;
        }
        if (ca_session_add(s, role_from_name(role), tmp) == -1) {
            free(tmp);
            return -1;
        }
    }
    free(tmp);
    /* add() 每次都会重置 notice，所以截断条数在循环之后统一补记。 */
    if (truncated_msgs > 0) s->last_truncated_msgs += truncated_msgs;
    return truncated_msgs > 0 ? -2 : 0;
}
