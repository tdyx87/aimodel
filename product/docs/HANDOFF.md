# HANDOFF — cagent 终端/网页代理（2026-10-22）

> 位于**变层**（product/）：内容是具体产品的，不能放到不变层（根目录 AGENTS.md / .role-system/）。

> 交接文档。新会话先读这一份，再按需读 `product/docs/design/contract.md`。
> 状态：**功能可用**，CI 三平台绿（`7e03e36`），工作树干净。
>
> **2026-10-22 更新**：完成「标准工具握手」（ADR-0006），
> 遗留清单里优先级最高的一项已消；顺带修掉 `run_shell` 的退出码竞态。
> 详见 §5 第 8、9 条。

---

## 1. 仓库与分支

| 项 | 值 |
|----|-----|
| 仓库 | `E:\workspace\aimodel`，远端 `https://github.com/tdyx87/aimodel.git` |
| 分支 | `feat/c-agent`（head = `7e03e36`，已推送） |
| 不变层 | `.role-system/`（角色系统）——**本轮及上轮均零改动** |
| 变层 | `product/`（本产品的源码、文档、审计、ADR） |

**分层纪律**（`AGENTS.md`）：换产品只换 `product/`；`.role-system/` 里不得出现领域名词。
本项目的定位是「角色系统本身的第一个用例」。

---

## 2. 当前能力（都已验证）

| 能力 | 形态 | 验证方式 |
|------|------|----------|
| 终端交互 | `cagent` | `ctest` 8/8 + 三个冒烟 + 真实服务 |
| **本地 Web UI** | `cagent --web` | `web_smoke.py` + `web_tool_smoke.py`（已进 CI） |
| 诊断 | `cagent --probe` | 打印配置 + 一次最小真实请求 |
| 离线自检 | `CAGENT_TRANSPORT=mem` | 不联网，走完整链路 |
| 六个工具 | read/write/edit/list_dir/search/run_shell | 真实服务已验证 `write_file` 全链路 |
| 会话持久化 | `/new` `/list` `/resume` `/delete` | `session_smoke.py` + 真实服务 |
| **标准工具握手** | 自动（`role:"tool"` + `tool_call_id`） | `core_test` 断言**请求体**形状；ADR-0006 |

**Web UI 关键事实**

- 只监听 `127.0.0.1`；URL 带 32 字节系统熵令牌（`?t=`）；校验 `Host` 头挡 DNS rebinding。
- Web 模式下 `run_shell` **仍需** `CAGENT_ALLOW_SHELL=1`。
- 浏览器批准走 ui 层输入源（`read_byte`），core/工具层不知道在跟谁打交道。
- 决策记录：`product/docs/adr/ADR-0005-local-web-ui.md`；契约：`contract.md` §15。

---

## 3. 关键设计约束（改动时必须遵守）

| 约束 | 内容 |
|------|------|
| 语言/依赖 | C99；依赖只有 libcurl（+ Windows 的 `ws2_32`/`bcrypt`）。**不引 Web 框架**（ADR-0005） |
| 内存 | RSS ≤ 100 MB；所有缓冲定长；`mem_load.py --turns 500` → 峰值 7.71 MB |
| 依赖方向 | `core → {llm, tools}`；**llm 不得依赖 tools**（工具 Schema 由 tools 层提供） |
| 平台差异 | 只能出现在传输层/平台层（`web_net.c`、`ui_term.c`）；业务逻辑不得有 `#ifdef _WIN32` |
| 零告警 | CI **把告警当失败**。MSVC 格式串含中文会误报 C4474 → 格式串纯 ASCII、中文作参数 |
| 密钥 | 绝不落盘/不进会话文件/不打印内容（只说「已设置/未设置」） |
| 权限 | P1 默认拒绝；P2 批准前展示完整内容；P4 路径沙箱；P5 命令原文 |
| 拒绝语义 | 必须区分**用户拒绝**与**策略拒绝**（`CA_PERM_DENY_POLICY`）；策略拒绝**不询问**用户 |
| 截断 | 必须显式标注，不许静默 |
| POSIX | glibc < 2.28 的声明面更窄 → 用 POSIX 就要**自己声明依赖**（`realpath` 是样板） |

---

## 4. 环境与工具链（踩过的坑都在这）

| 项 | 值 |
|----|-----|
| 构建（Windows） | `.\scripts\build.ps1 -Config Release -BuildDir build-curl -ExtraArgs @('-G','Visual Studio 15 2017','-A','x64','-DCMAKE_PREFIX_PATH=C:\Users\Administrator\AppData\Local\Temp\curl-inst')` |
| **POSIX 本地检查** | `wsl -d Ubuntu-18.04 -- bash scripts/posix_check.sh`（**一分钟出结果，优先用它**） |
| 内存实测 | `python scripts/mem_load.py --exe build-curl\Release\cagent.exe --turns 500` |
| 真实服务自检 | `python scripts/real_api_smoke.py --exe build-curl\Release\cagent.exe --tries 12 --sleep 10` |
| Web 冒烟 | `python scripts/web_smoke.py build-curl\Release\cagent.exe`、`python scripts/web_tool_smoke.py build-curl\Release\cagent.exe` |
| 等 CI | `python C:\Users\ADMINI~1\AppData\Local\Temp\opencode\wait_ci.py <sha> 1800` |
| 真实服务 | `CAGENT_API_BASE=https://open.bigmodel.cn/api/paas/v4`，`CAGENT_MODEL=GLM-4.7-FLASH` |

