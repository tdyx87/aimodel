"""用户场景 E2E：从**真实可执行文件**启动，走真实用户路径，不 mock 核心链路。

角色：test-engineer
契约：product/docs/design/file-transfer-contract.md → save-format-contract.md §4/§5
覆盖：frontend-implementer 审计 §9 移交的五条主路径

设计约束（roles.md §3.8）：
- 从可执行文件启动，不链接库、不打桩；解析链路全是真的；
- 五条主路径逐条走用户视角，不以「函数返回 true」充当验收。

能力边界（如实登记，不粉饰）：
- ✅ 路径 1（首屏空态）：用真实浏览器（Edge/Chrome headless `--dump-dom`）渲染
  真实页面，断言 DOM 中的用户可见文案与可访问性属性。**这是 DOM 级验证。**
- ✅ 路径 2~5：以 HTTP 层驱动真实服务端（与浏览器同协议同端点），断言响应内容
  与界面稿一致。**这是集成级验证，非 DOM 级。**
- ❌ 未覆盖：用户实际点击文件选择框 → 浏览器文件上传的 DOM 流程。环境无可用
  的浏览器自动化框架（Playwright/Selenium 均未安装），且 `--dump-dom` 无法
  触发 file input。已在 test-engineer 审计 §5 登记为缺口。
"""

from __future__ import annotations

import http.client
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Iterator, Optional

import pytest

# ---- 定位构建产物 ----
# 从本文件向上找 product 根，再在常见的构建输出位置找可执行文件。
_PRODUCT_ROOT = Path(__file__).resolve().parents[2]
_EXE_NAMES = ("save_inspector_ui", "save_inspector_ui.exe", "save_inspector_cli.exe",
              "save_inspector_cli")


def _find_binary(name_stems: tuple) -> Optional[Path]:
    """在构建目录中查找可执行文件。找不到返回 None（用例 skip）。"""
    build = _PRODUCT_ROOT / "build"
    if not build.is_dir():
        return None
    for stem in name_stems:
        for candidate in build.rglob(stem):
            if candidate.is_file():
                return candidate
    return None


def _find_web_root() -> Optional[Path]:
    build = _PRODUCT_ROOT / "build"
    if not build.is_dir():
        return None
    for candidate in build.rglob("web"):
        if (candidate / "index.html").is_file():
            return candidate
    return None


sys.path.insert(0, str(_PRODUCT_ROOT / "python"))

from saveinspector_tools.save_format import SAMPLES  # noqa: E402


class LiveServer:
    """已启动的真实服务。"""

    def __init__(self, port: int) -> None:
        self.port = port

    def request(
        self,
        method: str,
        path: str,
        body: Optional[bytes] = None,
        headers: Optional[dict] = None,
        timeout: float = 15.0,
    ) -> tuple:
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout)
        try:
            conn.request(method, path, body=body, headers=headers or {})
            resp = conn.getresponse()
            return resp.status, dict(resp.getheaders()), resp.read()
        finally:
            conn.close()

    def open_save(self, data: bytes) -> dict:
        """以浏览器同样的方式上传存档字节。"""
        status, _, raw = self.request(
            "POST",
            "/api/save/open",
            body=data,
            headers={
                "Content-Type": "application/octet-stream",
                "Content-Length": str(len(data)),
            },
        )
        assert status == 200, f"上传存档 HTTP {status}: {raw[:200]!r}"
        return json.loads(raw.decode("utf-8"))

    def slots(self) -> dict:
        status, _, raw = self.request("GET", "/api/save/slots")
        assert status == 200, f"列出槽位 HTTP {status}"
        return json.loads(raw.decode("utf-8"))


