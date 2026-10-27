#!/usr/bin/env python3
"""审计门禁：把「流程有没有真的发生」变成可机器检查的断言。

背景：角色的执行不能只靠提示词自觉。换一个模型，流程就可能被跳过，
而仓库里看不出任何异常。唯一可靠的痕迹是**审计文档**——每个角色完成
一次任务后必须落一份 `product/docs/audit/<日期>-<角色>-<任务>.md`。

两种模式：

    默认模式：校验历史审计的命名与角色是否合法（老账只查命名，不追溯内容）。
    --diff <ref>：要求「改了代码」的同时「新增了审计」，且新增审计必须含
                  角色/输入/产出/结论 四要素。CI 用这个模式。

用法：

    python scripts/check_gates.py
    python scripts/check_gates.py --diff origin/main
    python scripts/check_gates.py --require reviewer,test-engineer
"""

import argparse
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, ".role-system", "manifest.json")

DATE_RE = re.compile(r"^(\d{4})-(\d{2})-(\d{2})-(.+)$")

# 这些目录有改动，就必须有对应的审计。文档/审计自身的改动不触发。
CODE_PREFIXES = (
    "product/src/",
    "product/include/",
    "product/web/",
    "product/tests/",
    "product/scripts/",
    "product/tools/",
)

# 新增审计必须同时提到这四件事，否则认为「没有可核验的交付信息」。
REQUIRED_WORDS = ("输入", "产出", "结论", "移交")


def load_manifest():
    with open(MANIFEST, "r", encoding="utf-8") as f:
        return json.load(f)


def parse_name(filename, role_ids):
    """从 `<date>-<role>-<task>.md` 里拆出角色。角色 id 本身可能含连字符。"""
    if not filename.endswith(".md"):
        return None, None
    stem = filename[:-3]
    m = DATE_RE.match(stem)
    if not m:
        return None, None
    rest = m.group(4)
    # 优先匹配最长的角色 id，避免 security-reviewer 被 reviewer 抢先匹配。
    for rid in sorted(role_ids, key=len, reverse=True):
        if rest.startswith(rid + "-") or rest == rid:
            return rid, rest[len(rid):].lstrip("-")
    return None, None


def run_git(args):
    try:
        out = subprocess.run(
            ["git"] + args,
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError:
        return None
    if out.returncode != 0:
        return None
    return out.stdout.decode("utf-8", "replace")


def changed_files(base):
    """返回 (状态, 路径) 列表；状态为 A/M/D 等。base 为全零时返回 None。"""
    if not base or set(base) == {"0"}:
        return None
    out = run_git(["diff", "--name-status", base + "...HEAD"])
    if out is None:
        out = run_git(["diff", "--name-status", base + "..HEAD"])
    if out is None:
        return None
    result = []
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) >= 2:
            result.append((parts[0][:1], parts[-1]))
    return result


def audit_files(audit_dir):
    directory = os.path.join(ROOT, audit_dir)
    if not os.path.isdir(directory):
        return []
    return sorted(
        os.path.join(directory, n)
        for n in os.listdir(directory)
        if n.endswith(".md") and n != "_template.md"
    )


def main():
    parser = argparse.ArgumentParser(description="审计门禁")
    parser.add_argument("--diff", metavar="REF", help="与 REF 比较；代码有改动则要求新增审计")
    parser.add_argument("--require", metavar="ROLES", help="要求这些角色至少各有一份审计")
    parser.add_argument("--verbose", action="store_true", help="打印每个角色的审计数量")
    args = parser.parse_args()

    manifest = load_manifest()
    audit_dir = manifest.get("audit_dir", "product/docs/audit")
    role_ids = {r["id"] for r in manifest["roles"]}

    errors = []

    # ---- 1. 历史审计：命名与角色合法性 ----
    files = audit_files(audit_dir)
    counts = {}
    for path in files:
        name = os.path.basename(path)
        rid, task = parse_name(name, role_ids)
        if rid is None:
            errors.append(
                "%s/%s：文件名不合规（应为 <YYYY-MM-DD>-<角色>-<任务>.md，且角色需在 manifest 中）"
                % (audit_dir, name)
            )
            continue
        counts[rid] = counts.get(rid, 0) + 1
        if not task:
            errors.append("%s/%s：缺少任务名" % (audit_dir, name))
        if os.path.getsize(path) < 80:
            errors.append("%s/%s：内容过短，疑似占位文件" % (audit_dir, name))

    if args.verbose:
        for rid in sorted(role_ids):
            print("  %-22s %d" % (rid, counts.get(rid, 0)))

    # ---- 2. 指定角色必须有审计 ----
    if args.require:
        for rid in [r.strip() for r in args.require.split(",") if r.strip()]:
            if rid not in role_ids:
                errors.append("--require 指定了未知角色：%s" % rid)
            elif counts.get(rid, 0) == 0:
                errors.append("角色 `%s` 没有任何审计文档——该环节视为未发生" % rid)

    # ---- 3. 改动区间：代码变了就必须有新增审计 ----
    if args.diff:
        changes = changed_files(args.diff)
        if changes is None:
            print(
                "警告：无法与 `%s` 比较（浅克隆或引用不存在），跳过区间检查。" % args.diff,
                file=sys.stderr,
            )
        else:
            code_changed = sorted({p for s, p in changes if s != "D" and p.startswith(CODE_PREFIXES)})
            new_audits = sorted(
                p for s, p in changes
                if s == "A" and p.startswith(audit_dir + "/") and p.endswith(".md")
            )

            if code_changed and not new_audits:
                errors.append(
                    "改了 %d 个代码文件，但本次区间没有新增任何审计文档（%s/）。"
                    "角色流程未留痕，视为未执行。" % (len(code_changed), audit_dir)
                )

            for rel in new_audits:
                path = os.path.join(ROOT, rel)
                rid, task = parse_name(os.path.basename(rel), role_ids)
                if rid is None:
                    errors.append("%s：新增审计文件名不合规" % rel)
                    continue
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
                missing = [w for w in REQUIRED_WORDS if w not in text]
                if missing:
                    errors.append(
                        "%s：缺少必备信息（%s）" % (rel, "、".join(missing))
                    )

    if errors:
        print("角色门禁检查失败：")
        for e in errors:
            print("  - " + e)
        print("\n规范见 .role-system/agents.md §6.6 与 .role-system/invocation.md §4")
        return 1

    print("角色门禁检查通过：%d 份审计，%d 个角色" % (len(files), len(counts)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
