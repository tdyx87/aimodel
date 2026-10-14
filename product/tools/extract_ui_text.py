"""提取 app.js 中所有用户可见的中文文案，供文档核对使用（doc-engineer 职责）。"""
import pathlib
import re
import sys

sys.stdout.reconfigure(encoding="utf-8")

src = pathlib.Path("web/app.js").read_text(encoding="utf-8")
# 匹配任意字符串字面量里含中文的内容
candidates = set()
for m in re.finditer(r"(['\"])(.*?)\1", src, re.S):
    text = m.group(2)
    if any("\u4e00" <= ch <= "\u9fff" for ch in text):
        if len(text) <= 80 and "\n" not in text:
            candidates.add(text)

for t in sorted(candidates):
    print("-", t)