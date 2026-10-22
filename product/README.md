# cagent

一个**低内存终端 AI 代理**：C99、单进程、文本界面，用一组受限工具（读/写/改文件、列目录、搜索、执行命令）替你在工作区内干活。
内存预算 **≤ 100 MB**（含 libcurl），空闲实测约 6–7 MB。

> 版本 **0.1.0**。环境：Windows / Linux / macOS，C99 编译器 + CMake ≥ 3.10。
> 传输层依赖 **libcurl**（可选：缺失时降级为内存后端，只能离线自检）。

## 1. 构建

**Windows（PowerShell）**

```powershell
cd product
./scripts/build.ps1 -Config Release
```

**Linux / macOS**

```bash
cd product
./scripts/build.sh Release
```

两者都是「配置 + 构建 + ctest」。需要指定 libcurl 位置或生成器时，把参数透传给 CMake：

```powershell
./scripts/build.ps1 -Config Release -BuildDir build `
  -ExtraArgs @('-G','Visual Studio 15 2017','-A','x64',
               '-DCMAKE_PREFIX_PATH=C:\path\to\curl')
```

```bash
./scripts/build.sh Release -DCMAKE_PREFIX_PATH=/opt/curl
```

只想要测试构建、确认所有层可用（不联网）：

```bash
cmake -S . -B build && cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

POSIX 侧本地校验（WSL 也可，一分钟出结果）：

```bash
bash scripts/posix_check.sh
```

> **为什么必须有这一条**：CI 的 ubuntu/macos 用新 glibc，
> 老发行版（Ubuntu 18.04 / RHEL 7，glibc < 2.28）的头文件声明面更窄。
> 实测 `realpath` 在 glibc 2.27 上即使定义了 `_POSIX_C_SOURCE=200809L`
> 也不被声明 → 隐式声明返回 int → 64 位指针截断（未定义行为）。
> CI 全绿不等于老机器上安全。

## 2. 配置（环境变量）

| 变量 | 必填 | 说明 |
|------|------|------|
| `CAGENT_API_BASE` | 是 | OpenAI 兼容服务地址，如 `https://api.example.com/v1` |
| `CAGENT_MODEL` | 是 | 模型名 |
| `CAGENT_API_KEY` | 否 | 鉴权；**不填则不发送 Authorization 头**（本地无鉴权服务可不填） |
| `CAGENT_SYSTEM` | 否 | 系统提示 |
| `CAGENT_ALLOW_SHELL` | 否 | 设为 `1` 才允许模型执行命令；**默认拒绝** |
| `CAGENT_TRANSPORT` | 否 | `curl`（默认）或 `mem`（离线自检，不发任何网络请求） |
| `CAGENT_CONTEXT_BYTES` | 否 | 单次请求体上限，默认 `262144`（256 KB），范围 16 KB–4 MB；越界自动钳制 |

单次请求体超限时不会直接失败：先按完整轮次淘汰最旧历史并重试，界面上会说明丢弃了多少条消息；
只有**一轮对话自己**就超过上限时才报错，提示用 `/new` 开新会话。长会话不会因此中断。

密钥只存在于进程内存：**不打印、不落盘、不进会话文件**（PRD NFR-3）。

PowerShell 示例：

```powershell
$env:CAGENT_API_BASE="https://api.example.com/v1"
$env:CAGENT_MODEL="your-model"
$env:CAGENT_API_KEY="sk-..."
.\build\Release\cagent.exe
```

`cagent --version` / `cagent --help` 不需要任何环境变量。

## 3. 使用

启动后是一个 `> ` 提示符，直接说你想做什么。终端里会显示一个**带颜色的启动面板**：

```
cagent —— 低内存终端代理（C99）
上下文上限  256 KB
工作区      E:\workspace\demo
会话        C:\Users\me\AppData\Roaming\cagent\sessions
后端/模型   libcurl  ·  模型 GLM-4.7-FLASH
输入 /help 查看命令，/exit 退出。
> 把 README 里的 TODO 列表补上两项
```

（Windows Terminal / PowerShell 里带颜色；重定向到文件时自动退回纯文本，
不会出现转义乱码。）

### 3.0 界面什么都不显示 / 报「请求失败」？

先跑一次诊断，把事实打出来：

```powershell
.\cagent.exe --probe
```