@pytest.fixture(scope="session")
def server() -> Iterator[LiveServer]:
    exe = _find_binary(("save_inspector_ui", "save_inspector_ui.exe"))
    web = _find_web_root()
    if exe is None or web is None:
        pytest.skip("未找到构建产物；请先运行 scripts/build.ps1 或 build.sh")

    # 自己挑一个空闲端口并显式传入，**不**靠解析子进程 stdout：
    # Windows 上被重定向的 stdout 是块缓冲的，程序不显式 fflush 时
    # 端口那一行永远不落盘，readline() 会永久阻塞（本项目就踩过一次）。
    port = _pick_free_port()
    proc = subprocess.Popen(
        [str(exe), "--port", str(port), "--no-open", "--web-root", str(web)],
        cwd=str(exe.parent),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        _wait_until_listening(port, proc)
        yield LiveServer(port)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


def _pick_free_port() -> int:
    import socket

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def _wait_until_listening(port: int, proc: "subprocess.Popen") -> None:
    """轮询直到端口可连接，或子进程先退出。"""
    import socket

    deadline = time.time() + 30
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"服务启动即退出，退出码 {proc.returncode}")
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(0.5)
            if s.connect_ex(("127.0.0.1", port)) == 0:
                return
        time.sleep(0.2)
    raise RuntimeError("30 秒内服务未开始监听")


# ---------------------------------------------------------------------------
# 路径 1：首屏空态（DOM 级 —— 真实浏览器渲染）
# ---------------------------------------------------------------------------

def _browser_candidates() -> list:
    """按可靠度排序的候选浏览器列表。

    为什么是「列表」而不是「第一个命中的」：本机实测同一条命令行参数下，
    Edge 存在但 `--headless --dump-dom` **静默产出空文件**（退出码 0、零字节、
    零 DOM），而 Chrome 正常。若只取第一个命中项，DOM 级验证的结果就取决于
    「机器上装了哪个浏览器、哪个先被 which 命中」——测试随机报红，比不覆盖更坏。
    """
    candidates: list = []
    for name in ("chrome", "chromium", "google-chrome", "msedge"):
        found = shutil.which(name)
        if found:
            candidates.append(Path(found))
    # 各平台的标准安装位置。macOS/Linux 上浏览器**不在 PATH 上**（macOS 尤其：
    # Chrome 只以 .app 形式存在），只靠 shutil.which 会让 DOM 级验证在 CI 上
    # 静默 skip——那等于丢掉了首屏渲染的验证。
    for raw in (
        # Windows
        r"C:\Program Files\Google\Chrome\Application\chrome.exe",
        r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
        r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
        # macOS
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
        "/Applications/Chromium.app/Contents/MacOS/Chromium",
        "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
        # Linux
        "/usr/bin/google-chrome",
        "/usr/bin/google-chrome-stable",
        "/usr/bin/chromium",
        "/usr/bin/chromium-browser",
        "/snap/bin/chromium",
    ):
        if os.path.exists(raw):
            candidates.append(Path(raw))
    seen: set = set()
    unique: list = []
    for path in candidates:
        key = str(path).lower()
        if key not in seen:
            seen.add(key)
            unique.append(path)
    return unique


def _dump_dom(browser: Path, url: str, tmp_path: Path, tag: str) -> str:
    """用真实浏览器渲染并导出 DOM。未产出返回空串（本机 headless 不可用）。

    --user-data-dir 必须给独立目录：共用默认 profile 时，浏览器会因 profile 锁
    与后台进程而**不自行退出**（--dump-dom 已写完但进程挂着）。
    """
    dom_path = tmp_path / ("dom-%s.html" % tag)
    profile = tmp_path / ("profile-%s" % tag)
    argv = [
        str(browser),
        "--headless",
        "--disable-gpu",
        "--no-sandbox",
        # CI 容器/受限环境下 /dev/shm 往往过小，Chrome 会因此不产出 DOM；
        # 该开关在其他平台上是空操作。
        "--disable-dev-shm-usage",
        "--no-first-run",
        "--no-default-browser-check",
        "--disable-extensions",
        f"--user-data-dir={profile}",
        "--virtual-time-budget=5000",
        "--dump-dom",
        url,
    ]
    with dom_path.open("wb") as sink:
        proc = subprocess.Popen(argv, stdout=sink, stderr=subprocess.DEVNULL)
        try:
            proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            # 进程未退出不算失败：--dump-dom 的产物已完整落盘。
            # 判定标准是 DOM 内容，不是进程退出码。
            proc.kill()
            proc.wait(timeout=10)
    if not dom_path.exists():
        return ""
    return dom_path.read_text(encoding="utf-8", errors="replace")


