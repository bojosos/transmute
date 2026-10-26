#define _CRT_SECURE_NO_WARNINGS
#include "alchemy/darwin_native.hpp"
#include "alchemy/process_context.hpp"
#include "alchemy/engine_adapter.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace alchemy {
namespace {
constexpr std::string_view library = "/usr/lib/libSystem.B.dylib";
constexpr std::size_t limit = 1024 * 1024;
struct NativeError { std::uint32_t value; };

bool accessible(const transmute::GuestMemory& memory, std::uint64_t address,
                std::uint64_t size, bool writable) {
    if (!address || size > UINT64_MAX - address) return false;
    while (size) {
        const auto region = memory.query_region(address);
        if (!region || address < region->base || address - region->base >= region->size ||
            !region->readable ||
            (writable && !region->writable)) return false;
        const auto count = std::min(size, region->size - (address - region->base));
        if (!count) return false;
        address += count; size -= count;
    }
    return true;
}

std::string text(const transmute::GuestMemory& memory, std::uint64_t address) {
    if (!address) throw NativeError{14};
    bool terminated{};
    const auto value = memory.read_string(address, 1, limit, &terminated);
    if (!terminated) throw std::runtime_error("Darwin native string exceeds 1 MiB");
    return {value.begin(), value.end()};
}

std::uint64_t token(transmute::GuestMemory& memory, std::uint64_t& next,
                    std::uint64_t input, std::uint64_t separators) {
    const auto delimiters = text(memory, separators);
    const auto start = input ? input : next;
    if (!start) return 0;
    const auto value = text(memory, start);
    const auto begin = value.find_first_not_of(delimiters);
    if (begin == std::string::npos) { next = 0; return 0; }
    const auto end = value.find_first_of(delimiters, begin);
    if (end == std::string::npos) next = 0;
    else {
        if (!accessible(memory, start + end, 1, true)) throw NativeError{14};
        memory.write8(start + end, 0);
        next = start + end + 1;
    }
    return start + begin;
}

bool whitespace(int character) { return character == ' ' || (character >= '\t' && character <= '\r'); }
bool digit(int character, unsigned base) {
    if (character >= '0' && character <= '9') return static_cast<unsigned>(character - '0') < base;
    if (character >= 'a' && character <= 'f') return static_cast<unsigned>(character - 'a' + 10) < base;
    if (character >= 'A' && character <= 'F') return static_cast<unsigned>(character - 'A' + 10) < base;
    return false;
}

// Stream operations use Alchemy's owned FILE objects, never host FILE pointers.
struct Reader {
    ProcessContext& process;
    std::uint64_t stream, get_address{}, put_address{}, unlock_address{};
    std::size_t consumed{};
    bool ended{};
    Reader(ProcessContext& selected, std::uint64_t file) : process(selected), stream(file) {
        const auto resolve = [&](const char* name) {
            const auto value = process.resolve(library, name);
            if (!value) throw std::runtime_error(std::string("Darwin stream scanner requires ") + name);
            return value->address;
        };
        get_address = resolve("_fgetc"); put_address = resolve("_ungetc");
        unlock_address = resolve("_funlockfile");
        const std::array arguments{stream};
        static_cast<void>(process.calling_engine().invoke_callback(resolve("_flockfile"), arguments));
    }
    ~Reader() {
        const std::array arguments{stream};
        try { static_cast<void>(process.calling_engine().invoke_callback(unlock_address, arguments)); }
        catch (...) { }
    }
    int get() {
        const std::array arguments{stream};
        const auto value = static_cast<std::uint32_t>(process.calling_engine().invoke_callback(get_address, arguments));
        if (value == UINT32_MAX) { ended = true; return -1; }
        ++consumed; return static_cast<int>(value & 255);
    }
    void put(int character) {
        if (character < 0) return;
        const std::array arguments{static_cast<std::uint64_t>(character), stream};
        if (static_cast<std::uint32_t>(process.calling_engine().invoke_callback(put_address, arguments)) == UINT32_MAX)
            throw std::runtime_error("Darwin stream scanner could not restore lookahead");
        --consumed; ended = false;
    }
    int peek() { const auto value = get(); put(value); return value; }
    void skip_space() { int value{}; do { value = get(); } while (whitespace(value)); put(value); }
};

std::string number(Reader& reader, std::size_t width, char conversion) {
    std::string value;
    const auto take = [&] { value += static_cast<char>(reader.get()); };
    const auto peek = [&] { return value.size() < width ? reader.peek() : -1; };
    if (peek() == '+' || peek() == '-') take();
    const bool floating = std::string_view("fFeEgGaA").find(conversion) != std::string_view::npos;
    unsigned base = conversion == 'o' ? 8U : conversion == 'x' || conversion == 'X' ? 16U : 10U;
    if ((floating || conversion == 'i' || base == 16) && peek() == '0') {
        take();
        if (peek() == 'x' || peek() == 'X') { take(); base = 16; }
        else if (!floating && conversion == 'i') base = 8;
    }
    if (floating && (peek() == 'i' || peek() == 'I' || peek() == 'n' || peek() == 'N')) {
        const bool infinity = peek() == 'i' || peek() == 'I';
        const auto word = infinity ? std::string_view("infinity") : std::string_view("nan");
        for (const auto expected : word) {
            const auto current = peek();
            if (current != expected && current != expected - 'a' + 'A') break;
            take();
        }
        return value;
    }
    while (digit(peek(), base)) take();
    if (floating && peek() == '.') { take(); while (digit(peek(), base)) take(); }
    if (floating && ((base == 16 && (peek() == 'p' || peek() == 'P')) ||
                     (base == 10 && (peek() == 'e' || peek() == 'E')))) {
        take(); if (peek() == '+' || peek() == '-') take();
        while (digit(peek(), 10)) take();
    }
    return value;
}

void integer(transmute::GuestMemory& memory, std::uint64_t destination,
             std::uint64_t value, std::string_view length) {
    const unsigned bytes = length == "hh" ? 1 : length == "h" ? 2 : length.empty() ? 4 : 8;
    if (!accessible(memory, destination, bytes, true)) throw NativeError{14};
    for (unsigned index = 0; index < bytes; ++index)
        memory.write8(destination + index, static_cast<std::uint8_t>(value >> (index * 8)));
}

// fscanf over a stream: `arguments` addresses the pointer arguments (the variadic stack
// area, or a va_list, which on Darwin arm64 points at the same 8-byte slots).
std::uint64_t scan(ProcessContext& process, std::uint64_t stream, std::uint64_t format_address, std::uint64_t arguments,
                   transmute::GuestMemory& memory) {
    const auto format = text(memory, format_address);
    Reader reader(process, stream);
    unsigned assigned{};
    bool converted{};
    const auto failed = [&] { return reader.ended && !converted ? std::uint64_t{UINT32_MAX} : assigned; };
    for (std::size_t at = 0; at < format.size();) {
        if (whitespace(format[at])) { ++at; reader.skip_space(); continue; }
        if (format[at] != '%') {
            const auto current = reader.get();
            if (current != static_cast<unsigned char>(format[at++])) { reader.put(current); return failed(); }
            continue;
        }
        ++at;
        if (at == format.size()) throw std::runtime_error("incomplete Darwin fscanf conversion");
        const bool suppress = format[at] == '*';
        if (suppress) ++at;
        const auto width_begin = at;
        while (at < format.size() && digit(format[at], 10)) ++at;
        std::size_t width = limit;
        const bool has_width = at != width_begin;
        if (has_width) {
            const auto result = std::from_chars(format.data() + width_begin, format.data() + at, width);
            if (result.ec != std::errc{} || !width || width > limit)
                throw std::runtime_error("Darwin fscanf field width exceeds limit");
        }
        std::string length;
        if (at < format.size() && std::string_view("hljztL").find(format[at]) != std::string_view::npos) {
            length += format[at++];
            if (at < format.size() && (length == "h" || length == "l") && length[0] == format[at]) length += format[at++];
        }
        if (at == format.size()) throw std::runtime_error("incomplete Darwin fscanf conversion");
        const auto conversion = format[at++];
        if (std::string_view("%diuoxXfFeEgGaAcsn[").find(conversion) == std::string_view::npos)
            throw std::runtime_error(std::string("unsupported Darwin fscanf conversion %") + conversion);
        // Scan set: [chars], [^chars], ']' first taken literally, a-b ranges.
        std::array<bool, 256> member{};
        bool negate = false;
        if (conversion == '[') {
            negate = at < format.size() && format[at] == '^';
            if (negate) ++at;
            const auto set_begin = at;
            for (; at < format.size() && (format[at] != ']' || at == set_begin); ++at) {
                const auto first = static_cast<unsigned char>(format[at]);
                if (at + 2 < format.size() && format[at + 1] == '-' && format[at + 2] != ']' &&
                    static_cast<unsigned char>(format[at + 2]) >= first) {
                    for (unsigned value = first; value <= static_cast<unsigned char>(format[at + 2]); ++value) member[value] = true;
                    at += 2;
                } else member[first] = true;
            }
            if (at == format.size()) throw std::runtime_error("incomplete Darwin fscanf scan set");
            ++at;
        }
        if (conversion != 'c' && conversion != 'n' && conversion != '%' && conversion != '[') reader.skip_space();
        if (conversion == '%') {
            if (suppress || has_width || !length.empty()) throw std::runtime_error("unsupported Darwin fscanf percent modifiers");
            const auto current = reader.get();
            if (current != '%') { reader.put(current); return failed(); }
            continue;
        }
        std::uint64_t destination{};
        if (!suppress) {
            if (!accessible(memory, arguments, 8, false)) throw NativeError{14};
            destination = memory.read64(arguments); arguments += 8;
        }
        if (conversion == 'n') { if (!suppress) integer(memory, destination, reader.consumed, length); continue; }
        if (conversion == 's' || conversion == 'c' || conversion == '[') {
            if (!length.empty()) throw std::runtime_error("unsupported Darwin fscanf wide characters");
            if (conversion == 'c' && !has_width) width = 1;
            std::string value;
            while (value.size() < width) {
                const auto current = reader.get();
                if (current < 0 || (conversion == 's' && whitespace(current)) ||
                    (conversion == '[' && member[static_cast<unsigned char>(current)] == negate)) { reader.put(current); break; }
                value += static_cast<char>(current);
            }
            if (value.empty() || (conversion == 'c' && value.size() != width)) return failed();
            if (!suppress) {
                const auto bytes = value.size() + (conversion != 'c' ? 1 : 0);
                if (!accessible(memory, destination, bytes, true)) throw NativeError{14};
                memory.write_bytes(destination, {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
                if (conversion != 'c') memory.write8(destination + value.size(), 0);
                ++assigned;
            }
            converted = true; continue;
        }
        const auto value = number(reader, width, conversion);
        if (value.empty()) return failed();
        char* end{};
        errno = 0;
        if (std::string_view("fFeEgGaA").find(conversion) != std::string_view::npos) {
            if (!length.empty() && length != "l" && length != "L") throw std::runtime_error("unsupported Darwin fscanf float length");
            const auto result = std::strtod(value.c_str(), &end);
            if (end != value.c_str() + value.size()) return assigned;
            if (!suppress) {
                if (!accessible(memory, destination, length.empty() ? 4 : 8, true)) throw NativeError{14};
                if (length.empty()) memory.write32(destination, std::bit_cast<std::uint32_t>(static_cast<float>(result)));
                else memory.write64(destination, std::bit_cast<std::uint64_t>(result));
                ++assigned;
            }
        } else {
            const auto base = conversion == 'i' ? 0 : conversion == 'o' ? 8 : conversion == 'x' || conversion == 'X' ? 16 : 10;
            const auto result = conversion == 'd' || conversion == 'i'
                ? static_cast<std::uint64_t>(std::strtoll(value.c_str(), &end, base)) : std::strtoull(value.c_str(), &end, base);
            if (end != value.c_str() + value.size()) return assigned;
            if (!suppress) { integer(memory, destination, result, length); ++assigned; }
        }
        if (errno == ERANGE) process.set_errno(34);
        converted = true;
    }
    return assigned;
}
} // namespace

std::string_view deferred_native_api(std::string_view) {
    // setjmp/longjmp register transfer is provided by darwin_libsystem_extra on every engine.
    return {};
}

void install_darwin_native(ProcessContext& context) {
    struct TokenState { std::mutex mutex; std::uint64_t next{}; };
    const auto tokens = std::make_shared<TokenState>();
    static_cast<void>(context.install_function("_strtok", [tokens, &context](auto& state, auto& memory) {
        try { std::lock_guard lock(tokens->mutex); state.x[0] = token(memory, tokens->next, state.x[0], state.x[1]); }
        catch (const NativeError& error) { context.set_errno(error.value); state.x[0] = 0; }
    }));
    static_cast<void>(context.install_function("_strtok_r", [&context](auto& state, auto& memory) {
        try {
            if (!accessible(memory, state.x[2], 8, true)) throw NativeError{14};
            auto next = state.x[0] ? std::uint64_t{} : memory.read64(state.x[2]);
            const auto result = token(memory, next, state.x[0], state.x[1]);
            memory.write64(state.x[2], next); state.x[0] = result;
        } catch (const NativeError& error) { context.set_errno(error.value); state.x[0] = 0; }
    }));
    // fscanf(stream, format, ...) / vfscanf(stream, format, va_list) / scanf(format, ...) /
    // vscanf(format, va_list): standard input is *__stdinp.
    const auto standard_input = [&context](transmute::GuestMemory& memory) {
        const auto cell = context.resolve(library, "___stdinp");
        if (!cell) throw std::runtime_error("Darwin scanf requires __stdinp");
        return memory.read64(cell->address);
    };
    for (const auto& [name, which] : std::array<std::pair<const char*, int>, 4>{{{"_fscanf", 0}, {"_vfscanf", 1}, {"_scanf", 2}, {"_vscanf", 3}}}) {
        const int kind = which;
        static_cast<void>(context.install_function(name, [&context, standard_input, kind](auto& state, auto& memory) {
            try {
                if (kind == 0) state.x[0] = scan(context, state.x[0], state.x[1], state.sp, memory);
                else if (kind == 1) state.x[0] = scan(context, state.x[0], state.x[1], state.x[2], memory);
                else state.x[0] = scan(context, standard_input(memory), state.x[0], kind == 2 ? state.sp : state.x[1], memory);
            } catch (const NativeError& error) { context.set_errno(error.value); state.x[0] = UINT32_MAX; }
        }));
    }
    static_cast<void>(context.install_function("_backtrace", [&context](auto& state, auto& memory) {
        const auto count = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(state.x[1]));
        if (count <= 0) { state.x[0] = 0; return; }
        if (count > 1024 || !accessible(memory, state.x[0], static_cast<std::uint64_t>(count) * 8, true)) {
            context.set_errno(count > 1024 ? 22U : 14U); state.x[0] = 0; return;
        }
        auto frame = state.x[29], address = state.x[30];
        unsigned written{};
        while (written < static_cast<unsigned>(count)) {
            const auto code = memory.query_region(address);
            if (!code || !code->executable) break;
            memory.write64(state.x[0] + written++ * 8, address);
            if ((frame & 15) || !accessible(memory, frame, 16, false)) break;
            const auto next = memory.read64(frame);
            address = memory.read64(frame + 8);
            frame = next > frame && next - frame <= 1024 * 1024 ? next : 0;
        }
        state.x[0] = written;
    }));
}
} // namespace alchemy
