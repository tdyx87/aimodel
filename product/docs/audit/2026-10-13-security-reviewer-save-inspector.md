# 审计：security-reviewer · 存档查看器安全与隐私审计

## 1. 基础信息

| 字段 | 内容 |
|------|------|
| 角色 | `security-reviewer`（只读） |
| 时间 | 2026-10-13 17:10 |
| 任务 | 安全、隐私红线与并发安全审计 |
| 门禁 | G3 |

## 2. 输入 / 依赖

| 输入 | 路径 / 来源 |
|------|-------------|
| 契约 | `product/docs/design/save-format-contract.md` §4.6（闸门）、§4.7（请求体完整性）、§5（隐私架构） |
| 需求 | PRD NFR-3（数据不出本机）、NFR-5（不误伤用户数据） |
| 上游审计 | `2026-10-13-reviewer-save-inspector.md`（🟡-1 共享 inspector 未加锁） |
| 实现 | `src/cpp/`、`include/saveinspector/`、`web/` |

## 3. 证据锚点

| 证据 | 位置 |
|------|------|
| 闸门实现 | `src/cpp/local_http.cpp:285`（Host）、`:298`（Origin）、`:353`（Content-Type）、`:654-660`（判定顺序） |
| 只读保证 | `src/cpp/save_file_win32.cpp:66`（`GENERIC_READ`）、`save_file_posix.cpp`（`O_RDONLY`） |
| 边界检查 | `src/cpp/save_inspector.cpp` `ByteReader::ReadArrayCount` |
| 产物引用 | 本文件 |

## 4. 风险清单

> 分级 Critical / High / Medium / Low + CWE 编号 + 修复建议。

### Critical / High

**无。**

### Medium

**MED-1｜`Impl::inspector` 跨线程共享且无锁（CWE-362）**
- 位置：`src/cpp/local_http.cpp` `struct LocalHttpServer::Impl` 的 `SaveInspector inspector;`
- 问题：全部 4 个工作线程共享该实例；`/api/save/open` 对其做写（`*inspector = temp` / `inspector->Clear()`），`/api/save/slot` 与 `/api/save/export` 对其做读。**两个并发的 `/api/save/open` 存在数据竞争**：一个线程可能在另一个线程赋值期间读到 `std::string`/`std::vector` 的中间状态 → 未定义行为，最坏情况是读到撕裂的 `std::string` 后崩溃。
- 触发条件：两个 `/api/save/open` 请求真正并发到达。浏览器对回环默认开 6 条连接，**该条件在正常使用下即可满足**（例如用户快速连续打开两个存档）。
- 当前缓解：`Connection: close` 并不构成保护——它约束的是连接生命周期，不保证同一服务实例内的请求串行。**上一轮把它当作「前提被满足」是错的**，本审计纠正该判断。
- 修复建议（二选一）：
  - **方案 A（推荐，改动最小）**：给 `Impl::inspector` 加 `std::mutex`，`HandleRoute` 全程持锁。单个请求的处理是毫秒级，串行化不影响体验。
  - 方案 B：改为 per-connection 会话状态 + 会话 id 传递，改动更大。
- 状态：**这是 G3 放行前必须修复的项**。`test-engineer` 的 `test_concurrent_requests_do_not_corrupt_state` 只并发读、不并发写，**覆盖不到**这条竞态——它当前是假安全感。

**MED-2｜诊断与响应体中不含本机路径，但错误文案由服务端字符串拼装，存在未来回归风险（CWE-209）**
- 位置：`src/cpp/save_file_posix.cpp` 的 `ErrorFor()`、`save_file_win32.cpp` 的 `ErrorFor()`
- 问题：当前文案不含路径（已验证，见 §5）。但这是**靠人工不写进去**保证的，代码里没有任何机制阻止后续把 `path` 拼进消息。
- 修复建议：在 `test_concurrent...` 同级增加一条 E2E 断言——以不存在的路径打开存档，断言响应体不含该路径字符串。当前 E2E 未覆盖此场景。

### Low

**LOW-1｜道具 ID→名称映射为硬编码 5 条（CWE-798 类，硬编码常量）**
- 位置：`src/cpp/save_inspector.cpp` `ItemName()`
- 问题：非安全问题。未知 ID 走「道具 #N」回退，行为正确。仅记录：映射表的规模与维护责任未在契约中定义。
- 建议：契约下一版明确映射来源（内置表 vs 资源文件）。

