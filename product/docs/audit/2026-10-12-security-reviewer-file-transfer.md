# 角色审计记录

> 生成路径：`product/docs/audit/2026-10-12-security-reviewer-file-transfer.md`  
> 对应规范：`.role-system/agents.md` §6.6（只读角色的审查结论即其审计）
> 审查清单：`.role-system/agents/security-reviewer.md`（0 隐私红线 / 1 内存 / 2 输入校验 / 4 敏感数据与路径穿越）

---

## 1. 基础信息

| 字段 | 内容 |
|------|------|
| 角色 | security-reviewer |
| 时间 | 2026-10-12 21:30 |
| 任务 | 审计 `file_transfer` 的隐私红线、内存安全、输入校验、路径穿越防护与敏感数据处理 |
| 门禁 | G3 质量 |

## 2. 输入 / 依赖

| 输入 | 路径 / 来源 |
|------|-------------|
| 被审代码 | `product/src/cpp/file_transfer.cpp`、`product/include/aimodel/file_transfer.h` |
| 被审测试 | `product/tests/file_transfer_test.cpp`（14 用例） |
| 上游审计 | `2026-10-12-cpp-implementer-file-transfer-impl.md`（声明已修 1 项 High：尾随空格绕过） |
| 上游审计 | `2026-10-12-reviewer-file-transfer-cpp.md`（移交本角色复核安全类） |
| 契约 | `product/docs/design/file-transfer-contract.md` §6.2 路径校验、§6.3 文件名约束、§6.4 大小上限 |
| PRD 关注项 | `PRD-file-transfer.md:107`「需要 security-reviewer 复核路径穿越逻辑」 |

## 3. 证据锚点

| 证据 | 位置 |
|------|------|
| 隐私红线（清单 0） | `grep -nE 'socket\|send\|connect\|http\|curl\|getenv\|std::cout\|std::cerr\|printf' file_transfer.cpp` → 仅命中 `:295` 的 `snprintf`（时间戳格式化，非日志）。**零出站、零遥测、零明文日志** |
| 内存安全（清单 1） | 全文件无裸 `new/delete`；文件流 `std::ifstream`/`std::ofstream` 与 `std::string` 均由 RAII 覆盖；`opendir`/`_findfirst` 句柄在成功路径 `closedir`/`_findclose`，失败路径提前 return 前未打开（无泄漏） |
| 输入校验（清单 2） | 三处公开接口均有校验链：`AppendUpload`（`:382` 大小、`:389` 扩展名、`:398` 路径结构、`:405` 点/空白、`:424` 清洗后复校、`:440` 白名单纵深）、`ReadUpload`（`:552`/`:559`/`:568`）、`RemoveUpload`（`:604`/`:613`） |
| 路径穿越（清单 4，CWE-22） | `ContainsPathStructure`（`:171`）拒绝含分隔符或 `..` 的名字；`IsAllowedPath`（`:312`）拒绝绝对路径（`/` 前缀、盘符 `X:`、UNC `//`）并要求目标严格位于 root 段内；`MakeSafeFilename` 剥离分隔符与非法字符作纵深 |
| 危险扩展名（CWE-434） | `IsDangerousExtension`（`:185`）25 类可执行/脚本扩展名；比对前经 `ExtensionOf` → `TrimWhitespace` 归一化 |
| 大小上限（DoS 防护） | `:38` `kMaxFileSize = 10 MB`，校验先于任何磁盘写（`:382` 在 `:466` ofstream 之前） |
| 测试实证 | `ctest` 24/24；`RejectsPathTraversal`、`RejectsAbsolutePathNames`、`TrailingSpaceExtensionBypassIsRejected`、`EnforcesSizeLimitAtTenMegabytes` 均通过 |

## 4. 关键决策

| 决策 | 理由 | 被否决的备选 |
|------|------|--------------|
| 判定 **无 Critical / 无 High**，G3 可放行 | 清单 0/1/2/4 逐项实测通过；上游声明的 High（尾随空格绕过危险扩展名）已被修复且我独立复现验证——`a.exe `、`b.bat ` 现均返回 false | 仅凭上游审计「已修」就直接采信 —— 违反「非自证」原则，故自己复现了一遍 |
| 尾随空格绕过定为已修的 **High**（CWE-178 大小写/空白规范化不当） | Windows 落盘会剥掉文件名首尾空格与点，`a.exe ` 实际成为 `a.exe`（可执行文件）；原实现对扩展名比对未 trim，实测被绕过 | 定为 Medium —— 该漏洞直接产出可执行文件，属高危，只是当前用户为本地单人工具、无远程攻击面，故未到 Critical |
| TOCTOU 竞态定为 **Medium**（CWE-367），不阻断 G3 | `GetUniqueFilename` 用 `stat` 判空位与 `ofstream` 打开之间存在窗口，并发同名上传可互相覆盖。**当前 HTTP 层未接线、无并发入口，实际不可触发** | 定为 High —— 缺乏可达攻击路径（无并发入口、仅 127.0.0.1）；定为 Low —— 接线 HTTP 层后该窗口会真实存在，低估了后续风险 |
| 危险扩展名黑名单定为已实现的缓解，非根因修复 | 黑名单是纵深防御，理论上仍有未枚举的可执行类型；但对本地单人工具，风险可接受 | 要求改为白名单（只允许特定扩展名）—— 会过度限制产品用途，属产品决策而非安全必需 |

