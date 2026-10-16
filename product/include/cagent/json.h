/* json.h —— 零分配的 JSON 游标扫描器
 * 契约：docs/design/contract.md §C6 / §3
 *
 * 为什么不写「解析成树再取字段」的常规做法：
 *   DOM 树需要按输入大小动态分配，等于让内存随响应增长——
 *   而那正是本产品要杜绝的。第三方解析器几乎都自带这种 arena。
 *
 * 本文件的做法：游标在原始文本上移动，**不建树、不复制**。
 * 需要字符串值时，由调用方提供定长缓冲；装不下就**报错**，不截断。
 *
 * 路径访问用「逐层下降」表达，不做通用路径解析——
 * 省掉一个字符串解析器，也省掉一类难查的转义 bug。
 */

#ifndef CAGENT_JSON_H
#define CAGENT_JSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *s;
    size_t len;
    size_t pos;
} ca_json_cursor;

/* 初始化游标到整段文本。 */
void ca_json_init(ca_json_cursor *c, const char *text, size_t len);

/* 跳过空白与换行。 */
void ca_json_skip_ws(ca_json_cursor *c);

/* 当前字符；越界返回 '\0'。 */
char ca_json_peek(const ca_json_cursor *c);

/* 跳过**一个**完整的 JSON 值（对象 / 数组 / 字符串 / 数字 / 字面量）。
 * 返回 0 成功；-1 语法错误或越界。
 */
int ca_json_skip_value(ca_json_cursor *c);

/* 期望一个对象起始 '{' 并进入其中。返回值直接作为 ca_json_cursor 使用。
 * 返回 0 成功，-1 不是对象。
 */
int ca_json_enter_object(ca_json_cursor *c);

/* 期望数组起始 '[' 并进入其中。 */
int ca_json_enter_array(ca_json_cursor *c);

/* 游标约定：field / index / array_count 都操作**已经进入**的容器，
 * 即调用方已经调用过 ca_json_enter_object / ca_json_enter_array，
 * 游标正停在容器内容的第一个字符。
 *
 * 早先这几个函数自己又检查了一遍 '{' / '['，与 enter_* 的前置条件冲突，
 * 导致第一次查找就失败。这个约定必须统一，否则每个调用点都得猜。 */

/* 在已进入的对象里查找名为 name 的字段。
 * 成功时 out 被定位到该字段的值，返回 1；没有该字段返回 0；
 * 语法错误返回 -1。out 与 c 可为同一个游标（就地下降）。
 */
int ca_json_field(ca_json_cursor *c, const char *name, ca_json_cursor *out);

/* 在已进入的数组里取第 index 个元素（0 基）。
 * 成功时 out 被定位到该元素，返回 1；越界返回 0；语法错误返回 -1。
 */
int ca_json_index(ca_json_cursor *c, size_t index, ca_json_cursor *out);

/* 读字符串值到定长缓冲。
 * 返回  0 成功；
 *       -1 值不是字符串；
 *       -2 缓冲区不足（**不截断**，buf 未被修改）；
 *       -3 语法错误。
 * out_len 可为 NULL。
 */
int ca_json_read_string(ca_json_cursor *c, char *buf, size_t cap, size_t *out_len);

/* 把字符串值**流式**追加到定长缓冲（不经过临时缓冲）。
 *
 * 为什么需要它：工具调用的 arguments 是分片到达的，单片可能远大于
 * 一个栈上临时缓冲（一次 read_file 的参数轻易上千字节）。
 * 若经由临时缓冲读，装不下就会被误判成"参数超限"，
 * 从而拒绝本来合法的调用。
 *
 * 返回  0 成功（*len 已更新）；
 *      -1 值不是字符串；
 *      -2 容量不足（**不截断**：已写入的部分保留，*len 为已写长度，
 *         调用方据此判定 overflow 并拒绝执行）；
 *      -3 语法错误。
 */
int ca_json_append_string(ca_json_cursor *c, char *dst, size_t cap, size_t *len);

/* 读整数值。返回 0 成功，-1 类型不符或溢出，-2 语法错误。 */
int ca_json_read_int(ca_json_cursor *c, long long *out);

/* 在已进入的数组里取元素个数（用于遍历）。不改变原游标。
 * 返回元素个数；出错返回 -1。 */
long ca_json_array_count(ca_json_cursor *c);

#ifdef __cplusplus
}
#endif

#endif /* CAGENT_JSON_H */