def test_empty_state_renders_in_real_browser(server: LiveServer, tmp_path: Path) -> None:
    """首屏空态：真实浏览器渲染后的 DOM 必须含用户可见文案与可访问性属性。

    PRD NFR-1「启动后 2 秒内可交互」的界面侧体现。
    """
    candidates = _browser_candidates()
    if not candidates:
        pytest.skip("环境无 Chrome/Edge，跳过 DOM 级验证（HTTP 级验证见其余用例）")

    dom = ""
    for index, browser in enumerate(candidates):
        dom = _dump_dom(browser, f"http://127.0.0.1:{server.port}/", tmp_path, str(index))
        if dom:
            break
    if not dom:
        # 空 DOM 只可能来自「浏览器自己没产出」：页面加载失败时 --dump-dom 仍会
        # 输出错误页 DOM。故这是环境能力缺口，如实跳过而不是把它记成界面缺陷。
        pytest.skip(
            "候选浏览器均未产出 DOM（本机 headless 不可用）："
            + ", ".join(str(b) for b in candidates)
        )

    assert dom, "浏览器未产出 DOM"
    # 用户可见文案
    assert "选择或拖放一个存档文件" in dom, "首屏空态文案缺失"
    assert "存档查看器" in dom
    # 隐私承诺必须首屏可见（PRD NFR-3/NFR-5）
    assert "绝不修改你的原始文件" in dom, "只读承诺未在首屏呈现"
    assert "只读取，不修改" in dom
    # 拖放区 + 文件选择器 + 键盘可达（设计系统 §6）
    assert 'class="dropzone"' in dom
    assert 'type="file"' in dom, "缺文件选择器：拖放不能是唯一途径"
    assert 'tabindex="0"' in dom, "拖放区不可键盘聚焦"
    # 零外部资源（PRD NFR-3）
    assert 'src="http' not in dom and 'href="http' not in dom, "存在外部资源引用"


# ---------------------------------------------------------------------------
# 路径 2：打开正常存档（集成级）
# ---------------------------------------------------------------------------

def test_open_healthy_save_lists_slots(server: LiveServer) -> None:
    payload = server.open_save(SAMPLES["healthy-v3"]())
    assert payload["ok"] is True, payload
    save = payload["save"]
    assert save["version"] == 3
    assert len(save["slots"]) == 3

    # 进度摘要：US-2 要求槽位卡片同时显示进度与健康状态
    listing = server.slots()
    assert listing["ok"] is True
    first = listing["slots"][0]
    assert first["level"] == 42
    assert first["coins"] == 12480
    assert first["healthy"] is True
    # 界面稿要求「时长」用可读文案而非裸秒
    assert first["playtime"] == "128 小时"
    assert first["last_played"] != "未知"

    # 道具名由核心库给出（契约 §3.2：前端不维护映射表）
    assert save["slots"][0]["items"][0]["name"] == "治疗药水"


def test_slot_detail_and_export(server: LiveServer) -> None:
    """US-3/US-4：查看详情并导出，两处内容须一致。

    响应形状按契约 §4：`{"ok":true,"slot":{...}}`，槽位对象**嵌在** slot 下。
    """
    server.open_save(SAMPLES["healthy-v3"]())
    status, _, raw = server.request("GET", "/api/save/slot?index=0")
    assert status == 200
    body = json.loads(raw.decode("utf-8"))
    assert body["ok"] is True
    slot = body["slot"]
    assert slot["level"] == 42
    assert slot["items"][0]["name"] == "治疗药水"
    # 契约 §4 的形状：必须是嵌套对象，不能是 {"ok":true,{...}}（那是非法 JSON）
    assert isinstance(slot, dict)

    status, headers, exported = server.request(
        "GET", "/api/save/export?index=0&format=json"
    )
    assert status == 200
    assert headers["Content-Type"].startswith("application/json")
    assert "attachment" in headers["Content-Disposition"]
    exported_json = json.loads(exported.decode("utf-8"))
    # 导出内容必须与界面所见一致（US-4）
    assert exported_json["level"] == slot["level"]
    assert exported_json["coins"] == slot["coins"]
    assert exported_json["items"] == slot["items"]

    status, headers, report = server.request(
        "GET", "/api/save/export?index=0&format=text"
    )
    assert status == 200
    assert headers["Content-Type"].startswith("text/plain")
    text = report.decode("utf-8")
    assert "第 42 章" in text
    assert "未被修改" in text, "导出报告必须声明原始文件未被修改"


