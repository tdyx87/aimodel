# 审计：release-engineer · 三平台编译与全量测试接入 GitHub Actions

## 1. 基础信息

| 字段 | 内容 |
|------|------|
| 角色 | `release-engineer` |
| 时间 | 2026-10-13 21:40 |
| 任务 | 把 Linux / macOS 的编译与全量测试接入 GitHub Actions，并修掉「只有非 Windows 才编译」的代码里已可见的移植缺陷 |
| 门禁 | G4（发布前检查：三平台可构建）；同时为 G3「三平台验证」未完成项提供执行机制 |

## 2. 输入 / 依赖

| 输入 | 路径 / 来源 |
|------|-------------|
| 上游未完成项 | `2026-10-13-orchestrator-gate-g3-recheck.md` §9：G4 前置条件①Linux 跑一次 `scripts/build.sh`；②macOS 同上 |
| 执行机制要求 | `2026-10-13-orchestrator-gate-g3-recheck.md` §8：门禁证据应附环境指纹 |
| 构建入口 | `product/scripts/build.sh`（含 ctest + Python 单测 + E2E） |
| 工程规范 | `.role-system/agents.md` §3（构建）、§7.3（平台差异只用编译期隔离） |
| 用户约束 | 「linux / macos 要通过 github ci 来编译」「不要本地验证 linux/macOS，要 github actions 验证」 |

## 3. 证据锚点

| 证据 | 位置 |
|------|------|
| CI 定义 | `.github/workflows/ci.yml`（ubuntu-latest + macos-latest，调用 `scripts/build.sh Release`） |
| macOS 编译阻塞（已修） | `product/src/cpp/local_http_posix.cpp`：`MSG_NOSIGNAL` 加 `#if defined(...)` 分支；补 `#include <cerrno>` |
| 浏览器候选（CI 需要） | `product/tests/e2e/test_user_paths.py` `_browser_candidates()`：补 macOS `.app` 与 Linux 标准路径 |
| E2E 容器兼容 | 同文件 `_dump_dom()` 增 `--disable-dev-shm-usage` |
| 可执行位（CI 前置） | `product/scripts/build.sh` 由 100644 → 100755（`git update-index --chmod=+x`，提交 739f211） |
| 文档 | `product/docs/design/test-strategy.md` §7.1 |

## 4. 关键决策

| 决策 | 理由 | 被否决的备选 |
|------|------|--------------|
| CI 直接调用 `scripts/build.sh`，**不**在 YAML 里内联 cmake 步骤 | 两套构建定义必然漂移；且脚本「缺 pytest 就报错退出」的硬约束只有在 CI 真调它时才起作用 | 在 YAML 里手写 configure/build/ctest/pytest（与本地入口脱钩，漂移无人发现） |
| 矩阵只取 `ubuntu-latest` + `macos-latest` | 用户明确要求的就是这两个平台；Windows 已在本地以 MinGW 工具链验证过（ctest 41/41 + pytest 41/41）。MSVC 是**另一条工具链**而非另一个平台，另议 | 把 `windows-latest`（MSVC）也塞进来：多一条**本次未验证过**的腿，首次运行即可能红，且不是本次范围 |
| 不缓存 `build/`（含 `_deps` 里 googletest 的产物） | 构建产物与生成器/编译器版本强耦合，缓存会把「换镜像后莫名链接失败」引进来；省几十秒不值这个风险 | `actions/cache` 缓存 `build/_deps`（节省 gtest 编译，但引入环境耦合风险） |
| `MSG_NOSIGNAL` 用 `#if defined(MSG_NOSIGNAL)` 分支；macOS 走 `0`，靠 `NetGlobalInit` 的进程级 `SIGPIPE` 忽略兜底 | Darwin 没有 `MSG_NOSIGNAL`：无条件使用会让 macOS **编译失败**。进程级忽略后写断开连接只返回 `-1/EPIPE`，由既有 `n <= 0` 分支处理 | 给每个 `accept` 的连接设 `SO_NOSIGPIPE`（多一处对 accept 返回值的依赖，收益相同） |
| 补 `#include <cerrno>` | 该文件用了 `errno`/`EADDRINUSE` 却没直接包含声明头，只是靠其他头间接引入——换 libc/头版本即可能断 | 依赖传递包含（可移植性靠运气） |
| 浏览器候选补 macOS/Linux 安装路径 | macOS 上 Chrome 只以 `.app` 存在、**不在 `PATH`**；只查 `which` 会让 DOM 级验证在 CI 上静默 skip，等于丢掉首屏渲染验证 | 只查 `PATH`（CI 上 DOM 验证静默消失） |
| 指纹步骤避免 `\| head`，改用 `sed -n` | 作业 bash 带 `-eo pipefail`：上游被 SIGPIPE 打断会让这一步无辜变红，而它与被测代码无关 | `\| head -2`（偶发无故红） |
| CI 继续调用文档化的 `./scripts/build.sh`，而不是改成 `bash scripts/build.sh` | 让「可执行位」本身也被 CI 守着：将来模式再丢，CI 立刻报红，而不是等用户克隆后才发现仓库是坏的 | `bash scripts/build.sh`（把模式问题掩盖过去） |

