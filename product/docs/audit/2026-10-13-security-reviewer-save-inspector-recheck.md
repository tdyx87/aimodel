# 审计：security-reviewer · MED-1 复审

## 1. 基础信息

| 字段 | 内容 |
|------|------|
| 角色 | `security-reviewer`（只读） |
| 时间 | 2026-10-13 17:50 |
| 任务 | 复审 MED-1（跨线程数据竞争）的修复，并复核 MED-2 的测试闭环 |
| 门禁 | G3（复审轮） |

## 2. 输入 / 依赖

| 输入 | 路径 / 来源 |
|------|-------------|
| 首次审计 | `2026-10-13-security-reviewer-save-inspector.md`（裁决：❌ 打回） |
| 修复 | `src/cpp/local_http.cpp`（`Impl::inspector_mutex` + 路由期持锁）、`include/saveinspector/save_inspector.h`（`Clear()`） |
| 测试 | `tests/e2e/test_user_paths.py::test_concurrent_opens_do_not_corrupt_state` |

## 3. MED-1 修复核验

### 3.1 代码核验（逐项，非采信声明）

| 核对项 | 方法 | 结果 |
|-------|------|------|
| 锁成员存在 | 读 `Impl` 定义 | `mutable std::mutex inspector_mutex;` ✅ |
| 锁覆盖写路径 | 读 `ServeConnection` | 路由调用包在 `std::lock_guard<std::mutex> guard(inspector_mutex);` 内 ✅ |
| 锁覆盖读路径 | 同上 | `/api/save/slot`、`/api/save/export`、`/api/save/slots` 均在同一 `HandleRoute` 调用内 ✅ |
| 锁不覆盖网络 I/O | 同上 | 收包循环与 `net.SendAll` **在锁外**，仅 `HandleRoute` 在锁内 ✅ |
| 锁与队列锁分离 | 读两处 `std::mutex` | `mutex`（队列）与 `inspector_mutex`（业务）分离，**刻意不合并** ✅ |
| `mutable` 必要性 | 编译 | `HandleRoute` 接收 `SaveInspector*`（非 const），但加锁发生在 `Impl` 的非 const 成员函数里；`mutable` 使其在 const 上下文中亦可用，语义正确 ✅ |

### 3.2 潜在死锁核验

| 检查项 | 结果 |
|-------|------|
| 持锁期间是否调用会再次取同一把锁的代码？ | 否。`HandleRoute` 内不碰连接队列 ✅ |
| 持锁期间是否有阻塞等待（网络/条件变量）？ | 否。`HandleRoute` 只做内存解析与 JSON 拼装 ✅ |
| 锁获取顺序是否单一？ | 是。只有一处 `lock_guard(inspector_mutex)`，不存在嵌套获取 ✅ |
| `DrainQueue()`（取队列锁）与业务锁是否交叉？ | 否。`DrainQueue` 只取 `mutex`，`HandleRoute` 只取 `inspector_mutex`，两者无嵌套 ✅ |

> 结论：**无死锁风险**。锁粒度是「单个请求的业务处理」，不跨 I/O。

### 3.3 性能核验

单请求持锁时长 = 解析一份存档（毫秒级）+ JSON 拼装。E2E 实测 21 条用例
（含 4 线程 × 8 次并发读 + 4 线程并发写）总耗时 62s，其中绝大部分是真实浏览器
渲染与进程启动，**未观察到锁引入的可感知延迟**。

## 4. 回归测试有效性核验

