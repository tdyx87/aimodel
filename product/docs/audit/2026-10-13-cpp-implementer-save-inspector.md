# 审计：cpp-implementer · 存档查看器核心库与本地服务

## 1. 基础信息

| 字段 | 内容 |
|------|------|
| 角色 | `cpp-implementer` |
| 时间 | 2026-10-13 14:30 |
| 任务 | 实现核心解析库、只读文件访问、本地 HTTP 服务、CLI 与入口 |
| 门禁 | G2 |

## 2. 输入 / 依赖

| 输入 | 路径 / 来源 |
|------|-------------|
| 契约 | `product/docs/design/save-format-contract.md`（含 §2.1.1 CRC 参数、§4 协议、§5 隐私、§7 已知坑） |
| 契约审计 | `2026-10-13-architect-save-format.md`（复核结论：通过） |
| 上游依赖 | `roles.md` §3.3 硬约束；`agents.md` §7.1 C++11 禁止清单、§7.3 跨平台细则 |

## 3. 证据锚点

| 证据 | 位置 |
|------|------|
| 核心解析 | `src/cpp/save_inspector.cpp`（`ByteReader` 边界检查、`ParseSlotData` 版本迁移） |
| CRC64 | `src/cpp/save_inspector.cpp` `Crc64Ecma182`，测试向量断言见 `tests/save_inspector_test.cpp` |
| 只读保证 | `src/cpp/save_file.h`（仅声明 `ReadFileBytesReadOnly`）、`save_file_win32.cpp:66` `GENERIC_READ` |
| 闸门 | `src/cpp/local_http.cpp` `IsAllowedHostHeader` / `IsAllowedOriginHeader` / `MediaTypeOf` |
| 产物引用 | 本文件 |

## 4. 关键决策

| 决策 | 理由 | 被否决的备选 |
|------|------|--------------|
| 引入 `ByteReader` 作为唯一字节访问入口 | 存档是**不可信输入**。让所有读取都过一次边界检查，比在每个调用点手写 if 更难漏——漏一处就是一个越界读 | 裸指针 + 手工 offset 运算（易漏，且漏点不可见） |
| `ReadArrayCount` 先按「剩余字节 ÷ 元素大小」校验，再允许分配 | 伪造 `items_count=0xFFFFFFFF` 会让实现尝试分配 32 GB。契约 §3.3 要求不得崩溃，资源耗尽同样是 DoS | 先分配后校验（攻击面） |
| 校验和不符时**仍尝试解析** | PRD US-3 要求「部分损坏仍显示能读出的部分」。若校验失败即拒绝解析，用户会丢掉本来能看的道具数据 | 校验失败即丢弃整个槽位（违背 US-3） |
| 解析失败返回 `false` 而非抛异常 | 契约 §3.3「任意字节不得抛未捕获异常」。C++11 无 `optional`，`bool + 输出参数` 是可控方案 | 抛异常 + 全链路 catch（漏一个 catch 就崩） |
| `impl_->inspector` 为每个 `LocalHttpServer` 一份，非每连接一份 | 界面是「打开一次 → 反复查槽位」的单会话模型；每连接一份会让第二次请求拿不到状态 | 每连接一份（需要 session id 传递，复杂度不划算） |
| 严格按契约 §2.1.1 实现 CRC-64/ECMA-182 | 契约明确：十余种 CRC64 变体互不兼容，必须钉死。已按契约补的测试向量实现 | 选实现更快的查表法（变体一致的前提下可用，但须先证明与 MSB 先行等价——本期不做） |
| 时间格式化用 civil_from_days 算法，不用 `localtime` | `localtime` 依赖本地时区与平台行为，跨平台/跨时区不可靠；自实现无平台差异 | `gmtime`（依赖 CRT 实现差异）/ `localtime`（跨时区不一致） |

## 5. 风险与阻塞