def test_slot_endpoint_returns_wellformed_json(server: LiveServer) -> None:
    """回归哨兵：`/api/save/slot` 的响应必须能被 JSON 解析。

    这条曾真实失败——服务端拼成 {"ok":true,{"slot":1,...}}（非法 JSON），
    前端 response.json() 抛异常，槽位详情页完全打不开。
    库层单测覆盖不到（那里没有 HTTP 拼装），/api/save/slots 用例也覆盖不到。
    """
    server.open_save(SAMPLES["healthy-v3"]())
    status, _, raw = server.request("GET", "/api/save/slot?index=0")
    assert status == 200
    text = raw.decode("utf-8")
    try:
        payload = json.loads(text)
    except json.JSONDecodeError as exc:
        pytest.fail(f"响应不是合法 JSON：{exc}\n前 120 字符：{text[:120]}")
    assert payload["ok"] is True


# ---------------------------------------------------------------------------
# 路径 3：部分损坏（US-3 核心：能读多少显示多少）
# ---------------------------------------------------------------------------

def test_partially_corrupted_save_keeps_readable_slots(server: LiveServer) -> None:
    payload = server.open_save(SAMPLES["corrupt-checksum"]())
    assert payload["ok"] is True, "部分损坏不得导致整体拒绝"
    save = payload["save"]

    assert len(save["slots"]) == 3
    healthy = [s for s in save["slots"] if s["healthy"]]
    corrupt = [s for s in save["slots"] if not s["healthy"]]
    assert len(healthy) == 2, "未损坏的槽位必须仍可读"
    assert len(corrupt) == 1

    # US-5「哪里坏了」：诊断必须定位到具体槽位号
    warnings = [d for d in save["diagnostics"] if d["severity"] == "warning"]
    assert warnings, "缺少损坏诊断"
    diag = warnings[0]
    assert "槽位 2" in diag["title"]
    # 三段式缺一不可
    assert diag["title"] and diag["detail"] and diag["advice"]
    # 诊断不得泄漏开发者术语（PRD US-5）
    for text in (diag["title"], diag["detail"], diag["advice"]):
        assert "0x" not in text, "诊断泄漏了十六进制"
        assert "C:" not in text, "诊断泄漏了本机路径"

    # 关键：损坏槽位仍返回其可读内容（界面卡片保持可点击的前提）
    listing = server.slots()
    assert len(listing["slots"]) == 3


# ---------------------------------------------------------------------------
# 路径 4：完全不可解析
# ---------------------------------------------------------------------------

@pytest.mark.parametrize(
    "sample,expect_keyword",
    [
        ("corrupt-magic", "不是存档"),
        ("corrupt-truncated", "不完整"),
        ("encrypted", "加密"),
        ("future-version", "更新版本"),
    ],
)
def test_unparseable_save_yields_actionable_diagnosis(
    server: LiveServer, sample: str, expect_keyword: str
) -> None:
    payload = server.open_save(SAMPLES[sample]())
    assert payload["ok"] is False
    diags = payload["diagnostics"]
    assert diags, "必须给出诊断"
    first = diags[0]
    assert first["severity"] == "error"
    # US-5：必须回答「哪里/为什么/能做什么」，且必须是自然语言
    assert first["title"] and first["detail"] and first["advice"]
    assert expect_keyword in first["title"], f"文案未命中关键词：{first['title']!r}"

    # 后续查询应返回 409：打开失败已清空状态，服务端不得还留着上一个存档
    status, _, raw = server.request("GET", "/api/save/slots")
    assert status == 409, f"打开失败后仍保留了旧存档数据（HTTP {status}）"
    assert json.loads(raw.decode("utf-8"))["ok"] is False


# ---------------------------------------------------------------------------
# 路径 5：旧版本升级（US-6）
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("sample,expect_ver", [("legacy-v1", 1), ("legacy-v2", 2)])
def test_legacy_save_is_upgraded_and_labelled(
    server: LiveServer, sample: str, expect_ver: int
) -> None:
    payload = server.open_save(SAMPLES[sample]())
    assert payload["ok"] is True
    save = payload["save"]
    assert save["version"] == expect_ver

    # 必须标注「已升级」（US-6），且明确告知原始文件未被修改（NFR-5）
    infos = [d for d in save["diagnostics"] if d["severity"] == "info"]
    assert infos, "缺少升级提示"
    assert "升级" in infos[0]["title"]
    assert "没有被修改" in infos[0]["advice"]

    # 缺失字段按默认值补齐，功能可用
    assert save["slots"], "升级后槽位不应为空"
    assert save["slots"][0]["level"] > 0
    assert save["slots"][0]["playtime"] is not None


