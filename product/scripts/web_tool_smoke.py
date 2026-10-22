#!/usr/bin/env python3
"""Web UI 冒烟：页面、鉴权、SSE 事件、**浏览器点批准 → 工具真的执行**。

为什么要本地脚本化服务：真实服务会限流（429），用它验证「批准 → 落盘」
就是拿 flaky 当门禁。这里用本地 SSE 服务**确定性地**回一个 tool_calls，
验证的是我们的链路：浏览器批准 → 代理执行 → 文件真的出现。

用法：python scripts/web_tool_smoke.py [path/to/cagent]
"""
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

ROUNDS = {"n": 0}


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
                             "arguments": json.dumps({"path": "web_smoke.txt",
                                                      "content": "hello-web"})},
            }]}}]}
            self.wfile.write(("data: " + json.dumps(call) + "\n\n").encode())
        else:
            text = {"choices": [{"delta": {"content": "done"}}]}
            self.wfile.write(("data: " + json.dumps(text) + "\n\n").encode())
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()


def main():
    exe = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("build/cagent")
    if not exe.is_file():
        print("FAIL: cagent executable not found")
        return 1

    srv = HTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    api_port = srv.server_address[1]

    work = Path(tempfile.mkdtemp(prefix="cagent_web_"))
    env = dict(os.environ)
    env.update({
        "CAGENT_API_BASE": "http://127.0.0.1:%d/v1" % api_port,
        "CAGENT_MODEL": "smoke-model",
        "CAGENT_TRANSPORT": "curl",
        "APPDATA": str(work / "appdata"),
        "XDG_CONFIG_HOME": str(work / "xdg"),
    })
    for k in ("CAGENT_API_KEY", "CAGENT_ALLOW_SHELL", "CAGENT_SYSTEM"):
        env.pop(k, None)

    proc = subprocess.Popen([str(exe), "--web"], cwd=str(work), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    console = []
    threading.Thread(target=lambda: [console.append(l.decode("utf-8", "replace"))
                                     for l in proc.stdout], daemon=True).start()

    url = ""
    deadline = time.time() + 20
    while time.time() < deadline:
        m = re.search(r"http://127\.0\.0\.1:(\d+)/\?t=([0-9a-f]+)", "".join(console))
        if m:
            url = m.group(0)
            web_port = int(m.group(1))
            token = m.group(2)
            break
        time.sleep(0.2)
    if not url:
        print("FAIL: 没拿到 Web UI 地址")
        print("".join(console))
        proc.terminate()
        return 1
    print("  Web UI: %s" % url)

    events = []

    def sse():
        s = socket.create_connection(("127.0.0.1", web_port), timeout=20)
        s.sendall(("GET /api/events?t=%s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n\r\n"
                   % (token, web_port)).encode())
        try:
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                events.append(chunk.decode("utf-8", "replace"))
        except Exception:
            pass
        finally:
            s.close()

    threading.Thread(target=sse, daemon=True).start()
    time.sleep(0.5)

    def api(path, method="GET", obj=None):
        import http.client
        conn = http.client.HTTPConnection("127.0.0.1", web_port, timeout=30)
        body = json.dumps(obj).encode("utf-8") if obj is not None else None
        headers = {"X-Cagent-Token": token, "Host": "127.0.0.1:%d" % web_port}
        if body:
            headers["Content-Type"] = "application/json"
        conn.request(method, path, body=body, headers=headers)
        r = conn.getresponse()
        data = r.read()
        conn.close()
        return r.status, data.decode("utf-8", "replace")

    fails = []

    def check(cond, label):
        print("  %-34s %s" % (label, "OK" if cond else "FAIL"))
        if not cond:
            fails.append(label)

    st, _ = api("/api/send", "POST", {"text": "写个文件"})
    check(st == 200, "POST /api/send 返回 200")

    # 等批准事件
    got_approval = False
    deadline = time.time() + 30
    while time.time() < deadline:
        if "approval" in "".join(events):
            got_approval = True
            break
        time.sleep(0.3)
    check(got_approval, "浏览器收到批准请求事件")

    st, _ = api("/api/approve", "POST", {"decision": "allow"})
    check(st == 200, "POST /api/approve 返回 200")

    wrote = False
    deadline = time.time() + 30
    while time.time() < deadline:
        if (work / "web_smoke.txt").is_file():
            wrote = True
            break
        time.sleep(0.3)
    check(wrote, "批准后文件真的落盘")
    if wrote:
        content = (work / "web_smoke.txt").read_text(encoding="utf-8")
        check(content == "hello-web", "落盘内容正确")

    check("done" in "".join(events) or "tool" in "".join(events),
          "SSE 里有工具/结束事件")

    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()

    print("RESULT: %s" % ("PASS" if not fails else "FAIL %s" % fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
