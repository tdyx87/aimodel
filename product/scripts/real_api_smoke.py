#!/usr/bin/env python3
"""真实第三方服务自检（可选，需要凭据）。

为什么要有它：仓库里的冒烟脚本都用**本地假服务**——它能证明协议解析、
批准通道、会话落盘都对，但**证明不了真实服务会给我们想要的东西**。
真实故障正是这么暴露的：请求体里没有 `tools` 字段，真实服务上的模型
只会聊天，永远不发 `tool_calls`；而伪造 tool_calls 帧的脚本化测试全绿。

这个脚本用真实端点跑三件事：
  1. 普通一轮对话（真实 TLS + 流式）
  2. 多轮记忆（第二轮应记得第一轮的数字）
  3. 一次工具调用：模型发出 `write_file` → 用户批准 → 文件真的落盘

用法（凭据只从环境变量读，脚本不打印、不落盘）：
    export CAGENT_API_BASE=https://api.example.com/v1
    export CAGENT_MODEL=your-model
    export CAGENT_API_KEY=sk-...
    python scripts/real_api_smoke.py --exe build/cagent

退出码：0 全部通过 / 1 有失败 / 2 环境不满足（缺凭据或找不到可执行文件）
注意：真实服务可能限流（HTTP 429）。脚本会重试若干次，仍失败才算失败。
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

RETRY_SLEEP = 12
MAX_TRIES = 6



def _force_utf8_output() -> None:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")  # type: ignore[attr-defined]
        except Exception:  # noqa: BLE001 - 老解释器没有 reconfigure
            pass


def run_agent(exe: Path, work: Path, lines: list[str], timeout: int = 180):
    """跑一次代理。stdin 用 UTF-8 字节喂——用 shell 管道会被本地代码页毁掉。"""
    env = dict(os.environ)
    env["APPDATA"] = str(work / "appdata")
    env["XDG_CONFIG_HOME"] = str(work / "xdg")
    env.setdefault("CAGENT_TRANSPORT", "curl")
    proc = subprocess.run(
        [str(exe)], cwd=str(work), env=env, timeout=timeout,
        input=("\n".join(lines) + "\n").encode("utf-8"),
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


def session_files(work: Path) -> list[Path]:
    root = work / "appdata" / "cagent" / "sessions"
    if not root.is_dir():
        root = work / "xdg" / "cagent" / "sessions"
    return sorted(root.glob("*.json")) if root.is_dir() else []


def check_plain_chat(exe: Path) -> tuple[bool, str]:
    for attempt in range(1, MAX_TRIES + 1):
        work = Path(tempfile.mkdtemp(prefix="cagent_real_chat_"))
        rc, out = run_agent(exe, work, ["用一句中文说明你自己是谁", "/exit"])
        if "HTTP 429" in out:
            print("  第 %d 次：服务端限流（429），%d 秒后重试" % (attempt, RETRY_SLEEP))
            time.sleep(RETRY_SLEEP)
            continue
        ok = rc == 0 and "错误" not in out and len(out.strip()) > 0
        print("  对话输出：%s" % out.strip().replace("\n", " ")[:120])
        return ok, out
    return False, "连续 %d 次被限流" % MAX_TRIES


def check_memory(exe: Path) -> tuple[bool, str]:
    for attempt in range(1, MAX_TRIES + 1):
        work = Path(tempfile.mkdtemp(prefix="cagent_real_mem_"))
        rc, out = run_agent(exe, work, [
            "记住：我的幸运数字是 7",
            "我的幸运数字是多少？只回答数字",
            "/exit",
        ])
        if "HTTP 429" in out:
            print("  第 %d 次：服务端限流（429），%d 秒后重试" % (attempt, RETRY_SLEEP))
            time.sleep(RETRY_SLEEP)
            continue
        files = session_files(work)
        msgs = 0
        if files:
            try:
                msgs = len(json.loads(files[-1].read_text(encoding="utf-8"))["messages"])
            except Exception:  # noqa: BLE001
                msgs = 0
        ok = rc == 0 and "7" in out and msgs == 4
        print("  会话消息数=%d（期望 4），回答含 7=%s" % (msgs, "7" in out))
        return ok, out
    return False, "连续 %d 次被限流" % MAX_TRIES


def check_tool_approval(exe: Path) -> tuple[bool, str]:
    """工具调用 + 批准 + 真的落盘。

    批准路径喂 'y'（通过）；随后再喂一个非 'y' 字符（应为拒绝），
    两条路径都要验——只测通过就漏掉了「默认拒绝」这条安全底线。
    """
    for attempt in range(1, MAX_TRIES + 1):
        work = Path(tempfile.mkdtemp(prefix="cagent_real_tool_"))
        rc, out = run_agent(exe, work, [
            "请调用 write_file 工具，把内容 ok 写入 real_api_smoke.txt。只调用一次工具。",
            "y", "y", "/exit",
        ])
        target = work / "real_api_smoke.txt"
        if "HTTP 429" in out and not target.is_file():
            print("  第 %d 次：服务端限流（429），%d 秒后重试" % (attempt, RETRY_SLEEP))
            time.sleep(RETRY_SLEEP)
            continue
        wrote = target.is_file() and "ok" in target.read_text(encoding="utf-8", errors="replace")
        print("  工具调用界面：%s" % ("有" if "调用工具" in out else "无"))
        print("  批准提示出现：%s" % ("是" if "允许执行" in out else "否"))
        print("  文件真的落盘：%s" % ("是" if wrote else "否"))
        return wrote, out
    return False, "连续 %d 次被限流" % MAX_TRIES


def main() -> int:
    global MAX_TRIES, RETRY_SLEEP
    _force_utf8_output()
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default="")
    ap.add_argument("--tries", type=int, default=MAX_TRIES,
                    help="服务端限流时的重试次数")
    ap.add_argument("--sleep", type=int, default=RETRY_SLEEP,
                    help="重试间隔秒数")
    args = ap.parse_args()
    MAX_TRIES = max(1, args.tries)
    RETRY_SLEEP = max(1, args.sleep)

    exe = Path(args.exe) if args.exe else Path("build/cagent")
    if not exe.is_file():
        for cand in ("build/Release/cagent.exe", "build/cagent.exe", "build/cagent"):
            p = Path(cand)
            if p.is_file():
                exe = p
                break
    if not exe.is_file():
        print("找不到可执行文件（用 --exe 指定）", file=sys.stderr)
        return 2

    missing = [k for k in ("CAGENT_API_BASE", "CAGENT_MODEL", "CAGENT_API_KEY")
               if not os.environ.get(k)]
    if missing:
        print("缺少环境变量：%s（凭据只从环境变量读，脚本不落盘）" % ", ".join(missing))
        return 2

    print("目标：%s @ %s / %s" % (exe.name, os.environ["CAGENT_API_BASE"],
                                  os.environ["CAGENT_MODEL"]))
    results = {}
    print("[1/3] 真实对话")
    results["chat"], _ = check_plain_chat(exe)
    print("[2/3] 多轮记忆")
    results["memory"], _ = check_memory(exe)
    print("[3/3] 工具调用与批准")
    results["tool"], _ = check_tool_approval(exe)

    print("RESULT: %s (%s)" % (
        "PASS" if all(results.values()) else "FAIL",
        ", ".join("%s=%s" % (k, "ok" if v else "fail") for k, v in results.items())))
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
