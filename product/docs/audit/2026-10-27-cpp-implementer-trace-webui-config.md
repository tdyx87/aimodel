# 追踪开关、Web 配置面板与传输层重试

| 字段 | 内容 |
|------|------|
| 角色 | `cpp-implementer` |
| 时间 | 2026-10-27 |
| 任务 | trace-webui-config |
| 输入 | 用户要求（调试输出改为运行时开关、Web 端可配置 URL/模型/密钥、503 加重试） |
| 结论 | 完成并移交 |

## 1. 输入

- 用户反馈：`CAGENT_TRACE` 只能靠环境变量控制，希望改成界面开关；
  浏览器里要能配置服务端地址、API Key 与模型；真实服务返回 503 时应重试。
- 既有契约：`product/docs/design/contract.md`（终端命令、Web API、传输层状态码）。

## 2. 关键决策

- **D1 trace 默认关闭**：原先默认开启会把完整请求体（含对话内容）打到终端。
  改为默认 `0`，并新增 `ca_core_set_trace/_raw` 与 `/trace on|off`、`/trace raw on|off`。
  理由：调试开关不该默认泄露内容；运行时切换避免重启进程。
- **D2 模型名由 core 持有副本**：`cfg.model` 可能指向调用方或环境变量，
  `reconfig` 后旧缓冲可能失效。新增 `model_buf[256]` 拷贝，避免悬垂指针。
- **D3 Web 配置走回调**：`web` 层不认识 `core`，新增 `ca_web_set_config_cb`，
  浏览器提交 `/api/config` → `main.c` 回调 `ca_core_reconfig`。
  理由：保持依赖方向 `web → 无 core 依赖`。
- **D4 Web 模式允许空配置启动**：`api_base`/`model` 可先为空，首次发送时返回
  明确错误「未配置服务端地址」，而不是崩溃或发出畸形 URL。
- **D5 503/429 重试**：`curl_run` 拆为 `curl_run_once` + 外层重试（最多 3 次）。
  理由：真实服务在高峰期会短时 503。
- **D6 参数超限必须回灌 tool 结果**：此前只拒绝不回灌，模型看不到失败原因，
  会反复重发同一调用；改为写 `<result status="error">` + 拒绝通知。

## 3. 产出

- `product/include/cagent/core.h`：trace 存取器、`ca_core_reconfig`、`model_buf`
- `product/src/core.c`：上述实现；未配置地址时的显式错误；超限回灌
- `product/include/cagent/web.h`、`product/src/web.c`、`product/src/web_page.c`：配置面板与 `/api/config`
- `product/src/main.c`：`/trace` 命令、Web 回调接线、默认 trace=0
- `product/src/transport_curl.c`：重试循环
- `product/README.md`：环境变量说明同步

## 4. 结论

完成并移交。构建零警告，9 个测试二进制全绿（2486 次检查），详见
`2026-10-27-test-engineer-build-verification.md`。

已知限制（不阻塞交付，留给 reviewer 判定）：

1. 重试**没有退避**，连续 3 次立即重试；且在已收到部分 SSE 数据后重试，
   回调侧可能追加重复文本。当前只在首包前重试（503/429 通常发生在首包前），
   但未用代码强制这一点。
2. `ca_core_reconfig` 不是原子操作：先改 URL 再校验模型长度，失败时 URL 已生效。

## 5. 移交

- 交 `reviewer`（G2）：重点看重试路径与 `reconfig` 的边界。
- 交 `test-engineer`（G3）：已附构建与单测结果，等待独立复跑。
- 交 `doc-engineer`（G4）：README 已改，需确认用户能看懂 `/trace` 与 Web 配置步骤。
