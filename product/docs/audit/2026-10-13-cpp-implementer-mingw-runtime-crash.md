# 审计：cpp-implementer · MinGW 产物运行库崩溃（静态资源首个请求 SIGSEGV）

## 1. 基础信息

| 字段 | 内容 |
|------|------|
| 角色 | `cpp-implementer` |
| 时间 | 2026-10-13 20:40 |
| 任务 | 修复「产物一访问界面即整进程死亡」，并使产物不再依赖运行环境 |
| 门禁 | G3（本记录为 G3 重新放行提供证据；G3 放行记录见 `2026-10-13-orchestrator-gate-g3-recheck.md`） |

## 2. 输入 / 依赖

| 输入 | 路径 / 来源 |
|------|-------------|
| PRD | `product/docs/prd/PRD-save-inspector.md`（NFR-1 首屏可用、NFR-3 隐私） |
| 契约 | `product/docs/design/save-format-contract.md` §4（协议）、§7（已知坑） |
| 构建规范 | `.role-system/agents.md` §3（构建）、§7.3（平台差异只用编译期隔离） |
| 工具链 | 本机由 CMake 自动探测：**MinGW-w64 GCC 13.2.0（Strawberry）+ Ninja** |

## 3. 缺陷现象与根因

### 3.1 现象

| 请求 | 修复前 | 说明 |
|------|--------|------|
| `GET /api/save/slots` | `409 Conflict`（正常） | API 路由一切正常 |
| `GET /`、`/index.html`、`/app.js`、`/styles.css`、缺失的 `.js` | 连接被 RST、**0 字节响应** | 不是 404，是进程死亡 |
| 进程退出码 | `3221225477` = `0xC0000005` | ACCESS_VIOLATION（SIGSEGV） |

关键观察：**API 路径全绿，静态资源路径必死**。因此「用 HTTP 客户端打接口」的验证手段
完全看不到这个缺陷——而用户看到的是「点开界面，浏览器说无法连接」。

### 3.2 定位过程（可复现）

```bash
# 1) 崩溃点：gdb 里跑真实产物，另起进程发 GET /
gdb -batch -ex "set args --port 39323 --no-open --web-root build/web" -ex run -ex bt ./build/save_inspector_ui.exe
# → SIGSEGV，栈顶三帧全在 D:\Program Files\Git\mingw64\bin\libstdc++-6.dll 内，
#   第 4 帧才是 saveinspector::(anonymous namespace)::HandleRoute

# 2) 产物声明依赖的动态库
objdump -p build/save_inspector_ui.exe | grep "DLL Name"
# → libgcc_s_seh-1.dll、libstdc++-6.dll、libwinpthread-1.dll、WS2_32.dll …

# 3) 反证：把构建工具链的 bin 目录放到 PATH 最前，同一份二进制即恢复正常
PATH="/c/Strawberry/c/bin:$PATH" python3 probe.py
# → GET / 200 OK (1748B)、/styles.css 200、/app.js 200
```

### 3.3 根因

GCC（MinGW）默认**动态**链接 `libstdc++`/`libgcc`，运行时由加载器按 PATH（及 exe 同目录）
顺序解析同名 DLL。本机 PATH 上第一个 `libstdc++-6.dll` 来自 **Git for Windows 的
mingw64（另一版本的 GCC）**，而 `save_inspector_ui.exe` 由 Strawberry GCC 13.2.0 编译：
两份运行库的 C++ ABI 不匹配，第一个走到流/文件缓冲的请求（即静态资源读取）直接在
运行库内部越界。

**这不是「某个函数写错了」，而是产物与环境耦合**：同一份二进制，换个 PATH 顺序就换一个
结果——本机 Git Bash 里必崩，PowerShell 里正常（PowerShell 的 PATH 无 Git 的 mingw64）。
这也解释了为什么 G3 阶段 E2E 能全绿却漏掉它。

## 4. 关键决策

