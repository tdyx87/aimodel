/* json.c —— 零分配的 JSON 游标扫描器
 * 契约：docs/design/contract.md §C6
 */

#include "cagent/json.h"

#include <string.h>

void ca_json_init(ca_json_cursor *c, const char *text, size_t len) {
    if (c == NULL) return;
    c->s = text;
    c->len = text ? len : 0;
    c->pos = 0;
}

void ca_json_skip_ws(ca_json_cursor *c) {
    if (c == NULL) return;
    while (c->pos < c->len) {
        char ch = c->s[c->pos];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            c->pos++;
        } else {
            break;
        }
    }
}

char ca_json_peek(const ca_json_cursor *c) {
    if (c == NULL || c->pos >= c->len) return '\0';
    return c->s[c->pos];
}

static void skip_string(ca_json_cursor *c) {
    if (ca_json_peek(c) != '"') return;
    c->pos++; /* 跳过起始引号 */
    while (c->pos < c->len) {
        char ch = c->s[c->pos];
        if (ch == '\\') {
            c->pos += 2; /* 反斜杠 + 被转义的字符 */
            continue;
        }
        c->pos++;
        if (ch == '"') return;
    }
}

int ca_json_skip_value(ca_json_cursor *c) {
    if (c == NULL) return -1;
    ca_json_skip_ws(c);
    if (c->pos >= c->len) return -1;
    char ch = c->s[c->pos];

    if (ch == '"') {
        size_t before = c->pos;
        skip_string(c);
        return (c->pos > before) ? 0 : -1;
    }
    if (ch == '{' || ch == '[') {
        /* 必须同时跟踪**两种**括号。早先只跟踪起始那一种，
         * 于是 `[{"a":[1]}]` 会在内层的 ']' 提前返回，游标错位，
         * 后续解析随之错乱（表现为崩溃而不是解析失败）。
         * 字符串内的括号已被 skip_string 排除，不受此影响。 */
        int depth = 0;
        while (c->pos < c->len) {
            char cur = c->s[c->pos];
            if (cur == '"') {
                skip_string(c);
                continue;
            }
            if (cur == '{' || cur == '[') {
                depth++;
            } else if (cur == '}' || cur == ']') {
                depth--;
                if (depth == 0) {
                    c->pos++;
                    return 0;
                }
            }
            c->pos++;
        }
        return -1; /* 未闭合 */
    }
    /* 数字 / true / false / null：扫到下一个分隔符为止 */
    while (c->pos < c->len) {
        char cur = c->s[c->pos];
        if (cur == ',' || cur == '}' || cur == ']' ||
            cur == ' ' || cur == '\t' || cur == '\n' || cur == '\r') {
            break;
        }
        c->pos++;
    }
    return 0;
}

int ca_json_enter_object(ca_json_cursor *c) {
    if (c == NULL) return -1;
    ca_json_skip_ws(c);
    if (ca_json_peek(c) != '{') return -1;
    c->pos++;
    return 0;
}

int ca_json_enter_array(ca_json_cursor *c) {
    if (c == NULL) return -1;
    ca_json_skip_ws(c);
    if (ca_json_peek(c) != '[') return -1;
    c->pos++;
    return 0;
}

int ca_json_field(ca_json_cursor *c, const char *name, ca_json_cursor *out) {
    if (c == NULL || name == NULL) return -1;
    size_t name_len = strlen(name);
    /* 操作对象是**已进入**的对象（调用方已调 ca_json_enter_object）。
     * 早先在这里又检查了一遍 '{'，与 enter_object 的前置条件冲突，
     * 于是第一次查找就返回 -1。 */
    ca_json_cursor work = *c;

    for (;;) {
        ca_json_skip_ws(&work);
        if (work.pos >= work.len) return 0; /* 到头：没有该字段 */
        if (ca_json_peek(&work) == '}') return 0;
        if (ca_json_peek(&work) != '"') return -1;

        size_t key_begin = work.pos + 1;
        skip_string(&work);
        size_t key_len = (work.pos > key_begin + 1)
                             ? (work.pos - 1 - key_begin)
                             : 0;
        /* 键里可能有转义；本工具的字段名都是 ASCII 且无转义，
         * 含转义就当作不匹配（而不是错误），避免实现半套转义解码。 */
        int match = (key_len == name_len) &&
                    (memcmp(work.s + key_begin, name, name_len) == 0);

        ca_json_skip_ws(&work);
        if (ca_json_peek(&work) != ':') return -1;
        work.pos++;
        ca_json_skip_ws(&work);

        if (match) {
            if (out != NULL) *out = work;
            return 1;
        }
        if (ca_json_skip_value(&work) != 0) return -1;
        ca_json_skip_ws(&work);
        if (ca_json_peek(&work) == ',') {
            work.pos++;
            continue;
        }
        if (ca_json_peek(&work) == '}') return 0;
        return -1;
    }
}

