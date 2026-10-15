// 角色：cpp-implementer
// 产物：src/cpp/save_inspector.cpp
// 契约：docs/design/save-format-contract.md
//
// 本文件平台无关（纯计算 + 字符串）。文件读取隔离在 save_file_{win32,posix}.cpp。
//
// 稳健性是本文件的第一要务（PRD NFR-2 + 契约 §3.3「任意字节不得崩溃」）：
// 存档是**不可信输入**——用户可能拿被改过的、被截断的、来自别的游戏的文件。
// 因此所有长度、偏移、数组元素数在使用前都必须先对「剩余字节数」校验，
// 绝不能先分配再检查。

#include "saveinspector/save_inspector.h"

#include "save_file.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace saveinspector {
namespace {

// ---- 契约 §2.1 的固定常量 ----
const char kMagic[8] = {'S', 'A', 'V', 'E', 'F', 'I', 'L', 'E'};
const char kFooter[8] = {'E', 'N', 'D', 'S', 'L', 'O', 'T', '\0'};
const size_t kHeaderSize = 32;
const size_t kSlotEntrySize = 24;
const size_t kFooterSize = 8;
const uint32_t kMaxSupportedVersion = 3;

// 契约 §2.1.1：CRC-64/ECMA-182。参数逐位固定，实现方不得选变体——
// 十余种变体互不兼容，三平台各自实现会导致同一存档诊断结果相反。
const uint64_t kCrc64Poly = 0x42F0E1EBA9EA3693ULL;

// CRC-64 的 256 项查表（MSB 先行）。
//
// 为什么必须查表：逐位实现每字节 8 次循环，实测解析 99MB 存档时，仅校验和
// 一项就要几十秒——PRD NFR-2 要求「不长时间无响应」直接不成立。查表后每字节
// 一次移位加一次异或，与 Python 侧已验证的等价实现同一思路。
//
// 查表结果与逐位实现逐位一致（单元测试断言契约向量），两者不会各算各的。
struct Crc64Table {
    uint64_t entries[256];
    Crc64Table() {
        for (int i = 0; i < 256; ++i) {
            uint64_t crc = static_cast<uint64_t>(i) << 56;
            for (int bit = 0; bit < 8; ++bit) {
                if ((crc & 0x8000000000000000ULL) != 0) {
                    crc = (crc << 1) ^ kCrc64Poly;
                } else {
                    crc <<= 1;
                }
            }
            entries[i] = crc;
        }
    }
};

const Crc64Table& Crc64() {
    static const Crc64Table table;   // C++11 起局部静态是线程安全的
    return table;
}

const unsigned char kFlagCompressed = 0x1u;
const unsigned char kFlagEncrypted = 0x2u;

// 单个槽位内各数组的元素数上限。防止 0xFFFFFFFF 个元素触发巨额分配
// （资源耗尽，契约 §3.3）。真实存档的道具/成就数远小于此。
const uint32_t kMaxItems = 4096;
const uint32_t kMaxAchievements = 8192;

// 槽位数上限：100 MB 存档按最小 24B 槽位表算也在万级，留足余量。
const uint32_t kMaxSlots = 100000;

// ---- 边界检查读取器 ----
// 所有对不可信字节的访问都必须经它。任何越界都置 failed 并返回默认值，
// 调用方检查 failed() 后必须放弃本次解析（不得使用已读出的部分）。
class ByteReader {
public:
    ByteReader(const char* data, size_t size)
        : data_(data), size_(size), pos_(0), failed_(false) {}

    bool failed() const { return failed_; }
    size_t pos() const { return pos_; }
    size_t remaining() const { return (pos_ <= size_) ? (size_ - pos_) : 0; }
    bool seek(size_t offset) {
        if (offset > size_) {
            failed_ = true;
            return false;
        }
        pos_ = offset;
        return true;
    }

    bool ReadU8(unsigned char* out) {
        if (remaining() < 1) { failed_ = true; return false; }
        *out = static_cast<unsigned char>(data_[pos_]);
        pos_ += 1;
        return true;
    }

    bool ReadU32(uint32_t* out) {
        if (remaining() < 4) { failed_ = true; return false; }
        const unsigned char* p = reinterpret_cast<const unsigned char*>(data_ + pos_);
        *out = static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
        pos_ += 4;
        return true;
    }

    bool ReadU64(uint64_t* out) {
        if (remaining() < 8) { failed_ = true; return false; }
        const unsigned char* p = reinterpret_cast<const unsigned char*>(data_ + pos_);
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i) {
            v = (v << 8) | static_cast<uint64_t>(p[i]);
        }
        *out = v;
        pos_ += 8;
        return true;
    }