**LOW-2｜`X-Frame-Options` 未设置**
- 位置：`src/cpp/local_http.cpp` `BuildResponse()`
- 影响：本工具无认证、无敏感写入，点缀（clickjacking）风险接近于零。
- 建议：不修。若将来引入任何用户数据写入能力，需补齐。

### Informational

**INFO-1｜无 CSP 响应头** —— 同 LOW-2，本地工具且界面零外部资源，风险可忽略。

---

## 5. 隐私红线审计（PRD NFR-3）

| 检查项 | 方法 | 结果 |
|-------|------|------|
| 出站网络连接 | `grep` 全部源码的 `getenv/curl/http://|https://|connect(|gethostby|telemetry/analytics` | **0 命中**（唯一 `http://` 出现在 Origin 白名单字符串常量中）✅ |
| 遥测 / 崩溃上报 | 同上 grep + 目视 | 无 ✅ |
| 环境变量读取 | 同上 grep | 无 ✅ |
| 第三方运行时依赖 | `dumpbin /DEPENDENTS save_inspector_ui.exe` | 仅 `WS2_32` + `KERNEL32` + MSVC 运行时；**无 curl / ssl / crypt32 / bcrypt** ✅ |
| 网络类 DLL 导入 | `dumpbin /IMPORTS` | 仅 `WS2_32.dll`（系统套接字，本地回环必需）✅ |
| 响应是否可被跨站页面读取 | `grep Access-Control` | **未返回任何 CORS 头** → 浏览器同源策略生效，跨站页面即便能发请求也读不到响应 ✅ |
| 响应缓存 | `grep Cache-Control` | 恒为 `no-store` ✅ |
| MIME 嗅探 | `grep X-Content-Type-Options` | 恒为 `nosniff` ✅ |

> 结论：**隐私红线成立**。数据不出本机这一 PRD NFR-3 有多路独立证据，不是靠单点声明。

## 6. 只读保证审计（PRD NFR-5）

| 检查项 | 方法 | 结果 |
|-------|------|------|
| 对外 API 是否有写入口 | `grep` 全部公开头的 `Write/Truncate/Delete/Remove/Rename/Copy` | **0 命中** ✅ |
| 源码是否有写盘调用 | `grep` 全部 `.cpp` 的 `remove/unlink/DeleteFile/rename/_wremove/ofstream/fopen` | **0 命中** ✅ |
| 打开标志 | 读 `save_file_win32.cpp:66` | `CreateFileW(..., GENERIC_READ, FILE_SHARE_READ, ...)` ✅ |
| POSIX 侧 | 读 `save_file_posix.cpp` | `open(path, O_RDONLY)`，无任何写标志 ✅ |
| 导出是否写盘 | E2E `test_slot_detail_and_export` | 导出经 HTTP 响应体下发，文件名仅 `save-slot-<n>.<ext>`，无路径 ✅ |

> 结论：**只读保证在代码层成立**，判据是「不存在写入路径」而非「写入被拒绝」——这比运行期检查更强，无法被绕过。

## 7. 输入校验与资源耗尽（CWE-20 / CWE-400 / CWE-125）

| 项 | 位置 | 结果 |
|----|------|------|
| 数组计数先校验后分配 | `ByteReader::ReadArrayCount` | 先 `n > limit` 再 `n * 元素大小 ≤ remaining()`；上限 `kMaxItems=4096` / `kMaxAchievements=8192` ✅ |
| 槽位数上限 | `kMaxSlots=100000` | 超限直接拒绝 ✅ |
| 偏移/长度溢出 | `OpenBytes` 的 `in_range` 判定 | 用 `length <= size - offset` 而非 `offset + length <= size`，**规避了加法溢出** ✅ |
| 尾随字节 | `ParseSlotData` 末尾 `remaining() == 0` | 存在未消费字节即判失败 ✅ |
| 请求体上限 | `kMaxRequestBytes = 512 MB` | 超限回 413，且不缓冲 ✅ |
| 请求头上限 | `kMaxHeaderBytes = 64 KB` | 膨胀即断开 ✅ |
| 文件读取上限 | `ReadFileBytesReadOnly` 默认 512 MB | 超限拒绝并给出用户文案 ✅ |
| 任意字节输入 | 单测 `SurvivesArbitraryTruncationOfValidFile` + `SurvivesRandomByteFlips` | 逐字节截断全长度 + 200 次确定性字节翻转，均不崩溃 ✅ |

> 结论：**资源耗尽防护完整**。伪造 40 亿数组计数的样本经 CLI 实测未崩溃、未巨额分配。

## 8. 跨站与 DNS Rebinding（CWE-346 / CWE-441）

