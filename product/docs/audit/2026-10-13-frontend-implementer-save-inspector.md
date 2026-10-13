# 审计：frontend-implementer · 存档查看器界面

## 1. 基础信息

| 字段 | 内容 |
|------|------|
| 角色 | `frontend-implementer` |
| 时间 | 2026-10-13 15:10 |
| 任务 | 实现三屏界面（首屏空态 / 槽位列表 / 槽位详情 / 诊断视图） |
| 门禁 | G2 |

## 2. 输入 / 依赖

| 输入 | 路径 / 来源 |
|------|-------------|
| 界面稿 | `product/design/design-system.md`、`design/screens/{slot-list,slot-detail,diagnosis}.md` |
| 协议 | `product/docs/design/save-format-contract.md` §4（端点与状态码约定） |
| 上游审计 | `2026-10-13-ui-designer-save-inspector.md`（复核结论：通过） |

## 3. 证据锚点

| 证据 | 位置 |
|------|------|
| 槽位卡片 7 态 | `web/app.js` `slotCard()` / `slotTone()`（左竖条 + 角标文字双重编码） |
| 诊断三段式 | `web/app.js` `diagnosticNode()`（title / detail / advice 三段齐备，advice 带 `→` 前缀） |
| fetch+blob 导出 | `web/app.js` `exportSlot()`（按 Content-Type 区分成功与失败） |
| 产物引用 | 本文件 |

## 4. 关键决策

| 决策 | 理由 | 被否决的备选 |
|------|------|--------------|
| 导出按 `Content-Type` 区分成功/失败，而非只看 `response.ok` | 成功是 `application/json` 或 `text/plain` 的二进制流，失败也是 `application/json`（`{ok:false,message}`）。只看状态码无法区分「导出内容是 JSON 文件」与「这是错误 JSON」 | 只看 `response.ok`（错误 JSON 会被当正文存成文件） |
| 以 **fetch + blob** 替代 `window.location.href` | aimodel 实测：直接跳转时，服务端返回错误 JSON 会被浏览器当正文渲染，把用户**踢出整个界面**。这是契约 §4 点名要求 | `window.location.href`（已被实测证伪） |
| 存档以**二进制 body 上传**，不传路径 | 契约 §5 隐私约束：服务端**见不到用户本机路径**。浏览器读出字节后上传，服务端只处理字节 | 传路径让服务端读（同源，但服务端将持有路径，且闸门/错误信息更难控制） |
| 状态机 `empty/parsing/list/detail/diagnosis` 五态显式建模 | 界面稿 §2 的状态矩阵有 6 态（含空存档），空存档归入 `list` 且渲染专用空态 | 用若干布尔标志拼（组合爆炸，易出现非法组合） |
| 「换一个存档」返回 `empty` 并清空全部状态 | 避免上一个存档的诊断/槽位残留到新文件上 | 保留状态只换文件（用户会看到不相关的旧诊断） |
| `slotTone()` 用「关卡=0 且金币=0 且时长 0 分钟」判定空槽位 | 服务端未提供 `is_empty` 字段。当前启发式对「新档/空档」成立 | 前端猜（更糟）；等后端加字段（需改契约，超出本期） |

## 5. 风险与阻塞

- **空槽位判定是启发式，有误判可能**：若某存档存在「第 0 章 + 0 金币但已游玩 30 分钟」的槽位，会被误判为空槽位而隐藏进度摘要。**已在 §4 决策中如实登记**，建议契约下一版补 `is_empty` 字段。当前不阻塞：样本与常规存档均不触发。
- **无 JS 单测层**：`roles.md` §3.5 明确免构建形态下前端质量由 `test-engineer` 的用户场景 E2E 把关。已按该约定执行，未自行引入打包链。
- **`prefers-reduced-motion` 已处理**：骨架屏动画在用户开启减弱动效时停止（可访问性）。
- 前端不维护道具 ID→名称映射表（契约 §3.2），完全依赖核心库返回的 `name`。

## 6. 产出

| 产物 | 路径 |
|------|------|
| 页面骨架 | `web/index.html` |
| 逻辑与渲染 | `web/app.js` |
| 样式 | `web/styles.css` |

## 7. 结论

| 项 | 内容 |
|------|------|
| 角色级裁决 | ✅ 完成并移交 |
| 风险 / 影响 | Low（空槽位启发式，见 §5） |
| 备注 | 自检界面稿三条硬要求逐条核验：① 槽位卡片 7 态全部实现，`corrupt` **可点击**（`slotCard()` 对所有态绑定同一 click handler，未按态禁用）；② 诊断三段式 `title/detail/advice` 齐备且 advice 非空时有 `→` 前缀，代码中不存在跳过 advice 的分支；③ 导出走 fetch+blob，无 `window.location.href`。可访问性：拖放区同时提供点击与键盘（Enter/Space）、焦点态 `:focus-visible` 可见、严重级用图标+文字+颜色三重表达、`role="status"`/`aria-live` 播报。`node --check` 通过。 |

## 8. 复核记录

| 项 | 内容 |
|------|------|
| 复核人 | `reviewer`（已出具，见 `2026-10-13-reviewer-save-inspector-frontend.md`） |
| 复核结论 | ✅ 通过（1 项 🟡 建议） |
| 复核摘要 | 按 `roles.md` §3.7 的前端清单核验：XSS 防护（全站 `textContent`/`createTextNode`，无 `innerHTML`）、组件复用、错误状态覆盖。复核**独立 `grep` 确认无 `innerHTML`** 且无 `window.location.href`。 |

## 9. 移交

| 项 | 内容 |
|------|------|
| 交给 | `reviewer`（G2）、`test-engineer`（G3，E2E）、`doc-engineer`（G4，应用内帮助） |
| 下游依据 | 界面稿三份 + 契约 §4 |
| 开工前提 | G3 的用户场景 E2E 须覆盖：首屏空态、打开正常存档、部分损坏存档、完全不可解析、v1 旧档升级五条主路径 |
