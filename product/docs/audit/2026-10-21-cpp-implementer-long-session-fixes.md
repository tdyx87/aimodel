# cpp-implementer — 长会话可用性修复（请求体上限 + Windows 会话落盘）

| 字段 | 内容 |
|------|------|
| 角色 / 时间 / 任务 | cpp-implementer / 2026-10-21 / 修复实测暴露的两个「长会话不可用」缺陷：请求体 16 KB 硬上限、Windows 上会话文件只保留第一轮 |
| 结论 | 完成并移交（`reviewer` 审 G2） |

## 1. 输入

- `docs/design/contract.md` §2.1（分层预算：会话历史窗口 4 MB、超限淘汰最旧完整轮次 + UI 明示）、T3、V6。
- `docs/prd/PRD-c-agent.md` NFR-1（RSS ≤ 100 MB）、US-6（`/resume` 继续历史会话）。
- 实测证据（本轮自测产生，非推测）：
  - 离线 3 轮后会话文件仅 **439 字节 / 6 条消息**——这是**修好之后**的数字；修好之前为 2 条（第一轮），且后续 `ca_session_save` 每次都返回 `-1`，调用方 `(void)` 掉返回值（调试输出 `DBG save rc=` 已取证，见 §3）。
  - 旧代码 `src/core.c` 的 `CA_CORE_MAX_REQUEST 16384` 与 §2.1「4 MB 历史窗口」相差 256 倍：约 50 轮短对话或一次 >16 KB 工具结果后，**每一轮**都报 `请求体需要 N 字节，上限 16384` 并中止。

## 2. 关键决策

| 决策 | 理由 | 被否决的备选 |
|------|------|--------------|
| 请求体上限**默认 256 KB**，`CAGENT_CONTEXT_BYTES` 可配，钳制 [16 KB, 4 MB] | 与「本地小模型 + 长会话」的真实用量匹配：256 KB ≈ 数百轮短对话，又远低于 4 MB 历史窗口，留出淘汰空间；可配让用户按模型上下文窗口自行收紧/放宽 | ① 无上限——违反 V6/C3；② 直接放到 4 MB——单请求瞬时分配翻 16 倍，且 §2.1 的历史窗口是**累积**上限，二者不能同为 4 MB；③ 保持 16 KB——就是当前缺陷 |
| 超限时**淘汰最旧完整轮次后重试**，而不是报错 | 与 §2.1 历史窗口的既有语义一致（T3 要求 UI 明示丢弃量）；用户读到的是「继续可用」而不是「不可用」 | 直接报错——把预算问题转嫁成用户操作负担 |
| 只有**单轮独占**超限才报错，并提示 `/new` | 淘汰自己会无限循环（把唯一的轮也淘汰）；此时唯一可行的动作是让用户缩小输入或开新会话 | 截断该轮内容——会静默改变用户发出的请求语义（P2 精神不允许） |
| 请求体缓冲改为 **heap `body`**，在**所有校验之后**分配 | 256 KB 放在栈上会顶爆线程栈；放在校验前会在失败路径泄漏 | 栈上 `char body[256*1024]`——Windows 默认 1 MB 线程栈，招致栈溢出 |
| 淘汰按 **`user` 边界**对齐（`core_next_turn`） | 「完整轮次」的语义就是「从一条 user 消息到下一个 user 之前」，保证不会出现「有 assistant 无 user」的畸形历史 | 按消息条数淘汰——会把一问一答劈开，模型可能拒绝 |
| 保存会话改用 `MoveFileExA(..., MOVEFILE_REPLACE_EXISTING)`，POSIX 保留 `rename()` | MSVC 的 `rename()` 在**目标已存在**时失败（POSIX 是原子覆盖）。只用 `rename()` 的后果：第 2 轮起每次保存都失败，文件永远停在第一轮，`/resume` 丢掉整段对话——正好撞 US-6 | ① 先 `remove()` 再 `rename()`——中间窗口会丢文件，崩溃就不止丢一轮；② 直接写目标文件——写一半崩溃留下半截 JSON |
| 落盘失败**说一次**（`g_save_warned`），不再静默 | 本缺陷能存活整轮开发，就是因为返回值被 `(void)` 掉；NFR 层面「以为存了其实没存」比「明确报错」危险得多 | 每轮都打印——长会话刷屏；保持静默——就是缺陷本身 |

## 3. 产出