    // 变长数组的元素数：先读计数，**再**用「剩余字节 / 元素大小」校验，
    // 校验通过才允许上层分配。这是防资源耗尽的关键一步。
    bool ReadArrayCount(uint32_t* out, uint32_t limit, size_t min_element_bytes) {
        uint32_t n = 0;
        if (!ReadU32(&n)) return false;
        if (n > limit) { failed_ = true; return false; }
        if (min_element_bytes > 0 && n > 0) {
            const size_t need = static_cast<size_t>(n) * min_element_bytes;
            if (need > remaining()) { failed_ = true; return false; }
        }
        *out = n;
        return true;
    }

private:
    ByteReader(const ByteReader&);
    ByteReader& operator=(const ByteReader&);

    const char* data_;
    size_t size_;
    size_t pos_;
    bool failed_;
};

// ---- JSON 字符串转义（输出 "..." 内部内容）----
std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20u) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

Diagnostic MakeDiag(Severity sev, const std::string& title,
                    const std::string& detail, const std::string& advice) {
    Diagnostic d;
    d.severity = sev;
    d.title = title;
    d.detail = detail;
    d.advice = advice;
    return d;
}

// 槽位号文案：内部 0 基，展示 1 基。
std::string SlotLabel(uint32_t index0) {
    return std::to_string(index0 + 1);
}

}  // namespace

// ---------------------------------------------------------------------------
// 工具函数
// ---------------------------------------------------------------------------

uint64_t Crc64Ecma182(const std::string& bytes) {
    // MSB 先行（契约 §2.1.1：refin/refout 均为否），init=0，xorout=0。
    const Crc64Table& table = Crc64();
    uint64_t crc = 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(bytes.data());
    for (size_t i = 0; i < bytes.size(); ++i) {
        crc = (crc << 8) ^ table.entries[((crc >> 56) ^ p[i]) & 0xFFu];
    }
    return crc;
}

std::string FormatDuration(uint64_t seconds) {
    if (seconds == 0) {
        return std::string("0 分钟");
    }
    const uint64_t hours = seconds / 3600;
    const uint64_t minutes = (seconds % 3600) / 60;
    std::ostringstream os;
    if (hours > 0) {
        os << hours << " 小时";
        if (minutes > 0) os << " " << minutes << " 分";
    } else {
        os << minutes << " 分钟";
    }
    return os.str();
}

std::string FormatTimestamp(uint64_t utc_seconds) {
    if (utc_seconds == 0) {
        return std::string("未知");
    }
    // 用 UTC 直接计算，避免依赖本地时区（工具面向跨时区用户）。
    const uint64_t days = utc_seconds / 86400;
    const uint64_t rem = utc_seconds % 86400;
    const uint64_t hour = rem / 3600;
    const uint64_t minute = (rem % 3600) / 60;

    // civil_from_days（Howard Hinnant 算法）：免 tzset、无平台差异。
    int64_t z = static_cast<int64_t>(days) + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint64_t doe = static_cast<uint64_t>(z - era * 146097);
    const uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = static_cast<int64_t>(yoe) + era * 400;
    const uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint64_t mp = (5 * doy + 2) / 153;
    const uint64_t d = doy - (153 * mp + 2) / 5 + 1;
    const uint64_t m = (mp < 10) ? (mp + 3) : (mp - 9);
    const int64_t year = (m <= 2) ? (y + 1) : y;

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u %02u:%02u",
                  static_cast<int>(year), static_cast<unsigned>(m),
                  static_cast<unsigned>(d), static_cast<unsigned>(hour),
                  static_cast<unsigned>(minute));
    return std::string(buf);
}

std::string ItemName(uint32_t id) {
    switch (id) {
        case 1001: return "治疗药水";
        case 1002: return "铁剑";
        case 1003: return "护符";
        case 1004: return "护甲";
        case 1005: return "地图碎片";
        default: break;
    }
    // 未知 ID 不得返回空串——界面会把空名渲染成「有道具但没名字」。
    return "道具 #" + std::to_string(id);
}

const char* SeverityName(Severity s) {
    switch (s) {
        case Severity::kInfo:    return "info";
        case Severity::kWarning: return "warning";
        case Severity::kError:   return "error";
        default:                return "info";
    }
}

// ---------------------------------------------------------------------------
// SaveInspector
// ---------------------------------------------------------------------------

SaveInspector::SaveInspector() : has_data_(false) {}

