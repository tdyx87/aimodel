// 角色：cpp-implementer
// 任务：实现 chat_suggest 后端（本地规则式建议）
// 产物：src/cpp/chat_suggest.cpp
// 契约：docs/design/chat-system-contract.md（审计 2026-10-12-architect-chat-system.md）
//
// C++11 合规（agents.md §7.1）：不使用 <filesystem>/std::optional 等 C++14+ 设施；
// 本模块**平台无关**（纯计算 + 字符串），不引入任何平台 API，故无需 *_win32/posix 分支。
//
// 隐私架构（契约 §5）：BuildChatAdvice 是纯函数——不联网、不落盘、不读环境；
// 建议全部由传入的 SystemInfo 本地推导，数据不出本机。

#include "aimodel/chat_suggest.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace aimodel {
namespace {

const double kGiB = 1024.0 * 1024.0 * 1024.0;

// 粗略换算系数（用于「目标规模 → 所需显存」的量级估计，非精确值）：
//   - 4-bit 量化：约 0.6 GB / B（权重 + 少量 KV/运行时开销）
//   - fp16 高精度：约 2.0 GB / B
// 说明：这是本地启发式，用于给出「能跑 / 需检查 / 不建议」的粗略分类，
// 不代表任何具体模型的真实占用（PRD US-4 要求如实标注不确定性）。
const double kQuantGbPerBillion = 0.6;
const double kFp16GbPerBillion = 2.0;

// 不确定性声明：契约 §3.3 要求响应必须含「明确的不确定性说明」。
const char kUncertaintyNote[] =
    "这是基于你当前硬件报告的粗略推荐，未调用外部模型；"
    "如需精确判断，请按下列步骤核对。";

std::string FormatGb(double gb) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f GB", gb);
    return std::string(buf);
}

// 模型规模显示：整数去掉小数点，非整数保留一位（如 7、1.5）。
std::string FormatBillions(double billions) {
    char buf[64];
    const long long as_int = static_cast<long long>(billions);
    if (billions == static_cast<double>(as_int)) {
        std::snprintf(buf, sizeof(buf), "%lld", as_int);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f", billions);
    }
    return std::string(buf);
}

// 取所有显卡中最大的显存值（无独显/显存未知时为 0）。
uint64_t MaxVideoMemory(const SystemInfo& info) {
    uint64_t max_bytes = 0;
    for (size_t i = 0; i < info.gpus.size(); ++i) {
        if (info.gpus[i].video_memory_bytes > max_bytes) {
            max_bytes = info.gpus[i].video_memory_bytes;
        }
    }
    return max_bytes;
}

// 取所有分区中最大的剩余空间（无分区时为 0）。
uint64_t MaxDiskFree(const SystemInfo& info) {
    uint64_t max_free = 0;
    for (size_t i = 0; i < info.disks.size(); ++i) {
        if (info.disks[i].free_bytes > max_free) {
            max_free = info.disks[i].free_bytes;
        }
    }
    return max_free;
}

// 从问题文本中解析用户想跑的模型规模（如 "7B"、"13b"、"1.5B"）。
// 规则：数字串（可含一个小数点）后紧跟 'b'/'B'；"4-bit" 因 'b' 前是 '-' 不会误判。
// 未提及规模时返回 0。
double ParseTargetBillions(const std::string& query) {
    for (size_t i = 0; i < query.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(query[i]);
        if (std::isdigit(c) == 0) continue;
        // 数字串中间不重复起头（前一个字符是数字或小数点则跳过）。
        if (i > 0) {
            const unsigned char prev = static_cast<unsigned char>(query[i - 1]);
            if (std::isdigit(prev) != 0 || prev == '.') continue;
        }
        std::string num;
        bool seen_dot = false;
        size_t j = i;
        while (j < query.size()) {
            const char d = query[j];
            if (std::isdigit(static_cast<unsigned char>(d)) != 0) {
                num.push_back(d);
                ++j;
                continue;
            }
            if (d == '.' && !seen_dot) {
                seen_dot = true;
                num.push_back(d);
                ++j;
                continue;
            }
            break;
        }
        if (j < query.size() && (query[j] == 'b' || query[j] == 'B')) {
            char* end = NULL;
            const double value = std::strtod(num.c_str(), &end);
            if (end != num.c_str() && value > 0.0) {
                return value;
            }
        }
    }
    return 0.0;
}

