# cpp-implementer — POSIX 分支的三个真问题（CI 首跑抓到的）

| 字段 | 内容 |
|------|------|
| 角色 / 时间 / 任务 | cpp-implementer / 2026-10-21 / 修 Web UI 在 POSIX 上编译不过的问题，并**补上 POSIX 侧的本地校验手段** |
| 结论 | 完成并移交 |

## 1. 输入

- CI 首跑（`302ebc4`）：ubuntu/macOS **编译失败**，Windows 通过。
- 第二次（`288500c`）：Windows 通过，ubuntu/macOS **仍失败**。
- 失败细节：POSIX 分支三处编译错误（见 §3）。

## 2. 关键决策

| 决策 | 理由 | 被否决的备选 |
|------|------|--------------|
| 引入 `scripts/posix_check.sh`（WSL + gcc 一分钟全量语法检查） | 「CI 是唯一的 POSIX 验证手段」这个前提本身就不可靠：CI 用**新 glibc**，老发行版的声明面更窄。**能在本地查的先本地查** | 只等 CI——每次 POSIX 问题都要往返一轮（本次就是两轮） |
| 显式声明 `realpath` | 项目 CMakeLists 已写明立场：「用 POSIX 就得自己声明依赖」。而 `realpath` 正是这条立场的检验：glibc < 2.28 上只定义 `_POSIX_C_SOURCE=200809L` **不够** | 改用 `_XOPEN_SOURCE 700`——会打开一大片声明面，破坏「严格 POSIX」的既定立场 |
| 页面改成**片段数组**（每行一个独立字面量） | 单个字符串字面量 5495 字符 > ISO C99 保证的 4095 → GCC 报 `-Woverlength-strings`，而 CI **把告警当失败**。注意：相邻字面量会被拼接成一个长字面量，所以必须每行独立 | 拆成多段 `<script>` 外部文件——违背「单文件交付」；把 CI 的告警检查放宽——那等于放弃「零告警」门禁 |

## 3. 产出与修复

| 问题 | 根因 | 修复 |
|------|------|------|
| POSIX 分支 `struct ca_net_thread_ctx` **不完整类型** | 该结构体被定义在 Windows 分支里，POSIX 分支又在函数内另定义了一份 → POSIX 的 `ca_net_thread_start` 引用到不完整类型 | 结构体提到文件作用域，两个平台共用 |
| `pthread_cond_cond_broadcast` **函数名拼错** | 少写一个 `cond` | 改为 `pthread_cond_broadcast` |
| `struct timeval` **未声明** | 缺 `<sys/time.h>`（`SO_SNDTIMEO` 要用） | 补头文件 |
| 页面**超长字符串字面量** | 5495 > 4095（C99 保证值） | 改为 110 个独立片段，发送时逐片发出 |
| `realpath` **隐式声明**（额外发现） | glibc < 2.28 上 `realpath` 被 `__USE_XOPEN_EXTENDED` 挡着，只定义 `_POSIX_C_SOURCE=200809L` 不够 → 隐式声明返回 int → **64 位指针截断**（未定义行为） | 显式声明 `char *realpath(const char *restrict, char *restrict);` |

**新增**：`product/scripts/posix_check.sh` —— 在 WSL（Ubuntu 18.04 / glibc 2.27）
上对 `src/*.c`、`tests/*.c` 全量做 `-std=c99 -Wall -Wextra -Wpedantic -fsyntax-only` 检查。

**验证**

- WSL（glibc 2.27）：**ALL CLEAN: no warnings, no errors (POSIX 侧)**。
- Windows（MSVC /W4）：编译零告警；`ctest` 8/8；`web_smoke.py` PASS；`web_tool_smoke.py` PASS。
- 教训已写进 README §5 与脚本头注释。

## 4. 结论

CI 首跑暴露的 POSIX 问题已全部修掉，并**补上了「本地一分钟验 POSIX」的能力**——
这比修掉这几个具体错误更有价值：下一次 POSIX 问题不必等 CI。
完成并移交 G2 复核。

## 5. 移交

- `reviewer`：重点看 ① `realpath` 显式声明与 `<stdlib.h>` 的原型是否完全兼容
  （重复声明合法，但签名必须一致）；② 页面片段化后拼出来的 HTML 是否与原来**逐字节相同**
  （少一个换行就会让页面坏掉）；③ `posix_check.sh` 是否覆盖了所有源文件。
- `test-engineer`：`posix_check.sh` 在 CI 上是否也该跑（WSL runner 不必要，Linux job 直接跑更省）。

## 6. 复核（由独立复核者填写，非自证）

| 复核人 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| reviewer | 2026-10-21 | 通过 | 见 `2026-10-21-reviewer-gate-g2-posix-build.md` |