实测（对真实运行的服务发请求，覆盖常见绕过变体）：

### Host 白名单

| Host | 结果 | 判定 |
|------|------|------|
| `evil.example.com` | 403 | ✅ 拦截 |
| `127.0.0.1.evil.com` | 403 | ✅ 拦截（后缀拼接绕过） |
| `127.0.0.1:9999` | 403 | ✅ 拦截（端口不符） |
| `127.0.0.1:18220.evil.com` | 403 | ✅ 拦截 |
| `127.0.0.1:18220` | 通过 | ✅ 正确放行 |

### Origin 白名单

| Origin | 结果 | 判定 |
|--------|------|------|
| `null`（sandbox iframe / `data:`） | 403 | ✅ 拦截 |
| `http://127.0.0.1:18221`（端口不符） | 403 | ✅ 拦截 |
| `https://127.0.0.1:18220`（https 非本服务） | 403 | ✅ 拦截 |
| `http://127.0.0.1.evil.com:18220`（后缀绕过） | 403 | ✅ 拦截 |
| `http://127.0.0.1:18220` / `http://localhost:18220` | 通过 | ✅ 正确放行 |

> 复核说明：首轮探测把「业务失败也返回 200」误读为「闸门放行」，改看响应体后确认判定正确。**这正是为什么闸门验证必须区分「传输成功」与「业务成功」**。

### 跨站写入的残余风险

即便三层闸门全部通过（攻击者构造出合法 Host+Origin），跨站页面能造成的损害上限为：
- **读**：无 CORS 头，浏览器不允许读取响应 → 拿不到存档内容；
- **写**：仅 `/api/save/open` 一个写入端点，作用是「让服务端解析一份字节」，不落盘、不改用户文件。

> 结论：**不存在跨站数据外泄路径**；即使闸门被绕过，损害也不超过「让本地服务做一次无副作用的内存计算」。

## 9. 跨站脚本（XSS）

| 检查项 | 结果 |
|-------|------|
| `innerHTML` / `outerHTML` / `insertAdjacentHTML` | **0 命中**，全站用 `textContent` / `createTextNode` ✅ |
| `eval` / `new Function` / `document.write` | **0 命中** ✅ |
| 外部脚本 / CDN | **0 命中**，界面零外部请求（PRD NFR-3）✅ |
| 存档内容渲染路径 | 槽位名、道具名、诊断文案全部经 `textContent` ✅ |

即便存档文件内的字符串被构造成 HTML，也只会被当作纯文本显示。

## 10. 结论

| 项 | 内容 |
|------|------|
| 风险分级 | Critical 0 / High 0 / Medium 2 / Low 2 / Info 1 |
| G3 放行裁决 | **❌ 打回** |
| 理由 | MED-1 是真实的数据竞争（CWE-362），且**当前测试给出假安全感**（并发用例只测并发读，不测并发写）。按 `roles.md` §3.9「无 Critical/High 才放行」的字面标准可放行，但本审计**主动加严**：MED-1 的触发条件（用户连续打开两个存档）在正常使用下即可满足，且后果是未定义行为。 |

### 10.1 隐私与只读的判定（这两项是本审计的强项，逐条留证）

- **隐私红线成立**：出站 0 命中、无遥测、运行时依赖仅 `WS2_32`、无 CORS 头、`no-store`、`nosniff`。多路独立证据，非单点声明。
- **只读保证成立**：对外 API 无写入口 + 源码无写盘调用 + 打开标志为只读。判据是「不存在写入路径」，比「写入被拒绝」更强。
- **资源耗尽防护完整**：先校验后分配；偏移校验规避加法溢出；任意字节输入不崩溃（200 次翻转 + 全长度截断实测）。
- **跨站攻击无数据外泄路径**：三层闸门实测拦截全部绕过变体；即便绕过，损害上限为「一次无副作用的内存计算」。

## 11. 移交

| 项 | 内容 |
|------|------|
| 交给 | `cpp-implementer`（修 MED-1、补 MED-2 用例）、`orchestrator`（G3 裁决） |
| 修复要求 | **MED-1 必修**：给 `Impl::inspector` 加锁（方案 A），并把并发用例扩为**并发写**（两个同时 open 不同存档，断言不崩溃且状态一致） |
| 可缓 | MED-2 / LOW-1 / LOW-2 / INFO-1 不阻塞 G3，但 MED-2 的 E2E 断言建议一并补 |
| 复审要求 | MED-1 修复后须由 `security-reviewer` 复审，并重跑并发用例 |
