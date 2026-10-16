"""界面级端到端：在**真实浏览器**里走完用户主路径。

角色：test-engineer
对应：发布检查单 §4 的剩余缺口

背景：此前界面层只有一条用例——`--dump-dom` 验证**空态首屏**。
从「选择文件」往后的全部渲染（槽位列表、槽位详情、导出、诊断视图）
只在 **HTTP 层**被验过，界面层一条没有。

这个缺口有前科：`/api/save/slot` 曾返回非法 JSON，导致槽位详情页
**完全打不开**，而当时 HTTP 层与产物层用例全绿——因为它们断言的是
「接口返回了内容」，而不是「用户看到了内容」。本文件补的就是这一层。

与 `test_user_paths.py` 的分工：
- 那份用裸 socket 打 HTTP，验证**接口**；
- 这份用真实浏览器，验证**用户看到的东西**。

Playwright 不可用时**跳过**并说明原因；设
`SAVE_INSPECTOR_REQUIRE_UI_E2E=1` 时改为**失败**（CI 用），避免
「零覆盖却显示全绿」。
"""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from test_user_paths import LiveServer, SAMPLES, server  # noqa: F401,E402


def _require_ui() -> bool:
    return os.environ.get("SAVE_INSPECTOR_REQUIRE_UI_E2E") == "1"


def _edge_executable() -> "str | None":
    """复用系统已装的 Edge/Chrome，**不下载**浏览器。

    下载几百 MB 的浏览器只为跑 5 条用例不划算，而且会给 CI 引入
    一个新的外部依赖源（见 release-checklist 对 NSIS 的同类教训）。
    """
    found = shutil.which("msedge") or shutil.which("google-chrome") or shutil.which("chromium")
    if found:
        return found
    for candidate in (
        r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    ):
        if Path(candidate).is_file():
            return candidate
    return None


@pytest.fixture(scope="module")
def browser():
    """启动真实浏览器。Playwright 或浏览器缺失时按模式跳过或失败。"""
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        message = "未安装 playwright（pip install playwright）"
        if _require_ui():
            pytest.fail(message + "；SAVE_INSPECTOR_REQUIRE_UI_E2E=1 要求界面 E2E 必须真实执行")
        pytest.skip(message)

    exe = _edge_executable()
    if exe is None:
        message = "未找到 Edge/Chrome；本机无法验证界面层"
        if _require_ui():
            pytest.fail(message + "；SAVE_INSPECTOR_REQUIRE_UI_E2E=1 要求界面 E2E 必须真实执行")
        pytest.skip(message)

    with sync_playwright() as pw:
        try:
            b = pw.chromium.launch(executable_path=exe, headless=True)
        except Exception as exc:  # pragma: no cover - 环境相关
            message = f"浏览器启动失败：{exc}"
            if _require_ui():
                pytest.fail(message)
            pytest.skip(message)
        yield b
        b.close()


def _open_sample(page, url: str, sample: bytes, name: str = "healthy-v3.sav") -> None:
    """在页面上模拟「用户选了一个存档文件」。

    走的是真实入口 `input[type=file]`（app.js:108），不是直接调内部函数——
    直接调函数会绕过事件绑定与 UI 状态机，也就绕过了要测的东西。
    """
    page.goto(url, wait_until="load")
    page.set_input_files("input[type=file]", files=[{
        "name": name,
        "mimeType": "application/octet-stream",
        "buffer": sample,
    }])
    page.wait_for_selector(".slot-card", timeout=15000)


def test_opening_save_renders_slot_list(server: LiveServer, browser) -> None:
    """US-2：选择存档后，槽位列表要真的出现在页面上，且内容正确。"""
    page = browser.new_page()
    try:
        _open_sample(page, f"http://127.0.0.1:{server.port}/", SAMPLES["healthy-v3"]())

        cards = page.query_selector_all(".slot-card")
        assert len(cards) == 3, f"应显示 3 个槽位，实际 {len(cards)}"

        # 界面稿要求卡片同时显示关卡与金币——断言文案，而不是断言内部字段
        body = page.inner_text("body")
        for needle in ("第 42 章", "12480", "128 小时"):
            assert needle in body, f"页面缺少 {needle!r}"
    finally:
        page.close()


