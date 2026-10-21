#include "alchemy/darwin/libsystem/misc/log_store.hpp"

#include <bit>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace alchemy::gui::completion_kit::logs {
namespace {

struct Store {
    std::mutex mutex;
    std::vector<Entry> entries;
};
Store& store() { static Store value; return value; }
constexpr std::size_t store_limit = 100000;   // oldest entries go first

struct Argument { std::uint8_t descriptor{}; std::vector<std::uint8_t> data; };

std::vector<Argument> arguments_of(std::span<const std::uint8_t> buffer) {
    std::vector<Argument> result;
    if (buffer.size() < 2) return result;
    const auto count = buffer[1];
    std::size_t at = 2;
    for (unsigned index = 0; index < count && at + 2 <= buffer.size(); ++index) {
        Argument argument;
        argument.descriptor = buffer[at];
        const auto size = buffer[at + 1];
        at += 2;
        if (at + size > buffer.size()) break;
        argument.data.assign(buffer.begin() + static_cast<std::ptrdiff_t>(at), buffer.begin() + static_cast<std::ptrdiff_t>(at + size));
        at += size;
        result.push_back(std::move(argument));
    }
    return result;
}

std::uint64_t integer_of(const Argument& argument) {
    std::uint64_t value{};
    std::memcpy(&value, argument.data.data(), std::min<std::size_t>(argument.data.size(), 8));
    return value;
}

} // namespace

void record(Entry entry) {
    auto& value = store();
    std::lock_guard lock(value.mutex);
    if (value.entries.size() >= store_limit) value.entries.erase(value.entries.begin(), value.entries.begin() + static_cast<std::ptrdiff_t>(store_limit / 10));
    value.entries.push_back(std::move(entry));
}

std::vector<Entry> entries() {
    auto& value = store();
    std::lock_guard lock(value.mutex);
    return value.entries;
}

std::string compose(std::string_view format, std::span<const std::uint8_t> buffer,
                    const std::function<std::string(std::uint64_t)>& c_string,
                    const std::function<std::string(std::uint64_t)>& describe) {
    const auto arguments = arguments_of(buffer);
    std::size_t next = 0;
    std::string out;
    for (std::size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%') { out.push_back(format[i]); continue; }
        if (i + 1 < format.size() && format[i + 1] == '%') { out.push_back('%'); ++i; continue; }
        std::size_t j = i + 1;
        if (j < format.size() && format[j] == '{') {   // {public}, {private}, {bool}, ...
            const auto close = format.find('}', j);
            if (close == std::string_view::npos) { out.append(format.substr(i)); break; }
            j = close + 1;
        }
        std::string spec = "%";
        while (j < format.size() && std::string_view("-+ #0").find(format[j]) != std::string_view::npos) spec.push_back(format[j++]);
        bool star_width = false, star_precision = false;
        if (j < format.size() && format[j] == '*') { star_width = true; ++j; }
        while (j < format.size() && format[j] >= '0' && format[j] <= '9') spec.push_back(format[j++]);
        if (j < format.size() && format[j] == '.') {
            spec.push_back(format[j++]);
            if (j < format.size() && format[j] == '*') { star_precision = true; ++j; }
            while (j < format.size() && format[j] >= '0' && format[j] <= '9') spec.push_back(format[j++]);
        }
        while (j < format.size() && std::string_view("hlqjztL").find(format[j]) != std::string_view::npos) ++j;
        if (j >= format.size()) { out.append(format.substr(i)); break; }
        const char conversion = format[j];
        i = j;
        // os_log records '*' widths and precisions as their own (count) arguments.
        int width = 0, precision = -1;
        if (star_width && next < arguments.size()) width = static_cast<int>(integer_of(arguments[next++]));
        if (star_precision && next < arguments.size()) precision = static_cast<int>(integer_of(arguments[next++]));
        if (star_width) spec += std::to_string(width);
        if (star_precision) { if (spec.back() != '.') spec.push_back('.'); spec += std::to_string(precision); }
        if (next >= arguments.size()) { out += "<decode: missing data>"; continue; }
        const auto& argument = arguments[next++];
        const bool is_private = (argument.descriptor & 0x1) != 0;
        if (argument.data.empty()) { out += is_private ? "<private>" : "<decode: missing data>"; continue; }
        char text[512];
        switch (conversion) {
        case 'd': case 'i': {
            auto value = integer_of(argument);
            if (argument.data.size() == 4) value = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
            else if (argument.data.size() == 2) value = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(value)));
            else if (argument.data.size() == 1) value = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int8_t>(value)));
            std::snprintf(text, sizeof(text), (spec + "lld").c_str(), static_cast<long long>(value));
            out += text;
            break;
        }
        case 'u': case 'o': case 'x': case 'X': case 'c': {
            const auto value = integer_of(argument);
            if (conversion == 'c') std::snprintf(text, sizeof(text), (spec + "c").c_str(), static_cast<int>(value & 0xff));
            else std::snprintf(text, sizeof(text), (spec + "ll" + conversion).c_str(), static_cast<unsigned long long>(value));
            out += text;
            break;
        }
        case 'p':
            std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(integer_of(argument)));
            out += text;
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double value{};
            if (argument.data.size() == 4) value = std::bit_cast<float>(static_cast<std::uint32_t>(integer_of(argument)));
            else value = std::bit_cast<double>(integer_of(argument));
            std::snprintf(text, sizeof(text), (spec + conversion).c_str(), value);
            out += text;
            break;
        }
        case 's': {
            const auto value = integer_of(argument);
            auto string = value ? c_string(value) : std::string("(null)");
            if (precision >= 0 && string.size() > static_cast<std::size_t>(precision)) string.resize(static_cast<std::size_t>(precision));
            out += string;
            break;
        }
        case '@': {
            const auto value = integer_of(argument);
            out += value ? describe(value) : std::string("(null)");
            break;
        }
        default:
            out += "<decode: unsupported format %";
            out.push_back(conversion);
            out.push_back('>');
            break;
        }
    }
    return out;
}

} // namespace alchemy::gui::completion_kit::logs
