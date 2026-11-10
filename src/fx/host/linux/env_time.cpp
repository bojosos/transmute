// linux backend of fx/host/env.hpp and the out-of-line part of fx/host/time.hpp.
#include "fx/host/env.hpp"
#include "fx/host/linux/internal.hpp"
#include "fx/host/time.hpp"

#include <sys/resource.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>

namespace transmute::fx::host {

namespace {

// 100 ns units from 1601-01-01 to 1970-01-01.
constexpr std::uint64_t kUnixEpoch = 116444736000000000ull;

std::uint64_t filetime(const timespec& t) {
    return kUnixEpoch + static_cast<std::uint64_t>(t.tv_sec) * 10000000u + static_cast<std::uint64_t>(t.tv_nsec) / 100u;
}

std::uint64_t hundreds(const timeval& t) {
    return static_cast<std::uint64_t>(t.tv_sec) * 10000000u + static_cast<std::uint64_t>(t.tv_usec) * 10u;
}

} // namespace

std::size_t get_env(const char* name, char* value, std::size_t size) noexcept {
    const char* found = std::getenv(name);
    if (!found) return 0;
    const std::size_t length = std::strlen(found);
    if (!value || length + 1 > size) return length + 1;
    std::memcpy(value, found, length + 1);
    return length;
}

std::size_t get_env(const wchar_t* name, wchar_t* value, std::size_t size) noexcept {
    try {
        const auto narrow_name = linux_internal::narrow(name);
        const char* found = std::getenv(narrow_name.c_str());
        if (!found) return 0;
        const auto wide = linux_internal::widen(found);
        if (!value || wide.size() + 1 > size) return wide.size() + 1;
        std::wmemcpy(value, wide.c_str(), wide.size() + 1);
        return wide.size();
    } catch (...) {
        return 0;
    }
}

std::uint64_t wall_time() noexcept {
    timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return filetime(now);
}

bool process_times(ProcessTimes& times) noexcept {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return false;
    times.kernel = hundreds(usage.ru_stime);
    times.user = hundreds(usage.ru_utime);
    // Creation: field 22 of /proc/self/stat, in clock ticks since boot.
    times.created = 0;
    if (FILE* stat = std::fopen("/proc/self/stat", "r")) {
        char buffer[1024];
        const auto n = std::fread(buffer, 1, sizeof(buffer) - 1, stat);
        std::fclose(stat);
        buffer[n] = 0;
        const char* p = std::strrchr(buffer, ')');   // the command may hold spaces
        unsigned long long start = 0;
        for (int field = 3; p && field <= 22; ++field) {
            p = std::strchr(p + 1, ' ');
            if (p && field == 22) start = std::strtoull(p + 1, nullptr, 10);
        }
        timespec boot_now, real_now;
        clock_gettime(CLOCK_BOOTTIME, &boot_now);
        clock_gettime(CLOCK_REALTIME, &real_now);
        const long ticks_per_second = sysconf(_SC_CLK_TCK);
        if (start && ticks_per_second > 0) {
            const auto since_boot = static_cast<std::uint64_t>(boot_now.tv_sec) * 10000000u +
                                    static_cast<std::uint64_t>(boot_now.tv_nsec) / 100u;
            const auto started = start * 10000000ull / static_cast<unsigned long long>(ticks_per_second);
            const auto age = since_boot > started ? since_boot - started : 0;
            times.created = filetime(real_now) - age;
        }
    }
    return true;
}

void sleep_ms(unsigned milliseconds) noexcept {
    timespec duration{static_cast<time_t>(milliseconds / 1000), static_cast<long>(milliseconds % 1000) * 1000000L};
    while (nanosleep(&duration, &duration) != 0) {
    }
}

} // namespace transmute::fx::host
