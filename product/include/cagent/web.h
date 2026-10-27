/* web.h —— 本地 Web UI（对外接口）
 *
 * 为什么要有 Web UI：终端交互有真实门槛——单键批准、纯文本滚动、
 * 长工具输出刷屏。浏览器能给出「聊天记录 + 工具卡片 + 批准按钮」。
 *
 * 安全前提（不是可选项）：
 *   1. **只监听 127.0.0.1**：这个服务能让模型执行命令，绝不能暴露到外网。
 *   2. **随机 token**：URL 里带 token，页面把它放进请求头；
 *      没有 token 的请求一律 401。没有它，本机任意网页都能驱动你的代理。
 *   3. **校验 Host 头**：挡住 DNS rebinding（恶意网站把域名解析到
 *      127.0.0.1 再打过来）。
 *   4. Web 模式下 run_shell 仍需显式开 CAGENT_ALLOW_SHELL=1：
 *      「浏览器里点一下就执行命令」不该是默认状态。
 *
 * 依赖：不引入任何第三方库（与项目「依赖只有 libcurl」一致）。
 * 平台差异全部在 web_net.c。
 */
#ifndef CAGENT_WEB_H
#define CAGENT_WEB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_web ca_web;

/* 推给浏览器的事件类型。 */
typedef enum {
    CA_WEB_EV_TEXT = 0,   /* 一段文本（用户输入 / 模型回复） */
    CA_WEB_EV_TOOL,       /* 工具调用或工具结果 */
    CA_WEB_EV_APPROVAL,   /* 请求批准（浏览器显示按钮） */
    CA_WEB_EV_INFO,       /* 会话信息（模型、目录、上下文上限） */
    CA_WEB_EV_ERROR,      /* 错误 */
    CA_WEB_EV_DONE        /* 一轮结束 */
} ca_web_event_kind;

typedef struct {
    ca_web_event_kind kind;
    const char *text;   /* 正文（可为空） */
    const char *name;   /* 工具名 */
    const char *status; /* ok / denied / denied_by_policy */
} ca_web_event;

/* 启动服务。port=0 表示由系统分配空闲端口。
 * 返回 0 成功。 */
int  ca_web_start(ca_web **out, int port);

/* 关闭并释放。 */
void ca_web_stop(ca_web *w);

/* 带 token 的访问地址（控制台要打印它）。 */
const char *ca_web_url(const ca_web *w);
const char *ca_web_token(const ca_web *w);
int         ca_web_port(const ca_web *w);

/* 推送事件（线程安全，可在任意线程调用）。 */
void ca_web_emit(ca_web *w, const ca_web_event *ev);

/* 用户消息：浏览器 → 代理。返回 0 成功。 */
int  ca_web_push_user(ca_web *w, const char *text);

/* 设置运行时重配回调。用户在浏览器配置面板里填写 URL/模型/密钥并
 * 保存后，通过 /api/config 触发此回调，由外部（main.c）把新配置
 * 写进 ca_core（ca_core_reconfig）。为 NULL 时 /api/config 返回 501。 */
void ca_web_set_config_cb(ca_web *w,
                          void (*on_config)(const char *api_base,
                                            const char *model,
                                            const char *api_key,
                                            void *user),
                          void *user);

/* 取一条用户消息。返回 1=有消息，0=超时，-1=服务已停止。 */
int  ca_web_pop_user(ca_web *w, char *buf, size_t cap, int timeout_ms);

/* 批准决定：浏览器 → 代理。allow=1 允许，0 拒绝。 */
void ca_web_set_approval(ca_web *w, int allow);

/* 等待批准决定。返回 1=允许，0=拒绝，-1=超时。 */
int  ca_web_wait_approval(ca_web *w, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* CAGENT_WEB_H */
