# 审计：release-engineer · 三平台构建与全量测试接入 GitHub Actions

## 1. 基础信息

| 字段 | 内容 |
|------|------|
| 角色 | `release-engineer` |
| 时间 | 2026-10-13 22:30（初版）／2026-10-13 23:10（按「先 Windows」重排后更新） |
| 任务 | 把三平台的编译与全量测试接入 GitHub Actions；修掉 CI 才能暴露的移植与前置缺陷 |
| 门禁 | G4（发布前检查：三平台可构建）；同时为 G3「三平台验证」未完成项提供执行机制 |
| 放行顺序 | **先 Windows**（阻塞），Linux/macOS 先只观察、后期再验证 |

## 2. 输入 / 依赖

| 输入 | 路径 / 来源 |
|------|-------------|
| 上游未完成项 | `2026-10-13-orchestrator-gate-g3-recheck.md` §9：G4 前置条件①Linux 跑 `scripts/build.sh`；②macOS 同上 |
| 环境指纹要求 | 同上 §8：门禁证据应附环境指纹，否则「全绿」不可比对 |
| 构建入口 | `product/scripts/build.ps1`、`product/scripts/build.sh`（含 ctest + Python 单测 + E2E） |
| 工程规范 | `.role-system/agents.md` §3（构建）、§7.3（平台差异只用编译期隔离） |
| 用户约束 | ①「linux / macos 要通过 github ci 来编译」②「不要本地验证 linux/macOS，要 github actions 验证」③「先做 windows 也可以，后期验证 linux/macOS」 |

## 3. 证据锚点

| 证据 | 位置 |
|------|------|
| CI 定义 | `.github/workflows/ci.yml`（windows 阻塞 + ubuntu/macos 非阻塞，统一调用构建脚本） |
| 生成器钉死 | `scripts/build.ps1` `-Generator/-Arch`；`scripts/build.sh` `--generator/--arch` |
| macOS 编译阻塞（已修） | `src/cpp/local_http_posix.cpp`：`MSG_NOSIGNAL` 加 `#if defined(...)` 分支；补 `#include <cerrno>` |
| 可执行位（CI 前置，已修） | `scripts/build.sh` 由 100644 → 100755（提交 `739f211`） |
| 浏览器候选（CI 需要） | `tests/e2e/test_user_paths.py` `_browser_candidates()`：补 Windows/macOS/Linux 标准路径 |
| 文档 | `docs/design/test-strategy.md` §7.1 |

## 4. 关键决策

