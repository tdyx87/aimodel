# test-engineer — G3 质量门禁（真实服务可用性修复）

| 字段 | 内容 |
|------|------|
| 角色 / 时间 / 任务 | test-engineer / 2026-10-21 / 为「工具声明 / 状态码 / 4xx 原因 / 结果措辞」补测，并执行 G3 |
| 结论 | 完成并移交（本地全绿 + 真实服务 PASS；3 项未覆盖如实登记） |

## 1. 输入

- `docs/audit/2026-10-21-cpp-implementer-real-api.md`、`2026-10-21-reviewer-gate-g2-real-api.md`
- 契约 §4.3 / §5.1 / §5.3 / C1；PRD NFR-1、NFR-3

## 2. 执行结果（交付配置：真实 libcurl，Release）

| 项 | 结果 |
|----|------|
| `ctest` | **8/8 passed** |
| `core_test` | **200 次检查，0 失败**（新增 5 条：tools 进请求体、4xx 原因透出、无正文时保持通用文案、已执行措辞、被拒措辞） |
| `session_test` / `tools_test` | 1731 / 115 次检查，**0 失败** |
| 编译告警 | **0**（MSVC Release / x64） |
| `mem_load.py --turns 100` | PASS（峰值 7.6 MB，预算 100 MB） |
| `scripts/real_api_smoke.py`（真实 GLM-4.7-FLASH） | **RESULT: PASS (chat=ok, memory=ok, tool=ok)** |

## 3. 新增用例与「它能证伪什么」

| 用例 | 断言要点 | 能证伪的缺陷 |
|------|----------|--------------|
| `core_offers_tools_to_the_model` | 请求体含 `"tools"`、六个工具名、`"parameters"`、`old_string`/`command` 等参数名 | 「请求体没有 tools → 真实模型永远不会调用工具」（本轮根因） |
| `core_surfaces_http_error_body` | 429 + JSON 错误体 → `last_error` 含状态码与服务端 message | 「4xx 只报『请求失败』，用户看不到额度/过载原因」 |
| `core_keeps_generic_message_when_no_body` | 200 + 不可解析内容 → 仍报「无法解析」，**且不含 HTTP 字样** | 把半截 SSE 原文当「服务端原因」贴给用户 |
| `core_tool_result_tells_model_the_tool_already_ran` | 批准后第二个请求含 `tool_result` 与 "already been executed" | 模型收到裸结果就原样再要一次同一个工具 |
| `core_denied_tool_result_says_it_did_not_run` | 拒绝后第二个请求含 "did NOT run"，**且不含** "already been executed" | 被拒的工具被说成「已执行」→ 模型以为文件已写好 |

**负向验证**：把 core 的请求改成不传 tools → `core_offers_tools_to_the_model` 立即 FAIL（exit 1），恢复后转绿。
这排除了「用例只是恰好通过」。

**测试自身踩过的坑（已修）**：新用例最初用了 `x.txt` 这个文件名，而
`core_permission_defaults_to_deny` 依赖「`x.txt` 不存在」——前一个用例把它写出来，
后一个用例立刻变红。**跨用例的文件名共享就是隐式耦合**：测试之间必须用互不相同的文件名，
否则失败会指向错误的地方。

## 4. 真实服务验证（GLM-4.7-FLASH，429 频繁）

| 检查 | 结果 |
|------|------|
| 真实 TLS + 流式对话 | 通过（模型自述身份） |
| 多轮记忆 | 通过（第二轮正确回答「7」，会话文件 4 条消息） |
| 工具调用 → 批准 → 落盘 | 通过（界面显示 `[调用工具 write_file]` + 路径 + 内容 + `[y/N]` 提示；文件内容 `ok`） |
| 拒绝路径 | 通过（非 `y` 输入 → `status="denied"` 回灌 + 「已拒绝该工具调用，未做任何改动」） |
| 限流下的错误呈现 | 通过（`错误：HTTP 429: 该模型当前访问量过大，请您稍后再试`） |
| 密钥不落盘 | 通过（会话文件不含 key） |

> 记录一条方法学：真实服务限流（429）会让「偶发失败」看起来像「功能坏了」。
> 脚本按 `--tries/--sleep` 重试，并把限流与真失败**分开计数**——
> 否则一次限流就能把结论带偏。

## 5. 未覆盖（如实登记）

1. **非 OpenAI 兼容服务**（US-8）未做，工具调用路径只验证了 OpenAI 兼容形态。
2. **工具结果标准握手**（`role:"tool"` + `tool_call_id`）未实现，模型重复请求同一工具的根因未根治（reviewer R9）。
3. **真实服务的鉴权失败路径**未自动化：key 无效时应显示 `HTTP 401: ...`（本次人工验证过一次，
   显示正确，但没进自动化脚本——因为拿不到第二个 key）。
4. `search` 的**真实文件遍历**（含子目录、符号链接、大仓库）未在真实服务上跑，
   仅由 `tools_test` 覆盖字面搜索逻辑。

## 6. 结论

新增缺陷路径均有能证伪的用例；本地套件全绿；真实服务三件套 PASS。
3 项未覆盖已登记。完成并移交 G4 复核。

## 7. 复核（由独立复核者填写，非自证）

| 复核人 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| orchestrator | 2026-10-21 | 通过 | 复跑 `ctest` 8/8 与 `real_api_smoke.py`（PASS）；核对负向验证过程 |
