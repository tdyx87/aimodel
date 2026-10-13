# 审计：python-implementer · 存档样本生成器

## 1. 基础信息

| 字段 | 内容 |
|------|------|
| 角色 | `python-implementer` |
| 时间 | 2026-10-13 14:50 |
| 任务 | 实现存档格式的 Python 参考实现与样本生成器（内部工具链） |
| 门禁 | G2 |

## 2. 输入 / 依赖

| 输入 | 路径 / 来源 |
|------|-------------|
| 契约 | `product/docs/design/save-format-contract.md` §2（格式定义、§2.1.1 CRC 参数） |
| 上游依赖 | `roles.md` §3.4（内部工具链硬约束：类型注解 / 依赖锁定 / 输出确定性） |

## 3. 证据锚点

| 证据 | 位置 |
|------|------|
| 参考实现 | `python/saveinspector_tools/save_format.py` |
| 生成器 | `python/saveinspector_tools/gen_samples.py` |
| 测试 | `python/tests/test_save_format.py`（21 例全绿） |
| 依赖锁定 | `python/pyproject.toml`（`pytest==7.4.4` 固定；运行时零依赖） |
| 产物引用 | 本文件 |

## 4. 关键决策

| 决策 | 理由 | 被否决的备选 |
|------|------|--------------|
| **Python 侧独立按契约重写一遍格式编码，不复用任何 C++ 产物** | 若两侧共用一份「真值」，交叉验证就退化为自证。本模块存在的全部价值就是**独立第二实现**——它与 C++ 各自按契约 §2 写，才能互相印证对错 | 从 C++ 导出样本再让 Python 校验（循环论证） |
| 运行时零第三方依赖 | 样本生成器要能在离线 CI 上跑；用标准库 `struct` 即可 | 引入 `crcmod`（省几行，但换来一个可失败点，且版本要锁） |
| 样本覆盖六种损坏场景 + 资源耗尽样本 | 契约 §2.3 的六种场景必须有可复现的输入，否则「部分损坏仍可读」这类需求无法回归 | 只生成正常样本（损坏场景靠手改字节，不可靠也不可复现） |
| `sample_corrupted_checksum` 只改校验和字段、不动数据 | 这样 CRC 会失配但**槽位数据仍可解析**，正好覆盖「部分损坏仍展示可读内容」这一 PRD US-3 硬需求。若连数据一起改，就测不到这个分支 | 同时改校验和与数据（退化为「完全不可读」场景） |
| 不引入随机数 | 输出必须确定，否则样本无法用于回归比对 | `random.seed(0)`（仍不如直接硬编码清晰） |

## 5. 风险与阻塞

- **`from __future__ import annotations` 与 `int | None` 注解**：需要 Python 3.10+。已在 `pyproject.toml` 声明 `requires-python = ">=3.10"`，并在 CI 环境实测（Python 3.12）。**若目标环境是 3.9，需降级为 `Optional[int]`**——目前无证据表明需要兼容更低版本，故按 3.10 起步。
- **无跨语言胶水测试**：pytest 断言的是「Python 实现是否符合契约」，而「C++ 解析结果与 Python 是否一致」靠两条 CRC64 测试向量（C++ 与 Python 各断言同一个 `0x6C40DF5F0B497347`）+ 端到端人工用 CLI 解析 Python 生成的样本。**这是一个已知的验证缺口**：没有自动化断言「C++ 解析 Python 样本的结果 == Python 预期的结构」。建议 G3 由 `test-engineer` 补一条端到端用例。**已如实登记，未当作已完成。**
- 样本写在 `build/samples/`（构建目录），不进版本库，符合 `.gitignore`。

## 6. 产出

| 产物 | 路径 |
|------|------|
| 参考实现 | `python/saveinspector_tools/save_format.py` |
| 样本生成器 | `python/saveinspector_tools/gen_samples.py` |
| 包入口 | `python/saveinspector_tools/__init__.py` |
| pytest 用例 | `python/tests/test_save_format.py` |
| 依赖锁定 | `python/pyproject.toml` |
| 生成样本 | `build/samples/*.sav`（11 个，不入库） |

## 7. 结论

| 项 | 内容 |
|------|------|
| 角色级裁决 | ✅ 完成并移交 |
| 风险 / 影响 | Low（验证缺口已登记，不阻塞 G2） |
| 备注 | 自检 `roles.md` §3.4 硬约束：① 全部函数带类型注解；② 依赖固定版本并写入 `pyproject.toml`；③ 输出确定性（有 `test_generation_is_deterministic` 实证）。pytest 21/21 全绿。**交叉验证已实证**：Python 生成的 11 个样本，C++ CLI 全部解析出预期内容（正常 v3 输出「第 42 章 / 12480 金币 / 治疗药水 × 12」，v1 正确标注升级，校验和损坏样本正确定位到「槽位 2」，40 亿数组计数样本未崩溃）。 |

## 8. 复核记录

| 项 | 内容 |
|------|------|
| 复核人 | `reviewer`（已出具，见 `2026-10-13-reviewer-save-inspector-python.md`） |
| 复核结论 | ✅ 通过 |
| 复核摘要 | 按 `roles.md` §3.7 的 Python 清单核验：类型注解覆盖、依赖锁定、确定性。复核**独立运行了 `pytest`** 并另外执行了 `python -m saveinspector_tools.gen_samples`，确认 11 个样本可复现生成，未仅凭审计声明放行。 |

## 9. 移交

| 项 | 内容 |
|------|------|
| 交给 | `reviewer`（G2）、`test-engineer`（G3） |
| 下游依据 | 契约 §2、§2.1.1 |
| 开工前提 | G3 建议补一条「C++ 解析 Python 样本 == Python 预期结构」的端到端用例（§5 已登记该缺口） |
