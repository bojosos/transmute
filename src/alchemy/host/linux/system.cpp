// Linux host backend, area "system".
//
// Power requests, sleep and power-source watching go through logind and
// UPower over D-Bus, which this backend does not speak yet: they report
// Error::unsupported (and a power status without a battery).
#include "alchemy/host/linux/linux.hpp"
#include "alchemy/host/system.hpp"

#include <pwd.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace alchemy::host::system {

namespace {

// The "cpu" lines of /proc/stat, in clock ticks.
std::vector<CpuTimes> read_cpu_lines(bool total) {
    std::vector<CpuTimes> result;
    FILE* stat = std::fopen("/proc/stat", "re");
    if (!stat) return result;
    const auto tick_ns = 1000000000ull / static_cast<unsigned long long>(sysconf(_SC_CLK_TCK));
    char line[512];
    while (std::fgets(line, sizeof(line), stat)) {
        if (std::strncmp(line, "cpu", 3) != 0) break;
        const bool is_total = line[3] == ' ';
        if (is_total != total) continue;
        unsigned long long user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0;
        std::sscanf(line + (is_total ? 3 : 4), "%*[0-9] %llu %llu %llu %llu %llu %llu %llu", &user, &nice, &system,
                    &idle, &iowait, &irq, &softirq);
        if (is_total)
            std::sscanf(line + 3, " %llu %llu %llu %llu %llu %llu %llu", &user, &nice, &system, &idle, &iowait, &irq, &softirq);
        result.push_back({(user + nice) * tick_ns, (system + irq + softirq) * tick_ns, (idle + iowait) * tick_ns});
    }
    std::fclose(stat);
    return result;
}

} // namespace

Result<Memory> memory() {
    struct sysinfo info {};
    if (sysinfo(&info) != 0) return linux_backend::last_error();
    Memory m;
    m.physical = static_cast<std::uint64_t>(info.totalram) * info.mem_unit;
    m.available = static_cast<std::uint64_t>(info.freeram + info.bufferram) * info.mem_unit;
    // MemAvailable counts reclaimable cache too, as Windows' available memory does.
    std::ifstream meminfo("/proc/meminfo");
    std::string key;
    std::uint64_t kilobytes = 0;
    while (meminfo >> key >> kilobytes) {
        if (key == "MemAvailable:") {
            m.available = kilobytes * 1024;
            break;
        }
        meminfo.ignore(64, '\n');
    }
    return m;
}

Result<CpuTimes> cpu_times() {
    const auto lines = read_cpu_lines(true);
    if (lines.empty()) return Error::unsupported;
    return lines.front();
}

Result<std::vector<CpuTimes>> processor_times() {
    auto lines = read_cpu_lines(false);
    if (lines.empty()) return Error::unsupported;
    return lines;
}

Result<std::array<std::uint8_t, 16>> machine_uuid() {
    std::ifstream file("/etc/machine-id");
    std::string hex;
    if (!(file >> hex) || hex.size() < 32) return Error::not_found;
    std::array<std::uint8_t, 16> uuid{};
    for (std::size_t i = 0; i < 16; ++i) uuid[i] = static_cast<std::uint8_t>(std::stoul(hex.substr(2 * i, 2), nullptr, 16));
    return uuid;
}

Result<Pressure> memory_pressure() {
    // PSI: the share of the last 10 s some task stalled on memory.
    std::ifstream file("/proc/pressure/memory");
    std::string kind, average;
    if (!(file >> kind >> average) || average.rfind("avg10=", 0) != 0) {
        const auto m = memory();
        if (!m) return m.error();
        const double free_share = static_cast<double>(m->available) / static_cast<double>(m->physical);
        return free_share < 0.05 ? Pressure::critical : free_share < 0.15 ? Pressure::warning : Pressure::normal;
    }
    const double stalled = std::strtod(average.c_str() + 6, nullptr);
    return stalled >= 10.0 ? Pressure::critical : stalled >= 1.0 ? Pressure::warning : Pressure::normal;
}

Result<std::array<double, 3>> load_average() {
    std::array<double, 3> loads{};
    if (getloadavg(loads.data(), 3) != 3) return Error::unsupported;
    return loads;
}

Result<std::string> host_name(bool) {
    char name[256]{};
    if (gethostname(name, sizeof(name) - 1) != 0) return linux_backend::last_error();
    return std::string(name);
}

Result<std::string> user_name() {
    passwd entry{}, *found = nullptr;
    char buffer[4096];
    if (getpwuid_r(getuid(), &entry, buffer, sizeof(buffer), &found) == 0 && found) return std::string(found->pw_name);
    if (const char* user = std::getenv("USER")) return std::string(user);
    return Error::not_found;
}

Result<std::string> fonts_directory() { return std::string("/usr/share/fonts"); }

Result<PowerRequest> PowerRequest::create(std::string_view, bool, bool) { return Error::unsupported; }
PowerRequest& PowerRequest::operator=(PowerRequest&& other) noexcept {
    if (this != &other) {
        release();
        handle_ = std::exchange(other.handle_, {});
        system_ = other.system_;
        display_ = other.display_;
    }
    return *this;
}
void PowerRequest::release() noexcept { handle_ = {}; }

Result<PowerStatus> power_status() {
    PowerStatus status;
    status.external_power = true;
    // The first battery the kernel lists.
    for (const char* name : {"BAT0", "BAT1", "BAT2"}) {
        const std::string base = std::string("/sys/class/power_supply/") + name + "/";
        std::ifstream capacity(base + "capacity"), state(base + "status");
        int percent = 0;
        std::string text;
        if (!(capacity >> percent)) continue;
        status.battery = true;
        status.percent = static_cast<std::uint8_t>(std::min(percent, 100));
        state >> text;
        status.charging = text == "Charging";
        status.external_power = text != "Discharging";
        break;
    }
    return status;
}

Status watch_sleep(std::function<void(bool)>) { return Error::unsupported; }
Status watch_power_source(std::function<void()>) { return Error::unsupported; }

} // namespace alchemy::host::system
