# 审计：工具批准通道（test-engineer，G3）

| 字段 | 内容 |
|------|------|
| 角色 / 时间 / 任务 | `test-engineer` / 2026-10-21 / 批准通道与 UI 等待提示的质量门禁（G3） |
| 结论 | 完成并移交 |

## 1. 输入

- 审查结论：`2026-10-21-reviewer-gate-g2-tool-approval.md`（✅）
- 实现审计：`2026-10-21-cpp-implementer-tool-approval.md`
- 代码：`src/{main,tools,ui,core}.c`、`tests/{core,tools,ui}_test.c`、`scripts/tool_smoke.py`

## 2. 关键决策

1. **单元层与端到端层都要有**。单测证明「函数被调用」，证明不了「用户按 y 之后文件真的写了」——
   后者必须用真实二进制 + 真实 libcurl 跑（`scripts/tool_smoke.py`），并进 CI。
2. 破坏性路径的用例必须同时断言**副作用**（文件是否存在、内容是否一致），不能只看界面文案。
   理由：本轮 `core_write_tool_is_denied_by_default` 就因为探针路径错误而「永真通过」。
3. 拒绝路径不能只看「界面说拒绝了」，还要断言**文件未创建**；批准路径要断言**内容一致**。
4. 非 TTY 的降级必须有断言（无 ANSI、无等待提示），否则「干净输出」只是口头承诺。

## 3. 产出（用例清单与结果）

| 层 | 用例 | 断言要点 |
|----|------|---------|
| core | `core_runs_tool_when_user_approves` | `y` → 文件存在且内容 `ok` |
| core | `core_policy_may_veto_after_user_approves` | 用户 `y` + 策略否决 → 文件不存在、界面说明拒绝 |
| core | `core_runs_tool_when_user_approves_sees_pending` | 待批准内容出现在界面 |
| tools | `write_pending_display_shows_path_and_content` | 路径 + 内容都进 `pending_display` |
| tools | `write_pending_display_marks_truncation` | 超长内容标注「仅前/共」 |
| ui | `ui_wait_hint_is_tty_only` | 非 TTY 输出长度为 0 |
| ui | `ui_wait_hint_is_erased_by_first_output` | 擦行序列在正文之前，且只擦一次 |
| ui | `ui_wait_hint_cleared_when_no_text_arrives` | 只有工具调用时也擦净 |
| E2E | `scripts/tool_smoke.py`（CI 三平台） | 批准写盘、拒绝不写、预览可见、工具结果回灌（两轮） |

**执行结果（本地 Windows x64，Release）**

- `ctest`：**8/8 通过**
- `core_test`：18 例 / **78 检查 0 失败**
- `tools_test`：**115 检查 0 失败**
- `ui_test`：**50 检查 0 失败**
- `session_test`：21 例（POSIX 另 1 例）/ **1714 检查 0 失败**
- 冒烟：`curl_smoke` PASS、`session_smoke` PASS、`tool_smoke` PASS
- 额外手工核对：管道运行输出中无等待提示、无 `\x1b`

## 4. 结论

G3 的「单测 + E2E 全绿」满足。**仍存在未覆盖项**（不阻塞本次放行，但必须如实记录）：

| 未覆盖 | 风险 | 后续 |
|--------|------|------|
| 无 ASAN/UBSAN 构建 | 越界靠不变量断言，未断言的路径可能漏 | 独立工单 |
| 未对真实第三方 HTTPS 服务验证 | 只在本地/CI 本地 SSE 上验过 | 需凭据后补 |
| 活跃/超长会话 RSS 未测 | NFR-1 只有空闲基线 | 需专门压测 |
| 并发（两端同时写同一会话）未测 | 单用户场景，暂不覆盖 | 记录为已知边界 |

## 5. 移交

交给 `security-reviewer`（同属 G3），依据本审计与上表。

## 6. 复核（由独立复核者填写，非自证）

| 复核人 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| orchestrator（单人模式） | 2026-10-21 | 通过 | 本地复跑 `ctest` 与 `tool_smoke.py`，数字与本文一致 |
