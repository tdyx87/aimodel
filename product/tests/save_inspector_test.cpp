// 角色：cpp-implementer
// 产物：tests/save_inspector_test.cpp
// 契约：docs/design/save-format-contract.md
//
// 断言结构与不变量，不断言用例特有的具体数值（agents.md §7.4）。
// 覆盖重点：CRC64 测试向量、三种格式版本、迁移、六种损坏场景、
// 资源耗尽防护（伪造超大数组计数）。

#include "saveinspector/save_inspector.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char kMagic[8] = {'S', 'A', 'V', 'E', 'F', 'I', 'L', 'E'};
const char kFooter[8] = {'E', 'N', 'D', 'S', 'L', 'O', 'T', '\0'};

// 小端追加工具（与契约 §2.1 的编码约定一致）。
void AppendU32(std::string* s, uint32_t v) {
    s->push_back(static_cast<char>(v & 0xFFu));
    s->push_back(static_cast<char>((v >> 8) & 0xFFu));
    s->push_back(static_cast<char>((v >> 16) & 0xFFu));
    s->push_back(static_cast<char>((v >> 24) & 0xFFu));
}

void AppendU64(std::string* s, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        s->push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
    }
}

// 按契约 §2.2 的字段顺序拼一个槽位数据区。
std::string MakeSlotData(uint32_t version, uint32_t level, uint64_t coins,
                         const std::vector<uint32_t>& item_ids,
                         const std::vector<uint32_t>& achievements) {
    std::string s;
    AppendU32(&s, level);
    AppendU64(&s, coins);
    if (version >= 2) {
        AppendU64(&s, 3600 * 128);  // playtime_sec
        AppendU32(&s, static_cast<uint32_t>(item_ids.size()));
        for (size_t i = 0; i < item_ids.size(); ++i) {
            AppendU32(&s, item_ids[i]);
            AppendU32(&s, 2);
        }
    }
    if (version >= 3) {
        AppendU32(&s, static_cast<uint32_t>(achievements.size()));
        for (size_t i = 0; i < achievements.size(); ++i) {
            AppendU32(&s, achievements[i]);
        }
        AppendU64(&s, 1773500000);  // last_played_utc
        AppendU64(&s, 0xDEADBEEF);  // signature
    }
    return s;
}

// 组装一个完整存档文件（头 + 槽位表 + 数据区 + 尾）。
std::string MakeSave(uint32_t version, const std::vector<std::string>& slots,
                     uint32_t flags = 0) {
    const size_t kSlotEntrySize = 24;
    const size_t header_size = 32;

    std::string header;
    header.append(kMagic, 8);
    AppendU32(&header, version);
    AppendU32(&header, flags);
    AppendU32(&header, static_cast<uint32_t>(slots.size()));
    AppendU32(&header, 0);
    AppendU64(&header, 1773000000);

    // 数据区起点 = 头 + **完整**槽位表。
    // 这里必须用整张表的长度，不能用「循环中已写入的表长」——后者会让
    // 第一个槽位的偏移落在表内部，读出的是表项而不是槽位数据。
    const size_t data_start = header_size + slots.size() * kSlotEntrySize;

    std::string body;
    std::string table;
    for (size_t i = 0; i < slots.size(); ++i) {
        const uint64_t offset = static_cast<uint64_t>(data_start + body.size());
        AppendU64(&table, offset);
        AppendU64(&table, static_cast<uint64_t>(slots[i].size()));
        AppendU64(&table, saveinspector::Crc64Ecma182(slots[i]));
        body.append(slots[i]);
    }
    return header + table + body + std::string(kFooter, 8);
}