// 用户未指定规模时，按硬件给一个「合适档位」作为推荐目标。
double RecommendedTierBillions(double vram_gb, double ram_total_gb) {
    if (vram_gb >= 16.0) return 13.0;
    if (vram_gb >= 8.0) return 7.0;
    if (vram_gb >= 4.0) return 3.0;
    if (vram_gb >= 2.0) return 1.5;
    if (ram_total_gb >= 32.0) return 7.0;
    if (ram_total_gb >= 16.0) return 3.0;
    return 1.5;
}

}  // namespace

const char* ChatCategoryName(ChatCategory category) {
    switch (category) {
        case ChatCategory::kRecommend:      return "recommend";
        case ChatCategory::kNotRecommended: return "not_recommended";
        case ChatCategory::kCheck:
        default:                            return "check";
    }
}

ChatAdvice BuildChatAdvice(const SystemInfo& info, const std::string& query) {
    ChatAdvice advice;

    const std::string trimmed = detail::Trim(query);
    if (trimmed.empty()) {
        advice.ok = false;
        advice.error_code = "INVALID_QUERY";
        advice.error_message = "当前未检测到可用问题文本。请求必须包含非空的 query。";
        return advice;
    }

    const double vram_gb = static_cast<double>(MaxVideoMemory(info)) / kGiB;
    const double ram_total_gb = static_cast<double>(info.memory.total_bytes) / kGiB;
    const double ram_avail_gb = static_cast<double>(info.memory.available_bytes) / kGiB;
    const double disk_free_gb = static_cast<double>(MaxDiskFree(info)) / kGiB;

    double target = ParseTargetBillions(trimmed);
    const bool explicit_target = target > 0.0;
    if (!explicit_target) {
        target = RecommendedTierBillions(vram_gb, ram_total_gb);
    }

    const double need_quant_gb = target * kQuantGbPerBillion;
    const double need_fp16_gb = target * kFp16GbPerBillion;

    ChatCategory category = ChatCategory::kCheck;
    std::string lead;
    if (vram_gb <= 0.0) {
        if (ram_avail_gb >= need_quant_gb && ram_total_gb >= need_quant_gb) {
            category = ChatCategory::kCheck;
            lead = "未检测到可用显存，只能以 CPU 方式运行，速度会明显偏慢。";
        } else {
            category = ChatCategory::kNotRecommended;
            lead = "未检测到可用显存，且可用内存不足以承载该规模模型。";
        }
    } else if (vram_gb >= need_fp16_gb) {
        category = ChatCategory::kRecommend;
        lead = "显存充足，可直接以较高精度运行该规模模型。";
    } else if (vram_gb >= need_quant_gb) {
        category = ChatCategory::kCheck;
        lead = "显存不足以按高精度运行，需改用 4-bit 量化方案。";
    } else {
        category = ChatCategory::kNotRecommended;
        lead = "显存低于该规模模型的最低要求，建议选择更小的模型。";
    }

    const std::string vram_text =
        (vram_gb > 0.0) ? (FormatGb(vram_gb) + " 显存")
                        : std::string("未检测到独立显存");
    const std::string target_text = FormatBillions(target) + "B";

    // reason 必含：具体数值（US-2）+ 不确定性声明（US-4）。
    std::ostringstream reason;
    reason << lead
           << "目标规模约 " << target_text << "，当前 " << vram_text << "；"
           << "4-bit 量化约需 " << FormatGb(need_quant_gb)
           << "，高精度约需 " << FormatGb(need_fp16_gb) << "。"
           << kUncertaintyNote;

    ChatRecommendation rec;
    rec.category = category;
    rec.reason = reason.str();

    // ---- 下一步（始终给出可执行动作，含「先检查」）----
    ChatStep step_vram;
    step_vram.title = "先确认显存";
    step_vram.detail = "当前" + vram_text + "。";
    if (vram_gb <= 0.0) {
        step_vram.action =
            "如需显卡加速，请确认已安装独立显卡及其驱动；"
            "否则请接受 CPU 推理的速度损失。";
    } else {
        step_vram.action =
            "如果目标模型是 " + target_text +
            "，建议优先确认显存是否满足；否则改用更轻量的 4-bit 量化方案。";
    }
    rec.next_steps.push_back(step_vram);

    ChatStep step_ram;
    step_ram.title = "核对内存可用量";
    step_ram.detail = "当前内存总量约 " + FormatGb(ram_total_gb) +
                      "，可用约 " + FormatGb(ram_avail_gb) + "。";
    step_ram.action =
        "若运行任务需要较大工作集，建议先关闭后台程序或改用更小模型。";
    rec.next_steps.push_back(step_ram);

    ChatStep step_disk;
    step_disk.title = "检查磁盘剩余空间";
    step_disk.detail = "最大可用分区剩余约 " + FormatGb(disk_free_gb) + "。";
    step_disk.action =
        "下载 " + target_text + " 的 4-bit 量化模型约需 " +
        FormatGb(need_quant_gb) + " 空间，请预留余量。";
    rec.next_steps.push_back(step_disk);

    ChatStep step_quant;
    step_quant.title = "选择量化方案";
    step_quant.detail = "4-bit 量化在体积与质量之间较为均衡。";
    step_quant.action =
        "优先选择社区常用的 4-bit 量化版本，避免直接加载高精度权重。";
    rec.next_steps.push_back(step_quant);

    advice.recommendation = rec;
    return advice;
}