| 文件 | 变更 |
|------|------|
| `include/cagent/core.h` | 新增 `CA_CORE_DEFAULT_CONTEXT_BYTES`(256 KB)/`CA_CORE_MIN_CONTEXT_BYTES`/`CA_CORE_MAX_CONTEXT_BYTES`；`cfg.context_bytes`；`ca_core.body/body_cap/context_notified`；声明 `ca_core_context_bytes()`、`ca_core_free()` |
| `src/core.c` | 删除 `CA_CORE_MAX_REQUEST`；`core_collect_msgs` + `core_next_turn`（按 user 边界淘汰）+ `core_one_turn` 重试循环；提示 `（上下文超出 N 字节上限，已丢弃最早的 M 条消息）`；单轮超限报错文案；`run_once` 每轮重置 `context_notified`；修正 `nmsg == 0`（无会话/空历史）被误判为 `ERR_LIMIT` 的回归 |
| `src/session_store.c` | Windows 分支加 `<windows.h>` + `MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)`；POSIX 保留 `rename()` |
| `src/main.c` | 解析 `CAGENT_CONTEXT_BYTES`（非法值提示并回落默认）；TTY 横幅增「上下文上限：N KB」；`--help` 增变量说明；退出时 `ca_core_free(&g_core)`；`save_session_quietly` 失败告警一次（替换原有的调试输出） |
| `tests/core_test.c` | `g_bodies[4][20480]`、`g_context_bytes`；重写 `core_reports_oversized_request_instead_of_truncating`；新增并登记 `core_trims_oldest_turns_when_over_budget`、`core_clamps_context_bytes` |
| `tests/session_test.c` | 新增并登记 `session_save_overwrites_previous_file`（两次保存 → 文件必须 4 条消息） |
| `scripts/mem_load.py` | 新增：本地 SSE + 真实二进制跑 500 轮，读系统峰值 RSS，断言轮数/会话文件非空/峰值 ≤ 100 MB |

**验证（本地，交付配置 + 真实 libcurl）**

- `build.ps1 -Config Release -BuildDir build-curl`：零警告、`ctest` **8/8**。
- `session_test`：**1731 次检查，0 失败**。
- **负向验证**（防「测试根本没覆盖缺陷」）：`git stash push -- src/session_store.c` → 重建 → `session_test` **在 `session_save_overwrites_previous_file` 上失败**（exit 1）；`git stash pop` → 恢复绿。这条测试在 Linux/macOS 上改前改后都绿——它守的是 **Windows 分支**，价值恰恰在此。
- 负向验证中发现并修掉**测试自身的缺陷**：断言失败不中断用例，`strcmp(ca_session_text(&back, 2), ...)` 在载入失败时对 NULL 解引用（`0xC0000005`），崩溃会掩盖失败原因 → 先判空再比较。
- 端到端：离线 `mem` 模式 3 轮 → 会话文件 **439 字节 / 6 条消息**（`user,assistant` × 3），`/resume` 语义成立。
- `mem_load.py --turns 500`：**500/500 请求、15.8 s、峰值 RSS 7.71 MB（预算 100 MB）、PASS**；会话文件 75048 字节，512 条消息，首条为「第 245 轮」——**淘汰按设计生效**。

## 4. 结论

两个缺陷都已修复并有回归测试守住；长会话从「约 50 轮后每轮失败」变为「持续可用且内存不增长」，会话落盘从「只有第一轮」变为「每轮都在」。完成并移交 G2。

## 5. 移交

- `reviewer`（G2）：重点看 `core_one_turn` 的重试是否会不收敛（结论：每次至少淘汰一个完整轮次，`nmsg` 严格下降，到 `nmsg == 0` 转为单轮报错）、`body` 的分配/释放路径、`MoveFileExA` 失败时是否仍保留旧文件（是：先写 tmp，替换失败则 tmp 被删，旧文件不动）。
- `test-engineer`（G3）：新增用例的**登记表**是否真实执行（`CA_TEST` 必须登记到文件底部表，否则静默不跑——本轮已发生一次）。

## 6. 复核（由独立复核者填写，非自证）

| 复核人 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| reviewer | 2026-10-21 | 通过 | 见 `2026-10-21-reviewer-gate-g2-long-session.md` |
| test-engineer | 2026-10-21 | 通过 | 见 `2026-10-21-test-engineer-gate-g3-long-session.md` |