**工具链陷阱**

- `-DCMAKE_BUILD_TYPE` 对 VS 生成器无效，必须 `-G "Visual Studio 15 2017" -A x64`。
- MSVC 含中文 `printf` 格式串 → **C4474 误报** → 格式串纯 ASCII、中文作参数。
- `python -c "…中文…"` 经 PowerShell 会按 GBK 转码破坏 → 含中文一律先 `write` 落盘 .py 再执行。
- PowerShell here-string 会插值 → 改用 .py 脚本。
- `CA_TEST(x)` 必须登记到测试文件底部表，否则**静默不跑**。
- **给跨层共享的结构体加字段时，每一个填充它的地方都要同步改**。
  漏改的后果是「未初始化指针被当有效值解引用」→ 段错误，
  编译器不会报，历史路径的单测才撞得上（本轮 `ca_session_export` 就中过）。
  新字段的默认值要写成**可验证的不变量**（测试钉住），而不是「碰巧是 0」。
- **macOS arm64 runner 会偶发失败**（runner 自己提示「容量受限」）。
  判别方法：看失败的提交**改了什么**——本轮有一次**只改了 markdown** 的提交
  （`8a332e0`）在 macOS 的单元测试步骤红了，且**一条注解都没有**
  （步骤没走到诊断语句就死了）→ 文档文件不可能让用例失败，那是基础设施抖动。
  CI 现在会区分：日志里**有**失败用例 → 真实缺陷（可能间歇），**不重试**；
  没有失败用例、只是非零退出 → 疑似 runner 抖动，**重试一次**。
  刻意不把「有失败用例」也重试：间歇性缺陷正是要抓的东西
  （本轮的 `run_shell` 退出码竞态就是间歇的），一律重试会把它们盖掉。
- **CI 失败诊断怎么读**（不需要鉴权）：`python <tmp>/ci_annot2.py <check-run-id>`
  或直接看 GitHub 的注解页。工作流已改成 grep 纯 ASCII 标记
  （`The following tests FAILED` / `[FAIL]`），失败用例名与失败断言会直接进注解。
- 相对路径 + `Popen(cwd=…)` 在 POSIX 下找不到程序（Windows 隐形）→ 先 `resolve()`。
- 临时目录必须带进程号，否则并发测试互相删工作区。
- `git push` 需重试循环（失败 sleep 8s）。

---

## 5. 本轮（会话内）做过的事

按时间顺序，每项都有审计文档（`product/docs/audit/2026-10-21-*.md`）：

| # | 主题 | 关键修复 |
|---|------|----------|
| 1 | 真实服务可用性 | **请求体没有 `tools` 字段**（真实模型永远不会调用工具，脚本化冒烟伪造 tool_calls 造成假绿）；`header_cb` 把响应头长度当状态码；4xx 错误体被丢弃 |
| 2 | 控制台/失败文案 | 失败分类（连不上/TLS/超时/4xx）；Windows 控制台 UTF-8 + 不关 `ENABLE_PROCESSED_INPUT`（否则 Ctrl-C 失效→强杀→终端无回显）；`--probe` 诊断命令 |
| 3 | 面板/中文路径 | 面板**承载内容**（此前是空盒子+框外文字）；Windows 宽字符 API（`FindFirstFileW`/`_wfopen`）解决中文文件名乱码与读写失败 |
| 4 | 策略拒绝 | 不再问注定被拒的问题；区分 `denied` 与 `denied_by_policy`；ui 拆出 `ca_ui_show_pending`/`ca_ui_ask` |
| 5 | search 语义 | 二进制文件跳过必须**说明跳过多少个**（否则模型以为目录里没源码） |
| 6 | **Web UI** | 全新能力：HTTP/SSE + 结构化事件 + 内嵌页面 + 四道安全闸 + 三个真 bug（SSE 截断丢事件、头部截断导致带令牌也 401、fd 复用泄露） |
| 7 | POSIX 构建 | `ca_net_thread_ctx` 不完整类型、`pthread_cond_cond_broadcast` 拼错、`struct timeval` 缺头文件、页面超长字面量（→110 片段）、**`realpath` 隐式声明**（glibc<2.28 指针截断） |
| 8 | **标准工具握手** | 工具结果从「尾随 user 消息」改为 `role:"tool"` + `tool_call_id`，并回带 assistant 的 `tool_calls`（根治模型重复请求同一工具）；顺手删掉 `llm.h` 里 `ca_llm_build_request_full` 的重复声明；踩到三个**真 bug**：结构体加字段而 `ca_session_export` 没同步 → 未初始化指针被解引用 → 段错误；回带的 `tool_calls` 拼成对象而非数组（服务端 400）；回灌上限与请求体容量脱节 → 结果填满缓冲必然装不下 → 整轮失败 |
| 9 | **run_shell 退出码竞态** | macOS CI 偶发红，根因是 POSIX 分支里退出状态声明在循环体内 + 一次多余的 waitpid → **命令失败却报 `exit_code="0"`**（失败被说成成功）；**修了两轮**（第一轮不完整：只把「谎报」换成「不报」，症状还换了个样）；最终改为「管道 EOF 之后有界等待子进程真正退出」；两个确定性用例在 Linux 上就能复现两条路径 |