int ca_json_index(ca_json_cursor *c, size_t index, ca_json_cursor *out) {
    if (c == NULL) return -1;
    /* 同样是**已进入**的数组，不重复检查 '[' */
    size_t i = 0;
    ca_json_cursor work = *c;
    for (;;) {
        ca_json_skip_ws(&work);
        if (work.pos >= work.len) return 0;
        if (ca_json_peek(&work) == ']') return 0;
        if (i == index) {
            if (out != NULL) *out = work;
            return 1;
        }
        if (ca_json_skip_value(&work) != 0) return -1;
        i++;
        ca_json_skip_ws(&work);
        if (ca_json_peek(&work) == ',') {
            work.pos++;
            continue;
        }
        if (ca_json_peek(&work) == ']') return 0;
        return -1;
    }
}

long ca_json_array_count(ca_json_cursor *c) {
    if (c == NULL) return -1;
    /* 与 field/index 一致：操作**已进入**的数组。
     * 早先在这里调 enter_array，与调用方的用法冲突。 */
    long count = 0;
    ca_json_cursor work = *c;
    for (;;) {
        ca_json_skip_ws(&work);
        if (work.pos >= work.len) break;
        if (ca_json_peek(&work) == ']') break;
        if (ca_json_skip_value(&work) != 0) return -1;
        count++;
        ca_json_skip_ws(&work);
        if (ca_json_peek(&work) == ',') {
            work.pos++;
            continue;
        }
        if (ca_json_peek(&work) == ']') break;
        return -1;
    }
    return count;
}

int ca_json_read_string(ca_json_cursor *c, char *buf, size_t cap, size_t *out_len) {
    if (c == NULL || buf == NULL) return -3;
    ca_json_skip_ws(c);
    if (ca_json_peek(c) != '"') return -1;

    size_t begin = c->pos + 1;
    size_t p = begin;
    /* 第一遍：算解码后的长度，装不下就直接拒绝（不截断） */
    size_t need = 0;
    while (p < c->len) {
        char ch = c->s[p];
        if (ch == '\\') {
            if (p + 1 >= c->len) return -3;
            char esc = c->s[p + 1];
            switch (esc) {
                case '"': case '\\': case '/': case 'b':
                case 'f': case 'n': case 'r': case 't':
                    need += 1;
                    break;
                case 'u': {
                    if (p + 5 >= c->len) return -3;
                    /* \uXXXX：先按 UTF-8 编码后的宽度估算 */
                    unsigned code = 0;
                    for (int k = 0; k < 4; k++) {
                        char h = c->s[p + 2 + k];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= (unsigned)(h - 'A' + 10);
                        else return -3;
                    }
                    if (code < 0x80) need += 1;
                    else if (code < 0x800) need += 2;
                    else need += 3;   /* 不处理代理对：LLM 文本不产出它 */
                    p += 4;
                    break;
                }
                default: return -3;
            }
            p += 2;
            continue;
        }
        if (ch == '"') break;
        need += 1;
        p++;
    }
    if (p >= c->len) return -3; /* 未闭合 */
    if (need + 1 > cap) return -2;

    /* 第二遍：解码写入 */
    size_t q = begin;
    size_t w = 0;
    while (q < p) {
        char ch = c->s[q];
        if (ch != '\\') {
            buf[w++] = ch;
            q++;
            continue;
        }
        char esc = c->s[q + 1];
        switch (esc) {
            case '"': buf[w++] = '"'; q += 2; break;
            case '\\': buf[w++] = '\\'; q += 2; break;
            case '/': buf[w++] = '/'; q += 2; break;
            case 'b': buf[w++] = '\b'; q += 2; break;
            case 'f': buf[w++] = '\f'; q += 2; break;
            case 'n': buf[w++] = '\n'; q += 2; break;
            case 'r': buf[w++] = '\r'; q += 2; break;
            case 't': buf[w++] = '\t'; q += 2; break;
            case 'u': {
                unsigned code = 0;
                for (int k = 0; k < 4; k++) {
                    char h = c->s[q + 2 + k];
                    code <<= 4;
                    if (h >= '0' && h <= '9') code |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') code |= (unsigned)(h - 'a' + 10);
                    else code |= (unsigned)(h - 'A' + 10);
                }
                q += 6;
                if (code < 0x80) {
                    buf[w++] = (char)code;
                } else if (code < 0x800) {
                    buf[w++] = (char)(0xC0 | (code >> 6));
                    buf[w++] = (char)(0x80 | (code & 0x3F));
                } else {
                    buf[w++] = (char)(0xE0 | (code >> 12));
                    buf[w++] = (char)(0x80 | ((code >> 6) & 0x3F));
                    buf[w++] = (char)(0x80 | (code & 0x3F));
                }
                break;
            }
            default: return -3;
        }
    }
    buf[w] = '\0';
    if (out_len != NULL) *out_len = w;
    c->pos = p + 1; /* 跳过结束引号 */
    return 0;
}

