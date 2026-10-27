# 调用契约（Invocation Contract）

> 本文件属**不变层**，只描述与客户端无关的调用语义。
> 客户端专有语法（frontmatter、工具名、目录）由 `scripts/gen_agents.py` 从 `manifest.json` 生成，**不写在这里**。

---

## 1. 为什么需要这一层

角色定义（`roles.md`）回答「谁负责什么」，但它本身**不会被任何工具自动加载**。
角色要真的被执行，必须同时满足三件事：

| # | 条件 | 归属 | 失效表现 |
|---|------|------|---------|
| 1 | **可发现**：角色出现在客户端的 agent 发现路径 | 适配层（生成物） | 模型根本不知道有这个角色 |
| 2 | **可派发**：编排者能启动子角色，或能明确退化为串行 | 本文件 | 角色写了但永远不运行 |
| 3 | **可验证**：产物与结论落盘，能被机器检查 | 审计规范 §6.6 + `scripts/check_gates.py` | 跑没跑无人知道 |

> 只靠提示词「请按流程走」不构成条件 1~3。换一个模型就可能全部失效——这是本契约存在的理由。

---

## 2. 四个中性动作

所有派发行为都归结为这四个动作。它们**不绑定任何工具名**。

| 动作 | 语义 | 输入 | 输出 |
|------|------|------|------|
| `dispatch(role, task)` | 把一项任务交给某个角色执行 | 角色 id、任务 slug、输入路径、完成判据 | run 句柄 |
| `await(run)` | 等待该角色结束（前台）或稍后取回（后台） | run 句柄 | outcome |
| `message(run, msg)` | 补充上下文，或驳回要求重做 | run 句柄、消息 | — |
| `collect(run)` | 取回产物清单、审计路径与裁决 | run 句柄 | 见 §4 |

`dispatch` 必须携带四要素，缺一不可：

1. **role**：`manifest.json` 中存在的角色 id；
2. **task**：短 slug（小写、连字符），用于审计文件命名；
3. **input**：上游产物路径（PRD、契约、界面稿、上一轮审计）；
4. **done**：完成判据（哪一个门禁、要求什么结论）。

---

## 3. 环境映射

同一个动作在不同环境下的落地方式不同。**适配层负责翻译，角色定义不关心。**

| 动作 | OpenCode V2 | Claude Code | 无子代理环境（串行模式） |
|------|-------------|-------------|------------------------|
| `dispatch` | `subagent` 工具，`resource` = 角色 id | `Task` 工具，`subagent_type` = 角色 id | 人工切换到该角色，新开一轮对话 |
| `await` | 前台子会话直接返回；后台用任务句柄 | Task 返回即结束 | 人工等待本轮结束 |
| `message` | 对子会话继续输入 | 重新发起 Task 并附上下文 | 人工转述 |
| `collect` | 读取子会话最终回复 | 读取 Task 结果 | 人工汇总 |

**注册**：`.opencode/agents/<id>.md` 与 `.claude/agents/<id>.md` 都是生成物，
由 `scripts/gen_agents.py` 从 `manifest.json` 产出。**不要手改生成物**，改事实源后重新生成。

**能力约束**（`manifest.json` 的 `capability`）决定生成物的权限：

| capability | 含义 | 生成到客户端的约束 |
|-----------|------|------------------|
| `read-only` | 只审查、不改文件 | 禁止 edit；shell 默认询问 |
| `read-write` | 只在自己负责的目录内写 | edit 仅允许 `paths` 白名单；shell 允许 |
| `dispatch` | 只编排、不写产物 | 禁止 edit；只能派发 `may_dispatch` 列出的角色 |

---

## 4. 交付契约（硬规则）

任何一次 `dispatch` 结束时，`collect` 必须能拿到三样东西：

| 项 | 要求 |
|----|------|
| `audit` | `product/docs/audit/<YYYY-MM-DD>-<role>-<task>.md`（路径由 `manifest.json` 的 `audit_dir` 给出） |
| `verdict` | 该角色的裁决：`✅`/`❌`、风险分级，或「完成并移交」 |
| `artifacts` | 产物路径列表；只读角色可为空，但审计必须存在 |

**缺任一项视为该次派发未完成**，门禁不放行。

这是本契约唯一的强制点：它把「流程是否真的发生」从模型的记忆里挪到了文件系统上，
因此可以被 `scripts/check_gates.py` 和 CI 检查——与使用哪个模型无关。

---

## 5. 派发规则

1. **只有 `capability: dispatch` 的角色可以派发**（当前为 `orchestrator`）。
2. **不得派发自己**，避免递归。
3. **子角色不得再派发**：需要更多角色时，把需求交回 `orchestrator`。
4. **一次派发只做一件事**：任务粒度对齐审计文件的命名粒度。
5. **串行模式同样受约束**：没有子代理时，由主会话依次扮演，
   但每换一个角色前必须先读该角色的定义文件，并按 §4 落审计。

---

## 6. 串行模式的执行顺序

无子代理时，默认按门禁顺序推进，不可跳步：

```text
orchestrator(拆解)
  → product-manager(PRD)            ─┐
  → architect(契约) + ui-designer(界面稿) ─┤ G1
  → 实现角色（cpp / python / frontend）   ─┐
  → reviewer ✅                     ─┤ G2
  → test-engineer + security-reviewer ─┤ G3
  → doc-engineer + release-engineer  ─┘ G4
```

**缺哪个角色的审计，就说明哪一步没发生**；`check_gates.py` 会指出缺谁。

---

## 7. 一致性检查

```bash
python scripts/gen_agents.py --check   # 生成物与 manifest 是否漂移
python scripts/check_gates.py          # 审计文件命名/字段是否合规
python scripts/check_gates.py --diff origin/main   # 改了代码却没补审计 → 失败
```

两者都在 CI 中执行。任何一条失败，说明角色系统已退化为「文档」。
