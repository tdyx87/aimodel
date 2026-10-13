# 测试策略

| 字段 | 内容 |
|------|------|
| 角色 | `test-engineer` |
| 版本 | 0.1 |
| 上游 | PRD US-1~US-7、NFR-1~NFR-5；契约 §2/§4/§5 |

---

## 1. 分层与边界

| 层 | 位置 | 数量 | 覆盖什么 | 不覆盖什么 |
|----|------|------|---------|-----------|
| 库层单测 | `tests/save_inspector_test.cpp` | 21 | 格式解析、版本迁移、CRC64、损坏判定、资源耗尽防护 | HTTP 拼装、界面 |
| HTTP 层单测 | `tests/local_http_test.cpp` | 20 | 请求闸门、请求体完整性、状态码、**静态资源（含配 `static_root` 的读取、MIME、路径消毒）**、并发与 Stop | 真实可执行文件、界面 |
| 内部工具单测 | `python/tests/test_save_format.py` | 21 | Python 参考实现是否符合契约 | C++ 侧行为 |
| 用户场景 E2E | `tests/e2e/test_user_paths.py` | 20 | **真实可执行文件** → 真实 HTTP → 用户可见结果 | DOM 交互流 |

> 边界原则：每层只断言本层负责的事。库层不测 HTTP（那里没有 HTTP），
> E2E 不断言内部字段（那会把实现细节写死成契约）。

---

## 2. 五条主路径的覆盖方式

`frontend-implementer` 审计 §9 移交要求覆盖的五条，逐条对应：

| 主路径 | 验证层次 | 用例 |
|-------|---------|------|
| 首屏空态 | **DOM 级**（真实浏览器） | `test_empty_state_renders_in_real_browser` |
| 打开正常存档 | 集成级 | `test_open_healthy_save_lists_slots` |
| 部分损坏（能读多少显示多少） | 集成级 | `test_partially_corrupted_save_keeps_readable_slots` |
| 完全不可解析 | 集成级 | `test_unparseable_save_yields_actionable_diagnosis`（4 个参数化样本） |
| v1/v2 旧档升级 | 集成级 | `test_legacy_save_is_upgraded_and_labelled`（2 个参数化样本） |

---

## 3. E2E 的两条纪律

### 3.1 不 mock 核心链路

E2E 从 `save_inspector_ui` 可执行文件启动，经真实 TCP 走契约 §4 的端点，
存档字节由 Python 参考实现生成、经真实上传路径送达。全程无桩、无内存注入。

判据：把解析库换成任何实现，只要契约不变，E2E 都应通过。

### 3.2 断言用户可见结果，不断言实现细节

- ✅ 断言「槽位 2 的数据已被改动」出现在诊断里（US-5「哪里坏了」）
- ✅ 断言诊断不含 `0x` / `C:`（US-5 禁开发者术语、契约 §5 禁路径泄漏）
- ✅ 断言导出内容与界面所见一致（US-4）
- ❌ 不断言「解析器内部调了几次 `ReadU64`」——那是实现细节，会阻碍合法重构

---

## 4. 已闭环的登记缺口

| 缺口来源 | 缺口 | 闭环方式 |
|---------|------|---------|
| `python-implementer` 审计 §5 | 缺「C++ 解析 Python 样本 == Python 预期结构」的自动化断言 | `test_partially_corrupted_save_keeps_readable_slots` 等用例**以 Python 生成的样本为输入**并断言解析结果，等价闭环 |
| `reviewer` 🟡-1 | 共享 `inspector` 未加锁，依赖「每请求即关闭连接」 | `test_close_per_request_premise_holds` 断言 `Connection: close` 前提；`test_concurrent_requests_do_not_corrupt_state` 作回归哨兵 |
| `frontend-implementer` 审计 §5 | 空槽位判定靠前端启发式 | **未闭环**，见 §6 |

---

## 5. E2E 抓出的真实缺陷（本次的实际价值）

**`/api/save/slot` 返回非法 JSON** —— 服务端拼成 `{"ok":true,{"slot":1,...}}`，
前端 `response.json()` 抛异常，**槽位详情页完全打不开**。

为什么前三层都没抓到：

