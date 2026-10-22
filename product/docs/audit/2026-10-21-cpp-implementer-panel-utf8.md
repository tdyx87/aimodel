# cpp-implementer — 用户实测第二轮：面板内容落框 + 中文文件名

| 字段 | 内容 |
|------|------|
| 角色 / 时间 / 任务 | cpp-implementer / 2026-10-21 / 处理用户实测截图暴露的三个问题：空盒子、结果掉到框外、中文文件名乱码 |
| 结论 | 完成并移交（`reviewer` 审 G2） |

## 1. 输入

- 用户实测输出（TTY）：`┌─ 调用工具 list_dir ─` / `└─────────────────────` 之后**没有内容**，
  `<result ...>` 整段掉在框外；`list_dir` 结果里的中文文件名是乱码（`�½��ı��ĵ�.txt`）。
- 契约 §5.1、§5.2 P1/P2/P5、§8（平台差异收拢）；界面稿 §11。

## 2. 关键决策

| 决策 | 理由 | 被否决的备选 |
|------|------|--------------|
| 面板**在工具执行后**才画，标题带「· 结果」，正文是工具输出 | 之前是「先画空盒子 → 再在框外打印结果」，看起来像渲染坏了。改成：批准时画「等待批准」面板（正文是待批准内容），执行后画「结果」面板（正文是工具输出） | 执行后仍用裸文本——那正是用户嫌乱的形态 |
| 批准提示也进面板 | 待批准内容与「这是什么操作」绑在一起，用户不会把它当成上一条消息的尾巴 | 保持裸文本——用户反馈里就是这种形态 |
| 去掉「先画空面板」那一步 | 空盒子没有任何信息，只会让人以为渲染出错 | 保留空盒作为「正在调用」的预告——但它与批准面板会重复 |
| Windows 目录列举改用**宽字符 API**（`FindFirstFileW` + `WideCharToMultiByte(CP_UTF8)`） | 窄字符 API 返回的是**系统 ANSI 代码页**字节（中文机器是 GBK），与本程序通行的 UTF-8 不兼容，名字就成了乱码 | 只修输出编码——名字本来就是错的，改不动 |
| 新增 `ca_tool_fopen()`：Windows 上 UTF-8 → UTF-16 后 `_wfopen` | 光修列举不够：**写**和**读**同样用 fopen（系统代码页），中文路径会失败或建出乱码名字；用户会遇到「我让它写文件，它说找不到」 | 让用户避免中文文件名——用户的目录里本来就有中文名 |
| `search` 的目录遍历也换宽字符 API | 否则搜中文名文件时匹配的是乱码，等于搜不到 | 只修 list_dir——search 同样会漏 |
| 不用 `windows.h` 写测试 fixture，改用产品自己的 `ca_tool_fopen` | `windows.h` 把 `small` 定义成宏，会把无关代码里的 `small` 变量改成 `CHAR`（实测编译报 C2632） | 在测试里 include windows.h——踩过坑 |
| fixture 必须用**宽字符**创建 | 用 fopen 写 UTF-8 字节名，建出来的根本不是同一个名字，测的就不是「按 UTF-8 输出」这件事 | 用 fopen——fixture 本身就是错的 |

## 3. 产出

| 文件 | 变更 |
|------|------|
| `src/ui.c` | `ca_ui_confirm` 在 TTY 下把待批准内容放进盒面板（标题「调用工具 X · 等待批准」） |
| `src/core.c` | 去掉「先画空面板」；执行后画「调用工具 X · 结果」面板，正文是工具输出 |
| `src/tools.c` | `ca_tool_fopen()`（UTF-8 路径 → 宽字符打开）；`list_dir` 改用 `FindFirstFileW` + UTF-8 转换；`read_file`/`write_file` 改用 `ca_tool_fopen` |
| `src/tools_edit.c`、`src/tools_shell.c` | 文件打开改用 `ca_tool_fopen`；`search` 的目录遍历改用宽字符 API |
| `include/cagent/tools.h` | 声明 `ca_tool_fopen()` |
| `tests/tools_test.c` | 新增 `list_dir_prints_chinese_name_as_utf8`、`write_and_read_chinese_filename_roundtrip`；工作区 fixture 增加中文名文件 |

**验证**

- 本地：`ctest` **8/8**；`tools_test` **124 检查 0 失败**（新增 2 例）；`ui_test` 63 检查 0 失败；零告警。
- 新用例能证伪：修复前 `list_dir_prints_chinese_name_as_utf8` 红（名字是乱码），
  `write_and_read_chinese_filename_roundtrip` 红（写入失败）。
- 真实服务回归：工具调用 → 批准 → 文件真的落盘，仍通过。

## 4. 结论

用户截图里的三个问题都定位到根因并修掉：面板现在**装着内容**、中文文件名按 UTF-8 正确显示与读写。
完成并移交 G2。

## 5. 移交

- `reviewer`（G2）：重点看 ① 批准内容是否仍**在等待输入之前**完整写出（P2/P5）——面板化不能削弱这一点；
  ② `ca_tool_fopen` 的模式串与缓冲区长度处理。
- `test-engineer`（G3）：中文名用例在 POSIX 上是否同样成立（CI 三个平台都会跑）。

## 6. 复核（由独立复核者填写，非自证）

| 复核人 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| reviewer | 2026-10-21 | 通过 | 见 `2026-10-21-reviewer-gate-g2-panel-utf8.md` |
| test-engineer | 2026-10-21 | 通过 | 见 `2026-10-21-test-engineer-gate-g3-panel-utf8.md` |
