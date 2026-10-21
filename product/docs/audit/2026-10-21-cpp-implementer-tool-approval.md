# 审计：工具批准通道（实现）

| 字段 | 内容 |
|------|------|
| 角色 / 时间 / 任务 | `cpp-implementer` / 2026-10-21 / 打通工具批准通道，让读写/命令在用户同意后真正可用；顺带补齐 `--version` 与构建脚本 |
| 结论 | 完成并移交 |

## 1. 输入

- `docs/prd/PRD-c-agent.md`：US-3/US-4/US-5（工具可用）、NFR-3（密钥不落盘）、NFR-1（内存）
- `docs/design/contract.md` §5.2（P1 默认拒绝 / P2 展示完整参数 / P3 拒绝可继续 / P5 命令原文）、§7 降级
- 既有实现：`src/core.c`（`core_perm`）、`src/tools*.c`、`src/main.c`、`src/ui.c`

## 2. 关键决策

1. **`cfg.perm` 不能为 NULL**。`core_perm` 在 `perm == NULL` 时的语义是「提示照出、一律拒绝」，
   于是用户按 `y` 也不执行。新增 `main_perm`：用户明确同意后放行文件类工具；
   `run_shell` 额外要求 `CAGENT_ALLOW_SHELL=1`（`"0"` 与空串视为未开）。
   理由：把「默认拒绝」当成「默认不可用」是产品缺陷，不是安全特性。
   被否决的备选：`perm == NULL` 时直接放行——会把默认拒绝变成默认允许，违反 P1。
2. **`write_file` 在问权限前解析参数**，并把目标路径与内容交给 `pending_display`。
   理由：P2 要求展示完整参数；解析无副作用，有副作用的写盘仍在批准之后。
   被否决的备选：保持现状（只问「write_file 请求」）——那是盲批，用户无法判断。
3. 内容超过 2048 字节只展示开头并**显式标注**总长度与省略量。理由：1 MB 全量刷屏无信息价值；
   不标注的截断才是欺骗。此偏差已写入契约 §5.2 注释。
4. 等待提示（`ca_ui_wait_begin/end`）只在 TTY 生效，且任何真实输出到来时先擦除占位行。

## 3. 产出

- `src/main.c`：`main_perm` + `CAGENT_ALLOW_SHELL` + 启动显示工作区/会话目录 + `/help` 说明
- `src/tools.c`：`ca_tool_write_file` 先解析、展示、再问权限、再落盘
- `src/ui.c`、`include/cagent/ui.h`、`src/core.c`：等待提示
- `tests/core_test.c`：3 个批准通道用例；`tests/tools_test.c`：2 个展示用例；`tests/ui_test.c`：3 个等待提示用例
- `scripts/tool_smoke.py`（真实二进制 + 本地 SSE）、CI 新增一步
- `scripts/build.ps1`、`scripts/build.sh`、`CMakeLists.txt` 版本注入、`main.c --version/--help`

## 4. 复核（由独立复核者填写，非自证）

| 复核人 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| reviewer（单人模式，同一会话切换角色） | 2026-10-21 | 通过（修复后） | 判为**条件不通过**一次：`write_file` 未提供待批准内容。修复并补用例后复核通过，详见 `2026-10-21-reviewer-gate-g2-tool-approval.md` |

> 单人模式下本行属于自证：同一操作者切换角色。已在 orchestrator 审计中如实记录该局限。
