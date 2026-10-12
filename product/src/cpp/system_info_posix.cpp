// Linux 平台实现：/proc、/sys 与 CUPS 采集系统信息（UTF-8 原生）。

#include "aimodel/system_info.h"

#if defined(__linux__)

#include <dirent.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace aimodel {
namespace detail {

namespace {

// 读取小文件（/proc、/sys 均为小文件）
bool ReadSmallFile(const char* path, std::string* out) {
    if (path == NULL || out == NULL) {
        return false;
    }
    FILE* fp = std::fopen(path, "r");
    if (fp == NULL) {
        return false;
    }
    out->clear();
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) {
        out->append(buf, n);
    }
    std::fclose(fp);
    return true;
}

void CollectOs(SystemInfo* info) {
    struct utsname ut;
    if (::uname(&ut) == 0) {
        info->architecture = ut.machine;
        info->hostname = ut.nodename;
    }
    // /etc/os-release 的 PRETTY_NAME（如 Ubuntu 22.04.3 LTS）
    std::string release;
    if (ReadSmallFile("/etc/os-release", &release)) {
        std::vector<std::string> lines = SplitLines(release);
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].size() >= 12 && lines[i].compare(0, 12, "PRETTY_NAME=") == 0) {
                std::string value = lines[i].substr(12);
                if (value.size() >= 2 && value[0] == '"' &&
                        value[value.size() - 1] == '"') {
                    value = value.substr(1, value.size() - 2);
                }
                info->os_version = value;
                break;
            }
        }
    }
    info->os_name = "Linux";
    if (info->os_version.empty()) {
        // 回退到内核版本
        std::string proc_version;
        if (ReadSmallFile("/proc/sys/kernel/osrelease", &proc_version)) {
            info->os_version = Trim(proc_version);
        }
    }
}

void CollectCpus(SystemInfo* info) {
    std::string content;
    if (!ReadSmallFile("/proc/cpuinfo", &content)) {
        return;
    }
    // /proc/cpuinfo 行格式：`key<tab>: value`，按首个 ':' 切分键值
    std::vector<std::string> lines = SplitLines(content);
    CpuInfo cpu;
    cpu.core_count = 0;
    std::string seen_keys;
    std::string current_physical_id;  // /proc/cpuinfo 顺序为 physical id → siblings → core id
    uint32_t physical = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        size_t pos = lines[i].find(':');
        if (pos == std::string::npos) {
            continue;
        }
        std::string key = Trim(lines[i].substr(0, pos));
        std::string value = Trim(lines[i].substr(pos + 1));
        if (key == "processor") {
            ++cpu.core_count;
        } else if (key == "model name") {
            if (cpu.brand.empty()) {
                cpu.brand = value;
            }
        } else if (key == "vendor_id" || key == "vendor") {
            if (cpu.vendor.empty()) {
                cpu.vendor = value;
            }
        } else if (key == "physical id") {
            current_physical_id = value;
        } else if (key == "core id") {
            // core id 与 physical id 不相邻（中间隔 siblings），按 (physical_id, core_id)
            // 全局组合去重才是物理核数；不能只看相邻行
            std::string key_pair = current_physical_id + ":" + value;
            if (seen_keys.find("\n" + key_pair + "\n") == std::string::npos) {
                seen_keys += "\n" + key_pair + "\n";
                ++physical;
            }
        } else if (key == "cpu MHz" && cpu.frequency_mhz == 0) {
            double mhz = std::atof(value.c_str());
            if (mhz > 0) {
                cpu.frequency_mhz = static_cast<uint64_t>(mhz + 0.5);
            }
        }
    }
    if (physical > 0) {
        cpu.physical_core_count = physical;
    } else {
        cpu.physical_core_count = cpu.core_count;
    }
    info->cpus.push_back(cpu);
}

void CollectMemory(SystemInfo* info) {
    std::string content;
    if (!ReadSmallFile("/proc/meminfo", &content)) {
        return;
    }
    std::vector<std::string> lines = SplitLines(content);
    for (size_t i = 0; i < lines.size(); ++i) {
        // 形如 "MemTotal:       16311376 kB"
        size_t pos = lines[i].find(':');
        if (pos == std::string::npos) {
            continue;
        }
        std::string key = lines[i].substr(0, pos);
        std::string value = Trim(lines[i].substr(pos + 1));
        uint64_t kb = 0;
        if (ParseUInt64(value, &kb)) {
            if (key == "MemTotal") {
                info->memory.total_bytes = kb * 1024ULL;
            } else if (key == "MemAvailable") {
                info->memory.available_bytes = kb * 1024ULL;
            }
        }
    }
}

