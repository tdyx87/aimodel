# 审计：cpp-implementer — 修复 MSVC 下编译失败

> 示例审计文档。规范见 `.role-system/agents.md` §6.6。

---

- **角色**：cpp-implementer
- **时间**：2026-10-12
- **任务**：修复 Release 构建在 MSVC（VS2017）下无法编译的问题

## 输入
- 上游：`product/` 源码（`src/cpp/`、`include/aimodel/`、`tools/`）与 `product/CMakeLists.txt`
- 约束：`agents.md` 工程规范（跨平台编译期宏隔离、对外 UTF-8、C++11）、G3 零警告

## 关键决策
- **根因判定为源码编码与工具链不匹配**：14 个源文件均为 UTF-8 无 BOM 且含中文，MSVC 默认按系统 ANSI 代码页（GBK/936）解码，GBK 双字节错位配对吞掉后续字节、破坏 `#if/#endif` 结构，报 `C1020 意外的 #endif`。**以最小复现实证**（`cl /c export.h` 失败 vs `cl /utf-8` 通过），而非推测。
- **选 `/utf-8` 而非加 BOM**：编译成 exe 实测四种方案的**执行字符集**，仅加 BOM 会把中文字面量编成 GBK，导致 HTTP 响应（声明 `charset=utf-8`）在浏览器乱码；`/utf-8` 才同时固定源码与执行字符集。否决"只加 BOM"（虽能修复编译但引入乱码）。
- **中文移出 printf 格式串**：MSVC 格式串检查器遇非 ASCII 转码失败并误判占位符数量（`C4474`）。改 `fputs` / 先拼串再输出，从根上消除而非逐个抑制警告。
- **顺带修真 bug**：`CollectDisks` 原先无条件 `push_back`，`GetDiskFreeSpaceExW` 失败时仍产出 `total_bytes=0` 条目，违反测试不变量 → 改为失败即跳过该卷。

## 产出
- `product/CMakeLists.txt`：MSVC 加 `/utf-8`；警告选项抽为 `AIMODEL_WARN_FLAGS` 并覆盖全部 4 个目标（原 tools/tests 无警告选项）
- `product/tools/system_info_ui.cpp`：中文移出格式串；修 `printf` 多余实参
- `product/src/cpp/http_server_win32.cpp`：`select` 首参加 `static_cast<int>` 消除 `C4244`
- `product/src/cpp/system_info_win32.cpp`：磁盘容量查询失败时 `continue`
- 提交：`f1d3f49`、`89f3d58`

## 结论
完成并移交。构建**零警告零错误**；`ctest` 10/10 通过；端到端冒烟验证 HTTP 中文为合法 UTF-8 且无乱码、路径穿越被 404 拦截。

## 移交
交 `reviewer`（C++ 清单，G2）与 `test-engineer`（G3）。测试已由 cpp-implementer 附带的 `ctest` 初步验证，`test-engineer` 应据此做用户场景 E2E 复核。

## 复核
由 `orchestrator` 填写——审计非自证，须独立复核：
- 复核人 / 时间：orchestrator / 2026-10-12
- 结果：`✅ 通过`——抽检「产出」与门禁产物一致（`ctest` 10/10、零警告），「关键决策」理由成立（`/utf-8` vs 加 BOM 的实测对比可复现）。
