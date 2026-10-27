# 角色系统执行链路修复（发现 → 派发 → 留痕）

| 字段 | 内容 |
|------|------|
| 角色 | `orchestrator` |
| 时间 | 2026-10-27 |
| 任务 | role-system-execution |
| 输入 | 用户反馈「换模型后角色不再执行」；git 历史（`5135174` 角色在 `.claude/agents/`，`aad2853` 迁至 `.role-system/agents/`）；OpenCode V2 agents 文档 |
| 结论 | 完成并移交 |

## 1. 输入

- 用户报告：同一套角色方案，换模型后不再按流程执行。
- 证据链：
  - `git log --diff-filter=A -- .claude/*` → `5135174` 首次提交即存在 12 个 `.claude/agents/*.md`；
  - `git log --diff-filter=D -- .claude/agents/orchestrator.md` → `aad2853` 删除；
  - `git log --diff-filter=A -- .role-system/agents/orchestrator.md` → 同一提交新增（`git mv`）。
- 结论：角色从未在 OpenCode 的发现路径上；此前的「跑通」是 Claude Code 在加载。

## 2. 关键决策

- **D1 事实源与生成物分离**：`.role-system/` 只存中性文档与 `manifest.json`；
  `.opencode/agents/`、`.claude/agents/` 由 `scripts/gen_agents.py` 生成。
  理由：客户端语法一旦写进事实源，换客户端就会重演本次事故。
- **D2 能力用三值中性词**：`read-only` / `read-write` / `dispatch`，由生成器翻译成
  `permissions`（OpenCode）与 `tools` 逗号串（Claude Code）。
  否决「保留 `tools:` 字符串」——那是 Claude Code 语法，OpenCode 不认。
- **D3 派发去私有工具化**：删除 `orchestrator` 对 `team_*` 的依赖，改为
  `invocation.md` 的四个抽象动作。否决「引入 team MCP」——把通用系统绑死在私有工具上。
- **D4 强制落到文件系统**：`check_gates.py --diff` 在代码有改动时要求新增审计。
  理由：这是唯一与模型强弱无关的约束。
- **D5 老账不追溯**：历史 42 份审计只校验命名；四要素只对新增审计生效。
  理由：历史审计字段不统一，追溯会让门禁第一天就红。

## 3. 产出

- `.role-system/manifest.json`（12 角色，能力/路径/可派发）
- `.role-system/invocation.md`（调用契约 + 三环境映射 + 交付硬规则）
- `scripts/gen_agents.py`（生成 + `--check`）
- `scripts/check_gates.py`（命名校验 + 区间门禁）
- `.opencode/agents/*.md`、`.claude/agents/*.md`（各 12 个，生成物）
- `.role-system/agents/*.md`（剥离 frontmatter；`orchestrator.md` 重写去 `team_*`）
- `AGENTS.md`（路由硬规则、目录说明、自检命令）
- `.github/workflows/cagent.yml`（新增 `role-gates` 作业）
- `product/docs/adr/ADR-0007-role-system-execution.md`

## 4. 结论

完成。三条验收判据均已具备可执行形式：

1. 12 个角色在 `.opencode/agents/` 与 `.claude/agents/` 中可见；
2. `reviewer` 的 `permissions` 为 edit deny（仅审计目录 allow）；
3. 删一份审计或改代码不补审计 → `check_gates.py` 退出码 1（已在临时 worktree 实测）。

## 5. 移交

- 交 `reviewer`（G2）：审查生成器与门禁脚本的逻辑与边界。
- 交 `security-reviewer`：确认生成物未扩大权限（尤其只读角色）。
- 交 `release-engineer`：确认 CI 的 `role-gates` 作业不会误伤三平台构建。

## 6. 复核（待独立复核者填写）

| 复核者 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| | | | |
