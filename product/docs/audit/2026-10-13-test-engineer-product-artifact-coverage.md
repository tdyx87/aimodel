# 审计：test-engineer · 产物层覆盖缺口与测试层接线

## 1. 基础信息

| 字段 | 内容 |
|------|------|
| 角色 | `test-engineer` |
| 时间 | 2026-10-13 20:55 |
| 任务 | 补「配 `static_root` 的静态资源」覆盖；修复 E2E 浏览器选择；把 python 层接线做成不可静默跳过 |
| 门禁 | G3（为重新放行提供证据） |

## 2. 输入 / 依赖

| 输入 | 路径 / 来源 |
|------|-------------|
| 上游缺陷 | `2026-10-13-cpp-implementer-mingw-runtime-crash.md`（产物一访问界面即 SIGSEGV） |
| 原测试策略 | `product/docs/design/test-strategy.md` §1（分层）、§7（执行方式） |
| 既有用例 | `product/tests/local_http_test.cpp`、`product/tests/e2e/test_user_paths.py` |
| 执行环境 | Windows + MinGW GCC 13.2.0；Python 3.13.5；pytest 9.1.1 + pytest-timeout |

## 3. 证据锚点

| 证据 | 位置 |
|------|------|
| 新夹具（配 static_root） | `tests/local_http_test.cpp` `LocalHttpStaticTest` |
| 新用例（5 例） | 同文件：`ServesIndexAtRootWithSecurityHeaders`、`ServesEachAssetWithMatchingContentType`、`MissingAssetAndUnknownExtensionAreNotFound`、`TraversalStillRejectedWhenStaticRootConfigured`、`ApiRoutesStillWinOverStaticFiles` |
| web 目录注入 | `CMakeLists.txt` `SAVEINSPECTOR_TEST_WEB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/web"` |
| 浏览器候选 + 试错 | `tests/e2e/test_user_paths.py` `_browser_candidates()` / `_dump_dom()` |
| 超时保护 | `python/pyproject.toml` `dev = ["pytest==7.4.4", "pytest-timeout>=2.3"]` + `timeout = 300` |
| 构建脚本 | `scripts/build.sh`、`scripts/build.ps1`（解释器选择 + 缺 pytest 时显式失败） |

## 4. 关键决策

| 决策 | 理由 | 被否决的备选 |
|------|------|--------------|
| 新建 `LocalHttpStaticTest` 夹具，`static_root` 指向**源码树里的 `web/`**（CMake 传入宏） | 原来的夹具一律 `clear()` 掉 `static_root`，于是「GET / → 读文件 → 200」这条分支**在单测里一次都没被执行过**。指向真实 web/ 就不必造临时目录、拷文件 | 复制到 tmp 目录（多一层平台代码，且测的不是真资源）/ 只靠 E2E（E2E 只在特定环境跑，不够近） |
| 抽出 `HttpFixture` 基类，两个夹具只差 `static_root` | 避免把 `Send`/`Host`/`OpenRequest` 复制两份——复制出来的第二份迟早与第一份漂移 | 复制粘贴（维护陷阱） |
| E2E 浏览器改为「候选列表 + 逐个试到有 DOM」 | 本机实测：Edge 存在，但 `--headless --dump-dom` **静默产出空文件**（退出码 0、零字节）；取第一个命中项会让 DOM 级验证随机器随机失败 | 只取 `shutil.which` 第一个（现状，随机红）/ 直接判为 skip（把可跑的验证也丢掉） |
| 没有 DOM 产出时 `skip` 并写明原因，而不是断言失败 | 页面加载失败时 `--dump-dom` 仍会输出错误页 DOM，所以**空 DOM 只可能来自浏览器本身没产出** | 断言失败（把环境能力缺口记成界面缺陷，制造噪声） |
| 构建脚本缺 pytest 时**报错退出**，不再静默跳过 | 静默跳过 = 「产物层其实零覆盖却显示全绿」。这正是本次缺陷溜过 G3 的必要条件之一 | 保持静默跳过（同错重犯） |
| 解释器优先级 `$PYTHON` → 项目内 `.venv` → PATH | 系统 python 往往没装 pytest；项目内 venv 让「一条命令跑全流程」在干净机器上也能成立 | 只用 PATH python（干净机器上直接失败） |
| E2E **不**另开一条构建步骤 | `python/pyproject.toml` 的 `testpaths = ["tests", "../tests/e2e"]` 已声明一次跑两层；我最初加了独立 E2E 步骤，实测 41 例被跑了两遍（41 = 21 + 20），属重复执行，已撤掉 | 两条命令分别跑（重复执行、且两处 timeout 策略不一致） |

