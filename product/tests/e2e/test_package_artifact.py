"""产物级端到端：针对**安装产物**而非构建目录跑用户主路径。

角色：release-engineer
对应检查单 §4.4 的缺口。

背景：功能正确性已由 tests/e2e/test_user_paths.py 在**构建目录**下覆盖（41/41），
但「装出来的那份二进制」本身没跑过端到端。本用例把 web 资源指向**安装树/解压
目录**，用同一套 E2E 断言跑一遍，从而验证：

1. 安装布局正确（web/ 与可执行文件同级）；
2. 打包过程没有引入构建目录下不存在的行为差异。

未找到产物时 skip，不影响只跑单测的场景。
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Optional

import pytest

_PRODUCT_ROOT = Path(__file__).resolve().parents[2]


def _find_package_zip() -> Optional[Path]:
    build = _PRODUCT_ROOT / "build"
    if not build.is_dir():
        return None
    candidates = sorted(
        p for p in build.rglob("*.zip")
        if "save-inspector" in p.name and "source" not in p.name
    )
    return candidates[0] if candidates else None


@pytest.fixture(scope="session")
def unpacked() -> "tuple":
    """解压真实发布产物，返回 (可执行文件目录, web 目录, 清理函数)。"""
    archive = _find_package_zip()
    if archive is None:
        pytest.skip("未找到打包产物；请先运行 scripts/package.sh 或 cpack")

    tmp = Path(tempfile.mkdtemp(prefix="si_pkg_"))
    with zipfile.ZipFile(archive) as zf:
        zf.extractall(tmp)

    # 产物结构：<root>/bin/save_inspector_ui.exe + <root>/bin/web/
    roots = [p for p in tmp.iterdir() if p.is_dir()]
    assert roots, "产物结构异常：未解压出目录"
    base = roots[0]
    bindir = base / "bin"
    exe = bindir / ("save_inspector_ui.exe" if os.name == "nt" else "save_inspector_ui")
    if not exe.is_file():
        pytest.fail(f"产物中找不到入口可执行文件：{exe}")

    web = bindir / "web"
    assert (web / "index.html").is_file(), (
        "界面资源未与可执行文件同级——免安装形态启动会得到「界面资源缺失」"
    )

    # 关键：产物里不应出现测试框架或 Python 内部工具链
    leaked = [
        p.relative_to(base).as_posix()
        for p in base.rglob("*")
        if p.is_file() and (p.suffix == ".py" or "gtest" in p.name.lower()
                            or "gmock" in p.name.lower())
    ]
    assert not leaked, f"产物混入测试/Python 文件：{leaked[:5]}"

    yield exe, web
    shutil.rmtree(tmp, ignore_errors=True)


def test_package_layout_is_self_contained(unpacked) -> None:
    """产物自包含：入口 + 界面资源齐备，且无测试/Python 泄漏。"""
    exe, web = unpacked
    assert exe.is_file()
    assert (web / "index.html").is_file()
    assert (web / "app.js").is_file()
    assert (web / "styles.css").is_file()


def test_package_runs_and_serves_ui(unpacked) -> None:
    """从**产物**启动，走一次首屏与真实存档的完整主路径。"""
    import http.client
    import json
    import socket
    import time

    exe, web = unpacked
    sys.path.insert(0, str(_PRODUCT_ROOT / "python"))
    from saveinspector_tools.save_format import SAMPLES  # noqa: E402

    # 自选空闲端口，不依赖子进程 stdout（Windows 上重定向的 stdout 是块缓冲的）
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        port = int(s.getsockname()[1])

    proc = subprocess.Popen(
        [str(exe), "--port", str(port), "--no-open", "--web-root", str(web)],
        cwd=str(exe.parent),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        deadline = time.time() + 30
        ready = False
        while time.time() < deadline and not ready:
            if proc.poll() is not None:
                pytest.fail(f"产物启动即退出，退出码 {proc.returncode}")
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(0.5)
                ready = s.connect_ex(("127.0.0.1", port)) == 0
            if not ready:
                time.sleep(0.2)
        assert ready, "产物未在 30 秒内开始监听"

        def req(method: str, path: str, body: Optional[bytes] = None,
                headers: Optional[dict] = None):
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=15)
            try:
                conn.request(method, path, body=body, headers=headers or {})
                resp = conn.getresponse()
                return resp.status, resp.read()
            finally:
                conn.close()

        # 界面资源
        for path, needle in (
            ("/", "存档查看器"),
            ("/app.js", "openFile"),
            ("/styles.css", "dropzone"),
        ):
            status, raw = req("GET", path)
            assert status == 200, f"{path} HTTP {status}"
            text = raw.decode("utf-8", "replace")
            assert needle in text, f"{path} 内容不符合预期（找不到 {needle!r}）"

        # 真实存档主路径：打开 → 列出 → 详情（详情这条曾因非法 JSON 而整页不可用）
        save = SAMPLES["healthy-v3"]()
        status, raw = req(
            "POST", "/api/save/open", body=save,
            headers={"Content-Type": "application/octet-stream",
                     "Content-Length": str(len(save))},
        )
        assert status == 200
        assert json.loads(raw.decode("utf-8"))["ok"] is True

        status, raw = req("GET", "/api/save/slots")
        assert status == 200
        assert len(json.loads(raw.decode("utf-8"))["slots"]) == 3

        status, raw = req("GET", "/api/save/slot?index=0")
        assert status == 200
        payload = json.loads(raw.decode("utf-8"))
        assert payload["ok"] is True and payload["slot"]["level"] == 42

        # 导出
        status, raw = req("GET", "/api/save/export?index=0&format=text")
        assert status == 200
        assert "第 42 章" in raw.decode("utf-8")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()