| 核对项 | 结果 |
|-------|------|
| 是否新增**并发写**用例 | 是。`test_concurrent_opens_do_not_corrupt_state`：4 线程经 `threading.Barrier` 强制请求重叠，各自上传**不同**存档 ✅ |
| 断言是否真的能暴露该竞态 | 是。三个判据：① 不得返回 5xx（崩溃即读到撕裂状态）；② 响应体必须是**合法 JSON**（撕裂的 `std::string` 会产生非法 JSON）；③ 结束后状态必须是某个**完整**存档（关卡集合 ⊆ 已知集合）✅ |
| 是否修正了原用例的假安全感 | 是。首次审计前的 `test_concurrent_requests_do_not_corrupt_state` 只测并发**读**，覆盖不到该竞态；现已拆为「并发读」+「并发写」两条 ✅ |
| 用例是否确定性 | 是。`threading.Barrier` 强制重叠，不依赖调度运气；样本固定（由 Python 参考实现确定性生成）✅ |

> 诚实边界：这不是对数据竞争的**证明**（那需要 ThreadSanitizer 或压力跑）。
> 它是一个能稳定暴露「撕裂响应/崩溃」的回归哨兵。
> **未在 TSan 下验证**——已如实登记为 `security-reviewer` 审计 §5 的延续缺口。

## 5. MED-2 测试闭环核验

| 核对项 | 结果 |
|-------|------|
| 是否新增「错误不得泄漏本机路径」用例 | 是。`test_error_messages_do_not_leak_local_paths`（HTTP 链路，断言不含 `C:` / `AppData` / `/Users/`）与 `test_cli_error_does_not_leak_path`（CLI 链路，断言不含传入的路径名）✅ |
| 断言是否有意义 | 是。用 `SecretPlayerProfile` 作为探针字符串，若服务端把路径拼进消息即命中 ✅ |
| 诊断三段式是否仍被断言 | 是，HTTP 用例在检查泄漏的同时仍断言 `title/detail/advice` 齐备 ✅ |

## 6. 全量回归

| 层 | 结果 |
|----|------|
| C++ 单测 | 36/36 通过（`local_http_test` 15 + `save_inspector_test` 21） |
| Python 格式用例 | 21/21 通过 |
| 用户场景 E2E | 20/20 通过（含真实浏览器 DOM 验证、并发读、并发写、路径泄漏探针） |
| 编译告警 | 0（`/W4`） |

## 7. 结论

| 项 | 内容 |
|------|------|
| MED-1 | ✅ **已修复并核验** |
| MED-2 | ✅ **测试闭环已补** |
| 风险分级 | Critical 0 / High 0 / **Medium 0** / Low 2 / Info 1 |
| G3 放行裁决 | **✅ 通过** |
| 理由 | MED-1（唯一的 Medium）已修复并有并发写回归用例；其余 LOW/INFO 不构成阻塞（LOW-2 `X-Frame-Options`：本工具无认证无敏感写入，点缀风险接近零；INFO-1 CSP：界面零外部资源，本地工具） |

### 7.1 遗留（如实登记，不阻塞）

| 项 | 说明 |
|----|------|
| 未在 ThreadSanitizer 下验证 | 并发写用例是回归哨兵而非竞态的证明。跨平台 sanitizer 构建成本较高，建议后续版本纳入 |
| Linux / macOS 三平台验证未完成 | `test-engineer` 审计已登记：`save_file_posix.cpp` / `local_http_posix.cpp` 的宽字符、路径、socket 行为在非 Windows 上**从未被运行过** |
| LOW-1 道具映射表归属未定义 | 契约下一版宜明确（内置表 vs 资源文件） |

> 关于三平台：`roles.md` §3.9 要求 G3 放行条件为「无 Critical/High」，本审计按该标准裁决。
> 但 `roles.md` §3.8（test-engineer 完成标准）要求「平台相关逻辑三平台验证」——
> **该项未满足**，已由 orchestrator 在 G3 门禁处单独跟踪，不因本审计的放行而被掩盖。

## 8. 移交

| 项 | 内容 |
|------|------|
| 交给 | `orchestrator`（G3 裁决） |
| 结论 | MED-1/MED-2 已闭合；Medium 归零 |
| 提醒 | 放行**不等于**三平台验证已完成，请勿据本审计宣称跨平台已验证 |
