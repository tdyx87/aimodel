# cpp-implementer — 本地 Web UI（`--web`）

| 字段 | 内容 |
|------|------|
| 角色 / 时间 / 任务 | cpp-implementer / 2026-10-21 / 实现本地 Web UI：浏览器里聊天、看工具卡片、点按钮批准 |
| 结论 | 完成并移交（`reviewer` 审 G2，`security-reviewer` 审 G3） |

## 1. 输入

- 用户反馈：「给定 webui console 有点难用」→ 选定方案：**本地 Web UI，与控制台共存**，
  Web 模式下默认禁用 run_shell。
- 契约 §7（终端界面）、§8（平台差异）、§5.2 P1/P2/P5、§2.1（预算）；NFR-1（RSS ≤ 100 MB）。
- ADR-0005（本次新增：为什么自建而不引框架）。

## 2. 关键决策

| 决策 | 理由 | 被否决的备选 |
|------|------|--------------|
| **自建极小 HTTP/SSE 服务，不引第三方库** | 项目依赖只有 libcurl；NSIS 那次的教训是「把可选依赖变成阻塞项」拖红整条 CI。框架的请求对象/路由/模板缓存都不可控，内存承诺也没法保证 | libmicrohttpd / civetweb —— 引入新依赖链，Windows/MSVC 工具链问题会重演 |
| **平台差异全部收在 `web_net.c`** | 契约 §8；`web.c` 里没有一处 `#ifdef _WIN32` | 直接在 web.c 里写 winsock 分支 |
| **core 发结构化事件**，Web 直接映射 | 让 Web 层「解析界面文本猜」等于把界面格式变成契约，格式一改就全错 | 从终端文本里正则提取工具名/结果 |
| **浏览器批准 = ui 层的输入源**（`read_byte` 阻塞等决定） | 「终端按 y」与「浏览器点按钮」对上层都是「读一个字符」，core 与工具层完全不需要知道在跟谁打交道 | 给 core 加一条「Web 专用批准路径」——会把两种交互分裂成两套逻辑 |
| **控制台在 Web 模式下只做旁观者**（显示同样的内容，Ctrl-C 退出） | 两个输入源同时可用会让「谁在说话」含糊；控制台的输入是原始模式单键读，与 Web 的多客户端并发混在一起很难做对 | 控制台与浏览器都能发消息 |
| **令牌 = 32 字节系统熵**（Windows `BCryptGenRandom`，POSIX `/dev/urandom`） | 本机任意网页都能打 `127.0.0.1`，没有令牌就等于把代理敞开 | `rand()`/时间种子——可预测，等于没有 |
| **校验 Host 头** | 挡 DNS rebinding：恶意网站把域名解析到 127.0.0.1 再打过来 | 只靠令牌（令牌会从 URL 泄露给同源页面） |
| **连接数上限 64**（每连接一个线程） | 不限就是本机任何进程都能把本进程的内存花光——哪怕它没有令牌 | 不限 |
| **页面内嵌在二进制里** | 单文件交付是项目风格；也避免「路径不对就白屏」 | 独立静态文件（要处理文件从哪来） |

## 3. 产出

| 文件 | 变更 |
|------|------|
| `include/cagent/web.h` | Web UI 对外接口：事件类型、启动/停止、推送事件、用户消息队列、批准槽位 |
| `src/web_net.h` / `src/web_net.c` | 平台层：socket（winsock2 / BSD）、线程、互斥量、条件变量、系统熵 |
| `src/web.c` | HTTP 解析、路由、SSE 广播、事件/消息通道、令牌与 Host 校验、连接数上限 |
| `src/web_page.c` | 内嵌页面（原生 fetch + EventSource，无框架） |
| `include/cagent/core.h` / `src/core.c` | 新增 `ca_core_event` 与可选回调 `cfg.on_event`；在文本增量、工具开始/结束、批准请求、错误、一轮结束处发事件 |
| `src/main.c` | `--web` / `--web-port N`；Web 模式把 ui 的输入源换成 Web（批准走浏览器），主循环从 Web 队列取用户消息 |
| `CMakeLists.txt` | 新增 `cagent_web` 静态库；Windows 链接 `ws2_32` 与 `bcrypt` |
| `scripts/web_smoke.py` | 页面/鉴权/SSE 冒烟（本地，不联网） |
| `scripts/web_tool_smoke.py` | **确定性**的批准→落盘冒烟（本地脚本化 SSE 服务） |
| `docs/adr/ADR-0005-local-web-ui.md` | 为什么自建而不引框架 + 四条安全取舍 |
| `README.md` / `docs/design/contract.md` | Web UI 用法与契约 §15 |

**验证**

- 本地：`ctest` **8/8**；编译**零告警**（MSVC /W4 + GCC -Wall -Wextra -Wpedantic 双平台由 CI 验）。
- `web_smoke.py`：**PASS**（页面 200 / 无令牌 401 / 带令牌 200 / 发送 200 / SSE 收到事件）。
- `web_tool_smoke.py`：**PASS**（批准事件 → 允许 → **文件真的落盘，内容正确**）。
- 真实服务（GLM）：Web 通道的流式文本与错误事件都正确送达页面（`HTTP 429: 该模型当前访问量过大…`）。
  真实服务的工具调用腿因**账户级限流**未能复现（429 code 1113/1305），
  改用本地脚本化服务做确定性验证——不拿 flaky 当门禁。

**过程中修掉的三个真 bug**

1. `sse_send` 用 128 字节缓冲拼「事件名 + JSON」，`snprintf` 截断后返回「需要的长度」，
   代码把它当失败直接 return → **所有稍长的消息被静默丢弃**，页面只看得见最初那条 info。
   改为分段发送。
2. 请求头解析时把第一个 CRLF 换成 `'\0'`，整个头部块被截断到只剩请求行 →
   **带了令牌也 401**。改为单独取一份请求行副本，头部原样保留。
3. `sse_send` 之外，JSON 载荷装不下时原来会截断成半个 JSON（浏览器 `JSON.parse` 直接失败），
   现在显式降级为一条短消息。

## 4. 结论

Web UI 可用：浏览器里聊天、看工具卡片、点按钮批准；控制台仍显示同样内容。
安全四道闸（回环监听 / 随机令牌 / Host 校验 / run_shell 显式开启）已落地并有冒烟守着。
完成并移交 G2/G3。

## 5. 移交

- `reviewer`（G2）：重点看 ① 事件回调是否**会阻塞**（它在代理线程上，必须无阻塞）；
  ② `ca_web_emit` 在持锁时做阻塞写（SSE 客户端不读会拖住代理）——这是已知风险，需评估；
  ③ 令牌/Host 校验是否有绕过路径。
- `security-reviewer`（G3）：本服务能让模型执行命令，是**真实攻击面**：
  重点审 XSS（页面用 textContent）、CSRF/重放、DNS rebinding、连接耗尽、令牌泄露。
- `test-engineer`（G3）：两个冒烟脚本的确定性与跨平台表现（Windows/macOS/Linux）。

## 6. 复核（由独立复核者填写，非自证）

| 复核人 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| reviewer | 2026-10-21 | 通过 | 见 `2026-10-21-reviewer-gate-g2-web-ui.md` |
| security-reviewer | 2026-10-21 | 通过（无 Critical/High） | 见 `2026-10-21-security-reviewer-gate-g3-web-ui.md` |
| test-engineer | 2026-10-21 | 通过 | 见 `2026-10-21-test-engineer-gate-g3-web-ui.md` |