namespace {

// 解析一个槽位的数据区（契约 §2.2 的字段顺序）。
// 缺失字段按版本规则留默认值 —— 这就是「迁移」（契约 §2.2）。
//
// 返回 false 表示该槽位**无法解析**，此时 detail 内容不可信，调用方应
// 只报告诊断而不展示数据（PRD US-3 要求：能读多少展示多少）。
bool ParseSlotData(const std::string& bytes, size_t offset, size_t length,
                   FormatVersion version, SlotDetail* detail) {
    ByteReader r(bytes.data() + offset, length);

    if (!r.ReadU32(&detail->summary.level)) return false;
    if (!r.ReadU64(&detail->summary.coins)) return false;

    if (version >= FormatVersion::kV2) {
        if (!r.ReadU64(&detail->summary.playtime_sec)) return false;
        uint32_t item_count = 0;
        if (!r.ReadArrayCount(&item_count, kMaxItems, 8)) return false;
        detail->items.reserve(item_count);
        for (uint32_t i = 0; i < item_count; ++i) {
            Item item;
            if (!r.ReadU32(&item.id)) return false;
            if (!r.ReadU32(&item.count)) return false;
            item.name = ItemName(item.id);
            detail->items.push_back(item);
        }
    }

    if (version >= FormatVersion::kV3) {
        uint32_t ach_count = 0;
        if (!r.ReadArrayCount(&ach_count, kMaxAchievements, 4)) return false;
        detail->achievements.reserve(ach_count);
        for (uint32_t i = 0; i < ach_count; ++i) {
            uint32_t id = 0;
            if (!r.ReadU32(&id)) return false;
            detail->achievements.push_back(id);
        }
        if (!r.ReadU64(&detail->summary.last_played_utc)) return false;
        if (!r.ReadU64(&detail->signature)) return false;
    }

    // 存在未消费的尾随字节 = 该版本下的结构错位（可能被外部编辑过）。
    // 视为解析失败更安全：展示错位数据比明确报错更糟。
    return !r.failed() && r.remaining() == 0;
}

}  // namespace

