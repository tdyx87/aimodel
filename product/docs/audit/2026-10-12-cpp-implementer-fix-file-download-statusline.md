# 角色审计记录

> 生成路径：`product/docs/audit/2026-10-12-cpp-implementer-fix-file-download-statusline.md`
> 对应规范：`.role-system/agents.md` §6.6

---

## 1. 基础信息

| 字段 | 内容 |
|------|------|
| 角色 | cpp-implementer |
| 时间 | 2026-10-12 23:20 |
| 任务 | 修复文件传输**下载接口缺失 HTTP 状态行**的缺陷（验证时发现） |
| 门禁 | G2 合入（修复） |

## 2. 输入 / 依赖

| 输入 | 路径 / 来源 |
|------|-------------|
| 上游产物 | `product/docs/design/file-transfer-contract.md`（§4.2 下载） |
| 触发 | 对文件传输做端到端验证时，`curl` 报 `Received HTTP/0.9 when not allowed` |

## 3. 证据锚点

| 证据 | 位置 |
|------|------|
| 缺陷位置 | `product/src/cpp/http_server.cpp` 下载分支（原第 452 行起）：手工拼接响应时**直接从 `Content-Type:` 开始**，漏掉 `HTTP/1.1 200 OK\r\n` 状态行 |
| 对照 | 同文件 `BuildResponse()`（第 97 行）以 `"HTTP/1.1 " << status << " " << reason << "\r\n"` 开头 |
| 复现 | 修复前 `curl -sv /api/file/download?file_name=verify.txt` → `Received HTTP/0.9 when not allowed`；响应体为空 |
| 修复后 | `curl -D -` 显示 `HTTP/1.1 200 OK` + `Content-Disposition: attachment; filename="verify.txt"`；`diff` 源/下载内容一致 |
| 影响面 | 浏览器下载按钮（`window.location.href = api/file/download?...`）同样失败——即**下载功能对用户完全不可用** |

## 4. 关键决策

| 决策 | 理由 | 被否决的备选 |
|------|------|--------------|
| 保留手工拼接，补上状态行 | 下载需 `Content-Disposition`，`BuildResponse` 不支持自定义头 | 改用 `BuildResponse`（会丢掉 `Content-Disposition`） |
| 仅改这一处 | grep 确认全文件只有此一处手工拼响应（`os << "Content-Type`），其余均走 `BuildResponse` | 全量重构响应构造（超出修复范围） |
| 加注释说明「必须以状态行开头」 | 该缺陷隐蔽（HTTP 客户端才报错，服务端无异常），注释防止回归 | 仅改代码不加注释 |

## 5. 风险与阻塞

- **测试盲区**：现有单测只覆盖 `FileTransfer` 库层，**不覆盖 HTTP 层**，故该缺陷此前未被 `ctest` 捕获。建议后续为 HTTP 层补一个最小集成测试（起 `LocalServer` → 原始 socket 请求 → 断言响应以 `HTTP/1.1 200` 开头）。
- 危险扩展名上传返回的 code 为 `INTERNAL_ERROR`（非专用码），消息文案正确；契约 §5 未定义该码，属可接受，建议后续补充专用错误码。

## 6. 产出

| 产物 | 路径 |
|------|------|
| 修复 | `product/src/cpp/http_server.cpp`（下载分支补 `HTTP/1.1 200 OK` 状态行 + 注释） |
| 验证 | 本审计（端到端 curl 全流程） |

## 7. 结论

| 项 | 内容 |
|------|------|
| 角色级裁决 | 完成并移交 |
| 风险 / 影响 | High（修复前下载完全不可用）→ 修复后 Low |
| 备注 | 重编零警告；`ctest` 38/38 |

## 8. 复核记录

- 复核人 | orchestrator
- 复核时间 | 2026-10-12 23:22
- 复核结论 | 通过
- 复核摘要 | 复现修复前 `HTTP/0.9` 报错与修复后 `HTTP/1.1 200 OK`；`diff` 确认下载内容与源一致；确认全文件仅此一处手工拼响应。

## 9. 移交

| 项 | 内容 |
|------|------|
| 交给 | `test-engineer`（建议补 HTTP 层集成测试）、`security-reviewer` |
| 下游依据 | 本审计 + `http_server.cpp` |
| 开工前提 | 下载接口已可正常返回二进制流 |
