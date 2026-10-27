# AGENTS.md

> 本文件是**角色驱动开发系统**的总纲，面向 AI 编码代理与开发者。
> 本仓库的目的是**角色系统本身**；`product/` 是它的一个用例，换需求时替换。
> 角色体系设计见 `roles.md`（第一层）；角色的可执行定义见 `agents/`（第二层）。

---

## 1. 系统定位

- **本质**：一套用固定角色（12 个）+ 流水线 + 门禁，把「一句话需求」推进成可交付产物的开发系统。
- **用例与系统分离**：具体做的是什么产品、什么领域，全部落在 `product/`；本文件与 `roles.md`、`agents/` 只描述**与领域无关**的方法论。
- **当前用例**：见 `product/README.md`（换需求时整体替换 `product/`，本层不动）。

> 不变层的铁律：**不出现具体领域的名词**。若这里出现某个产品的专有词汇，那是用例内容泄漏进了系统层，应抽成通用表述或下沉到 `product/`。

## 2. 方法论：先角色，后 Agent

采用**两层架构**管理 AI 代理：

| 层 | 回答的问题 | 位置 |
|----|-----------|------|
| 第一层：角色设计 | 需要哪些**职责分工**？边界、输入输出、协作关系？ | `roles.md` |
| 第二层：角色正文 | 每个角色具体做什么、边界在哪、怎么交付？ | `agents/*.md` |
| 能力声明 | 每个角色能读/能写/能派发到什么程度？ | `manifest.json` |
| 调用契约 | 派发、等待、交付的中性语义是什么？ | `invocation.md` |

**为什么先角色**：
1. 职责先于实现——避免造出职责重叠或真空的 agent；
2. 角色稳定、可裁剪——小项目裁剪 agent 启用数量，不动角色设计；
3. 按工种拆分——工具链/评审标准不同的工种必须分开（见 `roles.md` 第 2 节）。

**映射规则（重要）**：角色正文是**纯文档**，任何客户端都不会自动发现它。
真正的可执行注册文件由 `scripts/gen_agents.py` 从 `manifest.json` 生成：

| 生成物 | 客户端 | 关键字段 |
|--------|--------|---------|
| `.opencode/agents/<id>.md` | OpenCode V2 | `description` / `mode` / `permissions` |
| `.claude/agents/<id>.md` | Claude Code | `name` / `description` / `tools` / `model` |

因此：**不要在 `agents/*.md` 里写 frontmatter**，也不要在生成物里手改。
能力约束写在 `manifest.json`（`read-only` / `read-write` / `dispatch`），
由生成器翻译成各客户端语法。换客户端只改生成器，事实源不动。

## 3. 目录结构

```
<repo>/
├─ AGENTS.md              ← 入口：导航与换需求步骤
├─ .role-system/          ★ 不变层：换需求不动
│   ├─ agents.md            # 方法论总纲（本文件）：两层架构、工程规范、编码风格
│   ├─ roles.md             # 第一层：12 个角色的职责/边界/输入输出/协作
│   ├─ agents/*.md          # 第二层：角色职责正文（纯文档，无客户端语法）
│   ├─ manifest.json        # 角色清单 + 能力声明（含审计目录）
│   └─ invocation.md        # 调用契约：派发/等待/交付 + 环境映射
├─ scripts/
│   ├─ gen_agents.py        # 生成客户端注册文件；--check 校验漂移
│   └─ check_gates.py       # 审计门禁：代码有改动就必须有审计
├─ .opencode/agents/*.md   ☆ 生成物（勿手改）
├─ .claude/agents/*.md     ☆ 生成物（勿手改）
└─ product/               ★ 变层：换需求就替换这一层
    ├─ README.md            # 面向用户的介绍（doc-engineer 维护）
    ├─ CMakeLists.txt       # 构建定义
    ├─ src/  include/  web/  design/  tests/  tools/  scripts/
    └─ docs/                # 需求与设计（prd/、design/）
```

**各工种产物目录**（均在 `product/` 内，不跨目录堆放）：
`src/`+`include/`（核心实现）、`web/`（前端）、`design/`（设计资产）、`tests/`（测试）、`docs/`（需求与设计文档）、`scripts/`+构建（release-engineer）。

## 4. 角色体系速览

12 个角色，按「工种 × 阶段」组织，详见 `roles.md`：

