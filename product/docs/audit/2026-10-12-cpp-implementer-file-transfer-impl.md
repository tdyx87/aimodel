# 角色审计记录

> 生成路径：`product/docs/audit/2026-10-12-cpp-implementer-file-transfer-impl.md`  
> 对应规范：`.role-system/agents.md` §6.6

---

## 1. 基础信息

| 字段 | 内容 |
|------|------|
| 角色 | cpp-implementer |
| 时间 | 2026-10-12 21:10 |
| 任务 | 修复 `file_transfer.cpp` 编译失败（4 个函数体错位拼接 + 误用 C++17 `<filesystem>`），并按契约补齐安全校验与单测 |
| 门禁 | G2 合入 |

## 2. 输入 / 依赖

| 输入 | 路径 / 来源 |
|------|-------------|
| 上游契约 | `product/docs/design/file-transfer-contract.md`（审计 `2026-10-12-architect-file-transfer.md`） |
| 需求来源 | `product/docs/prd/PRD-file-transfer.md`（审计 `2026-10-12-product-manager-prd-file-transfer.md`） |
| 界面稿 | `product/docs/design/file-transfer-ui.md`（审计 `2026-10-12-ui-designer-file-transfer.md`） |
| 工程约束 | `.role-system/agents.md` 第 6 节（锁定 C++11）、第 4 节（跨平台编译期宏隔离）、G3 零警告 |
| 既有约定 | `src/cpp/system_info_posix.cpp` 的 `<dirent.h>`/`sys/stat.h` 用法 |

## 3. 证据锚点

| 证据 | 位置 |
|------|------|
| 关键文件 | `product/src/cpp/file_transfer.cpp`（重写，694 行）、`product/include/aimodel/file_transfer.h` |
| 关键证据 | `file_transfer.cpp:6` C++11 合规说明；`:169` 起安全校验；`:338` 起公开接口 |
| 测试证据 | `product/tests/file_transfer_test.cpp`（14 个用例） |
| 构建证据 | `./scripts/build.ps1 -Config Release` → `100% tests passed, 0 tests failed out of 24`，`-Wall -Wextra -Wpedantic` 零警告 |

## 4. 关键决策

| 决策 | 理由 | 被否决的备选 |
|------|------|--------------|
| 用 `stat`/`mkdir`/`opendir`/`remove` 替代 `<filesystem>` | 项目锁 C++11（`CMakeLists.txt:12`），`<filesystem>` 是 C++17；且 `system_info_posix.cpp` 已有 POSIX 目录枚举先例 | 放宽到 C++17 —— 违反不变层工程规范，影响全部目标 |
| Windows 侧用 `_stat` 而非 `stat` | MinGW-w64 下 `struct stat` 与 `stat()` 在全局命名空间冲突，`::stat(path,&st)` 被解析为类型；`_stat64` 又与 `_stat` 宏映射的 `_stat64i32` 不匹配 | 直接 `::stat` —— 编译失败（实测两个报错） |
| 路径成分（分隔符 / `..`）**整体拒绝**而非清洗放行 | 原 `MakeSafeFilename` 把 `../escape.txt` 剥成合法的 `..escape.txt` 就落盘了，语义等于接受路径输入；单测实证该漏洞 | 保留「剥分隔符」式清洗 —— 路径穿越被绕过 |
| 点文件、尾点、首尾空白**整体拒绝** | 同理会把 `.hidden` 洗成 `hidden`、把 `a.exe ` 洗成可落盘名；Windows 落盘还会剥掉首尾空白，导致落盘名与校验名不一致（CWE-178） | 只校验清洗后的名字 —— 隐藏文件与扩展名绕过均可复现 |
| 扩展名比对前 trim 空白 | 实证 `a.exe `（尾随空格）绕过危险扩展名名单——`ToUpper("exe ")` ≠ `"EXE"`，而 Windows 落盘后就是 `a.exe`，属 High 级 | 不 trim —— 已被单测 `TrailingSpaceExtensionBypassIsRejected` 证伪 |
| 大小上限统一为 **10 MB** | 契约 §6.4 明确 10 MB，PRD/审计/界面稿三处一致；原头文件注释写 50 MB 属文档与契约矛盾 | 保留 50 MB —— 与契约冲突 |
| 新增 `enum class FileError` 承载契约 §5 错误码 | 原接口只有 bool，HTTP 层无法映射 `PATH_NOT_ALLOWED`/`TOO_LARGE` 等契约规定的 code | 只返回中文文案 —— 前端无法程序化判断 |
| 危险扩展名（25 类）**直接拒绝**而非清洗 | 契约 §6.3 第 4 条要求「禁止危险扩展名」；清洗后放行等于把 `payload.exe` 变成 `payloadexe`，仍存疑义 | 清洗放行 —— 语义偏离契约 |
| Windows 保留名比对**主名**（`CON.txt` 也拒） | 原实现只比完整名，`CON.txt` 漏过 | 只比完整名 —— 保留名绕过 |
| 写盘用 `ios::trunc` 而非 `ios::app` | 已用 `GetUniqueFilename` 保证不重名，app 语义（追加）无意义且会掩盖异常 | 保留 app —— 语义错误 |
| `ListUploads` 目录不存在时返回空列表而非失败 | 上传前不强制建目录，首次列出不应报错 | 返回失败 —— 契约未要求此行为但会造成 UI 误报错误 |