bool HasDiagnostic(const saveinspector::SaveFile& save, const char* needle) {
    for (size_t i = 0; i < save.diagnostics.size(); ++i) {
        if (save.diagnostics[i].title.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// CRC64：契约 §2.1.1 的强制测试向量
// ---------------------------------------------------------------------------

TEST(Crc64, MatchesContractTestVector) {
    // 契约规定：对 ASCII "123456789" 必须得到 0x6C40DF5F0B497347。
    // 这是跨平台一致性的唯一可验证手段——若实现方选了别的变体，此断言即失败。
    EXPECT_EQ(saveinspector::Crc64Ecma182(std::string("123456789")),
              0x6C40DF5F0B497347ULL);
}

TEST(Crc64, EmptyInputIsZero) {
    EXPECT_EQ(saveinspector::Crc64Ecma182(std::string()), 0ULL);
}

TEST(Crc64, IsDeterministicAndOrderSensitive) {
    const std::string a("hello");
    const std::string b("olleh");
    EXPECT_EQ(saveinspector::Crc64Ecma182(a), saveinspector::Crc64Ecma182(a));
    EXPECT_NE(saveinspector::Crc64Ecma182(a), saveinspector::Crc64Ecma182(b));
}

// ---------------------------------------------------------------------------
// 正常解析
// ---------------------------------------------------------------------------

TEST(SaveInspectorTest, ParsesV3Save) {
    std::vector<uint32_t> items;
    items.push_back(1001);
    items.push_back(1002);
    std::vector<uint32_t> achs;
    achs.push_back(1);
    achs.push_back(2);

    const std::string bytes = MakeSave(3, std::vector<std::string>{
        MakeSlotData(3, 42, 12480, items, achs),
        MakeSlotData(3, 8, 310, items, achs)});

    saveinspector::SaveInspector inspector;
    saveinspector::SaveFile save;
    std::string error;
    ASSERT_TRUE(inspector.OpenBytes(bytes, &save, &error)) << error;
    EXPECT_EQ(save.version, saveinspector::FormatVersion::kV3);
    ASSERT_EQ(save.slots.size(), 2u);

    EXPECT_EQ(save.slots[0].summary.level, 42u);
    EXPECT_EQ(save.slots[0].summary.coins, 12480u);
    EXPECT_TRUE(save.slots[0].summary.healthy);
    // ASSERT：下面要索引 items[0]，长度断言必须先行中止而非继续执行
    ASSERT_EQ(save.slots[0].items.size(), 2u);
    // 道具名必须由核心库给出（契约 §3.2），前端不维护映射表。
    EXPECT_EQ(save.slots[0].items[0].name, std::string("治疗药水"));
    ASSERT_EQ(save.slots[0].achievements.size(), 2u);
    // v3 不应产生升级提示
    EXPECT_FALSE(HasDiagnostic(save, "升级"));
}

TEST(SaveInspectorTest, UnknownItemIdGetsReadableFallbackName) {
    const std::vector<uint32_t> items(1, 999999);
    const std::string bytes = MakeSave(3, std::vector<std::string>{
        MakeSlotData(3, 1, 0, items, std::vector<uint32_t>())});

    saveinspector::SaveInspector inspector;
    saveinspector::SaveFile save;
    std::string error;
    ASSERT_TRUE(inspector.OpenBytes(bytes, &save, &error)) << error;
    ASSERT_EQ(save.slots.size(), 1u);
    ASSERT_EQ(save.slots[0].items.size(), 1u);
    // 未知 ID 不得返回空名——界面会把空名渲染成「有道具但没名字」。
    EXPECT_FALSE(save.slots[0].items[0].name.empty());
    EXPECT_NE(save.slots[0].items[0].name.find("999999"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 版本迁移（PRD US-6）
// ---------------------------------------------------------------------------

TEST(SaveInspectorTest, UpgradesV1SaveWithDefaults) {
    // v1 只有 level + coins。
    std::string s;
    AppendU32(&s, 7);
    AppendU64(&s, 500);
    const std::string bytes = MakeSave(1, std::vector<std::string>(1, s));

    saveinspector::SaveInspector inspector;
    saveinspector::SaveFile save;
    std::string error;
    ASSERT_TRUE(inspector.OpenBytes(bytes, &save, &error)) << error;

    EXPECT_EQ(save.version, saveinspector::FormatVersion::kV1);
    ASSERT_EQ(save.slots.size(), 1u);
    EXPECT_EQ(save.slots[0].summary.level, 7u);
    EXPECT_EQ(save.slots[0].summary.coins, 500u);
    // 缺失字段按默认值补齐，且必须标注「已升级」（PRD US-6）
    EXPECT_TRUE(save.slots[0].items.empty());
    EXPECT_EQ(save.slots[0].summary.playtime_sec, 0u);
    EXPECT_TRUE(save.slots[0].upgraded);
    EXPECT_TRUE(HasDiagnostic(save, "升级"));
}

TEST(SaveInspectorTest, UpgradesV2SaveWithDefaults) {
    const std::vector<uint32_t> items(1, 1003);
    const std::string bytes = MakeSave(2, std::vector<std::string>{
        MakeSlotData(2, 15, 900, items, std::vector<uint32_t>())});

    saveinspector::SaveInspector inspector;
    saveinspector::SaveFile save;
    std::string error;
    ASSERT_TRUE(inspector.OpenBytes(bytes, &save, &error)) << error;

    ASSERT_EQ(save.slots.size(), 1u);
    EXPECT_EQ(save.slots[0].items.size(), 1u);
    // v2 没有成就与最近游玩时间
    EXPECT_TRUE(save.slots[0].achievements.empty());
    EXPECT_EQ(save.slots[0].summary.last_played_utc, 0u);
    EXPECT_TRUE(save.slots[0].upgraded);
}

// ---------------------------------------------------------------------------
// 损坏场景（契约 §2.3 六种）
// ---------------------------------------------------------------------------

TEST(SaveInspectorTest, RejectsNonSaveFile) {
    saveinspector::SaveInspector inspector;
    saveinspector::SaveFile save;
    std::string error;
    EXPECT_FALSE(inspector.OpenBytes("this is a text file, not a save", &save, &error));
    // 错误文案必须是自然语言，不得含错误码/偏移量（PRD US-5）
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(error.find("不是存档"), std::string::npos);
}

TEST(SaveInspectorTest, RejectsFutureVersion) {
    const std::string bytes = MakeSave(4, std::vector<std::string>());
    saveinspector::SaveInspector inspector;
    saveinspector::SaveFile save;
    std::string error;
    EXPECT_FALSE(inspector.OpenBytes(bytes, &save, &error));
    EXPECT_NE(error.find("更新版本"), std::string::npos);
}

TEST(SaveInspectorTest, RejectsEncryptedSave) {
    const std::string bytes = MakeSave(3, std::vector<std::string>(), 0x2u);
    saveinspector::SaveInspector inspector;
    saveinspector::SaveFile save;
    std::string error;
    EXPECT_FALSE(inspector.OpenBytes(bytes, &save, &error));
    EXPECT_NE(error.find("加密"), std::string::npos);
}

TEST(SaveInspectorTest, RejectsTruncatedFile) {
    std::string bytes = MakeSave(3, std::vector<std::string>{
        MakeSlotData(3, 1, 1, std::vector<uint32_t>(), std::vector<uint32_t>())});
    ASSERT_GT(bytes.size(), 20u);
    bytes.resize(bytes.size() - 10);  // 砍掉尾部标记

    saveinspector::SaveInspector inspector;
    saveinspector::SaveFile save;
    std::string error;
    EXPECT_FALSE(inspector.OpenBytes(bytes, &save, &error));
    EXPECT_FALSE(error.empty());
}

TEST(SaveInspectorTest, DetectsChecksumMismatchButKeepsOtherSlotsReadable) {
    // 槽位 1 数据合法；槽位 2 的校验和被改坏。
    std::vector<uint32_t> items(1, 1001);
    std::string good = MakeSlotData(3, 5, 50, items, std::vector<uint32_t>());
    std::string bad = MakeSlotData(3, 6, 60, items, std::vector<uint32_t>());
    std::vector<std::string> slots;
    slots.push_back(good);
    slots.push_back(bad);
    std::string bytes = MakeSave(3, slots);

    // 篡改第二个槽位的最后一个字节（signature 区域）
    const size_t bad_offset = bytes.size() - 8 - good.size();
    bytes[bad_offset] = static_cast<char>(bytes[bad_offset] ^ 0xFF);

    saveinspector::SaveInspector inspector;
    saveinspector::SaveFile save;
    std::string error;
    ASSERT_TRUE(inspector.OpenBytes(bytes, &save, &error)) << error;

    ASSERT_EQ(save.slots.size(), 2u);
    EXPECT_TRUE(save.slots[0].summary.healthy);
    EXPECT_FALSE(save.slots[1].summary.healthy);
    // 损坏必须被明确定位到具体槽位（US-5「哪里坏了」）
    EXPECT_TRUE(HasDiagnostic(save, "槽位 2"));
    // 关键：部分损坏不得导致整体拒绝——其余槽位仍可用（PRD US-3）
    EXPECT_EQ(save.slots[0].summary.level, 5u);
}

TEST(SaveInspectorTest, ReportsOutOfRangeSlotOffset) {
    // 手工构造：槽位表里的 offset 指向文件之外。
    std::string header;
    header.append(kMagic, 8);
    AppendU32(&header, 3);
    AppendU32(&header, 0);
    AppendU32(&header, 1);
    AppendU32(&header, 0);
    AppendU64(&header, 1773000000);

    std::string table;
    AppendU64(&table, 0xFFFFFFFFFFFFFFF0ULL);  // 越界 offset
    AppendU64(&table, 16);
    AppendU64(&table, 0);

    const std::string bytes = header + table + std::string(kFooter, 8);

    saveinspector::SaveInspector inspector;
    saveinspector::SaveFile save;
    std::string error;
    ASSERT_TRUE(inspector.OpenBytes(bytes, &save, &error)) << error;
    ASSERT_EQ(save.slots.size(), 1u);
    EXPECT_FALSE(save.slots[0].summary.healthy);
    EXPECT_TRUE(HasDiagnostic(save, "位置异常"));
}

// ---------------------------------------------------------------------------
// 稳健性：不可信输入不得崩溃、不得耗尽内存
// ---------------------------------------------------------------------------

TEST(SaveInspectorTest, RejectsOversizedArrayCountWithoutAllocating) {
    // 伪造 items 计数为 0xFFFFFFFF：若实现先分配再校验，这里会尝试分配 32 GB。
    std::string s;
    AppendU32(&s, 1);
    AppendU64(&s, 0);
    AppendU64(&s, 0);
    AppendU32(&s, 0xFFFFFFFFu);
    const std::string bytes = MakeSave(2, std::vector<std::string>(1, s));

    saveinspector::SaveInspector inspector;
    saveinspector::SaveFile save;
    std::string error;
    // 关键：返回 true（文件本身合法）但槽位标记为不健康，而不是崩溃/OOM。
    ASSERT_TRUE(inspector.OpenBytes(bytes, &save, &error)) << error;
    ASSERT_EQ(save.slots.size(), 1u);
    EXPECT_FALSE(save.slots[0].summary.healthy);
}

TEST(SaveInspectorTest, RejectsOversizedSlotCount) {
    std::string header;
    header.append(kMagic, 8);
    AppendU32(&header, 3);
    AppendU32(&header, 0);
    AppendU32(&header, 0xFFFFFFFFu);  // 40 亿槽位
    AppendU32(&header, 0);
    AppendU64(&header, 0);
    const std::string bytes = header + std::string(kFooter, 8);

    saveinspector::SaveInspector inspector;
    saveinspector::SaveFile save;
    std::string error;
    EXPECT_FALSE(inspector.OpenBytes(bytes, &save, &error));
}

TEST(SaveInspectorTest, SurvivesArbitraryTruncationOfValidFile) {
    // 逐字节截断一个合法存档，任何一点都不能崩溃。
    const std::vector<uint32_t> items(2, 1001);
    const std::string full = MakeSave(3, std::vector<std::string>{
        MakeSlotData(3, 3, 300, items, std::vector<uint32_t>(2, 1))});

    for (size_t cut = 1; cut < full.size(); cut += 3) {
        saveinspector::SaveInspector inspector;
        saveinspector::SaveFile save;
        std::string error;
        inspector.OpenBytes(full.substr(0, cut), &save, &error);
        // 只要求不崩溃；成功或失败都合法
    }
    SUCCEED();
}

TEST(SaveInspectorTest, SurvivesRandomByteFlips) {
    const std::vector<uint32_t> items(2, 1001);
    std::string full = MakeSave(3, std::vector<std::string>{
        MakeSlotData(3, 3, 300, items, std::vector<uint32_t>(2, 1))});

    // 固定步长的确定性伪随机翻转（不引入随机种子依赖，保证可复现）
    uint32_t state = 12345u;
    for (int round = 0; round < 200; ++round) {
        state = state * 1103515245u + 12345u;
        const size_t pos = (state >> 8) % full.size();
        std::string mutated = full;
        mutated[pos] = static_cast<char>(mutated[pos] ^ 0x5A);
        saveinspector::SaveInspector inspector;
        saveinspector::SaveFile save;
        std::string error;
        inspector.OpenBytes(mutated, &save, &error);
    }
    SUCCEED();
}

// ---------------------------------------------------------------------------
// 导出（PRD US-4 / US-7）
// ---------------------------------------------------------------------------

TEST(SaveInspectorTest, ExportContainsUserFacingDataOnly) {
    const std::vector<uint32_t> items(1, 1002);
    const std::string bytes = MakeSave(3, std::vector<std::string>{
        MakeSlotData(3, 11, 1111, items, std::vector<uint32_t>(1, 7))});

    saveinspector::SaveInspector inspector;
    saveinspector::SaveFile save;
    std::string error;
    ASSERT_TRUE(inspector.OpenBytes(bytes, &save, &error)) << error;

    saveinspector::SlotDetail slot;
    ASSERT_TRUE(inspector.ReadSlot(0, &slot));

    const std::string json = inspector.SlotToJson(slot);
    EXPECT_NE(json.find("\"slot\":1"), std::string::npos);
    EXPECT_NE(json.find("\"level\":11"), std::string::npos);
    EXPECT_NE(json.find("铁剑"), std::string::npos);
    // 导出不得含本机路径（契约 §5）——本用例未提供路径，此处断言无盘符/反斜杠
    EXPECT_EQ(json.find('\\'), std::string::npos);

    const std::string report = inspector.SlotToTextReport(slot);
    EXPECT_NE(report.find("第 11 章"), std::string::npos);
    EXPECT_NE(report.find("铁剑"), std::string::npos);
    EXPECT_NE(report.find("未被修改"), std::string::npos);
}

TEST(SaveInspectorTest, DiagnosticsAlwaysCarryAdvice) {
    const std::vector<uint32_t> items(1, 1001);
    std::string good = MakeSlotData(3, 1, 1, items, std::vector<uint32_t>());
    std::string bad = MakeSlotData(3, 2, 2, items, std::vector<uint32_t>());
    std::vector<std::string> slots;
    slots.push_back(good);
    slots.push_back(bad);
    std::string bytes = MakeSave(3, slots);
    bytes[bytes.size() - 8 - good.size()] ^= 0xFF;

    saveinspector::SaveInspector inspector;
    saveinspector::SaveFile save;
    std::string error;
    ASSERT_TRUE(inspector.OpenBytes(bytes, &save, &error)) << error;
    ASSERT_FALSE(save.diagnostics.empty());

    for (size_t i = 0; i < save.diagnostics.size(); ++i) {
        const saveinspector::Diagnostic& d = save.diagnostics[i];
        // 三段式缺一不可（PRD US-5）
        EXPECT_FALSE(d.title.empty()) << "诊断 " << i << " 缺标题";
        EXPECT_FALSE(d.detail.empty()) << "诊断 " << i << " 缺说明";
        EXPECT_FALSE(d.advice.empty()) << "诊断 " << i << " 缺建议";
    }
}

TEST(SaveInspectorTest, ReadSlotRejectsOutOfRangeIndex) {
    const std::string bytes = MakeSave(3, std::vector<std::string>{
        MakeSlotData(3, 1, 1, std::vector<uint32_t>(), std::vector<uint32_t>())});
    saveinspector::SaveInspector inspector;
    saveinspector::SaveFile save;
    std::string error;
    ASSERT_TRUE(inspector.OpenBytes(bytes, &save, &error)) << error;

    saveinspector::SlotDetail slot;
    EXPECT_TRUE(inspector.ReadSlot(0, &slot));
    EXPECT_FALSE(inspector.ReadSlot(1, &slot));
    EXPECT_FALSE(inspector.ReadSlot(0, NULL));
}

TEST(SaveInspectorTest, FormatsDurationAndTimestamp) {
    EXPECT_EQ(saveinspector::FormatDuration(0), std::string("0 分钟"));
    EXPECT_EQ(saveinspector::FormatDuration(90), std::string("1 分钟"));
    EXPECT_EQ(saveinspector::FormatDuration(3600), std::string("1 小时"));
    EXPECT_EQ(saveinspector::FormatDuration(3600 * 128 + 720),
              std::string("128 小时 12 分"));
    EXPECT_EQ(saveinspector::FormatTimestamp(0), std::string("未知"));
    // 1773500000 = 2026-03-14T14:53:20Z（独立核算：2026-01-01T00:00:00Z 为
    // 1767225600，差 6274400s = 72 天 14:53:20）
    EXPECT_EQ(saveinspector::FormatTimestamp(1773500000),
              std::string("2026-03-14 14:53"));
}
