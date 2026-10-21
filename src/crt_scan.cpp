#include "crt_scan.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace transmute {
namespace {

constexpr std::size_t maximum_scan_units = 1024U * 1024U;

bool writable(GuestMemory& memory, std::uint64_t address, std::size_t bytes) {
    if (address == 0 || address > UINT64_MAX - bytes) return false;
    while (bytes != 0) {
        const auto region = memory.query_region(address);
        if (!region || !region->writable) return false;
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
            bytes, region->size - (address - region->base)));
        if (count == 0) return false;
        address += count;
        bytes -= count;
    }
    return true;
}

template<class Character>
std::basic_string<Character> read_text(GuestMemory& memory,
                                     std::uint64_t address,
                                     std::uint64_t limit) {
    const auto bound = std::min<std::uint64_t>(limit, maximum_scan_units);
    // Preserve the old pre-read overflow check, including its final increment.
    // A terminator before that boundary must still end the input successfully.
    const auto safe_bound = std::min<std::uint64_t>(
        bound, (UINT64_MAX - address) / sizeof(Character));
    bool terminated = false;
    const auto bytes = memory.read_string(address, sizeof(Character),
        static_cast<std::size_t>(safe_bound), &terminated);
    std::basic_string<Character> result(bytes.size() / sizeof(Character), Character{});
    if constexpr (sizeof(Character) == 1) {
        if (!bytes.empty()) std::memcpy(result.data(), bytes.data(), bytes.size());
    } else {
        for (std::size_t index = 0; index < result.size(); ++index)
            result[index] = static_cast<Character>(
                bytes[index * 2] | static_cast<std::uint16_t>(bytes[index * 2 + 1]) << 8);
    }
    if (terminated) return result;
    if (safe_bound < bound)
        throw std::runtime_error("guest scanf string address overflow");
    if (limit > maximum_scan_units)
        throw std::runtime_error("guest scanf string exceeds supported size");
    return result;
}

struct Conversion {
    std::size_t end{};
    std::size_t width{};
    std::string length;
    char type{};
    bool suppressed{};
};

template<class Character>
Conversion parse_conversion(std::basic_string_view<Character> format,
                            std::size_t start) {
    Conversion conversion;
    auto index = start + 1;
    if (index < format.size() && format[index] == '*') {
        conversion.suppressed = true;
        ++index;
    }
    while (index < format.size() && format[index] >= '0' && format[index] <= '9') {
        conversion.width = conversion.width * 10 + (format[index++] - '0');
        if (conversion.width > maximum_scan_units)
            throw std::runtime_error("guest scanf field width is too large");
    }
    if (index < format.size() &&
        std::string_view("hljztLwI").find(static_cast<char>(format[index])) !=
            std::string_view::npos) {
        const auto first = static_cast<char>(format[index++]);
        conversion.length.push_back(first);
        if (index < format.size() &&
            ((first == 'h' && format[index] == 'h') ||
             (first == 'l' && format[index] == 'l'))) {
            conversion.length.push_back(static_cast<char>(format[index++]));
        } else if (first == 'I' && index + 1 < format.size() &&
                   ((format[index] == '3' && format[index + 1] == '2') ||
                    (format[index] == '6' && format[index + 1] == '4'))) {
            conversion.length.push_back(static_cast<char>(format[index++]));
            conversion.length.push_back(static_cast<char>(format[index++]));
        }
    }
    if (index == format.size())
        throw std::runtime_error("incomplete guest scanf conversion");
    conversion.type = static_cast<char>(format[index++]);
    if (conversion.type == '[') {
        if (index < format.size() && format[index] == '^') ++index;
        if (index < format.size() && format[index] == ']') ++index;
        while (index < format.size() && format[index] != ']') ++index;
        if (index == format.size())
            throw std::runtime_error("incomplete guest scanf scanset");
        ++index;
    } else if (std::string_view("diuoxXfFeEgGaAcCsSpn").find(conversion.type) ==
               std::string_view::npos) {
        throw std::runtime_error("unsupported guest scanf conversion");
    }
    conversion.end = index;
    return conversion;
}

