#include "alchemy/darwin_numeric.hpp"
#include "alchemy/darwin_services.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr std::string_view library = "/usr/lib/libSystem.B.dylib";
void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}
struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    std::string output;
    alchemy::ProcessContext context{engine, [this](unsigned descriptor, auto bytes) {
        require(descriptor == 2, "perror output descriptor");
        const auto count = std::min<std::size_t>(3, bytes.size());
        output.append(reinterpret_cast<const char*>(bytes.data()), count);
        return alchemy::OutputResult{count, 0};
    }};
    std::unique_ptr<alchemy::DarwinServices> services;
    explicit Fixture(bool jit = false) {
        context.environment = {"LC_ALL=C", "TZ=UTC"};
        services = std::make_unique<alchemy::DarwinServices>(context);
        alchemy::install_darwin_numeric(context);   // libm, as run_program installs it
        engine.set_jit_enabled(jit); engine.set_jit_profiling(jit);
    }
    transmute::GuestMemory& memory() { return engine; }
    std::uint64_t symbol(std::string_view name) {
        const auto value = context.resolve(library, name);
        require(value.has_value(), "missing support symbol"); return value->address;
    }
    std::uint64_t call(std::string_view name, std::initializer_list<std::uint64_t> args = {}) {
        return engine.invoke_callback(symbol(name), std::span(args.begin(), args.size()));
    }
    std::uint64_t string(std::string_view value) {
        const auto address = context.allocate_guest(value.size() + 1);
        memory().write_bytes(address, {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
        memory().write8(address + value.size(), 0); return address;
    }
    std::string get(std::uint64_t address) {
        const auto value = memory().read_string(address, 1, 4096); return {value.begin(), value.end()};
    }
    std::uint64_t wide(std::initializer_list<std::uint32_t> value) {
        const auto address = context.allocate_guest((value.size() + 1) * 4);
        std::uint64_t at{};
        for (const auto character : value) memory().write32(address + at++ * 4, character);
        memory().write32(address + at * 4, 0); return address;
    }
    void slot(unsigned index, std::uint64_t value) {
        memory().write64(((engine.state().sp - 256) & ~15ULL) + index * 8, value);
    }
    // Authored ARM64 wrapper exercises the guest floating-point calling convention.
    std::uint64_t floating_bits(std::string_view name, std::uint64_t first, std::uint64_t second,
                               std::uint64_t pair, bool single) {
        std::vector<std::uint32_t> words{0xa9bf7bfd, single ? 0x1e270000U : 0x9e670000U,
            single ? 0x1e270021U : 0x9e670021U, 0xaa0103e0};
        const auto target = symbol(name);
        for (unsigned at = 0; at < 4; ++at) {
            const auto chunk = static_cast<std::uint32_t>((target >> (at * 16)) & 0xffff);
            words.push_back((at ? 0xf2800010U : 0xd2800010U) | (at << 21) | (chunk << 5));
        }
        words.push_back(0xd63f0200); words.push_back(single ? 0x1e260000U : 0x9e660000U);
        if (pair) { words.push_back(0x9e660021); words.push_back(0xf9000041); }
        words.push_back(0xa8c17bfd); words.push_back(0xd65f03c0);
        const auto code = context.allocate_guest(words.size() * 4);
        for (std::size_t at = 0; at < words.size(); ++at) memory().write32(code + at * 4, words[at]);
        engine.set_memory_access(code, true, false, true);
        const std::array<std::uint64_t, 3> arguments{first, second, pair};
        return engine.invoke_callback(code, arguments);
    }
    double floating(std::string_view name, double first, std::uint64_t second = 0, std::uint64_t pair = 0) {
        return std::bit_cast<double>(floating_bits(name, std::bit_cast<std::uint64_t>(first), second, pair, false));
    }
    float floating_single(std::string_view name, float first, std::uint64_t second = 0) {
        return std::bit_cast<float>(static_cast<std::uint32_t>(floating_bits(name, std::bit_cast<std::uint32_t>(first), second, 0, true)));
    }
};

void checked_memory_and_format() {
    Fixture f;
    f.engine.set_initial_register(9, 64);
    require(f.call("___chkstk_darwin", {0x1234}) == 0x1234, "Darwin stack probe changed x0");
    f.engine.set_initial_register(9, 2 * 1024 * 1024);
    bool stack_rejected{};
    try { static_cast<void>(f.call("___chkstk_darwin")); }
    catch (const std::runtime_error& error) { stack_rejected = std::string_view(error.what()).find("stack probe") != std::string_view::npos; }
    require(stack_rejected, "stack probe accepted an inaccessible stack range");
    f.engine.set_initial_register(9, 0);
    const auto source = f.string("abcdef"), destination = f.context.allocate_guest(64);
    require(f.call("___memcpy_chk", {destination, source, 7, 64}) == destination && f.get(destination) == "abcdef", "checked memcpy");
    bool rejected{};
    try { static_cast<void>(f.call("___memcpy_chk", {destination, source, 7, 3})); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected && f.get(destination) == "abcdef", "fortified overflow must fail before write");
    require(f.call("___memmove_chk", {destination + 1, destination, 6, 63}) == destination + 1 && f.get(destination) == "aabcdef", "checked overlapping memmove");
    f.slot(0, f.string("guest")); f.slot(1, static_cast<std::uint64_t>(-27LL));
    f.slot(2, std::bit_cast<std::uint64_t>(1.25));
    require(f.call("___sprintf_chk", {destination, 0, 64, f.string("%s:%d:%.2f")}) == 14 &&
        f.get(destination) == "guest:-27:1.25", "fortified ARM64 stack varargs");
    const auto pattern = f.context.allocate_guest(16);
    for (unsigned at = 0; at < 16; ++at) f.memory().write8(pattern + at, static_cast<std::uint8_t>(at));
    static_cast<void>(f.call("_memset_pattern16", {destination, pattern, 35}));
    for (unsigned at = 0; at < 35; ++at) require(f.memory().read8(destination + at) == at % 16, "pattern16 partial tail");
    require(f.call("_strcspn", {source, f.string("dx")}) == 3, "strcspn");
}

void bounded_string_helpers() {
    Fixture f;
    const auto destination = f.context.allocate_guest(16);
    const auto reset = [&](std::string_view value) {
        f.memory().fill_bytes(destination, 0xa5, 16);
        f.memory().write_bytes(destination, {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
        f.memory().write8(destination + value.size(), 0);
    };
    const auto guard = [&](unsigned first) {
        for (unsigned at = first; at < 16; ++at) require(f.memory().read8(destination + at) == 0xa5, "bounded string guard overwritten");
    };
    const auto reject = [&](std::string_view symbol, std::initializer_list<std::uint64_t> args) {
        std::array<std::uint8_t, 16> before{}, after{}; f.memory().read_bytes(destination, before);
        bool rejected{};
        try { static_cast<void>(f.call(symbol, args)); }
        catch (const std::runtime_error& error) { rejected = std::string_view(error.what()).find("fortified") != std::string_view::npos; }
        f.memory().read_bytes(destination, after);
        require(rejected && before == after, "fortified string failure changed destination");
    };
    reset("");
    require(f.call("___memset_chk", {destination, 0x123, 4, 4}) == destination, "memset_chk return");
    for (unsigned at = 0; at < 4; ++at) require(f.memory().read8(destination + at) == 0x23, "memset_chk unsigned byte");
    guard(4); reject("___memset_chk", {destination, 0, 5, 4});
    reset("ab");
    require(f.call("___strcat_chk", {destination, f.string("cd"), 5}) == destination && f.get(destination) == "abcd", "strcat_chk terminator bound");
    guard(5); reject("___strcat_chk", {destination, f.string("e"), 5});
    reset("ab");
    require(f.call("___strncat_chk", {destination, f.string("cdef"), 2, 5}) == destination && f.get(destination) == "abcd", "strncat_chk count excludes NUL");
    guard(5); reject("___strncat_chk", {destination, f.string("ef"), 2, 6});
    reset("");
    require(f.call("___strncpy_chk", {destination, f.string("z"), 4, 4}) == destination, "strncpy_chk return");
    require(f.memory().read8(destination) == 'z', "strncpy_chk prefix");
    for (unsigned at = 1; at < 4; ++at) require(f.memory().read8(destination + at) == 0, "strncpy_chk NUL padding");
    guard(4); reject("___strncpy_chk", {destination, f.string("abc"), 4, 3});
    // A mapped two-byte input has no terminator. Counted operations may not read past it.
    constexpr std::uint64_t raw = 0x320000000ULL;
    f.engine.map_memory(raw, 2); f.memory().write8(raw, 'K'); f.memory().write8(raw + 1, 'L');
    f.engine.set_memory_access(raw, true, false, false);
    f.memory().fill_bytes(destination, 0xa5, 16);
    require(f.call("___strncpy_chk", {destination, raw, 2, 2}) == destination &&
        f.memory().read8(destination) == 'K' && f.memory().read8(destination + 1) == 'L', "strncpy_chk exact prefix without NUL");
    guard(2);
    reset("ab");
    require(f.call("_strncat", {destination, raw, 2}) == destination && f.get(destination) == "abKL", "strncat exact nonterminated prefix");
    guard(5); require(f.call("_strncat", {destination, 0, 0}) == destination, "strncat zero count read source");
    reset("");
    require(f.call("___strlcpy_chk", {destination, f.string("abcdef"), 4, 4}) == 6 && f.get(destination) == "abc", "strlcpy_chk truncation return/NUL");
    guard(4); reject("___strlcpy_chk", {destination, f.string("x"), 5, 4});
    require(f.call("___strlcpy_chk", {0, f.string("abc"), 0, 0}) == 3, "strlcpy_chk zero capacity");
    reset("ab");
    require(f.call("___strlcat_chk", {destination, f.string("cdef"), 5, 5}) == 6 && f.get(destination) == "abcd", "strlcat_chk attempted length/NUL");
    guard(5); reject("___strlcat_chk", {destination, f.string("x"), 6, 5});
    f.memory().fill_bytes(destination, 'q', 4); f.memory().fill_bytes(destination + 4, 0xa5, 12);
    require(f.call("___strlcat_chk", {destination, f.string("xyz"), 4, 4}) == 7, "strlcat_chk nonterminated destination length");
    for (unsigned at = 0; at < 4; ++at) require(f.memory().read8(destination + at) == 'q', "strlcat_chk wrote nonterminated destination");
    guard(4);
    const auto phrase = f.string("abc123");
    require(f.call("_strspn", {phrase, f.string("cab")}) == 3 && f.call("_strpbrk", {phrase, f.string("x2")}) == phrase + 4 &&
        f.call("_strpbrk", {phrase, f.string("x")}) == 0, "string spans/search");
    require(f.call("_strcasecmp", {f.string("AbCd"), f.string("aBcD")}) == 0 &&
        f.call("_strncasecmp", {f.string("ABCx"), f.string("abCy"), 3}) == 0 && f.call("_strncasecmp", {0, 0, 0}) == 0, "C/POSIX case comparison");
    require(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(f.call("_strcasecmp", {f.string("\xc0"), f.string("\x80")}))) > 0, "case comparison uses unsigned bytes");
    const auto locale = f.call("_newlocale", {4, f.string("C"), 0});
    require(locale != 0 && f.call("_strcasecmp_l", {f.string("XYZ"), f.string("xyz"), locale}) == 0 &&
        f.call("_strncasecmp_l", {f.string("XYZa"), f.string("xyzb"), 3, locale}) == 0, "case comparison explicit C locale");
    static_cast<void>(f.call("_freelocale", {locale}));
    require(f.call("_strcasecmp_l", {phrase, phrase, locale}) == UINT64_MAX &&
        f.memory().read32(f.context.current_thread()->errno_address) == 22, "case comparison accepted a freed locale");
}

void sorting_and_wide_abi() {
    Fixture f;
    const auto values = f.context.allocate_guest(24);
    const std::array<std::int32_t, 6> input{5, -2, 8, -2, 0, 1};
    for (std::size_t at = 0; at < input.size(); ++at) f.memory().write32(values + at * 4, std::bit_cast<std::uint32_t>(input[at]));
    unsigned calls{};
    const auto comparator = f.engine.register_guest_call([&](auto& state, auto& memory) {
        const auto left = std::bit_cast<std::int32_t>(memory.read32(state.x[0]));
        const auto right = std::bit_cast<std::int32_t>(memory.read32(state.x[1]));
        const auto scratch = f.call("_malloc", {8}); static_cast<void>(f.call("_free", {scratch}));
        ++calls; state.x[0] = static_cast<std::uint32_t>((left > right) - (left < right));
    });
    static_cast<void>(f.call("_qsort", {values, input.size(), 4, comparator}));
    auto expected = input; std::sort(expected.begin(), expected.end());
    for (std::size_t at = 0; at < expected.size(); ++at)
        require(f.memory().read32(values + at * 4) == std::bit_cast<std::uint32_t>(expected[at]), "qsort signed comparator");
    require(calls > 0, "qsort comparator did not execute"); f.engine.unregister_guest_call(comparator);
    // qsort_r passes its thunk first; qsort_b calls the block's invoke function with the block first.
    const auto thunk = 0x1234ULL, block = f.context.allocate_guest(32);
    const auto descending = [&](std::uint64_t first) {
        return f.engine.register_guest_call([&, first](auto& state, auto& memory) {
            require(state.x[0] == first, "qsort_r/qsort_b first argument");
            const auto left = std::bit_cast<std::int32_t>(memory.read32(state.x[1]));
            const auto right = std::bit_cast<std::int32_t>(memory.read32(state.x[2]));
            state.x[0] = static_cast<std::uint32_t>((right > left) - (right < left));
        });
    };
    for (const auto with_block : {false, true}) {
        for (std::size_t at = 0; at < input.size(); ++at) f.memory().write32(values + at * 4, std::bit_cast<std::uint32_t>(input[at]));
        const auto callback = descending(with_block ? block : thunk);
        if (with_block) {
            f.memory().write64(block + 16, callback);
            static_cast<void>(f.call("_qsort_b", {values, input.size(), 4, block}));
        } else static_cast<void>(f.call("_qsort_r", {values, input.size(), 4, thunk, callback}));
        for (std::size_t at = 0; at < expected.size(); ++at)
            require(f.memory().read32(values + at * 4) == std::bit_cast<std::uint32_t>(expected[expected.size() - 1 - at]),
                    with_block ? "qsort_b descending" : "qsort_r descending");
        f.engine.unregister_guest_call(callback);
    }
    for (std::size_t at = 0; at < expected.size(); ++at) f.memory().write32(values + at * 4, std::bit_cast<std::uint32_t>(expected[at]));
    const auto key = f.context.allocate_guest(4);
    const auto search_comparator = f.engine.register_guest_call([&](auto& state, auto& memory) {
        require(state.x[0] == key, "bsearch comparator arguments reversed");
        const auto left = std::bit_cast<std::int32_t>(memory.read32(state.x[0]));
        const auto right = std::bit_cast<std::int32_t>(memory.read32(state.x[1]));
        const auto scratch = f.call("_malloc", {16}); static_cast<void>(f.call("_free", {scratch}));
        ++calls; state.x[0] = static_cast<std::uint32_t>((left > right) - (left < right));
    });
    f.memory().write32(key, 5);
    require(f.call("_bsearch", {key, values, input.size(), 4, search_comparator}) == values + 16, "bsearch found address/reentrant callback");
    f.memory().write32(key, 7);
    require(f.call("_bsearch", {key, values, input.size(), 4, search_comparator}) == 0 &&
        f.call("_bsearch", {0, 0, 0, 0, 0}) == 0, "bsearch absent/empty table");
    f.engine.unregister_guest_call(search_comparator);
    const auto source = f.wide({'a', 0x1f600, 'b'}), destination = f.context.allocate_guest(32);
    require(f.call("_wcscpy", {destination, source}) == destination && f.memory().read32(destination + 4) == 0x1f600, "Darwin wchar_t must be 32 bits");
    require(f.call("_wcscmp", {source, destination}) == 0 && f.call("_wcschr", {source, 0x1f600}) == source + 4, "wide comparison/search");
    static_cast<void>(f.call("_wcsncpy", {destination, source, 6}));
    require(f.memory().read32(destination + 20) == 0, "wcsncpy padding");
    require(f.call("_mbstowcs", {destination, f.string("ASCII"), 8}) == 5 && f.memory().read32(destination + 16) == 'I', "C-locale multibyte conversion");
    require(f.call("___tolower", {'A'}) == 'a' && f.call("___toupper", {'z'}) == 'Z' &&
        f.call("___maskrune", {'A', 0x100}) == 0x100, "C locale rune helpers");
    require(f.get(f.call("_nl_langinfo", {0})) == "US-ASCII" && f.get(f.call("_nl_langinfo", {7})) == "Sunday" &&
        f.get(f.call("_nl_langinfo", {50})) == "." && f.get(f.call("_nl_langinfo", {UINT64_MAX})).empty(), "Darwin nl_item numbering and invalid item");
}

void calendar_clocks_and_errors() {
    Fixture f;
    const auto seconds = f.context.allocate_guest(8), calendar = f.context.allocate_guest(56);
    f.memory().write64(seconds, UINT64_MAX);
    require(f.call("_gmtime_r", {seconds, calendar}) == calendar &&
        f.memory().read32(calendar) == 59 && f.memory().read32(calendar + 8) == 23 &&
        f.memory().read32(calendar + 20) == 69 && f.memory().read64(calendar + 40) == 0 &&
        f.get(f.memory().read64(calendar + 48)) == "UTC", "Darwin tm layout/negative epoch");
    require(f.call("_timegm", {calendar}) == UINT64_MAX, "valid pre-epoch timegm");
    f.memory().write64(seconds, 951782400);
    require(f.call("_localtime_r", {seconds, calendar}) == calendar && f.memory().read32(calendar + 12) == 29 &&
        f.call("_mktime", {calendar}) == 951782400, "UTC leap day roundtrip");
    const auto format = f.wide({'%', 'F', ' ', '%', 'T', ' ', '%', 'z', ' ', '%', 'Z'});
    const auto output = f.context.allocate_guest(256);
    require(f.call("_wcsftime", {output, 64, format, calendar}) == 29, "wcsftime count");
    const std::string expected = "2000-02-29 00:00:00 +0000 UTC";
    for (std::size_t at = 0; at < expected.size(); ++at) require(f.memory().read32(output + at * 4) == static_cast<unsigned char>(expected[at]), "wcsftime code units");
    const auto timespec = f.context.allocate_guest(16);
    require(f.call("_clock_gettime", {4, timespec}) == 0 && f.memory().read64(timespec + 8) < 1000000000, "Darwin monotonic clock");
    require(f.call("_clock_gettime", {99, timespec}) == UINT64_MAX &&
        f.memory().read32(f.context.current_thread()->errno_address) == 22, "invalid clock errno");
    require(f.call("_mach_timebase_info", {timespec}) == 0 && f.memory().read32(timespec) == 1 && f.memory().read32(timespec + 4) == 1, "Mach timebase");
    const auto before = f.call("_mach_absolute_time"); require(f.call("_mach_absolute_time") >= before, "Mach time monotonicity");
    require(f.call("___isPlatformVersionAtLeast", {1, 14, 0, 0}) == 1 && f.call("___isPlatformVersionAtLeast", {1, 11, 2, 0}) == 1 &&
        f.call("___isPlatformVersionAtLeast", {1, 14, 0, 1}) == 0 && f.call("___isPlatformVersionAtLeast", {1, 15, 0, 0}) == 0 &&
        f.call("___isPlatformVersionAtLeast", {2, 1, 0, 0}) == 0, "explicit supported Darwin profile");
    f.context.set_errno(2); static_cast<void>(f.call("_perror", {f.string("missing")}));
    require(f.output == "missing: No such file or directory\n" && f.memory().read32(f.context.current_thread()->errno_address) == 2, "perror partial output/errno");
    require(f.get(f.call("_strerror", {2})) == "No such file or directory", "Darwin error message");
    require(f.call("_strerror_r", {2, output, 2}) == 34, "strerror_r short buffer");
}

void locale_environment_selection() {
    Fixture f;
    const auto empty = f.string("");
    const auto environment = [&](std::string_view name, const char* value) {
        require(value ? f.call("_setenv", {f.string(name), f.string(value), 1}) == 0 :
            f.call("_unsetenv", {f.string(name)}) == 0, "locale test environment update");
    };
    environment("LC_CTYPE", "unsupported"); environment("LANG", "unsupported");
    require(f.get(f.call("_setlocale", {0, empty})) == "C", "LC_ALL=C must override category and LANG");
    environment("LC_ALL", nullptr); environment("LANG", "POSIX");
    environment("LC_CTYPE", "C"); environment("LC_NUMERIC", "POSIX");
    require(f.get(f.call("_setlocale", {0, empty})) == "C", "mixed C/POSIX categories must normalize together");
    environment("LC_TIME", "unsupported");
    require(f.get(f.call("_setlocale", {2, empty})) == "C", "single category must ignore other category failures");
    require(f.call("_setlocale", {0, empty}) == 0 && f.memory().read32(f.context.current_thread()->errno_address) == 2 &&
        f.get(f.call("_setlocale", {0, 0})) == "C", "LC_ALL must reject unsupported category without changing state");
    environment("LC_ALL", ""); environment("LANG", "unsupported");
    require(f.get(f.call("_setlocale", {2, empty})) == "C" && f.call("_setlocale", {4, empty}) != 0 &&
        f.call("_setlocale", {1, empty}) == 0, "empty LC_ALL/category precedence over LANG");
    environment("LC_ALL", "POSIX");
    require(f.get(f.call("_setlocale", {0, empty})) == "C", "LC_ALL=POSIX must override unsupported categories");
    const auto local = f.call("_newlocale", {4, f.string("C"), 0});
    require(local && f.call("_uselocale", {local}) == UINT64_MAX, "thread locale setup");
    require(f.get(f.call("_setlocale", {0, empty})) == "C" && f.call("_uselocale", {0}) == local,
        "process setlocale changed current thread locale ownership");
    require(f.call("_uselocale", {UINT64_MAX}) == local, "thread locale restore");
    static_cast<void>(f.call("_freelocale", {local}));
    environment("LC_ALL", "unsupported");
    require(f.call("_setlocale", {2, empty}) == 0 && f.get(f.call("_setlocale", {2, f.string("POSIX")})) == "C",
        "explicit locale must ignore environment while empty selection honors LC_ALL");
    for (const auto name : {"LC_ALL", "LC_COLLATE", "LC_CTYPE", "LC_MESSAGES", "LC_MONETARY", "LC_NUMERIC", "LC_TIME", "LANG"})
        environment(name, nullptr);
    require(f.get(f.call("_setlocale", {0, empty})) == "C", "missing locale environment must use C default");
}

void iso_calendar_formatting() {
    Fixture f;
    const auto seconds = f.context.allocate_guest(8), calendar = f.context.allocate_guest(56), output = f.context.allocate_guest(128);
    const auto format = f.wide({'%', 'G', '-', 'W', '%', 'V', '-', '%', 'u', '/', '%', 'g', '/', '%', 'C'});
    const std::array<std::pair<std::uint64_t, std::string_view>, 8> cases{{
        {0, "1970-W01-4/70/19"},
        {1419811200, "2015-W01-1/15/20"}, {1451520000, "2015-W53-4/15/20"},
        {1451606400, "2015-W53-5/15/20"}, {1451865600, "2016-W01-1/16/20"},
        {1483228800, "2016-W52-7/16/20"},
        {1546214400, "2019-W01-1/19/20"}, {1609459200, "2020-W53-5/20/20"}}};
    for (const auto& [timestamp, expected] : cases) {
        f.memory().write64(seconds, timestamp);
        require(f.call("_gmtime_r", {seconds, calendar}) == calendar, "ISO test calendar conversion");
        f.memory().fill_bytes(output, 0xa5, 128);
        require(f.call("_wcsftime", {output, 32, format, calendar}) == expected.size(), "ISO week formatting count");
        for (std::size_t at = 0; at < expected.size(); ++at)
            require(f.memory().read32(output + at * 4) == static_cast<unsigned char>(expected[at]), "ISO week-year boundary formatting");
        require(f.memory().read32(output + expected.size() * 4) == 0 && f.memory().read8(output + (expected.size() + 1) * 4) == 0xa5,
            "ISO formatting terminator/guard");
        f.memory().fill_bytes(output, 0xa5, 128);
        require(f.call("_wcsftime", {output, expected.size(), format, calendar}) == 0 && f.memory().read8(output) == 0xa5,
            "ISO formatting short buffer must not write");
    }
}

void floating_guest_abi(bool jit) {
    Fixture f(jit);
    const auto cell = f.context.allocate_guest(8);
    require(f.floating("_frexp", 6.0, cell) == 0.75 && f.memory().read32(cell) == 3, "frexp guest FP/pointer ABI");
    require(f.floating("_ldexp", 0.75, 3) == 6.0, "ldexp guest mixed FP/integer ABI");
    require(f.floating("_modf", -3.25, cell) == -0.25 && std::bit_cast<double>(f.memory().read64(cell)) == -3.0, "modf guest result ABI");
    require(f.floating("_hypot", 3.0, std::bit_cast<std::uint64_t>(4.0)) == 5.0, "hypot guest FP arguments");
    require(f.floating("_log2", 8.0) == 3.0 && f.floating("_exp2", 2.0) == 4.0, "native extension math helpers");
    require(f.floating("_copysign", 1.0, std::bit_cast<std::uint64_t>(-0.0)) == -1.0, "copysign negative zero");
    require(f.floating("___sincos_stret", 0.0, 0, cell) == 0.0 && std::bit_cast<double>(f.memory().read64(cell)) == 1.0, "Darwin sincos homogeneous result");
    require(f.floating("_sqrtl", 9.0) == 3.0 && f.floating("_acoshl", 1.0) == 0.0 &&
        f.floating("_frexpl", 6.0, cell) == 0.75 && f.memory().read32(cell) == 3, "Apple binary64 long-double math aliases");
    require(f.floating_single("_sqrtf", 9.0f) == 3.0f && f.floating_single("_powf", 2.0f, std::bit_cast<std::uint32_t>(3.0f)) == 8.0f, "binary32 S-register math ABI");
    require(f.floating_single("_frexpf", 6.0f, cell) == 0.75f && f.memory().read32(cell) == 3 &&
        f.floating_single("_ldexpf", 0.75f, static_cast<std::uint32_t>(-2)) == 0.1875f, "binary32 mixed integer math ABI");
    f.memory().write32(cell + 4, 0x12345678);
    require(f.floating_single("_modff", -3.25f, cell) == -0.25f && std::bit_cast<float>(f.memory().read32(cell)) == -3.0f &&
        f.memory().read32(cell + 4) == 0x12345678, "modff wrote beyond float32 result");
    require(std::signbit(f.floating_single("_copysignf", 0.0f, std::bit_cast<std::uint32_t>(-1.0f))) &&
        f.floating_single("_nextafterf", 1.0f, std::bit_cast<std::uint32_t>(2.0f)) == std::nextafter(1.0f, 2.0f), "binary32 signed zero/adjacent value");
    if (jit) require(f.engine.jit_executed_instructions() > 0, "FP ABI fixture did not execute translated blocks");
}
} // namespace

int main() {
    try {
        checked_memory_and_format(); bounded_string_helpers(); sorting_and_wide_abi(); calendar_clocks_and_errors();
        locale_environment_selection(); iso_calendar_formatting();
        floating_guest_abi(false); floating_guest_abi(true);
        std::cout << "Alchemy Python support tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
