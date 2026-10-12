// macOS 平台实现：sysctl、getfsstat 与 system_profiler 采集系统信息。

#include "aimodel/system_info.h"

#if defined(__APPLE__) && defined(__MACH__)

#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace aimodel {
namespace detail {

namespace {

// sysctl 字符串
std::string SysctlString(const char* name) {
    size_t size = 0;
    if (::sysctlbyname(name, NULL, &size, NULL, 0) != 0 || size == 0) {
        return std::string();
    }
    std::vector<char> buf(size + 1, '\0');
    if (::sysctlbyname(name, &buf[0], &size, NULL, 0) != 0) {
        return std::string();
    }
    return std::string(&buf[0]);
}

// sysctl 整数
uint64_t SysctlUint64(const char* name) {
    uint64_t value = 0;
    size_t size = sizeof(value);
    if (::sysctlbyname(name, &value, &size, NULL, 0) != 0) {
        return 0;
    }
    return value;
}

void CollectOs(SystemInfo* info) {
    struct utsname ut;
    if (::uname(&ut) == 0) {
        info->architecture = ut.machine;
        info->hostname = ut.nodename;
    }
    // 版本：kern.osproductversion（如 14.2.1），回退 kern.osrelease
    info->os_version = SysctlString("kern.osproductversion");
    if (info->os_version.empty()) {
        info->os_version = SysctlString("kern.osrelease");
    }
    info->os_name = "macOS";
}

void CollectCpus(SystemInfo* info) {
    CpuInfo cpu;
    cpu.brand = SysctlString("machdep.cpu.brand_string");
    cpu.vendor = SysctlString("machdep.cpu.vendor");
    cpu.physical_core_count = static_cast<uint32_t>(SysctlUint64("hw.physicalcpu"));
    cpu.core_count = static_cast<uint32_t>(SysctlUint64("hw.logicalcpu"));
    // 主频：hw.cpufrequency 为 Hz；Apple Silicon 动态调频可能无此节点
    uint64_t hz = SysctlUint64("hw.cpufrequency");
    cpu.frequency_mhz = hz / 1000000ULL;
    info->cpus.push_back(cpu);
}

void CollectMemory(SystemInfo* info) {
    info->memory.total_bytes = SysctlUint64("hw.memsize");
    // 可用量：vm_stat 免解析 mach API；解析 "Pages free/inactive/speculative"
    FILE* pipe = ::popen("vm_stat 2>/dev/null", "r");
    if (pipe == NULL) {
        return;
    }
    uint64_t page_size = 4096;
    char line[512];
    uint64_t free_pages = 0;
    uint64_t inactive_pages = 0;
    uint64_t speculative_pages = 0;
    bool first = true;
    while (std::fgets(line, sizeof(line), pipe) != NULL) {
        std::string text(line);
        if (first) {
            // 首行含 page size：`Mach Virtual Memory Statistics: (page size of 16384 bytes)`
            size_t pos = text.find("page size of ");
            if (pos != std::string::npos) {
                page_size = static_cast<uint64_t>(
                    std::strtoull(text.c_str() + pos + 13, NULL, 10));
            }
            first = false;
            continue;
        }
        size_t colon = text.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string key = Trim(text.substr(0, colon));
        uint64_t pages = 0;
        ParseUInt64(Trim(text.substr(colon + 1)), &pages);
        if (key == "Pages free") {
            free_pages = pages;
        } else if (key == "Pages inactive") {
            inactive_pages = pages;
        } else if (key == "Pages speculative") {
            speculative_pages = pages;
        }
    }
    ::pclose(pipe);
    info->memory.available_bytes =
        (free_pages + inactive_pages + speculative_pages) * page_size;
}

void CollectDisks(SystemInfo* info) {
    // getfsstat 取已挂载卷；跳过 devfs 等伪卷
    int count = ::getfsstat(NULL, 0, MNT_NOWAIT);
    if (count <= 0) {
        return;
    }
    std::vector<struct statfs> mounts(static_cast<size_t>(count));
    int got = ::getfsstat(&mounts[0],
                          static_cast<int>(mounts.size() * sizeof(struct statfs)),
                          MNT_NOWAIT);
    if (got <= 0) {
        return;
    }
    for (int i = 0; i < got; ++i) {
        const struct statfs& fs = mounts[static_cast<size_t>(i)];
        std::string fstype = fs.f_fstypename;
        if (fstype == "devfs" || fstype == "autofs" || fstype == "map" ||
                fstype == "fdesc" || fstype == "cifs") {
            continue;
        }
        DiskInfo disk;
        disk.mount_point = fs.f_mntonname;
        disk.file_system = fstype;
        disk.total_bytes = static_cast<uint64_t>(fs.f_blocks) * fs.f_bsize;
        disk.free_bytes = static_cast<uint64_t>(fs.f_bavail) * fs.f_bsize;
        info->disks.push_back(disk);
    }
}


void CollectGpus(SystemInfo* info) {
    // GPU：system_profiler（LC_ALL=C 强制英文输出，避免非英文系统解析失败）
    FILE* pipe = ::popen("LC_ALL=C system_profiler SPDisplaysDataType 2>/dev/null", "r");
    if (pipe == NULL) {
        return;
    }
    char line[1024];
    GpuInfo current;
    bool in_gpu = false;
    while (std::fgets(line, sizeof(line), pipe) != NULL) {
        std::string text(line);
        // `    Chipset Model: Apple M2` 或 `Chipset Model: AMD Radeon ...`
        size_t pos = text.find("Chipset Model:");
        if (pos != std::string::npos) {
            if (in_gpu) {
                info->gpus.push_back(current);
            }
            current = GpuInfo();
            current.name = Trim(text.substr(pos + 14));
            current.vendor = GpuVendorFromName(current.name);
            in_gpu = true;
            continue;
        }
        if (!in_gpu) {
            continue;
        }
        // 显存：`      VRAM (Total): 1536 MB` 或 `      VRAM (Dynamic, Max): 8 MB`
        pos = text.find("VRAM (");
        if (pos != std::string::npos) {
            size_t colon = text.find(':', pos);
            if (colon != std::string::npos) {
                std::string value = Trim(text.substr(colon + 1));
                double amount = std::strtod(value.c_str(), NULL);
                uint64_t bytes = 0;
                if (value.find("GB") != std::string::npos) {
                    bytes = static_cast<uint64_t>(amount * 1024.0 * 1024.0 * 1024.0);
                } else if (value.find("MB") != std::string::npos) {
                    bytes = static_cast<uint64_t>(amount * 1024.0 * 1024.0);
                }
                if (bytes > 0) {
                    current.video_memory_bytes = bytes;
                }
            }
        }
        // 厂商行：`      Vendor: AMD (0x1002)`
        pos = text.find("Vendor:");
        if (pos != std::string::npos && current.vendor == "Unknown") {
            current.vendor = Trim(text.substr(pos + 7));
            size_t paren = current.vendor.find('(');
            if (paren != std::string::npos) {
                current.vendor = Trim(current.vendor.substr(0, paren));
            }
        }
    }
    if (in_gpu) {
        info->gpus.push_back(current);
    }
    ::pclose(pipe);
}

void CollectPrinters(SystemInfo* info) {
    // CUPS：macOS 自带 lpstat
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

    // 默认打印机
    FILE* dpipe = ::popen("lpstat -d 2>/dev/null", "r");
    if (dpipe != NULL) {
        if (std::fgets(line, sizeof(line), dpipe) != NULL) {
            std::string text(line);
            size_t colon = text.find(':');
            if (colon != std::string::npos) {
                std::string default_name = Trim(text.substr(colon + 1));
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

#endif  // __APPLE__ && __MACH__