void CollectDisks(SystemInfo* info) {
    FILE* fp = std::fopen("/proc/mounts", "r");
    if (fp == NULL) {
        return;
    }
    char line[1024];
    while (std::fgets(line, sizeof(line), fp) != NULL) {
        // 格式：device mountpoint fstype options dump pass
        std::string text(line);
        std::istringstream ss(text);
        std::string device, mount_point, fstype;
        if (!(ss >> device >> mount_point >> fstype)) {
            continue;
        }
        // 只统计真实存储（过滤伪文件系统）
        if (fstype == "proc" || fstype == "sysfs" || fstype == "devtmpfs" ||
                fstype == "devpts" || fstype == "tmpfs" || fstype == "cgroup" ||
                fstype == "cgroup2" || fstype == "overlay" || fstype == "squashfs" ||
                fstype == "securityfs" || fstype == "debugfs" || fstype == "tracefs" ||
                fstype == "pstore" || fstype == "bpf" || fstype == "mqueue" ||
                fstype == "hugetlbfs" || fstype == "configfs" || fstype == "fusectl" ||
                fstype == "binfmt_misc" || fstype == "autofs") {
            continue;
        }
        struct statvfs vfs;
        if (::statvfs(mount_point.c_str(), &vfs) != 0) {
            continue;
        }
        DiskInfo disk;
        disk.mount_point = mount_point;
        disk.file_system = fstype;
        disk.total_bytes = static_cast<uint64_t>(vfs.f_blocks) * vfs.f_frsize;
        disk.free_bytes = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
        info->disks.push_back(disk);
    }
    std::fclose(fp);
}

void CollectGpus(SystemInfo* info) {
    // 枚举 /sys/bus/pci/devices 中 class 0x03*（显示控制器）
    DIR* dir = ::opendir("/sys/bus/pci/devices");
    if (dir == NULL) {
        return;
    }
    struct dirent* entry = NULL;
    while ((entry = ::readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        std::string base = std::string("/sys/bus/pci/devices/") + entry->d_name;
        std::string class_text;
        if (!ReadSmallFile((base + "/class").c_str(), &class_text)) {
            continue;
        }
        uint64_t pci_class = 0;
        if (!ParseUInt64(Trim(class_text), &pci_class)) {
            continue;
        }
        // 0x03xxxx = 显示控制器（VGA/3D/其他）
        if ((pci_class & 0xFF0000ULL) != 0x030000ULL) {
            continue;
        }
        GpuInfo gpu;
        std::string vendor_text, device_text;
        if (ReadSmallFile((base + "/vendor").c_str(), &vendor_text)) {
            uint64_t vendor_id = 0;
            if (ParseUInt64(Trim(vendor_text), &vendor_id)) {
                char hex[8];
                std::snprintf(hex, sizeof(hex), "%04llx",
                              static_cast<unsigned long long>(vendor_id));
                gpu.vendor = PciVendorName(hex);
            }
        }
        if (ReadSmallFile((base + "/device").c_str(), &device_text)) {
            uint64_t device_id = 0;
            if (ParseUInt64(Trim(device_text), &device_id)) {
                char hex[8];
                std::snprintf(hex, sizeof(hex), "0x%04llx",
                              static_cast<unsigned long long>(device_id));
                gpu.name = gpu.vendor + " " + std::string(hex);
            }
        }
        if (gpu.name.empty()) {
            gpu.name = gpu.vendor.empty() ? std::string("display controller")
                                          : (gpu.vendor + " display controller");
        }
        info->gpus.push_back(gpu);
    }
    ::closedir(dir);
}

void CollectPrinters(SystemInfo* info) {
    // CUPS：lpstat -p 输出形如 `printer myprinter is idle. enabled since ...`
    FILE* pipe = ::popen("lpstat -p 2>/dev/null", "r");
    if (pipe == NULL) {
        return;
    }
    char line[1024];
    while (std::fgets(line, sizeof(line), pipe) != NULL) {
        std::string text(line);
        if (text.compare(0, 7, "printer ") != 0) {
            continue;
        }
        std::istringstream ss(text);
        std::string prefix, name, is_word, status_word;
        ss >> prefix >> name >> is_word >> status_word;
        if (name.empty()) {
            continue;
        }
        PrinterInfo printer;
        printer.name = name;
        printer.is_default = false;
        if (status_word == "idle") {
            printer.status = "idle";
        } else if (status_word == "printing") {
            printer.status = "printing";
        } else {
            printer.status = "unknown";
        }
        info->printers.push_back(printer);
    }
    ::pclose(pipe);

    // 默认打印机：lpstat -d 输出形如 `system default destination: myprinter`
    FILE* dpipe = ::popen("lpstat -d 2>/dev/null", "r");
    if (dpipe != NULL) {
        if (std::fgets(line, sizeof(line), dpipe) != NULL) {
            std::string text(line);
            size_t pos = text.find(':');
            if (pos != std::string::npos) {
                std::string default_name = Trim(text.substr(pos + 1));
                for (size_t i = 0; i < info->printers.size(); ++i) {
                    if (info->printers[i].name == default_name) {
                        info->printers[i].is_default = true;
                    }
                }
            }
        }
        ::pclose(dpipe);
    }
}

}  // namespace

void CollectSystemInfo(SystemInfo* info) {
    if (info == NULL) {
        return;
    }
    CollectOs(info);
    CollectCpus(info);
    CollectMemory(info);
    CollectDisks(info);
    CollectGpus(info);
    CollectPrinters(info);
}

}  // namespace detail
}  // namespace aimodel

#endif  // __linux__

