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

# ---- Python 层：内部工具链单测 + 用户场景 E2E（一次 pytest 跑两层）----
# testpaths 已在 python/pyproject.toml 声明为 ["tests", "../tests/e2e"]：
# E2E 从**真实可执行文件**启动，与链接库的单测覆盖面不同。所以必须从
# python/ 目录跑 pytest，拆成两条命令会重复执行 E2E。
#
# 解释器优先级：$PYTHON 显式指定 > 项目内 .venv > PATH 上的 python3/python。
# 项目内 venv 优先很重要：系统 python 往往没装 pytest。
#
# 缺 python/pytest 时**不静默跳过**：静默跳过正是「产物层其实零覆盖、
# 却被当成全绿」的来源。要跳过请显式传 --no-python-tests。
if [[ -n "${PYTHON:-}" ]]; then
    PY="$PYTHON"
elif [[ -x "$ROOT/.venv/bin/python" ]]; then
    PY="$ROOT/.venv/bin/python"
elif [[ -x "$ROOT/.venv/Scripts/python.exe" ]]; then
    PY="$ROOT/.venv/Scripts/python.exe"
elif command -v python3 >/dev/null 2>&1; then
    PY=python3
elif command -v python >/dev/null 2>&1; then
    PY=python
else
    PY=""
fi

if [[ "$PYTHON_TESTS" == "ON" ]]; then
    if [[ -z "$PY" ]]; then
        echo "错误：未找到 python，Python 层用例（内部工具链单测 + 真实产物 E2E）未运行。" >&2
        echo "      安装 python 后重试，或显式传 --no-python-tests 跳过。" >&2
        exit 1
    fi
    if ! "$PY" -c "import pytest" >/dev/null 2>&1; then
        echo "错误：$PY 未安装 pytest，Python 层用例（含 E2E）未运行。" >&2
        echo "      修复：$PY -m pip install -e \"$ROOT/python[dev]\"" >&2
        echo "      或显式传 --no-python-tests 跳过（会让产物层零覆盖）。" >&2
        exit 1
    fi
    # 不再额外传 -q：pyproject 的 addopts 已有 -q，再传一次成了 -qq，
    # 连「N passed」的结论行都被抑制。构建脚本的用途就是留下门禁证据，
    # 结论行必须可见。
    (cd "$ROOT/python" && "$PY" -m pytest)
fi

echo "Build OK: $BUILD_DIR ($CONFIG)"
