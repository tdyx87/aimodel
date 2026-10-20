# 审计：reviewer · session 层与多轮记忆 G2 审查

| 字段 | 内容 |
|------|------|
| 角色 / 时间 / 任务 | `reviewer` · 2026-10-20 · 审查会话层（`session.c` / `session_store.c`）、`main` 会话命令、`core` 多轮记忆接入 |
| 结论 | **✅ 通过**（附 §7 的两条条件性说明）——审查中发现 2 个 High 缺陷，已修复并各留回归用例 |

## 1. 输入

| 输入 | 路径 / 引用 |
|------|-------------|
| 需求（US-6） | `docs/prd/PRD-c-agent.md:93-99`（Given/When/Then + 验收） |
| 契约 | `docs/design/contract.md` §2.1 分层预算、§6 会话持久化、§8 跨平台边界、§9 违约判定（V2、V6） |
| 被审查代码 | `include/cagent/session.h`、`src/session.c`、`src/session_store.c`、`src/main.c`、`src/core.c`、`src/llm.c`、`src/json.c` |
| 被审查测试 | `tests/session_test.c`、`tests/core_test.c`、`scripts/session_smoke.py` |
| 验证环境 | 本机 Windows（VS2017 x64）+ CI 三平台，提交 `03e5902` 全绿 |

## 2. 审查方法（关键决策）

| 决策 | 理由 | 被否决的备选 |
|------|------|-------------|
| 按契约**逐条核对不变量**，而不是「测试过了就算过」 | 测试只覆盖想到了的路径；本层的核心不变量是 `used <= cap`，它必须**在任何路径**上成立，包括没人写过用例的边界 | 只看 `ctest` 绿色 —— 这正是漏掉 F1 的原因，见 §3 |
| 把**外部输入**单独列成一类来追 | 会话文件是本产品唯一「用户可编辑、且会回灌进内存」的输入：id、内容、数量、大小都不可信 | 只审正常路径 |
| 发现缺陷后**先写用例复现，再修**，并**临时回退修复确认用例确实变红** | 用例不变红 = 没复现到真问题，那样的「修复」只是改动 | 直接修完就算 |
| 拒绝把「内存检测工具」当作本层的主要防线 | 本层所有越界都发生在**我们自己定义的不变量**上，断言它比外挂工具更早、更便宜 | 依赖 ASAN/UBSAN —— 本地无可用的 MSVC 方案，且 CI 未启用 |

## 3. 发现的问题

| # | 严重度 | 问题 | 证据 |
|---|--------|------|------|
| F1 | **High** | `ca_session_append_delta` 在「末条助手消息续写到容量边界」时把结尾 NUL 写到 `buf[cap]`（越界 1 字节），并把 `used` 顶到 `cap+1`，**破坏核心不变量 `used <= cap`**。症状不是崩溃，而是静默堆损坏 | 复现用例 `session_append_delta_respects_cap_at_tail`：修复前该用例断言失败（`used=1025 > cap=1024`） |
| F2 | **High** | `ca_session_save` 用 `s->id` 直接拼文件名，而 `s->id` 可来自**已载入的文件**（`/resume` 后再保存），即外部输入。一个 id 为 `../../pwned` 的会话文件会让保存写到会话目录之外 | 复现用例 `session_save_does_not_follow_traversal_id`：修复前在临时目录之上落下 `pwned.json` |
| F3 | Low（工程性） | 测试内手拼 `"%s%c%s"` 路径触发 GCC `-Wformat-truncation`，会让 Linux 腿变红 | CI 注解（ubuntu-latest）；改用 `ca_session_path_for` 后消失 |

**F1 的成因值得记下**：本层所有存储都是 `[off, off+len]` 含结尾 NUL 的紧凑布局，
「本条能用多少字节」就必然是 `cap - tail - 1`，而不是 `cap - tail`。
少减的那个 1 在**正常长度下完全看不出来**——只有把末条恰好顶到边界才暴露。

## 4. 修复与验证

| 项 | 内容 |
|----|------|
| 修复提交 | `adad080`（F1、F2）、`53d78c7`（F3） |
| F1 修法 | 截断量改为 `cap - tail - 1`，并在注释中写明「这 1 格留给 NUL」的推导 |
| F2 修法 | 保存全程走 `ca_session_path_for`（校验只有一处）；id 可疑则换新 id 再存，**不让保存失败**——用户的对话不该因为文件名可疑而丢掉 |
| 回归用例 | `tests/session_test.c`：`session_append_delta_respects_cap_at_tail`、`session_save_does_not_follow_traversal_id`（后者先删同名残留，避免被历史残留误判） |
| 复现确认 | 修复后临时回退 `session_store.c` 的改动重跑：两个用例**都确实变红**，F2 的越界文件也确实落到目录之外 |
| 全量结果 | `session_test` 21 例 / 1714 检查 0 失败；`ctest` 8/8；`curl_smoke`、`session_smoke` 均 PASS；CI 三平台 `03e5902` 全绿 |

## 5. 未发现问题的部分（附核对依据，避免「没写就是没查」）

| 核对项 | 依据 |
|--------|------|
| 分层预算无一处放宽 | 会话历史 4 MB 来自 `CA_LIMIT_HISTORY_BYTES`；`session_load` 按预算封顶读取，不随文件大小增长 |
| 淘汰按整轮，不产生孤立助手回复 | `session_turn_end` + `session_compact`；用例 `session_evicts_oldest_turn_and_reports_it` |
| 淘汰必在界面明示（T3） | `ca_session_take_notice` + `core_report_session_notice`；用例 `core_tells_user_when_history_is_evicted` |
| 单条超预算显式截断且记账 | `session_truncates_single_oversized_message_explicitly`、`session_json_load_truncates_oversized_message` |
| 平台差异未外溢 | `session_store.c` 是唯一的 `#ifdef` 所在；`session.c`/`core.c`/`main.c` 内无平台宏（`main.c` 仅 getcwd 的既有宏） |
| 损坏文件不致命 | `session_corrupt_file_is_listed_not_fatal`；`/list` 标注而非静默跳过 |
| 密钥不落盘 | 结构上 `session` 只有消息文本；用例 `session_json_never_contains_key_field`、`session_saved_file_has_no_key` |

## 6. 移交

| 下游 | 依据 |
|------|------|
| `test-engineer` | 本审查的 F1/F2 修复与回归用例；测试审计需覆盖「不变量断言」这一层（见 §2） |
| `security-reviewer` | F2 属安全类问题（路径穿越），需按其口径重新定级并给出残留风险结论 |

## 7. 条件性说明（诚实声明）

1. **本审查未做动态内存检测**。F1 是靠**不变量断言**抓到的，不是 ASAN/UBSAN。
   这意味着「同一类越界若发生在没有断言覆盖的路径上，仍可能漏过」。
   建议（非本门禁条件）在 CI 增加 ASAN 构建；当前不阻塞交付。
2. **Linux/macOS 上的行为只有 CI 结论**（本机为 Windows）。
   `session_store.c` 的 POSIX 分支由 CI 编译并运行，但未在真实 macOS 机器上手工验证。

## 8. 复核（由独立复核者填写，非自证）

| 复核人 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| `orchestrator` | 2026-10-20 | **通过** | 已抽检：核对 §3 的 F1/F2 与 `tests/session_test.c` 中对应用例、`adad080` 的实际改动一致；F2 的「先红后绿」在本地复跑一次得到相同结论（见 `2026-10-20-orchestrator-gate-g2-g3-session.md` §2） |
