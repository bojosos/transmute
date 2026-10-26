#define _CRT_SECURE_NO_WARNINGS
#include "alchemy/darwin_locale.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <locale.h>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace alchemy {
namespace {

constexpr std::uint32_t einval = 22, erange = 34, eilseq = 92, enoent = 2;
constexpr std::string_view library = "/usr/lib/libSystem.B.dylib";
struct LocaleError { std::uint32_t value; };
std::uint64_t checked(std::uint64_t address, std::uint64_t offset) {
    if (offset > UINT64_MAX - address) throw LocaleError{14};
    return address + offset;
}
std::string string(const transmute::GuestMemory& memory, std::uint64_t address) {
    bool terminated = false;
    const auto bytes = memory.read_string(address, 1, 1024 * 1024, &terminated);
    if (!terminated) throw std::runtime_error("Darwin locale string exceeds limit");
    return std::string(bytes.begin(), bytes.end());
}
void write_string(transmute::GuestMemory& memory, std::uint64_t address, std::string_view text) {
    memory.write_bytes(address, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    memory.write8(checked(address, text.size()), 0);
}
std::vector<std::uint32_t> wide_string(const transmute::GuestMemory& memory, std::uint64_t address) {
    std::vector<std::uint32_t> value;
    for (unsigned at = 0; at < 1024 * 1024; ++at) {
        const auto character = memory.read32(checked(address, at * 4ULL));
        if (!character) return value;
        value.push_back(character);
    }
    throw std::runtime_error("Darwin wide string exceeds limit");
}
std::uint32_t character_flags(std::uint32_t value) {
    if (value > 127) return 0;
    std::uint32_t flags = 0;
    const bool upper = value >= 'A' && value <= 'Z', lower = value >= 'a' && value <= 'z';
    const bool digit = value >= '0' && value <= '9', print = value >= 32 && value < 127;
    if (upper || lower) flags |= 0x100;
    if (value < 32 || value == 127) flags |= 0x200;
    if (digit) flags |= 0x400;
    if (print && value != 32) flags |= 0x800;
    if (lower) flags |= 0x1000;
    if (print && value != 32 && !upper && !lower && !digit) flags |= 0x2000;
    if (value == 32 || (value >= 9 && value <= 13)) flags |= 0x4000;
    if (upper) flags |= 0x8000;
    if (digit || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F')) flags |= 0x10000;
    if (value == 32 || value == 9) flags |= 0x20000;
    if (print) flags |= 0x40000;
    return flags;
}

class LocaleState {
public:
    explicit LocaleState(ProcessContext& value) : context(value) {
#ifdef _WIN32
        host_locale = _create_locale(LC_ALL, "C");
        if (!host_locale) throw std::bad_alloc();
#endif
    }
    ~LocaleState() {
#ifdef _WIN32
        _free_locale(host_locale);
#endif
    }
    ProcessContext& context;
#ifdef _WIN32
    _locale_t host_locale{};
#endif
    std::mutex mutex;
    std::map<std::uint64_t, bool> locales;
    std::map<std::uint64_t, std::uint64_t> current_locales;
    std::uint64_t locale_name{}, conventions{};
    std::array<std::uint64_t, 7> environment_keys{};
    std::uint64_t language_key{};
    std::array<std::uint64_t, 58> language_info{};
    std::string environment(std::uint64_t key) {
        const auto function = context.resolve(library, "_getenv");
        if (!function) throw std::runtime_error("Darwin locale requires getenv service");
        const std::array arguments{key};
        const auto value = context.calling_engine().invoke_callback(function->address, arguments);
        return value ? string(context.calling_engine(), value) : std::string{};
    }
    void select_environment(unsigned category) {
        const auto supported = [](std::string_view name) { if (name != "C" && name != "POSIX") throw LocaleError{enoent}; };
        const auto all = environment(environment_keys[0]);
        if (!all.empty()) { supported(all); return; }
        const auto select = [&](unsigned current) {
            auto selected = environment(environment_keys[current]);
            if (selected.empty()) selected = environment(language_key);
            if (selected.empty()) selected = "C";
            supported(selected);
        };
        // Validate every category before accepting LC_ALL. C and POSIX share
        // the canonical C state, so no partial category mutation is necessary.
        if (category == 0) for (unsigned current = 1; current < environment_keys.size(); ++current) select(current);
        else select(category);
    }
    void check_locale(std::uint64_t handle) {
        if (handle == 0 || handle == UINT64_MAX) return;
        std::lock_guard lock(mutex);
        if (!locales.contains(handle)) throw LocaleError{einval};
    }
    std::uint64_t new_locale() {
        const auto address = context.allocate_guest(8);
        context.calling_engine().write64(address, 0x434c4f43414c4521ULL);
        std::lock_guard lock(mutex);
        locales.emplace(address, true);
        return address;
    }
};

using Operation = std::function<std::uint64_t(transmute::CpuState&, transmute::GuestMemory&)>;
void install(ProcessContext& context, std::string symbol, Operation operation, std::uint64_t failure = UINT64_MAX) {
    static_cast<void>(context.install_function(std::move(symbol), [&context, operation = std::move(operation), failure](auto& state, auto& memory) {
        try { state.x[0] = operation(state, memory); }
        catch (const LocaleError& error) { context.set_errno(error.value); state.x[0] = failure; }
        catch (const std::bad_alloc&) { context.set_errno(12); state.x[0] = failure; }
    }));
}

} // namespace

std::function<bool(std::uint64_t)> install_darwin_locale(ProcessContext& context) {
    auto state = std::make_shared<LocaleState>(context);
    auto& memory = context.calling_engine();
    const auto empty = context.allocate_guest(1), point = context.allocate_guest(2);
    write_string(memory, empty, ""); write_string(memory, point, ".");
    state->locale_name = context.allocate_guest(2); write_string(memory, state->locale_name, "C");
    constexpr std::array<std::string_view, 7> category_keys{"LC_ALL", "LC_COLLATE", "LC_CTYPE", "LC_MONETARY", "LC_NUMERIC", "LC_TIME", "LC_MESSAGES"};
    for (std::size_t index = 0; index < category_keys.size(); ++index) {
        state->environment_keys[index] = context.allocate_guest(category_keys[index].size() + 1);
        write_string(memory, state->environment_keys[index], category_keys[index]);
    }
    state->language_key = context.allocate_guest(5); write_string(memory, state->language_key, "LANG");
    state->conventions = context.allocate_guest(96);
    memory.write64(state->conventions, point);
    for (unsigned at = 1; at < 10; ++at) memory.write64(state->conventions + at * 8, empty);
    memory.fill_bytes(state->conventions + 80, 127, 14);
    // Darwin nl_item numbering is independent of the host CRT. C/POSIX data
    // lives in guest storage and remains valid for the process lifetime.
    state->language_info.fill(empty);
    const auto language_value = [&](unsigned item, std::string_view value) {
        const auto address = context.allocate_guest(value.size() + 1);
        write_string(memory, address, value); state->language_info[item] = address;
    };
    language_value(0, "US-ASCII"); language_value(1, "%a %b %e %H:%M:%S %Y");
    language_value(2, "%m/%d/%y"); language_value(3, "%H:%M:%S");
    language_value(4, "%I:%M:%S %p"); language_value(5, "AM"); language_value(6, "PM");
    constexpr std::array<std::string_view, 7> days{"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    constexpr std::array<std::string_view, 12> months{"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    for (unsigned at = 0; at < days.size(); ++at) { language_value(7 + at, days[at]); language_value(14 + at, days[at].substr(0, 3)); }
    for (unsigned at = 0; at < months.size(); ++at) { language_value(21 + at, months[at]); language_value(33 + at, months[at].substr(0, 3)); }
    state->language_info[50] = point;
    language_value(52, "^[yY]"); language_value(53, "^[nN]");
    language_value(54, "yes"); language_value(55, "no"); language_value(57, "md");
    for (const auto localized : {false, true}) install(context, localized ? "_nl_langinfo_l" : "_nl_langinfo", [state, empty, localized](auto& s, auto&) {
        if (localized) state->check_locale(s.x[1]);
        // POSIX specifies an empty string for an unrecognized item.
        return s.x[0] < state->language_info.size() ? state->language_info[static_cast<std::size_t>(s.x[0])] : empty;
    }, 0);
    const auto rune = context.allocate_guest(3208);
    write_string(memory, rune, "RuneMagA"); write_string(memory, rune + 8, "NONE");
    memory.write32(rune + 56, UINT32_MAX);
    for (unsigned value = 0; value < 256; ++value) {
        memory.write32(rune + 60 + value * 4, character_flags(value));
        memory.write32(rune + 1084 + value * 4, value >= 'A' && value <= 'Z' ? value + 32 : value);
        memory.write32(rune + 2108 + value * 4, value >= 'a' && value <= 'z' ? value - 32 : value);
    }
    context.install_export(std::string(library), "__DefaultRuneLocale", {rune, ExportKind::data});
    install(context, "_setlocale", [state](auto& s, auto& m) {
        if (s.x[0] > 6) throw LocaleError{einval};
        if (s.x[1]) {
            const auto name = string(m, s.x[1]);
            if (name.empty()) state->select_environment(static_cast<unsigned>(s.x[0]));
            else if (name != "C" && name != "POSIX") throw LocaleError{enoent};
        }
        return state->locale_name;
    }, 0);
    install(context, "_localeconv", [state](auto&, auto&) { return state->conventions; });
    install(context, "_localeconv_l", [state](auto& s, auto&) { state->check_locale(s.x[0]); return state->conventions; });
    install(context, "___mb_cur_max", [](auto&, auto&) { return std::uint64_t{1}; });
    install(context, "___mb_cur_max_l", [state](auto& s, auto&) { state->check_locale(s.x[0]); return std::uint64_t{1}; });
    install(context, "_newlocale", [state](auto& s, auto& m) {
        // Darwin masks use one bit per category, starting at COLLATE bit zero.
        // They are independent of setlocale's category numbering.
        if (s.x[0] & ~63ULL) throw LocaleError{einval};
        const auto name = string(m, s.x[1]);
        if (name != "C" && name != "POSIX") throw LocaleError{enoent};
        state->check_locale(s.x[2]);
        return state->new_locale();
    }, 0);
    install(context, "_duplocale", [state](auto& s, auto&) { state->check_locale(s.x[0]); return state->new_locale(); }, 0);
    install(context, "_freelocale", [state](auto& s, auto&) {
        if (s.x[0] == 0 || s.x[0] == UINT64_MAX) throw LocaleError{einval};
        { std::lock_guard lock(state->mutex); if (!state->locales.erase(s.x[0])) throw LocaleError{einval}; }
        state->context.release_guest(s.x[0]);
        return std::uint64_t{0};
    });
    install(context, "_uselocale", [state](auto& s, auto&) {
        state->check_locale(s.x[0]);
        const auto identity = state->context.current_thread()->identity;
        std::lock_guard lock(state->mutex);
        const auto found = state->current_locales.find(identity);
        const auto previous = found == state->current_locales.end() ? UINT64_MAX : found->second;
        if (s.x[0]) state->current_locales.insert_or_assign(identity, s.x[0]);
        return previous;
    }, 0);
    const std::array<std::pair<const char*, std::uint32_t>, 12> classifications{{
        {"alnum", 0x500}, {"alpha", 0x100}, {"blank", 0x20000}, {"cntrl", 0x200},
        {"digit", 0x400}, {"graph", 0x800}, {"lower", 0x1000}, {"print", 0x40000},
        {"punct", 0x2000}, {"space", 0x4000}, {"upper", 0x8000}, {"xdigit", 0x10000}}};
    for (const auto& [name, mask] : classifications) for (const auto wide : {false, true}) for (const auto localized : {false, true}) {
        install(context, std::string(wide ? "_isw" : "_is") + name + (localized ? "_l" : ""), [state, localized, mask](auto& s, auto&) {
            if (localized) state->check_locale(s.x[1]);
            return static_cast<std::uint64_t>((character_flags(static_cast<std::uint32_t>(s.x[0])) & mask) != 0);
        });
    }
    for (const auto upper : {false, true}) for (const auto wide : {false, true}) for (const auto localized : {false, true})
        install(context, std::string(wide ? "_tow" : "_to") + (upper ? "upper" : "lower") + (localized ? "_l" : ""), [state, upper, localized](auto& s, auto&) {
            if (localized) state->check_locale(s.x[1]);
            const auto value = static_cast<std::uint32_t>(s.x[0]);
            return static_cast<std::uint64_t>(upper ? (value >= 'a' && value <= 'z' ? value - 32 : value) : (value >= 'A' && value <= 'Z' ? value + 32 : value));
        });
    install(context, "_wctype", [classifications](auto& s, auto& m) {
        const auto name = string(m, s.x[0]);
        for (const auto& [candidate, mask] : classifications) if (name == candidate) return static_cast<std::uint64_t>(mask);
        return std::uint64_t{0};
    });
    for (const auto localized : {false, true}) install(context, localized ? "_iswctype_l" : "_iswctype", [state, localized](auto& s, auto&) {
        if (localized) state->check_locale(s.x[2]);
        return static_cast<std::uint64_t>((character_flags(static_cast<std::uint32_t>(s.x[0])) & s.x[1]) != 0);
    });
    install(context, "_isascii", [](auto& s, auto&) { return static_cast<std::uint64_t>(s.x[0] <= 127); });
    install(context, "___maskrune", [](auto& s, auto&) { return static_cast<std::uint64_t>(character_flags(static_cast<std::uint32_t>(s.x[0]))) & s.x[1]; });
    install(context, "___tolower", [](auto& s, auto&) { const auto value = static_cast<std::uint32_t>(s.x[0]); return static_cast<std::uint64_t>(value >= 'A' && value <= 'Z' ? value + 32 : value); });
    install(context, "___toupper", [](auto& s, auto&) { const auto value = static_cast<std::uint32_t>(s.x[0]); return static_cast<std::uint64_t>(value >= 'a' && value <= 'z' ? value - 32 : value); });
    install(context, "_toascii", [](auto& s, auto&) { return s.x[0] & 127; });
    for (const auto wide : {false, true}) for (const auto localized : {false, true}) {
        install(context, std::string(wide ? "_wcscoll" : "_strcoll") + (localized ? "_l" : ""), [state, wide, localized](auto& s, auto& m) {
            if (localized) state->check_locale(s.x[2]);
            if (wide) { const auto a = wide_string(m, s.x[0]), b = wide_string(m, s.x[1]); return static_cast<std::uint64_t>(static_cast<std::int64_t>((a > b) - (a < b))); }
            const auto a = string(m, s.x[0]), b = string(m, s.x[1]);
            return static_cast<std::uint64_t>(static_cast<std::int64_t>((a > b) - (a < b)));
        });
        install(context, std::string(wide ? "_wcsxfrm" : "_strxfrm") + (localized ? "_l" : ""), [state, wide, localized](auto& s, auto& m) {
            if (localized) state->check_locale(s.x[3]);
            if (wide) { const auto value = wide_string(m, s.x[1]); if (s.x[2] > value.size()) { for (std::size_t at = 0; at < value.size(); ++at) m.write32(s.x[0] + at * 4, value[at]); m.write32(s.x[0] + value.size() * 4, 0); } return static_cast<std::uint64_t>(value.size()); }
            const auto value = string(m, s.x[1]); if (s.x[2] > value.size()) write_string(m, s.x[0], value); return static_cast<std::uint64_t>(value.size());
        });
    }
    install(context, "_wcslen", [](auto& s, auto& m) { return static_cast<std::uint64_t>(wide_string(m, s.x[0]).size()); });
    install(context, "_wmemchr", [](auto& s, auto& m) { for (std::uint64_t at = 0; at < s.x[2]; ++at) if (m.read32(checked(s.x[0], at * 4)) == static_cast<std::uint32_t>(s.x[1])) return checked(s.x[0], at * 4); return std::uint64_t{0}; });
    install(context, "_wmemcmp", [](auto& s, auto& m) { for (std::uint64_t at = 0; at < s.x[2]; ++at) { const auto a = m.read32(checked(s.x[0], at * 4)), b = m.read32(checked(s.x[1], at * 4)); if (a != b) return static_cast<std::uint64_t>(static_cast<std::int64_t>((a > b) - (a < b))); } return std::uint64_t{0}; });
    for (const auto move : {false, true}) install(context, move ? "_wmemmove" : "_wmemcpy", [move](auto& s, auto& m) { if (s.x[2] > UINT64_MAX / 4) throw LocaleError{einval}; m.copy_bytes(s.x[0], s.x[1], s.x[2] * 4, move); return s.x[0]; });
    install(context, "_wmemset", [](auto& s, auto& m) { if (s.x[2] > 1024 * 1024) throw LocaleError{einval}; for (std::uint64_t at = 0; at < s.x[2]; ++at) m.write32(checked(s.x[0], at * 4), static_cast<std::uint32_t>(s.x[1])); return s.x[0]; });
    for (const auto localized : {false, true}) {
        install(context, localized ? "_btowc_l" : "_btowc", [state, localized](auto& s, auto&) { if (localized) state->check_locale(s.x[1]); return s.x[0] <= 127 ? s.x[0] : UINT32_MAX; });
        install(context, localized ? "_wctob_l" : "_wctob", [state, localized](auto& s, auto&) { if (localized) state->check_locale(s.x[1]); return s.x[0] <= 127 ? s.x[0] : UINT32_MAX; });
        install(context, localized ? "_mbrtowc_l" : "_mbrtowc", [state, localized](auto& s, auto& m) {
            if (localized) state->check_locale(s.x[4]);
            if (!s.x[1]) { if (s.x[3]) m.fill_bytes(s.x[3], 0, 128); return std::uint64_t{0}; }
            if (!s.x[2]) return UINT64_MAX - 1;
            const auto value = m.read8(s.x[1]);
            if (value > 127) throw LocaleError{eilseq};
            if (s.x[0]) m.write32(s.x[0], value);
            if (s.x[3]) m.fill_bytes(s.x[3], 0, 128);
            return static_cast<std::uint64_t>(value != 0);
        });
        install(context, localized ? "_mbrlen_l" : "_mbrlen", [state, localized](auto& s, auto& m) {
            if (localized) state->check_locale(s.x[3]);
            if (!s.x[0]) { if (s.x[2]) m.fill_bytes(s.x[2], 0, 128); return std::uint64_t{0}; }
            if (!s.x[1]) return UINT64_MAX - 1;
            const auto value = m.read8(s.x[0]); if (value > 127) throw LocaleError{eilseq};
            if (s.x[2]) m.fill_bytes(s.x[2], 0, 128);
            return static_cast<std::uint64_t>(value != 0);
        });
        install(context, localized ? "_mbtowc_l" : "_mbtowc", [state, localized](auto& s, auto& m) { if (localized) state->check_locale(s.x[3]); if (!s.x[1]) return std::uint64_t{0}; if (!s.x[2]) throw LocaleError{eilseq}; const auto value = m.read8(s.x[1]); if (value > 127) throw LocaleError{eilseq}; if (s.x[0]) m.write32(s.x[0], value); return static_cast<std::uint64_t>(value != 0); });
        // wctomb(s, wc) and mblen(s, n): the stateless forms (the C locale's ASCII encoding has no
        // shift state, so a NULL s answers 0).
        install(context, localized ? "_wctomb_l" : "_wctomb", [state, localized](auto& s, auto& m) { if (localized) state->check_locale(s.x[2]); if (!s.x[0]) return std::uint64_t{0}; if (static_cast<std::uint32_t>(s.x[1]) > 127) throw LocaleError{eilseq}; m.write8(s.x[0], static_cast<std::uint8_t>(s.x[1])); return std::uint64_t{1}; });
        install(context, localized ? "_mblen_l" : "_mblen", [state, localized](auto& s, auto& m) { if (localized) state->check_locale(s.x[2]); if (!s.x[0]) return std::uint64_t{0}; if (!s.x[1]) throw LocaleError{eilseq}; const auto value = m.read8(s.x[0]); if (value > 127) throw LocaleError{eilseq}; return static_cast<std::uint64_t>(value != 0); });
        install(context, localized ? "_wcrtomb_l" : "_wcrtomb", [state, localized](auto& s, auto& m) { if (localized) state->check_locale(s.x[3]); if (!s.x[0]) { if (s.x[2]) m.fill_bytes(s.x[2], 0, 128); return std::uint64_t{1}; } if (s.x[1] > 127) throw LocaleError{eilseq}; m.write8(s.x[0], static_cast<std::uint8_t>(s.x[1])); if (s.x[2]) m.fill_bytes(s.x[2], 0, 128); return std::uint64_t{1}; });
        for (const auto bounded : {false, true}) {
            install(context, std::string(bounded ? "_mbsnrtowcs" : "_mbsrtowcs") + (localized ? "_l" : ""), [state, localized, bounded](auto& s, auto& m) {
                if (localized) state->check_locale(s.x[bounded ? 5 : 4]);
                const auto source = m.read64(s.x[1]);
                const auto maximum = bounded ? s.x[2] : 1024 * 1024;
                const auto capacity = s.x[0] ? s.x[bounded ? 3 : 2] : UINT64_MAX;
                std::uint64_t count = 0;
                for (; count < maximum && count < capacity; ++count) {
                    const auto value = m.read8(checked(source, count));
                    if (value > 127) throw LocaleError{eilseq};
                    if (s.x[0]) m.write32(checked(s.x[0], count * 4), value);
                    if (!value) { if (s.x[0]) m.write64(s.x[1], 0); return count; }
                }
                if (s.x[0]) m.write64(s.x[1], checked(source, count));
                return count;
            });
            install(context, std::string(bounded ? "_wcsnrtombs" : "_wcsrtombs") + (localized ? "_l" : ""), [state, localized, bounded](auto& s, auto& m) {
                if (localized) state->check_locale(s.x[bounded ? 5 : 4]);
                const auto source = m.read64(s.x[1]);
                const auto maximum = bounded ? s.x[2] : 1024 * 1024;
                const auto capacity = s.x[0] ? s.x[bounded ? 3 : 2] : UINT64_MAX;
                std::uint64_t count = 0;
                for (; count < maximum && count < capacity; ++count) {
                    const auto value = m.read32(checked(source, count * 4));
                    if (value > 127) throw LocaleError{eilseq};
                    if (s.x[0]) m.write8(checked(s.x[0], count), static_cast<std::uint8_t>(value));
                    if (!value) { if (s.x[0]) m.write64(s.x[1], 0); return count; }
                }
                if (s.x[0]) m.write64(s.x[1], checked(source, count * 4));
                return count;
            });
        }
    }
    install(context, "_mbsinit", [](auto& s, auto& m) { if (!s.x[0]) return std::uint64_t{1}; for (unsigned at = 0; at < 128; ++at) if (m.read8(s.x[0] + at)) return std::uint64_t{0}; return std::uint64_t{1}; });
    for (const auto floating : {0, 1, 2}) for (const auto wide : {false, true}) for (const auto localized : {false, true}) {
        const auto name = std::string(wide ? "_wcs" : "_str") + (floating == 0 ? "tof" : (floating == 1 ? "tod" : "told")) + (localized ? "_l" : "");
        install(context, name, [state, floating, wide, localized](auto& s, auto& m) {
            if (localized) state->check_locale(s.x[2]);
            std::string input;
            if (wide) { for (const auto value : wide_string(m, s.x[0])) { if (value > 127) throw LocaleError{eilseq}; input += static_cast<char>(value); } }
            else input = string(m, s.x[0]);
            char* end = nullptr;
            errno = 0;
#ifdef _WIN32
            const auto value = floating == 0 ? static_cast<double>(_strtof_l(input.c_str(), &end, state->host_locale)) : _strtod_l(input.c_str(), &end, state->host_locale);
#else
            const auto value = floating == 0 ? static_cast<double>(std::strtof(input.c_str(), &end)) : std::strtod(input.c_str(), &end);
#endif
            if (s.x[1]) m.write64(s.x[1], checked(s.x[0], static_cast<std::uint64_t>(end - input.c_str()) * (wide ? 4 : 1)));
            if (errno == ERANGE) state->context.set_errno(erange);
            s.v[0] = floating == 0 ? std::array<std::uint64_t, 2>{std::bit_cast<std::uint32_t>(static_cast<float>(value)), 0} : std::array<std::uint64_t, 2>{std::bit_cast<std::uint64_t>(value), 0};
            return s.x[0];
        });
    }
    for (const auto unsigned_value : {false, true}) for (const auto wide : {false, true}) for (const auto localized : {false, true}) {
        // strtol, strtoul, strtoll and strtoull are DarwinServices' (services.cpp).
        if (!wide && !localized) continue;
        for (const auto long_long : {false, true}) install(context, std::string(wide ? "_wcs" : "_str") + (unsigned_value ? (long_long ? "toull" : "toul") : (long_long ? "toll" : "tol")) + (localized ? "_l" : ""), [state, unsigned_value, wide, localized](auto& s, auto& m) {
            if (localized) state->check_locale(s.x[3]);
            if (s.x[2] != 0 && (s.x[2] < 2 || s.x[2] > 36)) throw LocaleError{einval};
            std::string input;
            if (wide) { for (const auto value : wide_string(m, s.x[0])) { if (value > 127) throw LocaleError{eilseq}; input += static_cast<char>(value); } }
            else input = string(m, s.x[0]);
            char* end = nullptr;
            errno = 0;
            const auto value = unsigned_value ? std::strtoull(input.c_str(), &end, static_cast<int>(s.x[2])) : static_cast<unsigned long long>(std::strtoll(input.c_str(), &end, static_cast<int>(s.x[2])));
            if (s.x[1]) m.write64(s.x[1], checked(s.x[0], static_cast<std::uint64_t>(end - input.c_str()) * (wide ? 4 : 1)));
            if (errno == ERANGE) state->context.set_errno(erange);
            return static_cast<std::uint64_t>(value);
        });
    }
    // Catalog lookup failure is a valid API result. No Apple message catalogs are supplied.
    install(context, "_catopen", [](auto&, auto&) -> std::uint64_t { throw LocaleError{enoent}; });
    install(context, "_catgets", [&context](auto& s, auto&) { context.set_errno(9); return s.x[3]; });
    install(context, "_catclose", [](auto&, auto&) -> std::uint64_t { throw LocaleError{9}; });
    return [state](std::uint64_t handle) {
        try { state->check_locale(handle); return true; }
        catch (const LocaleError&) { return false; }
    };
}

} // namespace alchemy