## 5. 风险与阻塞

- **原先 E2E 的静态资源断言为何没拦住**：E2E 确实有 `/index.html`、`/styles.css` 的断言，
  但它跑在 PowerShell 环境里，那里 PATH 上**没有** Git 的 mingw64，DLL 恰好解析正确。
  即「E2E 绿」是真的，只是它没有把「运行环境」当作受控变量。已由产品侧修复（产物自包含）
  关闭该变量。
- **浏览器 DOM 交互流仍未自动化**：`--dump-dom` 无法触发 file input，用户「点选择框 →
  选文件 → 上传」的 DOM 流程依旧无覆盖（原审计 §5 已登记，本次未变）。
- **三平台未验证**：本次全部在 Windows + MinGW 上跑；Linux/macOS 仍待补（G4 前置条件）。
- **本机 Edge 不可用于无头验证**：已升级为「试到底」，但若某台机器只有 Edge 且它不产出
  DOM，DOM 级验证会 skip —— 属于**如实登记的能力缺口**，不是通过。

## 6. 产出

| 产物 | 路径 / 结果 |
|------|-------------|
| 单测新增（HTTP 层 15 → 20 例） | `tests/local_http_test.cpp` |
| E2E 夹具修复 | `tests/e2e/test_user_paths.py` |
| 依赖声明 | `python/pyproject.toml`（补 `pytest-timeout`） |
| 构建脚本接线 | `scripts/build.sh`、`scripts/build.ps1` |
| 测试策略更新 | `docs/design/test-strategy.md` §1（数量与实际一致）、§7（执行方式与依赖） |
| 全量结果 | ctest **41/41**；单次 pytest **41/41**（21 格式 + 20 E2E）；`scripts/build.sh` 与 `scripts/build.ps1` 双双 `Build OK` |

## 7. 结论

| 项 | 内容 |
|------|------|
| 角色级裁决 | ✅ 覆盖缺口已闭环，且对「产物层」的验证从「在一个环境里跑过」升级为「产物自包含 + 覆盖该分支」 |
| 证据 | 新 5 例在 `static_root` 真实存在的前提下断言 200 + MIME + 正文非空 + 安全响应头 + 路径消毒；浏览器 DOM 验证实测在 Chrome 上渲染并命中全部文案与可访问性断言 |
| 风险 / 影响 | 修复前：静态资源分支**零覆盖**，且验证手段随环境抖动；修复后：Low（余下为已登记的三平台与浏览器交互缺口） |
| 备注 | 本次未修改任何已有断言的宽松度，也未删除用例；新增断言全部是「更严格」。 |

## 8. 复核记录

| 项 | 内容 |
|------|------|
| 谁复核 | `orchestrator`（G3 重新放行） |
| 复核方式 | 独立执行 `./scripts/build.sh Release` 与 `powershell -File scripts/build.ps1`；逐条核对新用例是否真的读取了 web/ 下的文件（而非空目录） |
| 复核结论 | 一致，见 `2026-10-13-orchestrator-gate-g3-recheck.md` |

## 9. 移交

| 项 | 内容 |
|------|------|
| 交给 | `security-reviewer`（静态资源分支的路径消毒断言已补，请复核是否足够）、`orchestrator`、`doc-engineer`（G4） |
| 开工前提 | 静态资源用例须保持「配 static_root」的形态，退回 `clear()` 即等于撤回覆盖 |
| 遗留 | 三平台验证、浏览器 file input 交互流 |
