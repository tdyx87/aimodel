# AGENTS.md

> 本文件是**仓库入口**：先读这里，再按需进入两层。
> 本仓库的主要目的是**角色系统本身**——`product/` 只是它的第一个用例。

---

## 这个仓库是什么

这是一个**角色驱动开发系统**：用一组固定角色（12 个）+ 流水线 + 门禁，把"一句话需求"推进成可交付产物。
`product/` 里的硬件报告只是**第一个用例**，用来验证角色系统能跑通。

**换新需求时，只替换 `product/` 的内容，`.role-system/` 不动。**

---

## 三层结构（变与不变的分离）

```
aimodel/
├─ AGENTS.md              ← 本文件：入口与导航
├─ .role-system/          ★ 不变层：换需求不动
│   ├─ agents.md            # 方法论总纲：两层架构、目录约定、工程规范、编码风格
│   ├─ roles.md             # 第一层：12 个角色的职责/边界/输入输出/协作
│   └─ agents/*.md          # 第二层：12 个角色的可执行定义（frontmatter 约束 tools/model）
└─ product/               ★ 变层：换需求就替换这一层
    ├─ README.md            # 面向用户的介绍（doc-engineer 维护）
    ├─ CMakeLists.txt       # 构建定义
    ├─ src/  include/  web/  design/  tests/  tools/  scripts/
    └─ docs/                # 需求与设计（prd/、design/）
```

### 判断某处改动该进哪层

| 问自己 | 属于 |
|--------|------|
| 这条规则换个产品还成立吗？（如 C++11 合规、只读审查、门禁顺序） | `.role-system/`（不变层） |
| 这条是具体某个功能的需求/代码/界面？（如"显示显存"） | `product/`（变层） |

> 不变层的铁律：**不出现具体领域的名词**。若 `agents.md` / `roles.md` / `agents/*.md` 里写死了某个产品的"推理/模型/硬件"字样，那是变层内容泄漏进了不变层，需抽成通用表述。

---

## 怎么用（两种模式）

### 单人模式（当前）
一个会话先后扮演多个角色，但**结论格式不变**。典型顺序：
`product-manager`（写 PRD）→ `architect`（出契约）+ `ui-designer`（出界面稿）→ 各工种实现 → `reviewer` → `test-engineer` → …→ `release-engineer`。

### 多 agent 模式
`orchestrator` 拆任务、按角色派发（`team_run_task` / `team_task`），四道门禁按序放行。

---

## 四道门禁（顺序不可跳）

| 门禁 | 位置 | 放行条件 |
|------|------|---------|
| **G1 设计** | 设计→实现 | PRD 存在且含 Given/When/Then + architect 契约 + ui-designer 界面稿 |
| **G2 合入** | 实现→测试 | `reviewer` ✅（按对应工种清单） |
| **G3 质量** | 测试→交付 | `test-engineer` 单测 + E2E 全绿 + `security-reviewer` 无 Critical/High |
| **G4 发布** | 文档→发布 | `doc-engineer` 用户文档完成 + `release-engineer` 签名/安装包/更新检查单全绿 |

完整角色定义、流水线、工程规范 → 见 `.role-system/agents.md` 与 `.role-system/roles.md`。

---

## 构建与测试（当前用例）

```powershell
cd product
./scripts/build.ps1 -Config Release        # Windows：配置 + 构建 + ctest
```

```bash
cd product
./scripts/build.sh Release                 # Linux / macOS
```

---

## 加一个新需求的步骤

1. 在 `product/docs/prd/` 新增 `PRD-<feature>.md`（让 `product-manager` 来做，含 Given/When/Then）。
2. 让 `architect` 出 `product/docs/design/<feature>.md` 契约、`ui-designer` 出 `product/design/` 界面稿 → 过 G1。
3. 各工种在 `product/` 内并行实现 → `reviewer`（G2）→ `test-engineer` + `security-reviewer`（G3）→ 交付（G4）。
4. 若新需求需要**新工种/新角色/新门禁**，那才动 `.role-system/`，并遵守其活文档规则。

> 要完全替换用例（比如做一个和硬件无关的新产品）：清空 `product/` 内的源码与文档，保留目录骨架，从第 1 步重来。

---

## 扩展角色系统（什么情况下才动 `.role-system/`）

绝大多数需求**只替换 `product/`**，不变层不动。只有角色系统的能力边界不够时，才扩展 `.role-system/`，且**每次扩展都记 `product/docs/adr/`**（背景/决策/备选/后果），一次扩展一个议题。

### 什么时候该加新角色

先过这条判断（与 `roles.md` 第 2 节一致）：只有当**工具链、评审标准、产出物目录三者都不同**时，才独立设角色。三者不全不同 → 归入既有角色，不新增。

- **新工种**（如未来加 Rust 后端）：工具链不同 + 评审标准不同 + 目录不同 → 新增一个实现角色。
- **通用角色**（非工种，如新增"性能工程师"）：职责不与既有角色重叠、有明确移交规则 → 新增。

### 加一个新角色的步骤

1. **`roles.md`**：在总览表与 §3 各角色详述中登记——阶段、一句话职责、权限（写哪个目录）、输入/输出、移交规则、完成标准。
2. **`agents/<role>.md`**：写第二层可执行定义。frontmatter 的 `tools` 落地**最小权限**（只读角色不给 Write/Edit），`name` = 角色名。
3. **接线三处**，否则新角色是孤岛：
   - `architect` 的接口契约增加该工种分支；
   - `reviewer` 增加该工种的**审查清单**（审查角色**始终是一个 `reviewer` + 分工种清单，不按工种拆成多个审查角色**）；
   - `test-engineer` 的测试工具矩阵增加对应栈。
4. **门禁**：确认新角色的产物经过哪道门禁，必要时调整放行条件（见下）。
5. **活文档**：同步 `agents.md` 的角色速览与流水线图。

### 加一道新门禁

门禁必须**有序、有明确放行条件、不可跳过**：

1. 在 `roles.md` 与 `agents.md` 的门禁表中登记 G5（或插入既有序列的对应位置）。
2. 更新 `orchestrator` 的「门禁按序」职责，保证编排时不会跳过。
3. 明确放行判据由哪个角色、按什么结论放行（如 `✅ 通过` / 无 Critical）。

### 扩展后的铁律

- 不变层仍**不出现领域名词**——新角色描述的是"职责与方法"，不是"做推理还是做报表"。
- 提交符合 Conventional Commits；裁剪/扩展决策记入 `product/docs/adr/`，保持可追溯。

