#!/usr/bin/env bash
# scripts/package.sh — 打包脚本
# 用法：./scripts/package.sh [Release|Debug]
#
# 角色：release-engineer
#
# 本脚本只做「构建 + 测试 + 打包 + 冒烟」，**不**做签名与公证——
# 那两步需要证书与平台凭据，当前版本未配置，详见发布检查单。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build"
PKG_DIR="$BUILD_DIR/package"
CONFIG="Release"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

for arg in "$@"; do
    case "$arg" in
        Release|Debug) CONFIG="$arg" ;;
        -h|--help) sed -n '2,4p' "$0"; exit 0 ;;
        *) echo "未知参数：$arg" >&2; exit 2 ;;
    esac
done

echo "==> 1/4 构建并跑全量测试"
# 打包前必须测试过：未验证的产物不该被发布（roles.md §3.11「发布前必须过 G3 门禁」）
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$CONFIG"
cmake --build "$BUILD_DIR" --config "$CONFIG" --parallel "$JOBS"
if command -v ctest >/dev/null 2>&1; then
    ctest --test-dir "$BUILD_DIR" -C "$CONFIG" --output-on-failure
fi
if [[ -d "$ROOT/python" ]] && command -v python3 >/dev/null 2>&1; then
    (cd "$ROOT/python" && python3 -m pytest -q)
fi

echo "==> 2/4 安装到暂存目录"
# 先 install 再打包：TGZ 等部分格式依赖 install tree，产物内容与安装结果一致。
STAGE="$BUILD_DIR/stage"
rm -rf "$STAGE"
cmake --install "$BUILD_DIR" --config "$CONFIG" --prefix "$STAGE"

echo "==> 3/4 生成安装包"
rm -rf "$PKG_DIR"
mkdir -p "$PKG_DIR"
(cd "$BUILD_DIR" && cpack -G TGZ -B "$PKG_DIR")

echo "==> 4/4 产物冒烟（安装 → 启动 → 首个请求 → 关闭）"
# 冒烟只验证「装出来的东西能跑起来」，不代表功能全对（功能由 E2E 覆盖）。
BIN="$STAGE/bin/save_inspector_ui"
[[ -x "$BIN" ]] || BIN="$STAGE/bin/save_inspector_ui.exe"
if [[ ! -x "$BIN" ]]; then
    echo "未找到产物可执行文件：$STAGE/bin/" >&2
    find "$STAGE" -maxdepth 3 -type f | head -20 >&2
    exit 1
fi
# web/ 必须与可执行文件同级，否则启动后界面资源缺失
if [[ ! -f "$(dirname "$BIN")/web/index.html" ]]; then
    echo "产物缺少 web/index.html（界面资源必须与可执行文件同级）" >&2
    exit 1
fi

"$BIN" --port 0 --no-open --web-root "$(dirname "$BIN")/web" > "$BUILD_DIR/smoke.log" 2>&1 &
SMOKE_PID=$!
trap 'kill $SMOKE_PID 2>/dev/null || true' EXIT
sleep 2

# 从启动日志里取端口（程序不显式 flush 时该行可能尚未落盘，故轮询）
PORT=""
for _ in $(seq 1 30); do
    if [[ -f "$BUILD_DIR/smoke.log" ]]; then
        PORT="$(grep -oE '127\.0\.0\.1:[0-9]+' "$BUILD_DIR/smoke.log" | head -1 | cut -d: -f2 || true)"
        [[ -n "$PORT" ]] && break
    fi
    sleep 0.5
done
if [[ -z "$PORT" ]]; then
    echo "冒烟失败：未能确定监听端口" >&2
    cat "$BUILD_DIR/smoke.log" >&2
    exit 1
fi

CODE="$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:${PORT}/" || true)"
kill $SMOKE_PID 2>/dev/null || true
trap - EXIT

if [[ "$CODE" != "200" ]]; then
    echo "冒烟失败：首页返回 HTTP ${CODE}（期望 200）" >&2
    exit 1
fi

echo
echo "打包完成："
ls -la "$PKG_DIR"
echo "暂存安装树：$STAGE"
echo "冒烟：首页 HTTP $CODE ✓"