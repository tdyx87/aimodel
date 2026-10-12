// 单元测试：ChatSuggest 的本地规则式建议与 JSON 契约。
// 契约：docs/design/chat-system-contract.md（§2.2 数据模型、§3 服务边界、§5 隐私）。
// 断言结构/不变量（分类判定、必含字段、隐私声明），不断言用例特有的具体数值。

#include "aimodel/chat_suggest.h"

#include <gtest/gtest.h>

#include <string>

namespace {

const uint64_t kGiB = 1024ull * 1024ull * 1024ull;

// 构造一份可控的 SystemInfo：vram_bytes=0 表示无独立显卡。
aimodel::SystemInfo MakeInfo(uint64_t vram_bytes, uint64_t ram_total,
                             uint64_t ram_avail, uint64_t disk_free) {
    aimodel::SystemInfo info;
    info.os_name = "Windows";
    info.architecture = "x86_64";

    if (vram_bytes > 0) {
        aimodel::GpuInfo gpu;
        gpu.vendor = "TestVendor";
        gpu.name = "Test GPU";
        gpu.video_memory_bytes = vram_bytes;
        info.gpus.push_back(gpu);
    }

    info.memory.total_bytes = ram_total;
    info.memory.available_bytes = ram_avail;

    aimodel::DiskInfo disk;
    disk.mount_point = "C:\\";
    disk.file_system = "NTFS";
    disk.total_bytes = disk_free + kGiB;
    disk.free_bytes = disk_free;
    info.disks.push_back(disk);

    aimodel::CpuInfo cpu;
    cpu.brand = "Test CPU";
    cpu.core_count = 8;
    info.cpus.push_back(cpu);

    return info;
}

}  // namespace

TEST(ChatSuggestTest, EmptyQueryIsRejected) {
    const aimodel::SystemInfo info = MakeInfo(8 * kGiB, 32 * kGiB, 16 * kGiB, 100 * kGiB);
    const aimodel::ChatAdvice advice = aimodel::BuildChatAdvice(info, "");
    EXPECT_FALSE(advice.ok);
    EXPECT_EQ(advice.error_code, std::string("INVALID_QUERY"));
    EXPECT_FALSE(advice.error_message.empty());
}

TEST(ChatSuggestTest, WhitespaceOnlyQueryIsRejected) {
    const aimodel::SystemInfo info = MakeInfo(8 * kGiB, 32 * kGiB, 16 * kGiB, 100 * kGiB);
    const aimodel::ChatAdvice advice = aimodel::BuildChatAdvice(info, "   \t\n ");
    EXPECT_FALSE(advice.ok);
    EXPECT_EQ(advice.error_code, std::string("INVALID_QUERY"));
}

TEST(ChatSuggestTest, StrongGpuRecommendsTargetModel) {
    // 24 GB 显存 + 明确提到 7B：高精度余量充足 → recommend。
    const aimodel::SystemInfo info = MakeInfo(24 * kGiB, 64 * kGiB, 48 * kGiB, 400 * kGiB);
    const aimodel::ChatAdvice advice =
        aimodel::BuildChatAdvice(info, "我这电脑能跑 7B 模型吗？");
    ASSERT_TRUE(advice.ok);
    EXPECT_EQ(advice.recommendation.category, aimodel::ChatCategory::kRecommend);
}

TEST(ChatSuggestTest, MediumGpuNeedsQuantization) {
    // 6 GB 显存跑 7B：不足以高精度、但够 4-bit → check。
    const aimodel::SystemInfo info = MakeInfo(6 * kGiB, 32 * kGiB, 20 * kGiB, 200 * kGiB);
    const aimodel::ChatAdvice advice = aimodel::BuildChatAdvice(info, "7B 能跑吗");
    ASSERT_TRUE(advice.ok);
    EXPECT_EQ(advice.recommendation.category, aimodel::ChatCategory::kCheck);
}

TEST(ChatSuggestTest, WeakGpuIsNotRecommended) {
    // 2 GB 显存跑 7B：低于最低要求 → not_recommended。
    const aimodel::SystemInfo info = MakeInfo(2 * kGiB, 16 * kGiB, 8 * kGiB, 200 * kGiB);
    const aimodel::ChatAdvice advice = aimodel::BuildChatAdvice(info, "跑 7B");
    ASSERT_TRUE(advice.ok);
    EXPECT_EQ(advice.recommendation.category, aimodel::ChatCategory::kNotRecommended);
}

