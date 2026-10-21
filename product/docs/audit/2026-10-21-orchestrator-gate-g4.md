# 审计：G4 门禁记录（orchestrator）

| 字段 | 内容 |
|------|------|
| 角色 / 时间 / 任务 | `orchestrator` / 2026-10-21 / 核对 G4 放行材料并记录本版口径 |
| 结论 | G4 **有条件放行**：文档与检查单齐备；发布口径为 `0.1.0（可用，未达稳定门槛）` |

## 1. 输入（放行材料逐项核对）

| 门禁 | 材料 | 核对结果 |
|------|------|---------|
| G1 设计 | PRD ✅ / 契约 ✅ / **界面稿 `design/ui-terminal.md`（本次补齐）** | 上次「有条件通过」的条件已履行 |
| G2 合入 | `2026-10-21-reviewer-gate-g2-tool-approval.md`（✅，含首轮不通过与修复） | 有结论、有复核行 |
| G3 质量 | `2026-10-21-test-engineer-gate-g3-tool-approval.md`（完成）/ `2026-10-21-security-reviewer-gate-g3-tool-approval.md`（无 Critical/High） | 两份均在，未覆盖项已列 |
| G4 发布 | `2026-10-21-doc-engineer-readme.md`（完成）/ `2026-10-21-release-engineer-gate-g4.md`（有条件通过） | 检查单 13 项、全部满足（V12 由 CI 实跑转绿，V13 记录并修复缺可执行位），1 项 ⚠️（`build.sh` 未实跑） |

## 2. 抽检（非仅凭声明）

1. 本地复跑 `ctest --test-dir build-curl -C Release` → 8/8。
2. 本地复跑 `scripts/tool_smoke.py build-curl/Release/cagent.exe` → PASS（批准写盘 / 拒绝不写 / 预览可见）。
3. 新目录从零构建 `build.ps1 -BuildDir build-doc` → 138 s、libcurl、8/8。
4. CI：`3873bf5` ubuntu / macos / windows **全部 success**，其中 ubuntu 的「文档入口冒烟」步骤首次实跑 `scripts/build.sh`。
   该步骤**首次运行即发现真缺陷**（脚本缺可执行位，全新 clone 上 `Permission denied`），已修并复跑通过。
5. 抽查 reviewer 的关键判断：`write_file` 的 `pending_display` 确在 `check_perm` 之前赋值；
   首轮「未展示内容」的缺陷已由用例 `write_pending_display_shows_path_and_content` 钉住。

## 3. 关键决策

1. **不放行「稳定版」口径**。理由：macOS 上 `build.sh` 未经脚本路径验证、真实第三方服务未验、活跃会话 RSS 未测
   （release 审计 C1–C3，其中 C1 的 Linux 部分已达成）。把 0.1.0 说成稳定，等于把未验证风险转给用户。
2. 承认单人模式的局限：同一操作者切换角色，复核行本质是自证。已在各审计 §6 显式标注，
   这也是 `agents.md` §6.6 记录的已知问题，不因本次交付而消失。

## 4. 结论

G4 有条件放行。**当前状态：代理可用（读写/命令经批准真的执行）、界面可用（含批准与等待提示）、
三平台 CI 全绿**；未达稳定门槛的三条条件见 release 审计 §4。

## 5. 移交

给用户/下一轮迭代：
- 已完成：C1 的 Linux 部分（`build.sh` 进 CI 并实跑，顺带修掉缺可执行位）；macOS 仍靠 CI 的 cmake 直连路径验证
- 需外部资源：C2（真实第三方 HTTPS + 凭据）
- 需专门压测：C3（活跃/超长会话 RSS）
- 待排期：US-8（非兼容服务适配）、`search` 正则（需 ADR）、ASAN/UBSAN

## 6. 复核（由独立复核者填写，非自证）

| 复核人 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| （无） | — | — | 单人模式下无独立复核者；本行留空并如实说明，不构成客观正确性证明 |
