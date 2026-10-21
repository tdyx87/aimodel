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

## 2. 配置（环境变量）

| 变量 | 必填 | 说明 |
|------|------|------|
| `CAGENT_API_BASE` | 是 | OpenAI 兼容服务地址，如 `https://api.example.com/v1` |
| `CAGENT_MODEL` | 是 | 模型名 |
| `CAGENT_API_KEY` | 否 | 鉴权；**不填则不发送 Authorization 头**（本地无鉴权服务可不填） |
| `CAGENT_SYSTEM` | 否 | 系统提示 |
| `CAGENT_ALLOW_SHELL` | 否 | 设为 `1` 才允许模型执行命令；**默认拒绝** |
| `CAGENT_TRANSPORT` | 否 | `curl`（默认）或 `mem`（离线自检，不发任何网络请求） |

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

启动后是一个 `> ` 提示符，直接说你想做什么：

```
cagent —— 低内存终端代理（C99）
输入 /help 查看命令，/exit 退出。
工作区：E:\workspace\demo
会话：C:\Users\me\AppData\Roaming\cagent\sessions
> 把 README 里的 TODO 列表补上两项
```

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

## 5. 测试

```bash
ctest --test-dir build -C Release --output-on-failure   # 全部单测

python scripts/curl_smoke.py   build/Release/cagent.exe # 真实 HTTP 传输
python scripts/session_smoke.py build/Release/cagent.exe # /new /list /resume /delete
python scripts/tool_smoke.py   build/Release/cagent.exe # 批准通道（y 真的写、n 不写）
```

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
