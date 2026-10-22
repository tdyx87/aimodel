"""Web UI 冒烟：启动 -> 取页面 -> 发消息 -> 收事件 -> 批准 -> 验证。

只用标准库（不依赖第三方），CI 上也能跑。
"""
import http.client
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

EXE = Path(sys.argv[1] if len(sys.argv) > 1
           else r"E:\workspace\aimodel\product\build-curl\Release\cagent.exe")
# **必须转成绝对路径**：子进程用 cwd=<临时目录> 启动，
# 而 POSIX 下相对的程序路径是按**新 cwd** 解析的 → 找不到可执行文件。
# Windows 不报（CreateProcess 按父进程 cwd 解析），所以这个 bug 在 Windows 上隐形。
EXE = EXE.resolve()
PORT = int(os.environ.get("WEB_PORT", "0"))



def _force_utf8_output() -> None:
    """把 stdout/stderr 切成 UTF-8。

    Windows CI 的 stdout 是 cp1252，打印中文会直接
    UnicodeEncodeError —— 脚本挂掉，看起来像「功能坏了」，
    其实是控制台编码问题。
    """
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:  # noqa: BLE001 - 老解释器没有 reconfigure
            pass

def wait_port(proc, timeout=20):
    """从子进程 stdout 里读出 URL（含端口与 token）。"""
    deadline = time.time() + timeout
    buf = ""
    while time.time() < deadline:
        raw = proc.stdout.readline()
        if not raw:
            break
        line = raw.decode("utf-8", "replace")
        buf += line
        m = re.search(r"http://127\.0\.0\.1:(\d+)/\?t=([0-9a-f]+)", buf)
        if m:
            return int(m.group(1)), m.group(2)
    return None, None


def main():
    _force_utf8_output()
    work = Path(tempfile.mkdtemp(prefix="web_smoke_"))
    env = dict(os.environ)
    env["APPDATA"] = str(work / "appdata")
    env["CAGENT_API_BASE"] = "http://127.0.0.1:9/v1"   # 故意指向不通的地址
    env["CAGENT_MODEL"] = "smoke-model"
    env["CAGENT_TRANSPORT"] = "mem"                   # 离线自检：不联网
    env.pop("CAGENT_API_KEY", None)

    args = [str(EXE), "--web"]
    if PORT:
        args += ["--web-port", str(PORT)]
    proc = subprocess.Popen(args, cwd=str(work), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        port, token = wait_port(proc)
        if not port:
            print("FAIL: 没拿到 URL/端口")
            print(proc.stdout.read(2000) if proc.stdout else "")
            return 1
        print("  端口=%d token 长度=%d" % (port, len(token)))

        # 1) 页面可取
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        conn.request("GET", "/")
        r = conn.getresponse()
        body = r.read().decode("utf-8", "replace")
        ok_page = (r.status == 200 and "cagent" in body and "EventSource" in body)
        print("  页面: status=%d 长度=%d -> %s" % (r.status, len(body), ok_page))
        conn.close()

        # 2) 无 token 的 API 必须 401
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        conn.request("GET", "/api/info")
        r = conn.getresponse()
        r.read()
        ok_auth = (r.status == 401)
        print("  无 token 访问 /api/info: %d -> %s" % (r.status, ok_auth))
        conn.close()

        # 3) 带 token 可以访问
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        conn.request("GET", "/api/info", headers={"X-Cagent-Token": token,
                                                  "Host": "127.0.0.1:%d" % port})
        r = conn.getresponse()
        r.read()
        ok_tok = (r.status == 200)
        print("  带 token 访问 /api/info: %d -> %s" % (r.status, ok_tok))
        conn.close()

        # 4) 发消息 → 代理应当处理（mem 后端会回一句预设）
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        payload = json.dumps({"text": "你好"}).encode("utf-8")
        conn.request("POST", "/api/send", body=payload,
                     headers={"Content-Type": "application/json",
                              "X-Cagent-Token": token,
                              "Host": "127.0.0.1:%d" % port})
        r = conn.getresponse()
        r.read()
        ok_send = (r.status == 200)
        print("  POST /api/send: %d -> %s" % (r.status, ok_send))
        conn.close()

        # 5) SSE 能收到事件
        s = socket.create_connection(("127.0.0.1", port), timeout=10)
        s.sendall(("GET /api/events?t=%s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n\r\n"
                   % (token, port)).encode())
        time.sleep(0.5)
        s.sendall(json.dumps({"text": "再来一句"}).encode() and
                  ("POST /api/send HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
                   "Content-Type: application/json\r\nX-Cagent-Token: %s\r\n"
                   "Content-Length: %d\r\n\r\n%s"
                   % (port, token, len(payload), payload)).encode())
        s.settimeout(5)
        got = b""
        deadline = time.time() + 5
        while time.time() < deadline and b"event:" not in got:
            try:
                chunk = s.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            got += chunk
        ok_sse = b"event:" in got
        print("  SSE 收到事件: %s" % ok_sse)
        s.close()

        results = [ok_page, ok_auth, ok_tok, ok_send, ok_sse]
        print("RESULT: %s" % ("PASS" if all(results) else "FAIL"))
        return 0 if all(results) else 1
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
