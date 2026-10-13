"""存档查看器内部工具链包（用户不可见，不随产品分发）。

角色：python-implementer
契约：product/docs/design/save-format-contract.md §2

本包是存档格式的**独立第二实现**，用于交叉验证 C++ 侧，并生成测试样本。
"""

from .save_format import (  # noqa: F401
    Item,
    Slot,
    SAMPLES,
    build_save,
    crc64_ecma182,
    encode_slot,
)

__all__ = [
    "Item",
    "Slot",
    "SAMPLES",
    "build_save",
    "crc64_ecma182",
    "encode_slot",
]
