#!/usr/bin/env bash
# scripts/build.sh — Linux/macOS 构建脚本（agents.md 第 3 节命令）
# 用法：./scripts/build.sh [Release|Debug] [--no-tests]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build"
CONFIG="${1:-Release}"
TESTS="ON"
if [[ "${2:-}" == "--no-tests" ]]; then
    TESTS="OFF"
fi

cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$CONFIG" \
    -DAIMODEL_BUILD_TESTS="$TESTS"
cmake --build "$BUILD_DIR" --parallel

if [[ "$TESTS" == "ON" ]]; then
    ctest --test-dir "$BUILD_DIR" --output-on-failure
fi

echo "Build OK: $BUILD_DIR ($CONFIG)"