| 阶段 | 角色 |
|------|------|
| 规划 | `product-manager`、`architect` |
| 执行 | `cpp-implementer`、`python-implementer`、`frontend-implementer`、`ui-designer` |
| 质量 | `reviewer`、`test-engineer`、`security-reviewer` |
| 交付 | `doc-engineer`、`release-engineer` |
| 协调 | `orchestrator` |

## 5. 协作流水线与门禁

```text
PRD(用户故事) → 架构契约(architect) + 界面稿(ui-designer)
  → 并行实现（cpp ∥ python内部工具 ∥ frontend）
  → 审查(reviewer) → 测试(单测+用户场景E2E) → 安全+隐私(security-reviewer)
  → 用户文档(doc-engineer) → 安装包+签名+更新(release-engineer)
全程 orchestrator 协调。
```

| 门禁 | 位置 | 放行条件 |
|------|------|---------|
| G1 设计 | 设计→实现 | `product-manager` 的 PRD 已存在且含 Given/When/Then 验收标准 + architect 模块契约 + ui-designer 界面稿（含空/错误状态）；三者各有审计文档 |
| G2 合入 | 实现→测试 | `reviewer` ✅（按对应工种清单）+ 对应实现角色有审计文档 |
| G3 质量 | 测试→交付 | `test-engineer` 单测+E2E 全绿 + `security-reviewer` 无 Critical/High；两者各有审计文档 |
| G4 发布 | 文档→发布 | `doc-engineer` 用户文档完成 + `release-engineer` 签名/安装包/更新检查单全绿；两者各有审计文档 |

> 每道门禁要求**对应角色已在 `product/docs/audit/` 留痕**（§6.6）——审计文档是该环节确已发生的凭证，`orchestrator` 放行前核验。

## 6. 全局约束（所有角色遵守）

1. **提交规范**：Conventional Commits（`feat:`/`fix:`/`docs:`/`test:`…），一次提交只做一件事。
2. **目录约定**：各工种产物只进对应目录（第 3 节），不跨目录堆放。
3. **跨平台**：C++ 平台差异用编译期宏隔离（`*_win32.cpp`/`*_posix.cpp`/`*_apple.cpp`），对外一律 UTF-8。
4. **安全红线**：密钥/令牌不落盘不打日志；第三方依赖固定版本并记录许可。
5. **活文档**：发现更优的角色划分或规范，同步更新 `roles.md` 与对应 agent 文件。
6. **审计留痕（强制）**：每个角色完成一次任务后，**必须**写一份审计文档到 `product/docs/audit/`（见下）。审计是门禁放行的依据——没有对应角色的审计文档，其产物不得进入下一阶段。

### 6.6 审计文档规范（所有角色统一）

- **命名**：`product/docs/audit/<YYYY-MM-DD>-<role>-<task>.md`（一角色一任务一份）。
- **何时写**：任务完成、移交下一角色之前。
- **最小字段**（每份必含）：

  | 字段 | 内容 |
  |------|------|
  | 角色 / 时间 / 任务 | 谁、何时、做了什么任务 |
  | 输入 | 消费的上游产物（文件/PRD/契约），含引用 |
  | 关键决策 | 做了什么选择 + **理由**（含被否决的备选） |
  | 产出 | 生成的产物及路径（文件、报告、结论） |
  | 结论 | 角色级裁决（如 `reviewer` 的 ✅/❌、`security-reviewer` 的风险分级）；无裁决的角色写"完成并移交" |
  | 移交 | 交给下游哪个角色、下游该依据什么开工 |

- **只读角色同样留痕**：`reviewer`/`security-reviewer` 不写代码，但其**审查结论就是审计**——按上述格式记录（`结论` 字段承载 ✅/❌ 或风险分级）。没有审计 = 该审查未发生，门禁不放行。
- **非自证，可被抽检**：审计记录的是**操作者声明的理由**，不等于客观正确——若操作者判断有误，审计会忠实记下错误理由。因此审计不单独作为放行依据，须经**独立复核**：
  - `orchestrator` 对每个门禁**至少抽检 1 份**上游审计，核对「产出」与「结论」是否与产物一致、「关键决策」的理由是否成立；
  - 下游角色开工前可质疑上游审计的「结论」，异议经 `orchestrator` 裁决；门禁放行以**复核通过**为准，而非仅凭审计存在。
  - 复核结论回写到被检审计的「复核」字段（谁、何时、通过/打回及原因）。
