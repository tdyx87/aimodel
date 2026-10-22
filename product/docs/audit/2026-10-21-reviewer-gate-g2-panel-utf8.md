# reviewer — G2 合入评审（面板内容 / 中文路径）

| 字段 | 内容 |
|------|------|
| 角色 / 时间 / 任务 | reviewer / 2026-10-21 / 评审「工具面板内容化 + Windows UTF-8 路径处理」 |
| 结论 | ✅ 通过（R1–R9） |

## 1. 输入

- 实现审计：`docs/audit/2026-10-21-cpp-implementer-panel-utf8.md`
- 契约 §5.1、§5.2 P1/P2/P5、§8；界面稿 §11

## 2. 审查清单

| # | 检查项 | 结论 | 依据 |
|---|--------|------|------|
| R1 | 批准内容是否仍在**等待输入之前**完整写出 | 通过 | `ca_ui_confirm` 先画面板（正文即待批准内容）再读按键；`ui_confirm_shows_full_text_before_waiting` 仍绿。面板化没有削弱 P2/P5 |
| R2 | 面板是否折叠/截断正文 | 通过 | 逐行输出，超长行退化为原样输出（不截断）；`ui_panel_keeps_every_body_line` 守着 |
| R3 | 非 TTY 路径是否仍然干净 | 通过 | 面板只在 `is_tty` 下画；非 TTY 仍是 `[调用工具 x]` + 原始文本，`ui_plain_mode_never_emits_decoration` 守着 |
| R4 | 是否引入**新的编码不一致** | 通过 | 边界统一：路径内部一律 UTF-8 字节串，只有在调用 Windows API 的那一刻转 UTF-16，输出再转回 UTF-8 |
| R5 | `ca_tool_fopen` 的模式串与长度处理 | 通过 | 模式串逐字节转宽字符并留 NUL；路径缓冲 `CA_PATH_MAX*2`，`MultiByteToWideChar` 失败即返回 NULL（不越界） |
| R6 | 宽字符遍历是否仍遵守「跳过 . 与 ..」 | 通过 | 转换后再比较 `"."`/`".."`（POSIX 分支不变） |
| R7 | `search` 是否仍会遍历工作区外 | 通过 | 遍历基址来自 `ca_ws_resolve`（工作区内），未改判定逻辑 |
| R8 | 是否有**新的动态分配/无界缓冲** | 通过 | 全部定长栈缓冲；`WideCharToMultiByte` 有显式目标长度 |
| R9 | 告警 | 通过 | 零告警。过程中踩到 `windows.h` 把 `small` 定义成宏导致 C2632——测试改为不 include `windows.h`，改用产品自己的 `ca_tool_fopen` |

## 3. 遗留（记录，不阻塞）

- POSIX 上路径本来是字节串，无需转换；本次改动对 POSIX 是 no-op（`ca_tool_fopen` 直接 fopen）。
- `edit_file` 的临时文件路径已改用 `ca_tool_fopen`，但**临时名**仍由 ASCII 组成（`*.cagent_tmp`），无编码问题。
- Windows 的符号链接检查仍只做词法判断（POSIX 才做 realpath）——这是既有限制，本轮未扩大范围。

## 4. 结论

三项用户可见问题（空面板、内容掉出框、中文名乱码）都按根因修复，
平台差异仍然收拢在 Windows 分支内，没有泄漏到工具语义。✅ 通过，放行 G3。

## 5. 移交

`test-engineer` + `security-reviewer`（G3）：重点看中文名用例在三个平台上的表现，
以及 `ca_tool_fopen` 是否引入路径穿越风险（它只做打开，不做校验——校验仍在 `ca_ws_resolve`）。

## 6. 复核（由独立复核者填写，非自证）

| 复核人 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| orchestrator | 2026-10-21 | 通过 | 抽查 `ca_tool_fopen` 与两处宽字符遍历；确认 `.role-system/` 与根 `AGENTS.md` 零改动 |
