# ADR-0007：角色系统采用「事实源 + 生成物 + 机器门禁」

- 状态：已接受（2026-10-27）
- 相关：`AGENTS.md`（路由规则）、`.role-system/agents.md` §2/§3/§6.6、`.role-system/invocation.md`、`.role-system/manifest.json`
- 触发：用户报告「同一套角色方案，换模型后就不再执行」

## 背景

角色系统最初能跑通，是因为 12 个角色文件当时位于 `.claude/agents/*.md`
——那是 Claude Code 的发现路径，由客户端自动加载。

提交 `aad2853`（`refactor: 按变与不变分三层目录`）用 `git mv` 把它们搬进
`.role-system/agents/`。分层本身正确，但**副作用是把角色迁出了所有客户端的
发现路径**：OpenCode V1/V2 的发现路径是 `.opencode/agent|agents/`，
Claude Code 是 `.claude/agents/`，两者都不认识 `.role-system/`。

此后唯一约束只剩 `AGENTS.md` 里的说明文字。强模型会照着做，弱模型直接跳过，
于是表现为「换模型就不执行」。这不是模型问题，而是**执行完全依赖提示词自觉**。

另有两个叠加缺陷：

1. 角色 frontmatter 是 Claude Code 语法（`tools:` 逗号串、`model: inherit`），
   即使原样放回 `.opencode/agents/`，OpenCode 也不会按它限制权限——
   `reviewer` 理论上能改代码。
2. `orchestrator` 依赖 `team_run_task` / `team_task` 等工具，那是某个 MCP 的
   私有能力，本环境不存在，派发必然失败。

## 决策

把角色系统拆成三层，各司其职：

| 层 | 内容 | 是否手改 |
|----|------|---------|
| 事实源 | `.role-system/agents/*.md`（职责正文）、`roles.md`、`manifest.json`、`invocation.md` | ✅ 只改这里 |
| 生成物 | `.opencode/agents/*.md`、`.claude/agents/*.md` | ❌ 由脚本生成 |
| 强制 | `scripts/check_gates.py` + CI `role-gates` 作业 | ❌ 机器执行 |

具体规则：

1. **事实源去客户端化**：`agents/*.md` 不再含 frontmatter；能力用中性词
   `read-only` / `read-write` / `dispatch` 写在 `manifest.json`。
2. **客户端语法只存在于生成器**：`scripts/gen_agents.py` 把同一份 manifest
   翻译成各客户端 frontmatter，`--check` 检出漂移。
3. **调用契约中性化**：`invocation.md` 定义 `dispatch` / `await` / `message` /
   `collect`，并给出 OpenCode、Claude Code、无子代理三种映射。
4. **强制落在文件系统上**：每次派发必须产出
   `product/docs/audit/<date>-<role>-<task>.md`；`check_gates.py --diff` 在
   代码有改动时要求同区间新增审计。跳过流程 → CI 失败，与模型强弱无关。
5. **老账不追溯**：历史审计只校验命名与角色合法性；内容四要素只对新增审计生效。

## 理由

- **可发现性必须由目录保证**，不能靠模型记得读文档。
- **能力约束必须由权限系统保证**，不能靠角色描述里的「我是只读的」。
- **流程必须留下可机检的痕迹**，否则「跑没跑」永远只能靠人问。
- 事实源与生成物分离，使「换客户端」只需改生成器，角色定义一个字不动——
  这正是用户要的「通用文档」。

## 后果

- 好处：角色在 OpenCode V2 与 Claude Code 下都能被发现；`reviewer` 的只读
  约束成为权限而非约定；流程跳过会在 CI 暴露。
- 代价：新增两个脚本与一份 manifest，改角色需多跑一次生成命令；
  生成物入库（24 个文件），评审时需注意「不要手改」。
- 代价：`check_gates.py` 只能验证「审计存在且字段齐」，无法判断审计内容是
  否诚实。因此审计仍须 `orchestrator` 抽检复核（`agents.md` §6.6 既有要求）。

## 备选与否决理由

- **把角色文件直接放回 `.claude/agents/`**：只恢复了一个客户端；OpenCode 仍
  不可见，且权限字段格式不对。治标不治本。
- **在 `AGENTS.md` 里写更强硬的提示词**：执行仍取决于模型是否服从，正是本次
  要消除的依赖。
- **用符号链接把 `.role-system/agents/` 链进客户端目录**：Windows 需要管理员
  权限，且无法解决 frontmatter 语法差异；生成器更可控。
- **保留 `team_*` 并引入对应 MCP**：把通用角色系统绑死在一个私有工具上，
  与「通用文档」目标冲突。
