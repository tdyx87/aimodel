"""把各场景存档样本写到磁盘，供 C++ 单测 / 手工验证 / 演示使用。

角色：python-implementer
用法：
    python -m saveinspector_tools.gen_samples <输出目录>
输出文件名与场景名的对应见 SAMPLES 的键。
"""

from __future__ import annotations

import sys
from pathlib import Path

from .save_format import SAMPLES


def write_all(out_dir: Path) -> List[Path]:
    """生成全部样本，返回写入的文件路径列表。"""
    out_dir.mkdir(parents=True, exist_ok=True)
    written: List[Path] = []
    for name, factory in sorted(SAMPLES.items()):
        path = out_dir / f"{name}.sav"
        path.write_bytes(factory())
        written.append(path)
    return written


def main(argv: List[str]) -> int:
    if len(argv) != 2:
        print("用法：python -m saveinspector_tools.gen_samples <输出目录>", file=sys.stderr)
        return 2
    target = Path(argv[1])
    for path in write_all(target):
        print(f"已生成 {path}（{path.stat().st_size} 字节）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
