#!/usr/bin/env bash
# scripts/build.sh — Linux/macOS 构建脚本
# 用法：./scripts/build.sh [Release|Debug] [--no-tests] [--no-python-tests]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build"
CONFIG="Release"
TESTS="ON"
PYTHON_TESTS="ON"

# 逐个扫描参数：此前只看 "$2"，写成 `build.sh --no-tests Release` 会被静默忽略，
# 而开关被丢掉比报错更难查。
for arg in "$@"; do
    case "$arg" in
        Release|Debug)     CONFIG="$arg" ;;
        --no-tests)        TESTS="OFF" ;;
        --no-python-tests) PYTHON_TESTS="OFF" ;;
        -h|--help)
            sed -n '2,3p' "$0"
            exit 0 ;;
        *)
            echo "未知参数：$arg" >&2
            echo "用法：$(basename "$0") [Release|Debug] [--no-tests] [--no-python-tests]" >&2
            exit 2 ;;
    esac
done

cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$CONFIG" \
    -DSAVEINSPECTOR_BUILD_TESTS="$TESTS"
cmake --build "$BUILD_DIR" --parallel

if [[ "$TESTS" == "ON" ]]; then
    ctest --test-dir "$BUILD_DIR" --output-on-failure
fi

# Python 内部工具链的用例（pytest）
if [[ "$PYTHON_TESTS" == "ON" ]]; then
    if command -v python3 >/dev/null 2>&1; then
        PY=python3
    elif command -v python >/dev/null 2>&1; then
        PY=python
    else
        PY=""
    fi
    if [[ -n "$PY" ]]; then
        (cd "$ROOT/python" && "$PY" -m pytest -q)
    else
        echo "跳过 Python 用例：未找到 python" >&2
    fi
fi

echo "Build OK: $BUILD_DIR ($CONFIG)"
