// 通用实现：平台无关的字符串工具、JSON 序列化与统一入口。
// 平台差异逻辑一律放在 system_info_{win32,posix,apple}.cpp 中，
// 本文件必须能在三平台无差别编译。

#include "aimodel/system_info.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace aimodel {
namespace detail {

std::string Trim(const std::string& s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(begin, end - begin);
}

std::string ToLower(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
    }
    return out;
}

std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> lines;
    std::string current;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            if (!current.empty() && current[current.size() - 1] == '\r') {
                current.erase(current.size() - 1);
            }
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(s[i]);
        }
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

bool ParseUInt64(const std::string& s, uint64_t* out) {
    if (out == NULL || s.empty()) {
        return false;
    }
    int base = 10;
    size_t start = 0;
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        start = 2;
    }
    char* end = NULL;
    unsigned long long value = std::strtoull(s.c_str() + start, &end, base);
    if (end == s.c_str() + start) {
        return false;
    }
    *out = static_cast<uint64_t>(value);
    return true;
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
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

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string GpuVendorFromName(const std::string& name) {
    const std::string lower = ToLower(name);
    if (lower.find("nvidia") != std::string::npos ||
        lower.find("geforce") != std::string::npos ||
        lower.find("quadro") != std::string::npos) {
        return "NVIDIA";
    }
    if (lower.find("radeon") != std::string::npos || lower.find("amd") != std::string::npos ||
        lower.find("ati ") != std::string::npos) {
        return "AMD";
    }
    if (lower.find("intel") != std::string::npos) {
        return "Intel";
    }
    if (lower.find("apple") != std::string::npos) {
        return "Apple";
    }
    if (lower.find("microsoft") != std::string::npos ||
        lower.find("basic render") != std::string::npos) {
        return "Microsoft";
    }
    return "Unknown";
}

std::string PciVendorName(const std::string& hex_vendor) {
    const std::string lower = ToLower(hex_vendor);
    if (lower == "10de") return "NVIDIA";
    if (lower == "1002" || lower == "1022") return "AMD";
    if (lower == "8086") return "Intel";
    if (lower == "106b") return "Apple";
    if (lower == "1237" || lower == "15ad") return "VMware";
    if (lower == "80ee") return "Oracle VirtualBox";
    if (lower == "1af4") return "Red Hat (virtio)";
    if (lower == "1a03") return "ASPEED";
    if (lower == "102b") return "Matrox";
    if (lower == "14e4") return "Broadcom";
    if (lower == "19e5") return "Huawei";
    if (lower == "1d17") return "Zhaoxin";
    if (lower == "740f") return "Loongson";
    return "PCI vendor " + lower;
}

}  // namespace detail

std::string ToJson(const SystemInfo& info) {
    std::ostringstream os;
    os << "{\n";
    os << "  \"os_name\": \"" << detail::JsonEscape(info.os_name) << "\",\n";
    os << "  \"os_version\": \"" << detail::JsonEscape(info.os_version) << "\",\n";
    os << "  \"architecture\": \"" << detail::JsonEscape(info.architecture) << "\",\n";
    os << "  \"hostname\": \"" << detail::JsonEscape(info.hostname) << "\",\n";

    os << "  \"cpus\": [";
    for (size_t i = 0; i < info.cpus.size(); ++i) {
        const CpuInfo& cpu = info.cpus[i];
        os << (i == 0 ? "\n" : ",\n");
        os << "    {\n";
        os << "      \"vendor\": \"" << detail::JsonEscape(cpu.vendor) << "\",\n";
        os << "      \"brand\": \"" << detail::JsonEscape(cpu.brand) << "\",\n";
        os << "      \"core_count\": " << cpu.core_count << ",\n";
        os << "      \"physical_core_count\": " << cpu.physical_core_count << ",\n";
        os << "      \"frequency_mhz\": " << cpu.frequency_mhz << "\n";
        os << "    }";
    }
    os << (info.cpus.empty() ? "" : "\n  ") << "],\n";

    os << "  \"gpus\": [";
    for (size_t i = 0; i < info.gpus.size(); ++i) {
        const GpuInfo& gpu = info.gpus[i];
        os << (i == 0 ? "\n" : ",\n");
        os << "    {\n";
        os << "      \"vendor\": \"" << detail::JsonEscape(gpu.vendor) << "\",\n";
        os << "      \"name\": \"" << detail::JsonEscape(gpu.name) << "\",\n";
        os << "      \"video_memory_bytes\": " << gpu.video_memory_bytes << "\n";
        os << "    }";
    }
    os << (info.gpus.empty() ? "" : "\n  ") << "],\n";

    os << "  \"memory\": {\n";
    os << "    \"total_bytes\": " << info.memory.total_bytes << ",\n";
    os << "    \"available_bytes\": " << info.memory.available_bytes << "\n";
    os << "  },\n";

    os << "  \"disks\": [";
    for (size_t i = 0; i < info.disks.size(); ++i) {
        const DiskInfo& disk = info.disks[i];
        os << (i == 0 ? "\n" : ",\n");
        os << "    {\n";
        os << "      \"mount_point\": \"" << detail::JsonEscape(disk.mount_point) << "\",\n";
        os << "      \"file_system\": \"" << detail::JsonEscape(disk.file_system) << "\",\n";
        os << "      \"total_bytes\": " << disk.total_bytes << ",\n";
        os << "      \"free_bytes\": " << disk.free_bytes << "\n";
        os << "    }";
    }
    os << (info.disks.empty() ? "" : "\n  ") << "],\n";

    os << "  \"printers\": [";
    for (size_t i = 0; i < info.printers.size(); ++i) {
        const PrinterInfo& printer = info.printers[i];
        os << (i == 0 ? "\n" : ",\n");
        os << "    {\n";
        os << "      \"name\": \"" << detail::JsonEscape(printer.name) << "\",\n";
        os << "      \"is_default\": " << (printer.is_default ? "true" : "false") << ",\n";
        os << "      \"status\": \"" << detail::JsonEscape(printer.status) << "\"\n";
        os << "    }";
    }
    os << (info.printers.empty() ? "" : "\n  ") << "]\n";
    os << "}\n";
    return os.str();
}

SystemInfo GetSystemInfo() {
    SystemInfo info;
    detail::CollectSystemInfo(&info);
    return info;
}

}  // namespace aimodel
