# 审计：reviewer · 存档查看器实现审查（G2 合入门禁）

## 1. 基础信息

| 字段 | 内容 |
|------|------|
| 角色 | `reviewer`（只读） |
| 时间 | 2026-10-13 15:40 |
| 任务 | 按 `roles.md` §3.7 分工种清单审查三工种实现产物 |
| 门禁 | G2 |

## 2. 输入 / 依赖

| 输入 | 路径 / 来源 |
|------|-------------|
| 契约 | `product/docs/design/save-format-contract.md` |
| 实现审计 ×3 | `2026-10-13-{cpp,python,frontend}-implementer-save-inspector.md` |
| 审查清单 | `roles.md` §3.7（C++ / Python / 前端三份清单） |

## 3. 证据锚点

| 证据 | 位置 |
|------|------|
| 复核命令 | 见 §5 逐项「复核方法」列 |
| 产物引用 | 本文件 §4 各条 |

## 4. 审查结论：分级问题清单

> 格式依 `roles.md` §3.7：🔴阻塞 / 🟡建议 / 🟢可选，每条给 `文件:行号` + 问题 + 依据 + 建议。

### 🔴 阻塞项

**无。**

### 🟡 建议项

**🟡-1｜共享 inspector 未加锁，依赖「Connection: close」这一隐含前提**
- 位置：`src/cpp/local_http.cpp` `struct LocalHttpServer::Impl` 的 `SaveInspector inspector;`
- 问题：该实例由全部 4 个工作线程共享，且 `HandleRoute` 对它做读-改-写（`/api/save/open` 里 `*inspector = temp`）。当前协议响应恒带 `Connection: close`，浏览器对回环只开有限连接，实测无竞态；但一旦放宽为 keep-alive 或提高并发，即成数据竞争（未定义行为，可能读到半个 SlotDetail）。
- 依据：契约 §4「`Connection: close`」为当前唯一约束；`roles.md` §3.3「跨平台、内存安全」。
- 建议：本期不改（改协议牵扯契约），但**必须**在契约 §4 旁注明「`inspector` 无锁共享，以 close-per-request 为前提」；若将来改 keep-alive，须先加锁或改为 per-connection 状态。已列入 G3 移交项。
- 状态：实现方已在 `cpp-implementer` 审计 §5 主动登记，非隐瞒。

**🟡-2｜`SaveInspector::OpenFile` 已实现但 HTTP 层未使用**
- 位置：`src/cpp/save_inspector.cpp` `OpenFile()` vs `local_http.cpp` `/api/save/open` 只调 `OpenBytes`
- 问题：契约 §3.2 定义了 `OpenFile`，实现也在，但产品链路（浏览器上传字节）用不到它。这不是缺陷——浏览器出于安全不能把本机路径交给网页，`OpenFile` 服务的是 CLI 与可能的将来直读场景——但**属于未被产品使用的对外 API**，构成未来维护面。
- 依据：契约 §3.2；`roles.md` §3.3「核心模块」。
- 建议：在头文件注明 `OpenFile` 面向 CLI/嵌入式调用，HTTP 链路因浏览器同源策略不适用。🟢级，不阻塞。

### 🟢 可选项

**🟢-1｜`SlotToJson` 的输出重复了 `SaveToJson` 内层的槽位序列化**
- 位置：`src/cpp/save_inspector.cpp` `SaveToJson()` 循环内调 `SlotToJson()`
- 问题：无实际问题，仅记录当前实现是「复用」而非「重复」。
- 结论：无需改动。

### 🟢-2｜空槽位判定依赖前端启发式**
- 位置：`web/app.js` `slotTone()`
- 问题：见 `frontend-implementer` 审计 §5。已如实登记，非隐瞒。
- 结论：本期接受。

## 5. 复核方法与结果（不采信审计声明，独立执行）