| 层 | 为什么漏 |
|----|---------|
| 库层单测 | 没有 HTTP 拼装，`SlotToJson` 的返回值本身是合法的 |
| HTTP 层单测 | 只测了 `/api/save/slots`（数组拼接），没测 `/api/save/slot`（对象拼接） |
| 前端 | 无 JS 测试层（免构建形态的既定取舍） |

**这正是「用户场景 E2E」不可替代的证据**：两个测试都通过，功能却是坏的。
修复后新增 `test_slot_endpoint_returns_wellformed_json` 作回归哨兵。

第二个缺陷：**打开失败后服务端仍保留上一个存档的数据**。界面已切到诊断视图，
但 `/api/save/slots` 仍返回旧存档槽位 —— 服务端状态与界面所见不一致。
已通过 `SaveInspector::Clear()` 修复，并由 `test_unparseable_...` 断言打开失败后
`/api/save/slots` 返回 409。

---

## 6. 未覆盖项（如实登记）

| 项 | 原因 | 建议 |
|----|------|------|
| **DOM 交互流**（点击文件选择框 → 上传 → 点卡片 → 详情） | 环境无 Playwright/Selenium；`--dump-dom` 无法触发 file input | 在有浏览器自动化的环境补 Playwright 用例。已验证的等价物是「真实浏览器渲染首屏 + HTTP 层走完数据路径」 |
| **Linux / macOS 三平台验证** | 本环境仅 Windows | `roles.md` §3.8 要求平台相关逻辑三平台验证。**必须在有 macOS/Linux 的机器上补跑**，否则 G3 的三平台要求无法真正闭合 |
| 空槽位判定启发式 | 契约未提供 `is_empty` 字段 | 契约下一版补字段后，用例可加断言 |
| 超大文件（>512 MB）的实际行为 | 测试环境不便生成 | 已有上限校验的单测；端到端行为待专门环境验证 |
| 压缩标记存档的解压 | 本期不做（契约 §2.3） | 不适用 |

---

## 7. 执行方式

```powershell
# Windows：一条命令跑全流程（CMake 构建 + ctest + Python 层）
./scripts/build.ps1 -Config Release
```

```bash
# Linux / macOS
./scripts/build.sh Release
```

一次 `pytest` 就能同时跑到内部工具单测与用户场景 E2E：`python/pyproject.toml`
的 `testpaths` 已声明 `["tests", "../tests/e2e"]`。因此**必须从 `python/` 目录
执行**，否则只收集到其中一层：

```powershell
cd python; pytest -q                # 21 格式 + 20 E2E
cd python; pytest ../tests/e2e -v   # 只跑 E2E
```

Python 层需要 `pytest`；E2E 的超时保护还需要 `pytest-timeout`（两者都在
`python/pyproject.toml` 的 `dev` extra 里声明）。两个构建脚本按
`$PYTHON` → 项目内 `.venv` → PATH 的顺序挑选解释器；**缺 python/pytest 时脚本
报错退出，而不是静默跳过**——静默跳过等于「产物层其实零覆盖、却显示全绿」，
本项目正是这样漏掉过一次产物崩溃（见审计
`2026-10-13-cpp-implementer-mingw-runtime-crash.md`）。要主动跳过请显式传
`--no-python-tests` / `-NoPythonTests`。

E2E 依赖已构建的可执行文件；未构建时**自动 skip 而非失败**，避免在只跑
单测的场景里产生噪声。

### 7.1 三平台续验由 CI 执行

Windows 之外的平台不在开发机上；靠「记得去跑一次」等于没有跑。
`.github/workflows/ci.yml` 在 `ubuntu-latest` 与 `macos-latest` 上各跑一次**同一个**
`scripts/build.sh Release`：

- 不为 CI 另写一套构建步骤——两套定义必然漂移，而漂移出来的那套不会有人维护；
- 作业日志打印工具链指纹（`uname -a`、`c++ --version`、`cmake --version`、Python
  版本），让「全绿」可比对、可复现；
- POSIX 专有代码（`save_file_posix.cpp`、`local_http_posix.cpp`）**只在 CI 上**
  得到编译与运行验证：它们在 Windows 构建里根本不参与编译；
- E2E 的 DOM 级验证需要真实浏览器，所以 `_browser_candidates()` 除 `PATH` 外还按
  macOS/Linux 的标准安装位置查找 Chrome/Chromium（macOS 的 Chrome 只以 `.app`
  形式存在，并不在 `PATH` 上），否则该验证会在 CI 上静默 skip。