```
cagent 诊断（--probe）
  后端：libcurl
  地址：https://api.example.com/v1
  模型：your-model
  密钥：已设置（不显示内容）
  控制台：TTY（启用颜色与边框）
  HTTP 状态：429
  传输结果：服务端返回错误状态码
  响应片段：{"error":{"code":"1305","message":"该模型当前访问量过大"}}
  结论：请求未成功，见上面的传输结果与响应片段
```

对照这几行就能定位：

| 现象 | 原因与处理 |
|------|------------|
| 「无法连接到服务端（目标：…）」 | 地址/网络/代理问题；检查 `CAGENT_API_BASE` 与网络 |
| 「TLS 握手或证书验证失败」 | 证书链或代理拦截；试试 `curl -v <地址>` 对比 |
| 「HTTP 401: …」 | 密钥没设、过期或没权限 |
| 「HTTP 429: 该模型当前访问量过大」 | 服务端限流，等一会儿重试，或换模型 |
| 「HTTP 404 / 400」 | 地址路径或模型名不对（模型名要和 `/models` 里的一致） |
| 中文显示成乱码 / 界面全空 | 旧版本未设控制台 UTF-8；重新构建后即解决（见 §4 的版本说明） |
| 敲键盘没有回显 | 旧版本进原始模式后被强杀，终端被留在无回显状态；新版本 Ctrl-C 可正常退出；当前终端执行 `reset` 即可恢复 |


### 3.0.5 Web UI（浏览器里聊）

终端交互有门槛：单键批准、长输出刷屏、历史难翻。
`--web` 会在本机起一个**只监听 127.0.0.1** 的小服务，浏览器里就是一个聊天界面：

```powershell
.\cagent.exe --web
# 控制台会打印：
#   浏览器打开: http://127.0.0.1:53211/?t=<令牌>
```

浏览器里能看到聊天记录、工具卡片（调用了什么、结果是什么），
**破坏性操作会出现「允许 / 拒绝」两个按钮**——比在终端里按 y 顺手。

```powershell
.\cagent.exe --web --web-port 8080    # 指定端口（默认由系统分配）
```

安全上有四道闸（详见 `docs/adr/ADR-0005-local-web-ui.md`）：

| 闸 | 说明 |
|----|------|
| 只监听 127.0.0.1 | 绝不暴露到局域网 |
| 随机令牌 | URL 里的 `?t=` 是访问凭证；无令牌的请求一律 401 |
| 校验 Host 头 | 挡住 DNS rebinding（恶意网站把域名指到 127.0.0.1） |
| run_shell 仍需显式开启 | Web 模式下也要 `CAGENT_ALLOW_SHELL=1` |

**令牌不要转发给别人**——拿到它的人可以在你的工作区里驱动这个代理。

### 3.1 批准：动文件与执行命令前一定会问

写文件、改文件、执行命令都是**默认拒绝**的。模型要求这些操作时，界面会先把**具体内容**
摆出来，再问一个字符：

```
将写入文件：notes/todo.md
内容 37 字节：

- 买牛奶

--- write_file 请求。允许执行？[y/N] y
--- 
<result status="ok" path="notes/todo.md" bytes="37">已写入。</result>
```

- 只有 `y` / `Y` 才执行。回车、其它键、EOF 一律视为拒绝（**没有默认值**）。
- 拒绝不会中断对话：模型会收到「已拒绝」，可以改个做法继续。
- 执行命令（`run_shell`）额外要求 `CAGENT_ALLOW_SHELL=1`；即使你按了 `y`，
  没有这个开关也会被策略拒绝并说明原因。设置后，界面会**完整展示命令原文**。
- 文件内容超过 2048 字节时只展示开头，并显式标注总长度（不会悄悄截断）。

### 3.2 工作区边界

工具只能读写**启动时当前目录**内部。`..`、绝对路径越界、指向外部的符号链接一律拒绝，
且拒绝时不泄露外部目录结构。启动横幅会显示工作区，先看它再批准写入。

### 3.3 命令

| 命令 | 作用 |
|------|------|
| `/help` | 命令表与批准说明 |
| `/mem` | 当前/峰值 RSS、传输后端、历史占用 |
| `/new` | 开始新会话（只清内存，不删磁盘旧会话） |
| `/list` | 列出历史会话（新到旧） |
| `/resume <id>` | 继续指定会话 |
| `/delete <id>` | 删除指定会话 |
| `/exit`（或 Ctrl-C / EOF） | 保存并退出 |

