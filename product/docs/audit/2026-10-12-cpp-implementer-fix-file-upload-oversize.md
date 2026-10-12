# 角色审计记录

> 生成路径：`product/docs/audit/2026-10-12-cpp-implementer-fix-file-upload-oversize.md`
> 对应规范：`.role-system/agents.md` §6.6

---

## 1. 基础信息

| 字段 | 内容 |
|------|------|
| 角色 | cpp-implementer |
| 时间 | 2026-10-12 23:30 |
| 任务 | 修复上传接口在「请求体超过服务端接收上限」时的错误响应不一致，并消除偶发连接重置 |
| 门禁 | G2 合入（修复） |

## 2. 输入 / 依赖

| 输入 | 路径 / 来源 |
|------|-------------|
| 上游产物 | `product/docs/design/file-transfer-contract.md`（§6.4 大小上限与错误体） |
| 触发 | 对上传做端到端验证（含超限场景）时发现 |

## 3. 证据锚点

| 证据 | 位置 |
|------|------|
| 缺陷 1 | `product/src/cpp/http_server.cpp`：请求体 > `kMaxRequestBytes`(11 MB) 时 `raw.clear()` → 落到兜底 `400 Bad Request`（`text/plain`），非契约 JSON |
| 影响 | 前端 `fetch(...).json().catch(()=>null)` 解析失败 → 只显示笼统「操作失败，请重试。」，而非「文件过大」 |
| 缺陷 2 | 超限时未读取请求体即关闭连接 → 客户端仍在发送时收到 RST，偶发读不到错误响应（表现为「网络错误」） |
| 复现（修复前） | 12 MB 上传 → `HTTP/1.1 400 Bad Request` + `text/plain` `Bad Request` |
| 修复后 | 12 MB 上传 → `200` + `{"ok":false,"code":"TOO_LARGE","message":"文件过大，请选择小于 10 MB 的文件。"}`；连续 5 次稳定，无空响应 |
| 非上传接口 | 12 MB → `413` + `{"ok":false,"code":"REQUEST_TOO_LARGE","message":"请求体过大。"}` |

## 4. 关键决策

| 决策 | 理由 | 被否决的备选 |
|------|------|--------------|
| 超限时**保留请求头**（不再 `raw.clear()`） | 需要按路径区分返回哪种错误体 | 清空 raw（丢失路径，无法区分） |
| 路由比较前**去掉 query string** | 与 `HandleRequest` 的路由口径一致（`/api/file/upload?file_name=...`） | 直接比较带 query 的 path（首版即因此误判，已修正） |
| 上传接口返回契约 `TOO_LARGE`（200 + JSON） | 与 `BuildFileErrorResponse`、契约 §6.4 一致；前端可解析出用户文案 | 统一返回 413（前端 `messageForCode` 无此码，只能显示兜底文案） |
| 回错误前**有界排空**请求体（上限 32 MB、每次 1s 超时） | 避免过早关闭导致客户端 RST、读不到错误体；丢弃不缓冲，不违背内存防护初衷 | 直接关闭（偶发「网络错误」）；无上限排空（超大声明值会长时间阻塞） |

## 5. 风险与阻塞

- 排空上限 32 MB：若客户端声明的请求体远超此值，仍可能被 RST——属滥用场景，可接受。
- 仍未补 HTTP 层自动化测试（同 `2026-10-12-cpp-implementer-fix-file-download-statusline.md` §5 的建议）。

## 6. 产出

| 产物 | 路径 |
|------|------|
| 修复 | `product/src/cpp/http_server.cpp`（超限分支：按路径返回 JSON 错误 + 有界排空） |
| 验证 | 本审计（上传全路径复测） |

## 7. 结论

| 项 | 内容 |
|------|------|
| 角色级裁决 | 完成并移交 |
| 风险 / 影响 | Medium（修复前超限上传提示不友好且偶发失败）→ 修复后 Low |
| 备注 | 重编零警告；`ctest` 38/38 |

## 8. 复核记录

- 复核人 | orchestrator
- 复核时间 | 2026-10-12 23:32
- 复核结论 | 通过
- 复核摘要 | 复现修复前 `400 Bad Request` 纯文本；修复后 12 MB 上传连续 5 次返回 JSON `TOO_LARGE`；确认正常上传/10MB+1/路径穿越/危险扩展名/错误方法均无回归。

## 9. 移交

| 项 | 内容 |
|------|------|
| 交给 | `test-engineer`（建议补 HTTP 层集成测试）、`frontend-implementer`（无需改动：前端已按 JSON 解析） |
| 下游依据 | 本审计 + `http_server.cpp` |
| 开工前提 | 上传超限已返回可解析的 JSON 错误 |
