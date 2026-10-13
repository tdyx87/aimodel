// 跨平台系统信息采集：CPU / GPU / 内存 / 硬盘 / 打印机。
// 统一接口见本文件；平台差异实现由 CMake 按平台选择
// src/cpp/system_info_{win32,posix,apple}.cpp（agents.md：禁止运行时平台判断）。

// 导出宏统一定义于 aimodel/export.h（共享库启用 dllexport/dllimport，静态库为空）。
#ifndef AIMODEL_SYSTEM_INFO_H_
#define AIMODEL_SYSTEM_INFO_H_

#include "aimodel/export.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aimodel {

// 每个 CPU 封装一条（Windows 按封装枚举；Linux/macOS 聚合为一条）。
struct CpuInfo {
    std::string vendor;             // 厂商，如 GenuineIntel / AuthenticAMD / Apple
    std::string brand;              // 型号名
    uint32_t core_count = 0;        // 逻辑核心数（0 表示未知）
    uint32_t physical_core_count = 0;  // 物理核心数（0 表示未知）
    uint64_t frequency_mhz = 0;     // 主频 MHz（0 表示未知；动态调频平台可能为 0）
};

struct GpuInfo {
    std::string vendor;             // NVIDIA / AMD / Intel / Apple / PCI vendor ...
    std::string name;               // 型号名
    uint64_t video_memory_bytes = 0;  // 显存字节数（0 表示未知/统一内存不暴露）
};

struct MemoryInfo {
    uint64_t total_bytes = 0;       // 物理内存总量
    uint64_t available_bytes = 0;   // 可用内存（0 表示未知）
};

struct DiskInfo {
    std::string mount_point;        // Windows: 盘符（C:\）；POSIX: 挂载路径（/）
    std::string file_system;        // NTFS / ext4 / apfs ...
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
};

struct PrinterInfo {
    std::string name;
    bool is_default = false;        // 是否默认打印机
    std::string status;             // idle / printing / offline / unknown ...
};

struct SystemInfo {
    std::string os_name;            // Windows / Linux / macOS
    std::string os_version;
    std::string architecture;       // x86_64 / arm64 ...
    std::string hostname;

    std::vector<CpuInfo> cpus;
    std::vector<GpuInfo> gpus;
    MemoryInfo memory;
    std::vector<DiskInfo> disks;
    std::vector<PrinterInfo> printers;
};

/**
 * @brief 采集当前系统信息（CPU/GPU/内存/硬盘/打印机）。
 * 任何子项采集失败仅使对应字段为空/0，不抛异常。
 *
 * 结果带 3 秒进程内短 TTL 缓存：采集需访问注册表 / /proc / /sys，
 * 部分平台还要拉起子进程（Linux 的 lpstat、macOS 的 system_profiler），
 * 单次可达秒级；HTTP 层每个请求都会调用本函数，故加缓存避免重复开销。
 * @return 填充后的 SystemInfo 结构体。
 */
AIMODEL_API SystemInfo GetSystemInfo();

/**
 * @brief 序列化为 JSON（UTF-8），用于日志与 CLI 输出。
 * @param info 待序列化的系统信息。
 * @return UTF-8 JSON 字符串（多行缩进）。
 */
AIMODEL_API std::string ToJson(const SystemInfo& info);

namespace detail {

// 通用字符串工具（三平台实现共用）。
AIMODEL_API std::string Trim(const std::string& s);
AIMODEL_API std::string ToLower(const std::string& s);
AIMODEL_API std::vector<std::string> SplitLines(const std::string& s);
AIMODEL_API bool ParseUInt64(const std::string& s, uint64_t* out);  // 支持 0x 前缀十六进制
AIMODEL_API std::string JsonEscape(const std::string& s);
AIMODEL_API bool EqualsIgnoreCase(const std::string& a, const std::string& b);
AIMODEL_API std::string GpuVendorFromName(const std::string& name);
AIMODEL_API std::string PciVendorName(const std::string& hex_vendor);  // "10de" -> NVIDIA

// 平台实现入口：由 CMake 按平台选择源文件，禁止在业务逻辑中做平台判断。
AIMODEL_API void CollectSystemInfo(SystemInfo* info);

}  // namespace detail
}  // namespace aimodel

#endif  // AIMODEL_SYSTEM_INFO_H_
