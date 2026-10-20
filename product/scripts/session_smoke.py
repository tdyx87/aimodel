#!/usr/bin/env python3
"""会话命令的离线端到端冒烟（US-6）。

单测证明的是函数，证明不了「命令行真的能存下来、下次还找得到」。
这个脚本用**真实可执行文件 + 真实子进程**验证那条链：

  一轮对话 → 自动落盘 → 换个进程 /list 还能看到 → /resume 把历史带回来
           → /delete 之后真的没了 → /new 开新会话 → 坏 id 不崩不误删

不联网：CAGENT_TRANSPORT=mem 让传输层走预设 SSE。
会话目录被重定向到临时目录，绝不碰用户真实的配置目录。

用法：python3 scripts/session_smoke.py <cagent 可执行文件>
"""
import os
import re
import subprocess
import sys
import tempfile

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

FAILS = []


def check(cond, label):
    print(("  ok    " if cond else "  FAIL  ") + label)
    if not cond:
        FAILS.append(label)


def main(argv):
    if len(argv) < 2:
        print("用法：session_smoke.py <cagent 可执行文件>")
        return 2
    exe = os.path.abspath(argv[1])
    if not os.path.isfile(exe):
        print("找不到可执行文件: %s" % exe)
        return 2

    workdir = tempfile.mkdtemp(prefix="cagent_sess_smoke_")
    env = dict(os.environ)
    env["CAGENT_API_BASE"] = "http://127.0.0.1:1/v1"  # 不该被访问
    env["CAGENT_MODEL"] = "dummy"
    env["CAGENT_TRANSPORT"] = "mem"
    # 重定向配置目录：Windows 认 APPDATA，POSIX 认 XDG_CONFIG_HOME。
    # 两个都设，脚本就与平台无关。
    env["APPDATA"] = workdir
    env["XDG_CONFIG_HOME"] = workdir
    env.pop("CAGENT_API_KEY", None)

    def run(text):
        p = subprocess.run([exe], input=text.encode("utf-8"),
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           env=env, timeout=120)
        return p.returncode, p.stdout.decode("utf-8", errors="replace")

    print("[1] one turn, then /list shows one session")
    rc, out = run("first question\n/list\n/exit\n")
    check(rc == 0, "exit code 0")
    check("2 条消息" in out, "list shows 2 messages (question+answer)")
    check("还没有历史会话" not in out, "list is not empty")
    ids = re.findall(r"^\s+(s[0-9a-f]+)\s+\d+ 条消息", out, re.M)
    check(len(ids) == 1, "exactly one session id found")
    sid = ids[0] if ids else "missing"

    print("[2] a fresh process still sees it (it really hit the disk)")
    rc, out = run("/list\n/exit\n")
    check(sid in out, "id survives a new process")

    print("[3] /resume brings the history back")
    rc, out = run("/resume %s\n/list\n/exit\n" % sid)
    check("已恢复会话" in out, "resume reported success")
    check("2 条消息" in out, "history is 2 messages after resume")

    print("[4] /resume with a bad id neither crashes nor deletes")
    rc, out = run("/resume no-such-session\n/list\n/exit\n")
    check(rc == 0, "exit code 0")
    check("无法恢复" in out, "clear failure message")
    check(sid in out, "existing session untouched")

    print("[5] /delete really removes it")
    rc, out = run("/delete %s\n/list\n/exit\n" % sid)
    check("已删除会话" in out, "delete reported success")
    check("还没有历史会话" in out, "list is empty after delete")

    print("[6] /new starts a fresh session")
    rc, out = run("first question\n/new\nsecond question\n/exit\n")
    check("已开始新会话" in out, "new-session message shown")

    print("[7] the exit hint names the session so it can be resumed")
    rc, out = run("a question\n/exit\n")
    check("本次会话已保存" in out, "exit prints resume hint")

    if FAILS:
        print("FAILED: " + "; ".join(FAILS))
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
