// Windows 平台实现：注册表 + Win32 API 采集系统信息。
// 对外一律 UTF-8：宽字符经 detail::WideToUtf8 转换（agents.md 第 6.3 条）。

#include "aimodel/system_info.h"

#if defined(_WIN32) || defined(_WIN64)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winspool.h>
#include <stdio.h>
#include <wchar.h>

#include <string>
#include <vector>

namespace aimodel {
namespace detail {

// 内部工具（不进公共头：wchar_t 不得暴露给调用方，reviewer Y3）。
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) {
        return std::string();
    }
    int size = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                     NULL, 0, NULL, NULL);
    if (size <= 0) {
        return std::string();
    }
    std::string out(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                          &out[0], size, NULL, NULL);
    return out;
}

namespace {

std::string RegReadString(HKEY root, const wchar_t* subkey, const wchar_t* value_name) {
    HKEY key = NULL;
    if (::RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return std::string();
    }
    std::string result;
    DWORD type = 0;
    DWORD size = 0;
    if (::RegQueryValueExW(key, value_name, NULL, &type, NULL, &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && size > 0) {
        std::wstring buffer(size / sizeof(wchar_t), L'\0');
        DWORD read_size = size;
        if (::RegQueryValueExW(key, value_name, NULL, &type,
                               reinterpret_cast<LPBYTE>(&buffer[0]), &read_size) == ERROR_SUCCESS) {
            while (!buffer.empty() && buffer[buffer.size() - 1] == L'\0') {
                buffer.erase(buffer.size() - 1);
            }
            result = WideToUtf8(buffer);
        }
    }
    ::RegCloseKey(key);
    return result;
}

void CollectOs(SystemInfo* info) {
    // OS 名称与版本：注册表 CurrentVersion（避免 GetVersionEx 的清单伪造问题）
    std::string product = RegReadString(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName");
    std::string display = RegReadString(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion");
    std::string build = RegReadString(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber");
    info->os_name = product.empty() ? std::string("Windows") : product;
    info->os_version = display.empty() ? build : (display + " (" + build + ")");

    // 架构
    SYSTEM_INFO si;
    ::GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: info->architecture = "x86_64"; break;
        case PROCESSOR_ARCHITECTURE_INTEL: info->architecture = "x86"; break;
#if defined(PROCESSOR_ARCHITECTURE_ARM64)
        case PROCESSOR_ARCHITECTURE_ARM64: info->architecture = "arm64"; break;
#endif
        case PROCESSOR_ARCHITECTURE_ARM: info->architecture = "arm"; break;
        default: info->architecture = "unknown"; break;
    }

    // 主机名
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD length = MAX_COMPUTERNAME_LENGTH + 1;
    if (::GetComputerNameW(name, &length)) {
        info->hostname = WideToUtf8(std::wstring(name, length));
    }
}

void CollectCpus(SystemInfo* info) {
    CpuInfo cpu;
    // CPU 型号/厂商：注册表 CentralProcessor\0
    cpu.brand = RegReadString(HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
    cpu.vendor = RegReadString(HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"VendorIdentifier");

    SYSTEM_INFO si;
    ::GetSystemInfo(&si);
    cpu.core_count = si.dwNumberOfProcessors;

    // 物理核数：GetLogicalProcessorInformation 统计 RelationProcessorCore 条目，
    // 不能用逻辑数兜底（超线程下会高估，与头文件"0 表示未知"契约冲突）
    cpu.physical_core_count = 0;
    DWORD plen = 0;
    ::GetLogicalProcessorInformation(NULL, &plen);
    if (plen > 0) {
        std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> infos(
            plen / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) + 1);
        if (::GetLogicalProcessorInformation(&infos[0], &plen)) {
            size_t count = plen / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
            for (size_t i = 0; i < count; ++i) {
                if (infos[i].Relationship == RelationProcessorCore) {
                    ++cpu.physical_core_count;
                }
            }
        }
    }
    if (cpu.physical_core_count == 0) {
        cpu.physical_core_count = si.dwNumberOfProcessors;  // 兜底：未知时退化为逻辑数
    }

    // 主频（GHz -> MHz）
    HKEY key = NULL;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &key)
            == ERROR_SUCCESS) {
        DWORD mhz = 0;
        DWORD size = sizeof(mhz);
        DWORD type = 0;
        if (::RegQueryValueExW(key, L"~MHz", NULL, &type,
                reinterpret_cast<LPBYTE>(&mhz), &size) == ERROR_SUCCESS && type == REG_DWORD) {
            cpu.frequency_mhz = mhz;
        }
        ::RegCloseKey(key);
    }
    info->cpus.push_back(cpu);
}

void CollectMemory(SystemInfo* info) {
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    if (::GlobalMemoryStatusEx(&status)) {
        info->memory.total_bytes = status.ullTotalPhys;
        info->memory.available_bytes = status.ullAvailPhys;
    }
}

void CollectDisks(SystemInfo* info) {
    wchar_t drives[512];
    DWORD len = ::GetLogicalDriveStringsW(static_cast<DWORD>(sizeof(drives) / sizeof(drives[0])),
                                          drives);
    if (len == 0 || len >= sizeof(drives) / sizeof(drives[0])) {
        return;
    }
    for (wchar_t* p = drives; *p != L'\0'; p += wcslen(p) + 1) {
        UINT type = ::GetDriveTypeW(p);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) {
            continue;
        }
        DiskInfo disk;
        disk.mount_point = WideToUtf8(std::wstring(p));

        ULARGE_INTEGER free_bytes_available, total_bytes, total_free_bytes;
        if (::GetDiskFreeSpaceExW(p, &free_bytes_available, &total_bytes, &total_free_bytes)) {
            disk.total_bytes = total_bytes.QuadPart;
            disk.free_bytes = total_free_bytes.QuadPart;
        }
        wchar_t fs_name[MAX_PATH + 1] = {0};
        if (::GetVolumeInformationW(p, NULL, 0, NULL, NULL, NULL, fs_name, MAX_PATH)) {
            disk.file_system = WideToUtf8(std::wstring(fs_name));
        }
        info->disks.push_back(disk);
    }
}

void CollectGpus(SystemInfo* info) {
    // 通过注册表枚举显示适配器（Class\{4d36e968-e325-11ce-bfc1-08002be10318}\NNNN）
    const wchar_t* kClassKey =
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
        L"{4d36e968-e325-11ce-bfc1-08002be10318}";
    for (int index = 0; index < 16; ++index) {
        wchar_t subkey[256];
        // ISO C：宽字符格式串中宽字符串用 %ls（%s 在 MinGW/UCRT 期望窄串）
        swprintf(subkey, 256, L"%ls\\%04d", kClassKey, index);
        std::string desc = RegReadString(HKEY_LOCAL_MACHINE, subkey, L"DriverDesc");
        if (desc.empty()) {
            continue;
        }
        GpuInfo gpu;
        gpu.name = desc;
        gpu.vendor = GpuVendorFromName(desc);
        // 显存大小（多为 REG_DWORD，单位 MB）
        HKEY key = NULL;
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ, &key) == ERROR_SUCCESS) {
            const wchar_t* kMemValues[] = {L"HardwareInformation.MemorySize",
                                           L"Hardware Information.qwMemorySize"};
            for (size_t v = 0; v < sizeof(kMemValues) / sizeof(kMemValues[0]); ++v) {
                DWORD type = 0;
                DWORD size = 0;
                if (::RegQueryValueExW(key, kMemValues[v], NULL, &type, NULL, &size)
                        == ERROR_SUCCESS) {
                    if (type == REG_DWORD && size == sizeof(DWORD)) {
                        DWORD raw = 0;
                        DWORD read_size = size;
                        if (::RegQueryValueExW(key, kMemValues[v], NULL, &type,
                                reinterpret_cast<LPBYTE>(&raw), &read_size) == ERROR_SUCCESS) {
                            // 单位启发式：< 1MB 视为历史约定的 MB，否则已是字节
                            if (raw < 1024UL * 1024UL) {
                                gpu.video_memory_bytes =
                                    static_cast<uint64_t>(raw) * 1024ULL * 1024ULL;
                            } else {
                                gpu.video_memory_bytes = static_cast<uint64_t>(raw);
                            }
                        }
                    } else if (type == REG_QWORD && size == sizeof(unsigned long long)) {
                        unsigned long long bytes = 0;
                        DWORD read_size = size;
                        if (::RegQueryValueExW(key, kMemValues[v], NULL, &type,
                                reinterpret_cast<LPBYTE>(&bytes), &read_size) == ERROR_SUCCESS) {
                            gpu.video_memory_bytes = bytes;
                        }
                    }
                    break;
                }
            }
            ::RegCloseKey(key);
        }
        info->gpus.push_back(gpu);
    }
}

