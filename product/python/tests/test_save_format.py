"""pytest 用例：断言 Python 参考实现与契约一致。

角色：python-implementer
契约：product/docs/design/save-format-contract.md §2

关键点：这里断言的是**格式本身**是否正确（字节级），而不是「和 C++ 一致」
——后者需要跨语言胶水。本文件保证本实现正确，C++ 侧的等价性由
save_inspector_test.cpp 里的 CRC64 强制向量 + 结构断言共同保证。
"""

from __future__ import annotations

import struct
from typing import Callable

import pytest

from saveinspector_tools.save_format import (
    FOOTER,
    HEADER_SIZE,
    MAGIC,
    SLOT_ENTRY_SIZE,
    Item,
    Slot,
    build_save,
    crc64_ecma182,
    encode_slot,
    sample_corrupted_checksum,
    sample_corrupted_magic,
    sample_corrupted_slot_offset,
    sample_corrupted_truncated,
    sample_empty,
    sample_encrypted,
    sample_future_version,
    sample_healthy_v3,
    sample_huge_array_count,
    sample_legacy_v1,
    sample_legacy_v2,
)


# ---------------------------------------------------------------------------
# CRC64：契约 §2.1.1 的强制测试向量（与 C++ 侧同一个断言）
# ---------------------------------------------------------------------------

def test_crc64_matches_contract_vector() -> None:
    # 契约 §2.1.1：对 ASCII "123456789" 必须得到 0x6C40DF5F0B497347。
    assert crc64_ecma182(b"123456789") == 0x6C40DF5F0B497347


def test_crc64_empty_is_zero() -> None:
    assert crc64_ecma182(b"") == 0


def test_crc64_is_order_sensitive() -> None:
    assert crc64_ecma182(b"hello") != crc64_ecma182(b"olleh")


# ---------------------------------------------------------------------------
# 文件结构
# ---------------------------------------------------------------------------

def test_save_header_layout() -> None:
    data = sample_healthy_v3()
    assert data[:8] == MAGIC
    version, flags, slot_count, reserved = struct.unpack_from("<IIII", data, 8)
    assert version == 3
    assert flags == 0
    assert slot_count == 3
    assert reserved == 0
    assert data[-8:] == FOOTER


@pytest.mark.parametrize(
    "factory,expect_version",
    [
        (sample_legacy_v1, 1),
        (sample_legacy_v2, 2),
        (sample_healthy_v3, 3),
    ],
)
def test_version_is_encoded_in_header(
    factory: Callable[[], bytes], expect_version: int
) -> None:
    data = factory()
    version = struct.unpack_from("<I", data, 8)[0]
    assert version == expect_version


@pytest.mark.parametrize(
    "factory",
    [sample_healthy_v3, sample_legacy_v1, sample_legacy_v2, sample_empty],
)
def test_slot_table_offsets_point_at_real_data(factory: Callable[[], bytes]) -> None:
    """槽位表里的偏移必须指向数据区，而不是表内部。

    这条曾经是真实缺陷：偏移误用「循环中已写入的表长」时，
    第一个槽位的偏移会落在槽位表内部，读出的是表项而非槽位数据。
    """
    data = factory()
    slot_count = struct.unpack_from("<I", data, 16)[0]
    data_start = HEADER_SIZE + slot_count * SLOT_ENTRY_SIZE
    for i in range(slot_count):
        offset, length, checksum = struct.unpack_from(
            "<QQQ", data, HEADER_SIZE + i * SLOT_ENTRY_SIZE
        )
        assert offset >= data_start, f"槽位 {i} 的偏移落在表内部"
        assert offset + length <= len(data) - len(FOOTER)
        assert checksum == crc64_ecma182(data[offset : offset + length])


def test_slot_data_is_little_endian() -> None:
    slot = Slot(level=42, coins=12480, playtime_sec=0, items=[], achievements=[],
                last_played_utc=0, signature=0)
    blob = encode_slot(slot, 3)
    assert blob[:4] == struct.pack("<I", 42)
    assert blob[4:12] == struct.pack("<Q", 12480)


def test_version_2_has_no_achievements_field() -> None:
    slot = Slot(level=1, coins=1, playtime_sec=0, items=[Item(1001, 1)],
                achievements=[], last_played_utc=0, signature=0)
    blob = encode_slot(slot, 2)
    # v1 与 v2 的长度差 = playtime(8) + items_count(4) + 1 个 item(8)
    assert len(blob) == len(encode_slot(slot, 1)) + 20


# ---------------------------------------------------------------------------
# 损坏场景（契约 §2.3 六种）
# ---------------------------------------------------------------------------

def test_corrupt_magic_breaks_magic() -> None:
    assert sample_corrupted_magic()[:8] != MAGIC


def test_corrupt_truncated_loses_footer() -> None:
    data = sample_corrupted_truncated()
    assert not data.endswith(FOOTER)


def test_corrupt_checksum_only_changes_checksum_field() -> None:
    """只改校验和、不动数据——这样槽位数据仍可解析。

    正好覆盖「部分损坏仍展示可读内容」的需求（PRD US-3）。
    """
    healthy = sample_healthy_v3()
    broken = sample_corrupted_checksum(slot_index=1)
    assert len(healthy) == len(broken)
    field_offset = HEADER_SIZE + 1 * SLOT_ENTRY_SIZE + 16
    assert healthy[:field_offset] == broken[:field_offset]
    assert healthy[field_offset + 8 :] == broken[field_offset + 8 :]
    assert healthy[field_offset : field_offset + 8] != broken[
        field_offset : field_offset + 8
    ]


def test_corrupt_slot_offset_is_out_of_range() -> None:
    data = sample_corrupted_slot_offset()
    offset = struct.unpack_from("<Q", data, HEADER_SIZE)[0]
    assert offset > len(data)


def test_encrypted_sets_flag() -> None:
    flags = struct.unpack_from("<I", sample_encrypted(), 12)[0]
    assert flags & 0x2


def test_future_version_is_beyond_supported() -> None:
    assert struct.unpack_from("<I", sample_future_version(), 8)[0] > 3


# ---------------------------------------------------------------------------
# 资源耗尽防护
# ---------------------------------------------------------------------------

def test_huge_array_count_is_encoded_but_never_dereferenced() -> None:
    """样本里带了 40 亿的道具计数。

    生成它只是为了让 C++ 侧验证「先按剩余字节校验、后分配」；
    Python 侧绝不能真的去迭代这个计数。
    """
    data = sample_huge_array_count()
    data_start = HEADER_SIZE + SLOT_ENTRY_SIZE
    offset, length, _ = struct.unpack_from("<QQQ", data, HEADER_SIZE)
    assert offset == data_start
    items_count = struct.unpack_from("<I", data, offset + 20)[0]
    assert items_count == 0xFFFFFFFF


# ---------------------------------------------------------------------------
# 确定性（roles.md §3.4：输出确定性）
# ---------------------------------------------------------------------------

def test_generation_is_deterministic() -> None:
    """同一输入两次生成必须字节一致——否则样本不可用于回归比对。"""
    assert sample_healthy_v3() == sample_healthy_v3()
    assert sample_corrupted_checksum() == sample_corrupted_checksum()