TEST(ChatSuggestTest, NoGpuFallsBackToCpuWhenMemorySuffices) {
    // 无独显但内存充足：只能 CPU 推理 → check。
    const aimodel::SystemInfo info = MakeInfo(0, 32 * kGiB, 20 * kGiB, 200 * kGiB);
    const aimodel::ChatAdvice advice = aimodel::BuildChatAdvice(info, "3B 模型");
    ASSERT_TRUE(advice.ok);
    EXPECT_EQ(advice.recommendation.category, aimodel::ChatCategory::kCheck);
}

TEST(ChatSuggestTest, NoGpuAndInsufficientMemoryIsNotRecommended) {
    // 无独显且内存不足承载目标规模 → not_recommended。
    const aimodel::SystemInfo info = MakeInfo(0, 2 * kGiB, 1 * kGiB, 200 * kGiB);
    const aimodel::ChatAdvice advice = aimodel::BuildChatAdvice(info, "13B 模型");
    ASSERT_TRUE(advice.ok);
    EXPECT_EQ(advice.recommendation.category, aimodel::ChatCategory::kNotRecommended);
}

TEST(ChatSuggestTest, RecommendationCarriesStepsAndUncertaintyNote) {
    const aimodel::SystemInfo info = MakeInfo(8 * kGiB, 32 * kGiB, 16 * kGiB, 100 * kGiB);
    const aimodel::ChatAdvice advice = aimodel::BuildChatAdvice(info, "推荐一个合适的模型");
    ASSERT_TRUE(advice.ok);

    // 必须给出可执行下一步（PRD US-3）。
    EXPECT_FALSE(advice.recommendation.next_steps.empty());
    for (size_t i = 0; i < advice.recommendation.next_steps.size(); ++i) {
        EXPECT_FALSE(advice.recommendation.next_steps[i].title.empty());
        EXPECT_FALSE(advice.recommendation.next_steps[i].action.empty());
    }

    // 必须包含不确定性声明（PRD US-4 / 契约 §3.3）。
    EXPECT_NE(advice.recommendation.reason.find("未调用外部模型"),
              std::string::npos);
}

TEST(ChatSuggestTest, CategoryNamesMatchContract) {
    EXPECT_EQ(std::string(aimodel::ChatCategoryName(aimodel::ChatCategory::kRecommend)),
              std::string("recommend"));
    EXPECT_EQ(std::string(aimodel::ChatCategoryName(aimodel::ChatCategory::kCheck)),
              std::string("check"));
    EXPECT_EQ(std::string(aimodel::ChatCategoryName(aimodel::ChatCategory::kNotRecommended)),
              std::string("not_recommended"));
}

TEST(ChatSuggestTest, ResponseJsonHasContractFields) {
    const aimodel::SystemInfo info = MakeInfo(24 * kGiB, 64 * kGiB, 48 * kGiB, 400 * kGiB);
    const std::string query = "能跑 7B 吗";
    const aimodel::ChatAdvice advice = aimodel::BuildChatAdvice(info, query);
    const std::string json = aimodel::ChatAdviceToJson(info, query, advice);

    EXPECT_NE(json.find("\"ok\":true"), std::string::npos);
    EXPECT_NE(json.find("\"request\""), std::string::npos);
    EXPECT_NE(json.find("\"config_snapshot\""), std::string::npos);
    EXPECT_NE(json.find("\"recommendations\""), std::string::npos);
    EXPECT_NE(json.find("\"category\":\"recommend\""), std::string::npos);
    EXPECT_NE(json.find("\"next_steps\""), std::string::npos);
    // 隐私边界必须出现在 meta 中（契约 §2.2 / §5）。
    EXPECT_NE(json.find("\"privacy_scope\":\"local-only\""), std::string::npos);
}

TEST(ChatSuggestTest, ErrorResponseJsonCarriesCode) {
    const aimodel::SystemInfo info = MakeInfo(8 * kGiB, 32 * kGiB, 16 * kGiB, 100 * kGiB);
    const aimodel::ChatAdvice advice = aimodel::BuildChatAdvice(info, "");
    const std::string json = aimodel::ChatAdviceToJson(info, "", advice);

    EXPECT_NE(json.find("\"ok\":false"), std::string::npos);
    EXPECT_NE(json.find("\"code\":\"INVALID_QUERY\""), std::string::npos);
    // 错误响应不应携带成功字段。
    EXPECT_EQ(json.find("\"recommendations\""), std::string::npos);
}

TEST(ChatSuggestTest, CapabilityJsonDeclaresLocalOnly) {
    const std::string json = aimodel::ChatCapabilityJson();
    EXPECT_NE(json.find("\"ok\":true"), std::string::npos);
    EXPECT_NE(json.find("/api/chat/suggest"), std::string::npos);
    EXPECT_NE(json.find("\"privacy_scope\":\"local-only\""), std::string::npos);
}