**验证基线（当前）**：`ctest` 8/8；`core_test` 239 检查 / `session_test` 1735 / `llm_test` 77 /
`tools_test` 132 / `ui_test` 63 / `json_test` 74 / `sse_test` 84 / `http_test` 25 / `mem_test` 37 —— 全 0 失败；
`mem_load --turns 500` 峰值 8.0x MB；`posix_check.sh` ALL CLEAN；两个 Web 冒烟 PASS。

---

## 6. 已知遗留（按优先级）

| 优先 | 项 | 说明 |
|------|-----|------|
| ~~高~~ | ~~工具结果**标准握手**~~ | **已做**（2026-10-22，ADR-0006）：`role:"tool"` + `tool_call_id`，并回带 assistant 的 `tool_calls`；见 `product/docs/audit/2026-10-22-cpp-implementer-tool-handshake.md` |
| 中 | `search` 正则 | 现仅字面匹配（契约 §5.1 原文含正则） |
| 中 | US-8 非 OpenAI 兼容格式 | 未做 |
| 中 | 性能实测 | US-1 <500 ms / US-2 首字 <2 s 无数据 |
| 中 | ASAN/UBSAN 构建 | 无 |
| 中 | POSIX `run_shell` 收尾读循环可能阻塞 | 子进程已退出、但**后台进程仍持有管道写端**时，收尾的读循环会等后台进程结束；工具的硬超时管不到收尾阶段（本轮修退出码竞态时顺带发现，未修） |
| 低 | Web 令牌过期/一次性化 | 当前进程内固定，重启即换（security Low 项） |
| 低 | SSE 发送并行化 | 现在串行；32 个坏客户端最坏拖住代理约 64 s |
| 低 | Web 模式 `/list` `/resume` | 只支持 `/new` `/exit` |
| 低 | 页面从内嵌字符串拆成独立文件 | 改 UI 需重新编译 |

---

## 7. 下一步建议（挑一个做，别摊大饼）

1. **`search` 正则**（契约 §5.1 原文含正则，兑现成本低）
2. **US-8 非 OpenAI 兼容适配层**（`ca_transport_ops` 已是接口，加一个后端即可；
   标准握手已把请求侧形状收敛到 `ca_llm_msg`，映射起来不难）
3. **ASAN/UBSAN CI 任务**（Linux 一条 job 即可，能兜住内存类问题；
   本轮刚踩过一个「结构体加字段没同步 → 未初始化指针」的段错误，正需要它）
4. **性能实测**（US-1/US-2，把「未验证」变成「已验证」）
5. **真机复验标准握手**（真实服务账户限流，本轮只验证了请求体形状，没连真机）

---

## 8. 新会话的起步动作

```powershell
cd E:\workspace\aimodel
git log --oneline -3
git status                      # 期望：干净
cd product
# ① 先跑本地 POSIX 检查（最快的信号）
wsl -d Ubuntu-18.04 -- bash scripts/posix_check.sh
# ② 再跑 Windows 构建 + 测试
.\scripts\build.ps1 -Config Release -BuildDir build-curl -ExtraArgs @(
  '-G','Visual Studio 15 2017','-A','x64',
  '-DCMAKE_PREFIX_PATH=C:\Users\Administrator\AppData\Local\Temp\curl-inst')
# ③ 想连真实服务
$env:CAGENT_API_BASE="https://open.bigmodel.cn/api/paas/v4"
$env:CAGENT_MODEL="GLM-4.7-FLASH"
$env:CAGENT_API_KEY="<你的 key>"
.\build-curl\Release\cagent.exe --probe
.\build-curl\Release\cagent.exe --web
```

> **密钥提醒**：本会话的 API key 曾在对话中明文出现，建议轮换；仓库内已核对无任何 key 片段。
> 真实服务当前对本账户有**限流**（429 code 1113/1305），做工具类验证请优先用本地脚本化服务
> （`web_tool_smoke.py` 那种），别拿 flaky 当门禁。
