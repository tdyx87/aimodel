# 角色审计记录

> 生成路径：`product/docs/audit/2026-10-12-cpp-implementer-chat-system-impl.md`
> 对应规范：`.role-system/agents.md` §6.6

---

## 1. 基础信息

| 字段 | 内容 |
|------|------|
| 角色 | cpp-implementer |
| 时间 | 2026-10-12 23:15 |
| 任务 | 实现聊天/推荐后端：`chat_suggest` 模块 + HTTP 路由 + 构建接线 + 单测 |
| 门禁 | G2 合入 |

## 2. 输入 / 依赖

| 输入 | 路径 / 来源 |
|------|-------------|
| 上游产物 | `product/docs/design/chat-system-contract.md`、`product/docs/design/http-ui.md` |
| 依赖 | `product/include/aimodel/system_info.h`（`SystemInfo`、`ToJson`、`detail::JsonEscape`）、既有 `file_transfer` 风格 |

## 3. 证据锚点

| 证据 | 位置 |
|------|------|
| 关键文件 | `product/include/aimodel/chat_suggest.h`、`product/src/cpp/chat_suggest.cpp` |
| 关键行 | `chat_suggest.cpp`：`BuildChatAdvice`（分类判定）、`ChatAdviceToJson`（契约 §2.2）、`ChatCapabilityJson`（§3.2） |
| 关键文件 | `product/src/cpp/http_server.cpp`：`ExtractJsonStringField`、`/api/chat/*` 路由块 |
| 关键文件 | `product/CMakeLists.txt`：`src/cpp/chat_suggest.cpp`、`chat_suggest_test` 目标 |
| 实测 | `ctest` 38/38 全绿（其中 `ChatSuggestTest` 12 项）；重编改动文件零警告 |

## 4. 关键决策

| 决策 | 理由 | 被否决的备选 |
|------|------|--------------|
| `BuildChatAdvice` 设计为**纯函数**（只吃 `SystemInfo`+`query`） | 可单测、可审计、天然满足契约 §5 隐私（不联网/不落盘） | 在函数内直接调 `GetSystemInfo()`（不可测、耦合采集） |
| JSON 序列化放**库层**（`ChatAdviceToJson`） | 与既有 `system_info::ToJson` 一致；HTTP 层只做路由 | 全部塞进 `http_server.cpp`（JSON 逻辑与路由混杂） |
| `config_snapshot` 复用 `ToJson(info)` | 单一数据源；`http_server` 对同一 `SystemInfo` 只采集一次 | 另写一份精简序列化（重复维护、易漂移） |
| 显存换算用**启发式**系数（4-bit 0.6 GB/B、fp16 2.0 GB/B） | 只做量级分类，配合 reason 中的不确定性声明 | 精确模型占用表（无权威数据源，超出本期） |
| 新增 `ExtractJsonStringField`（支持 `\uXXXX` 代理对） | 契约请求体为 JSON；避免引入第三方 JSON 依赖（C++11、零新增依赖） | 引入 nlohmann/json 等（新增依赖，违背最小依赖） |
| 无 query 字段/空 query 统一按 `INVALID_QUERY` 处理 | 契约 §2.2 ErrorResponse 已定义该码 | 返回 400（与契约「错误在响应体内」不一致） |

## 5. 风险与阻塞

- 每次请求调用一次 `GetSystemInfo()`（本机毫秒级，可接受）；未做缓存。
- 建议逻辑为启发式，仅用于粗略分类，已在 reason 中如实标注不确定性。

## 6. 产出

| 产物 | 路径 |
|------|------|
| 头文件 | `product/include/aimodel/chat_suggest.h` |
| 实现 | `product/src/cpp/chat_suggest.cpp` |
| 路由 | `product/src/cpp/http_server.cpp`（新增 `/api/chat/capability`、`/api/chat/suggest`） |
| 构建 | `product/CMakeLists.txt` |
| 单测 | `product/tests/chat_suggest_test.cpp` |

## 7. 结论

| 项 | 内容 |
|------|------|
| 角色级裁决 | 完成并移交 |
| 风险 / 影响 | Low |
| 备注 | 零警告、`ctest` 38/38；端到端 curl 验证见 `test-engineer` 审计 |

## 8. 复核记录

- 复核人 | orchestrator
- 复核时间 | 2026-10-12 23:15
- 复核结论 | 通过
- 复核摘要 | 重编 `chat_suggest.cpp`/`http_server.cpp` 观察零 warning；复现 `ctest` 38/38；`config_snapshot` 复用 `ToJson` 与代码一致。

## 9. 移交

| 项 | 内容 |
|------|------|
| 交给 | `reviewer`（C++ 清单）、`security-reviewer`（隐私） |
| 下游依据 | `chat_suggest.h/.cpp`、`http_server.cpp` 路由块、`CMakeLists.txt` |
| 开工前提 | 按 C++ 清单与隐私红线审查 |
