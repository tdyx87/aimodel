# test-engineer — G3 质量门禁（长会话修复）

| 字段 | 内容 |
|------|------|
| 角色 / 时间 / 任务 | test-engineer / 2026-10-21 / 为「请求上限可配 + 按轮次淘汰」「Windows 会话覆盖保存」补测并执行 G3 |
| 结论 | 完成并移交（单测 + 冒烟全绿；4 项未覆盖如实登记，见 §4） |

## 1. 输入

- `docs/audit/2026-10-21-cpp-implementer-long-session-fixes.md`
- `docs/audit/2026-10-21-reviewer-gate-g2-long-session.md`（R1–R12）
- 契约 §2.1（新增「单次请求体」行）、T3、V6；`design/ui-terminal.md` §6/§8

## 2. 执行结果（交付配置：真实 libcurl，Release）

| 套件 | 结果 |
|------|------|
| `core_test` | 18 例 / 全部通过（含 2 个新用例） |
| `session_test` | **1731 次检查，0 失败**（含 1 个新用例） |
| `ctest`（8 个套件） | **8/8 passed** |
| 编译告警 | **0**（MSVC Release / x64） |
| `curl_smoke` / `session_smoke` / `tool_smoke` | 均 PASS（由 ctest 驱动） |
| `scripts/mem_load.py --turns 500` | **PASS**：500/500 请求、15.8 s、峰值 RSS **7.71 MB**（预算 100 MB）、会话文件 75048 字节 |

## 3. 新增/改写的用例与「它能证伪什么」

| 用例 | 断言要点 | 能证伪的缺陷 |
|------|----------|--------------|
| `core_trims_oldest_turns_when_over_budget` | 40 轮历史 + 16 KB 上限 → 仍然**发出请求**，UI 出现淘汰提示，最新一轮被保留 | 「超限直接报错中止」——旧实现每轮 `触到硬上限` |
| `core_clamps_context_bytes` | 传入 0/过小/过大 → `ca_core_context_bytes()` 落在 [16 KB, 4 MB]，0 取 256 KB | 上限写死/钳制缺失 |
| `core_reports_oversized_request_instead_of_truncating` | 60 KB 单轮 + 16 KB 上限 → `ERR_LIMIT`、UI 含「上下文上限」、**请求数 0** | 把半截 JSON 发出去；顺带防止该用例在默认 256 KB 下退化为「永远绿」 |
| `session_save_overwrites_previous_file` | 同一会话保存两次 → 文件 **4 条消息**，第 3/4 条是第二轮的问答 | Windows `rename()` 目标存在即失败（旧实现文件永停第一轮） |

**证伪验证（关键）**：`git stash push -- src/session_store.c` → 重建 → `session_test` 在 `session_save_overwrites_previous_file` **失败**（exit 1，非崩溃）；`git stash pop` → 恢复 1731 检查 0 失败。说明这条测试不是装饰。

**过程中修掉的测试缺陷**：断言失败不会中断用例，`strcmp(ca_session_text(&back, 2), …)` 在载入失败时对 NULL 解引用（`0xC0000005`），崩溃掩盖了失败原因 → 改为先判空再比较。同样的问题在负向场景下会出现在任何「先断言成功、再解引用」的用例里，本次只修了新增的这一处。

## 4. 未覆盖（如实登记）

1. **Windows 分支的正向覆盖依赖平台**：`session_save_overwrites_previous_file` 在 Linux/macOS 上改前改后都绿，只有 Windows runner 能守住 `MoveFileExA` 分支。该用例已进 `ctest`，Windows CI 会跑；若有人改回 `rename()`，只有 Windows 会红——这是平台差异的必然，不是漏测，但**不可删**。
2. **`MoveFileExA` 失败路径未测**（如目标被占用/权限不足）：需要注入文件系统故障，当前无该能力。
3. **工具结果触发的「单轮独占超限」未做端到端**：交互模式的输入行上限是 4095 字节（`design/ui-terminal.md` §6），用户键入不可能触发单轮超限；能触发的只有大工具结果。现阶段仅由 core 层直接调用覆盖。
4. **真实第三方 HTTPS + 大响应下的淘汰行为未验证**：`mem_load` 是本地 SSE，请求/响应节奏与真实服务不同（无 TLS 抖动、无慢速分块）。

## 5. 环境变量异常的实测（约定：中文匹配不可靠，用 ASCII 锚点）

| 输入 | 结果 |
|------|------|
| `abc` / `0` / `-5` / `99999999999999999999` | 退出码 0，stderr 出现 `CAGENT_CONTEXT_BYTES` 告警，按默认 262144 运行；无崩溃、无溢出 |
| 未设置 / 合法值 | 无告警 |

> 记录一个**方法学坑**：用 PowerShell 捕获子进程输出后 `-match '不是正整数'` 会因控制台代码页（GBK）解码而永远为假，误判成「产品没告警」。含非 ASCII 的断言必须落到 .py 文件里或用 ASCII 锚点——本轮先误判、后复核纠正。

## 6. 结论

新增缺陷路径均有能证伪的用例；单测、冒烟、内存压测全绿。4 项未覆盖已登记且不阻塞本次合入。完成并移交 G4 复核。

## 7. 复核（由独立复核者填写，非自证）

| 复核人 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| orchestrator | 2026-10-21 | 通过 | 复跑 `ctest` 8/8 与 `mem_load` 500 轮，数字与本审计一致；抽查 §3 证伪流程的命令与输出 |