std::size_t number_size(const Conversion& conversion) {
    const auto& length = conversion.length;
    if (std::string_view("fFeEgGaA").find(conversion.type) != std::string_view::npos) {
        if (length.empty()) return 4;
        if (length == "l" || length == "L") return 8;
    } else if (conversion.type == 'p' && length.empty()) {
        return 8;
    } else {
        if (length == "hh") return 1;
        if (length == "h") return 2;
        if (length.empty() || length == "l" || length == "I32") return 4;
        if (length == "ll" || length == "j" || length == "z" ||
            length == "t" || length == "I" || length == "I64") return 8;
    }
    throw std::runtime_error("unsupported guest scanf length modifier");
}

template<class Character>
int scan(const CpuState& state, GuestMemory& memory, _locale_t locale) {
    static_assert(std::is_pointer_v<va_list> && sizeof(wchar_t) == 2);
    const auto input = read_text<Character>(memory, state.x[1], state.x[2]);
    const auto format = read_text<Character>(memory, state.x[3], UINT64_MAX);
    const bool secure = (state.x[0] & _CRT_INTERNAL_SCANF_SECURECRT) != 0;
    const bool legacy_wide = (state.x[0] & _CRT_INTERNAL_SCANF_LEGACY_WIDE_SPECIFIERS) != 0;
    std::size_t argument_count = 0;
    const auto next_argument = [&] {
        if (state.x[5] == 0 || argument_count >= 256 ||
            state.x[5] > UINT64_MAX - (argument_count + 1) * 8)
            throw std::runtime_error("invalid guest scanf argument list");
        return memory.read64(state.x[5] + argument_count++ * 8);
    };
    std::size_t position = 0;
    std::size_t cursor = 0;
    int assignments = 0;
    while (position < format.size()) {
        auto start = position;
        while (start < format.size()) {
            if (format[start] != '%') { ++start; continue; }
            if (start + 1 < format.size() && format[start + 1] == '%') {
                start += 2;
                continue;
            }
            break;
        }
        const bool final_literals = start == format.size();
        const auto conversion = final_literals ? Conversion{format.size()} :
            parse_conversion<Character>(format, start);
        auto fragment = format.substr(position, conversion.end - position);
        // A host-owned count observes exactly how much the UCRT fragment read.
        // Keeping one conversion per fragment avoids inferring writes from the
        // assignment count, which excludes suppression and %n.
        for (const auto character : std::string_view("%I64n"))
            fragment.push_back(static_cast<Character>(character));
        position = conversion.end;
        std::int64_t consumed = -1;
        std::array<std::uint64_t, 3> slots{};
        std::size_t slot_count = 0;
        std::uint64_t destination = 0;
        std::size_t bytes = 0;
        std::size_t units = 0;
        unsigned unit_bytes = 1;
        bool text = false;
        bool terminated = false;
        std::vector<std::uint64_t> storage;
        if (!final_literals && !conversion.suppressed) {
            destination = next_argument();
            if (destination == 0) { errno = EINVAL; return EOF; }
            text = std::string_view("cCsS[").find(conversion.type) != std::string_view::npos;
            if (text) {
                if (!conversion.length.empty() && conversion.length != "h" &&
                    conversion.length != "l" && conversion.length != "w")
                    throw std::runtime_error("unsupported guest scanf string modifier");
                const bool wide_output = conversion.length == "l" ||
                    conversion.length == "w" || (conversion.length != "h" &&
                    (sizeof(Character) == 2
                        ? (legacy_wide ? conversion.type != 'C' && conversion.type != 'S'
                                       : conversion.type == 'C' || conversion.type == 'S')
                        : conversion.type == 'C' || conversion.type == 'S'));
                unit_bytes = wide_output ? 2U : 1U;
                terminated = conversion.type != 'c' && conversion.type != 'C';
                units = conversion.width != 0 ? conversion.width :
                    terminated ? input.size() - cursor : 1;
                if (terminated) ++units;
                if (secure) {
                    // scanf_s reads an unsigned int from an eight-byte ARM64
                    // argument slot; the upper bytes are not part of its value.
                    const auto capacity = static_cast<std::uint32_t>(next_argument());
                    if (capacity == 0 || capacity > maximum_scan_units) {
                        errno = EINVAL;
                        return EOF;
                    }
                    units = static_cast<std::size_t>(capacity);
                    if (!writable(memory, destination, units * unit_bytes))
                        throw std::runtime_error("invalid guest scanf output buffer");
                    slots[slot_count + 1] = capacity;
                } else if constexpr (sizeof(Character) == 2) {
                    if (!wide_output && terminated) units *= MB_LEN_MAX;
                }
                bytes = units * unit_bytes;
            } else {
                bytes = number_size(conversion);
            }
            storage.resize((bytes + 7) / 8);
            auto* output = reinterpret_cast<std::uint8_t*>(storage.data());
            if (!text || secure || !terminated) {
                if (!writable(memory, destination, bytes))
                    throw std::runtime_error("invalid guest scanf output buffer");
                for (std::size_t index = 0; index < bytes; ++index)
                    output[index] = memory.read8(destination + index);
            }
            slots[slot_count++] = reinterpret_cast<std::uintptr_t>(output);
            if (secure && text) ++slot_count;
        }
        slots[slot_count] = reinterpret_cast<std::uintptr_t>(&consumed);
        const auto arguments = reinterpret_cast<va_list>(slots.data());
        const auto result = [&] {
            if constexpr (sizeof(Character) == 2) {
                return __stdio_common_vswscanf(state.x[0], input.c_str() + cursor,
                    input.size() - cursor, fragment.c_str(), locale, arguments);
            } else {
                return __stdio_common_vsscanf(state.x[0], input.c_str() + cursor,
                    input.size() - cursor, fragment.c_str(), locale, arguments);
            }
        }();
        if (destination != 0 && (consumed >= 0 || secure && text)) {
            auto* output = reinterpret_cast<std::uint8_t*>(storage.data());
            auto written = bytes;
            if (text && terminated && !secure) {
                written = 0;
                while (written < bytes) {
                    const auto zero = output[written] == 0 &&
                        (unit_bytes == 1 || output[written + 1] == 0);
                    written += unit_bytes;
                    if (zero) break;
                }
            }
            if (!text && conversion.type == 'n' && consumed >= 0) {
                std::uint64_t count = 0;
                std::memcpy(&count, output, bytes);
                count += cursor;
                std::memcpy(output, &count, bytes);
            }
            if (!writable(memory, destination, written))
                throw std::runtime_error("guest scanf result exceeds writable output memory");
            for (std::size_t index = 0; index < written; ++index)
                memory.write8(destination + index, output[index]);
        }
        if (consumed < 0)
            return assignments == 0 && result == EOF ? EOF : assignments;
        if (static_cast<std::uint64_t>(consumed) > input.size() - cursor)
            throw std::runtime_error("invalid host scanf consumed count");
        cursor += static_cast<std::size_t>(consumed);
        if (!final_literals && !conversion.suppressed && conversion.type != 'n')
            ++assignments;
    }
    return assignments;
}

} // namespace

int scan_guest_string(const CpuState& state, GuestMemory& memory,
                      bool wide, _locale_t locale) {
    if (state.x[1] == 0 || state.x[3] == 0) {
        errno = EINVAL;
        return EOF;
    }
    return wide ? scan<wchar_t>(state, memory, locale)
                : scan<char>(state, memory, locale);
}

} // namespace transmute
