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
#include <stdio.h>

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

/* ------------------------------------------------------------ 文件打开（UTF-8 路径）*/

/* 打开文件。Windows 上内部一律用 UTF-8 字节串，而窄字符 API
 * （fopen）用的是**系统 ANSI 代码页**（简体中文机器是 GBK），
 * 于是中文路径会失败或建出乱码名字。本函数在 Windows 上把路径转成
 * UTF-16 后用 _wfopen，POSIX 上直接 fopen。
 * 失败返回 NULL，语义与 fopen 一致。 */
FILE *ca_tool_fopen(const char *path, const char *mode);

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
    CA_PERM_DENY = 1,
    /* 策略拒绝：不是用户的决定，是**规则**不允许。
     * 两者必须区分——
     *   用户拒绝：用户可以改主意，模型换个做法即可；
     *   策略拒绝：用户按 y 也没用（比如 run_shell 没开开关），
     *   继续重试只是白费一次批准。 */
    CA_PERM_DENY_POLICY = 2
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
    /* 破坏性工具**待展示内容**：在调用工具之前填好，供权限界面展示。
     * P5 要求命令原文完整呈现、不得折叠——由工具层准备原始文本，
     * 避免权限层重复解析参数（那样可能解析出与实际执行不同的东西）。 */
    const char *pending_display;
} ca_tool_ctx;

/* 取得某个破坏性工具**待展示给用户的内容**（契约 P2/P5）。
 *
 * 契约 P2/P5：批准前必须看到「完整参数」与「命令原文」。
 * 这里单独提供查询，而不是让权限回调自己去解析参数——
 * 权限层不该重复解析一遍，更不该可能解析出与实际执行不同的东西。
 *
 * 返回 NULL 表示当前没有待批准的操作。 */
const char *ca_tool_pending_display(const ca_tool_ctx *ctx, const char *tool_name);

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

/* ---- search ----
 *
 * 只做**字面**搜索，不做正则：引入正则引擎意味着引入一套自己的
 * 回溯/灾难性匹配保护逻辑，而那与「极低内存、行为可预测」的目标相悖。
 * 正则搜索留给后续议题，届时按 ADR 走（契约 §5.1 原写的是「字面/正则」，
 * 本轮只交付字面部分，如实标注）。
 *
 * 结果按文件分组、带行号，且必须有截断标注——与 read_file 同理。
 */
typedef struct {
    const char *pattern;      /* 字面串，必需 */
    const char *glob;         /* 文件名子串过滤，可为 NULL */
    int max_results;          /* <=0 用默认 200 */
} ca_search_opts;

ca_tool_status ca_tool_search(ca_tool_ctx *ctx, const ca_search_opts *opts);

/* 从模型给的参数 JSON 直接执行 search。
 * 早先 core 的分发表里**没有 search**：函数存在、测试覆盖，但模型永远
 * 调不到——「契约 §5.1 六工具」在真实调用路径上只有五个。 */
ca_tool_status ca_tool_search_json(ca_tool_ctx *ctx, const char *args_json);

/* ---- run_shell ----
 *
 * P5：命令原文必须**完整**展示给用户，不得折叠或省略——
 * 用户批准前必须看清到底要执行什么。折叠命令等于让用户盲批。
 *
 * 输出有上限（CA_TOOL_RESULT_LIMIT），且有超时上限：
 * 一个挂死的命令会把整个代理卡住，那比失败更糟。
 */

/* 默认超时。超时后会杀掉进程并如实报告，不假装成功。 */
#define CA_SHELL_TIMEOUT_MS 60000

/* args_json 形如 {"command":"git status","timeout_ms":60000}
 * timeout_ms 缺省或 <=0 时用 CA_SHELL_TIMEOUT_MS。 */
ca_tool_status ca_tool_run_shell(ca_tool_ctx *ctx, const char *args_json);

/* ---- edit_file ----
 *
 * 按**精确匹配**替换。这条工具最大的风险不是写错，而是「匹配到多处、
 * 全量替换」——那会静默破坏代码而不报错。
 *
 * 因此：
 *   - old_string 为空一律拒绝（空串匹配所有位置）
 *   - 出现多次且未显式 replace_all 时**拒绝**并要求更精确的上下文，
 *     而不是替用户猜
 *   - 落盘走「临时文件 + 原子替换」，中途失败不会留下半截文件
 */

/* 替换前的影响面统计。必须在**请求权限之前**算好——
 * 否则用户是在「不知道会改多少」的情况下批准。契约 P2。 */
typedef struct {
    int occurrences;     /* old_string 出现次数 */
    int lines_before;
    int lines_after;
    long long bytes_before;
    long long bytes_after;
} ca_edit_preview;

/* 计算影响面，**不做任何修改**。
 * 返回 CA_TOOL_OK 表示参数可解析且目标文件存在（occurrences 可能为 0）。 */
ca_tool_status ca_tool_edit_preview(ca_tool_ctx *ctx, const char *args_json,
                                   ca_edit_preview *out);

/* 执行替换。args_json 形如
 *   {"path":"a.c","old_string":"x","new_string":"y","replace_all":false} */
ca_tool_status ca_tool_edit_file(ca_tool_ctx *ctx, const char *args_json);

/* ---- 交给模型的工具清单（OpenAI 兼容 tools 数组） ----
 *
 * 为什么必须由 tools 层提供而不是 llm 层写死：名称/参数只在 tools 层是
 * 唯一的真相，llm 层另写一份迟早会与实现不一致（比如参数名写错，
 * 模型照着错的名字调用，最后表现为「工具总是失败」）。
 *
 * 返回静态只读 JSON，长度固定，不随调用增长。 */
const char *ca_tools_schema_json(void);

#ifdef __cplusplus
}
#endif

#endif /* CAGENT_TOOLS_H */