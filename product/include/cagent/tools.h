/* tools.h —— 工具层
 * 契约：docs/design/contract.md §5
 *
 * 三条贯穿全层的硬约束：
 *   P1 默认全部拒绝（P4 的路径逃逸检查在此层，而非只在调用方）
 *   P4 路径必须规范化后落在工作区内，.. 与符号链接指向外部一律拒绝
 *   工具结果必须携带来源元信息（路径 / 行范围 / 是否截断）
 *
 * 为什么结果元信息是强制的：模型需要知道内容是否完整。
 * 不带「已截断」标记的结果会让模型基于残缺数据下结论——
 * 它会以为整个文件都读完了。
 */

#ifndef CAGENT_TOOLS_H
#define CAGENT_TOOLS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 分层预算（契约 §2.1）。工具结果进对话前必过此上限。 */
#define CA_TOOL_RESULT_LIMIT (1u * 1024u * 1024u)
#define CA_FILE_READ_LIMIT (4u * 1024u * 1024u)

/* ------------------------------------------------------------ 工作区与路径 */

typedef struct ca_workspace ca_workspace;

/* 打开工作区。root 会被规范化后记录。失败返回 NULL。 */
ca_workspace *ca_ws_open(const char *root);
void ca_ws_free(ca_workspace *ws);

/* 把输入路径解析为工作区内的绝对路径（写入 out）。
 *
 * 返回  0 成功，out 为绝对路径；
 *      -1 路径逃逸出工作区（含 .. 越界、符号链接指向外部、盘符/根不符）；
 *      -2 路径非法或缓冲区不足（need_len 给出所需长度）。
 *
 * 检查顺序：先**词法**规范化并判包含关系，再对最深的已存在祖先做
 * realpath 后**二次**判包含关系。只做词法检查是不够的——
 * 工作区内的一个符号链接可以指向外部，那是最常见的逃逸方式。
 */
int ca_ws_resolve(ca_workspace *ws, const char *input, char *out, size_t cap,
                  size_t *need_len);

/* ------------------------------------------------------------ 结果输出 */

/* 定长结果写入器。装不下即报错，不静默截断。 */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} ca_out;

int ca_out_init(ca_out *o, char *buf, size_t cap);
void ca_out_puts(ca_out *o, const char *s);
void ca_out_putc(ca_out *o, char c);
void ca_out_printf(ca_out *o, const char *fmt, ...);

/* puts 的可失败版本：装不下返回 -1 且不写入任何内容。
 * ca_out_puts 是便利版本（忽略失败）；需要据此判断「结果是否完整」的
 * 地方必须用这个。 */
int ca_out_try_puts(ca_out *o, const char *s);

/* ------------------------------------------------------------ 权限（P1/P2/P3） */

typedef enum {
    CA_PERM_ALLOW = 0,
    CA_PERM_DENY = 1
} ca_permission;

/* 权限回调。返回 DENY 表示用户拒绝。
 * 工具层**不做任何缓存**：每次调用都问一次（P1：默认全部拒绝，
 * 上一轮批准不等于这一轮批准）。 */
typedef ca_permission (*ca_perm_cb)(const char *tool_name, void *user);

/* 全局默认策略：拒绝一切。可由上层替换。 */
ca_permission ca_perm_deny_all(const char *tool_name, void *user);

/* ------------------------------------------------------------ 工具实现 */

typedef struct {
    ca_out *out;      /* 结果写入 */
    ca_workspace *ws;
    ca_perm_cb perm;
    void *perm_user;
} ca_tool_ctx;

/* 工具返回码。区分「用户拒绝」与「执行失败」很重要：
 * 前者要让模型知道可以换个做法继续，后者是技术故障。 */
typedef enum {
    CA_TOOL_OK = 0,
    CA_TOOL_DENIED = 1,        /* 用户拒绝（可回灌并继续对话） */
    CA_TOOL_ERR_PATH = 2,      /* 路径非法或越权 */
    CA_TOOL_ERR_IO = 3,        /* 读写出错 */
    CA_TOOL_ERR_TOO_LARGE = 4, /* 超过硬上限，拒绝而非截断 */
    CA_TOOL_ERR_NOTFOUND = 5,
    CA_TOOL_ERR_ARGS = 6       /* 参数无法解析 */
} ca_tool_status;

const char *ca_tool_status_text(ca_tool_status s);

/* 读文件。按 CA_FILE_READ_LIMIT 截断并标注行范围与截断标记。
 * args_json 形如 {"path":"src/a.c","offset":0,"limit":200} */
ca_tool_status ca_tool_read_file(ca_tool_ctx *ctx, const char *args_json);

/* 写文件。**破坏性操作，必须先过权限回调**（P1/P2）。
 * args_json 形如 {"path":"a.c","content":"..."} */
ca_tool_status ca_tool_write_file(ca_tool_ctx *ctx, const char *args_json);

/* 列目录。args_json 形如 {"path":"src"} */
ca_tool_status ca_tool_list_dir(ca_tool_ctx *ctx, const char *args_json);

#ifdef __cplusplus
}
#endif

#endif /* CAGENT_TOOLS_H */