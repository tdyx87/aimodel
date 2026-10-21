# 审计：工具批准通道（reviewer，G2）

| 字段 | 内容 |
|------|------|
| 角色 / 时间 / 任务 | `reviewer` / 2026-10-21 / 审查批准通道与构建脚本改动（G2 合入门禁） |
| 结论 | ✅ 通过（首轮**不通过**，缺陷修复后复核通过） |

## 1. 输入

- 改动：`src/main.c`、`src/tools.c`、`src/ui.c`、`include/cagent/ui.h`、`src/core.c`、
  `tests/{core,tools,ui}_test.c`、`scripts/tool_smoke.py`、`scripts/build.{ps1,sh}`、`CMakeLists.txt`
- 基准：`docs/design/contract.md` §5.2、§7、§8；`design/ui-terminal.md`
- 上游审计：`2026-10-21-cpp-implementer-tool-approval.md`

## 2. 关键决策（审查结论与理由）

| # | 发现 | 严重度 | 处置 |
|---|------|-------|------|
| R1 | `main_perm` 放行所有工具会在 `CAGENT_ALLOW_SHELL` 未设时也放行命令 | 高 | **误报**：`run_shell` 有独立分支拒绝，有用例 `core_policy_may_veto_after_user_approves` 钉住 |
| R2 | `ca_tool_write_file` 未设置 `pending_display`，用户看到「write_file 请求。允许执行？」即盲批（违反 P2/P5） | **高** | 首轮判**不通过**。实现改为先解析参数、展示路径+内容（超长显式标注）再问权限；补 `write_pending_display_shows_path_and_content` / `..._marks_truncation` |
| R3 | 新用例只写了 `CA_TEST` 未登记到手工用例表，**静默不跑**（检查数不变，看不出来） | 中 | 已登记；在表头写明「手工维护，新增必须登记」 |
| R4 | `core_write_tool_is_denied_by_default` 的探针拼的是 `"\new.txt"`（盘符根），断言「不存在」**永远成立**——即使工具真写了也发现不了 | 中 | 改为按工作区根探测；顺带发现该用例此前的「通过」没有证明力 |
| R5 | `ui_clear_waiting` 里 `"\r\x1b[2K"` 实为 **5** 字节，代码写死 4，擦行序列被截断成 `ESC[2`，会吃掉后续内容首字符 | 中 | 改用 `sizeof(erase)-1`；用例 `ui_wait_hint_is_erased_by_first_output` 抓到 |
| R6 | 批准后 `write_file` 先解析内容（上限 1 MB 的 malloc）再问权限，拒绝路径多一次分配 | 低 | 可接受：分配失败返回明确错误，且不再有「先执行后询问」 |
| R7 | 启动横幅显示工作区/会话目录仅在 TTY，管道输出保持干净 | 低 | 符合 §7 降级，无需改 |
| R8 | `scripts/build.sh` 模式为 100644（无执行位），文档写的 `./scripts/build.sh Release` 在全新 Linux clone 上 `Permission denied` | 中 | 由新增的 CI「文档入口冒烟」首跑暴露；`update-index --chmod=+x` → 100755，复跑绿（`3873bf5`） |

## 3. 产出（结论回到产物）

- R2/R3/R4/R5 的修复与用例：提交 `782614f`（tools 展示）、`069c035`（批准通道 + 构建）、`8c1e7b0`（等待提示）；R8：`3873bf5`
- 复查命令与结果：`ctest --test-dir build-curl -C Release` → **8/8**；
  `core_test` 18 例 / 78 检查；`tools_test` 115 检查；`ui_test` 50 检查；零告警
- 三个冒烟：`curl_smoke` PASS / `session_smoke` PASS / `tool_smoke` PASS
- CI：`069c035` 在 ubuntu / macos / windows 三平台全绿

## 4. 结论

通过。首轮的 R2 属于**功能性**缺陷（批准界面形同虚设），不是风格问题；修复后以用例可复现地验证。
R4 揭示了一个更值得记录的问题：**断言写在错误的路径上，测试会永真**——这类「假通过」比失败更危险，
建议后续对「断言文件不存在」的用例统一走工作区根探针。

## 5. 移交

交给 `test-engineer` / `security-reviewer` 过 G3。依据：本审计、上述用例与冒烟输出。

## 6. 复核（由独立复核者填写，非自证）

| 复核人 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| orchestrator（单人模式） | 2026-10-21 | 通过 | 核对 `pending_display` 确在 `check_perm` 之前赋值；探针改为 `ws_file_exists()` 后本地复跑通过 |