std::string ChatAdviceToJson(const SystemInfo& info, const std::string& query,
                             const ChatAdvice& advice) {
    std::ostringstream os;
    if (!advice.ok) {
        // 契约 §2.2 ErrorResponse。
        os << "{\"ok\":false,\"error\":{\"code\":\""
           << detail::JsonEscape(advice.error_code) << "\",\"message\":\""
           << detail::JsonEscape(advice.error_message) << "\"}}";
        return os.str();
    }

    const ChatRecommendation& rec = advice.recommendation;
    os << "{\"ok\":true,\"request\":{\"query\":\"" << detail::JsonEscape(query)
       << "\"},"
       // config_snapshot 复用 system_info 的 ToJson（同一份采集数据），
       // 其字段是契约 §2.2 ConfigSnapshot 的超集（另含 hostname/printers）。
       << "\"config_snapshot\":" << ToJson(info) << ","
       << "\"recommendations\":[{"
       << "\"category\":\"" << ChatCategoryName(rec.category) << "\","
       << "\"reason\":\"" << detail::JsonEscape(rec.reason) << "\","
       << "\"next_steps\":[";
    for (size_t i = 0; i < rec.next_steps.size(); ++i) {
        if (i != 0) os << ",";
        os << "{\"title\":\"" << detail::JsonEscape(rec.next_steps[i].title)
           << "\",\"detail\":\"" << detail::JsonEscape(rec.next_steps[i].detail)
           << "\",\"action\":\"" << detail::JsonEscape(rec.next_steps[i].action)
           << "\"}";
    }
    os << "]}],"
       << "\"meta\":{\"source\":\"local-system-info\","
       << "\"model_kind\":\"local-rule-based-suggestion\","
       << "\"privacy_scope\":\"local-only\"}}";
    return os.str();
}

std::string ChatCapabilityJson() {
    std::ostringstream os;
    os << "{\"ok\":true,\"capability\":{"
       << "\"endpoint\":\"/api/chat/suggest\",\"method\":\"POST\","
       << "\"request\":{\"query\":\"string\"},"
       << "\"categories\":[\"recommend\",\"check\",\"not_recommended\"],"
       << "\"model_kind\":\"local-rule-based-suggestion\","
       << "\"privacy_scope\":\"local-only\","
       << "\"description\":\""
       << detail::JsonEscape(
              "基于本机硬件报告的本地规则建议，不调用外部模型，数据不出本机。")
       << "\"}}";
    return os.str();
}

}  // namespace aimodel