## 5. 风险与阻塞

- 🔴 Critical：**无**
- 🔴 High（阻断 G3）：**无**（原 1 项已修并复现验证）
- 🟡 Medium（不阻断，须在 HTTP 接线时处置）：
  - **CWE-367 TOCTOU**：`AppendUpload` 判空名与写盘之间有竞态窗口。`http_server` 接入并发请求后会真实存在。**处置要求**：改 `open(O_CREAT\|O_EXCL)` 原子占名，或在请求层为上传加锁。接线前不得宣称该功能并发安全。
  - **危险扩展名黑名单不完备**：25 类为常见集合，未覆盖全部可执行类型。本地单人工具下可接受；若产品面向多用户或开放网络，应改为白名单。
- 🟢 Low（记录备查）：
  - `ListUploads` 返回上传目录内的文件名清单。若该接口经 HTTP 暴露，等价于目录列举信息泄露（其他用户上传的文件名可见）。当前为本地单人工具，且契约未要求该接口对外暴露。**建议 HTTP 层不要暴露 `ListUploads`**。

## 6. 产出

| 产物 | 路径 |
|------|------|
| 安全审查结论（即本审计） | `product/docs/audit/2026-10-12-security-reviewer-file-transfer.md` |

## 7. 结论

| 项 | 内容 |
|------|------|
| 角色级裁决 | **✅ 通过**（无 Critical / 无 High） |
| 风险 / 影响 | Medium（库层本身安全；遗留 2 项 Medium 均与「HTTP 层尚未接线」这一现状绑定，须在接线时处置） |
| 备注 | 隐私红线实测干净（零出站、零遥测、零明文日志）；内存安全全 RAII；路径穿越（CWE-22）、危险上传（CWE-434）、DoS 大小上限均有校验且有单测实证。**注：本结论仅覆盖库层。HTTP 层接入后须复审**——尤其 TOCTOU（CWE-367）与 `ListUploads` 是否对外暴露。 |

## 8. 复核记录

- 复核人 | orchestrator
- 复核时间 | 2026-10-12 21:35
- 复核结论 | 通过
- 复核摘要 | 抽检 security-reviewer 的两项核心判定：(1) **「隐私红线干净」可复现**——实测 `grep -nE 'socket|send|connect|http|curl|getenv|std::cout|std::cerr|printf' file_transfer.cpp` 仅命中 `:295` 的 `snprintf`（时间戳格式化），确为零出站、零遥测、零明文日志。(2) **「无 Critical/High」成立且未轻信上游**——security-reviewer 声明「已修 High 并独立复现验证」，实测 `ctest` 中 `TrailingSpaceExtensionBypassIsRejected`、`RejectsPathTraversal`、`RejectsAbsolutePathNames`、`EnforcesSizeLimitAtTenMegabytes` 四个用例全部 Passed，确证修复有效；三处公开接口的校验链（`AppendUpload`/`ReadUpload`/`RemoveUpload`）经 grep 确认均含 `ContainsPathStructure` + `IsAllowedPath` 双重防护。TOCTOU 判 Medium 的理由（HTTP 层未接线、无并发入口）经核实成立。复核通过，G3 安全项放行；§5 的 2 项 Medium 已登记为 HTTP 接线前置条件。

## 9. 移交

| 项 | 内容 |
|------|------|
| 交给 | `test-engineer`（G3 剩余项：三平台复核 + 用户场景 E2E）；后续 HTTP 接线角色（须先处置 §5 的 2 项 Medium） |
| 下游依据 | 本审计 §5 风险清单；`security-reviewer.md` 边界「G3 无 Critical/High 才放行」 |
| 开工前提 | `test-engineer` 需在 Linux/macOS 实测 `opendir`/`mkdir`/`stat` 分支；HTTP 接线角色须先消除 TOCTOU 并决定 `ListUploads` 是否暴露 |
