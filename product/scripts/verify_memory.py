#!/usr/bin/env python3
"""独立复核 cagent 的内存承诺（PRD NFR-1）。

角色：test-engineer

为什么用独立脚本而不是产品自报：
    产品内置的内存查询服务于**用户**（我想知道我用了多少），
    验收脚本服务于**门禁**（你承诺了多少）。
    两者若是同一份代码，就是自己给自己打分——那不是验收。

因此本脚本**完全不使用 cagent 的任何代码**，只用操作系统接口读 RSS：
    Windows: ctypes 调 kernel32 的 GetProcessMemoryInfo
    POSIX  : 读 /proc/<pid>/statm（无 /proc 时回退 psutil 的 ps）

用法:
    python verify_memory.py --exe <可执行文件> [--budget-mb N] [--samples 5]

退出码:
    0 通过 / 1 超预算或进程无法测量
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

IS_WINDOWS = os.name == "nt"


def rss_bytes(pid: int) -> int:
    """读指定进程的 RSS（字节）。只走操作系统接口。"""
    if IS_WINDOWS:
        import ctypes
        from ctypes import wintypes

        class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
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

        psapi = ctypes.WinDLL("psapi")
        kernel32 = ctypes.WinDLL("kernel32")

        psapi.GetProcessMemoryInfo.argtypes = [
            wintypes.HANDLE,
            ctypes.POINTER(PROCESS_MEMORY_COUNTERS),
            wintypes.DWORD,
        ]
        psapi.GetProcessMemoryInfo.restype = wintypes.BOOL
        kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel32.OpenProcess.restype = wintypes.HANDLE
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel32.CloseHandle.restype = wintypes.BOOL

        # 必须 OpenProcess：GetCurrentProcess 的伪句柄只对**自身**有效，
        # 而这里要读的是子进程的 RSS。
        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        PROCESS_VM_READ = 0x0010
        handle = kernel32.OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, False, pid
        )
        if not handle:
            raise OSError(f"OpenProcess({pid}) 失败，权限不足或进程已退出")

        counters = PROCESS_MEMORY_COUNTERS()
        counters.cb = ctypes.sizeof(counters)
        try:
            ok = psapi.GetProcessMemoryInfo(
                handle, ctypes.byref(counters), wintypes.DWORD(counters.cb)
            )
        finally:
            kernel32.CloseHandle(handle)
        if not ok:
            raise OSError("GetProcessMemoryInfo 失败")
        return int(counters.WorkingSetSize)

    statm = Path(f"/proc/{pid}/statm")
    if statm.is_file():
        fields = statm.read_text().split()
        return int(fields[1]) * os.sysconf("SC_PAGE_SIZE")

    # macOS / BSD **没有 /proc**。`ps -o rss=` 在两者上都可用，单位 KB。
    #
    # 早先的文件头写着「没有 /proc 时会退回 psutil 或 ps」，
    # 而实现里**根本没有这个分支**——macOS 上内存门禁直接不可用，
    # 且报错信息还写得很像那么回事（「该平台无法读取 /proc/<pid>/statm」）。
    #
    # 这是第四次「覆盖幻觉」：文档/注释声称覆盖了什么，代码里没有。
    #   1. Windows 超时用例用 sleep（cmd 里没有），目标分支一次没跑到
    #   2. 告警门禁只匹配 MSVC 格式，GCC 的 12 条全漏检
    #   3. 符号链接用例把链接建在 CWD，防护从没被真正测过
    #   4. 这一处：注释承诺的降级路径不存在
    #
    # 教训：**验证机制本身的覆盖范围，也必须被验证。**
    # 一条从未被执行过的降级路径，等于没有。
    try:
        out = subprocess.run(
            ["ps", "-o", "rss=", "-p", str(pid)],
            capture_output=True, text=True, timeout=5,
        )
        if out.returncode == 0 and out.stdout.strip():
            return int(out.stdout.strip()) * 1024
    except (OSError, subprocess.SubprocessError, ValueError):
        pass
    raise OSError("无法读取 RSS：/proc 不可用，且 ps 取值失败")


def _force_utf8_output() -> None:
    """Windows 控制台默认是 cp1252，打印中文会抛 UnicodeEncodeError。

    早先没做这件事，于是内存门禁在 Windows 上**根本没跑起来**——
    脚本在第一行 print 就崩了，报告的是 traceback 而不是内存数字。
    """
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError, OSError):
            pass


def main() -> int:
    _force_utf8_output()
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True, help="可执行文件路径")
    ap.add_argument("--budget-mb", type=float, default=8.0, help="RSS 预算（MB）")
    ap.add_argument("--samples", type=int, default=5, help="采样次数")
    ap.add_argument("--settle", type=float, default=0.5, help="每次采样间隔（秒）")
    args = ap.parse_args()

    exe = Path(args.exe)
    if not exe.is_file():
        print(f"找不到可执行文件: {exe}", file=sys.stderr)
        return 1

    proc = subprocess.Popen([str(exe)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    samples = []
    try:
        for i in range(args.samples):
            time.sleep(args.settle)
            if proc.poll() is not None:
                break
            try:
                samples.append(rss_bytes(proc.pid))
            except OSError as exc:
                print(f"读取 RSS 失败: {exc}", file=sys.stderr)
                return 1
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.wait(timeout=10)

    if not samples:
        print("未取到任何样本（进程可能立即退出）", file=sys.stderr)
        return 1

    peak = max(samples)
    avg = sum(samples) / len(samples)
    budget = args.budget_mb * 1024 * 1024

    print(f"进程: {exe.name}")
    print(f"  样本数   : {len(samples)}")
    print(f"  平均 RSS : {avg / 1048576:.2f} MB")
    print(f"  峰值 RSS : {peak / 1048576:.2f} MB")
    print(f"  预算     : {args.budget_mb:.2f} MB")

    if peak > budget:
        print(f"  结论     : 超预算 {peak / 1048576 - args.budget_mb:.2f} MB")
        return 1
    print(f"  结论     : 达标（余量 {args.budget_mb - peak / 1048576:.2f} MB）")
    return 0


if __name__ == "__main__":
    sys.exit(main())