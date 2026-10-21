#!/usr/bin/env bash
# 配置 + 构建 + 测试（Linux / macOS）
#
# 这是仓库文档（AGENTS.md）里写的那个入口：
#     ./scripts/build.sh Release
#
# 用法：./scripts/build.sh [Debug|Release] [额外的 cmake 参数...]
#   CAGENT_BUILD_DIR 可覆盖构建目录（默认 build），CI 里用独立目录做冒烟。
set -uo pipefail
set +e

CONFIG="${1:-Release}"
if [ $# -gt 0 ]; then shift; fi
BUILD_DIR="${CAGENT_BUILD_DIR:-build}"

# 变量一律用 ${} 包住：后面紧跟中文全角标点时，bash 在非 UTF-8 locale
# （如 macOS runner 的 C locale）会把标点的字节当成变量名的一部分，
# 报 `BUILD_DIR，: unbound variable` —— 在 Linux 上却不会，因为它用的是 UTF-8 locale。
echo "== 配置（${BUILD_DIR}，${CONFIG}）=="
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$CONFIG" "$@"
if [ $? -ne 0 ]; then exit 1; fi

echo "== 构建 =="
cmake --build "$BUILD_DIR" --config "$CONFIG"
if [ $? -ne 0 ]; then exit 1; fi

echo "== 测试 =="
ctest --test-dir "$BUILD_DIR" -C "$CONFIG" --output-on-failure
exit $?
