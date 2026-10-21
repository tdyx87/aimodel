# reviewer — G2 合入评审（长会话修复）

| 字段 | 内容 |
|------|------|
| 角色 / 时间 / 任务 | reviewer / 2026-10-21 / 评审「请求体上限可配 + 按轮次淘汰」与「Windows 会话保存覆盖」两项修复 |
| 结论 | ✅ 通过（R1–R9 无阻塞项；R3 当场修复后复验，R5 转为文档前置条件） |

## 1. 输入

- 实现审计：`docs/audit/2026-10-21-cpp-implementer-long-session-fixes.md`
- 契约：`docs/design/contract.md` §2.1（本轮新增「单次请求体」行 + 修订说明）、§6、T3、V6
- 代码：`src/core.c`（`core_collect_msgs` / `core_next_turn` / `core_one_turn`）、`src/session_store.c`（`ca_session_save`）、`src/main.c`（`CAGENT_CONTEXT_BYTES`、`save_session_quietly`）、`include/cagent/core.h`
- 测试：`tests/core_test.c`、`tests/session_test.c`

## 2. 审查清单与结论

| # | 检查项 | 结论 | 依据 |
|---|--------|------|------|
| R1 | 淘汰循环是否**收敛**（不能死循环/无限重试） | 通过 | `core_next_turn` 从 `drop+1` 起找下一个 user，返回值严格大于 `drop`；`drop` 单调递增且有上界 `nmsg`，循环必然终止。`nmsg - drop ≥ 1` 恒成立（`msgs+drop` 永不为空指针区间） |
| R2 | 淘汰单位是否为**完整轮次** | 通过 | 只从 `user` 边界切；不会出现「有答无问」。且**只裁剪发送视图，不改写用户会话**——会话自身的 4 MB 预算照旧生效，两层职责没有混 |
| R3 | 报错/提示文案是否与当前模式一致 | **发现问题，已修** | 单轮模式（`--prompt`，`cfg.session == NULL`）下也提示「用 /new 开新会话」，而该模式没有 `/new` 命令。已改为按 `session != NULL` 分支给建议；重建后 `ctest` 8/8 |
| R4 | 请求体缓冲的分配/释放是否成对、失败路径是否泄漏 | 通过 | 在**全部校验之后**分配；`ca_core_free` 释放并清零。`main` 退出路径已调用 |
| R5 | 重复 `ca_core_init` 是否会泄漏 | 通过（转为前置条件） | `init` 先 `memset` 再分配；若调用方对同一对象二次 init，会泄漏上一块缓冲。C API 无法安全探测未初始化内存，故**不作为缺陷**，但已把「init 前必须 `ca_core_free`」写进 `core.h` 注释；本仓库测试 setup 已先 free |
| R6 | 超限时是否**至少**保证「优雅失败」而非半截 JSON | 通过 | `rc == -2` 时绝不发送；单轮独占超限直接 `ERR_LIMIT` 且 `g_req_count == 0`（用例已断言） |
| R7 | Windows 替换语义是否正确、失败是否保留旧文件 | 通过 | 先写 `.tmp` 再 `MoveFileExA(MOVEFILE_REPLACE_EXISTING)`；失败分支 `remove(tmp)` 后返回 -1，**旧文件不动**。POSIX 分支保持 `rename()` 原子覆盖 |
| R8 | 「失败必须可见」是否落实 | 通过（本轮修复的根因） | `save_session_quietly` 不再吞返回值：失败时 stderr 告警一次（`g_save_warned`），并说明「退出后不会保留」。调试用的 `DBG` 输出已全部删除（`grep DBG src/` 无命中） |
| R9 | 新增用例是否**真的会跑** | 通过 | `core_trims_oldest_turns_when_over_budget`、`core_clamps_context_bytes`、`session_save_overwrites_previous_file` 均已登记在各自文件底部表；运行输出可见 `[ RUN ]`。`session_test` 1731 检查 0 失败 |
| R10 | 测试能否**证伪**缺陷 | 通过 | 临时回退 `session_store.c` 后 `session_save_overwrites_previous_file` 立即失败（exit 1）；恢复后转绿。测试对 Windows 分支有效，非「永远绿」的装饰 |
| R11 | 文档与实现是否一致 | 通过 | 契约 §2.1 新增行与修订说明、`design/ui-terminal.md` §8 两条文案、README 环境变量表，均与 `core.c` 实际文案/钳制范围一致（16 KB–4 MB，默认 256 KB） |
| R12 | 是否引入回归（内存/性能） | 通过 | `mem_load` 500 轮：峰值 7.71 MB（预算 100 MB）、15.8 s；淘汰后会话 512 条、首条为第 245 轮，符合设计 |

## 3. 遗留（不阻塞，记录在案）

- `context_notified` 每轮重置，因此长会话中**每轮**都会打印一次淘汰提示。信息正确但会长时段刷屏；若日后有用户反馈，可改为「同一会话只提示一次」或折叠成计数。属可用性打磨，不影响正确性。
- `MoveFileExA` 未带 `MOVEFILE_WRITE_THROUGH`：断电场景下可能丢掉最近一轮，与契约「至多丢一轮」一致，未做额外加固。
- 仍未在 macOS 上实跑 `scripts/build.sh`（CI 该步骤仅 Linux）——这条归 release 的 C1，不属本次合入范围。

## 4. 结论

两项修复的**根因**都处理了（不是打补丁式绕过）：上限不再写死、超限改为按契约淘汰；Windows 的替换语义差异用平台分支解决，并把「静默失败」改成「明确告警」。测试具备证伪能力。✅ 通过，放行 G3。

## 5. 移交

`test-engineer` + `security-reviewer`（G3）：重点验证 `CAGENT_CONTEXT_BYTES` 的**异常输入**（负数、超长数字、非数字）不会溢出/崩溃；`MoveFileExA` 分支在 Windows 上无新增告警（零告警是硬门禁）。

## 6. 复核（由独立复核者填写，非自证）

| 复核人 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| orchestrator | 2026-10-21 | 通过 | 抽查 R3 修复处与 `grep DBG` 结果，与本审计一致；R1 收敛性复核重读 `core_next_turn`，确认严格递增 |
