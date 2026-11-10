// Linux host backend, area "system".
//
// Power requests are logind inhibitor locks (org.freedesktop.login1.Manager.Inhibit over the
// system bus, through libsystemd's sd-bus loaded at run time): the lock is a descriptor, held
// while the request is. Power-source changes come from the kernel's uevents for the
// power_supply subsystem. Sleep watching (logind's PrepareForSleep) is not written yet:
// Error::unsupported.
#include "alchemy/host/linux/linux.hpp"
#include "alchemy/host/system.hpp"

#include <dlfcn.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <optional>
#include <thread>

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

namespace {

// libsystemd's sd-bus, the calls Inhibit needs (sd-bus.h; loaded at run time so the build and a
// host without systemd need no libsystemd).
struct SdBus {
    int (*open_system)(void** bus) = nullptr;
    void* (*unref)(void* bus) = nullptr;
    int (*call_method)(void* bus, const char* destination, const char* path, const char* interface, const char* member,
                       void* error, void** reply, const char* types, ...) = nullptr;
    int (*message_read)(void* message, const char* types, ...) = nullptr;
    void* (*message_unref)(void* message) = nullptr;
    void (*error_free)(void* error) = nullptr;
};
const SdBus* sd_bus() noexcept {
    static const auto table = []() -> std::optional<SdBus> {
        void* library = dlopen("libsystemd.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!library) return std::nullopt;
        SdBus value;
        value.open_system = reinterpret_cast<int (*)(void**)>(dlsym(library, "sd_bus_open_system"));
        value.unref = reinterpret_cast<void* (*)(void*)>(dlsym(library, "sd_bus_unref"));
        value.call_method = reinterpret_cast<decltype(value.call_method)>(dlsym(library, "sd_bus_call_method"));
        value.message_read = reinterpret_cast<decltype(value.message_read)>(dlsym(library, "sd_bus_message_read"));
        value.message_unref = reinterpret_cast<void* (*)(void*)>(dlsym(library, "sd_bus_message_unref"));
        value.error_free = reinterpret_cast<void (*)(void*)>(dlsym(library, "sd_bus_error_free"));
        if (!value.open_system || !value.unref || !value.call_method || !value.message_read || !value.message_unref || !value.error_free)
            return std::nullopt;
        return value;   // the library stays loaded
    }();
    return table ? &*table : nullptr;
}

// sd_bus_error (sd-bus.h): name, message, a private flag.
struct BusError {
    const char* name = nullptr;
    const char* message = nullptr;
    int need_free = 0;
};

} // namespace

Result<PowerRequest> PowerRequest::create(std::string_view reason, bool system, bool display) {
    if (!system && !display) return PowerRequest{};
    const auto* bus_api = sd_bus();
    if (!bus_api) return Error::unsupported;   // no systemd: nothing here can hold the machine awake
    void* bus = nullptr;
    if (bus_api->open_system(&bus) < 0 || !bus) return Error::unsupported;
    // "idle" blocks idle actions (automatic suspend, and screen blanking where the desktop honours
    // logind); explicit sleep stays possible, as with macOS's user-idle assertions.
    const std::string why(reason.empty() ? std::string_view("Alchemy") : reason);
    BusError error;
    void* reply = nullptr;
    const auto called = bus_api->call_method(bus, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
                                             "Inhibit", &error, &reply, "ssss", "idle", "Alchemy", why.c_str(), "block");
    int descriptor = -1;
    if (called >= 0 && reply) {
        int borrowed = -1;   // owned by the reply: duplicated before the reply goes
        if (bus_api->message_read(reply, "h", &borrowed) >= 0 && borrowed >= 0) descriptor = fcntl(borrowed, F_DUPFD_CLOEXEC, 3);
    }
    if (reply) bus_api->message_unref(reply);
    bus_api->error_free(&error);
    bus_api->unref(bus);
    if (descriptor < 0) return called == -EACCES || called == -EPERM ? Error::access_denied : Error::unsupported;
    PowerRequest request;
    request.handle_ = linux_backend::handle_of(descriptor);
    request.system_ = system;
    request.display_ = display;
    return request;
}
PowerRequest& PowerRequest::operator=(PowerRequest&& other) noexcept {
    if (this != &other) {
        release();
        handle_ = std::exchange(other.handle_, {});
        system_ = other.system_;
        display_ = other.display_;
    }
    return *this;
}
void PowerRequest::release() noexcept {
    // Closing the inhibitor descriptor ends the lock.
    if (handle_) ::close(linux_backend::fd_of(handle_));
    handle_ = {};
}

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
Status watch_power_source(std::function<void()> changed) {
    // One registration per process, as on the other hosts.
    static std::atomic<bool> watching{false};
    if (!changed) return Error::invalid_argument;
    if (watching.exchange(true)) return Error::already;
    // The kernel's uevents (NETLINK_KOBJECT_UEVENT, group 1): each power_supply change (AC plugged,
    // charge level, charging state) arrives as one; listening needs no privilege.
    const int socket_fd = ::socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (socket_fd < 0) { watching = false; return linux_backend::last_error(); }
    sockaddr_nl address{};
    address.nl_family = AF_NETLINK;
    address.nl_groups = 1;
    if (::bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        const auto failure = linux_backend::last_error();
        ::close(socket_fd);
        watching = false;
        return failure;
    }
    // The registration lasts for the process: the thread and its socket are never released.
    std::thread([socket_fd, changed = std::move(changed)] {
        std::array<char, 8192> buffer{};
        for (;;) {
            const auto got = ::recv(socket_fd, buffer.data(), buffer.size() - 1, 0);
            if (got < 0) { if (errno == EINTR || errno == ENOBUFS) continue; return; }
            buffer[static_cast<std::size_t>(got)] = '\0';
            // NUL-separated "KEY=value" fields after the "action@devpath" header.
            bool power_supply = false;
            for (std::size_t at = 0; at < static_cast<std::size_t>(got); at += std::strlen(buffer.data() + at) + 1)
                if (std::strcmp(buffer.data() + at, "SUBSYSTEM=power_supply") == 0) power_supply = true;
            if (power_supply) changed();
        }
    }).detach();
    return {};
}

} // namespace alchemy::host::system
