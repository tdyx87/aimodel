#!/usr/bin/env python3
"""用**真实 libcurl** 打一个本地 SSE 服务，验证传输层。

为什么需要这个东西：`transport_curl.c` 在 CI 上只被**编译**，从没被**运行**过。
编译通过证明不了它能在线上发一个请求、按块收回来、并正确传递 Authorization。

所以这里不做 mock：起真 socket、发真 POST、读真流式响应。
唯一的「假」是服务端在本地 —— 那正是为了让结果可重复。

用法：
    python scripts/curl_smoke.py build-curl/Release/cagent.exe

退出码 0 = 全部断言通过。任何一项不满足都返回 1，并打印实际值。
"""

import os
import socket
import subprocess
import sys
import tempfile
import threading
import time

KEY = "sk-smoke-test-123"
MARKER = "curl-smoke-ok"
TOKENS = ["流式", "回复", "-", MARKER]


def free_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Server(threading.Thread):
    def __init__(self, port: int):
        super().__init__(daemon=True)
        self.port = port
        self.records = []
        self.ready = threading.Event()
        self._sock = None

    def run(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("127.0.0.1", self.port))
        s.listen(2)
        self._sock = s
        self.ready.set()
        conn, _ = s.accept()
        try:
            self._handle(conn)
        finally:
            conn.close()
            s.close()

    def _handle(self, conn):
        conn.settimeout(10)
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = conn.recv(4096)
            if not chunk:
                return
            data += chunk
        head, _, rest = data.partition(b"\r\n\r\n")
        lines = head.decode("latin-1").split("\r\n")
        headers = {}
        for line in lines[1:]:
            k, _, v = line.partition(":")
            headers[k.strip().lower()] = v.strip()
        need = int(headers.get("content-length", "0"))
        while len(rest) < need:
            chunk = conn.recv(4096)
            if not chunk:
                break
            rest += chunk

        self.records.append({
            "request_line": lines[0],
            "authorization": headers.get("authorization", "<none>"),
            "content_type": headers.get("content-type", "<none>"),
            "body_len": len(rest),
        })

        conn.sendall(b"HTTP/1.1 200 OK\r\n"
                     b"Content-Type: text/event-stream\r\n"
                     b"Connection: close\r\n\r\n")
        # 分多次写、每次之间停顿：这样「是否真的逐块到达」才可观测。
        for tok in TOKENS:
            payload = ('data: {"choices":[{"delta":{"content":"%s"}}]}\n\n' % tok)
            conn.sendall(payload.encode("utf-8"))
            time.sleep(0.12)
        conn.sendall(b"data: [DONE]\n\n")

    def stop(self):
        try:
            if self._sock:
                self._sock.close()
        except OSError:
            pass


def run(exe: str) -> int:
    port = free_port()
    server = Server(port)
    server.start()
    if not server.ready.wait(5):
        print("FAIL: 本地服务未能启动")
        return 1

    env = dict(os.environ)
    env.update({
        "CAGENT_API_BASE": "http://127.0.0.1:%d/v1" % port,
        "CAGENT_MODEL": "smoke-model",
        "CAGENT_API_KEY": KEY,
    })
    env.pop("CAGENT_TRANSPORT", None)  # 必须是真实后端

    with tempfile.TemporaryDirectory() as tmp:
        out_path = os.path.join(tmp, "out.txt")
        with open(out_path, "wb") as out, open(os.devnull, "rb") as null_in:
            proc = subprocess.Popen([exe], stdin=subprocess.PIPE,
                                    stdout=out, stderr=subprocess.STDOUT, env=env)
            proc.stdin.write(b"hello\n/exit\n")
            proc.stdin.flush()
            proc.stdin.close()

            # 边跑边采样输出长度：若实现是「攒完再吐」，中途长度恒为 0。
            sizes = []
            deadline = time.time() + 20
            while proc.poll() is None and time.time() < deadline:
                time.sleep(0.15)
                try:
                    sizes.append(os.path.getsize(out_path))
                except OSError:
                    sizes.append(0)
            proc.wait(timeout=10)

        with open(out_path, "rb") as f:
            text = f.read().decode("utf-8", "replace")

    server.stop()
    failures = []

    if proc.returncode != 0:
        failures.append("退出码 %s（期望 0）" % proc.returncode)

    if not server.records:
        failures.append("服务端没收到任何请求")
    else:
        rec = server.records[0]
        if "/chat/completions" not in rec["request_line"]:
            failures.append("请求路径不对: %s" % rec["request_line"])
        if rec["authorization"] != "Bearer " + KEY:
            failures.append("Authorization 不对: %s" % rec["authorization"])
        if "application/json" not in rec["content_type"]:
            failures.append("Content-Type 不对: %s" % rec["content_type"])
        if rec["body_len"] <= 0:
            failures.append("请求体为空")

    if MARKER not in text:
        failures.append("回复内容缺失（期望含 %r）" % MARKER)

    # 渐进输出：至少有两个采样点长度在增长，且最终大于中途。
    grew = sum(1 for a, b in zip(sizes, sizes[1:]) if b > a)
    if grew < 2:
        failures.append("输出不是渐进式的（增长采样点 %d 个）: %s" % (grew, sizes))

    if failures:
        print("FAIL")
        for f in failures:
            print("  - " + f)
        print("--- 程序输出 ---")
        print(text)
        return 1

    print("PASS")
    print("  服务端收到: %s" % server.records[0]["request_line"])
    print("  鉴权头:     %s" % server.records[0]["authorization"])
    print("  输出增长采样: %s" % sizes)
    return 0


def main() -> int:
    # Windows 控制台默认是 cp1252/cp936，打印中文会抛 UnicodeEncodeError，
    # 于是「测试通过」这件事本身会把 CI 弄红（在 Windows runner 上真实发生过）。
    # 换成 UTF-8 并把无法编码的字符降级，诊断信息永远打得出来。
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, OSError):
            pass

    if len(sys.argv) != 2:
        print("用法: python scripts/curl_smoke.py <cagent 可执行文件>")
        return 2
    exe = sys.argv[1]
    if not os.path.isfile(exe):
        print("找不到可执行文件: %s" % exe)
        return 2
    return run(exe)


if __name__ == "__main__":
    sys.exit(main())
