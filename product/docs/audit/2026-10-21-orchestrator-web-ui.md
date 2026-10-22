# orchestrator — Web UI 能力交付记录

| 字段 | 内容 |
|------|------|
| 角色 / 时间 / 任务 | orchestrator / 2026-10-21 / 记录「本地 Web UI」这一新能力的范围决策、实现与门禁 |
| 结论 | 完成并移交（发布口径仍为 `0.1.0`，但「可用形态」增加一种） |

## 1. 输入

- 用户反馈：「给定 webui console 有点难用」。
- 选型（用户确认）：**本地 Web UI，与控制台共存**；Web 模式下默认禁用 run_shell。

## 2. 关键决策

1. **不引入 Web 框架**（ADR-0005）：项目依赖只有 libcurl；NSIS 那次的教训是
   「把可选依赖变成阻塞项」拖红整条 CI。框架的内存占用也不受本项目的 NFR-1 约束。
2. **这是新能力，不改「不变层」**：`.role-system/` 与根 `AGENTS.md` 零改动。
   能力边界（Web UI 怎么定位）记在 `product/docs/adr/ADR-0005-local-web-ui.md`。
3. **安全四道闸不可协商**：回环监听 / 随机令牌 / Host 校验 / run_shell 显式开启。
   这四条写进了契约 §15 与 ADR-0005，将来谁想「简化」都会撞上文档。
4. **控制台在 Web 模式下是旁观者**：显示同样内容，Ctrl-C 退出。
   两个输入源同时可用会让「谁在说话」含糊，也把 Windows 原始模式的单键读
   与 Web 的多客户端并发混在一起。

## 3. 产出

| 项 | 位置 |
|----|------|
| Web 服务（HTTP/SSE、令牌、通道） | `product/src/web.c`、`product/src/web_net.c`、`product/src/web_page.c` |
| 对外接口 | `product/include/cagent/web.h` |
| core 结构化事件 | `product/include/cagent/core.h`、`product/src/core.c` |
| 入口 | `product/src/main.c`（`--web` / `--web-port N`） |
| 冒烟 | `product/scripts/web_smoke.py`、`product/scripts/web_tool_smoke.py`（已进 CI） |
| 决策记录 | `product/docs/adr/ADR-0005-local-web-ui.md` |
| 契约 | `product/docs/design/contract.md` §15（Web UI 规定） |
| 审计 | cpp-implementer / reviewer G2 / security-reviewer G3 / test-engineer G3 |

## 4. 门禁

| 门禁 | 结论 | 依据 |
|------|------|------|
| G2 | ✅ 通过 | `2026-10-21-reviewer-gate-g2-web-ui.md`（R1–R12） |
| G3 | ✅ 通过 | `2026-10-21-security-reviewer-gate-g3-web-ui.md`（无 Critical/High）+ `2026-10-21-test-engineer-gate-g3-web-ui.md` |

## 5. 结论

Web UI 可用：浏览器里聊天、看工具卡片、点按钮批准；控制台仍显示同样内容。
安全面已审。发布版本号不变（`0.1.0`），但「可用形态」从「只有终端」变成「终端 + 本地 Web UI」。

## 6. 移交

- `release-engineer`：把「Web UI 可用」写进发布说明（V 项检查单未变——检查单针对的是
  「从零构建能跑起来」这件事，Web UI 是附加形态，不改变原有门禁）。
- 后续议题：① 令牌一次性化/过期（安全 Low 项）；② SSE 发送并行化（S9 可用性）；
  ③ Web 模式补 `/list` `/resume`；④ 页面从内嵌字符串拆成独立文件。

## 7. 复核（由独立复核者填写，非自证）

| 复核人 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| reviewer（交叉） | 2026-10-21 | 通过 | 抽查 ADR-0005 的取舍与契约 §15 的一致性；确认凭据未出现在任何提交文件里 |
