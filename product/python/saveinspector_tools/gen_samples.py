"""把各场景存档样本写到磁盘，供 C++ 单测 / 手工验证 / 演示使用。

角色：python-implementer
用法：
    python -m saveinspector_tools.gen_samples <输出目录>
    python -m saveinspector_tools.gen_samples <输出目录> --with-large
输出文件名与场景名的对应见 SAMPLES 的键。
"""

from __future__ import annotations

import sys
from pathlib import Path

from .save_format import SAMPLES, sample_large_save


def write_all(out_dir: Path, with_large: bool = False) -> List[Path]:
    """生成全部样本，返回写入的文件路径列表。

    with_large=True 时额外生成约 100MB 的大存档，用于验证 PRD NFR-2。
    默认不生成——它占磁盘且生成耗时，只在需要时显式开启。
    """
    out_dir.mkdir(parents=True, exist_ok=True)
    written: List[Path] = []
    for name, factory in sorted(SAMPLES.items()):
        path = out_dir / f"{name}.sav"
        path.write_bytes(factory())
        written.append(path)
    if with_large:
        big = out_dir / "large-100mb.sav"
        big.write_bytes(sample_large_save())
        written.append(big)
    return written


def main(argv: List[str]) -> int:
    args = [a for a in argv[1:] if not a.startswith("--")]
    if not args:
        print(
            "用法：python -m saveinspector_tools.gen_samples <输出目录> [--with-large]",
            file=sys.stderr,
        )
        return 2
    target = Path(args[0])
    for path in write_all(target, with_large="--with-large" in argv[1:]):
        print(f"已生成 {path}（{path.stat().st_size} 字节）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
