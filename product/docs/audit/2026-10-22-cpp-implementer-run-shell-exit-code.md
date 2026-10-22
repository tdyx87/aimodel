# 审计：修 run_shell 在 POSIX 上的退出码竞态（失败被报成成功）

| 字段 | 内容 |
|------|------|
| 角色 | `cpp-implementer` |
| 时间 | 2026-10-22 |
| 任务 | 定位并修复 macOS CI 上 `tools_test` 偶发失败的根因：`run_shell` 的退出码竞态 |

| 结论 | 完成并移交 |
|------|------------|

一句话：命令返回 3（失败），代理却报 `exit_code="0"`（成功）——
**失败被说成成功**，是本产品最不能出现的一类错误。

## 1. 输入

- 触发：本次提交的标准工具握手（`ef6671f`）在 **macOS CI 上红了**，
  而 ubuntu / windows 绿。
- CI 注解（不需要鉴权即可读）留下的线索：
  `88% tests passed, 1 tests failed out of 8` + `133 次检查，1 次失败`。
- 定位过程见 §2。

## 2. 关键决策

### D1. 先证明「不是我改坏的」，再动手

第一个怀疑对象当然是本次改动（标准握手）。但有两���**可验证**的事实否掉了它：

1. `git diff 4c1444c..ef6671f -- src/tools*.c include/cagent/tools.h tests/tools_test.c ...`
   **完全为空** —— `tools_test` 的全部输入与上一个绿色提交逐字节相同，
   连 CMakeLists 都没动。
2. `133 次检查`这个数字对上了：`tools_test` 在 Linux 上正好 133 次检查，
   其余用例（core 239 / session 1742 / llm 77 / json 74 / sse 84 / http 23 / mem 37）
   都不是 133。

**结论**：失败的是 `tools_test`，且**与本次改动无关**（输入相同 → 二进制相同 →
只能是环境/时序差异）。于是问题变成「哪个断言在 macOS 上会偶发红」。

### D2. 根因：POSIX 分支里退出状态的作用域 + 一次多余的 waitpid

`src/tools_shell.c` 的 POSIX 分支原本长这样：

```c
for (;;) {
    ...
    int status = 0;                        /* ← 声明在循环体内 */
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) { ...; break; }          /* 路径 A：子进程在这里被回收 */
    ...
}
...
int exit_code = -1;
if (!timed_out) {
    int status = 0;                        /* ← 又一个新变量，初值 0 */
    if (waitpid(pid, &status, WNOHANG) == 0) { /* 已在上方回收 */ }
    exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
```

- **路径 A**（子进程在循环内被 `waitpid` 回收）：`status` 随循环作用域一起消失；
  循环后的 `waitpid` 对**已回收的 pid** 只得到 `ECHILD`（返回 -1），
  那个新的 `status` 保持初值 0 → `exit_code = 0`。
- **路径 B**（管道先 EOF，循环从 `n == 0` 跳出）：子进程尚未被回收，
  循环后的 `waitpid` 正常拿到真实退出码 → `exit_code` 正确。

**所以退出码对不对，取决于「管道 EOF」与「进程退出」谁先被观察到**——
一条纯时序竞态。macOS 上的时序恰好会偶发命中路径 A。

### D2b. 第一次修复**不完整**（如实记录：这是本条的关键教训）

第一版修法是：`status` 提到循环外 + `reaped` 标记 + 循环后 `WNOHANG` 取一次，
取不到就报 `-1`（未知，不猜 0）。

**它没有真正修好。** 第二次 CI（`579fe58`）在 macOS 上又红了，
而且这次**换了失败断言**：

```
[FAIL] run_shell_executes_and_reports_output
  tools_test.c:587: 断言失败: strstr(g_result, "exit_code=\"0\"") != NULL
```

—— 命令**成功**了（输出里有 marker），可退出码**报不出来**。

**为什么**：**管道 EOF ≠ 进程已退出**。有的 shell 会先关掉 fd 1（以及 fd 2），
再走完退出流程（收尾、跑 trap）。于是「EOF」与「进程真的死了」之间有一个窗口，
这个窗口里 `waitpid(WNOHANG)` 只会回答「还没退出」。
macOS 的 `/bin/sh` 是 bash，这个窗口尤其容易命中。

- 第一版修法在这个窗口里报 `-1`（**退出码报不出来**）；
- 最初版本在这个窗口里报 `0`（**失败被说成成功**）。

**同一个窗口，两种症状。** 第一版只是把「谎报成功」换成了「不报」，
并没有让退出码变对。

**最终修法**：管道 EOF 之后，**有界地等**子进程真正退出（最多 200 ms，
每次 1 ms）再取退出码。正常情况下进程在微秒级退出，立刻拿到真实退出码；
万一它真的赖着不走，就如实报「未知」(-1)，不猜。

> 教训：**「不猜」不等于「对」**。把一个错的值换成一个诚实的未知值，
> 确实比谎报好，但缺陷还在——而且症状换了样，更难认出来。
> 修 bug 要问的是「正确的值是什么、怎么拿到它」，不是「怎么让它别错」。

### D3. 让这两条路径**确定性**可测，而不是靠运气

新用例 `run_shell_reports_exit_code_reliably` 不用 `exit 3`（它退出得太快，
总是走路径 B），而是让**后台进程继承 stdout**：

