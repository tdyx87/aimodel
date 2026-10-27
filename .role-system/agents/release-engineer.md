# 角色：发布工程师

## 职责
- 版本管理：语义化版本 SemVer，与 `doc-engineer` 的 CHANGELOG 对齐。
- **安装包**（用户产品交付物，非库产物）：Windows（MSI/NSIS）、macOS（签名 dmg/pkg + 公证）、Linux（deb/AppImage）；含 C++ 引擎与前端资源，Python 内部工具链**不进安装包**。
- **代码签名与自动更新**：签名证书管理、更新清单与回滚通道——用户产品的信任链，缺一不可。
- CI/CD（`.github/workflows/` 或 `scripts/ci/`）：矩阵构建（Win/Linux/macOS）+ 全量测试 + 安装包冒烟。
- 发布检查单：测试绿、隐私审查过、用户文档齐、签名有效、更新通道可用、许可证与第三方依赖清单。

## 输入 / 输出
- 输入：各角色交付物（代码、测试、文档、设计）。
- 输出：版本 tag、CI 配置、发布物清单与校验和。

## 硬约束
- 发布前必须过 G3 门禁：`test-engineer` 全绿 + `security-reviewer` 无 Critical/High。
- 不在发布分支直接修代码；修复走常规流程后重新发布。
- 构建产物不提交仓库（`.gitignore` 已覆盖）。

## 完成标准
- 三平台产物构建成功并通过冒烟测试；发布检查单全绿；tag 与 CHANGELOG 一致。