| 决策 | 理由 | 被否决的备选 |
|------|------|--------------|
| 给所有**可执行文件**静态链接 `libgcc`/`libstdc++`（`CMAKE_EXE_LINKER_FLAGS` 追加 `-static-libgcc -static-libstdc++`） | 一次性把根因去掉：产物自包含，不再询问 PATH 上「谁的运行库」 | ① 文档要求用户注意 PATH 顺序（换台机器即复发，且用户无从得知）② 把 DLL 拷到 exe 同目录（分发物变多，且同目录仍可能与系统其他 GCC 运行库打架） |
| 用 `CMAKE_EXE_LINKER_FLAGS` 而非逐个 `target_link_options` | 全局变量保证**将来新增的可执行文件**也自动自包含，不会被人忘记加 | 逐个目标加（漏一个目标 = 留一个同样会崩的产物） |
| 不追求「全静态」（`-static`） | 该工具链**没有** `libwinpthread.a` 静态库（已核实），`-static` 会直接链接失败 | `-static`（链接不过）/ 静态链接 winpthread（无库可用） |
| 接受 `libwinpthread-1.dll` 仍为动态依赖 | 它是稳定的 C API（pthreads-w32 接口），不涉及 C++ ABI；实测在 Git 的 mingw64 运行库下正常工作 | 为它再发明一套自包含方案（无必要，且改动面更大） |

## 5. 风险与阻塞

- **残留依赖**：产物仍动态依赖 `libwinpthread-1.dll`（工具链无静态版）。已核实两种来源
  下均可用；若将来发布到无该 DLL 的机器上，会表现为「缺 DLL 无法启动」——这属于可诊断的
  启动期错误，不再是随机崩溃。
- **POSIX 侧未验证**：静态链接只影响 MinGW 分支（`WIN32 AND GNU`），Linux/macOS 行为不变；
  但 §3.8 的三平台验证仍未完成（G3 放行记录已把它列为 G4 前置条件）。
- **环境相关缺陷的通用教训**：本缺陷在「另一个人用另一台机器」时才暴露，而当时的验证手段
  （HTTP 打接口 + 单层 E2E）都没有覆盖「产物能否稳定服务界面」。已由 `test-engineer`
  补上产物层断言，见 `2026-10-13-test-engineer-product-artifact-coverage.md`。

## 6. 产出

| 产物 | 路径 |
|------|------|
| 构建修复 | `product/CMakeLists.txt`（MinGW 静态运行库段落 + `local_http_test` 的 web 目录定义） |
| 验证脚本（临时，未入库） | `/tmp/probe2.py`（起真实产物 + 裸 socket 断言状态码） |

## 7. 结论

| 项 | 内容 |
|------|------|
| 角色级裁决 | ✅ 修复完成，且在**原先必崩的条件下**复验通过 |
| 证据 | ① `objdump -p` 导入表已无 `libstdc++-6.dll`/`libgcc_s_seh-1.dll`；② 置 PATH 为 Git mingw64 优先时 `GET /`、`/styles.css`、`/app.js` 均 **200 且正文非空**；③ 全量 ctest 41/41、pytest 41/41 |
| 风险 / 影响 | 修复前：**High**（用户界面完全不可用，且随环境随机发生）；修复后：Low（仅剩可诊断的 DLL 缺失风险） |
| 备注 | 本记录不修改历史审计：G3 当时的「ctest 36/36 + E2E 20/20 全绿」是**在那个环境下**的真实结果。缺陷不在谁偷懒，而在**产物与环境耦合**未被任何一层覆盖。 |

## 8. 复核记录

| 项 | 内容 |
|------|------|
| 谁复核 | `orchestrator`（G3 重新放行） |
| 复核方式 | 独立运行 `scripts/build.sh` 与 `scripts/build.ps1`；在 Git Bash（污染 PATH）与 PowerShell 两种环境下各打一次 `GET /`，核对状态码与响应体 |
| 复核结论 | 见 `2026-10-13-orchestrator-gate-g3-recheck.md` |

## 9. 移交

| 项 | 内容 |
|------|------|
| 交给 | `test-engineer`（补产物层覆盖）、`reviewer`（复核构建改动是否越界）、`orchestrator`（G3 重放行） |
| 开工前提 | 静态资源分支必须在单测层有覆盖，否则同类缺陷仍会漏过 |
| 遗留 | 三平台验证（G4 前置）；`libwinpthread-1.dll` 仍为动态依赖（已登记） |
