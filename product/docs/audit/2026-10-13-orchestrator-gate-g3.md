# 审计：orchestrator · 存档查看器 G3 门禁裁决

## 1. 基础信息

| 字段 | 内容 |
|------|------|
| 角色 | `orchestrator` |
| 时间 | 2026-10-13 18:00 |
| 任务 | 裁决 G3 质量门禁（条件已变更，需重新裁决） |
| 门禁 | G3（测试 → 交付） |

## 2. 输入 / 依赖

| 输入 | 路径 / 来源 |
|------|-------------|
| 测试审计 | `2026-10-13-test-engineer-save-inspector.md`（⚠ 有条件完成） |
| 安全审计 | `2026-10-13-security-reviewer-save-inspector.md`（❌ 打回，MED-1）+ `-recheck.md`（✅ 通过） |
| 门禁条件 | `roles.md` §4：G3 = 单测 + 用户场景 E2E 全绿 + `security-reviewer` 无 Critical/High；两者各有审计文档 |

## 3. 门禁条件逐条核对

| 条件 | 是否满足 | 核对依据 |
|------|---------|---------|
| 单测全绿 | ✅ | ctest 36/36 + pytest 21/21（格式）+ 20/20（E2E） |
| 用户场景 E2E 全绿 | ✅ | 20/20，含真实浏览器 DOM 验证与五条主路径 |
| `security-reviewer` 无 Critical/High | ✅ | 首次审计 Critical 0 / High 0；唯一的 Medium（MED-1 数据竞争）已修复并复审通过，**Medium 归零** |
| 两者各有审计文档 | ✅ | test-engineer 与 security-reviewer（含复审）各一份 |

**G3 的四条字面条件全部满足。**

## 4. 抽检记录

抽检 **2 份**（要求至少 1 份），抽 `test-engineer` —— 它是唯一一份**主动标注「我的完成标准未满足」**的审计，最需要验证该标注是否诚实。

### 抽检 A：`test-engineer` 审计（抽 1 份）

| 核对项 | 方法 | 结果 |
|--------|------|------|
| 「结论」与产物是否一致 | 审计 §7 判「⚠ 有条件完成」，并写明「平台相关逻辑三平台验证不满足」；核对测试策略 §6「未覆盖项」首行即为三平台验证，且 `save_file_posix.cpp`/`local_http_posix.cpp` 确实**从未被编译运行**（本机只有 Windows） | ✅ **标注属实**，未把未做的事说成做了 |
| 「关键决策」的理由是否成立 | 抽查「E2E 不依赖子进程 stdout 解析端口」：复核方核对代码确为 `_pick_free_port()` + TCP 轮询；且测试策略 §5.3 记录了因此踩坑（stdout 块缓冲导致 30 分钟挂死） | ✅ 理由成立且有踩坑记录支撑 |
| 审计是否如实登记了**自身测试的缺陷** | 抽查策略 §5.3 四条：① E2E 填占位字节、② 200 状态码混淆成功与失败、③ 顺序耦合、④ 时间戳期望值凭印象。逐条核对代码——`Send(request, body)` 确已改为发真实 body；`test_unparseable_...` 确已改为断言响应体而非状态码；`test_close_per_request...` 确已改用静态资源；`FormatTimestamp` 期望值确已改为独立核算值 | ✅ **全部属实且已修** |

> 值得记录：① 与 ② 两条属于「**验证手段本身在说谎**」——E2E 曾用占位字节当 body、曾把业务失败当成功。这类问题比缺测试更危险，因为它们让 CI 变绿。若 `test-engineer` 只报「E2E 全绿」而不自查，这两条会一直藏着。

### 抽检 B：`security-reviewer` 审计（抽 1 份）

| 核对项 | 方法 | 结果 |
|--------|------|------|
| 隐私红线结论是否可信 | 复核方独立执行 `dumpbin /DEPENDENTS` 与 `/IMPORTS`，确认运行时依赖仅 `WS2_32`+`KERNEL32`+MSVC 运行时，**无 curl/ssl/crypt32/bcrypt** | ✅ 与审计一致 |
| 只读保证结论是否可信 | 复核方独立 grep 全部公开头的 `Write/Truncate/Delete/Remove/Rename/Copy` 与全部 `.cpp` 的 `remove/unlink/DeleteFile/ofstream/fopen`，均为 0 命中；并读 `save_file_win32.cpp:66` 确认 `GENERIC_READ` | ✅ 与审计一致 |
| MED-1 是否真实存在（而非编造） | 复核方核对：修复前 `Impl::inspector` 确为无锁成员，`/api/save/open` 确对其做写、`/api/save/slot` 确对其做读，且工作线程池为 4 个 | ✅ 属实 |
| MED-1 修复是否真实有效 | 复核方核对修复后代码：锁覆盖 `HandleRoute` 全程，收包与发送在锁外，两把锁互不嵌套 | ✅ 属实 |