void CollectPrinters(SystemInfo* info) {
    // 两级调用：先取所需缓冲区大小，再取数据
    DWORD needed = 0;
    DWORD count = 0;
    ::EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL, 2, NULL, 0,
                    &needed, &count);
    if (needed == 0) {
        return;
    }
    std::vector<BYTE> buffer(needed);
    if (!::EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL, 2, &buffer[0],
                         needed, &needed, &count)) {
        return;
    }
    PRINTER_INFO_2W* printers = reinterpret_cast<PRINTER_INFO_2W*>(&buffer[0]);
    // 默认打印机
    wchar_t default_name[256] = {0};
    DWORD default_size = 256;
    BOOL has_default = ::GetDefaultPrinterW(default_name, &default_size);
    for (DWORD i = 0; i < count; ++i) {
        PrinterInfo printer;
        if (printers[i].pPrinterName != NULL) {
            printer.name = WideToUtf8(printers[i].pPrinterName);
        }
        if (has_default && printers[i].pPrinterName != NULL &&
                wcscmp(printers[i].pPrinterName, default_name) == 0) {
            printer.is_default = true;
        }
        if (printers[i].Status == 0) {
            printer.status = "idle";
        } else if ((printers[i].Status & PRINTER_STATUS_OFFLINE) != 0) {
            printer.status = "offline";
        } else if ((printers[i].Status & PRINTER_STATUS_PRINTING) != 0) {
            printer.status = "printing";
        } else if ((printers[i].Status & PRINTER_STATUS_ERROR) != 0) {
            printer.status = "error";
        } else if ((printers[i].Status & PRINTER_STATUS_PAUSED) != 0) {
            printer.status = "paused";
        } else {
            printer.status = "unknown";
        }
        info->printers.push_back(printer);
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

#endif  // _WIN32 || _WIN64

