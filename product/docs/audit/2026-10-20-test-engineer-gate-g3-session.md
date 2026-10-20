# 审计：test-engineer · 会话层与多轮记忆 G3 测试

| 字段 | 内容 |
|------|------|
| 角色 / 时间 / 任务 | `test-engineer` · 2026-10-20 · 会话层（US-6）与 `core` 多轮记忆的单测 + 端到端冒烟 |
| 结论 | **完成并移交**：单测与 E2E 全绿，三平台 CI 通过 |

## 1. 输入

| 输入 | 路径 / 引用 |
|------|-------------|
| 需求与验收 | `docs/prd/PRD-c-agent.md` US-6、US-7；NFR-1（100 MB，ADR-0003） |
| 契约测试要求 | `docs/design/contract.md` §10、§2.1（淘汰规则）、§6 |
| 被测代码 | `src/session.c`、`src/session_store.c`、`src/core.c`、`src/main.c` |
| 上游审计 | `2026-10-20-reviewer-gate-g2-session.md`（F1/F2 及其回归用例） |
| 验证环境 | 本机 Windows VS2017 x64；CI：ubuntu-latest / macos-latest / windows-latest，提交 `03e5902` 全绿 |

## 2. 测试产出与结果

| 层 | 载体 | 结果 |
|----|------|------|
| 单元 | `tests/session_test.c` | **21 例 / 1714 次检查，0 失败**（其中 1 例为 POSIX-only） |
| 单元 | `tests/core_test.c` | 72 次检查，0 失败（含多轮记忆 2 例） |
| 全量 | `ctest` | **8/8 通过**（含本次新增的 `session_test`） |
| E2E（真实传输） | `scripts/curl_smoke.py` | PASS：真 socket、真 POST、`Authorization` 正确、输出渐进到达 |
| E2E（会话命令） | `scripts/session_smoke.py` | PASS：7 组断言（见 §3） |
| 三平台 | GitHub Actions | ubuntu / macos / windows 三腿全绿（`03e5902`） |

## 3. 逐条覆盖（US-6 的 Given/When/Then 与验收）

| 验收要求 | 覆盖方式 | 用例 |
|----------|----------|------|
| 以文件形式落盘 | 真实子进程写盘 + 读回 | `session_saves_lists_loads_and_deletes`、`session_smoke.py [1][2]` |
| 可列出 | 扫描目录、按 mtime 新到旧 | `session_list_sorts_newest_first`、`session_smoke.py [2]` |
| 可恢复 | 载入后历史延续（再问一句变成 3 条） | `session_saves_lists_loads_and_deletes`、`session_smoke.py [3]` |
| 可删除 | 删除后列表为空 | `session_smoke.py [5]`；`session_delete_refuses_path_traversal` |
| **损坏文件不应导致无法启动** | 手工写入截断 JSON，`/list` 仍返回 2 条并标注坏文件 | `session_corrupt_file_is_listed_not_fatal`、`session_smoke.py [4]` |
| 淘汰必明示（T3） | notice 字段 + 界面文案 | `session_evicts_oldest_turn_and_reports_it`、`core_tells_user_when_history_is_evicted` |
| 预算不越界（V6） | 500 轮灌入 + 边界续写 | `session_never_exceeds_budget_under_flood`、`session_append_delta_respects_cap_at_tail` |
| 密钥不落盘（NFR-3） | 断言文件内容无 `api_key`/`Authorization` | `session_saved_file_has_no_key`、`session_json_never_contains_key_field` |
| 私人内容不公开（G3 新增） | POSIX 断言文件 mode 无 group/other 读位 | `session_file_is_not_world_readable`（CI 的 Linux/macOS 腿执行） |

**E2E 为什么必须独立成脚本**：单测证明的是函数，证明不了
「命令行真的能存、换一个进程还找得到」。`session_smoke.py` 用真实可执行文件 +
真实子进程跑 7 组断言，并把 `APPDATA`/`XDG_CONFIG_HOME` 重定向到临时目录，
所以它既验证了落盘链路，又不会污染用户真实的配置目录。

## 4. 未覆盖 / 局限（不得据此声称已覆盖）

| 项 | 说明 |
|----|------|
| 真实第三方 API | 无凭据；已证实的只有本地 SSE 服务与 CI 上的真实 libcurl 请求 |
| Linux/macOS 手工验证 | 本机为 Windows，两平台只有 CI 结论 |
| 动态内存检测 | 未运行 ASAN/UBSAN。F1（越界写）靠**不变量断言** `used <= cap` 抓到——这是本层的主要防线，但它只覆盖写了断言的路径 |
| 活跃/超长会话下的 RSS | A1 目前只有 `mem_probe` 的空闲基线；没有「历史堆到接近 4 MB 时」的实测数字，NFR-1 的活跃场景仍**未测** |
| US-1/US-2 时延 | <500 ms 响应、<2 s 首字目标无实测数据 |

## 5. 关键决策与理由

| 决策 | 理由 | 被否决的备选 |
|------|------|-------------|
| 会话测试用**真实文件系统**（临时目录），不用桩 | 落盘层的主要风险正是平台差异（目录、分隔符、mtime、权限），桩会把它们全部抹掉 | 用内存假文件系统 —— 恰好绕开唯一值得测的部分 |
| E2E 用真实子进程驱动 stdin/stdout | 「换个进程还能恢复」是本需求的核心，进程内调用测不出来 | 只在测试里直接调 `ca_session_load` |
| 断言**排序不变量**（mtime 不递增）而不是断言具体时间 | 秒级时间戳在同秒内不可区分，断言具体值会随机器抖动（契约 §7.4：断言不变量不断言用例特有数值） | 断言精确顺序 —— 会偶发变红 |
| 越界探针先删同名残留再断言 | 带 bug 的历史运行会留下 `pwned.json`，不清掉会把下一次运行误判成「本次越界」 | 只断言文件不存在 —— 已被实际误判过一次 |

## 6. 移交

| 下游 | 依据 |
|------|------|
| `security-reviewer` | 本审计的覆盖矩阵（尤其 §3 的安全相关两行）与 §4 的局限 |
| `doc-engineer` | US-6 的用户可见行为：`/new` `/list` `/resume` `/delete`、退出提示、以及「会话不落盘时的降级提示」必须写进用户文档 |

## 7. 复核（由独立复核者填写，非自证）

| 复核人 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| `orchestrator` | 2026-10-20 | **通过** | 抽检：随机挑 3 条 §3 的覆盖行，逐个在源码中找到对应用例并本地复跑（`session_test` 21 例 0 失败、两个冒烟脚本 PASS）；§4 的「未覆盖」清单与契约 §2.5.6 的「尚未成立的能力」一致，未发现夸大 |
