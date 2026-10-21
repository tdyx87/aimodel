# 审计：发布检查单（release-engineer，G4）

| 字段 | 内容 |
|------|------|
| 角色 / 时间 / 任务 | `release-engineer` / 2026-10-21 / cagent 0.1.0 发布检查（G4） |
| 结论 | **有条件通过**：可交付 0.1.0，条件见 §4（均为「已记录未验证」，不是已知缺陷） |

## 1. 输入

- `product/README.md`、`product/design/ui-terminal.md`
- 上游审计：G2（reviewer ✅）、G3（test-engineer 完成、security-reviewer 无 Critical/High）、
  doc-engineer（完成）、ui-designer（完成）
- 代码：`feat/c-agent` 分支，`8c1e7b0` 之后

## 2. 检查单

| # | 项 | 结果 | 证据 |
|---|----|------|------|
| V1 | 版本号一致（CMake / `--version` / README） | ✅ | `project(... VERSION 0.1.0)`；`cagent 0.1.0 (C99)`；README 首段 |
| V2 | 按 README 的入口**从零构建** | ✅ | `build.ps1 -Config Release -BuildDir build-doc`：138 s，`CAGENT_TRANSPORT_BACKEND=libcurl`，**8/8** 测试通过，退出码 0 |
| V3 | `--version` / `--help` 不依赖环境变量 | ✅ | 空环境下 `--version` → `cagent 0.1.0 (C99)` + `传输后端：libcurl`；`--help` 输出为合法 UTF-8，退出码 0 |
| V4 | 单测全绿 | ✅ | `ctest` 8/8（core 78 / tools 115 / ui 50 / session 1714 检查） |
| V5 | 冒烟全绿 | ✅ | `curl_smoke`、`session_smoke`、`tool_smoke` 均 PASS |
| V6 | CI 三平台 | ✅ | `069c035` ubuntu/macos/windows 全部 success |
| V7 | 无零告警回退 | ✅ | CI 的「编译告警为硬要求」步骤通过 |
| V8 | 仓库无密钥/凭据 | ✅ | 全仓 `sk-` 模式扫描 0 命中；会话文件用例保证不含 key |
| V9 | 用户文档完整（构建/配置/批准/会话/限制） | ✅ | `README.md` |
| V10 | 界面稿存在且与实现一致 | ✅ | `design/ui-terminal.md` §11 验收映射 |
| V11 | 审计齐备（每个角色一份，含复核行） | ✅ | `docs/audit/2026-10-21-*` 共 8 份 + G4 本文件 |
| V12 | 文档入口被**验证**（不是只写了个脚本） | ✅ | `build.ps1` 从零构建 138 s；`build.sh` 由 CI 步骤「文档入口冒烟」在 ubuntu 实跑（配置/构建/`ctest` 全绿），run `3873bf5` |
| V13 | 脚本在全新 clone 上可直接执行 | ✅（修复后） | 首跑即失败：`./scripts/build.sh: Permission denied`——Windows 提交导致模式 100644。`update-index --chmod=+x` 后为 100755，复跑绿 |

## 3. 关键决策

1. V2 采用「新目录从零构建」而不是增量构建，理由：增量构建会掩盖脚本缺参数、缓存残留等问题
   （本次正是靠它确认 `-DCMAKE_PREFIX_PATH` 真的到达 CMake）。
2. V12 先判 ⚠️：`build.sh` 从未被执行过，等于「文档里的 Linux/macOS 入口未验证」。加 CI 步骤后**首跑就红**，且是**真缺陷**（缺可执行位，见 V13）——这说明「写了入口但没跑过」本身就是缺陷来源，不是形式主义。
3. 不以「CI 绿」代替「手工可用」：CI 的 cagent 跑在非 TTY 下，交互界面只有用例级保证。

## 4. 放行条件（必须满足才可宣称 0.1.0 稳定）

| # | 条件 | 责任角色 | 判据 |
|---|------|---------|------|
| C1 | 在 Linux 或 macOS 上实跑一次 `scripts/build.sh Release` | release-engineer | **Linux 部分已达成**（run `3873bf5`，ubuntu，配置/构建/测试全绿）；macOS 仍待验证 |
| C2 | 用真实第三方 HTTPS 服务跑通一轮（含一次工具批准） | test-engineer | 有日志，key 不落盘 |
| C3 | 活跃会话与超长会话的 RSS 实测（500 轮） | test-engineer | ≤100 MB，附采样 |

C1 的 Linux 部分已满足（并在过程中修掉可执行位缺陷），macOS 部分与 C2、C3 **未满足**，因此本版本标记为 **0.1.0（可用，未达稳定门槛）**；C2 依赖外部凭据，属已知阻塞。

## 5. 移交

交给 `orchestrator` 记录 G4 结论并决定发布口径（`0.1.0` 标注「未达稳定门槛」）。

## 6. 复核（由独立复核者填写，非自证）

| 复核人 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| orchestrator（单人模式） | 2026-10-21 | 通过 | 复跑 V2（138 s / 8/8）与 V3（`--version` 输出一致）；V12 → ✅ 以 CI run `3873bf5` 为准，V13 的修复同为 `3873bf5` |