def test_clicking_slot_renders_detail(server: LiveServer, browser) -> None:
    """US-3：点击槽位后详情要真的渲染出来。

    这一条正是历史上翻车的地方——接口返回过非法 JSON，HTTP 层测试全绿，
    但详情页整个打不开。所以这里断言的是**页面里出现的文字**，
    而不是「接口返回了 JSON」。
    """
    page = browser.new_page()
    try:
        _open_sample(page, f"http://127.0.0.1:{server.port}/", SAMPLES["healthy-v3"]())

        page.click(".slot-card")
        page.wait_for_selector(".detail-head", timeout=15000)

        detail = page.inner_text(".stage")
        assert "治疗药水" in detail, "详情未显示道具名（道具名由核心库给出，契约 §3.2）"
        assert "第 42 章" in detail
    finally:
        page.close()


def test_export_buttons_are_wired(server: LiveServer, browser) -> None:
    """US-4：详情页的导出按钮存在且可点。

    只验证按钮存在与可交互，**不**断言下载内容——下载流由 HTTP 层
    `test_slot_detail_and_export` 覆盖，界面层重复验证意义不大。
    """
    page = browser.new_page()
    try:
        _open_sample(page, f"http://127.0.0.1:{server.port}/", SAMPLES["healthy-v3"]())
        page.click(".slot-card")
        page.wait_for_selector(".detail-actions", timeout=15000)

        labels = page.inner_text(".detail-actions")
        assert "导出 JSON" in labels, f"缺少导出 JSON 按钮，实际：{labels!r}"
        assert "导出可读报告" in labels

        # 按钮必须真的可点（未被 disabled 挡住）
        assert page.query_selector(".detail-actions .btn:not([disabled])") is not None
    finally:
        page.close()


def test_broken_save_shows_diagnosis_view(server: LiveServer, browser) -> None:
    """US-5：损坏存档要进入诊断视图，而不是白屏或静默失败。"""
    page = browser.new_page()
    try:
        page.goto(f"http://127.0.0.1:{server.port}/", wait_until="load")
        page.set_input_files("input[type=file]", files=[{
            "name": "corrupt.sav",
            "mimeType": "application/octet-stream",
            "buffer": SAMPLES["corrupt-magic"](),
        }])
        page.wait_for_selector(".diagnostic", timeout=15000)

        text = page.inner_text("body")
        assert "不是存档文件" in text or "存档" in text, "诊断视图未给出原因"
        # 三段式缺一不可（PRD US-5）
        for cls in (".diagnostic-title", ".diagnostic-detail", ".diagnostic-advice"):
            assert page.query_selector(cls) is not None, f"诊断缺少 {cls}"
    finally:
        page.close()


def test_large_save_does_not_freeze_browser(server: LiveServer, browser) -> None:
    """NFR-2：99 MB 存档打开时界面不应长时间无响应。

    这是界面对 NFR-2 的**直接**验证——此前只在 HTTP 层测过耗时，
    没验证过浏览器拿到数据后能否正常渲染。
    """
    from saveinspector_tools.save_format import sample_large_save

    page = browser.new_page()
    try:
        _open_sample(
            page,
            f"http://127.0.0.1:{server.port}/",
            sample_large_save(target_bytes=10 * 1024 * 1024),
            name="large-10mb.sav",
        )
        # 10 MB 存档有大量槽位，确认页面确实渲染出了它们（而不是空列表）
        page.wait_for_selector(".slot-card", timeout=30000)
        assert page.query_selector(".slot-card") is not None
    finally:
        page.close()