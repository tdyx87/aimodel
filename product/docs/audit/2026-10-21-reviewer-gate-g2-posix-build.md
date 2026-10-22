# reviewer — G2 合入评审（POSIX 构建修复 + 本地 POSIX 校验）

| 字段 | 内容 |
|------|------|
| 角色 / 时间 / 任务 | reviewer / 2026-10-21 / 评审「POSIX 编译修复 + realpath 显式声明 + 页面片段化 + posix_check.sh」 |
| 结论 | ✅ 通过（R1–R8） |

## 1. 输入

- 实现审计：`docs/audit/2026-10-21-cpp-implementer-posix-build.md`
- CMakeLists 的既有立场：「关掉编译器扩展后 `-std=c99` 是真的，用 POSIX 就得自己声明依赖」

## 2. 审查清单

| # | 检查项 | 结论 | 依据 |
|---|--------|------|------|
| R1 | `realpath` 显式声明是否与 `<stdlib.h>` 的原型**兼容** | 通过 | 声明为 `char *realpath(const char *restrict, char *restrict);`，与 glibc/macOS 的原型一致（`__restrict` 即 C99 的 `restrict`；`__THROW`/`__nonnull` 是属性，重复声明时可省略）。合法 C |
| R2 | 声明会不会**掩盖**别的缺失声明 | 通过 | 脚本对 `src/*.c` + `tests/*.c` 全量检查，POSIX 侧「ALL CLEAN」。不是只补了报错的这一处 |
| R3 | 页面片段化后内容是否**逐字节完整** | 通过 | 实测取回页面：5288 字节（与片段化前相同），`<!DOCTYPE html>` 开头、`</html>` 结尾、`</script>`/`</style>`/`</body>` 齐全、无 `\r` |
| R4 | 片段化是否引入**新的长度问题** | 通过 | 110 个元素，最长一行 94 字符，远低于 4095 限制；每个元素是**独立**字面量（相邻字面量会被拼接，所以不能写成 `"a"\n"b"`） |
| R5 | 页面发送路径是否正确（Content-Length 与实际一致） | 通过 | 先累加所有片段长度再写头，然后逐片发送；`http_send` 的旧路径不再用于页面 |
| R6 | `posix_check.sh` 的编译选项是否**与 CI 一致** | 通过 | `-std=c99 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -Wall -Wextra -Wpedantic`，与 CMakeLists 的定义一致；额外补 `-DCAGENT_VERSION`（CMake 注入的宏） |
| R7 | 该脚本是否会**漏掉**某些源文件 | 通过 | 遍历 `src/*.c` 与 `tests/*.c`；`src/web_net.c` 里的平台私有文件也在 `src/` 下 |
| R8 | 是否有**告警**（CI 零告警门禁） | 通过 | WSL gcc：零告警；MSVC /W4：零告警 |

## 3. 遗留（记录，不阻塞）

- `posix_check.sh` 目前只在 Windows + WSL 下使用；CI 的 Linux job 其实可以直接跑它
  （省一次往返），但那会与已有的构建步骤重复——**收益小、脚本改动风险大**，暂不动。
- 脚本只做 `-fsyntax-only`，不做链接与运行；运行期行为仍由 CI 的三个平台覆盖。
- 声明 `realpath` 只在 `src/tools.c` 里；将来若有别的文件用到 `realpath`，
  需要同样处理（或把声明挪到公共头）。

## 4. 结论

CI 首跑抓到的 POSIX 问题已全部修掉，并额外补上了一个**此前不存在的本地 POSIX 校验能力**——
这直接降低了「POSIX 问题必须靠 CI 往返」的成本。✅ 通过。

## 5. 移交

`test-engineer` + `security-reviewer`（G3）：重点看 `posix_check.sh` 的可移植性
（macOS 的 gcc 需要 `brew install gcc`，命令行名可能是 `gcc-13`），
以及 `realpath` 声明在 macOS SDK 下是否同样无冲突。

## 6. 复核（由独立复核者填写，非自证）

| 复核人 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| orchestrator | 2026-10-21 | 通过 | 复跑 `posix_check.sh`（ALL CLEAN）与页面完整性检查（PASS）；确认 `.role-system/` 与根 `AGENTS.md` 零改动 |