| # | 审查项 | 复核方法 | 结果 |
|---|--------|---------|------|
| 1 | C++11 合规 | `grep` 扫描 `std::optional`/`make_unique`/`string_view`/`filesystem`/`if constexpr`/泛型 lambda/`[[nodiscard]]` | 仅命中一条**注释**中的说明文字，实际代码 0 命中 ✅ |
| 2 | 头文件无 `using namespace` | `grep` 扫描 `include/saveinspector/*.h` | 0 命中 ✅ |
| 3 | 平台差异编译期隔离 | 列 `src/cpp/` 实际文件 vs `CMakeLists.txt` 的 `target_sources` | 6 个文件逐一对应，无遗漏、无悬空引用 ✅ |
| 4 | 统一导出宏 | 检查 `include/saveinspector/export.h` + CMake 的 `SAVEINSPECTOR_BUILD`/`SAVEINSPECTOR_SHARED` | 与 aimodel 同构，PUBLIC 传递正确 ✅ |
| 5 | 内存安全：边界检查顺序 | 读 `ByteReader::ReadArrayCount` | 先 `ReadU32` → 校验 `n > limit` → 校验 `n * 元素大小 ≤ remaining()` → 才允许上层 `reserve`。顺序正确 ✅ |
| 6 | 只读保证（PRD NFR-5） | `grep` 对外头文件找 `Write/Truncate/Delete/Remove/Rename/Copy`；`grep` 源码找写盘调用 | 对外 API **无任何写盘入口**；全源码唯一 `CreateFileW` 在 `save_file_win32.cpp:66`，标志为 `GENERIC_READ` ✅ |
| 7 | 响应体不含本机路径（契约 §5） | 检查 `local_http.cpp` 中全部 `JsonEscape(...)` 入参 | 仅用户语言文案 / 时长 / 时间戳，无 `path`/`static_root` ✅ |
| 8 | 前端 XSS 面 | `grep` `innerHTML/outerHTML/eval/document.write` | 0 命中（全站 `textContent`/`createTextNode`）✅ |
| 9 | 前端零外部请求 | `grep` `http://`/`https://`/`cdn` | 0 命中（同源相对路径）✅ |
| 10 | 导出禁用 `window.location` | `grep` `window.location` | 仅 2 处**注释**在解释为什么禁止，代码 0 命中 ✅ |
| 11 | Python 类型注解 | **用 Python `ast` 模块遍历**全部函数与参数 | 首次发现 3 处缺失（`test_version_is_encoded_in_header` 的 2 个参数、`test_slot_table_offsets_point_at_real_data` 的 1 个参数）→ **已由实现方修复**，复跑 AST 校验 0 缺失 ✅ |
| 12 | Python 依赖锁定 | 检查 `pyproject.toml` | `pytest==7.4.4` 精确版本；运行时零第三方依赖 ✅ |
| 13 | 独立执行测试 | 自行运行 `ctest` 与 `pytest`，非引用审计声明 | ctest 36/36 绿、pytest 21/21 绿 ✅ |
| 14 | 跨语言格式一致 | 用 CLI 解析 Python 生成的 11 个样本 | 正常 v3 输出正确；v1 正确标注升级；校验和损坏正确定位「槽位 2」；40 亿数组计数未崩溃 ✅ |

> 复核方法 11 值得记录：先用 PowerShell 逐个 `def` 扫 6 行判断注解，得到 17 处「缺失」的**误报**；改用 Python `ast` 后得到真实结果（3 处）。**扫描脚本本身不可靠时不能据此下结论**——这与 aimodel 中「测试辅助函数写错却因断言太弱而假绿」是同一类问题：验证手段本身要有验证。

## 6. 结论

| 项 | 内容 |
|------|------|
| 角色级裁决 | **✅ 通过** |
| 阻塞项 | 0（🟡/🟢 不构成阻塞） |
| 依据 | `roles.md` §3.7：阻塞项清零才可 ✅；本轮阻塞项为 0 |
| 备注 | 审查过程中发现并已修复 1 项真实缺陷（Python 类型注解缺失 3 处），另有 1 项已知设计边界（共享 inspector 无锁）已由实现方主动登记并移交 G3。审查未出现「仅凭审计声明放行」的情形：14 项复核全部独立执行，其中 2 项（类型注解、跨语言一致性）由复核方主动发现问题。 |

## 7. 移交

| 项 | 内容 |
|------|------|
| 交给 | `orchestrator`（G2 门禁裁决）、`test-engineer`（G3）、`security-reviewer`（G3） |
| 下游依据 | 本审查结论 + 契约 + 三份实现审计 |
| 开工前提 | G2 需 orchestrator 完成抽检后方可放行；G3 须处理 🟡-1（共享 inspector 前提复核） |