| 决策 | 理由 | 被否决的备选 |
|------|------|--------------|
| CI 直接调用 `scripts/build.{ps1,sh}`，**不**在 YAML 里内联 cmake 步骤 | 两套构建定义必然漂移；且脚本「缺 python/pytest 就报错退出」这条硬约束，只有在 CI 真调它时才起作用 | 在 YAML 里手写 configure/build/ctest/pytest（与本地入口脱钩） |
| **显式钉死生成器与架构**（Windows 腿 `-G "Visual Studio 17 2022" -Arch x64`） | 不指定时 CMake 默认生成器**因机器而异**：本机实测默认是 **Ninja + MinGW**（`CMAKE_GENERATOR:INTERNAL=Ninja`、`c++` 解析到 Strawberry），而运行器上会落到 Visual Studio + MSVC。工具链是构建输入之一，要复现就得能钉死 | 依赖「自动探测」（本机与 CI 会跑出两条不同工具链，且不指定时 CI 上到底选谁完全由镜像决定） |
| 生成器开关**默认留空**（保留自动探测） | 既有用法与文档不变；本机用户的 Ninja + MinGW 流程不受影响 | 硬编码 `-G`（会把本地已验证的 MinGW 流程一并改掉） |
| **Windows 阻塞、Linux/macOS 先非阻塞**（`continue-on-error: ${{ matrix.experimental }}`） | 按用户指定的放行顺序：先让 Windows 成为受控放行平台；Linux/macOS 仍每天跑、结果可见，但**不把绿的结论算数**，也不让它们把 CI 卡红 | ①三条腿全阻塞（当前 Linux/macOS 未验证，会把 CI 长期卡红）②不跑 Linux/macOS（丢掉了唯一的 POSIX 编译信号） |
| 矩阵用 `include` 而不是 `os: [...]` 列表 | 三条腿的 shell 与命令不同（pwsh vs bash、不同的脚本与参数），`include` 才能如实表达 | 列表 + 步内 `if` 判断（可读性差，且易漏） |
| `MSG_NOSIGNAL` 用 `#if defined(MSG_NOSIGNAL)` 分支；macOS 走 `0`，靠 `NetGlobalInit` 的进程级 `SIGPIPE` 忽略兜底 | Darwin 没有这个宏：无条件使用会让 macOS **编译失败**。进程级忽略后写已断开的连接只返回 `-1/EPIPE`，由既有 `n <= 0` 分支处理 | 给每个 `accept` 的连接设 `SO_NOSIGPIPE`（多一处对 accept 返回值的依赖） |
| 补 `#include <cerrno>` | 该文件用了 `errno`/`EADDRINUSE` 却没直接包含声明头，仅靠其他头间接引入——换 libc/头版本即可能断 | 依赖传递包含 |
| 指纹从 **CMakeCache** 读，不用 `c++ --version` | Windows 上没有 `c++` 命令；而 CMakeCache 三平台都有。另：VS 生成器**不缓存** `CMAKE_CXX_COMPILER`，故同时打印 `CMAKE_GENERATOR_INSTANCE`（即 VS 安装路径） | `c++ --version`（Windows 必失败）/ 解析构建日志（脆弱） |
| 安装依赖用 `python` 而非 `python3` | setup-python 三平台都提供 `python`，而 Windows 运行器上没有 `python3` | `python3`（Windows 腿会在装依赖那一步就红） |
| CI 保持调用文档化的 `./scripts/build.sh`，不改成 `bash scripts/build.sh` | 让「可执行位」本身也被 CI 守着：将来模式再丢，CI 立刻报红，而不是等用户克隆后发现仓库是坏的 | `bash scripts/build.sh`（把模式问题掩盖过去） |
| 指纹步骤不用 `\| head`，改 `sed -n` | 作业 bash 带 `-eo pipefail`，上游被 SIGPIPE 打断会让这一步无辜变红 | `\| head -2`（偶发无故红） |
| 不缓存 `build/` | `_deps` 里的 googletest 产物与生成器/编译器版本强耦合，缓存会把「换镜像后莫名链接失败」引进来 | `actions/cache`（省几十秒，换环境耦合风险） |

## 5. 风险与阻塞

- **Windows 腿：已在本机以 MSVC 全量验证通过**（用的就是 CI 会跑的那条命令，见 §8）。
  仍需 CI 那次运行本身变绿，才算「CI 上成立」。
- **Linux/macOS：完全未验证，且当前非阻塞**。按要求不在本地验证（不启用 WSL/Docker），
  只由 CI 观察。**不得把这两条腿的现状称为「通过」**——`experimental` 去掉之前，
  三平台验证这一项始终是未完成。
- **首次运行的 POSIX 编译结论未知**：`save_file_posix.cpp` 与 `local_http_posix.cpp`
  从未被编译过；已按「Darwin 无 `MSG_NOSIGNAL`」「不直接包含声明头」两条事实修了两处
  可见缺陷，但编译错误通常不会只有两处。以 CI 日志为准，逐条修，不猜。
- **CI 上的 DOM 级验证依赖镜像自带浏览器**：若某平台没有可用浏览器，用例会 skip 并在
  日志写明原因（如实登记的能力缺口，不是通过）。三平台的常见安装路径都已进候选列表。
- **MSVC 的运行时分发**：改用 MSVC 后产物依赖 VC 运行时（与 MinGW 侧静态链接
  `libstdc++` 的自包含策略不同）。GitHub 运行器自带，不影响 CI；分发形态留待发布环节。
- **未纳入范围**：安装包与签名、MSVC 静态 CRT 选项。

## 6. 产出

