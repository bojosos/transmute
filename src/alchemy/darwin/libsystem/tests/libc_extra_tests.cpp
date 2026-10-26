// libc names the core lacked (misc/libc_extra.cpp): <string.h>, <wchar.h>, <search.h>,
// <stdlib.h>, <time.h>, <ctype.h>, the BSD/System V signal calls, dprintf, paths, nftw and ether_*.
#include "alchemy/darwin/kit/export_fill/tests/export_fill_test_kit.hpp"

#include <limits>

namespace {

Address wide_text(Fixture& f, std::u32string_view value) {
    const auto address = f.process.allocate_guest((value.size() + 1) * 4);
    for (std::size_t at = 0; at < value.size(); ++at) f.engine.write32(address + at * 4, static_cast<std::uint32_t>(value[at]));
    f.engine.write32(address + value.size() * 4, 0);
    return address;
}
std::u32string read_wide(Fixture& f, Address address) {
    std::u32string value;
    for (Address at = 0;; at += 4) { const auto c = f.engine.read32(address + at); if (!c) return value; value.push_back(static_cast<char32_t>(c)); }
}

void test_strings(Fixture& f) {
    const auto buffer = f.process.allocate_guest(64);
    require(f.sys("_stpcpy", {buffer, f.text("abc")}) == buffer + 3 && f.read(buffer) == "abc", "stpcpy");
    require(f.sys("_stpncpy", {buffer, f.text("xy"), 5}) == buffer + 2 && f.byte(buffer + 4) == 0, "stpncpy pads");
    const auto line = f.text("a,b;c"), cursor = f.process.allocate_guest(8);
    f.engine.write64(cursor, line);
    const auto first = f.sys("_strsep", {cursor, f.text(",;")}), second = f.sys("_strsep", {cursor, f.text(",;")});
    const auto third = f.sys("_strsep", {cursor, f.text(",;")});
    require(f.read(first) == "a" && f.read(second) == "b" && f.read(third) == "c" && f.engine.read64(cursor) == 0 &&
            f.sys("_strsep", {cursor, f.text(",")}) == 0, "strsep");
    const auto haystack = f.text("Hello, World");
    require(f.sys("_strcasestr", {haystack, f.text("world")}) == haystack + 7 && f.sys("_strnstr", {haystack, f.text("World"), 8}) == 0 &&
            f.sys("_strnstr", {haystack, f.text("World"), 12}) == haystack + 7, "strcasestr/strnstr");
    require(f.sys("_memmem", {haystack, 12, f.text("lo,"), 3}) == haystack + 3 && f.sys("_memmem", {haystack, 12, f.text("xyz"), 3}) == 0, "memmem");
    require(f.sys("_index", {haystack, 'o'}) == haystack + 4 && f.sys("_rindex", {haystack, 'o'}) == haystack + 8, "index/rindex");
    require(f.sys("_memccpy", {buffer, haystack, ',', 12}) == buffer + 6, "memccpy stops after the byte");
    require(f.sys("_ffs", {0x10}) == 5 && f.sys("_ffs", {0}) == 0 && f.sys("_fls", {0x10}) == 5 && f.sys("_flsll", {1ULL << 40}) == 41, "ffs/fls");
    require(f.sys("_bcmp", {f.text("abc"), f.text("abd"), 2}) == 0 && f.sys("_bcmp", {f.text("abc"), f.text("abd"), 3}) != 0 &&
            f.sys("_timingsafe_bcmp", {f.text("abc"), f.text("abd"), 3}) == 1, "bcmp/timingsafe_bcmp");
    require(f.sys("_memset_s", {buffer, 4, 'z', 8}) == 84 && f.read(buffer).substr(0, 4) == "zzzz", "memset_s fills smax and reports EOVERFLOW");
    f.sys("_strmode", {0040755, buffer});
    require(f.read(buffer) == "drwxr-xr-x ", "strmode");
    f.sys("_swab", {f.text("abcd"), buffer, 4});
    require(f.read(buffer).substr(0, 4) == "badc", "swab");
}

void test_wide(Fixture& f) {
    const auto buffer = f.process.allocate_guest(128);
    f.sys("_wcscpy", {buffer, wide_text(f, U"ab")});
    f.sys("_wcscat", {buffer, wide_text(f, U"cd")});
    require(read_wide(f, buffer) == U"abcd", "wcscat");
    require(f.sys("_wcsspn", {buffer, wide_text(f, U"ba")}) == 2 && f.sys("_wcscspn", {buffer, wide_text(f, U"c")}) == 2, "wcsspn/wcscspn");
    require(read_wide(f, f.sys("_wcsdup", {buffer})) == U"abcd" && f.sys("_wcsnlen", {buffer, 3}) == 3, "wcsdup/wcsnlen");
    require(f.sys("_wcscasecmp", {wide_text(f, U"ABC"), wide_text(f, U"abc")}) == 0, "wcscasecmp");
    require(f.sys("_wcwidth", {'a'}) == 1 && f.sys("_wcwidth", {0x263a}) == UINT64_MAX && f.sys("_wcswidth", {buffer, 10}) == 4, "wcwidth");
    require(f.sys("_wcslcpy", {buffer, wide_text(f, U"hello"), 3}) == 5 && read_wide(f, buffer) == U"he", "wcslcpy");
    require(f.sys("_wcslcat", {buffer, wide_text(f, U"llo"), 16}) == 5 && read_wide(f, buffer) == U"hello", "wcslcat");
}

void test_search(Fixture& f) {
    require(f.sys("_hcreate", {8}) == 1, "hcreate");
    const auto key = f.text("alpha");
    const auto entry = f.sys("_hsearch", {key, 0x42, 1});   // ENTER
    require(entry && f.engine.read64(entry + 8) == 0x42, "hsearch ENTER");
    require(f.sys("_hsearch", {f.text("alpha"), 0, 0}) == entry && f.sys("_hsearch", {f.text("beta"), 0, 0}) == 0, "hsearch FIND");
    f.sys("_hdestroy");
    // tsearch over string keys, compared with strcmp.
    const auto strcmp = f.symbol("/usr/lib/libSystem.B.dylib", "_strcmp");
    const auto root = f.process.allocate_guest(8);
    f.engine.write64(root, 0);
    const auto b = f.text("b"), a = f.text("a"), c = f.text("c");
    const auto node_b = f.sys("_tsearch", {b, root, strcmp});
    require(node_b && f.engine.read64(node_b) == b, "tsearch adds");
    f.sys("_tsearch", {a, root, strcmp}); f.sys("_tsearch", {c, root, strcmp});
    require(f.sys("_tsearch", {f.text("b"), root, strcmp}) == node_b, "tsearch finds an existing key");
    require(f.sys("_tfind", {f.text("c"), root, strcmp}) != 0 && f.sys("_tfind", {f.text("z"), root, strcmp}) == 0, "tfind");
    require(f.sys("_tdelete", {f.text("b"), root, strcmp}) != 0 && f.sys("_tfind", {f.text("b"), root, strcmp}) == 0 &&
            f.sys("_tfind", {f.text("a"), root, strcmp}) != 0 && f.sys("_tfind", {f.text("c"), root, strcmp}) != 0, "tdelete keeps the rest");
    // lsearch appends a missing key (2-byte NUL-terminated strings, compared with strcmp), lfind finds it.
    const auto table = f.process.allocate_guest(8), count = f.process.allocate_guest(8);
    f.engine.write64(count, 0);
    const auto key_x = f.text("x");
    const auto added = f.sys("_lsearch", {key_x, table, count, 2, strcmp});
    require(added == table && f.engine.read64(count) == 1 && f.sys("_lfind", {f.text("x"), table, count, 2, strcmp}) == table &&
            f.sys("_lfind", {f.text("y"), table, count, 2, strcmp}) == 0, "lsearch/lfind");
    // insque / remque.
    const auto one = f.process.allocate_guest(16), two = f.process.allocate_guest(16);
    f.sys("_insque", {one, 0});
    f.sys("_insque", {two, one});
    require(f.engine.read64(one) == two && f.engine.read64(two + 8) == one, "insque links");
    f.sys("_remque", {two});
    require(f.engine.read64(one) == 0, "remque unlinks");
}

void test_stdlib(Fixture& f) {
    require(f.sys("_putenv", {f.text("ALCHEMY_PUTENV=yes")}) == 0 && f.read(f.sys("_getenv", {f.text("ALCHEMY_PUTENV")})) == "yes", "putenv");
    require(f.sys("_putenv", {f.text("novalue")}) == UINT64_MAX, "putenv without '=' fails");
    const auto error = f.process.allocate_guest(8);
    require(f.sys("_strtonum", {f.text("42"), 0, 100, error}) == 42 && f.engine.read64(error) == 0, "strtonum");
    require(f.sys("_strtonum", {f.text("420"), 0, 100, error}) == 0 && f.read(f.engine.read64(error)) == "too large", "strtonum too large");
    require(f.sys("_strtonum", {f.text("4x"), 0, 100, error}) == 0 && f.read(f.engine.read64(error)) == "invalid", "strtonum invalid");
    require(f.sys("_strtonum", {f.text("-5"), static_cast<Address>(-10), 10, error}) == static_cast<Address>(-5), "strtonum negative");
    require(f.sys("_reallocarray", {0, UINT64_MAX / 2, 4}) == 0, "reallocarray refuses an overflowing size");
    const auto block = f.sys("_reallocarray", {0, 4, 8});
    require(block != 0, "reallocarray allocates");
    f.sys("_free", {block});
    require(f.sys("_a64l", {f.sys("_l64a", {123456})}) == 123456, "a64l/l64a round trip");
    // mergesort and heapsort with strcmp over 2-byte NUL-terminated elements.
    const auto pairs = f.process.allocate_guest(10);
    f.engine.write_bytes(pairs, std::vector<std::uint8_t>{'d', 0, 'b', 0, 'e', 0, 'a', 0, 'c', 0});
    const auto strcmp = f.symbol("/usr/lib/libSystem.B.dylib", "_strcmp");
    require(f.sys("_mergesort", {pairs, 5, 2, strcmp}) == 0, "mergesort");
    require(guest_bytes(f, pairs, 10) == std::vector<std::uint8_t>{'a', 0, 'b', 0, 'c', 0, 'd', 0, 'e', 0}, "mergesort sorts");
    f.engine.write_bytes(pairs, std::vector<std::uint8_t>{'d', 0, 'b', 0, 'e', 0, 'a', 0, 'c', 0});
    require(f.sys("_heapsort", {pairs, 5, 2, strcmp}) == 0 && guest_bytes(f, pairs, 2) == std::vector<std::uint8_t>{'a', 0}, "heapsort");
    // radixsort orders string pointers.
    const auto strings = f.process.allocate_guest(24);
    const auto pear = f.text("pear"), apple = f.text("apple"), fig = f.text("fig");
    f.engine.write64(strings, pear); f.engine.write64(strings + 8, apple); f.engine.write64(strings + 16, fig);
    require(f.sys("_radixsort", {strings, 3, 0, 0}) == 0 && f.engine.read64(strings) == apple && f.engine.read64(strings + 8) == fig &&
            f.engine.read64(strings + 16) == pear, "radixsort");
    // getsubopt.
    const auto options = f.text("ro,size=4,bogus"), option_cursor = f.process.allocate_guest(8), value = f.process.allocate_guest(8);
    f.engine.write64(option_cursor, options);
    const auto tokens = f.process.allocate_guest(24);
    f.engine.write64(tokens, f.text("ro")); f.engine.write64(tokens + 8, f.text("size")); f.engine.write64(tokens + 16, 0);
    require(f.sys("_getsubopt", {option_cursor, tokens, value}) == 0 && f.engine.read64(value) == 0, "getsubopt ro");
    require(f.sys("_getsubopt", {option_cursor, tokens, value}) == 1 && f.read(f.engine.read64(value)) == "4", "getsubopt size=4");
    require(f.sys("_getsubopt", {option_cursor, tokens, value}) == UINT64_MAX, "getsubopt unknown");
    // mkdtemps.
    const auto directory = std::filesystem::temp_directory_path() / "alchemy-mkdtempsXXXXXX.d";
    const auto pattern = f.text(alchemy::guest_path_string(directory));
    require(f.sys("_mkdtemps", {pattern, 2}) == pattern, "mkdtemps");
    const auto made = f.read(pattern);
    require(made.ends_with(".d") && made.find("XXXXXX") == std::string::npos, "mkdtemps keeps the suffix");
    f.sys("_rmdir", {pattern});
}

void test_time_ctype_signals(Fixture& f) {
    const auto spec = f.process.allocate_guest(16);
    require(f.sys("_timespec_get", {spec, 1}) == 1 && f.engine.read64(spec) > 1'600'000'000ULL, "timespec_get");
    require(f.sys("_timespec_get", {spec, 2}) == 0, "timespec_get of an unknown base");
    require(f.sys("_settimeofday", {spec, 0}) == UINT64_MAX, "settimeofday needs root");
    require(f.sys("_posix2time", {1234}) == 1234, "posix2time");
    require(f.sys("_digittoint", {'f'}) == 15 && f.sys("_ishexnumber", {'A'}) == 1 && f.sys("_isnumber", {'x'}) == 0, "ctype extensions");
    const auto names = f.symbol("/usr/lib/libSystem.B.dylib", "_sys_signame");
    require(f.read(f.engine.read64(names + 15 * 8)) == "term", "sys_signame");
    const auto previous = f.sys("_sigblock", {1U << 29});   // SIGUSR1 (30)
    require(f.sys("_sigsetmask", {previous}) & (1U << 29), "sigblock then sigsetmask returns the blocked mask");
    require(f.sys("_sighold", {30}) == 0 && f.sys("_sigrelse", {30}) == 0, "sighold/sigrelse");
    require(f.sys("_sigignore", {30}) == 0 && f.sys("_bsd_signal", {30, 0}) == 1, "sigignore, then bsd_signal returns SIG_IGN");
    require(f.sys("_siginterrupt", {30, 1}) == 0, "siginterrupt");
}

void test_io_paths(Fixture& f) {
    const auto buffer = f.process.allocate_guest(1024);
    require(f.sys("_dirname_r", {f.text("/usr/lib/libz.dylib"), buffer}) == buffer && f.read(buffer) == "/usr/lib", "dirname_r");
    require(f.sys("_basename_r", {f.text("/usr/lib/"), buffer}) == buffer && f.read(buffer) == "lib", "basename_r");
    require(f.sys("_dirname_r", {f.text("file"), buffer}) == buffer && f.read(buffer) == ".", "dirname_r of a bare name");
    const auto root = std::filesystem::temp_directory_path() / "alchemy-libc-paths";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    const auto deep = f.text(alchemy::guest_path_string(root / "a" / "b"));
    require(f.sys("_mkpath_np", {deep, 0755}) == 0 && std::filesystem::is_directory(root / "a" / "b"), "mkpath_np makes every level");
    require(f.sys("_mkpath_np", {deep, 0755}) == 17, "mkpath_np of an existing directory is EEXIST");
    { std::ofstream file(root / "a" / "file.txt"); file << "x"; }
    // nftw counts files and directories; the callback returns 0 to continue.
    static int files, directories;
    files = directories = 0;
    const auto visit = f.process.install_function("_libc_extra_test_nftw", [](transmute::CpuState& s, transmute::GuestMemory&) {
        if (static_cast<std::uint32_t>(s.x[2]) == 0) ++files;
        if (static_cast<std::uint32_t>(s.x[2]) == 1) ++directories;
        s.x[0] = 0;
    });
    require(f.sys("_nftw", {f.text(alchemy::guest_path_string(root)), visit, 8, 0}) == 0 && files == 1 && directories == 3, "nftw visits the tree");
    // dprintf writes through a descriptor.
    const auto output = root / "printed.txt";
    const auto descriptor = f.sys("_open", {f.text(alchemy::guest_path_string(output)), 0x601, 0644});
    alchemy::CallRegs registers{};
    registers.x[0] = descriptor; registers.x[1] = f.text("n=%d");
    registers.push_variadic(std::uint64_t{42});
    require(alchemy::engine_call_regs(f.engine, f.symbol("/usr/lib/libSystem.B.dylib", "_dprintf"), registers).x0 == 4, "dprintf");
    f.sys("_close", {descriptor});
    { std::ifstream file(output); std::string line; std::getline(file, line); require(line == "n=42", "dprintf wrote the text"); }
    // ether_aton / ether_ntoa.
    const auto ethernet = f.sys("_ether_aton", {f.text("0:1b:21:ab:cd:ef")});
    require(ethernet && f.byte(ethernet + 1) == 0x1b && f.read(f.sys("_ether_ntoa", {ethernet})) == "0:1b:21:ab:cd:ef", "ether_aton/ether_ntoa");
    require(f.sys("_ether_aton", {f.text("0:1b:21")}) == 0, "a short address is refused");
    std::filesystem::remove_all(root, ignored);
}

// libm (numeric_math.cpp's install_numeric_extra): the integer results and out-parameters
// (the reference engine reports x0 only).
void test_math(Fixture& f) {
    const auto real = [&](std::string_view name, double value, std::initializer_list<Address> integers = {}) {
        alchemy::CallRegs registers{};
        registers.set_d(0, value);
        std::size_t index = 0;
        for (const auto argument : integers) registers.x[index++] = argument;
        return alchemy::engine_call_regs(f.engine, f.symbol("/usr/lib/libSystem.B.dylib", name), registers).x0;
    };
    const auto real2 = [&](std::string_view name, double first, double second, Address integer) {
        alchemy::CallRegs registers{};
        registers.set_d(0, first); registers.set_d(1, second);
        registers.x[0] = integer;
        return alchemy::engine_call_regs(f.engine, f.symbol("/usr/lib/libSystem.B.dylib", name), registers).x0;
    };
    require(real("_lround", 2.5) == 3 && real("_lround", -2.5) == static_cast<Address>(-3), "lround rounds half away from zero");
    require(real("_llrint", 2.5) == 2 && real("_lrint", 3.5) == 4, "lrint rounds to even in the default mode");
    require(real("_ilogb", 1024.0) == 10 && real("_ilogbl", 0.5) == static_cast<std::uint32_t>(-1), "ilogb");
    require(real("_finite", 1.0) == 1 && real("_finite", std::numeric_limits<double>::infinity()) == 0, "finite");
    const auto quotient = f.process.allocate_guest(4);
    static_cast<void>(real2("_remquo", 7.0, 2.0, quotient));
    require(f.engine.read32(quotient) == 4 || f.engine.read32(quotient) == 3, "remquo's quotient bits");   // 7/2 = 3.5 rounds to 4
    const auto sign = f.process.allocate_guest(4);
    static_cast<void>(real("_lgamma_r", -0.5, {sign}));
    require(static_cast<std::int32_t>(f.engine.read32(sign)) == -1, "lgamma_r reports Γ(-0.5) < 0");
    static_cast<void>(real("_lgamma_r", 3.0, {sign}));
    require(f.engine.read32(sign) == 1, "lgamma_r of a positive argument");
    const auto flags = f.process.allocate_guest(2);
    require(f.sys("_fegetexceptflag", {flags, 0x1f}) == 0, "fegetexceptflag");
    static_cast<transmute::GuestMemory&>(f.engine).write8(flags, 0x10); static_cast<transmute::GuestMemory&>(f.engine).write8(flags + 1, 0);   // FE_INEXACT
    require(f.sys("_fesetexceptflag", {flags, 0x10}) == 0, "fesetexceptflag");
}

} // namespace

int main() {
    try {
        const auto stage = [](std::string_view name) { std::cout << "[stage] " << name << std::endl; };
        Fixture f;
        stage("strings"); test_strings(f);
        stage("wide"); test_wide(f);
        stage("search"); test_search(f);
        stage("stdlib"); test_stdlib(f);
        stage("time, ctype, signals"); test_time_ctype_signals(f);
        stage("io and paths"); test_io_paths(f);
        stage("math"); test_math(f);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "libc extra tests passed\n";
    return 0;
}