int ca_json_append_string(ca_json_cursor *c, char *dst, size_t cap, size_t *len) {
    if (c == NULL || dst == NULL || len == NULL) return -3;
    ca_json_skip_ws(c);
    if (ca_json_peek(c) != '"') return -1;

    size_t w = *len;
    size_t p = c->pos + 1;
    int overflow = 0;

    while (p < c->len) {
        unsigned char ch = (unsigned char)c->s[p];
        if (ch == '"') {
            /* 正常收尾。超限时仍返回 -2，但已写入部分保留。 */
            if (cap > 0) dst[w < cap ? w : cap - 1] = '\0';
            c->pos = p + 1;
            *len = w;
            return overflow ? -2 : 0;
        }
        if (ch == '\\') {
            if (p + 1 >= c->len) return -3;
            unsigned char esc = (unsigned char)c->s[p + 1];
            char out_buf[4];
            size_t out_len = 0;
            if (esc == '"') { out_buf[0] = '"'; out_len = 1; p += 2; }
            else if (esc == '\\') { out_buf[0] = '\\'; out_len = 1; p += 2; }
            else if (esc == '/') { out_buf[0] = '/'; out_len = 1; p += 2; }
            else if (esc == 'b') { out_buf[0] = '\b'; out_len = 1; p += 2; }
            else if (esc == 'f') { out_buf[0] = '\f'; out_len = 1; p += 2; }
            else if (esc == 'n') { out_buf[0] = '\n'; out_len = 1; p += 2; }
            else if (esc == 'r') { out_buf[0] = '\r'; out_len = 1; p += 2; }
            else if (esc == 't') { out_buf[0] = '\t'; out_len = 1; p += 2; }
            else if (esc == 'u') {
                if (p + 5 >= c->len) return -3;
                unsigned code = 0;
                for (int k = 0; k < 4; k++) {
                    char h = c->s[p + 2 + k];
                    code <<= 4;
                    if (h >= '0' && h <= '9') code |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') code |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') code |= (unsigned)(h - 'A' + 10);
                    else return -3;
                }
                p += 6;
                if (code < 0x80) {
                    out_buf[0] = (char)code;
                    out_len = 1;
                } else if (code < 0x800) {
                    out_buf[0] = (char)(0xC0 | (code >> 6));
                    out_buf[1] = (char)(0x80 | (code & 0x3F));
                    out_len = 2;
                } else {
                    out_buf[0] = (char)(0xE0 | (code >> 12));
                    out_buf[1] = (char)(0x80 | ((code >> 6) & 0x3F));
                    out_buf[2] = (char)(0x80 | (code & 0x3F));
                    out_len = 3;
                }
            } else {
                return -3;
            }
            for (size_t i = 0; i < out_len; i++) {
                if (w + 1 >= cap) { overflow = 1; break; }
                dst[w++] = out_buf[i];
            }
            continue;
        }
        /* 普通字符（含 UTF-8 多字节，逐字节透传） */
        if (w + 1 >= cap) { overflow = 1; p++; continue; }
        dst[w++] = (char)ch;
        p++;
    }
    *len = w;
    return -3; /* 未闭合 */
}

int ca_json_read_int(ca_json_cursor *c, long long *out) {
    if (c == NULL || out == NULL) return -2;
    ca_json_skip_ws(c);
    size_t start = c->pos;
    long long sign = 1;
    /* 负号只处理一次。早先写成"先跳过 '-'，再判断一次 '-'"，
     * 结果负号被吃掉两次，负数变成正数——而 token 用量等字段
     * 出现负值时会静默出错。 */
    if (c->pos < c->len && c->s[c->pos] == '-') {
        sign = -1;
        c->pos++;
    }
    if (c->pos >= c->len || c->s[c->pos] < '0' || c->s[c->pos] > '9') {
        c->pos = start; /* 类型不符：还原游标，别让调用方拿到半截位置 */
        return -1;
    }
    long long value = 0;
    while (c->pos < c->len && c->s[c->pos] >= '0' && c->s[c->pos] <= '9') {
        value = value * 10 + (c->s[c->pos] - '0');
        c->pos++;
    }
    if (c->pos == start) return -1;
    *out = sign * value;
    return 0;
}