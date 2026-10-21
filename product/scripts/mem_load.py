#!/usr/bin/env python3
"""活跃会话内存压测：500 轮对话后 RSS 是否仍低于 100 MB（PRD NFR-1）。

为什么不能只测空闲基线：
    `verify_memory.py` 只证明「开着不干活时很轻」。用户真正会遇到的场景是
    **一次会话越聊越长**——消息历史、会话文件、流式缓冲都随轮次增长。
    只有把轮次打上去，才能证明「低内存」不是启动瞬间的假象。

为什么用真实二进制而不是链接内存模块：
    NFR-1 是端到端承诺（libcurl + 会话 + UI + core 一起跑），
    只测某一层等于挑选最容易达标的那个环境。

用法：
    python scripts/mem_load.py --exe build/Release/cagent --turns 500

退出码：0 达标 / 1 超预算或环境不满足
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from verify_memory import _force_utf8_output, rss_bytes  # noqa: E402

REQUESTS = {"n": 0}
LOCK = threading.Lock()

# 每轮回复固定长度：让「轮次」成为唯一变量，而不是回复大小。
REPLY = "这是本地压测服务返回的一段固定长度文本，用于制造流式解析与会话增长。" * 2


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        self.rfile.read(length)
        with LOCK:
            REQUESTS["n"] += 1
        body = ("data: " + json.dumps({"choices": [{"delta": {"content": REPLY}}]}) + "\n\n").encode()
        done = b"data: [DONE]\n\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(body) + len(done)))
        self.end_headers()
        self.wfile.write(body)
        self.wfile.write(done)
        self.wfile.flush()


def os_peak_bytes(pid: int) -> int | None:
    """系统的「历史峰值」：比抽样更可靠（抽样可能错过瞬时高点）。"""
    try:
        if os.name == "nt":
            import ctypes
            from ctypes import wintypes

            class PMC(ctypes.Structure):
                _fields_ = [
                    ("cb", wintypes.DWORD),
                    ("PageFaultCount", wintypes.DWORD),
                    ("PeakWorkingSetSize", ctypes.c_size_t),
                    ("WorkingSetSize", ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                    ("PagefileUsage", ctypes.c_size_t),
                    ("PeakPagefileUsage", ctypes.c_size_t),
                ]

            psapi, k32 = ctypes.WinDLL("psapi"), ctypes.WinDLL("kernel32")
            h = k32.OpenProcess(0x1000 | 0x0010, False, pid)
            if not h:
                return None
            c = PMC()
            c.cb = ctypes.sizeof(c)
            ok = psapi.GetProcessMemoryInfo(h, ctypes.byref(c), wintypes.DWORD(c.cb))
            k32.CloseHandle(h)
            return int(c.PeakWorkingSetSize) if ok else None

        status = Path(f"/proc/{pid}/status")
        if status.is_file():
            for line in status.read_text().splitlines():
                if line.startswith("VmHWM:"):
                    return int(line.split()[1]) * 1024
        return None
    except Exception:  # noqa: BLE001 - 峰值只是加分项，拿不到就退回抽样
        return None


def find_exe(arg: str | None) -> Path | None:
    if arg:
        p = Path(arg)
        return p if p.is_file() else None
    root = Path(__file__).resolve().parent.parent
    for cand in ("build/Release/cagent.exe", "build/cagent.exe", "build/cagent"):
        p = root / cand
        if p.is_file():
            return p
    return None


def main() -> int:
    _force_utf8_output()
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe")
    ap.add_argument("--turns", type=int, default=500)
    ap.add_argument("--budget-mb", type=float, default=100.0)
    ap.add_argument("--sample-ms", type=int, default=50)
    args = ap.parse_args()

    exe = find_exe(args.exe)
    if exe is None:
        print("找不到可执行文件（用 --exe 指定）", file=sys.stderr)
        return 1

    work = Path(tempfile.mkdtemp(prefix="cagent_memload_"))
    srv = HTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    port = srv.server_address[1]

    env = dict(os.environ)
    env.update({
        "CAGENT_API_BASE": "http://127.0.0.1:%d/v1" % port,
        "CAGENT_MODEL": "memload-model",
        "CAGENT_TRANSPORT": "curl",
        "APPDATA": str(work / "appdata"),
        "XDG_CONFIG_HOME": str(work / "xdg"),
    })
    # 压测不受本机环境干扰：没有 key、不开 shell。
    for k in ("CAGENT_API_KEY", "CAGENT_ALLOW_SHELL", "CAGENT_SYSTEM"):
        env.pop(k, None)

    proc = subprocess.Popen(
        [str(exe)], cwd=str(work), env=env,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )

    drained = {"bytes": 0}

    def drain():
        try:
            while True:
                chunk = proc.stdout.read(8192)
                if not chunk:
                    break
                drained["bytes"] += len(chunk)
        except (ValueError, OSError):
            pass

    threading.Thread(target=drain, daemon=True).start()

    samples: list[int] = []
    stop = threading.Event()

    def sample():
        while not stop.is_set():
            if proc.poll() is not None:
                return
            try:
                samples.append(rss_bytes(proc.pid))
            except OSError:
                return
            stop.wait(args.sample_ms / 1000.0)

    threading.Thread(target=sample, daemon=True).start()

    t0 = time.time()
    try:
        for i in range(1, args.turns + 1):
            proc.stdin.write(("第 %d 轮：说点什么\n" % i).encode())
            proc.stdin.flush()
        proc.stdin.write(b"/exit\n")
        proc.stdin.flush()
        proc.stdin.close()
        rc = proc.wait(timeout=max(120, args.turns))
    except subprocess.TimeoutExpired:
        proc.kill()
        print("FAIL: 超过 %d 秒仍未结束" % max(120, args.turns))
        return 1
    finally:
        stop.set()
        srv.shutdown()

    elapsed = time.time() - t0
    time.sleep(0.05)  # 让最后一次采样落地

    # 会话文件：证明这真的是一次「越聊越长」的会话，而不是 500 次空转。
    sess_root = work / "appdata" / "cagent" / "sessions"
    if not sess_root.is_dir():
        sess_root = work / "xdg" / "cagent" / "sessions"
    files = sorted(sess_root.glob("*")) if sess_root.is_dir() else []
    sess_bytes = files[-1].stat().st_size if files else 0

    budget = args.budget_mb * 1048576
    peak_sample = max(samples) if samples else 0
    peak_os = os_peak_bytes(proc.pid) if proc.returncode is not None else None
    peak = max(peak_sample, peak_os or 0)

    print("进程     : %s" % exe.name)
    print("  轮次     : %d（服务端收到 %d 次请求）" % (args.turns, REQUESTS["n"]))
    print("  耗时     : %.1f s" % elapsed)
    print("  输出量   : %.1f KB" % (drained["bytes"] / 1024.0))
    print("  会话文件 : %d 字节" % sess_bytes)
    print("  样本数   : %d" % len(samples))
    print("  峰值 RSS : %.2f MB%s" % (
        peak / 1048576.0, "（系统峰值）" if peak_os else "（抽样峰值）"))
    print("  预算     : %.2f MB" % args.budget_mb)

    fails = []
    if rc != 0:
        fails.append("进程退出码 %d" % rc)
    if REQUESTS["n"] < args.turns:
        fails.append("只完成 %d/%d 轮" % (REQUESTS["n"], args.turns))
    if sess_bytes <= 0:
        fails.append("会话文件为空（没有落到盘上）")
    if peak <= 0:
        fails.append("没取到 RSS 样本")
    elif peak > budget:
        fails.append("峰值超预算 %.2f MB" % ((peak - budget) / 1048576.0))

    if fails:
        for f in fails:
            print("  FAIL: " + f)
        print("RESULT: FAIL (%d)" % len(fails))
        return 1
    print("  结论     : 达标（余量 %.2f MB）" % ((budget - peak) / 1048576.0))
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