## 5. 风险与阻塞

- **⚠ CI 结论尚未观察，不得视为已验证**。本记录提交后由 GitHub Actions **首次执行**；
  在运行变绿之前，「Linux/macOS 可构建且用例全绿」这一结论**尚未成立**。
  这也是本次约定的验收方式（用户明确要求不在本地验证 linux/macOS）。
  首次运行结论应在 Actions 页面回填到 §7。
- **macOS 侧只做到「静态排查 + 交 CI 编译」**：`MSG_NOSIGNAL` 与 `<cerrno>` 两处是
  按「Darwin 无该宏／不直接包含声明头」的事实修改的，没有本地运行验证。若 CI 仍报错，
  以日志逐条修，不猜。
- **CI 上的 DOM 级验证依赖镜像自带浏览器**：若某平台没有可用浏览器，用例会 skip 并在
  日志写明原因（如实登记的能力缺口，不是通过）。ubuntu-latest 预装 `google-chrome`，
  macos-latest 预装 Google Chrome.app，两者都落在新的候选列表里。
- **Python 版本**：CI 固定 3.12，与 `pyproject.toml` 里 `pytest==7.4.4` 的钉版相容；
  本地开发机是 3.13 + pytest 9.1.1，差异由 CI 覆盖。
- **`build.sh` 缺可执行位（已修）**：它在 git 里是 100644，而文档与 CI 都用 `./scripts/build.sh` 调用。Windows 上 `./x.sh` 照样能跑（bash 直接解释），所以本地一直未暴露；Linux/macOS 上会直接 `Permission denied`，足以让两条腿在构建之前同时变红。已在提交 739f211 改为 100755，并确认全仓库再无「带 shebang 却无 x 位」的文件。
- **未纳入 CI 的范围**：MSVC 工具链、安装包与签名（`release-engineer` 的其余检查单）。

## 6. 产出

| 产物 | 路径 / 结果 |
|------|-------------|
| CI 定义 | `.github/workflows/ci.yml`（2 个作业腿，`timeout-minutes: 40`，并发取消旧运行） |
| 移植修复 | `local_http_posix.cpp`（`MSG_NOSIGNAL` 分支、`<cerrno>`） |
| E2E 适配 | `test_user_paths.py`（macOS/Linux 浏览器路径、`--disable-dev-shm-usage`） |
| 文档 | `test-strategy.md` §7.1 |
| 本地可验证的一条腿 | Windows + MinGW：`./scripts/build.sh Release` → ctest **41/41**、pytest **41 passed**、`Build OK` |
| YAML 校验 | 用 PyYAML 解析并通过结构断言（矩阵 = ubuntu/macos、`working-directory: product`、5 个步骤） |
| 可执行位修复 | `git ls-files -s product/scripts/build.sh` → **100755** |

## 7. 结论

| 项 | 内容 |
|------|------|
| 角色级裁决 | ⏳ **待 CI 结论**（不自行判定通过） |
| 已确立的部分 | CI 机制已就位并与本地/文档构建同源；`MSG_NOSIGNAL` 这一**必然阻塞 macOS 编译**的缺陷已修 |
| 待回填的部分 | 首次运行结论（作业名 = `ubuntu-latest` / `macos-latest`）：是否为 `success`、ctest 与 pytest 的计数、DOM 级验证是跑了还是 skip |
| 风险 / 影响 | 修复前：Linux/macOS **从未编译过**（`save_file_posix.cpp`/`local_http_posix.cpp` 在 Windows 构建里不参与编译），三平台结论无从谈起；机制就位后：可持续回归 |

## 8. 复核记录

| 项 | 内容 |
|------|------|
| 本地能复核的部分 | Windows 腿全量重跑：ctest 41/41、pytest 41/41（含真实浏览器首屏 DOM 验证）、`Build OK`、退出码 0 |
| workflow 静态复核 | YAML 可解析；矩阵、工作目录、超时、步骤顺序符合预期 |
| 可执行位复核 | `git ls-files -s` 确认为 100755；仓库内不再有「带 shebang 却无 x 位」的文件 |
| 不能复核的部分 | Linux/macOS 的实际编译与运行——**按用户要求不在本地验证**（不启用 WSL/Docker），交由 CI |

## 9. 移交

| 项 | 内容 |
|------|------|
| 交给 | `orchestrator`（G3/G4 放行需以 CI 结论为准）、`test-engineer`（CI 上 DOM 验证的实际执行情况） |
| 放行前提 | `.github/workflows/ci.yml` 首次运行**全绿**；任何一条腿红都按缺陷回流，不降低条件放行 |
| 建议下一步 | ① 回填首次 CI 结论到 §7；② 视需要把 MSVC（windows-latest）纳入矩阵，闭合「另一条 Windows 工具链」 |
