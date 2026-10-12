# 角色审计记录

> 生成路径：`product/docs/audit/2026-10-12-test-engineer-chat-system.md`
> 对应规范：`.role-system/agents.md` §6.6

---

## 1. 基础信息

| 字段 | 内容 |
|------|------|
| 角色 | test-engineer |
| 时间 | 2026-10-12 23:25 |
| 任务 | 聊天系统单测 + 用户场景冒烟（含既有用例回归复核） |
| 门禁 | G3 质量 |

## 2. 输入 / 依赖

| 输入 | 路径 / 来源 |
|------|-------------|
| 上游产物 | `chat_suggest` 实现、`web/` 前端、HTTP 路由 |
| 依赖 | GTest v1.14.0；`product/scripts/build.ps1` |

## 3. 证据锚点

| 证据 | 位置 |
|------|------|
| 单测文件 | `product/tests/chat_suggest_test.cpp`（12 项） |
| 实测 | `ctest --test-dir build -C Release` → **38/38 通过**（system_info 10 + file_transfer 16 + chat_suggest 12） |
| 端到端 | 启动 `system_info_ui --no-open --port 8799`，curl 三接口：capability 200、suggest(7B) 返回 `not_recommended` 且含配置摘要/下一步/不确定性声明、空 query 返回 `INVALID_QUERY`、错误方法 405 |
| 静态资源 | `curl /`、`/app.js`、`/styles.css` 均含聊天模块 |
| 回归 | `/api/system-info` 仍返回 200 |

## 4. 关键决策

| 决策 | 理由 | 被否决的备选 |
|------|------|--------------|
| 断言**分类不变量**与契约必含字段，不断言具体数值 | 遵守 `agents.md` §8.4「断言结构与不变量」 | 断言精确 GB 数字（脆弱） |
| 用直接构造的 `SystemInfo` 驱动纯函数单测 | `BuildChatAdvice` 纯函数，无需真实硬件 | 依赖真实 `GetSystemInfo()`（不可控） |
| 端到端以 curl 冒烟覆盖三接口 | 免构建前端无 JS 单测层，接口级冒烟性价比高 | 引入浏览器自动化（本环境未就绪） |
| **修复**文件传输单测的非幂等隔离问题 | 其临时根为固定名，第二次运行读到残留文件导致偶发失败，破坏「G3 全绿」可复现性；改为「时间戳 + 自增序号」唯一目录 | 只登记不修（门禁结论不可复现） |

## 5. 风险与阻塞

- **平台覆盖**：仅在 Windows 实测；Linux/macOS 未验证（`chat_suggest` 平台无关，风险低，但未实测）。
- **E2E 层级**：为接口/静态资源级冒烟，非浏览器交互级自动化；四态中的「加载/错误态」由代码审查 + 手工触发保障。
- **已修复**：`FileTransferTest` 的临时目录此前为固定名，第二次运行会因 `build/ft_test_*` 残留而偶发失败（`EnsureDirectoryHandlesWindowsDrivePrefix`、`MissingDirectoryListsAsEmpty` 等）。本轮改为每次运行唯一的临时根，`ctest` 连续两次 38/38 全绿（幂等）。

## 6. 产出

| 产物 | 路径 |
|------|------|
| 单测 | `product/tests/chat_suggest_test.cpp` |
| 测试结论 | 本审计（`ctest` 38/38 + 端到端冒烟） |

## 7. 结论

| 项 | 内容 |
|------|------|
| 角色级裁决 | ✅ 通过（Windows 范围） |
| 风险 / 影响 | Low |
| 备注 | 三平台与浏览器级 E2E 为已知局限，登记于 §5 |

## 8. 复核记录

- 复核人 | orchestrator
- 复核时间 | 2026-10-12 23:25
- 复核结论 | 通过
- 复核摘要 | 复现 `ctest` 38/38；复现 curl 三接口与静态资源 grep；确认回归项 `/api/system-info` 200。

## 9. 移交

| 项 | 内容 |
|------|------|
| 交给 | `security-reviewer`（隐私红线）、`orchestrator`（G3 核验） |
| 下游依据 | 本审计 + 单测 + 冒烟证据 |
| 开工前提 | 按隐私红线与安全清单复核 |
