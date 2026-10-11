// 单元测试：验证 GetSystemInfo 的结构不变量与 JSON 序列化。
// 注：具体数值依赖运行机器，只断言结构与不变量，不断言具体硬件值。

#include "aimodel/system_info.h"

#include <gtest/gtest.h>

#include <string>

namespace {

aimodel::SystemInfo Collect() {
    return aimodel::GetSystemInfo();
}

}  // namespace

TEST(SystemInfoTest, OsFieldsArePresent) {
    aimodel::SystemInfo info = Collect();
    EXPECT_FALSE(info.os_name.empty());
    EXPECT_FALSE(info.os_version.empty());
    EXPECT_FALSE(info.architecture.empty());
    EXPECT_FALSE(info.hostname.empty());
}

TEST(SystemInfoTest, CpuPresentWithPlausibleCoreCount) {
    aimodel::SystemInfo info = Collect();
    ASSERT_FALSE(info.cpus.empty()) << "至少应枚举到一个 CPU 条目";
    const aimodel::CpuInfo& cpu = info.cpus[0];
    EXPECT_GE(cpu.core_count, 1u);
    EXPECT_GE(cpu.physical_core_count, 1u);
    // 品牌名在已知平台上均可获取（Linux/Windows/macOS 均有来源）
    EXPECT_FALSE(cpu.brand.empty());
}

TEST(SystemInfoTest, MemoryPlausible) {
    aimodel::SystemInfo info = Collect();
    // 至少 1 MB，至多 1 PB（防御解析错误）
    EXPECT_GT(info.memory.total_bytes, 1024ULL * 1024ULL);
    EXPECT_LT(info.memory.total_bytes, 1024ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL);
    // 可用量应不超过总量（vm_stat 解析下界，Windows/macOS/Linux 均应满足）
    if (info.memory.available_bytes > 0) {
        EXPECT_LE(info.memory.available_bytes, info.memory.total_bytes);
    }
}

TEST(SystemInfoTest, DisksPresentAndPlausible) {
    aimodel::SystemInfo info = Collect();
    ASSERT_FALSE(info.disks.empty()) << "至少应枚举到一个磁盘/卷";
    for (size_t i = 0; i < info.disks.size(); ++i) {
        const aimodel::DiskInfo& disk = info.disks[i];
        EXPECT_FALSE(disk.mount_point.empty());
        EXPECT_GT(disk.total_bytes, 0ULL);
        EXPECT_LE(disk.free_bytes, disk.total_bytes);
    }
}

TEST(SystemInfoTest, GpusWellFormed) {
    aimodel::SystemInfo info = Collect();
    // CI/虚拟机可能无独立 GPU，故允许为空；但有条目时字段必须自洽
    for (size_t i = 0; i < info.gpus.size(); ++i) {
        const aimodel::GpuInfo& gpu = info.gpus[i];
        EXPECT_FALSE(gpu.vendor.empty());
        EXPECT_FALSE(gpu.name.empty());
        // 显存为 0（未知）或 < 128GB（防单位换算错误，如字节误乘 MB）
        EXPECT_TRUE(gpu.video_memory_bytes == 0 ||
                    gpu.video_memory_bytes < 128ULL * 1024 * 1024 * 1024)
            << gpu.name << " vram=" << gpu.video_memory_bytes;
    }
}

TEST(SystemInfoTest, PrintersWellFormed) {
    aimodel::SystemInfo info = Collect();
    // 无打印机环境允许为空；有条目时名称非空、状态在已知集合内
    for (size_t i = 0; i < info.printers.size(); ++i) {
        const aimodel::PrinterInfo& printer = info.printers[i];
        EXPECT_FALSE(printer.name.empty());
        EXPECT_TRUE(printer.status == "idle" || printer.status == "printing" ||
                    printer.status == "offline" || printer.status == "error" ||
                    printer.status == "paused" || printer.status == "unknown")
            << "unexpected status: " << printer.status;
    }
}

TEST(SystemInfoTest, JsonSerializesAndRoundTripsStructurally) {
    aimodel::SystemInfo info = Collect();
    std::string json = aimodel::ToJson(info);
    ASSERT_FALSE(json.empty());
    // JSON 结构关键键必须存在
    EXPECT_NE(json.find("\"os_name\""), std::string::npos);
    EXPECT_NE(json.find("\"cpus\""), std::string::npos);
    EXPECT_NE(json.find("\"gpus\""), std::string::npos);
    EXPECT_NE(json.find("\"memory\""), std::string::npos);
    EXPECT_NE(json.find("\"disks\""), std::string::npos);
    EXPECT_NE(json.find("\"printers\""), std::string::npos);
    // 括号配对
    EXPECT_EQ(json[0], '{');
    EXPECT_EQ(json[json.size() - 2], '}');  // 结尾 "}\n"
}

TEST(SystemInfoTest, JsonEscapesControlCharacters) {
    aimodel::SystemInfo info;
    info.os_name = "line1\nline2\t\"quoted\"";
    std::string json = aimodel::ToJson(info);
    EXPECT_NE(json.find("line1\\nline2"), std::string::npos);
    EXPECT_NE(json.find("\\\"quoted\\\""), std::string::npos);
}

TEST(SystemInfoTest, TrimAndParseHelpers) {
    EXPECT_EQ(aimodel::detail::Trim("  abc  "), std::string("abc"));
    EXPECT_EQ(aimodel::detail::Trim("\t\nabc\r\n"), std::string("abc"));
    uint64_t value = 0;
    EXPECT_TRUE(aimodel::detail::ParseUInt64("1234", &value));
    EXPECT_EQ(value, 1234ULL);
    EXPECT_TRUE(aimodel::detail::ParseUInt64("0x10de", &value));
    EXPECT_EQ(value, 0x10deULL);
    EXPECT_FALSE(aimodel::detail::ParseUInt64("", &value));
    EXPECT_FALSE(aimodel::detail::ParseUInt64("abc", &value));
}

TEST(SystemInfoTest, VendorMapping) {
    EXPECT_EQ(aimodel::detail::PciVendorName("10de"), std::string("NVIDIA"));
    EXPECT_EQ(aimodel::detail::PciVendorName("8086"), std::string("Intel"));
    EXPECT_EQ(aimodel::detail::GpuVendorFromName("NVIDIA GeForce RTX 4090"),
              std::string("NVIDIA"));
    EXPECT_EQ(aimodel::detail::GpuVendorFromName("AMD Radeon RX 7900"),
              std::string("AMD"));
}