### 3.4 会话与隐私

- 位置：Windows `%APPDATA%\cagent\sessions`；Linux/macOS `$XDG_CONFIG_HOME/cagent/sessions`
  （缺省 `~/.config/cagent/sessions`）。
- 文件是 **JSON 明文**（包含对话内容）。POSIX 上权限为 `0600`；Windows 依赖用户目录 ACL。
- **不包含 API key**（有用例守着）；但仍然不要在工作区里放敏感信息再让模型读。
- `/new` 不清磁盘；要删就用 `/delete <id>`。
- 目录不可用时降级为「仅内存」并在启动时明说，不会假装能恢复。

### 3.5 管道与非 TTY

非 TTY 下自动降级：无横幅、无等待提示、**无任何 ANSI 转义**，输出可直接被脚本消费。

```bash
echo "总结这个目录" | ./cagent
```

（批准询问仍然读一个字符：管道里给 `y` 就是同意，给 `n` 或直接 EOF 就是拒绝。）

## 4. 已验证与未验证（如实记录）

**已在 CI 三平台（Ubuntu / macOS / Windows）验证**

- 全部分层单测：`core` / `http` / `json` / `llm` / `sse` / `tools` / `ui` / `session`（含 POSIX 权限用例）
- 零告警（`-Werror` 等）
- 真实 libcurl 请求（本地 SSE 服务）+ 会话命令冒烟 + 批准通道冒烟（真实二进制）
- 空闲 RSS 基线（Windows ~6.7 MB / Linux ~5.9 MB）

**未验证（不要当成已完成）**

- 真实第三方 HTTPS 服务（无凭据）；已验的是本地服务与 CI 上的本地 SSE
- 活跃/超长会话的 RSS（只有空闲基线）
- 首字延迟 < 2 s、单轮 < 500 ms（无实测数据）
- Linux/macOS 手工交互（只有 CI 自动化）
- 非 OpenAI 兼容格式的服务（US-8 未做）
- 无 ASAN/UBSAN 构建；越界靠不变量断言覆盖
- `search` 仅字面匹配，**不支持正则**
- 工具结果以**尾随 user 消息**回灌（未采用 `role:"tool"` + `tool_call_id` 标准握手），
  因此模型偶尔会重复请求同一个工具；已附「工具已执行」提示缓解，但未根治
- Web UI 只做了本机单用户场景（无多用户、无 HTTPS、无鉴权体系）；
  浏览器端的「真人观感」只能人工确认（CI 没有交互式浏览器）

## 5. 测试

```bash
ctest --test-dir build -C Release --output-on-failure   # 全部单测

python scripts/curl_smoke.py   build/Release/cagent.exe # 真实 HTTP 传输
python scripts/session_smoke.py build/Release/cagent.exe # /new /list /resume /delete
python scripts/tool_smoke.py   build/Release/cagent.exe # 批准通道（y 真的写、n 不写）
```

### 5.1 真实服务自检（需要凭据，可选）

仓库里的冒烟都跑在**本地假服务**上——它们能证明协议解析与批准通道正确，
但证明不了真实服务会给我们想要的东西。真实故障正是这么暴露的
（请求体里没有 `tools` 字段，真实模型只会聊天，而伪造 `tool_calls` 的测试全绿）。

```bash
export CAGENT_API_BASE=https://api.example.com/v1
export CAGENT_MODEL=your-model
export CAGENT_API_KEY=sk-...
python scripts/real_api_smoke.py --exe build/Release/cagent.exe
```

验三件事：真实 TLS + 流式对话、多轮记忆、一次**工具调用走完批准并真的落盘**。
凭据只从环境变量读，脚本不打印、不落盘；服务端限流（429）会自动重试。

## 6. 目录

```
product/
  include/cagent/   公共头（core / llm / sse / http / tools / ui / session / mem）
  src/              实现（传输层唯一平台差异所在）
  tests/            单测（CTest 注册）
  scripts/          构建脚本与冒烟脚本
  design/           界面稿
  docs/prd|design|audit|adr/  需求、契约、审计、决策记录
```
