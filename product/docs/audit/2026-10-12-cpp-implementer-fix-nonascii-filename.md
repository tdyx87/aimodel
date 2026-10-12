# 角色审计记录

> 生成路径：`product/docs/audit/2026-10-12-cpp-implementer-fix-nonascii-filename.md`
> 对应规范：`.role-system/agents.md` §6.6

---

## 1. 基础信息

| 字段 | 内容 |
|------|------|
| 角色 | cpp-implementer |
| 时间 | 2026-10-12 23:32 |
| 任务 | 修复 Windows 上**中文（非 ASCII）文件名上传失败 / 落盘乱码** |
| 门禁 | G2 合入（修复） |

## 2. 输入 / 依赖

| 输入 | 路径 / 来源 |
|------|-------------|
| 上游产物 | `product/docs/design/file-transfer-contract.md`（§6.3 文件名约束，未限制非 ASCII） |
| 触发 | 用户反馈「上传中文失败」；端到端复现确认 |
| 规范 | `.role-system/agents.md` §7.3.3（Windows 宽字符 API 输出一律经 UTF-8 转换） |

## 3. 证据锚点

| 证据 | 位置 |
|------|------|
| 缺陷位置 | `product/src/cpp/file_transfer.cpp`：Windows 分支用 `CreateFileA`、`_mkdir`、`_stat`、`_findfirst`、`std::ifstream/std::remove` 处理 **UTF-8 路径** |
| 复现（修复前） | `中文名.txt` 上传 → `{"ok":false,"code":"WRITE_FAILED"}`；`硬件报告.txt` 上传「成功」但落盘为 `纭欢鎶ュ憡.txt`（UTF-8 字节被按 GBK 解释的乱码） |
| 根因 | Windows ANSI 文件 API 按**系统代码页**（简中 GBK/936）解释路径：UTF-8 字节若为合法 GBK → 乱码；若非法 GBK → 创建失败 |
| 修复（代码） | `file_transfer.cpp` 新增 `Utf8ToWide`/`WideToUtf8Local`，Windows 分支改用 `GetFileAttributesW`、`_wmkdir`、`CreateFileW`、`FindFirstFileW/FindNextFileW`、`_wfopen`、`_wremove` |
| 修复（下载头） | `product/src/cpp/http_server.cpp`：`Content-Disposition` 由「裸 UTF-8」改为 RFC 6266 —— `filename="<ASCII 回退>"; filename*=UTF-8''<百分号编码>` |
| 修复后实测 | 6 个中文名（含此前失败的 `中文名.txt`）上传/下载全部成功；落盘名与原名一致；`Content-Disposition: attachment; filename="_________.txt"; filename*=UTF-8''%E4%B8%AD%E6%96%87%E5%90%8D.txt` |
| 回归 | ASCII 名、重名换名（中文基名保留）、路径穿越、危险扩展名、TOO_LARGE 均正常；`ctest` 39/39（新增 1 项中文用例） |

## 4. 关键决策

| 决策 | 理由 | 被否决的备选 |
|------|------|--------------|
| 路径转 UTF-16 走 **W 系列 API** | 唯一正确的 Windows 非 ASCII 路径方案 | 设置进程代码页 / `setlocale`（不影响 ANSI API 行为，治标不治本） |
| 目录枚举改用 `FindFirstFileW`（而非 `_wfindfirst`） | 纯 Win32、无 MinGW CRT 命名（`_wfindfirst64i32` 等）歧义 | `_wfindfirst`（跨工具链命名不确定） |
| 读取改用 `_wfopen` + `fread`（弃 `std::ifstream`） | MinGW 的 libstdc++ 不支持 `ifstream` 宽字符路径（MSVC 扩展） | `std::ifstream(wchar_t*)`（非可移植） |
| 顺带修 `Content-Disposition` 为 RFC 5987 | 中文名下载若不编码，浏览器会乱码或拒绝该响应头 | 维持裸 UTF-8（下载文件名乱码） |
| 空目录枚举视为空列表 | `FindFirstFileW` 对空目录报 `ERROR_FILE_NOT_FOUND`，属正常而非错误 | 报错（既有语义，但语义不正确） |
| 新增回归单测 `HandlesNonAsciiFilenamesOnDisk` | 旧实现在 `中文名.txt` 上返回 `WRITE_FAILED`，该用例可捕获 | 不补测试（缺陷会再次回归） |

## 5. 风险与阻塞

- 修复集中在 Windows 分支；POSIX 分支（`stat`/`mkdir`/`opendir`）不变——Linux/macOS 本就以 UTF-8 字节为文件名，无需改动。
- `Utf8ToWide`/`WideToUtf8Local` 与 `system_info_win32.cpp` 的 `WideToUtf8` 存在少量重复；遵循既有「wchar_t 不进公共头、各文件自带转换」的风格，未抽公共头。

## 6. 产出

| 产物 | 路径 |
|------|------|
| 修复 | `product/src/cpp/file_transfer.cpp`（Windows 宽字符 API） |
| 修复 | `product/src/cpp/http_server.cpp`（下载头 RFC 5987） |
| 测试 | `product/tests/file_transfer_test.cpp`（新增 `HandlesNonAsciiFilenamesOnDisk`） |
| 验证 | 本审计 |

## 7. 结论

| 项 | 内容 |
|------|------|
| 角色级裁决 | 完成并移交 |
| 风险 / 影响 | High（修复前中文文件名上传失败/乱码）→ 修复后 Low |
| 备注 | 重编零警告；`ctest` 39/39 |

## 8. 复核记录

- 复核人 | orchestrator
- 复核时间 | 2026-10-12 23:34
- 复核结论 | 通过
- 复核摘要 | 复现修复前 `中文名.txt` → WRITE_FAILED 与 `硬件报告.txt` → 乱码落盘；修复后 6 个中文名上传/下载/列目录/落盘名全部正确；确认 ASCII 与安全边界无回归。

## 9. 移交

| 项 | 内容 |
|------|------|
| 交给 | `test-engineer`（补 HTTP 层集成测试）、`frontend-implementer`（无需改动） |
| 下游依据 | 本审计 + `file_transfer.cpp`/`http_server.cpp` |
| 开工前提 | 非 ASCII 文件名已可正常上传/下载 |
