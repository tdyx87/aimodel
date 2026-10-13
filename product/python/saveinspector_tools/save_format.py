"""存档查看器内部工具链：存档格式的参考实现（Python 侧）。

角色：python-implementer
产物：python/saveinspector_tools/
契约：product/docs/design/save-format-contract.md §2

**用户不可见，不随产品安装包分发**（roles.md §3.4）。

存在意义：核心库用 C++ 实现，若两侧各写一份格式编码，任一处漂移都很难在
编译期或测试期发现。本模块是**独立的第二实现**，用于：

1. 生成测试与手工验证所需的存档样本（各版本 + 各损坏场景）；
2. 在 pytest 中断言「C++ 解析结果」与「本实现预期的结构」一致。

因此本模块**不得**从 C++ 侧导入任何代码——它必须独立按契约 §2 写一遍，
这样它的正确性才有交叉验证价值。

硬约束（roles.md §3.4）：
- 全部函数带类型注解；
- 输出确定性（固定随机种子，或干脆不引入随机）；
- 依赖固定版本并写入 lock 文件。
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import List, Tuple

# ---- 契约 §2.1 的固定常量 ----
MAGIC = b"SAVEFILE"
FOOTER = b"ENDSLOT\x00"
HEADER_SIZE = 32
SLOT_ENTRY_SIZE = 24

# ---- 契约 §2.1.1：CRC-64/ECMA-182，参数不得改动 ----
# poly 正常式 / init=0 / refin=refout=False / xorout=0
# 对 ASCII "123456789" 必须得到 0x6C40DF5F0B497347（见 tests 中的交叉断言）。
_CRC64_POLY = 0x42F0E1EBA9EA3693
_CRC64_INIT = 0x0000000000000000
_CRC64_XOROUT = 0x0000000000000000


def crc64_ecma182(data: bytes) -> int:
    """CRC-64/ECMA-182（MSB 先行）。

    参数与 C++ 实现逐位一致；契约 §2.1.1 给出强制测试向量
    crc64_ecma182(b"123456789") == 0x6C40DF5F0B497347。
    """
    crc = _CRC64_INIT
    for byte in data:
        crc ^= byte << 56
        for _ in range(8):
            if crc & 0x8000000000000000:
                crc = ((crc << 1) ^ _CRC64_POLY) & 0xFFFFFFFFFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFFFFFFFFFF
    return crc ^ _CRC64_XOROUT


@dataclass(frozen=True)
class Item:
    """道具条目。id 与 C++ 侧 Item::id 语义一致。"""

    id: int
    count: int


@dataclass
class Slot:
    """一个槽位。缺失字段用 None 表示「该版本没有此字段」，区别于 0。"""

    level: int
    coins: int
    playtime_sec: int | None = None
    items: List[Item] | None = None
    achievements: List[int] | None = None
    last_played_utc: int | None = None
    signature: int | None = None


@dataclass
class SlotEntry:
    """槽位表项：偏移、长度、校验和。"""

    offset: int
    length: int
    checksum: int


def encode_slot(slot: Slot, version: int) -> bytes:
    """按契约 §2.2 的字段顺序编码槽位数据区。

    字段顺序与 C++ 侧 ParseSlotData 的读取顺序严格对称。
    """
    out = bytearray()
    out += struct.pack("<I", slot.level)
    out += struct.pack("<Q", slot.coins)
    if version >= 2:
        out += struct.pack("<Q", slot.playtime_sec or 0)
        items = slot.items or []
        out += struct.pack("<I", len(items))
        for item in items:
            out += struct.pack("<I", item.id)
            out += struct.pack("<I", item.count)
    if version >= 3:
        achievements = slot.achievements or []
        out += struct.pack("<I", len(achievements))
        for ach in achievements:
            out += struct.pack("<I", ach)
        out += struct.pack("<Q", slot.last_played_utc or 0)
        out += struct.pack("<Q", slot.signature or 0)
    return bytes(out)


def build_save(
    slots: List[Slot],
    version: int = 3,
    flags: int = 0,
    created_utc: int = 1773000000,
) -> bytes:
    """组装一个完整存档文件（头 + 槽位表 + 数据区 + 尾）。

    注意数据区起点是「头 + **完整**槽位表」，不能用循环中已写入的表长——
    否则第一个槽位的偏移会落在表内部，读出的是表项而非槽位数据。
    """
    encoded = [encode_slot(slot, version) for slot in slots]
    data_start = HEADER_SIZE + len(slots) * SLOT_ENTRY_SIZE

    header = bytearray()
    header += MAGIC
    header += struct.pack("<I", version)
    header += struct.pack("<I", flags)
    header += struct.pack("<I", len(slots))
    header += struct.pack("<I", 0)
    header += struct.pack("<Q", created_utc)

    table = bytearray()
    body = bytearray()
    for blob in encoded:
        table += struct.pack(
            "<Q", data_start + len(body)
        )
        table += struct.pack("<Q", len(blob))
        table += struct.pack("<Q", crc64_ecma182(blob))
        body += blob

    return bytes(header) + bytes(table) + bytes(body) + FOOTER


# ---- 场景样本（供 C++ 单测与手工验证使用）----

def sample_healthy_v3() -> bytes:
    """正常 v3：三槽位，第二个为 part-damage（校验和将被改坏）。"""
    return build_save(
        [
            Slot(
                level=42,
                coins=12480,
                playtime_sec=3600 * 128,
                items=[Item(1001, 12), Item(1002, 1)],
                achievements=[1, 2],
                last_played_utc=1773500000,
                signature=0xDEADBEEF,
            ),
            Slot(
                level=8,
                coins=310,
                playtime_sec=3600 * 9,
                items=[Item(1001, 3)],
                achievements=[1],
                last_played_utc=1773400000,
                signature=0xCAFEBABE,
            ),
            Slot(
                level=1,
                coins=0,
                playtime_sec=0,
                items=[],
                achievements=[],
                last_played_utc=0,
                signature=0,
            ),
        ],
        version=3,
    )


def sample_legacy_v1() -> bytes:
    """v1：只有关卡与金币，用于验证迁移（PRD US-6）。"""
    return build_save(
        [Slot(level=7, coins=500), Slot(level=8, coins=600)],
        version=1,
    )


def sample_legacy_v2() -> bytes:
    """v2：有道具与时长，缺成就。"""
    return build_save(
        [
            Slot(
                level=15,
                coins=900,
                playtime_sec=3600 * 40,
                items=[Item(1003, 1), Item(1005, 7)],
                achievements=[],
                last_played_utc=0,
                signature=0,
            )
        ],
        version=2,
    )


def sample_empty() -> bytes:
    """零槽位：界面应显示空状态而非空白。"""
    return build_save([], version=3)


def sample_corrupted_magic() -> bytes:
    """文件头被改坏：不是存档文件。"""
    data = bytearray(sample_healthy_v3())
    data[0:8] = b"NOTASAVE"
    return bytes(data)


def sample_corrupted_truncated() -> bytes:
    """尾部被截断：保存时中断。"""
    data = sample_healthy_v3()
    return data[: len(data) - 12]


def sample_corrupted_checksum(slot_index: int = 1) -> bytes:
    """指定槽位的校验和与数据不符：被其他工具改过。

    只改槽位表里的 checksum 字段，不动数据本身——这样 CRC 重算会失配，
    而槽位数据仍可解析，正好覆盖「部分损坏仍展示可读内容」的需求。
    """
    data = bytearray(sample_healthy_v3())
    entry_offset = HEADER_SIZE + slot_index * SLOT_ENTRY_SIZE
    # checksum 是槽位表的第三个 u64，偏移 +16
    field_offset = entry_offset + 16
    original = struct.unpack_from("<Q", data, field_offset)[0]
    struct.pack_into("<Q", data, field_offset, original ^ 0xFFFFFFFF)
    return bytes(data)


def sample_corrupted_slot_offset() -> bytes:
    """槽位偏移越界：文件被截断或结构错位。"""
    data = bytearray(sample_healthy_v3())
    entry_offset = HEADER_SIZE  # 第一个槽位
    struct.pack_into("<Q", data, entry_offset, 0xFFFFFFFFFFFFFFF0)
    return bytes(data)


def sample_encrypted() -> bytes:
    """带加密标记：需要游戏本体才能读。"""
    return build_save([Slot(level=1, coins=1)], version=3, flags=0x2)


def sample_future_version() -> bytes:
    """来自更新版本的游戏：本工具应拒绝并给出可执行建议。"""
    return build_save([Slot(level=1, coins=1)], version=4)


def sample_huge_array_count() -> bytes:
    """槽位数据里伪造超大道具计数：验证不发生巨额分配。

    直接构造字节而非走 build_save，因为正常编码不会产生这种值。
    """
    body = bytearray()
    body += struct.pack("<I", 1)      # level
    body += struct.pack("<Q", 0)      # coins
    body += struct.pack("<Q", 0)      # playtime
    body += struct.pack("<I", 0xFFFFFFFF)  # items count = 40 亿
    blob = bytes(body)

    header = bytearray()
    header += MAGIC
    header += struct.pack("<I", 2)
    header += struct.pack("<I", 0)
    header += struct.pack("<I", 1)
    header += struct.pack("<I", 0)
    header += struct.pack("<Q", 1773000000)

    data_start = HEADER_SIZE + SLOT_ENTRY_SIZE
    table = bytearray()
    table += struct.pack("<Q", data_start)
    table += struct.pack("<Q", len(blob))
    table += struct.pack("<Q", crc64_ecma182(blob))

    return bytes(header) + bytes(table) + blob + FOOTER


SAMPLES = {
    "healthy-v3": sample_healthy_v3,
    "legacy-v1": sample_legacy_v1,
    "legacy-v2": sample_legacy_v2,
    "empty": sample_empty,
    "corrupt-magic": sample_corrupted_magic,
    "corrupt-truncated": sample_corrupted_truncated,
    "corrupt-checksum": sample_corrupted_checksum,
    "corrupt-slot-offset": sample_corrupted_slot_offset,
    "encrypted": sample_encrypted,
    "future-version": sample_future_version,
    "huge-array-count": sample_huge_array_count,
}
