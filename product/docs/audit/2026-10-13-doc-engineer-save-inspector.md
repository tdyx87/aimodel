# 审计：doc-engineer · 存档查看器用户文档

## 1. 基础信息

| 字段 | 内容 |
|------|------|
| 角色 | `doc-engineer` |
| 时间 | 2026-10-13 19:20 |
| 任务 | 产出面向用户的 README、用户手册、FAQ、CHANGELOG |
| 门禁 | G4 |

## 2. 输入 / 依赖

| 输入 | 路径 / 来源 |
|------|-------------|
| 需求 | `product/docs/prd/PRD-save-inspector.md`（US-1~US-7、NFR-1~NFR-5） |
| 实际界面 | `product/web/index.html`、`product/web/app.js` |
| 入口行为 | `product/tools/save_inspector_ui.cpp`（`--port` / `--no-open` / `--web-root`） |
| 已知限制 | `product/CHANGELOG.md` 依据本轮实测行为撰写 |

## 3. 证据锚点

| 证据 | 位置 |
|------|------|
| README | `product/README.md` |
| 用户手册 | `product/docs/manual/getting-started.md` |
| FAQ | `product/docs/manual/faq.md` |
| CHANGELOG | `product/CHANGELOG.md` |
| 界面文案来源 | `tools/extract_ui_text.py` 提取 `web/app.js` 中全部用户可见中文 |
| 产物引用 | 本文件 |

## 4. 关键决策

| 决策 | 理由 | 被否决的备选 |
|------|------|--------------|
| 先用脚本提取 `app.js` 的真实文案，再写文档 | `roles.md` §3.10 硬约束「步骤与实际 UI 一致」。凭记忆写按钮名必然出错 | 直接照着 PRD 写（PRD 说的是需求，不是界面） |
| README 开头先讲「**不会**做什么」 | 工具的唯一独特价值就是只读+本地。把「绝不修改」「数据不出本机」前置，比藏在页脚更有说服力 | 只列功能（用户看不出与普通查看器的区别） |
| FAQ 覆盖「六类读取失败」且每条给出**可执行动作** | 与 PRD US-5 的诊断设计一致：用户遇到问题需要的是「我现在该干什么」 | 只列错误现象（等于把界面文案抄一遍） |
| CHANGELOG 明确写「已知限制」而非只写新增 | 0.1.0 的限制（加密不支持、压缩不解压、只支持一种格式）会直接决定用户能否用，不写等于隐瞒 | 只写新增功能 |
| 签名缺失在 FAQ 里主动解释 | 发布检查单确认本版本未签名，用户一定会遇到系统提示。文档不解释，用户会以为装错了 | 回避（用户困惑） |
| 构建信息隔离到 `docs/design/` | `roles.md` §3.10：内部开发者文档归 `architect`。README 只在末尾给一行指向 | 在 README 写构建章节 |

## 5. 风险与阻塞

- ~~**产物级端到端未执行**~~ → **已闭环**：`release-engineer` 在 G4 收尾时新增
  `tests/e2e/test_package_artifact.py`，针对真实 ZIP 产物跑完
  「界面资源 → 打开 → 列出 → 详情 → 导出」全链路（2/2 通过）。
  文档描述的「3 步出结果」现已在**产物**上得到验证。
- **文档中的按钮名可能随后续 UI 改动失同步**：`roles.md` §3.10 要求「`frontend-implementer` 变更须同步」，但**没有任何机制强制**。本轮已用 `tools/extract_ui_text.py` 建立可复用的文案提取手段（人工比对已完成），但**尚未接入 CI 自动比对**。登记为遗留。
- FAQ 中「常见存档路径」是行业惯例而非本工具可验证的事实（取决于具体游戏），已用「常见位置」措辞并提示优先用游戏内菜单查找。
- 未做本地化：产品当前只面向中文用户，`index.html` 的 `lang="zh-CN"` 与界面文案一致。

## 6. 产出

| 产物 | 路径 |
|------|------|
| 面向用户介绍 | `product/README.md` |
| 使用手册 | `product/docs/manual/getting-started.md` |
| 常见问题 | `product/docs/manual/faq.md` |
| 版本记录 | `product/CHANGELOG.md` |
| 文案提取辅助脚本 | `product/tools/extract_ui_text.py`（工具链辅助，不随产品分发） |

## 7. 结论

| 项 | 内容 |
|------|------|
| 角色级裁决 | ✅ 完成 |
| 依据 | `roles.md` §3.10 完成标准：「一个从没接触过产品的用户能按文档完成安装到出结果的全流程」。文档覆盖该全链路，且**末端「出结果」已在真实发布产物上实测验证**（`test_package_artifact.py` 2/2） |
| 自检 | ① 无开发者行话（README/手册/FAQ 通读无接口、编译、格式编号等术语）；② 按钮名全部取自 `app.js` 实际文案；③ 与 PRD US-1~US-7 逐条对应；④ 签名缺失、已知限制均主动披露 |

## 8. 复核记录

| 项 | 内容 |
|------|------|
| 复核人 | `orchestrator`（见 `2026-10-13-orchestrator-gate-g4.md`） |
| 复核方式 | 独立执行 `tools/extract_ui_text.py` 提取界面文案，与文档中引用的按钮名/提示语逐字比对；并实测从 ZIP 解压启动后首页含「只读取，不修改」 |
| 复核结论 | 通过（附 1 项待 G4 收尾或 CI 补验） |

## 9. 移交

| 项 | 内容 |
|------|------|
| 交给 | `orchestrator`（G4 裁决） |
| 待办 | UI 改动时须同步本文档；建议后续加 CI 步骤自动比对文案 |