bool SaveInspector::OpenBytes(const std::string& bytes, SaveFile* out,
                              std::string* error) {
    save_ = SaveFile();
    has_data_ = false;
    if (out != NULL) *out = SaveFile();

    // ---- 文件头 ----
    if (bytes.size() < kHeaderSize) {
        if (error != NULL) {
            *error = "这个文件太小，不像是存档。请确认是否选错了文件。";
        }
        return false;
    }
    if (std::memcmp(bytes.data(), kMagic, 8) != 0) {
        if (error != NULL) {
            *error = "这不是存档文件。请确认选择的是游戏的存档文件。";
        }
        return false;
    }

    ByteReader header(bytes.data(), kHeaderSize);
    header.seek(8);
    uint32_t raw_version = 0;
    uint32_t flags = 0;
    uint32_t slot_count = 0;
    uint32_t reserved = 0;
    if (!header.ReadU32(&raw_version) || !header.ReadU32(&flags) ||
        !header.ReadU32(&slot_count) || !header.ReadU32(&reserved) ||
        !header.ReadU64(&save_.created_utc)) {
        if (error != NULL) *error = "存档文件头已损坏。";
        return false;
    }

    // ---- 版本 ----
    if (raw_version == 0 || raw_version > kMaxSupportedVersion) {
        if (error != NULL) {
            *error = "这个存档来自更新版本的游戏。请升级游戏后再打开。";
        }
        return false;
    }
    const FormatVersion version = static_cast<FormatVersion>(raw_version);

    // ---- 加密标记：不尝试破解（契约 §2.3）----
    if ((flags & kFlagEncrypted) != 0) {
        if (error != NULL) {
            *error = "这个存档已加密，需要游戏本体才能读取。";
        }
        return false;
    }
    if ((flags & kFlagCompressed) != 0) {
        save_.diagnostics.push_back(MakeDiag(
            Severity::kWarning, "这个存档使用了压缩",
            "本工具暂不解压压缩存档，部分内容可能无法显示。",
            "建议先用游戏本体打开一次并重新存档。"));
    }

    // ---- 槽位表边界 ----
    if (slot_count > kMaxSlots) {
        if (error != NULL) *error = "存档的槽位数量异常，文件可能已损坏。";
        return false;
    }
    const size_t table_end = kHeaderSize +
                             static_cast<size_t>(slot_count) * kSlotEntrySize;
    if (table_end + kFooterSize > bytes.size()) {
        if (error != NULL) {
            *error = "存档不完整，可能在保存时被中断。";
        }
        return false;
    }

    // ---- 尾部标记 ----
    if (std::memcmp(bytes.data() + bytes.size() - kFooterSize, kFooter, 8) != 0) {
        if (error != NULL) {
            *error = "存档不完整，可能在保存时被中断。";
        }
        return false;
    }

    save_.version = version;
    if (version < FormatVersion::kV3) {
        const uint32_t from = static_cast<uint32_t>(version);
        // 中文文案不进 snprintf 的格式串：MSVC 的格式串检查器遇非 ASCII 会
        // 转码失败并报 C4819（即使已加 /utf-8），还可能中止扫描而误判占位符
        // 数量（C4474）。带插值时先用 std::string 拼接，格式串保持纯 ASCII。
        save_.diagnostics.push_back(MakeDiag(
            Severity::kInfo, "已从 v" + std::to_string(from) + " 升级到 v3 读取",
            "旧版本存档缺少部分字段，我们按默认值补齐。",
            "无需操作；你的原始文件没有被修改，导出内容会包含补齐后的字段。"));
    }

    // ---- 逐槽位解析 ----
    ByteReader table(bytes.data(), bytes.size());
    if (!table.seek(kHeaderSize)) {
        if (error != NULL) *error = "存档的槽位表已损坏。";
        return false;
    }

    save_.slots.reserve(slot_count);
    for (uint32_t i = 0; i < slot_count; ++i) {
        uint64_t offset = 0;
        uint64_t length = 0;
        uint64_t stored_crc = 0;
        if (!table.ReadU64(&offset) || !table.ReadU64(&length) ||
            !table.ReadU64(&stored_crc)) {
            if (error != NULL) *error = "存档的槽位表不完整。";
            return false;
        }

        SlotDetail detail;
        detail.summary.index = i;
        detail.source_version = version;
        detail.upgraded = (version < FormatVersion::kV3);
        const std::string label = SlotLabel(i);

        // 偏移/长度越界：在使用前校验，且避免 offset+length 溢出。
        const bool in_range =
            (offset >= kHeaderSize && length > 0 &&
             offset <= static_cast<uint64_t>(bytes.size()) &&
             length <= static_cast<uint64_t>(bytes.size()) - offset &&
             offset + length <= static_cast<uint64_t>(bytes.size() - kFooterSize));

        if (!in_range) {
            detail.summary.healthy = false;
            save_.diagnostics.push_back(MakeDiag(
                Severity::kWarning,
                "槽位 " + label + " 的数据位置异常",
                "这个槽位在文件里的位置超出了文件范围，通常是文件被截断或改坏了。",
                "其余槽位仍可正常使用；建议用游戏本体打开一次并重新存档。"));
            save_.slots.push_back(detail);
            continue;
        }

        const std::string data = bytes.substr(
            static_cast<size_t>(offset), static_cast<size_t>(length));

        // 校验和：先验，比解析便宜，且能直接定位「被改动过」。
        const uint64_t actual_crc = Crc64Ecma182(data);
        if (actual_crc != stored_crc) {
            detail.summary.healthy = false;
            save_.diagnostics.push_back(MakeDiag(
                Severity::kWarning,
                "槽位 " + label + " 的数据已被改动",
                "这个槽位的数据与游戏写入时的记录不一致，通常是被其他存档编辑工具改过，"
                "或者在保存过程中断电。",
                "建议先用游戏本体打开一次并重新存档；重新存档前请先备份这个文件。"));
            // 校验和不符时**仍然尝试解析**——PRD US-3 要求「部分损坏仍显示
            // 能读出的部分」。解析失败也不应让整个界面拒绝展示。
            if (!ParseSlotData(data, 0, data.size(), version, &detail)) {
                detail.summary.healthy = false;
            }
            save_.slots.push_back(detail);
            continue;
        }

        if (!ParseSlotData(data, 0, data.size(), version, &detail)) {
            detail.summary.healthy = false;
            save_.diagnostics.push_back(MakeDiag(
                Severity::kWarning,
                "槽位 " + label + " 的数据无法完整读取",
                "这个槽位的内容与它声明的格式对不上，文件可能在保存时被中断。",
                "其余槽位仍可正常使用；建议用游戏本体打开一次并重新存档。"));
            save_.slots.push_back(detail);
            continue;
        }

        save_.slots.push_back(detail);
    }

    has_data_ = true;
    if (out != NULL) *out = save_;
    return true;
}