```
sh -c 'sleep 1 & exit 3'      # 后台 sleep 持有管道写端 → 管道不会先 EOF
cmd /c 'ping -n 2 127.0.0.1 & exit 3'
```

这样 poll 必然先超时、子进程必然在循环内被回收 → **必然命中路径 A**。

新用例 `run_shell_reports_exit_code_after_stdout_closed_early` 则精确制造
D2b 的窗口：

```
sh -c 'exec 1>&- 2>&-; sleep 0.05; exit 3'   # 写端立刻关闭（EOF），进程 50 ms 后才死
```

**同一个测试文件在 Linux 上就能复现两个缺陷，不必等 macOS 偶发。**

> 已实测（A/B，同一份测试代码）：
>
> | 实现 | 对 `exit 3` 报什么 | 用例结果 |
> |------|------------------|---------|
> | 最初版本（status 在循环体内 + 多余 waitpid） | `exit_code="0"` | **失败（失败被说成成功）** |
> | 第一版修复（status 提到循环外 + 单次 WNOHANG） | `exit_code="-1"` | 失败（退出码报不出来） |
> | 最终版本（有界等待进程真正退出） | `exit_code="3"` | **通过** |

### D4. 顺带改进 CI 的诊断通道（否则下次还得猜）

原 step 13 只 `tail -12 buildsh.log`。ctest 把每个用例的输出打在该用例自己的
位置，日志末尾只剩最后一个用例的尾巴 —— 于是「哪个用例、哪条断言失败」
全都不在日志末尾里。macOS 那次失败只留下「133 次检查，1 次失败」这种
**没有指向性**的数字。

改为：从日志里 grep **纯 ASCII 的机器可读标记**
（`The following tests FAILED` 与 `[FAIL]`），把失败用例名与失败断言
作为注解发出来。不按中文 grep —— Windows 上非 ASCII 会被输出成 `?`
（这个坑本工作流已经吃过两次）。

## 3. 产出

| 文件 | 改动 |
|------|------|
| `src/tools_shell.c` | POSIX 分支：`status` 提到循环外 + `reaped` 标记；**管道 EOF 之后有界等待子进程真正退出**（≤200 ms）再取退出码；已回收的不再 waitpid |
| `tests/tools_test.c` | 新增 `run_shell_reports_exit_code_reliably`（命中「循环内被回收」那条路径）与 `run_shell_reports_exit_code_after_stdout_closed_early`（命中「EOF 先于退出」那条路径）；均已登记到用例表 |
| `.github/workflows/cagent.yml` | step 13 的失败注解改为 grep `The following tests FAILED` / `[FAIL]`，失败用例与断言直接可见；单元测试步骤与 build.sh 步骤区分「真实缺陷」与「runner 抖动」，只对后者重试 |

> **CI 诊断的改动直接决定了本条能不能查清**：第一次 macOS 失败只留下
> 「133 次检查，1 次失败」这种没有指向性的数字；加了 `[FAIL]` 注解之后，
> 第二次失败立刻指出了「哪个用例、哪条断言、哪个文件第几行」，
> 于是 D2b 那个「第一版修复不完整」的问题当天就查清了。

## 4. 术语与实现边界（如实记录）

- **Windows 分支未改**：它用 `GetExitCodeProcess` 取退出码，不存在双重 waitpid
  的问题。平台差异仍然只出现在传输/平台层（契约 §8）。
- **未修的相邻问题**（如实记录，不在本轮范围）：POSIX 分支在
  「子进程已退出、但后台进程仍持有管道写端」时会**在收尾的读循环里阻塞**
  （等后台进程结束）。本用例里它阻塞约 1 秒（`sleep 1`），
  属于「工具的硬超时管不到收尾阶段」。修它需要改收尾读循环的策略，
  与本次退出码问题是两件事，留作独立条目。
- **CI 结论**：macOS 那次失败与本次「标准工具握手」改动**无关**
  （见 D1 的字节级证据），但它确实被那次提交带进 CI 才暴露出来。

## 5. 复核（由独立复核者填写，非自证）

| 复核人 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| （orchestrator / `reviewer`） | | 待复核 | 核对根因分析是否成立、修复是否覆盖两条路径、CI 诊断改动是否有效 |

## 6. 验证记录（本会话实测）

| 项 | 命令 | 结果 |
|----|------|------|
| Windows 构建 + ctest | `scripts/build.ps1 -Config Release -BuildDir build-curl ...` | 8/8 通过，**零告警** |
| POSIX 语法 | `wsl -d Ubuntu-18.04 -- bash scripts/posix_check.sh` | `ALL CLEAN` |
| Linux 全量测试 | WSL + gcc 直接编译（WSL 的 CMake 3.10 < 项目要求的 3.15） | 修复前：新用例**确定性失败**；修复后：全部通过（见 D3 的 A/B 表） |
| 单测计数（Windows） | 各 `*_test.exe` | core 239 / session 1735 / llm 77 / tools 132 / ui 63 / json 74 / sse 84 / http 25 / mem 37，**全 0 失败** |
| 内存 | `mem_load.py --turns 500` | 峰值 8.0x MB（预算 100 MB） |
| Web 冒烟 | `web_smoke.py` / `web_tool_smoke.py` | 均 PASS |
