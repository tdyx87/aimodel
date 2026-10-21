# 审计：用户文档（doc-engineer，G4）

| 字段 | 内容 |
|------|------|
| 角色 / 时间 / 任务 | `doc-engineer` / 2026-10-21 / 编写 `product/README.md` 用户文档（G4 文档门禁） |
| 结论 | 完成并移交 |

## 1. 输入

- `docs/prd/PRD-c-agent.md`（US-1~US-8、NFR-1/3）
- `docs/design/contract.md` §5.2、§6、§7
- `product/design/ui-terminal.md`
- 上游审计：cpp-implementer / reviewer G2 / test-engineer G3 / security-reviewer G3 / ui-designer

## 2. 关键决策

1. **「已验证 / 未验证」两栏并列**，不写「基本完成」。理由：未验证项写成完成，是把风险转嫁给用户。
   未验证项包括：真实第三方 HTTPS、活跃会话 RSS、首字延迟、Linux/macOS 手工交互、US-8。
2. 会话文件位置与**明文**性质、POSIX `0600`、不含 API key——都写进 README 而不是只留在契约里。
   理由：这直接影响用户该不该在受控目录里用它。
3. 批准语义写清「只认 y」「回车不算同意」「命令需额外开关」，并给出真实输出示例。
4. 构建入口以**脚本**为主（`build.ps1` / `build.sh`），同时给出手工 CMake 命令，便于排查。
5. 事实核对方式：所有命令与文案都在本机实跑/读代码核对过；`--version` 与 `--help` 单独验证不依赖环境变量。

## 3. 产出

- `product/README.md`：构建、配置表、使用与批准示例、工作区边界、命令表、会话与隐私、
  非 TTY 降级、已验证/未验证、测试命令、目录结构
- 文档中每条「已验证」都能对应到 CI 步骤或本地实跑记录

## 4. 结论

G4 的「用户文档完成」满足。**未验证项已显式列出**，不宣称完成。

## 5. 移交

交给 `release-engineer` 做发布检查单。

## 6. 复核（由独立复核者填写，非自证）

| 复核人 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| release-engineer（单人模式） | 2026-10-21 | 通过 | 按 README 从零构建一次（`build.ps1 -BuildDir build-doc`）成功；`--version` 输出与文档一致 |
