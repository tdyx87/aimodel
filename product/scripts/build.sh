#!/usr/bin/env bash
# scripts/build.sh — Linux/macOS 构建脚本
# 用法：./scripts/build.sh [Release|Debug] [--no-tests]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build"
CONFIG="Release"
TESTS="ON"

# 逐个扫描参数：此前只看 "$2"，写成 `build.sh --no-tests Release` 就会被忽略，
# 而开关被静默丢弃比报错更难查。
for arg in "$@"; do
    case "$arg" in
        Release|Debug) CONFIG="$arg" ;;
        --no-tests)   TESTS="OFF" ;;
        -h|--help)    sed -n '2,3p' "$0"; exit 0 ;;
        *)
            echo "未知参数：$arg" >&2
            echo "用法：$(basename "$0") [Release|Debug] [--no-tests]" >&2
            exit 2
            ;;
    esac
done

cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$CONFIG" \
    -DAIMODEL_BUILD_TESTS="$TESTS"
cmake --build "$BUILD_DIR" --parallel

if [[ "$TESTS" == "ON" ]]; then
    ctest --test-dir "$BUILD_DIR" --output-on-failure
fi

echo "Build OK: $BUILD_DIR ($CONFIG)"