- **⚠ 实现过程中踩到并已修复的编译问题（记录以防复发）**：
  1. **中文进 `snprintf` 格式串触发 C4819**（即使加了 `/utf-8`）。实测定位：中文单独作为 `const char*` 赋值不触发，作为 `snprintf` 的格式串参数触发，且会连带误判占位符数量（C4474）。已改为「`std::string` 拼接 + `fputs`」，并在 `tools/save_inspector_cli.cpp` 与 `save_inspector.cpp` 留下约定注释。
  2. **`body` 变量遮蔽形参**（C4457 家族）。`HandleRoute` 的 `body` 形参被静态资源分支的局部 `body` 覆盖，已改名 `content`。这与 aimodel 曾修过的是同一类问题。
  3. **测试辅助函数算错槽位偏移**：最初用「循环中已写入的表长」计算数据区起点，导致第一个槽位的偏移落在槽位表内部。生产代码无误，**是测试写错了**。已在 `MakeSave` 中改为「头 + 完整表长」并加注释说明原因。
- **`impl_->inspector` 是所有工作线程共享的单一实例，未加锁**。当前成立的前提是：HTTP 响应严格 `Connection: close`，浏览器对回环只开少量连接，实测无竞态。但**这是一个已知的边界**：若将来放宽为 keep-alive 或提高并发，会引入数据竞争。当前不改，因为契约 §4 明确 `Connection: close`，且改动会牵扯协议。**移交 G3 时须由 `security-reviewer` 与 `test-engineer` 复核这一前提是否被满足。**
- **未实现压缩存档解压**：契约 §2.3 明确本期只识别并提示，已实现 `flags` bit0 的 warning 分支。符合范围。
- 物品 ID→名称映射表是硬编码的 5 条（`ItemName`）。契约 §3.2 要求映射归核心库，已满足；但样本外的 ID 走「道具 #N」回退，属已知限制。

## 6. 产出

| 产物 | 路径 |
|------|------|
| 对外头 | `include/saveinspector/{save_inspector,local_http,export}.h` |
| 核心实现 | `src/cpp/save_inspector.cpp` |
| 只读文件访问 | `src/cpp/save_file.h`、`save_file_win32.cpp`、`save_file_posix.cpp` |
| 本地服务 | `src/cpp/local_http.cpp`、`local_http_impl.h`、`local_http_win32.cpp`、`local_http_posix.cpp` |
| 工具 | `tools/save_inspector_cli.cpp`、`tools/save_inspector_ui.cpp` |
| 构建 | `CMakeLists.txt` |
| 单测 | `tests/save_inspector_test.cpp`（21 例）、`tests/local_http_test.cpp`（15 例） |

## 7. 结论

| 项 | 内容 |
|------|------|
| 角色级裁决 | ✅ 完成并移交 |
| 风险 / 影响 | Medium（共享 inspector 的未加锁前提，见 §5） |
| 备注 | 自检：`roles.md` §3.3 硬约束逐条核验——① C++11 合规：无 `optional`/`make_unique`/泛型 lambda/初始化捕获（`bool + T` 替代 `optional`）；② 平台差异隔离为 `save_file_win32.cpp` / `save_file_posix.cpp` / `local_http_*`，由 CMake `target_sources` 选择；③ 对外 UTF-8；④ 失败给用户可读错误，不崩溃（有「随机字节翻转 200 次 + 逐字节截断全长度」两条用例实证）。构建零告警（`/W4`），36/36 ctest 全绿。 |

## 8. 复核记录

| 项 | 内容 |
|------|------|
| 复核人 | `reviewer`（已出具，见 `2026-10-13-reviewer-save-inspector-cpp.md`） |
| 复核结论 | ✅ 通过（1 项 🟡 建议、0 项 🔴 阻塞） |
| 复核摘要 | 按 `roles.md` §3.7 的 C++ 清单逐条核验：抽查 `ByteReader::ReadArrayCount` 的「先校验后分配」顺序、`CreateFileW` 的 `GENERIC_READ` 标志、以及闸门三分支的判定逻辑，均与契约一致。复核独立运行了 `ctest`，未仅凭审计声明放行。 |

## 9. 移交

| 项 | 内容 |
|------|------|
| 交给 | `reviewer`（G2）、`test-engineer`（G3）、`security-reviewer`（G3） |
| 下游依据 | 契约 §2/§3/§4/§5 |
| 开工前提 | G3 须复核 §5 中「共享 inspector 未加锁」的前提是否成立（Connection: close + 有限并发） |
