// 角色：cpp-implementer
// 任务：聊天/推荐服务端接口（本地规则式建议）
// 产物：include/aimodel/chat_suggest.h
// 契约：docs/design/chat-system-contract.md
// 审计：docs/audit/2026-10-12-cpp-implementer-chat-system-impl.md

#ifndef AIMODEL_CHAT_SUGGEST_H_
#define AIMODEL_CHAT_SUGGEST_H_

#include <string>
#include <vector>

#include "aimodel/export.h"
#include "aimodel/system_info.h"

namespace aimodel {

// 推荐分类（契约 §2.2 Recommendation.category）。
enum class ChatCategory {
    kRecommend = 0,    // "recommend"       适合当前硬件运行
    kCheck,            // "check"           需要进一步检查或配置
    kNotRecommended    // "not_recommended" 对当前硬件不推荐
};

// 分类的契约字符串（"recommend" / "check" / "not_recommended"）。
AIMODEL_API const char* ChatCategoryName(ChatCategory category);

// 一条下一步建议（契约 §2.2 Recommendation.next_steps[]）。
struct ChatStep {
    std::string title;   // 短标题
    std::string detail;  // 当前值 / 事实
    std::string action;  // 可执行动作
};

// 推荐项（契约 §2.2 Recommendation）。
struct ChatRecommendation {
    ChatCategory category = ChatCategory::kCheck;
    std::string reason;                 // 推荐理由 / 限制说明（含不确定性声明）
    std::vector<ChatStep> next_steps;   // 下一步可执行建议
};

// 一次建议的完整结果。
// ok=false 时 error_code / error_message 有效（契约 §2.2 ErrorResponse）。
struct ChatAdvice {
    bool ok = true;
    std::string error_code;     // 例如 "INVALID_QUERY"
    std::string error_message;  // 面向用户的中文提示
    ChatRecommendation recommendation;
};

// 依据本机系统信息与用户问题，产出**本地规则式**建议。
//
// 隐私边界（契约 §5）：纯函数——不访问网络、不落盘、不读环境变量；
// 仅使用传入的 SystemInfo 与 query 做本地计算。
//
// query 为空或全为空白 → ok=false, error_code="INVALID_QUERY"。
AIMODEL_API ChatAdvice BuildChatAdvice(const SystemInfo& info,
                                       const std::string& query);

// 契约 §2.2 的响应序列化（UTF-8）：
//   ok=true  → ChatResponse   { ok, request, config_snapshot, recommendations, meta }
//   ok=false → ErrorResponse  { ok:false, error:{code,message} }
AIMODEL_API std::string ChatAdviceToJson(const SystemInfo& info,
                                         const std::string& query,
                                         const ChatAdvice& advice);

// 契约 §3.2 GET /api/chat/capability 的能力说明 JSON。
AIMODEL_API std::string ChatCapabilityJson();

}  // namespace aimodel

#endif  // AIMODEL_CHAT_SUGGEST_H_