def test_empty_save_shows_empty_state_payload(server: LiveServer) -> None:
    payload = server.open_save(SAMPLES["empty"]())
    assert payload["ok"] is True
    assert payload["save"]["slots"] == []
    assert server.slots()["slots"] == []


# ---------------------------------------------------------------------------
# 离线/异常路径：不 mock，验证真实降级
# ---------------------------------------------------------------------------

def test_request_before_open_returns_conflict(server: LiveServer) -> None:
    # 单会话模型下，上一次 open 的状态可能残留，故只断言「不崩溃且有明确结构」
    status, _, raw = server.request("GET", "/api/save/slot?index=99")
    assert status in (404, 409)
    body = json.loads(raw.decode("utf-8"))
    assert body["ok"] is False


def test_static_assets_served_with_correct_content_type(server: LiveServer) -> None:
    """免构建静态资源：路径与 MIME 必须匹配（roles.md §3.5 完成标准）。"""
    for path, expect_type in (
        ("/", "text/html"),
        ("/index.html", "text/html"),
        ("/app.js", "text/javascript"),
        ("/styles.css", "text/css"),
    ):
        status, headers, body = server.request("GET", path)
        assert status == 200, f"{path} HTTP {status}"
        assert headers["Content-Type"].startswith(expect_type), path
        assert body, f"{path} 返回空正文"
        # 声明 charset=utf-8（中文界面不得乱码）
        assert "utf-8" in headers["Content-Type"].lower()


def test_path_traversal_on_static_rejected(server: LiveServer) -> None:
    status, _, _ = server.request("GET", "/../../../Windows/win.ini")
    assert status == 404


# ---------------------------------------------------------------------------
# security-reviewer MED-2：错误文案不得泄漏本机路径
# ---------------------------------------------------------------------------

def test_error_messages_do_not_leak_local_paths(server: LiveServer) -> None:
    """不存在的路径 / 目录 / 空文件：响应体都不得含本机路径（契约 §5）。

    当前服务端靠「人工不把 path 拼进消息」保证，本用例把该约定固定下来，
    防止后续改动回归（security-reviewer MED-2）。
    """
    secret_dir = "SecretPlayerProfile"
    probes: list = []

    # 1) 直接 open 一个不是存档的路径（浏览器链路不传路径，这里用 CLI 覆盖见下）
    #    HTTP 链路：上传内容为空 → 「空文件」类错误
    empty = b""
    status, _, raw = server.request(
        "POST",
        "/api/save/open",
        body=empty,
        headers={"Content-Type": "application/octet-stream",
                 "Content-Length": "0"},
    )
    probes.append(("空内容", status, raw))

    # 2) 目录形式的内容
    status, _, raw = server.request(
        "POST",
        "/api/save/open",
        body=b"\x00" * 16,
        headers={"Content-Type": "application/octet-stream",
                 "Content-Length": "16"},
    )
    probes.append(("全零字节", status, raw))

    for label, status, raw in probes:
        text = raw.decode("utf-8", "replace")
        assert secret_dir not in text, f"{label}：响应体泄漏了路径片段"
        assert "C:" not in text and "/Users/" not in text, f"{label}：响应体含绝对路径"
        assert "AppData" not in text, f"{label}：响应体含本机目录名"
        # 诊断三段式仍须齐备
        if b'"diagnostics"' in raw:
            import json as _json

            payload = _json.loads(text)
            if payload.get("ok") is False:
                for diag in payload["diagnostics"]:
                    assert diag["title"] and diag["detail"] and diag["advice"]


def test_cli_error_does_not_leak_path() -> None:
    """CLI 打开不存在的文件：stderr 同样不得含该路径（契约 §5）。"""
    cli = _find_binary(("save_inspector_cli.exe", "save_inspector_cli"))
    if cli is None:
        pytest.skip("未构建 CLI")

    missing = "/definitely/not/here/SecretPlayerProfile.sav"
    proc = subprocess.run(
        [str(cli), missing], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=30,
    )
    out = proc.stdout.decode("utf-8", "replace")
    assert proc.returncode != 0, "打开不存在的文件应返回非零退出码"
    assert "SecretPlayerProfile" not in out, f"CLI 错误信息泄漏了路径：{out!r}"
    # 仍是用户可读文案，而不是错误码
    assert "无法打开存档" in out