bool SaveInspector::OpenFile(const std::string& path, SaveFile* out,
                             std::string* error) {
    std::string bytes;
    std::string read_error;
    if (!ReadFileBytesReadOnly(path, &bytes, &read_error)) {
        if (error != NULL) *error = read_error;
        return false;
    }
    return OpenBytes(bytes, out, error);
}

bool SaveInspector::ReadSlot(uint32_t index, SlotDetail* out) const {
    if (out == NULL) return false;
    if (index >= save_.slots.size()) return false;
    *out = save_.slots[index];
    return true;
}

std::vector<SlotSummary> SaveInspector::ListSlots() const {
    std::vector<SlotSummary> list;
    list.reserve(save_.slots.size());
    for (size_t i = 0; i < save_.slots.size(); ++i) {
        list.push_back(save_.slots[i].summary);
    }
    return list;
}

std::string SaveInspector::SlotToJson(const SlotDetail& slot) const {
    std::ostringstream os;
    os << "{\"slot\":" << (static_cast<uint32_t>(slot.summary.index) + 1)
       << ",\"level\":" << slot.summary.level
       << ",\"coins\":" << slot.summary.coins
       << ",\"playtime_sec\":" << slot.summary.playtime_sec
       << ",\"playtime\":\"" << JsonEscape(FormatDuration(slot.summary.playtime_sec))
       << "\",\"last_played\":\"" << JsonEscape(FormatTimestamp(slot.summary.last_played_utc))
       << "\",\"healthy\":" << (slot.summary.healthy ? "true" : "false")
       << ",\"upgraded\":" << (slot.upgraded ? "true" : "false")
       << ",\"items\":[";
    for (size_t i = 0; i < slot.items.size(); ++i) {
        if (i != 0) os << ",";
        os << "{\"id\":" << slot.items[i].id
           << ",\"count\":" << slot.items[i].count
           << ",\"name\":\"" << JsonEscape(slot.items[i].name) << "\"}";
    }
    os << "],\"achievements\":[";
    for (size_t i = 0; i < slot.achievements.size(); ++i) {
        if (i != 0) os << ",";
        os << slot.achievements[i];
    }
    os << "],\"signature\":" << slot.signature << "}";
    return os.str();
}

std::string SaveInspector::SlotToTextReport(const SlotDetail& slot) const {
    std::ostringstream os;
    os << "===== 存档报告 =====\n";
    os << "槽位：" << (static_cast<uint32_t>(slot.summary.index) + 1) << "\n";
    os << "当前关卡：第 " << slot.summary.level << " 章\n";
    os << "累计金币：" << slot.summary.coins << "\n";
    os << "累计游玩：" << FormatDuration(slot.summary.playtime_sec) << "\n";
    os << "最近游玩：" << FormatTimestamp(slot.summary.last_played_utc) << "\n";
    os << "数据状态：" << (slot.summary.healthy ? "正常" : "存在异常") << "\n";
    if (slot.upgraded) {
        os << "备注：本槽位由旧版本存档升级读取，原始文件未被修改。\n";
    }
    os << "\n道具（" << slot.items.size() << "）\n";
    for (size_t i = 0; i < slot.items.size(); ++i) {
        os << "  - " << slot.items[i].name << " × " << slot.items[i].count << "\n";
    }
    os << "\n已解锁成就（" << slot.achievements.size() << "）\n";
    for (size_t i = 0; i < slot.achievements.size(); ++i) {
        os << "  - 成就 #" << slot.achievements[i] << "\n";
    }
    os << "\n本报告由存档查看器生成，原始存档未被修改。\n";
    return os.str();
}

std::string SaveInspector::SaveToJson(const SaveFile& save) const {
    std::ostringstream os;
    os << "{\"ok\":true,\"save\":{";
    os << "\"version\":" << static_cast<uint32_t>(save.version)
       << ",\"created\":\"" << JsonEscape(FormatTimestamp(save.created_utc))
       << "\",\"slots\":[";
    for (size_t i = 0; i < save.slots.size(); ++i) {
        if (i != 0) os << ",";
        os << SlotToJson(save.slots[i]);
    }
    os << "],\"diagnostics\":[";
    for (size_t i = 0; i < save.diagnostics.size(); ++i) {
        if (i != 0) os << ",";
        const Diagnostic& d = save.diagnostics[i];
        os << "{\"severity\":\"" << SeverityName(d.severity)
           << "\",\"title\":\"" << JsonEscape(d.title)
           << "\",\"detail\":\"" << JsonEscape(d.detail)
           << "\",\"advice\":\"" << JsonEscape(d.advice) << "\"}";
    }
    os << "]}}";
    return os.str();
}

}  // namespace saveinspector
