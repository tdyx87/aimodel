// 角色：cpp-implementer
// 任务：存档查看器核心库（格式解析 / 版本迁移 / 完整性诊断 / 导出）
// 产物：include/saveinspector/save_inspector.h、src/cpp/save_inspector.cpp
// 契约：docs/design/save-format-contract.md（审计 2026-10-13-architect-save-format.md）
//
// C++11 合规（agents.md §7.1）：不使用 std::optional / make_unique / 泛型 lambda /
// 初始化捕获。需要「可能无值」时用 `bool + T` 组合，需要 unique_ptr 时显式 new。
//
// 隐私红线（契约 §5）：本库**只读**打开存档，API 层面不提供任何写入入口，
// 因此不存在误改用户存档的代码路径（可被 security-reviewer 以「产物中不存在
// 写入 API」为判据核验）。

#ifndef SAVEINSPECTOR_SAVE_INSPECTOR_H_
#define SAVEINSPECTOR_SAVE_INSPECTOR_H_

#include <cstdint>
#include <string>
#include <vector>

#include "saveinspector/export.h"

namespace saveinspector {

// 存档格式版本（契约 §2.2）。枚举值即文件头中的 format_ver。
enum class FormatVersion {
    kV1 = 1,  // 仅 关卡 + 金币
    kV2 = 2,  // 增加 道具 + 游玩时长
    kV3 = 3   // 增加 成就 + 最近游玩 + 签名
};

// 诊断条目严重级。三档都必须真的用到：只用两档会让「已升级」这类信息性
// 事项无处安放（契约 §5.1 遗留项）。
enum class Severity {
    kInfo,     // 信息（如「已从 v1 升级读取」）
    kWarning,  // 部分可用（如某槽位校验和不匹配）
    kError     // 致命（整个文件无法解析）
};

// 一条面向用户的诊断。三个文本字段均为自然语言，**不得**填入错误码、
// 文件偏移量或堆栈（PRD US-5 硬约束）。
struct Diagnostic {
    Severity severity;
    std::string title;   // 哪里坏了
    std::string detail;  // 为什么坏
    std::string advice;  // 我能做什么（不得为空）
};

// 道具。name 由核心库填充——前端不维护映射表（契约 §3.2）。
// 未知 ID 的 name 为「道具 #<id>」，不返回空串。
struct Item {
    uint32_t id = 0;
    uint32_t count = 0;
    std::string name;
};

// 槽位摘要（列表视图用）。
struct SlotSummary {
    uint32_t index = 0;            // 0 基；界面显示时 +1
    uint32_t level = 0;
    uint64_t coins = 0;
    uint64_t playtime_sec = 0;
    uint64_t last_played_utc = 0;  // 0 = 未知
    bool healthy = true;
};

// 槽位详情。
struct SlotDetail {
    SlotSummary summary;
    std::vector<Item> items;
    std::vector<uint32_t> achievements;
    uint64_t signature = 0;
    bool upgraded = false;            // 是否由旧版本迁移而来
    FormatVersion source_version = FormatVersion::kV3;
};

// 解析结果。
struct SaveFile {
    FormatVersion version = FormatVersion::kV3;
    uint64_t created_utc = 0;
    std::vector<SlotDetail> slots;
    std::vector<Diagnostic> diagnostics;
};

// 存档读取器。
//
// 生命周期：Open 之后对象持有解析结果，可重复查询；重新 Open 会覆盖。
// 线程安全：未加锁，实例由调用方串行使用（HTTP 层每个工作线程各自持有实例）。
class SAVEINSPECTOR_API SaveInspector {
public:
    SaveInspector();

    // 只读打开文件。**绝不**写入、修改或删除 path 指向的文件（PRD NFR-5）。
    // 失败返回 false，并把可向用户展示的原因写入 error（面向用户的中文，
    // 不含路径与偏移量）；部分损坏时返回 true，且 out->diagnostics 载明问题。
    bool OpenFile(const std::string& path, SaveFile* out, std::string* error);

    // 从内存字节解析（供测试、拖放、粘贴复用同一套逻辑）。
    bool OpenBytes(const std::string& bytes, SaveFile* out, std::string* error);

    // 按 0 基索引取槽位详情。索引越界返回 false。
    bool ReadSlot(uint32_t index, SlotDetail* out) const;

    std::vector<SlotSummary> ListSlots() const;

    // 导出。JSON 供机器消费，文本报告供用户贴进工单（PRD US-4 / US-7）。
    // 两者均**不含**本机路径（契约 §5）。
    std::string SlotToJson(const SlotDetail& slot) const;
    std::string SlotToTextReport(const SlotDetail& slot) const;

    // 整个存档的 JSON（/api/save/open 的响应体）。
    std::string SaveToJson(const SaveFile& save) const;

    bool has_data() const { return has_data_; }

    // 只读访问已解析结果（HTTP 层序列化用）。
    const SaveFile& save() const { return save_; }

    // 清空已加载的存档。用于「打开新文件失败」的场景：调用方已切到诊断视图，
    // 若不清空，/api/save/slots 会继续返回上一个存档的数据，造成服务端状态
    // 与界面所见不一致。
    void Clear() {
        save_ = SaveFile();
        has_data_ = false;
    }

private:
    SaveFile save_;
    bool has_data_;
};

// ---- 工具函数（对外暴露以便测试与前端复用）----

// CRC-64/ECMA-182，参数见契约 §2.1.1。对 "123456789" 必须返回
// 0x6C40DF5F0B497347 —— 这是跨平台一致性的唯一可验证手段。
SAVEINSPECTOR_API uint64_t Crc64Ecma182(const std::string& bytes);

// 秒 → 「128 小时 12 分」这类中文可读文案（0 → 「0 分钟」）。
SAVEINSPECTOR_API std::string FormatDuration(uint64_t seconds);

// Unix 秒 → 「2026-03-14 21:08」；0 → 「未知」。
SAVEINSPECTOR_API std::string FormatTimestamp(uint64_t utc_seconds);

// 道具 ID → 名称；未知 ID 返回「道具 #<id>」。
SAVEINSPECTOR_API std::string ItemName(uint32_t id);

// 严重级 → JSON 字符串（"info" / "warning" / "error"）。
SAVEINSPECTOR_API const char* SeverityName(Severity s);

}  // namespace saveinspector

#endif  // SAVEINSPECTOR_SAVE_INSPECTOR_H_
