# 角色审计记录

> 生成路径：`product/docs/audit/2026-10-12-security-reviewer-chat-system.md`
> 对应规范：`.role-system/agents.md` §6.6

---

## 1. 基础信息

| 字段 | 内容 |
|------|------|
| 角色 | security-reviewer（只读） |
| 时间 | 2026-10-12 23:30 |
| 任务 | 聊天系统的安全与**隐私**审计（隐私红线、输入校验、注入、XSS、并发） |
| 门禁 | G3 质量 |

## 2. 输入 / 依赖

| 输入 | 路径 / 来源 |
|------|-------------|
| 审查对象 | `chat_suggest.*`、`http_server.cpp`（chat 段）、`web/app.js`（chat 模块） |
| 依据 | 契约 §5 隐私架构；`roles.md` §3.9 |

## 3. 证据锚点

| 证据 | 位置 |
|------|------|
| 绑定地址 | `http_server.cpp` `Start()` 强校验 `bind_host == "127.0.0.1"`（非回环直接拒绝） |
| 零出站 | `chat_suggest.cpp` 仅用标准库（cctype/cstdio/cstdlib/sstream）；无 socket/curl/http 客户端 |
| 输入用途 | `query` 仅用于 `detail::Trim` + 关键词扫描（`ParseTargetBillions`）；不落盘、不打日志、不回显到日志 |
| 注入防护 | 响应 JSON 中所有外部/动态字符串经 `detail::JsonEscape`；`web/app.js` 全部用 `textContent` 写入（无 `innerHTML`/`eval`） |
| 资源上限 | `http_server.cpp` 请求体上限 `kMaxRequestBytes = 11 MB`，超大 `Content-Length` 直接拒绝 |
| 错误语义 | 采集/序列化异常 → `{ok:false,error:{code:UNEXPECTED_ERROR,message:"请求失败，请稍后重试。"}}`，不泄露堆栈/路径 |

## 4. 关键决策（风险分级）

| 项 | 分级 | 说明 |
|----|------|------|
| 隐私红线（仅本机、无出站、输入不落盘） | 🟢 通过 | 与 `PRD-chat-system.md` NFR-3 一致；无网络调用、无日志、无持久化 |
| XSS（前端渲染服务端/用户内容） | 🟢 通过 | 一律 `textContent`；无 `innerHTML`/`eval` |
| 注入（JSON 响应构造） | 🟢 通过 | 动态串全部 `JsonEscape` |
| 请求体 DoS | 🟢 通过 | 11 MB 上限 + 头区 64 KB 上限 |
| `ExtractJsonStringField` 键匹配 | 🟡 Low | 子串查找在本契约受控请求体下无安全影响；建议契约变复杂时加固（CWE-20 输入校验） |
| 单线程 accept 循环 | 🟢 通过 | localhost 单用户场景；与既有服务一致，无新增并发面 |

## 5. 风险与阻塞

- 无 Critical / High。
- 🟡 Low：JSON 字段解析为轻量实现（见 §4）；不阻断放行。

## 6. 产出

| 产物 | 路径 |
|------|------|
| 安全审计 | 本审计 |

## 7. 结论

| 项 | 内容 |
|------|------|
| 角色级裁决 | ✅ 通过 |
| 风险 / 影响 | Low |
| 备注 | 无 Critical/High，满足 G3 放行条件 |

## 8. 复核记录

- 复核人 | orchestrator
- 复核时间 | 2026-10-12 23:30
- 复核结论 | 通过
- 复核摘要 | 复现隐私证据：`chat_suggest.cpp` 仅标准库、无出站；确认 `web/app.js` 无 `innerHTML`；确认绑定地址强校验仍在。

## 9. 移交

| 项 | 内容 |
|------|------|
| 交给 | `orchestrator`（G3 核验） |
| 下游依据 | 本审计 + `test-engineer` 审计 |
| 开工前提 | 无 Critical/High（已满足） |