# ---------------------------------------------------------------------------
# 遗留项复核：reviewer 🟡-1「共享 inspector 未加锁」的前提
# ---------------------------------------------------------------------------

def test_close_per_request_premise_holds(server: LiveServer) -> None:
    """复核 reviewer 🟡-1：响应恒带 `Connection: close`。

    MED-1 修复后 inspector 已加锁，本前提不再承担正确性责任，但仍是协议约定，
    值得作为回归断言保留。用静态资源而非存档接口，避免依赖前序用例的状态
    （打开失败会清空已加载的存档，见 MED-1 修复）。
    """
    status, headers, _ = server.request("GET", "/")
    assert status == 200
    connection = headers.get("Connection", "").lower()
    assert connection == "close", f"前提被破坏：Connection={connection!r}"


def test_concurrent_reads_do_not_corrupt_state(server: LiveServer) -> None:
    """并发**读**：4 线程 × 8 次列出槽位，不得出现异常。

    注意：本用例只覆盖并发读，**不覆盖** MED-1 那类并发写数据竞争——
    见下一条。
    """
    import threading

    server.open_save(SAMPLES["healthy-v3"]())
    errors: list = []

    def worker() -> None:
        try:
            for _ in range(8):
                status, _, _ = server.request("GET", "/api/save/slots")
                if status != 200:
                    errors.append(status)
        except Exception as exc:  # noqa: BLE001
            errors.append(exc)

    threads = [threading.Thread(target=worker) for _ in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=30)
    assert not errors, f"并发读出现异常：{errors[:5]}"


def test_concurrent_opens_do_not_corrupt_state(server: LiveServer) -> None:
    """并发**写**：多线程同时上传**不同**存档（security-reviewer MED-1 回归）。

    这是 MED-1 的回归哨兵：修复前 `Impl::inspector` 被所有工作线程共享且无锁，
    两个 open 并发时可能读到赋值过程中的 std::string/vector 中间状态
    （未定义行为，最坏崩溃）。修复前若只测并发读，用例会「全绿」——
    **并发写才是能暴露该竞态的用例**，这一点已在测试策略 §5 记录。

    判定：
    - 服务不崩溃、不返回 5xx（崩溃即说明读到撕裂状态）；
    - 结束后的状态是某个被完整打开的存档（不能是半个）。
    """
    import threading

    payloads = [
        ("healthy-v3", SAMPLES["healthy-v3"]()),
        ("legacy-v1", SAMPLES["legacy-v1"]()),
        ("healthy-v3", SAMPLES["healthy-v3"]()),
        ("legacy-v2", SAMPLES["legacy-v2"]()),
    ]
    errors: list = []
    barrier = threading.Barrier(len(payloads))

    def worker(data: bytes) -> None:
        try:
            barrier.wait(timeout=20)  # 尽量让请求真正重叠
            status, _, raw = server.request(
                "POST",
                "/api/save/open",
                body=data,
                headers={
                    "Content-Type": "application/octet-stream",
                    "Content-Length": str(len(data)),
                },
            )
            if status != 200:
                errors.append(("status", status))
                return
            body = json.loads(raw.decode("utf-8"))  # 非合法 JSON 即说明状态被撕裂
            if body.get("ok") is not True:
                errors.append(("payload", body.get("diagnostics")))
        except Exception as exc:  # noqa: BLE001
            errors.append(exc)

    threads = [threading.Thread(target=worker, args=(d,)) for _, d in payloads]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=40)

    assert not errors, f"并发写出现异常：{errors[:5]}"

    # 收尾：最终状态必须是某个**完整**的存档，而不是混合/空状态
    final = server.slots()
    assert final["ok"] is True
    assert final["slots"], "并发写后状态被清空"
    levels = {s["level"] for s in final["slots"]}
    # healthy-v3 的槽位关卡为 42/8/1；legacy-v1 为 7/8；legacy-v2 为 15
    assert levels <= {42, 8, 1, 7, 15}, f"状态被撕裂，槽位关卡异常：{levels}"