## 5. 风险与阻塞

- **🔴 已修（High，CWE-178）**：尾随空格绕过危险扩展名（`a.exe ` → 落盘 `a.exe`）。单测实证后修复（`ExtensionOf`/`MakeSafeFilename`/`IsSafeFilename` 三处加 trim）。
- **🟡 未修（Medium，CWE-367 TOCTOU）**：`AppendUpload` 的 `GetUniqueFilename`（`stat` 判空位）与 `ofstream` 打开之间存在竞态窗口，并发上传同名文件理论上可互相覆盖。**当前产品为单用户本地工具且 HTTP 层尚未接线，实际不可触发**；待 HTTP 接线引入并发请求时，`http_server` 层需加锁或改用 `O_EXCL` 原子创建。
- **HTTP 层未接线**：`FileTransfer` 尚未接到 `http_server.cpp`，`/api/file/upload`、`/api/file/download` 仍返回 404（`http_server.cpp:184` 对 `/api/` 前缀一律 404）。本任务只交付可测的服务端库层。
- **前端未实现**：`web/app.js` 无上传/下载交互。
- **跨平台仅验证 Windows**：Linux/macOS 分支（`opendir`/`mkdir`/`stat`）已按 `#if` 编译期隔离编写，但本环境未实测，`test-engineer` 需在三平台复核。

## 6. 产出

| 产物 | 路径 |
|------|------|
| 服务端实现（重写） | `product/src/cpp/file_transfer.cpp` |
| 公共接口（补错误码枚举 + 10MB 上限） | `product/include/aimodel/file_transfer.h` |
| 单测（14 用例） | `product/tests/file_transfer_test.cpp` |
| 构建接线 | `product/CMakeLists.txt`（新增 `file_transfer_test` 目标） |
| 审计 | `product/docs/audit/2026-10-12-cpp-implementer-file-transfer-impl.md` |

## 7. 结论

| 项 | 内容 |
|------|------|
| 角色级裁决 | ✅ 完成并移交 |
| 风险 / 影响 | Medium（库层自身无 Critical/High；遗留 TOCTOU 需 HTTP 层接线时处置） |
| 备注 | 编译零警告；`ctest` 24/24 通过（含 14 个 file_transfer 用例，覆盖路径穿越、绝对路径、保留名、危险扩展名含尾随空格绕过、10MB 上限边界、中文文件名）。**库层已可交付，但功能对用户尚不可见**（HTTP/前端未接）。 |

## 8. 复核记录

- 复核人 | orchestrator
- 复核时间 | 2026-10-12 21:35
- 复核结论 | 通过
- 复核摘要 | 抽检核对三点：(1) **产出一致**——审计声明「`ctest` 24/24 通过、零警告」，实测 `ctest --test-dir build -C Release` 确为 `100% tests passed, 0 tests failed out of 24`，`./scripts/build.ps1 -Config Release` 输出无 warning；产物文件均存在（`file_transfer.cpp` 694 行、`file_transfer.h`、`file_transfer_test.cpp`、`CMakeLists.txt` 已含 `file_transfer_test` 目标）。(2) **关键决策理由成立**——「不用 `<filesystem>`」与 `CMakeLists.txt:12` 的 `CMAKE_CXX_STANDARD 11` 一致；「路径成分整体拒绝而非清洗」被单测 `RejectsPathTraversal` 反证成立（原实现确实会放行 `../escape.txt`）；「10MB 上限」与契约 §6.4 一致。(3) **风险自陈无隐瞒**——主动记录了已修的 High 与未修的 TOCTOU，且后者确为现状（HTTP 层未接线，`grep 'api/file'` 仅命中文档）。复核通过，可放行进入 G3。

## 9. 移交

| 项 | 内容 |
|------|------|
| 交给 | `reviewer`（C++ 清单，G2）、`security-reviewer`（G3，重点复核路径穿越与扩展名校验） |
| 下游依据 | `product/docs/design/file-transfer-contract.md` §4/§5/§6；本审计 §4 关键决策 |
| 开工前提 | `reviewer` 按 C++ 清单审查（C++11 合规、跨平台宏隔离、RAII、导出宏、测试配套）；`security-reviewer` 重点验 §5 已修漏洞是否真修、TOCTOU 风险是否可接受 |