- **模板**：`product/docs/audit/_template.md`（首次使用时复制改写）。
- **机器校验**：`python scripts/check_gates.py` 检查命名与角色合法性；
  `--diff <base>` 在代码有改动时要求同区间新增审计，并检查新增审计是否含
  「输入 / 产出 / 结论 / 移交」四要素。CI 会执行，**跳过流程会失败而不是静默通过**。
- 审计文档随 `product/` 走（用例产物），换需求时一并替换；但**规范本身**（本条）属不变层。

## 7. 工程规范（C++ 工种执行细则）

### 7.1 C++11 禁止清单（cpp-implementer/reviewer 必查）

| 禁止（C++14+） | 替代（C++11） |
|------|------|
| `std::optional` | 自定义包装 / `bool + T` 组合 |
| `std::string_view` | `const std::string&` |
| `std::filesystem` | 平台 API 封装（`*_win32/posix/apple`） |
| `std::make_unique`（C++14） | `std::unique_ptr<T>(new T(...))` |
| 泛型 lambda / 初始化捕获 | 具名函数对象或具体类型 lambda |
| `if constexpr`、结构化绑定、折叠表达式 | 模板特化 / `std::get` / 循环展开 |
| `[[nodiscard]]`/`[[maybe_unused]]` | `(void)x;` 规避警告 |

允许：`auto`、`nullptr`、`constexpr`、范围 for、`override/final`、智能指针、`static_assert`、`enum class`、`std::thread/mutex/atomic`。

### 7.2 编码风格表

| 对象 | 风格 | 示例 |
|------|------|------|
| 类型 / 结构体 / 枚举 | `UpperCamelCase` | `ServerConfig` |
| 函数 / 局部变量 | `lower_snake_case` | 公开 `DoThing`→实现体 `do_thing`；调用同名小驼峰 API 保持一致 |
| 类私有成员 | `trailing_underscore_` | `buffer_` |
| 公开结构体字段 | `lower_snake_case` | `total_bytes` |
| 常量 / 宏 | `UPPER_SNAKE_CASE` | `<LIB>_API` |
| 文件名 | `snake_case.cpp/.h` | `server_win32.cpp` |
| 命名空间 | 小写单层 | `<lib>::detail` |
| 注释 / 文档 | 头文件 Doxygen（`@param/@return`） | 公开 API 必填 |

缩进 4 空格（禁 Tab）、行宽 ≤ 120、LF 换行、文件末尾保留空行。

### 7.3 跨平台执行细则（对应第 6 节全局约束第 3 条）

1. 平台分支只用编译期宏（`_WIN32`/`__APPLE__ && __MACH__`/`__linux__`），禁止运行时字符串判断。
2. 平台实现隔离为 `*_win32.cpp`/`*_posix.cpp`/`*_apple.cpp`，由 CMake `target_sources` 按平台选择。
3. Windows 宽字符 API 输出一律经 `detail::WideToUtf8` 转 UTF-8；静态库不启用 dllimport/dllimport 语义（见 CMake `<LIB>_SHARED`）。
4. 宽字符格式化用 ISO C 的 `%ls`（MinGW/UCRT 下 `swprintf` 的 `%s` 期望窄串，曾导致宽字符路径/键名构造失败）。
5. 读取系统结构/注册表等外部值，先确认**单位**（字节 vs MB、计数 vs 索引），换算需启发式或官方文档支撑。

### 7.4 构建与测试（`product/` 的本地入口）

```powershell
# Windows（在 product/ 目录下）
./scripts/build.ps1 -Config Release        # 配置 + 构建 + ctest
```

```bash
# Linux / macOS
./scripts/build.sh Release                 # 同上
```

- G3 完成标准：构建**零警告**（`-Wall -Wextra -Wpedantic` / `/W4`）+ `ctest` 全绿。
- 单测位于 `tests/`，命名 `<被测文件>_test.cpp`；断言结构与不变量，不断言用例特有的具体数值。

6. **构建产物**：`build/`、`node_modules/`、`__pycache__/`、`*.egg-info/` 等不入库（`.gitignore` 已覆盖）。
