"""工具批准通道冒烟：真的 cagent + 真的 libcurl + 本地 SSE 服务。

单元测试证明的是 core 函数的行为；这里证明**用户实际拿到的东西**能用：
  1. 模型要求写文件 → 用户输入 y → 文件真的按内容落盘；
  2. 模型要求写文件 → 用户输入 n → 文件不出现；
  3. 两种情况下，界面都完整显示「要写哪个文件」（契约 P5，不能是黑盒）。

用法：python scripts/tool_smoke.py [path/to/cagent]
"""
import json
import os
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ROUNDS = {"n": 0}


def find_exe():
    if len(sys.argv) > 1:
        return Path(sys.argv[1]).resolve()
    for cand in ("build/Release/cagent.exe", "build/cagent.exe", "build/cagent"):
        p = ROOT / cand
        if p.is_file():
            return p
    return None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        ROUNDS["n"] += 1
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        if ROUNDS["n"] == 1:
            call = {"choices": [{"delta": {"tool_calls": [{
                "index": 0, "id": "c1",
                "function": {"name": "write_file",
                             "arguments": json.dumps({"path": "smoke.txt",
                                                      "content": "hello-smoke"})},
            }]}}]}
            self.wfile.write(("data: " + json.dumps(call) + "\n\n").encode())
        else:
            # 第二轮必须带着工具结果，否则「回灌」这一环没被验到。
            assert b"hello-smoke" in body or b"status" in body
            text = {"choices": [{"delta": {"content": "done"}}]}
            self.wfile.write(("data: " + json.dumps(text) + "\n\n").encode())
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()


def run_case(exe, port, answer):
    work = Path(tempfile.mkdtemp(prefix="cagent_tool_"))
    env = dict(os.environ)
    env.update({
        "CAGENT_API_BASE": "http://127.0.0.1:%d/v1" % port,
        "CAGENT_MODEL": "smoke-model",
        "CAGENT_TRANSPORT": "curl",
        "APPDATA": str(work / "appdata"),
        "XDG_CONFIG_HOME": str(work / "xdg"),
    })
    # 冒烟不该受本机环境影响：不设 key，也不开 shell。
    for k in ("CAGENT_API_KEY", "CAGENT_ALLOW_SHELL", "CAGENT_SYSTEM"):
        env.pop(k, None)
    proc = subprocess.run([str(exe)], cwd=str(work), env=env,
                          input=("write it\n" + answer + "\n/exit\n").encode(),
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=120)
    return work, proc.stdout, proc.returncode


def main():
    exe = find_exe()
    if exe is None or not exe.is_file():
        print("FAIL: cagent executable not found")
        return 1

    srv = HTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    port = srv.server_address[1]

    fails = []

    def check(cond, label):
        print(("  PASS  " if cond else "  FAIL  ") + label)
        if not cond:
            fails.append(label)

    ROUNDS["n"] = 0
    work, out, rc = run_case(exe, port, "y")
    target = work / "smoke.txt"
    check(rc == 0, "approved run exits 0 (got %d)" % rc)
    check(target.is_file(), "approved: file exists")
    if target.is_file():
        check(target.read_bytes() == b"hello-smoke", "approved: content matches")
    check(b"write_file" in out, "approved: tool call visible")
    check(b"smoke.txt" in out, "approved: pending path visible")
    check(ROUNDS["n"] >= 2, "approved: tool result fed back (rounds=%d)" % ROUNDS["n"])

    ROUNDS["n"] = 0
    work2, out2, rc2 = run_case(exe, port, "n")
    check(rc2 == 0, "denied run exits 0 (got %d)" % rc2)
    check(not (work2 / "smoke.txt").exists(), "denied: file not created")
    check(b"smoke.txt" in out2, "denied: pending path still shown")

    srv.shutdown()
    if fails:
        print("RESULT: FAIL (%d)" % len(fails))
        return 1
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
