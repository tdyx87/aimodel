# orchestrator — 长会话缺陷修复的 G2/G3 复核记录

| 字段 | 内容 |
|------|------|
| 角色 / 时间 / 任务 | orchestrator / 2026-10-21 / 记录 G4 之后发现的两个「可用性缺陷」的修复过程与门禁重跑 |
| 结论 | 完成并移交（G2 ✅ / G3 ✅；发布口径保持 `0.1.0`，不升级） |

## 1. 输入

- `docs/audit/2026-10-21-cpp-implementer-long-session-fixes.md`
- `docs/audit/2026-10-21-reviewer-gate-g2-long-session.md`（R1–R12）
- `docs/audit/2026-10-21-test-engineer-gate-g3-long-session.md`
- `docs/audit/2026-10-21-release-engineer-gate-g4.md` §7（追加记录）
- 缺陷证据：调试运行中 `ca_session_save` 从第二轮起返回 -1；`core.c` 旧上限 `16384` 与契约 §2.1 的 4 MB 冲突

## 2. 关键决策

1. **不在「G4 已放行」的结论上打补丁了事**：这两个缺陷都属「文档承诺可用、实际不可用」，
   按 G2/G3 重跑一遍，而不是只写一行 changelog。理由：G4 的检查单里有「长会话」相关条目，
   它当时是**按单测绿的**，单测没覆盖到的正是这次的真实用法（连续多轮 / 重启恢复）。
2. **版本号不动**：C1 的 macOS 部分与 C2（真实第三方 HTTPS）仍未达成，稳定性未变；
   本次修复提高的是「可用」，不是「稳定门槛」。
3. **能力边界记在当前层**：两个缺陷都是 `product/` 的实现问题，不动 `.role-system/`；
   「审计必须能被下游复跑证伪」这条经验来自既有 `agents.md` §6.6，不需要新增规则。

## 3. 产出

| 门禁 | 结论 | 依据 |
|------|------|------|
| G2 合入 | ✅ 通过 | reviewer R1–R12；R3（单轮模式误提示 `/new`）当场修复并复验；R5（重复 init 泄漏）转为 `core.h` 前置条件 |
| G3 质量 | ✅ 通过 | `ctest` 8/8、`session_test` 1731 检查 0 失败、`mem_load` 500 轮 7.71 MB PASS；4 项未覆盖已登记 |
| G4 复核 | 记录追加 | release 审计 §7：C3 达成；C1 的 macOS 部分改为 CI 实跑（`runner.os != 'Windows'`）；C2 仍阻塞 |

**本轮新增的 CI 覆盖**

- 「文档入口冒烟（`scripts/build.sh`）」从仅 Linux 扩到 Linux + macOS——C1 的 macOS 部分从此有实证。
- 新增「长会话内存实测（500 轮）」，把 `mem_load.py` 纳入门禁：淘汰逻辑若写错（内存不降反升），
  只有长会话能暴露，短冒烟与单测都看不见。

**这两个新步骤一接上就红了两次**（run `1dcf301` → 修 → `0dc6e35` 全绿），暴露的都是真缺陷：

1. macOS runner 的 C locale 下，`build.sh` 里 `$BUILD_DIR，` 被当成一个变量名（全角标点的
   字节算进了名字）→ `unbound variable`；Linux 的 UTF-8 locale 不会。已改 `${BUILD_DIR}`。
2. `mem_load.py` 给 `Popen` 传相对路径，而 `cwd` 是临时目录 → `FileNotFoundError`；
   本地一直用绝对路径，所以没暴露。已先 `resolve()`。

结论：**新增门禁的第一跑必须当真**。两次「刚接上就红」都直接对应线上会用到的路径，
不是 CI 配置噪音。C1 已全部达成（release 审计 §8）。


## 4. 结论

两个缺陷的根因都已消除，且各有能证伪的回归测试（临时回退 `session_store.c` 即转红）。
G2/G3 已重跑通过；G4 的 C1、C3 已达成（run `0dc6e35` 三平台全绿），
发布口径维持 `0.1.0`（可用，未达稳定门槛），唯一未达成项为 C2。

## 5. 移交

- `doc-engineer`：README 已补 `CAGENT_CONTEXT_BYTES` 与超限行为说明（本轮已完成，无需再动）。
- `release-engineer`：等 macOS 冒烟与 C2 有结论后再评估是否上调口径。
- 后续若收到「淘汰提示每轮刷屏」的反馈，再打磨 `context_notified` 的粒度（reviewer §3 已记录）。

## 6. 复核（由独立复核者填写，非自证）

| 复核人 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| reviewer（交叉） | 2026-10-21 | 通过 | 核对 §3 的门禁结论与三份审计一致；确认 `.role-system/` 与根 `AGENTS.md` 本轮零改动（`git status` 无相关路径） |
