#define _CRT_SECURE_NO_WARNINGS
#include "alchemy/darwin_support.hpp"
#include "alchemy/darwin_version.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <tuple>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include "alchemy/host/time.hpp"

namespace alchemy {
namespace {
constexpr std::string_view system_library = "/usr/lib/libSystem.B.dylib";
constexpr std::uint64_t string_limit = 1024 * 1024, transfer_limit = 256ULL * 1024 * 1024;
constexpr std::uint32_t einval = 22, erange = 34, eoverflow = 84;
struct SupportError { std::uint32_t value; };
std::uint64_t add(std::uint64_t address, std::uint64_t amount) {
    if (amount > UINT64_MAX - address) throw SupportError{14};
    return address + amount;
}
void validate(const transmute::GuestMemory& memory, std::uint64_t address, std::uint64_t length, bool writable) {
    const auto end = add(address, length);
    while (address < end) {
        const auto region = memory.query_region(address);
        if (!region || !region->readable || (writable && !region->writable) || address < region->base ||
            address - region->base >= region->size) throw SupportError{14};
        address = std::min(end, add(region->base, region->size));
    }
}
std::string text(const transmute::GuestMemory& memory, std::uint64_t address) {
    bool terminated{};
    const auto bytes = memory.read_string(address, 1, string_limit, &terminated);
    if (!terminated) throw std::runtime_error("Darwin support string exceeds 1 MiB");
    return {bytes.begin(), bytes.end()};
}
std::string prefix(const transmute::GuestMemory& memory, std::uint64_t address,
                   std::uint64_t maximum, bool* terminated = nullptr) {
    bool ended{};
    const auto bytes = memory.read_string(address, 1,
        static_cast<std::size_t>(std::min(maximum, string_limit)), &ended);
    if (!ended && maximum > string_limit) throw std::runtime_error("Darwin bounded string exceeds 1 MiB");
    if (terminated) *terminated = ended;
    return {bytes.begin(), bytes.end()};
}
[[noreturn]] void fortified(std::string_view operation) {
    throw std::runtime_error("Darwin fortified " + std::string(operation) + " exceeds destination bound");
}
void put_text(transmute::GuestMemory& memory, std::uint64_t address, std::string_view value) {
    memory.write_bytes(address, {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
    memory.write8(add(address, value.size()), 0);
}
std::vector<std::uint32_t> wide(const transmute::GuestMemory& memory, std::uint64_t address) {
    std::vector<std::uint32_t> result;
    for (std::uint64_t index = 0; index < string_limit; ++index) {
        const auto value = memory.read32(add(address, index * 4));
        if (!value) return result;
        result.push_back(value);
    }
    throw std::runtime_error("Darwin support wide string exceeds limit");
}
void put_wide(transmute::GuestMemory& memory, std::uint64_t address, std::span<const std::uint32_t> values) {
    for (std::size_t index = 0; index < values.size(); ++index) memory.write32(add(address, index * 4ULL), values[index]);
    memory.write32(add(address, values.size() * 4ULL), 0);
}
std::string encode(std::span<const std::uint32_t> values) {
    std::string output;
    for (const auto value : values) {
        if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) throw SupportError{92};
        if (value < 0x80) output += static_cast<char>(value);
        else if (value < 0x800) { output += static_cast<char>(0xc0 | (value >> 6)); output += static_cast<char>(0x80 | (value & 63)); }
        else if (value < 0x10000) { output += static_cast<char>(0xe0 | (value >> 12)); output += static_cast<char>(0x80 | ((value >> 6) & 63)); output += static_cast<char>(0x80 | (value & 63)); }
        else { output += static_cast<char>(0xf0 | (value >> 18)); output += static_cast<char>(0x80 | ((value >> 12) & 63)); output += static_cast<char>(0x80 | ((value >> 6) & 63)); output += static_cast<char>(0x80 | (value & 63)); }
    }
    return output;
}
std::vector<std::uint32_t> decode(std::string_view input) {
    std::vector<std::uint32_t> result;
    for (std::size_t index = 0; index < input.size();) {
        auto value = static_cast<std::uint8_t>(input[index++]);
        std::uint32_t scalar = value; unsigned continuation = 0; std::uint32_t minimum = 0;
        if (value >= 0xc2 && value <= 0xdf) { scalar = value & 31; continuation = 1; minimum = 0x80; }
        else if (value >= 0xe0 && value <= 0xef) { scalar = value & 15; continuation = 2; minimum = 0x800; }
        else if (value >= 0xf0 && value <= 0xf4) { scalar = value & 7; continuation = 3; minimum = 0x10000; }
        else if (value >= 0x80) throw SupportError{92};
        for (unsigned at = 0; at < continuation; ++at) {
            if (index == input.size()) throw SupportError{92};
            value = static_cast<std::uint8_t>(input[index++]);
            if ((value & 0xc0) != 0x80) throw SupportError{92};
            scalar = (scalar << 6) | (value & 63);
        }
        if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) throw SupportError{92};
        result.push_back(scalar);
    }
    return result;
}
std::string error_text(std::int32_t value) {
    static constexpr std::pair<std::int32_t, const char*> errors[] = {
        {0, "Undefined error: 0"}, {1, "Operation not permitted"}, {2, "No such file or directory"},
        {3, "No such process"}, {4, "Interrupted system call"}, {5, "Input/output error"},
        {6, "Device not configured"}, {7, "Argument list too long"}, {8, "Exec format error"},
        {9, "Bad file descriptor"}, {10, "No child processes"}, {11, "Resource deadlock avoided"},
        {12, "Cannot allocate memory"}, {13, "Permission denied"}, {14, "Bad address"},
        {16, "Resource busy"}, {17, "File exists"}, {18, "Cross-device link"}, {19, "Operation not supported by device"},
        {20, "Not a directory"}, {21, "Is a directory"}, {22, "Invalid argument"},
        {23, "Too many open files in system"}, {24, "Too many open files"}, {25, "Inappropriate ioctl for device"},
        {27, "File too large"}, {28, "No space left on device"}, {29, "Illegal seek"},
        {30, "Read-only file system"}, {31, "Too many links"}, {32, "Broken pipe"},
        {33, "Numerical argument out of domain"}, {34, "Result too large"}, {35, "Resource temporarily unavailable"},
        {45, "Operation not supported"}, {60, "Operation timed out"}, {61, "Connection refused"},
        {63, "File name too long"}, {66, "Directory not empty"}, {78, "Function not implemented"},
        {84, "Value too large to be stored in data type"}, {89, "Operation canceled"}, {92, "Illegal byte sequence"}
    };
    for (const auto& [number, message] : errors) if (value == number) return message;
    return "Unknown error: " + std::to_string(value);
}
std::int64_t floor_div(std::int64_t number, std::int64_t divisor) {
    auto value = number / divisor;
    if (number % divisor < 0) --value;
    return value;
}
std::int64_t year_days(std::int64_t year) {
    --year;
    return year * 365 + floor_div(year, 4) - floor_div(year, 100) + floor_div(year, 400) - 719162;
}
bool leap(std::int64_t year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }
constexpr std::array<int, 12> month_days{0,31,59,90,120,151,181,212,243,273,304,334};
std::pair<std::int64_t, int> iso_week(const std::tm& calendar) {
    const auto weeks = [](std::int64_t year) {
        const auto january_first = (year_days(year) + 4) % 7;
        const auto weekday = january_first < 0 ? january_first + 7 : january_first;
        return weekday == 4 || (weekday == 3 && leap(year)) ? 53 : 52;
    };
    auto year = static_cast<std::int64_t>(calendar.tm_year) + 1900;
    const auto weekday = calendar.tm_wday == 0 ? 7 : calendar.tm_wday;
    auto week = (calendar.tm_yday + 11 - weekday) / 7;
    if (!week) { --year; week = weeks(year); }
    else if (week > weeks(year)) { ++year; week = 1; }
    return {year, week};
}
std::string decimal(std::int64_t value, std::size_t width) {
    auto result = std::to_string(value);
    const std::size_t sign = value < 0 ? 1 : 0;
    if (result.size() - sign < width) result.insert(sign, width - (result.size() - sign), '0');
    return result;
}
std::int64_t civil_seconds(const std::tm& value) {
    auto year = static_cast<std::int64_t>(value.tm_year) + 1900;
    const auto delta = floor_div(value.tm_mon, 12);
    year += delta;
    const auto month = static_cast<int>(value.tm_mon - delta * 12);
    const auto days = year_days(year) + month_days[month] + (month > 1 && leap(year)) + value.tm_mday - 1;
    return days * 86400 + static_cast<std::int64_t>(value.tm_hour) * 3600 +
        static_cast<std::int64_t>(value.tm_min) * 60 + value.tm_sec;
}
std::tm utc_tm(std::int64_t seconds) {
    const auto day = floor_div(seconds, 86400);
    std::int64_t low = static_cast<std::int64_t>(INT32_MIN) + 1900;
    std::int64_t high = static_cast<std::int64_t>(INT32_MAX) + 1901;
    if (day < year_days(low) || day >= year_days(high)) throw SupportError{eoverflow};
    while (high - low > 1) {
        const auto middle = low + (high - low) / 2;
        if (year_days(middle) <= day) low = middle; else high = middle;
    }
    const auto yday = static_cast<int>(day - year_days(low));
    int month = 11;
    while (month > 0 && yday < month_days[month] + (month > 1 && leap(low))) --month;
    const auto daytime = seconds - day * 86400;
    std::tm result{};
    result.tm_year = static_cast<int>(low - 1900); result.tm_mon = month;
    result.tm_mday = yday - month_days[month] - (month > 1 && leap(low)) + 1;
    result.tm_yday = yday; result.tm_wday = static_cast<int>((day + 4) % 7);
    if (result.tm_wday < 0) result.tm_wday += 7;
    result.tm_hour = static_cast<int>(daytime / 3600); result.tm_min = static_cast<int>((daytime / 60) % 60);
    result.tm_sec = static_cast<int>(daytime % 60);
    return result;
}
std::tm read_tm(const transmute::GuestMemory& memory, std::uint64_t address) {
    validate(memory, address, 56, false);
    const auto integer = [&](unsigned offset) { return std::bit_cast<std::int32_t>(memory.read32(address + offset)); };
    std::tm value{};
    value.tm_sec = integer(0); value.tm_min = integer(4); value.tm_hour = integer(8);
    value.tm_mday = integer(12); value.tm_mon = integer(16); value.tm_year = integer(20);
    value.tm_wday = integer(24); value.tm_yday = integer(28); value.tm_isdst = integer(32);
    return value;
}
void write_tm(transmute::GuestMemory& memory, std::uint64_t address, const std::tm& value,
              std::int64_t offset, std::uint64_t zone) {
    validate(memory, address, 56, true);
    const std::array fields{value.tm_sec,value.tm_min,value.tm_hour,value.tm_mday,value.tm_mon,
        value.tm_year,value.tm_wday,value.tm_yday,value.tm_isdst};
    for (std::size_t index = 0; index < fields.size(); ++index) memory.write32(address + index * 4, static_cast<std::uint32_t>(fields[index]));
    memory.write32(address + 36, 0); memory.write64(address + 40, std::bit_cast<std::uint64_t>(offset));
    memory.write64(address + 48, zone);
}

struct State {
    ProcessContext& context;
    std::mutex mutex;
    std::map<std::pair<std::uint64_t, bool>, std::uint64_t> error_buffers;
    std::map<std::uint64_t, std::uint64_t> tm_buffers;
    std::uint64_t utc_name{}, names{}, timezone{}, daylight{}, tz_key{};
    std::array<std::uint64_t, 2> zone_names{};
    explicit State(ProcessContext& value) : context(value) {
        utc_name = string("UTC"); tz_key = string("TZ");
        names = context.allocate_guest(16); timezone = context.allocate_guest(8); daylight = context.allocate_guest(4);
        zone_names = {utc_name, utc_name};
    }
    std::uint64_t string(std::string_view value) {
        const auto address = context.allocate_guest(value.size() + 1);
        put_text(context.calling_engine(), address, value);
        return address;
    }
    std::uint64_t call(std::string_view symbol, std::span<const std::uint64_t> arguments) {
        const auto exported = context.resolve(system_library, symbol);
        if (!exported) throw std::runtime_error("Darwin support requires service " + std::string(symbol));
        return context.calling_engine().invoke_callback(exported->address, arguments);
    }
    bool utc() {
        const std::array arguments{tz_key};
        const auto address = call("_getenv", arguments);
        if (!address) return false;
        const auto name = text(context.calling_engine(), address);
        if (name.empty()) return false;
        if (name == "UTC" || name == "UTC0" || name == "GMT" || name == "GMT0" || name == "Etc/UTC") return true;
        throw std::runtime_error("Darwin time: unsupported guest TZ setting " + name);
    }
    void zones() {
        const bool universal = utc();
        std::array<std::string, 2> labels{"UTC", "UTC"};
        long offset = 0; int summer = 0;
#ifdef _WIN32
        if (!universal) {
            _tzset();
            if (_get_timezone(&offset) || _get_daylight(&summer)) throw SupportError{einval};
            for (int index = 0; index != 2; ++index) {
                std::array<char, 256> buffer{}; std::size_t needed{};
                if (_get_tzname(&needed, buffer.data(), buffer.size(), index)) throw SupportError{einval};
                labels[static_cast<std::size_t>(index)] = buffer.data();
            }
        }
#else
        if (!universal) {
            const auto now = std::time(nullptr); std::tm local{};
            if (!localtime_r(&now, &local)) throw SupportError{eoverflow};
            offset = static_cast<long>(now - civil_seconds(local)); summer = local.tm_isdst > 0;
            std::array<char, 128> label{}; std::strftime(label.data(), label.size(), "%Z", &local);
            labels = {label.data(), label.data()};
        }
#endif
        std::lock_guard lock(mutex);
        for (std::size_t index = 0; index < labels.size(); ++index) {
            zone_names[index] = labels[index] == "UTC" ? utc_name : string(labels[index]);
            context.calling_engine().write64(names + index * 8, zone_names[index]);
        }
        context.calling_engine().write64(timezone, static_cast<std::uint64_t>(offset));
        context.calling_engine().write32(daylight, static_cast<std::uint32_t>(summer));
    }
    std::uint64_t error_buffer(bool signal, std::string_view value) {
        std::uint64_t address;
        { std::lock_guard lock(mutex);
            auto& slot = error_buffers[{context.current_thread()->identity, signal}];
            if (!slot) slot = context.allocate_guest(256);
            address = slot;
        }
        put_text(context.calling_engine(), address, value);
        return address;
    }
    std::uint64_t tm_buffer() {
        std::lock_guard lock(mutex);
        auto& slot = tm_buffers[context.current_thread()->identity];
        if (!slot) slot = context.allocate_guest(56);
        return slot;
    }
    std::uint64_t convert_time(transmute::GuestMemory& memory, std::uint64_t source, std::uint64_t destination, bool local) {
        const auto seconds = std::bit_cast<std::int64_t>(memory.read64(source));
        std::tm value;
        std::int64_t offset = 0;
        std::uint64_t name = utc_name;
        if (!local || utc()) value = utc_tm(seconds);
        else {
            const auto host = static_cast<std::time_t>(seconds);
#ifdef _WIN32
            if (localtime_s(&value, &host)) throw SupportError{eoverflow};
#else
            if (!localtime_r(&host, &value)) throw SupportError{eoverflow};
#endif
            offset = civil_seconds(value) - seconds;
            std::lock_guard lock(mutex);
            name = zone_names[value.tm_isdst > 0 ? 1 : 0];
        }
        write_tm(memory, destination, value, offset, name);
        return destination;
    }
    std::uint64_t make_time(transmute::GuestMemory& memory, std::uint64_t address, bool universal) {
        auto value = read_tm(memory, address);
        std::int64_t result;
        std::int64_t offset = 0; std::uint64_t name = utc_name;
        if (universal || utc()) { result = civil_seconds(value); value = utc_tm(result); }
        else {
            errno = 0;
            const auto host = std::mktime(&value);
            if (host == static_cast<std::time_t>(-1) && errno) throw SupportError{eoverflow};
            result = static_cast<std::int64_t>(host); offset = civil_seconds(value) - result;
            std::lock_guard lock(mutex); name = zone_names[value.tm_isdst > 0 ? 1 : 0];
        }
        write_tm(memory, address, value, offset, name);
        return std::bit_cast<std::uint64_t>(result);
    }
};
using Operation = std::function<std::uint64_t(transmute::CpuState&, transmute::GuestMemory&)>;
void install(const std::shared_ptr<State>& owner, std::string symbol, Operation operation, std::uint64_t failure = UINT64_MAX) {
    static_cast<void>(owner->context.install_function(std::move(symbol), [owner, operation = std::move(operation), failure](auto& state, auto& memory) {
        try { state.x[0] = operation(state, memory); }
        catch (const SupportError& error) { owner->context.set_errno(error.value); state.x[0] = failure; }
        catch (const std::bad_alloc&) { owner->context.set_errno(12); state.x[0] = failure; }
    }));
}
std::uint64_t cpu_nanoseconds(bool thread) {
    const auto value = thread ? host::time::thread_cpu_ns() : host::time::process_cpu_ns();
    if (!value) throw SupportError{5};   // EIO: the host has no CPU time for it
    return *value;
}
std::uint64_t uptime_nanoseconds(bool continuous) {
    return continuous ? host::time::continuous_ns() : host::time::monotonic_ns();
}
std::uint64_t nanoseconds(std::uint64_t clock) {
    if (clock == 0) return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    if (clock == 4 || clock == 5 || clock == 6) return uptime_nanoseconds(true);
    if (clock == 8 || clock == 9) return uptime_nanoseconds(false);
    if (clock == 12 || clock == 16) return cpu_nanoseconds(clock == 16);
    throw SupportError{einval};
}
} // namespace

void install_darwin_support(ProcessContext& context) {
    auto state = std::make_shared<State>(context);
    const auto missing = [&](std::string symbol, Operation operation, std::uint64_t failure = UINT64_MAX) {
        if (!context.resolve(system_library, symbol)) install(state, std::move(symbol), std::move(operation), failure);
    };
    // Darwin ARM64 stack probing receives byte size in W9. It does not adjust SP.
    // Preserve even the permitted scratch registers and validate every intervening page.
    static_cast<void>(context.install_function("___chkstk_darwin", [](auto& s, auto& memory) {
        const auto size = static_cast<std::uint32_t>(s.x[9]);
        if (size > s.sp) throw std::runtime_error("Darwin stack probe address underflow");
        try { validate(memory, s.sp - size, size, true); }
        catch (const SupportError&) { throw std::runtime_error("Darwin stack probe exceeds checked stack bounds"); }
    }));
    // libm (double, float and long double): install_darwin_numeric (numeric_math.cpp).
    install(state, "___isPlatformVersionAtLeast", [](auto& s, auto&) {
        // Platform 1 is macOS. This is Alchemy's supported API profile, not image SDK metadata.
        const auto at_least = [&](std::uint64_t major, std::uint64_t minor, std::uint64_t patch) {
            return std::tuple{major, minor, patch} <= std::tuple{std::uint64_t{macos_major}, std::uint64_t{macos_minor},
                                                                 std::uint64_t{macos_patch}};
        };
        return static_cast<std::uint64_t>(s.x[0] == 1 && at_least(s.x[1], s.x[2], s.x[3]));
    });
    install(state, "___isPlatformOrVariantPlatformVersionAtLeast", [](auto& s, auto&) {
        // (platform, major, minor, subminor, variant platform, major, minor,
        // subminor): true when either check holds. Alchemy is macOS only.
        const auto at_least = [&](std::uint64_t major, std::uint64_t minor, std::uint64_t patch) {
            return std::tuple{major, minor, patch} <= std::tuple{std::uint64_t{macos_major}, std::uint64_t{macos_minor},
                                                                 std::uint64_t{macos_patch}};
        };
        const bool first = (s.x[0] & 0xffffffffU) == 1 && at_least(s.x[1] & 0xffffffffU, s.x[2] & 0xffffffffU, s.x[3] & 0xffffffffU);
        const bool second = (s.x[4] & 0xffffffffU) == 1 && at_least(s.x[5] & 0xffffffffU, s.x[6] & 0xffffffffU, s.x[7] & 0xffffffffU);
        return static_cast<std::uint64_t>(first || second);
    });
    missing("_remainderf", [](auto& s, auto&) {
        const auto value = std::remainder(std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0])),
                                          std::bit_cast<float>(static_cast<std::uint32_t>(s.v[1][0])));
        s.v[0] = {std::bit_cast<std::uint32_t>(value), 0}; return s.x[0];
    });
    for (const auto move : {false, true}) install(state, move ? "___memmove_chk" : "___memcpy_chk", [move](auto& s, auto& m) {
        if (s.x[2] > s.x[3]) throw std::runtime_error("Darwin fortified memory copy exceeds destination bound");
        m.copy_bytes(s.x[0], s.x[1], s.x[2], move); return s.x[0];
    });
    install(state, "___strcpy_chk", [](auto& s, auto& m) { const auto value = text(m, s.x[1]); if (value.size() >= s.x[2]) throw std::runtime_error("Darwin fortified strcpy exceeds destination bound"); put_text(m, s.x[0], value); return s.x[0]; });
    missing("___memset_chk", [](auto& s, auto& m) {
        if (s.x[2] > s.x[3]) fortified("memset");
        if (s.x[2] > transfer_limit) throw SupportError{einval};
        validate(m, s.x[0], s.x[2], true); m.fill_bytes(s.x[0], static_cast<std::uint8_t>(s.x[1]), s.x[2]); return s.x[0];
    });
    missing("___strncpy_chk", [](auto& s, auto& m) {
        if (s.x[2] > s.x[3]) fortified("strncpy");
        if (s.x[2] > transfer_limit) throw SupportError{einval};
        const auto value = prefix(m, s.x[1], s.x[2]);
        validate(m, s.x[0], s.x[2], true);
        m.write_bytes(s.x[0], {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
        m.fill_bytes(add(s.x[0], value.size()), 0, s.x[2] - value.size()); return s.x[0];
    });
    for (const auto bounded : {false, true}) missing(bounded ? "___strncat_chk" : "___strcat_chk", [bounded](auto& s, auto& m) {
        const auto bound = s.x[bounded ? 3 : 2]; bool ended{};
        const auto destination = prefix(m, s.x[0], bound, &ended);
        if (!ended) fortified(bounded ? "strncat" : "strcat");
        const auto source = bounded ? prefix(m, s.x[1], s.x[2]) : text(m, s.x[1]);
        const auto length = add(destination.size(), source.size());
        if (length >= bound) fortified(bounded ? "strncat" : "strcat");
        validate(m, s.x[0], length + 1, true); put_text(m, s.x[0], destination + source); return s.x[0];
    });
    for (const auto concatenate : {false, true}) missing(concatenate ? "___strlcat_chk" : "___strlcpy_chk", [concatenate](auto& s, auto& m) {
        const auto capacity = s.x[2];
        if (capacity > s.x[3]) fortified(concatenate ? "strlcat" : "strlcpy");
        const auto source = text(m, s.x[1]);
        if (!capacity) return static_cast<std::uint64_t>(source.size());
        bool ended{};
        const auto destination = concatenate ? prefix(m, s.x[0], capacity, &ended) : std::string{};
        if (concatenate && !ended) return add(capacity, source.size());
        const auto count = std::min<std::uint64_t>(source.size(), capacity - destination.size() - 1);
        validate(m, s.x[0], destination.size() + count + 1, true);
        put_text(m, s.x[0], destination + source.substr(0, static_cast<std::size_t>(count)));
        return add(destination.size(), source.size());
    });
    missing("_strncat", [](auto& s, auto& m) {
        const auto destination = text(m, s.x[0]), source = prefix(m, s.x[1], s.x[2]);
        validate(m, s.x[0], add(add(destination.size(), source.size()), 1), true);
        put_text(m, s.x[0], destination + source); return s.x[0];
    });
    for (const auto span : {false, true}) missing(span ? "_strspn" : "_strpbrk", [span](auto& s, auto& m) {
        std::array<bool, 256> characters{};
        for (const auto character : text(m, s.x[1])) characters[static_cast<unsigned char>(character)] = true;
        for (std::uint64_t at = 0; at < string_limit; ++at) {
            const auto value = m.read8(add(s.x[0], at));
            if (span ? (!value || !characters[value]) : !value) return span ? at : std::uint64_t{0};
            if (!span && characters[value]) return add(s.x[0], at);
        }
        throw std::runtime_error("Darwin string search exceeds 1 MiB");
    }, 0);
    for (const auto bounded : {false, true}) for (const auto localized : {false, true})
        missing(std::string(bounded ? "_strncasecmp" : "_strcasecmp") + (localized ? "_l" : ""), [state, bounded, localized](auto& s, auto& m) {
            if (localized) {
                const std::array arguments{0ULL, s.x[bounded ? 3 : 2]};
                if (state->call("_tolower_l", arguments) != 0) throw SupportError{einval};
            }
            const auto maximum = bounded ? std::min(s.x[2], string_limit) : string_limit;
            const auto lower = [](std::uint8_t value) { return static_cast<std::uint8_t>(value >= 'A' && value <= 'Z' ? value + 32 : value); };
            for (std::uint64_t at = 0; at < maximum; ++at) {
                const auto left = lower(m.read8(add(s.x[0], at))), right = lower(m.read8(add(s.x[1], at)));
                if (left != right) return static_cast<std::uint64_t>(static_cast<std::int64_t>(left) - right);
                if (!left) return std::uint64_t{0};
            }
            if (!bounded || s.x[2] > string_limit) throw std::runtime_error("Darwin case comparison exceeds 1 MiB");
            return std::uint64_t{0};
        });
    missing("_bsearch", [state](auto& s, auto& m) {
        const auto key = s.x[0], base = s.x[1], count = s.x[2], width = s.x[3], comparator = s.x[4];
        if (!count) return std::uint64_t{0};
        if (!width || count > transfer_limit / width) throw SupportError{einval};
        validate(m, base, count * width, false);
        std::uint64_t low{}, high = count;
        while (low < high) {
            const auto middle = low + (high - low) / 2, address = add(base, middle * width);
            const std::array arguments{key, address};
            const auto result = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(state->context.calling_engine().invoke_callback(comparator, arguments)));
            if (!result) return address;
            if (result < 0) high = middle; else low = middle + 1;
        }
        return std::uint64_t{0};
    }, 0);
    install(state, "___sprintf_chk", [state](auto& s, auto&) {
        const std::array probe{0ULL, 0ULL, s.x[3], s.sp}; const auto count = state->call("_vsnprintf", probe);
        if (count == UINT32_MAX || count == UINT64_MAX) return count;
        if (count >= s.x[2]) throw std::runtime_error("Darwin fortified sprintf exceeds destination bound");
        const std::array arguments{s.x[0], s.x[2], s.x[3], s.sp}; return state->call("_vsnprintf", arguments);
    });
    install(state, "___snprintf_chk", [state](auto& s, auto&) {
        if (s.x[1] > s.x[3]) throw std::runtime_error("Darwin fortified snprintf exceeds destination bound");
        const std::array arguments{s.x[0], s.x[1], s.x[4], s.sp}; return state->call("_vsnprintf", arguments);
    });
    install(state, "___chk_fail", [](auto&, auto&) -> std::uint64_t { throw std::runtime_error("Darwin fortified operation failed"); });
    install(state, "_strcspn", [](auto& s, auto& m) { const auto source = text(m, s.x[0]), reject = text(m, s.x[1]); const auto at = source.find_first_of(reject); return static_cast<std::uint64_t>(at == std::string::npos ? source.size() : at); });
    for (const auto bounded : {false, true}) install(state, bounded ? "_wcsncmp" : "_wcscmp", [bounded](auto& s, auto& m) {
        const auto maximum = bounded ? std::min(s.x[2], string_limit) : string_limit;
        for (std::uint64_t at = 0; at < maximum; ++at) { const auto a = m.read32(add(s.x[0], at * 4)), b = m.read32(add(s.x[1], at * 4)); if (a != b) return static_cast<std::uint64_t>(static_cast<std::int64_t>((a > b) - (a < b))); if (!a) return std::uint64_t{0}; }
        if (!bounded || s.x[2] > string_limit) throw std::runtime_error("Darwin wide comparison exceeds limit");
        return std::uint64_t{0};
    });
    install(state, "_wcscpy", [](auto& s, auto& m) { const auto value = wide(m, s.x[1]); put_wide(m, s.x[0], value); return s.x[0]; });
    install(state, "_wcsncpy", [](auto& s, auto& m) { if (s.x[2] > string_limit) throw SupportError{einval}; bool ended = false; for (std::uint64_t at = 0; at < s.x[2]; ++at) { const auto value = ended ? 0 : m.read32(add(s.x[1], at * 4)); ended = !value; m.write32(add(s.x[0], at * 4), value); } return s.x[0]; });
    for (const auto reverse : {false, true}) install(state, reverse ? "_wcsrchr" : "_wcschr", [reverse](auto& s, auto& m) {
        std::uint64_t found = 0;
        for (std::uint64_t at = 0; at < string_limit; ++at) { const auto address = add(s.x[0], at * 4); const auto value = m.read32(address); if (value == static_cast<std::uint32_t>(s.x[1])) { found = address; if (!reverse) return found; } if (!value) return found; }
        throw std::runtime_error("Darwin wide search exceeds limit");
    }, 0);
    install(state, "_wcstok", [](auto& s, auto& m) {
        const auto delimiters = wide(m, s.x[1]);
        auto current = s.x[0] ? s.x[0] : m.read64(s.x[2]);
        if (!current) return std::uint64_t{0};
        const auto delimiter = [&](std::uint32_t value) { return std::find(delimiters.begin(), delimiters.end(), value) != delimiters.end(); };
        std::uint64_t inspected = 0;
        while (delimiter(m.read32(current))) { if (++inspected == string_limit) throw std::runtime_error("Darwin wide token exceeds limit"); current = add(current, 4); }
        if (!m.read32(current)) { m.write64(s.x[2], 0); return std::uint64_t{0}; }
        const auto result = current;
        while (const auto value = m.read32(current)) {
            if (delimiter(value)) { m.write32(current, 0); m.write64(s.x[2], add(current, 4)); return result; }
            if (++inspected == string_limit) throw std::runtime_error("Darwin wide token exceeds limit");
            current = add(current, 4);
        }
        m.write64(s.x[2], 0); return result;
    }, 0);
    for (const auto narrow : {false, true}) install(state, narrow ? "_wcstombs" : "_mbstowcs", [state, narrow](auto& s, auto& m) {
        const auto cell = state->context.allocate_guest(8);
        try { m.write64(cell, s.x[1]); const std::array arguments{s.x[0], cell, s.x[2], 0ULL}; const auto result = state->call(narrow ? "_wcsrtombs" : "_mbsrtowcs", arguments); state->context.release_guest(cell); return result; }
        catch (...) { state->context.release_guest(cell); throw; }
    });
    install(state, "_memset_pattern16", [](auto& s, auto& m) { if (s.x[2] > transfer_limit) throw SupportError{einval}; std::array<std::uint8_t, 16> pattern{}; m.read_bytes(s.x[1], pattern); for (std::uint64_t at = 0; at < s.x[2]; at += 16) m.write_bytes(add(s.x[0], at), std::span(pattern).first(static_cast<std::size_t>(std::min<std::uint64_t>(16, s.x[2] - at)))); return s.x[0]; });
    // qsort, qsort_r (a thunk before the two elements) and qsort_b (a block:
    // its invoke function gets the block, then the two elements). A heap sort,
    // so the comparator sees only elements of the array.
    enum class Comparator { plain, thunk, block };
    const auto sorter = [state](Comparator kind) {
        return [state, kind](auto& s, auto& m) {
            const auto base = s.x[0], count = s.x[1], width = s.x[2];
            const auto comparator = kind == Comparator::thunk ? s.x[4] : s.x[3];
            if (count < 2) return base;
            if (!width || count > transfer_limit / width) throw SupportError{einval};
            validate(m, base, count * width, true);
            const auto function = kind == Comparator::block ? m.read64(comparator + 16) : comparator;
            const auto compare = [&](std::uint64_t left, std::uint64_t right) {
                const auto a = base + left * width, b = base + right * width;
                const auto value = kind == Comparator::plain
                    ? state->context.calling_engine().invoke_callback(function, std::array{a, b})
                    : state->context.calling_engine().invoke_callback(function, std::array{kind == Comparator::thunk ? s.x[3] : comparator, a, b});
                return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));
            };
            std::vector<std::uint8_t> left(static_cast<std::size_t>(width)), right(left.size());
            const auto swap = [&](std::uint64_t first, std::uint64_t second) {
                if (first == second) return;
                m.read_bytes(base + first * width, left); m.read_bytes(base + second * width, right);
                m.write_bytes(base + first * width, right); m.write_bytes(base + second * width, left);
            };
            const auto sift = [&](std::uint64_t root, std::uint64_t end) {
                while (root < end / 2) {
                    auto child = root * 2 + 1;
                    if (child + 1 < end && compare(child, child + 1) < 0) ++child;
                    if (compare(root, child) >= 0) return;
                    swap(root, child); root = child;
                }
            };
            for (auto root = count / 2; root != 0;) sift(--root, count);
            for (auto end = count; end > 1;) { swap(0, --end); sift(0, end); }
            return base;
        };
    };
    install(state, "_qsort", sorter(Comparator::plain));
    install(state, "_qsort_r", sorter(Comparator::thunk));
    install(state, "_qsort_b", sorter(Comparator::block));
    install(state, "_strerror", [state](auto& s, auto&) { return state->error_buffer(false, error_text(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(s.x[0])))); }, 0);
    install(state, "_strerror_r", [](auto& s, auto& m) { const auto value = error_text(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(s.x[0]))); if (s.x[2] <= value.size()) return std::uint64_t{erange}; put_text(m, s.x[1], value); return std::uint64_t{0}; });
    install(state, "_perror", [state](auto& s, auto& m) { const auto error = m.read32(state->context.current_thread()->errno_address); std::string value = s.x[0] ? text(m, s.x[0]) : ""; if (!value.empty()) value += ": "; value += error_text(std::bit_cast<std::int32_t>(error)) + '\n'; std::span bytes(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()); while (!bytes.empty()) { const auto result = state->context.output(2, bytes); if (result.error || !result.bytes_written || result.bytes_written > bytes.size()) throw std::runtime_error("Darwin perror output failed"); bytes = bytes.subspan(result.bytes_written); } return s.x[0]; });
    install(state, "_strsignal", [state](auto& s, auto&) {
        static constexpr const char* descriptions[] = {"Unknown signal", "Hangup", "Interrupt", "Quit", "Illegal instruction", "Trace/BPT trap", "Abort trap", "EMT trap", "Floating point exception", "Killed", "Bus error", "Segmentation fault", "Bad system call", "Broken pipe", "Alarm clock", "Terminated", "Urgent I/O condition", "Stopped (signal)", "Stopped", "Continued", "Child exited", "Stopped (tty input)", "Stopped (tty output)", "I/O possible", "Cputime limit exceeded", "Filesize limit exceeded", "Virtual timer expired", "Profiling timer expired", "Window size changes", "Information request", "User defined signal 1", "User defined signal 2"};
        return state->error_buffer(true, s.x[0] < std::size(descriptions) ? descriptions[s.x[0]] : "Unknown signal: " + std::to_string(s.x[0]));
    }, 0);
    install(state, "_gmtime_r", [state](auto& s, auto& m) { return state->convert_time(m, s.x[0], s.x[1], false); }, 0);
    install(state, "_localtime_r", [state](auto& s, auto& m) { return state->convert_time(m, s.x[0], s.x[1], true); }, 0);
    install(state, "_gmtime", [state](auto& s, auto& m) { return state->convert_time(m, s.x[0], state->tm_buffer(), false); }, 0);
    install(state, "_localtime", [state](auto& s, auto& m) { return state->convert_time(m, s.x[0], state->tm_buffer(), true); }, 0);
    install(state, "_mktime", [state](auto& s, auto& m) { return state->make_time(m, s.x[0], false); });
    install(state, "_timegm", [state](auto& s, auto& m) { return state->make_time(m, s.x[0], true); });
    install(state, "_tzset", [state](auto& s, auto&) { state->zones(); return s.x[0]; });
    install(state, "_wcsftime", [](auto& s, auto& m) {
        if (!s.x[1]) return std::uint64_t{0};
        if (s.x[1] > string_limit) throw SupportError{einval};
        const auto specification = encode(wide(m, s.x[2])); const auto tm = read_tm(m, s.x[3]);
        if (tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31 || tm.tm_hour < 0 || tm.tm_hour > 23 || tm.tm_min < 0 || tm.tm_min > 59 || tm.tm_sec < 0 || tm.tm_sec > 60 || tm.tm_wday < 0 || tm.tm_wday > 6 || tm.tm_yday < 0 || tm.tm_yday > 365) throw SupportError{einval};
        std::string output;
        for (std::size_t index = 0; index < specification.size(); ++index) {
            if (specification[index] != '%') { output += specification[index]; continue; }
            if (++index == specification.size()) throw std::runtime_error("Darwin wcsftime incomplete conversion");
            const auto conversion = specification[index];
            if (conversion == '%') output += '%';
            else if (conversion == 'n' || conversion == 't') output += conversion == 'n' ? '\n' : '\t';
            else if (conversion == 'Z') { const auto zone = m.read64(s.x[3] + 48); if (zone) output += text(m, zone); }
            else if (conversion == 'z') { const auto offset = std::bit_cast<std::int64_t>(m.read64(s.x[3] + 40)); if (offset < -86400 || offset > 86400) throw SupportError{einval}; const auto absolute = offset < 0 ? -offset : offset; std::array<char,16> buffer{}; std::snprintf(buffer.data(), buffer.size(), "%c%02lld%02lld", offset < 0 ? '-' : '+', static_cast<long long>(absolute / 3600), static_cast<long long>((absolute / 60) % 60)); output += buffer.data(); }
            else if (conversion == 'e') { if (tm.tm_mday < 10) output += ' '; output += std::to_string(tm.tm_mday); }
            else if (conversion == 'u') output += std::to_string(tm.tm_wday == 0 ? 7 : tm.tm_wday);
            else if (conversion == 'C') output += decimal(floor_div(static_cast<std::int64_t>(tm.tm_year) + 1900, 100), 2);
            else if (conversion == 'G' || conversion == 'g' || conversion == 'V') {
                const auto [year, week] = iso_week(tm);
                if (conversion == 'G') output += decimal(year, 4);
                else if (conversion == 'g') output += decimal((year % 100 + 100) % 100, 2);
                else output += decimal(week, 2);
            }
            else {
                std::string format = "%"; format += conversion;
                if (conversion == 'F') format = "%Y-%m-%d";
                else if (conversion == 'D') format = "%m/%d/%y";
                else if (conversion == 'R') format = "%H:%M";
                else if (conversion == 'T') format = "%H:%M:%S";
                else if (conversion == 'r') format = "%I:%M:%S %p";
                else if (std::string_view("aAbBcdHIjmMpSUwWxXyY").find(conversion) == std::string_view::npos) throw std::runtime_error(std::string("unsupported Darwin wcsftime conversion %") + conversion);
                std::array<char,256> buffer{}; const auto written = std::strftime(buffer.data(), buffer.size(), format.c_str(), &tm);
                if (!written) return std::uint64_t{0};
                output.append(buffer.data(), written);
            }
            if (output.size() > string_limit) throw SupportError{erange};
        }
        const auto result = decode(output);
        if (result.size() >= s.x[1]) return std::uint64_t{0};
        put_wide(m, s.x[0], result); return static_cast<std::uint64_t>(result.size());
    }, 0);
    context.install_export(std::string(system_library), "_tzname", {state->names, ExportKind::data});
    context.install_export(std::string(system_library), "_timezone", {state->timezone, ExportKind::data});
    context.install_export(std::string(system_library), "_daylight", {state->daylight, ExportKind::data});
    state->zones();
    install(state, "_clock", [](auto&, auto&) { return cpu_nanoseconds(false) / 1000; });
    install(state, "_mach_absolute_time", [](auto&, auto&) { return uptime_nanoseconds(false); });
    install(state, "_mach_continuous_time", [](auto&, auto&) { return uptime_nanoseconds(true); });
    install(state, "_mach_timebase_info", [](auto& s, auto& m) { validate(m, s.x[0], 8, true); m.write32(s.x[0], 1); m.write32(s.x[0] + 4, 1); return std::uint64_t{0}; });
    install(state, "_clock_gettime", [](auto& s, auto& m) { const auto value = nanoseconds(s.x[0]); validate(m, s.x[1], 16, true); m.write64(s.x[1], value / 1000000000); m.write64(s.x[1] + 8, value % 1000000000); return std::uint64_t{0}; });
    install(state, "_clock_gettime_nsec_np", [](auto& s, auto&) { return nanoseconds(s.x[0]); }, 0);
    install(state, "_clock_getres", [](auto& s, auto& m) { static_cast<void>(nanoseconds(s.x[0])); if (s.x[1]) { validate(m, s.x[1], 16, true); m.write64(s.x[1], 0); m.write64(s.x[1] + 8, 100); } return std::uint64_t{0}; });
}
} // namespace alchemy
