# cpp-implementer — 策略拒绝不该问用户；拒绝原因要如实区分

| 字段 | 内容 |
|------|------|
| 角色 / 时间 / 任务 | cpp-implementer / 2026-10-21 / 修用户实测暴露的「策略拒绝却先问 y/N，且谎称用户拒绝」 |
| 结论 | 完成并移交（`reviewer` 审 G2） |

## 1. 输入

- 用户实测输出：
  ```
  --- run_shell 请求。允许执行？[y/N] y
  （按策略拒绝执行命令：需要显式开启，设置 CAGENT_ALLOW_SHELL=1 后重试）
  <result status="denied" tool="run_shell">用户拒绝了执行命令。…</result>
  ```
- 契约 §5.2 P1（默认拒绝）/ P2（批准前展示）/ P3（拒绝要结构化回灌）、§7。

## 2. 关键决策

| 决策 | 理由 | 被否决的备选 |
|------|------|--------------|
| 权限流程改为 **展示内容 → 查策略 → 才问用户** | 策略拒绝时向用户要 y/N 是**问一个注定被拒的问题**：用户按了 y 仍不执行，还会以为「我明明批准了」。两处都在骗人 | 保留原顺序、只改文案——用户仍要白按一次键 |
| 策略拒绝时**仍展示内容** | 用户有权知道代理想干什么（P2/P5）；只是不再问他「同意吗」 | 策略拒绝就什么都不显示——用户会不知道发生了什么 |
| 新增 `CA_PERM_DENY_POLICY`，把「策略拒绝」与「用户拒绝」分开 | 两者对模型是完全不同的信息：前者重试无用，后者换个做法可以再来。回灌文案必须如实 | 统一写「已拒绝」——把规则约束说成用户意愿，是在替用户编造决定 |
| 工具层回灌用 `status="denied_by_policy"` | 模型需要能区分「换个做法」和「这条路走不通」 | 继续用 `denied` |
| `ca_ui_confirm` 拆成 `ca_ui_show_pending` + `ca_ui_ask` | 「展示」与「提问」是两件事：有的路径只展示不提问。拆分后各取所需，`ca_ui_confirm` 保留为两者的组合（既有测试不受影响） | 在 core 里用 `is_tty` 之类条件绕开——把策略逻辑漏进渲染层 |

## 3. 产出

| 文件 | 变更 |
|------|------|
| `include/cagent/tools.h` | `ca_permission` 新增 `CA_PERM_DENY_POLICY` |
| `include/cagent/ui.h`、`src/ui.c` | 拆出 `ca_ui_show_pending()`（只展示）与 `ca_ui_ask()`（只提问）；`ca_ui_confirm()` = 两者组合 |
| `src/core.c` | 权限回调改为「展示 → 策略 → 提问」；新增 `ca_core.deny_by_policy`，界面提示按原因区分 |
| `src/core.h` | 新增 `deny_by_policy` 字段 |
| `src/tools.c` | `check_perm()` 返回 `ca_permission`；`write_file` 按原因给不同回灌文案 |
| `src/tools_shell.c` | `ask_perm()` 返回 `ca_permission`；`run_shell`（两处）按原因给不同回灌文案 |
| `src/main.c` | `main_perm` 在 `CAGENT_ALLOW_SHELL` 未开启时返回 `CA_PERM_DENY_POLICY`（并说明如何开启） |
| `tests/core_test.c` | 假权限回调支持 kind=2（策略拒绝）；新增 `core_policy_denial_does_not_ask_the_user` |

**验证**

- 本地：`ctest` **8/8**；`core_test` **205 检查 0 失败**；`ui_test` 63、`tools_test` 128 检查 0 失败；零告警。
- **真实服务回归**（`CAGENT_ALLOW_SHELL` 未设置，模型请求 `run_shell`）：
  - 界面**不再出现**「允许执行？」；
  - 仍展示命令原文（`echo hi`）与策略原因（提示开 `CAGENT_ALLOW_SHELL=1`）；
  - 回灌为 `<result status="denied_by_policy" …>策略不允许执行命令（用户即使批准也不会执行）…`；
  - 界面提示为「按策略拒绝该工具调用（用户即使批准也不会执行），未做任何改动。」。

## 4. 结论

用户看到的「问了还是被拒、还说我拒绝了」这一处三重误导已全部消除：
不再问无意义的问题、不再谎称用户拒绝、策略原因可操作。完成并移交 G2。

## 5. 移交

- `reviewer`（G2）：重点看 ① 顺序调整是否削弱 P2（批准前必须看到完整内容）；
  ② `CA_PERM_DENY_POLICY` 的传播链是否完整（main → core → tools → 回灌）；
  ③ 既有 `core_policy_may_veto_after_user_approves` 用例（用户批准后被策略否决）语义是否仍正确。
- `test-engineer`（G3）：新用例的覆盖面；`ca_ui_show_pending/ask` 拆分后既有 ui 用例是否仍成立。

## 6. 复核（由独立复核者填写，非自证）

| 复核人 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| reviewer | 2026-10-21 | 通过 | 见 `2026-10-21-reviewer-gate-g2-policy-denial.md` |
| test-engineer | 2026-10-21 | 通过 | 见 `2026-10-21-test-engineer-gate-g3-policy-denial.md` |