> 值得记录：`security-reviewer` 主动**加严**了门禁字面标准。按 `roles.md` §3.9「无 Critical/High 才放行」，MED-1（Medium）本可放行；它判定「**触发条件在正常使用下即可满足，后果是未定义行为**」而打回。这个判断是对的——门禁条文是下限，不是上限。

## 5. 关于 `roles.md` §3.8 三平台要求的裁决

`test-engineer` 的完成标准（`roles.md` §3.8）写的是「全量用例绿 + 用户主路径 E2E 通过 + **平台相关逻辑三平台验证**」，而 G3 门禁表（`roles.md` §4）只写「单测 + E2E 全绿 + 无 Critical/High」。**两处口径不一致**：完成标准比门禁条件多了一项。

本裁决：

- **G3 门禁：放行**（按 §4 的字面条件，四条全满足）
- **三平台验证：判定为未完成，不掩盖**，转入 G4 前置条件：
  `product/src/cpp/save_file_posix.cpp` 与 `local_http_posix.cpp` **从未被运行过**。这两份文件包含 Windows 侧不存在的全部逻辑：`open(O_RDONLY)`、`stat`、`select`、`MSG_NOSIGNAL`、`getsockname`、`close`。其中 POSIX `getsockname` 曾是 aimodel 的真实 bug（`(socklen_t*)&addr` 误用），本次虽已修正，但**修正本身未经运行验证**。
- 同时提请注意：这暴露了 `roles.md` 的内部不一致（§3.8 完成标准 > §4 门禁条件）。**该不一致属不变层缺陷，本次不改 `.role-system/`**，记录于此待后续统一。

## 6. 结论

| 项 | 内容 |
|------|------|
| 角色级裁决 | ✅ **G3 门禁放行** |
| 附条件 | 三平台验证未完成，**G4 发布前必须补**（Linux + macOS 各跑一次 ctest + pytest + 一次冒烟） |
| 风险 / 影响 | Medium（三平台未验证） |
| 备注 | 抽检 2 份属实。本次 G3 的实际产出：E2E 抓出 2 个真实 bug（其中「槽位详情页完全打不开」是两个测试都通过、功能却坏掉的经典案例）；`security-reviewer` 打回 1 个真实数据竞争；`test-engineer` 自查发现并修复 4 处「验证手段说谎」的测试缺陷 |

## 7. G3 阶段小结（验证实验观察）

| 观察项 | 结论 |
|--------|------|
| 门禁拦住真问题了吗 | **是，且是三个**：① G2 审查拦下 Python 注解缺失；② G3 E2E 拦下「详情页完全打不开」；③ G3 安全审计拦下跨线程数据竞争 |
| 哪一层价值最高 | **E2E 与安全审计**。单测层（36 例）对这三个问题**全都无能为力** |
| 只读角色的价值 | 高。`security-reviewer` 主动加严门禁条文并打回，是本次唯一「本可放行但选择不放行」的决策 |
| 成本 | G3 阶段：测试策略 1 + E2E 20 例 + 安全审计 2 份（含复审）+ 测试审计 1 + 本放行记录。约 1.5 小时 |
| 框架的短板 | `roles.md` §3.8 与 §4 的口径不一致（完成标准比门禁条件多「三平台验证」），导致「门禁能放行但角色标准未达成」的灰色地带 |

## 8. 移交

| 项 | 内容 |
|------|------|
| 交给 | `doc-engineer`、`release-engineer`（G4） |
| 下游依据 | 契约全文 + 全部审计 + 测试策略 |
| G4 前置条件（**不可跳过**） | ① Linux 跑一次 `scripts/build.sh`（含 ctest + pytest）；② macOS 同上；③ 至少一次真实浏览器冒烟（首屏 + 打开 + 详情 + 导出） |
| 下一门禁 | G4 发布：`doc-engineer` 用户文档完成 + `release-engineer` 安装包/签名检查单全绿 |