| 产物 | 路径 / 结果 |
|------|-------------|
| CI 定义 | `.github/workflows/ci.yml`（3 条腿；Windows 阻塞、ubuntu/macos 观察；`timeout-minutes: 60`；并发取消旧运行） |
| 生成器开关 | `scripts/build.ps1`（`-Generator`/`-Arch`）、`scripts/build.sh`（`--generator`/`--arch`） |
| 移植修复 | `local_http_posix.cpp`（`MSG_NOSIGNAL` 分支、`<cerrno>`） |
| 可执行位修复 | `scripts/build.sh` → 100755 |
| E2E 适配 | `test_user_paths.py`（三平台浏览器路径、`--disable-dev-shm-usage`） |
| 文档 | `test-strategy.md` §7.1 |
| **Windows 腿本地全量验证** | `build.ps1 -Config Release -Generator "Visual Studio 17 2022" -Arch x64` → **MSVC 19.44.35222.0**、ctest **41/41**、pytest **41 passed**（含真实浏览器首屏 DOM）、`Build OK`、退出码 0 |
| YAML 校验 | PyYAML 解析 + 结构断言（3 条腿、shell、工作目录、步骤顺序） |

## 7. 结论

| 项 | 内容 |
|------|------|
| Windows 腿 | ✅ **本机以 MSVC 全量验证通过**（命令与 CI 完全一致）；CI 首次运行结论待观察 |
| Linux / macOS 腿 | ⏳ **未验证**（未在本地验证，也尚未取得 CI 结论）；当前仅观察、不参与放行 |
| 角色级裁决 | ⏳ **待 CI 结论**（不自行判定通过）；Windows 变绿即满足本次「先做 Windows」的放行目标 |
| 待回填 | 首次运行结果（作业名 = `windows-latest` / `ubuntu-latest` / `macos-latest`）：结论、ctest 与 pytest 计数、DOM 验证是跑了还是 skip |
| 风险 / 影响 | 机制就位前：Linux/macOS **从未编译过**，三平台结论无从谈起；就位后：可持续回归，且工具链被钉死，可复现 |

## 8. 复核记录

| 项 | 内容 |
|------|------|
| Windows 腿（本地，MSVC） | ① 清空构建目录后按 CI 同一条命令执行；② 日志确认编译器识别为 `MSVC 19.44.35222.0`；③ `CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022`、`CMAKE_GENERATOR_PLATFORM:INTERNAL=x64`、`CMAKE_GENERATOR_INSTANCE:INTERNAL=C:/2022`；④ ctest **41/41**、pytest **41 passed**（无 skip，DOM 验证真跑了）；⑤ 产物位于 `build/Release/`，`web/` 随产物就位；⑥ 退出码 0 |
| 顺带修正一次误判 | 首次「MSVC 验证」其实仍是 MinGW：本机 CMake 的默认生成器是 Ninja，脚本未指定 `-G` 时自动探测到 Ninja + Strawberry。**这正是加 `-Generator` 开关的依据**——不钉死生成器，所谓「MSVC 验证」只是自欺 |
| 可执行位复核 | `git ls-files -s` 确认为 100755；仓库内不再有「带 shebang 却无 x 位」的文件 |
| workflow 静态复核 | YAML 可解析；`continue-on-error` 与矩阵 `experimental` 对应一致 |
| 不能复核的部分 | Linux/macOS 的实际编译与运行——按用户要求不在本地验证 |

## 9. 移交

| 项 | 内容 |
|------|------|
| 交给 | `orchestrator`（放行需以 CI 结论为准）、`test-engineer`（CI 上 DOM 验证的实际执行情况） |
| 放行前提 | Windows 腿在 CI 上**绿**；任何一条腿红都按缺陷回流，不降低条件放行 |
| Linux/macOS 转阻塞的条件 | 首次运行结论被人工复核、缺陷修完、连续为绿之后，去掉 `experimental`；届时同步更新本审计与 §7.1 表格 |
| 建议下一步 | ① 回填首次 CI 结论；② 三平台转阻塞后把「三平台验证」从 G4 前置条件中划掉 |
