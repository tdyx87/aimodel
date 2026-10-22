#!/bin/bash
# POSIX 侧的全量语法检查 —— 本地就能发现 POSIX 分支的编译错误。
#
# 为什么需要它（这是本项目吃过最深的一次亏）：
#   CI 的 ubuntu/macos 用的是**新 glibc**，而老发行版（Ubuntu 18.04 /
#   RHEL 7 / CentOS 7，glibc < 2.28）的头文件**声明面更窄**。
#   实测：realpath 在 glibc 2.27 上即使定义了 _POSIX_C_SOURCE=200809L
#   也不被声明 → 隐式声明返回 int → 64 位指针被截断。
#   CI 全绿，但代码在老机器上是未定义行为。
#
#   既然「CI 是唯一的 POSIX 验证手段」不可改变（CI 慢且要等），
#   就把能在本地查的先查了：WSL + gcc 一分钟出结果。
#
# 用法（Windows 上的 WSL）：
#   wsl -d Ubuntu-18.04 -- bash scripts/posix_check.sh
# 用法（Linux/macOS）：
#   bash scripts/posix_check.sh
set -u
cd "$(dirname "$0")/.." || exit 1
FLAGS="-std=c99 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -Wall -Wextra -Wpedantic"
FLAGS="$FLAGS -DCAGENT_VERSION=\"0.1.0\" -Iinclude -Isrc -fsyntax-only"
fail=0
for f in src/*.c tests/*.c; do
  out=$(gcc $FLAGS "$f" 2>&1)
  if [ -n "$out" ]; then
    echo "=== $f"
    echo "$out" | head -12
    fail=1
  fi
done
if [ "$fail" = 0 ]; then
  echo "ALL CLEAN: no warnings, no errors (POSIX 侧)"
fi
exit $fail
