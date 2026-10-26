// Chrome libSystem support tests (src/alchemy/darwin/libsystem/misc/leftovers.cpp):
// the malloc zone registry with a guest zone made the default zone the way
// PartitionAlloc's EarlyMallocZoneRegistration does it, and the libSystem
// leftovers Chrome's images import.
#include "alchemy/gui/tests/fill_fixture.hpp"
#include "alchemy/darwin/compression/bzip2.hpp"
#include "alchemy/host/network_policy.hpp"
#include "alchemy/darwin/network/http.hpp"
#include "alchemy/process_tree.hpp"
#include "alchemy/path.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <chrono>
#include <thread>
#include <cmath>
#include <condition_variable>
#include <mutex>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace {
using namespace fill_test;
constexpr std::string_view sys = "/usr/lib/libSystem.B.dylib";

// A guest zone whose function table is synthetic calls over a block map:
// what a guest allocator (PartitionAlloc) registers.
struct GuestZone {
    Address zone{}, introspect{};
    std::map<Address, Address> blocks;   // address -> size
    std::vector<std::string> calls;
    Address allocate(Fixture& f, Address size, Address alignment = 16) {
        const auto address = f.process.allocate_guest(std::max<Address>(size, 1), std::max<Address>(alignment, 16));
        blocks[address] = size;
        return address;
    }
};

GuestZone& make_zone(Fixture& f) {
    static GuestZone state;
    auto* z = &state;
    const auto function = [&f, z](const char* name, std::function<Address(transmute::CpuState&)> body) {
        return f.process.install_function(std::string("_test_zone_") + name, [z, name, body](auto& s, auto&) {
            z->calls.emplace_back(name);
            s.x[0] = body(s);
        }, "alchemy.test.zone");
    };
    z->zone = f.block(160);
    z->introspect = f.block(17 * 8);
    const auto put = [&](Address offset, Address value) { f.engine.write64(z->zone + offset, value); };
    put(16, function("size", [z](auto& s) { const auto found = z->blocks.find(s.x[1]); return found == z->blocks.end() ? 0 : std::max<Address>(found->second, 1); }));
    put(24, function("malloc", [&f, z](auto& s) { return z->allocate(f, s.x[1]); }));
    put(32, function("calloc", [&f, z](auto& s) { return z->allocate(f, s.x[1] * s.x[2]); }));
    put(40, function("valloc", [&f, z](auto& s) { return z->allocate(f, s.x[1], 16384); }));
    put(48, function("free", [&f, z](auto& s) { z->blocks.erase(s.x[1]); f.process.release_guest(s.x[1]); return 0; }));
    put(56, function("realloc", [&f, z](auto& s) {
        const auto address = z->allocate(f, s.x[2]);
        if (s.x[1]) {
            f.engine.copy_bytes(address, s.x[1], std::min(z->blocks[s.x[1]], s.x[2]));
            z->blocks.erase(s.x[1]); f.process.release_guest(s.x[1]);
        }
        return address;
    }));
    put(72, f.text("TestZone"));
    put(96, z->introspect);
    f.engine.write32(z->zone + 104, 9);
    put(112, function("memalign", [&f, z](auto& s) { return z->allocate(f, s.x[2], s.x[1]); }));
    put(128, function("pressure_relief", [](auto& s) { return s.x[1] / 2; }));
    f.engine.write64(z->introspect + 7 * 8, function("statistics", [&f, z](auto& s) {
        Address bytes = 0;
        for (const auto& [address, size] : z->blocks) bytes += size;
        f.engine.write32(s.x[1], static_cast<std::uint32_t>(z->blocks.size()));
        f.engine.write64(s.x[1] + 8, bytes); f.engine.write64(s.x[1] + 16, bytes); f.engine.write64(s.x[1] + 24, bytes);
        return 0;
    }));
    return state;
}

std::vector<Address> all_zones(Fixture& f) {
    const auto self = f.engine.read32(f.symbol(sys, "_mach_task_self_"));
    const auto array = f.block(8), count = f.block(4);
    require(f.status(sys, "_malloc_get_all_zones", {self, 0, array, count}) == 0, "malloc_get_all_zones");
    std::vector<Address> result;
    for (std::uint32_t index = 0; index < f.engine.read32(count); ++index)
        result.push_back(f.engine.read64(f.engine.read64(array) + index * 8));
    return result;
}

void system_zone(Fixture& f) {
    const auto system = f.call(sys, "_malloc_default_zone");
    require(system != 0 && all_zones(f) == std::vector<Address>{system}, "one zone: the system zone");
    require(f.call(sys, "_malloc_default_purgeable_zone") == system, "purgeable zone is the system zone");
    require(f.read_text(f.call(sys, "_malloc_get_zone_name", {system})) == "DefaultMallocZone", "system zone name");
    // A complete function table up to the advertised version (10: claimed_address).
    require(f.engine.read32(system + 104) == 10, "system zone version");
    for (const Address offset : {16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 112, 120, 128, 136})
        require(f.engine.read64(system + offset) != 0, "system zone member at offset " + std::to_string(offset));
    const auto introspect = f.engine.read64(system + 96);
    for (Address offset = 0; offset < 128; offset += 8)
        require(f.engine.read64(introspect + offset) != 0, "introspection member at offset " + std::to_string(offset));
    const auto zone_call = [&](Address function, std::initializer_list<Address> arguments) {
        return f.engine.invoke_callback(function, std::span(arguments.begin(), arguments.size()));
    };
    require(zone_call(f.engine.read64(introspect + 8), {system, 17}) == 32 && zone_call(f.engine.read64(introspect + 8), {system, 0}) == 16,
            "introspect->good_size rounds to the quantum");
    require(zone_call(f.engine.read64(introspect + 16), {system}) == 1, "introspect->check");
    require(zone_call(f.engine.read64(introspect + 64), {system}) == 0, "introspect->zone_locked");
    require(zone_call(f.engine.read64(introspect + 0), {0, 0, 1, system, 0, 0}) == 46, "enumerator is not supported");
    const auto results = f.block(3 * 8);
    require(zone_call(f.engine.read64(system + 80), {system, 24, results, 3}) == 3, "batch_malloc");
    require(zone_call(f.engine.read64(system + 136), {system, f.engine.read64(results)}) == 1, "claimed_address(own block)");
    require(zone_call(f.engine.read64(system + 136), {system, f.block(16)}) == 0, "claimed_address(other memory)");
    zone_call(f.engine.read64(system + 88), {system, results, 3});
    require(f.call(sys, "_malloc_zone_from_ptr", {f.engine.read64(results)}) == 0, "batch_free");
    const auto empty = f.call(sys, "_malloc", {0});
    require(zone_call(f.engine.read64(system + 16), {system, empty}) != 0, "size() claims a malloc(0) block");
    f.call(sys, "_free", {empty});
    const auto block = f.call(sys, "_malloc", {40});
    require(block && f.call(sys, "_malloc_size", {block}) == 40, "malloc_size");
    require(f.call(sys, "_malloc_zone_from_ptr", {block}) == system, "malloc_zone_from_ptr(host block)");
    require(f.call(sys, "_malloc_zone_from_ptr", {f.block(16)}) == 0, "malloc_zone_from_ptr(not malloc memory)");
    const auto grown = f.call(sys, "_realloc", {block, 4000});
    require(grown && f.call(sys, "_malloc_size", {grown}) == 4000, "realloc");
    f.call(sys, "_free", {grown});
    const auto aligned = f.block(8);
    require(f.status(sys, "_posix_memalign", {aligned, 4096, 100}) == 0 && f.engine.read64(aligned) % 4096 == 0, "posix_memalign");
    require(f.status(sys, "_posix_memalign", {aligned, 12, 100}) == 22, "posix_memalign EINVAL");
    f.call(sys, "_free", {f.engine.read64(aligned)});
    const auto live = f.call(sys, "_malloc", {64});
    const auto stats = f.block(32);
    f.call(sys, "_malloc_zone_statistics", {system, stats});
    require(f.engine.read32(stats) > 0 && f.engine.read64(stats + 8) >= 64, "system zone statistics");
    f.call(sys, "_free", {live});
}

// PartitionAlloc's order (Chromium early_zone_registration_apple.cc, design):
// register the delegating zone, unregister and re-register the default zone.
void guest_default_zone(Fixture& f) {
    auto& z = make_zone(f);
    const auto system = f.call(sys, "_malloc_default_zone");
    const auto purgeable = f.call(sys, "_malloc_default_purgeable_zone");
    const auto host_block = f.call(sys, "_malloc", {24});
    f.call(sys, "_malloc_zone_register", {z.zone});
    require(all_zones(f) == std::vector<Address>{system, z.zone}, "register appends");
    f.call(sys, "_malloc_zone_unregister", {system});
    require(all_zones(f) == std::vector<Address>{z.zone}, "unregister moves the last zone into the slot");
    f.call(sys, "_malloc_zone_register", {system});
    f.call(sys, "_malloc_zone_unregister", {purgeable});
    f.call(sys, "_malloc_zone_register", {purgeable});
    require(all_zones(f) == std::vector<Address>{z.zone, system}, "guest zone is zone 0");
    require(f.call(sys, "_malloc_default_zone") == z.zone, "malloc_default_zone is zone 0");

    // malloc(3) now runs the guest zone.
    z.calls.clear();
    const auto block = f.call(sys, "_malloc", {100});
    require(z.blocks.contains(block) && z.calls == std::vector<std::string>{"malloc"}, "malloc goes to the default zone");
    require(f.call(sys, "_malloc_size", {block}) == 100, "malloc_size asks the owning zone");
    require(f.call(sys, "_malloc_zone_from_ptr", {block}) == z.zone, "malloc_zone_from_ptr(guest block)");
    require(f.call(sys, "_malloc_zone_from_ptr", {host_block}) == system, "host blocks stay in the system zone");
    const auto grown = f.call(sys, "_realloc", {block, 300});
    require(z.blocks.contains(grown) && !z.blocks.contains(block), "realloc goes to the owning zone");
    const auto zeroed = f.call(sys, "_calloc", {4, 8});
    require(z.blocks.contains(zeroed), "calloc goes to the default zone");
    const auto aligned = f.block(8);
    require(f.status(sys, "_posix_memalign", {aligned, 256, 64}) == 0 && z.blocks.contains(f.engine.read64(aligned)) &&
            f.engine.read64(aligned) % 256 == 0, "posix_memalign uses the zone's memalign");
    z.calls.clear();
    f.call(sys, "_free", {host_block});
    require(z.calls.empty(), "free of a host block stays on the host");
    f.call(sys, "_free", {grown});
    require(!z.blocks.contains(grown) && z.calls.back() == "free", "free goes to the owning zone");
    f.call(sys, "_free", {zeroed});
    f.call(sys, "_free", {f.engine.read64(aligned)});
    require(z.blocks.empty(), "every guest block freed");

    // malloc_zone_* on the guest zone, and NULL as the default zone.
    const auto direct = f.call(sys, "_malloc_zone_malloc", {z.zone, 8});
    require(z.blocks.contains(direct), "malloc_zone_malloc");
    const auto stats = f.block(32);
    f.call(sys, "_malloc_zone_statistics", {z.zone, stats});
    require(f.engine.read32(stats) == 1 && f.engine.read64(stats + 8) == 8, "guest zone statistics through introspect");
    f.call(sys, "_malloc_zone_free", {0, direct});
    require(z.blocks.empty(), "malloc_zone_free(NULL) is the default zone");
    require(f.call(sys, "_malloc_zone_pressure_relief", {z.zone, 64}) == 32, "pressure relief calls the zone");
    require(f.read_text(f.call(sys, "_malloc_get_zone_name", {z.zone})) == "TestZone", "guest zone name");
    const auto system_block = f.call(sys, "_malloc_zone_malloc", {system, 16});
    require(f.call(sys, "_malloc_zone_from_ptr", {system_block}) == system, "malloc_zone_malloc(system zone) stays on the host");
    f.call(sys, "_malloc_zone_free", {system, system_block});

    // Back to the system zone.
    f.call(sys, "_malloc_zone_unregister", {z.zone});
    require(all_zones(f) == std::vector<Address>{system}, "unregistered");
    require(f.call(sys, "_malloc_default_zone") == system, "system zone is the default again");
    const auto again = f.call(sys, "_malloc", {8});
    require(f.call(sys, "_malloc_zone_from_ptr", {again}) == system, "malloc back on the host");
    f.call(sys, "_free", {again});
}

void mstats(Fixture& f) {
    const auto live = f.call(sys, "_malloc", {64});
    alchemy::CallRegs registers{};
    const auto result = f.block(40);
    registers.indirect_result = result;
    static_cast<void>(f.regs(sys, "_mstats", registers));
    require(f.engine.read64(result + 8) > 0, "mstats chunks_used");
    f.call(sys, "_free", {live});
}

// argv as guest memory; returns the argv array address.
Address make_argv(Fixture& f, std::initializer_list<std::string_view> items) {
    const auto argv = f.block((items.size() + 1) * 8);
    std::size_t index = 0;
    for (const auto item : items) f.engine.write64(argv + 8 * index++, f.text(item));
    return argv;
}

void getopt_long(Fixture& f) {
    const auto optind = f.symbol(sys, "_optind"), optarg = f.symbol(sys, "_optarg");
    // struct option {name, has_arg, flag, val}: --database=PATH, --url PATH, --no-rate-limit (flag), --verbose.
    const auto longopts = f.block(5 * 32);
    const auto flag = f.block(4);
    const auto put = [&](unsigned index, std::string_view name, unsigned has_arg, Address flag_address, unsigned value) {
        f.engine.write64(longopts + index * 32, f.text(name));
        f.engine.write32(longopts + index * 32 + 8, has_arg);
        f.engine.write64(longopts + index * 32 + 16, flag_address);
        f.engine.write32(longopts + index * 32 + 24, value);
    };
    put(0, "database", 1, 0, 'd'); put(1, "url", 1, 0, 'u'); put(2, "no-rate-limit", 0, flag, 7); put(3, "verbose", 0, 0, 'v');
    const auto argv = make_argv(f, {"crashpad_handler", "--database=/tmp/db", "file1", "--url", "http://x", "-v", "--no-rate", "file2", "--bogus"});
    const auto spec = f.text("v");
    const auto index = f.block(4);
    f.engine.write32(optind, 1);
    f.engine.write32(f.symbol(sys, "_opterr"), 0);
    const auto next = [&] { return static_cast<std::int32_t>(f.call(sys, "_getopt_long", {9, argv, spec, longopts, index})); };
    require(next() == 'd' && f.read_text(f.engine.read64(optarg)) == "/tmp/db", "--database=value");
    require(next() == 'u' && f.read_text(f.engine.read64(optarg)) == "http://x", "--url value (after a permuted non-option)");
    require(next() == 'v', "-v");
    require(next() == 0 && f.engine.read32(flag) == 7 && f.engine.read32(index) == 2, "unique prefix sets the flag");
    require(next() == '?', "unknown long option");
    require(next() == -1, "end");
    require(f.engine.read32(optind) == 7 && f.read_text(f.engine.read64(argv + 7 * 8)) == "file1" &&
            f.read_text(f.engine.read64(argv + 8 * 8)) == "file2", "non-options permuted to the end");
    // Plain getopt stops at the first non-option.
    const auto plain = make_argv(f, {"tool", "-ab", "-c", "value", "rest", "-a"});
    f.engine.write32(optind, 1);
    const auto spec2 = f.text("abc:");
    const auto step = [&] { return static_cast<std::int32_t>(f.call(sys, "_getopt", {6, plain, spec2})); };
    require(step() == 'a' && step() == 'b', "grouped short options");
    require(step() == 'c' && f.read_text(f.engine.read64(optarg)) == "value", "short option argument");
    require(step() == -1 && f.engine.read32(optind) == 4, "getopt stops at the first non-option");
    f.engine.write32(optind, 1);
    const auto missing = make_argv(f, {"tool", "-c"});
    const auto spec3 = f.text(":c:");
    require(f.call(sys, "_getopt", {2, missing, spec3}) == ':', "leading ':' reports a missing argument");
    f.engine.write32(optind, 1);
}

// getopt_long_only: "-name" is a long option; a single letter that is a short option stays short.
void getopt_long_only(Fixture& f) {
    const auto optind = f.symbol(sys, "_optind"), optarg = f.symbol(sys, "_optarg");
    const auto longopts = f.block(3 * 32);
    const auto put = [&](unsigned index, std::string_view name, unsigned has_arg, unsigned value) {
        f.engine.write64(longopts + index * 32, f.text(name));
        f.engine.write32(longopts + index * 32 + 8, has_arg);
        f.engine.write32(longopts + index * 32 + 24, value);
    };
    put(0, "verbose", 0, 'V'); put(1, "url", 1, 'u');
    const auto argv = make_argv(f, {"tool", "-verbose", "-d", "x", "-url=http://y", "-v", "--url", "z", "-bogus"});
    const auto spec = f.text("vd:");
    f.engine.write32(optind, 1);
    f.engine.write32(f.symbol(sys, "_opterr"), 0);
    const auto next = [&] { return static_cast<std::int32_t>(f.call(sys, "_getopt_long_only", {9, argv, spec, longopts, 0})); };
    require(next() == 'V', "-verbose is the long option");
    require(next() == 'd' && f.read_text(f.engine.read64(optarg)) == "x", "-d stays a short option");
    require(next() == 'u' && f.read_text(f.engine.read64(optarg)) == "http://y", "-url=value");
    require(next() == 'v', "-v is the short option, not a prefix of --verbose");
    require(next() == 'u' && f.read_text(f.engine.read64(optarg)) == "z", "--url value");
    require(next() == '?', "-bogus is neither");
    require(next() == -1, "end");
    f.engine.write32(optind, 1);
}

// err_set_file: warn/err messages go to the stream; err_set_exit registers the exit hook.
void err_hooks(Fixture& f) {
    const auto file = std::filesystem::temp_directory_path() / "alchemy-err-set-file.txt";
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
    const auto stream = f.call(sys, "_fopen", {f.text(alchemy::guest_path_string(file)), f.text("w")});
    require(stream != 0, "fopen");
    f.call(sys, "_err_set_file", {stream});
    alchemy::CallRegs registers;
    registers.x[0] = f.text("hello %d");
    registers.push_variadic(std::uint64_t{5});
    f.regs(sys, "_warnx", registers);
    alchemy::CallRegs coded;
    coded.x[0] = 2;
    coded.x[1] = f.text("missing");
    f.regs(sys, "_warnc", coded);
    f.call(sys, "_err_set_file", {0});
    require(f.call(sys, "_fclose", {stream}) == 0, "fclose");
    std::ifstream in(file);
    std::string first, second;
    std::getline(in, first);
    std::getline(in, second);
    in.close();
    std::filesystem::remove(file, ignored);
    require(first.ends_with("hello 5"), "warnx wrote to the err_set_file stream: " + first);
    require(second.ends_with("missing: No such file or directory"), "warnc names its code: " + second);
    const auto hook = f.process.install_function("_err_test_exit_hook", [](transmute::CpuState&, transmute::GuestMemory&) {});
    f.call(sys, "_err_set_exit", {hook});
    f.call(sys, "_err_set_exit", {0});
}

void byte_swap(Fixture& f) {
    const auto header = f.block(32);
    f.engine.write32(header, 0xfeedfacf);
    f.engine.write32(header + 4, 0x0100000c);
    f.call(sys, "_swap_mach_header_64", {header, 0});
    require(f.engine.read32(header) == 0xcffaedfe && f.engine.read32(header + 4) == 0x0c000001, "swap_mach_header_64");
    const auto segment = f.block(72);
    f.engine.write32(segment, 0x19);
    f.memory().write8(segment + 8, '_');
    f.engine.write64(segment + 24, 0x0000000100000000ULL);
    f.engine.write32(segment + 64, 3);
    f.call(sys, "_swap_segment_command_64", {segment, 0});
    require(f.engine.read32(segment) == 0x19000000 && f.memory().read8(segment + 8) == '_' &&
            f.engine.read64(segment + 24) == 0x0000000001000000ULL && f.engine.read32(segment + 64) == 0x03000000, "swap_segment_command_64");
    const auto arches = f.block(40);
    f.engine.write32(arches + 20, 0x01020304);
    f.call(sys, "_swap_fat_arch", {arches, 2, 0});
    require(f.engine.read32(arches + 20) == 0x04030201, "swap_fat_arch second entry");
}

void bz2_stream(Fixture& f) {
    constexpr std::string_view bz2 = "/usr/lib/libbz2.1.0.dylib";
    std::vector<std::uint8_t> plain(100000);
    for (std::size_t index = 0; index < plain.size(); ++index) plain[index] = static_cast<std::uint8_t>((index * 7) ^ (index >> 5));
    auto packed = alchemy::gui::completion_kit::bzip2::encode(plain, 9);
    packed.push_back(0xab);   // a byte after the stream stays unconsumed
    const auto input = f.block(packed.size()), output = f.block(plain.size());
    f.engine.write_bytes(input, packed);
    const auto strm = f.block(80);
    require(f.status(bz2, "_BZ2_bzDecompressInit", {strm, 0, 0}) == 0, "BZ2_bzDecompressInit");
    f.engine.write64(strm + 24, output);
    std::int32_t result = 0;
    std::size_t fed = 0, rounds = 0;
    while (result == 0 && rounds++ < 1000) {
        if (!f.engine.read32(strm + 8) && fed < packed.size()) {
            const auto chunk = std::min<std::size_t>(4096, packed.size() - fed);
            f.engine.write64(strm, input + fed);
            f.engine.write32(strm + 8, static_cast<std::uint32_t>(chunk));
            fed += chunk;
        }
        f.engine.write32(strm + 32, 1000);   // small output windows
        result = f.status(bz2, "_BZ2_bzDecompress", {strm});
    }
    require(result == 4, "BZ_STREAM_END");
    std::vector<std::uint8_t> got(plain.size());
    f.engine.read_bytes(output, got);
    require(got == plain && f.engine.read32(strm + 36) == plain.size(), "decoded bytes and total_out");
    require(f.engine.read32(strm + 8) == 1 && f.memory().read8(f.engine.read64(strm)) == 0xab, "trailing byte left in next_in");
    require(f.status(bz2, "_BZ2_bzDecompressEnd", {strm}) == 0, "BZ2_bzDecompressEnd");
    // Not bzip2 data.
    const auto bad = f.block(80);
    require(f.status(bz2, "_BZ2_bzDecompressInit", {bad, 0, 0}) == 0, "init");
    const auto junk = f.text("this is not bzip2 data at all");
    f.engine.write64(bad, junk);
    f.engine.write32(bad + 8, 29);
    f.engine.write64(bad + 24, output);
    f.engine.write32(bad + 32, 100);
    require(f.status(bz2, "_BZ2_bzDecompress", {bad}) == -5, "BZ_DATA_ERROR_MAGIC");
    f.call(bz2, "_BZ2_bzDecompressEnd", {bad});
}

void misc(Fixture& f) {
    // __progname comes from the module loader (no image is loaded here): publish one.
    const auto progname = f.block(8);
    f.engine.write64(progname, f.text("crashpad_handler"));
    f.process.install_export(std::string(sys), "___progname", {progname, alchemy::ExportKind::data});
    require(f.read_text(f.call(sys, "_getprogname")) == "crashpad_handler", "getprogname");
    f.call(sys, "_setprogname", {f.text("/Applications/Tool.app/Contents/MacOS/tool")});
    require(f.read_text(f.call(sys, "_getprogname")) == "tool", "setprogname keeps the base name");
    // Math (floating-point arguments and results).
    alchemy::CallRegs registers{};
    registers.set_s(0, 2.5f);
    const auto rounded = f.regs(sys, "_nearbyintf", registers);
    require(!rounded.complete || rounded.s(0) == 2.0f, "nearbyintf ties to even");
    registers.set_s(0, 3.0f); registers.set_s(1, 4.0f); registers.set_s(2, 5.0f);
    const auto fused = f.regs(sys, "_fmaf", registers);
    require(!fused.complete || fused.s(0) == 17.0f, "fmaf");
    const auto not_a_number = f.regs(sys, "_nan", registers);
    require(!not_a_number.complete || std::isnan(not_a_number.d(0)), "nan");
    require(f.call(sys, "___math_errhandling") == 2, "math_errhandling");
    // Membership and audit tokens.
    const auto uuid = f.block(16);
    require(f.call(sys, "_mbr_uid_to_uuid", {501, uuid}) == 0 && f.memory().read8(uuid) == 0xff && f.memory().read8(uuid + 10) == 0xaa &&
            f.memory().read8(uuid + 14) == 0x01 && f.memory().read8(uuid + 15) == 0xf5, "mbr_uid_to_uuid compatibility UUID");
    const auto token = f.block(32);
    f.engine.write32(token + 4, 501);
    require(f.call("/usr/lib/libbsm.0.dylib", "_audit_token_to_euid", {token}) == 501, "audit_token_to_euid");
    // Code signing and SIP.
    const auto flags = f.block(4, 0xff);
    require(f.status(sys, "_csops", {0, 0, flags, 4}) == 0 && f.engine.read32(flags) == 0, "csops CS_OPS_STATUS");
    require(f.status(sys, "_csops", {0, 5, flags, 4}) == -1, "csops other operations fail");
    const auto config = f.block(4, 0xff);
    require(f.status(sys, "_csr_get_active_config", {config}) == 0 && f.engine.read32(config) == 0, "csr_get_active_config");
    require(f.status(sys, "___mac_syscall", {f.text("Sandbox"), 0, 0}) == -1, "__mac_syscall");
    require(f.call(sys, "_acl_get_link_np", {f.text("/tmp"), 0x100}) == 0, "acl_get_link_np: no ACL");
    require(f.call(sys, "_acl_get_fd_np", {0, 0x100}) == 0 && f.call(sys, "_acl_get_file", {f.text("/tmp"), 0x100}) == 0, "no file carries an ACL");
    require(f.status(sys, "_acl_valid", {0x1000}) == -1, "acl_valid: no acl_t exists");
    require(f.status(sys, "_fsetattrlist", {0, f.block(24), f.block(16), 16, 0}) == -1, "fsetattrlist: no attribute lists");
    // QoS overrides are tokens.
    const auto override_token = f.call(sys, "_pthread_override_qos_class_start_np", {f.call(sys, "_pthread_self"), 0x19, 0});
    require(override_token != 0 && f.call(sys, "_pthread_override_qos_class_end_np", {override_token}) == 0, "QoS override");
    const auto qos = f.block(4);
    require(f.call(sys, "_pthread_get_qos_class_np", {f.call(sys, "_pthread_self"), qos, 0}) == 0 && f.engine.read32(qos) == 0x15, "QoS class");
    require(f.call(sys, "_dispatch_queue_create_with_target$V2", {f.text("org.chromium.test"), 0, 0}) != 0, "dispatch_queue_create_with_target");
    require(f.read_text(f.call(sys, "_tmpnam", {0})).starts_with("/tmp/tmp."), "tmpnam");
}

std::uint32_t guest_errno(Fixture& f) { return f.engine.read32(f.call(sys, "___error")); }
std::int64_t result(Address value) { return static_cast<std::int64_t>(value); }

// A per-run guest directory under /tmp (the host temporary directory).
std::string scratch_directory(Fixture& f, std::string_view name) {
    const auto path = "/tmp/alchemy-c2-" + std::to_string(f.call(sys, "_getpid")) + "-" + std::string(name);
    static_cast<void>(f.call(sys, "_mkdir", {f.text(path), 0755}));
    return path;
}

// struct sockaddr_un {sun_len, sun_family = AF_UNIX, sun_path[104]}.
Address unix_address(Fixture& f, std::string_view path) {
    const auto address = f.block(106);
    f.memory().write8(address, static_cast<std::uint8_t>(2 + path.size() + 1));
    f.memory().write8(address + 1, 1);
    std::vector<std::uint8_t> bytes(path.begin(), path.end());
    f.engine.write_bytes(address + 2, bytes);
    return address;
}

void set_nonblocking(Fixture& f, Address descriptor) {
    alchemy::CallRegs registers{};
    registers.x[0] = descriptor; registers.x[1] = 4;   // F_SETFL
    registers.push_variadic(std::uint64_t{4});          // O_NONBLOCK
    require(f.regs(sys, "_fcntl", registers).x0 == 0, "fcntl(F_SETFL, O_NONBLOCK)");
}

// ioctl(FIONBIO) on a pipe sets O_NONBLOCK as fcntl does (libuv, in
// Electron's Node, sets non-blocking mode this way).
void pipe_fionbio(Fixture& f) {
    const auto descriptors = f.block(8);
    require(f.call(sys, "_pipe", {descriptors}) == 0, "pipe");
    const auto reader = f.memory().read32(descriptors), writer = f.memory().read32(descriptors + 4);
    const auto on = f.block(4);
    f.memory().write32(on, 1);
    alchemy::CallRegs registers{};
    registers.x[0] = reader; registers.x[1] = 0x8004667e;   // FIONBIO
    registers.push_variadic(on);
    require(f.regs(sys, "_ioctl", registers).x0 == 0, "ioctl(FIONBIO) on a pipe");
    alchemy::CallRegs get{};
    get.x[0] = reader; get.x[1] = 3;   // F_GETFL
    require((f.regs(sys, "_fcntl", get).x0 & 4) == 4, "FIONBIO sets O_NONBLOCK");
    const auto buffer = f.block(8);
    require(result(f.call(sys, "_read", {reader, buffer, 8})) == -1 && guest_errno(f) == 35, "a non-blocking empty pipe read: EAGAIN");
    f.memory().write32(on, 0);
    require(f.regs(sys, "_ioctl", registers).x0 == 0 && (f.regs(sys, "_fcntl", get).x0 & 4) == 0, "FIONBIO 0 clears O_NONBLOCK");
    static_cast<void>(f.call(sys, "_close", {reader}));
    static_cast<void>(f.call(sys, "_close", {writer}));
}

// sendfile(2) sends a file's bytes on a stream socket and refuses a
// non-socket with ENOTSOCK; an AF_UNIX stream whose peer closed with unread
// data reads end-of-file, as on Darwin (the host resets it).
void sendfile_and_unix_eof(Fixture& f) {
    const auto directory = scratch_directory(f, "sendfile");
    const auto path = directory + "/data.txt";
    alchemy::CallRegs open{};
    open.x[0] = f.text(path); open.x[1] = 0x202;   // O_RDWR | O_CREAT
    open.push_variadic(std::uint64_t{0644});
    const auto file = static_cast<std::int32_t>(f.regs(sys, "_open", open).x0);
    require(file >= 0, "open data file");
    require(f.call(sys, "_write", {static_cast<Address>(file), f.text("hello sendfile"), 14}) == 14, "write data file");
    // libuv's copyfile passes the source's st_mode, file-type bits included.
    require(f.call(sys, "_fchmod", {static_cast<Address>(file), 0100666}) == 0, "fchmod ignores the file-type bits");
    const auto pair = f.block(8);
    require(f.call(sys, "_socketpair", {1, 1, 0, pair}) == 0, "socketpair");
    const auto first = f.memory().read32(pair), second = f.memory().read32(pair + 4);
    const auto length = f.block(8);
    f.memory().write64(length, 0);   // to the end of the file
    require(f.call(sys, "_sendfile", {static_cast<Address>(file), first, 6, length, 0, 0}) == 0 && f.memory().read64(length) == 8,
            "sendfile from offset 6 to the end");
    const auto buffer = f.block(32);
    require(f.call(sys, "_read", {second, buffer, 32}) == 8 && f.read_text(buffer, 8) == "sendfile", "sendfile bytes arrive");
    const auto pipe_ends = f.block(8);
    require(f.call(sys, "_pipe", {pipe_ends}) == 0, "pipe");
    f.memory().write64(length, 4);
    require(result(f.call(sys, "_sendfile", {static_cast<Address>(file), f.memory().read32(pipe_ends + 4), 0, length, 0, 0})) == -1 &&
            guest_errno(f) == 38, "sendfile to a pipe: ENOTSOCK");
    // A socket dup2'd onto descriptor 0 is not a terminal, whatever the host's stdin is.
    const auto saved = f.call(sys, "_dup", {0});
    require(f.call(sys, "_dup2", {first, 0}) == 0, "dup2 a socket onto 0");
    require(f.call(sys, "_isatty", {0}) == 0 && guest_errno(f) == 25, "isatty of a socket on descriptor 0: ENOTTY");
    require(f.call(sys, "_dup2", {saved, 0}) == 0, "restore descriptor 0");
    static_cast<void>(f.call(sys, "_close", {saved}));
    // The peer closes with data it never read: this side reads end-of-file.
    require(f.call(sys, "_write", {first, f.text("unread"), 6}) == 6, "write to the peer");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    static_cast<void>(f.call(sys, "_close", {second}));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    require(f.call(sys, "_read", {first, buffer, 32}) == 0, "an AF_UNIX stream whose peer closed reads end-of-file");
    for (const auto descriptor : {static_cast<Address>(file), static_cast<Address>(first), static_cast<Address>(f.memory().read32(pipe_ends)),
                                  static_cast<Address>(f.memory().read32(pipe_ends + 4))})
        static_cast<void>(f.call(sys, "_close", {descriptor}));
    static_cast<void>(f.call(sys, "_unlink", {f.text(path)}));
}

void unix_sockets(Fixture& f) {
    const auto directory = scratch_directory(f, "unix");
    const auto path = directory + "/SingletonSocket";
    static_cast<void>(f.call(sys, "_unlink", {f.text(path)}));
    const auto server = f.call(sys, "_socket", {1, 1, 0});
    require(result(server) >= 0, "socket(AF_UNIX, SOCK_STREAM)");
    const auto address = unix_address(f, path);
    require(f.call(sys, "_bind", {server, address, 106}) == 0, "bind");
    require(f.call(sys, "_listen", {server, 5}) == 0, "listen");
    const auto second = f.call(sys, "_socket", {1, 1, 0});
    const auto rebind = result(f.call(sys, "_bind", {second, address, 106}));
    require(rebind == -1 && guest_errno(f) == 48, "bind of an existing path: EADDRINUSE (got " + std::to_string(rebind) + " errno " + std::to_string(guest_errno(f)) + ")");
    f.call(sys, "_close", {second});

    // getsockname: the bound pathname.
    const auto name = f.block(128), length = f.block(4);
    f.engine.write32(length, 128);
    require(f.call(sys, "_getsockname", {server, name, length}) == 0 && f.memory().read8(name + 1) == 1 &&
            f.read_text(name + 2) == path, "getsockname of the listener");

    // A blocking client, then a nonblocking client that completes asynchronously.
    const auto client = f.call(sys, "_socket", {1, 1, 0});
    const auto connected_status = f.call(sys, "_connect", {client, address, 106});
    require(connected_status == 0, "connect (" + std::to_string(result(connected_status)) + ", errno " + std::to_string(guest_errno(f)) + ")");
    f.engine.write32(length, 128);
    require(f.call(sys, "_getpeername", {client, name, length}) == 0 && f.read_text(name + 2) == path, "getpeername of the client");
    const auto peer = f.block(128), peer_length = f.block(4);
    f.engine.write32(peer_length, 128);
    const auto accepted = f.call(sys, "_accept", {server, peer, peer_length});
    require(result(accepted) >= 0 && f.memory().read8(peer + 1) == 1, "accept (an unnamed peer)");
    f.engine.write32(length, 128);
    require(f.call(sys, "_getsockname", {accepted, name, length}) == 0 && f.read_text(name + 2) == path, "accepted socket is named by the listener path");

    // Data both ways through read/write and send/recv.
    const auto message = f.text("START\n/tmp\0x");
    require(f.call(sys, "_write", {client, message, 6}) == 6, "write");
    // poll and kevent see the data.
    const auto pollfd = f.block(8);
    f.engine.write32(pollfd, static_cast<std::uint32_t>(accepted)); f.memory().write8(pollfd + 4, 1);
    require(f.call(sys, "_poll", {pollfd, 1, 2000}) == 1 && (f.memory().read8(pollfd + 6) & 1), "poll POLLIN");
    const auto queue = f.call(sys, "_kqueue");
    const auto change = f.block(32), event = f.block(32), timeout = f.block(16);
    f.engine.write64(change, accepted); f.memory().write8(change + 8, 0xff); f.memory().write8(change + 9, 0xff);   // EVFILT_READ
    f.memory().write8(change + 10, 1);                                                                             // EV_ADD
    f.engine.write64(timeout, 2);
    require(f.call(sys, "_kevent", {queue, change, 1, event, 1, timeout}) == 1 && f.engine.read64(event) == accepted &&
            f.engine.read64(event + 16) == 6, "kevent EVFILT_READ with the byte count");
    const auto buffer = f.block(64);
    require(f.call(sys, "_read", {accepted, buffer, 64}) == 6 && f.read_text(buffer, 6) == "START\n", "read");
    require(f.call(sys, "_send", {accepted, message, 5, 0}) == 5 && f.call(sys, "_recv", {client, buffer, 64, 0}) == 5, "send/recv");

    // sendmsg/recvmsg with two iovecs; control data is refused.
    const auto iov = f.block(32), header = f.block(48);
    f.engine.write64(iov, message); f.engine.write64(iov + 8, 3);
    f.engine.write64(iov + 16, message + 3); f.engine.write64(iov + 24, 3);
    f.engine.write64(header + 16, iov); f.engine.write32(header + 24, 2);
    require(f.call(sys, "_sendmsg", {client, header, 0}) == 6, "sendmsg gathers the iovecs");
    const auto in_iov = f.block(32), in_header = f.block(48), first = f.block(2), rest = f.block(16);
    f.engine.write64(in_iov, first); f.engine.write64(in_iov + 8, 2);
    f.engine.write64(in_iov + 16, rest); f.engine.write64(in_iov + 24, 16);
    f.engine.write64(in_header + 16, in_iov); f.engine.write32(in_header + 24, 2);
    f.engine.write64(in_header + 32, f.block(64)); f.engine.write32(in_header + 40, 64);
    require(f.call(sys, "_recvmsg", {accepted, in_header, 0}) == 6 && f.read_text(first, 2) == "ST" && f.read_text(rest, 4) == "ART\n" &&
            f.engine.read32(in_header + 40) == 0, "recvmsg scatters, no control data");
    f.engine.write64(header + 32, f.block(16)); f.engine.write32(header + 40, 16);
    require(result(f.call(sys, "_sendmsg", {client, header, 0})) == -1 && guest_errno(f) == 45, "SCM_RIGHTS/SCM_CREDS: EOPNOTSUPP");

    // A nonblocking connect.
    const auto later = f.call(sys, "_socket", {1, 1, 0});
    set_nonblocking(f, later);
    const auto connected = result(f.call(sys, "_connect", {later, address, 106}));
    require(connected == 0 || guest_errno(f) == 36, "nonblocking connect: 0 or EINPROGRESS");
    const auto writable = f.block(8);
    f.engine.write32(writable, static_cast<std::uint32_t>(later)); f.memory().write8(writable + 4, 4);
    require(f.call(sys, "_poll", {writable, 1, 2000}) == 1, "poll POLLOUT after a nonblocking connect");
    f.engine.write32(peer_length, 128);
    const auto third = f.call(sys, "_accept", {server, peer, peer_length});
    require(result(third) >= 0, "accept the nonblocking client");
    for (const auto descriptor : {later, third, client, accepted, server, queue}) f.call(sys, "_close", {descriptor});

    // The socket file stays until unlink, as on Darwin; then connect fails with ENOENT.
    require(f.call(sys, "_unlink", {f.text(path)}) == 0, "unlink of the socket file");
    const auto orphan = f.call(sys, "_socket", {1, 1, 0});
    require(result(f.call(sys, "_connect", {orphan, address, 106})) == -1 && guest_errno(f) == 2, "connect to a missing path: ENOENT");
    f.call(sys, "_close", {orphan});
    // Datagram and seqpacket AF_UNIX sockets are unavailable on the host.
    require(result(f.call(sys, "_socket", {1, 2, 0})) == -1 && guest_errno(f) == 44, "AF_UNIX SOCK_DGRAM: ESOCKTNOSUPPORT");
    require(result(f.call(sys, "_socket", {1, 5, 0})) == -1 && guest_errno(f) == 44, "AF_UNIX SOCK_SEQPACKET: ESOCKTNOSUPPORT");
    static_cast<void>(f.call(sys, "_rmdir", {f.text(directory)}));
}

// ProcessSingleton's links (Chromium process_singleton_posix.cc, design):
// SingletonLock -> "<host>-<pid>" (dangling) and SingletonSocket -> a socket
// in a temporary directory.
void symlink_records(Fixture& f) {
    const auto profile = scratch_directory(f, "profile"), sockets = scratch_directory(f, "sockets");
    const auto lock = profile + "/SingletonLock";
    require(f.call(sys, "_symlink", {f.text("alchemy-host-4242"), f.text(lock)}) == 0, "dangling symlink");
    const auto buffer = f.block(256);
    require(f.call(sys, "_readlink", {f.text(lock), buffer, 256}) == 17 && f.read_text(buffer, 17) == "alchemy-host-4242", "readlink");
    const auto info = f.block(144);
    require(f.call(sys, "_lstat", {f.text(lock), info}) == 0 && (f.memory().read8(info + 5) & 0xf0) == 0xa0 &&
            f.engine.read64(info + 96) == 17, "lstat: S_IFLNK with the link length");
    require(result(f.call(sys, "_stat", {f.text(lock), info})) == -1 && guest_errno(f) == 2, "stat follows the dangling link: ENOENT");
    require(result(f.call(sys, "_symlink", {f.text("other"), f.text(lock)})) == -1 && guest_errno(f) == 17, "symlink over a link: EEXIST");
    require(f.call(sys, "_unlink", {f.text(lock)}) == 0, "unlink the link");
    require(result(f.call(sys, "_readlink", {f.text(lock), buffer, 256})) == -1, "the link is gone");

    // A socket reached through a link to another directory.
    const auto target = sockets + "/SingletonSocket", link = profile + "/SingletonSocket";
    static_cast<void>(f.call(sys, "_unlink", {f.text(target)}));
    const auto server = f.call(sys, "_socket", {1, 1, 0});
    require(f.call(sys, "_bind", {server, unix_address(f, target), 106}) == 0 && f.call(sys, "_listen", {server, 1}) == 0, "listener");
    require(f.call(sys, "_symlink", {f.text(target), f.text(link)}) == 0, "link to the socket");
    const auto client = f.call(sys, "_socket", {1, 1, 0});
    require(f.call(sys, "_connect", {client, unix_address(f, link), 106}) == 0, "connect through the link");
    f.call(sys, "_close", {client});
    f.call(sys, "_close", {server});
    for (const auto& path : {link, target}) static_cast<void>(f.call(sys, "_unlink", {f.text(path)}));
    for (const auto& path : {profile, sockets}) static_cast<void>(f.call(sys, "_rmdir", {f.text(path)}));
}

// kevent64 EVFILT_TIMER, as MessagePumpKqueue arms its wake-up timer (design).
void kevent_timers(Fixture& f) {
    const auto queue = f.call(sys, "_kqueue");
    const auto change = f.block(48), event = f.block(48 * 4), timeout = f.block(16);
    const auto arm = [&](Address ident, std::uint16_t flags, std::uint32_t fflags, std::uint64_t data) {
        f.memory().fill_bytes(change, 0, 48);
        f.engine.write64(change, ident);
        f.memory().write8(change + 8, 0xf9); f.memory().write8(change + 9, 0xff);   // EVFILT_TIMER (-7)
        f.memory().write8(change + 10, static_cast<std::uint8_t>(flags)); f.memory().write8(change + 11, static_cast<std::uint8_t>(flags >> 8));
        f.engine.write32(change + 12, fflags); f.engine.write64(change + 16, data);
        return f.call(sys, "_kevent64", {queue, change, 1, event, 0, 0, 0});
    };
    f.engine.write64(timeout, 2);
    // One-shot, 20 ms (NOTE_USECONDS).
    require(arm(0x1234567890ULL, 0x11, 0x2, 20000) == 0, "arm a one-shot timer");
    const auto started = f.call(sys, "_mach_absolute_time");
    require(f.call(sys, "_kevent64", {queue, 0, 0, event, 4, 0, timeout}) == 1 && f.engine.read64(event) == 0x1234567890ULL &&
            f.engine.read64(event + 16) == 1, "the one-shot timer fires once");
    require(f.call(sys, "_mach_absolute_time") - started >= 15000000, "not before its interval");
    f.engine.write64(timeout, 0); f.engine.write64(timeout + 8, 50000000);
    require(f.call(sys, "_kevent64", {queue, 0, 0, event, 4, 0, timeout}) == 0, "a one-shot timer is gone after firing");
    // Absolute deadline in mach time (NOTE_MACHTIME | NOTE_ABSOLUTE | NOTE_NSECONDS), 10 ms ahead.
    require(arm(7, 0x11, 0x100 | 0x8 | 0x4, f.call(sys, "_mach_absolute_time") + 10000000) == 0, "arm an absolute timer");
    f.engine.write64(timeout, 2); f.engine.write64(timeout + 8, 0);
    require(f.call(sys, "_kevent64", {queue, 0, 0, event, 4, 0, timeout}) == 1 && f.engine.read64(event) == 7, "the absolute timer fires");
    // Periodic 5 ms, then deleted.
    require(arm(9, 0x1, 0, 5) == 0, "arm a periodic timer");
    require(f.call(sys, "_kevent64", {queue, 0, 0, event, 4, 0, timeout}) == 1 && f.engine.read64(event + 16) >= 1, "the periodic timer fires");
    require(f.call(sys, "_kevent64", {queue, 0, 0, event, 4, 0, timeout}) == 1, "and fires again");
    require(arm(9, 0x2, 0, 0) == 0, "EV_DELETE");
    f.engine.write64(timeout, 0); f.engine.write64(timeout + 8, 30000000);
    require(f.call(sys, "_kevent64", {queue, 0, 0, event, 4, 0, timeout}) == 0, "a deleted timer is silent");
    require(result(arm(10, 0x1, 0x10000, 5)) == -1 && guest_errno(f) == 22, "unknown timer flags: EINVAL");

    // An overdue wake-up: MessagePumpKqueue arms EV_ADD | EV_ONESHOT, NOTE_USECONDS
    // with data = wakeup - now < 0, plus NOTE_LEEWAY with ext[1] (design). It
    // fires at once, once (was EINVAL: message_pump_kqueue.cc "set timer" FATAL).
    f.engine.write64(change + 40, 500);
    const auto overdue = [&](std::uint16_t flags, std::uint32_t fflags) {
        f.memory().write8(change + 10, static_cast<std::uint8_t>(flags)); f.memory().write8(change + 11, static_cast<std::uint8_t>(flags >> 8));
        f.engine.write32(change + 12, fflags); f.engine.write64(change + 16, static_cast<std::uint64_t>(std::int64_t{-1500}));
        return f.call(sys, "_kevent64", {queue, change, 1, event, 0, 0, 0});
    };
    f.engine.write64(change, 11); f.memory().write8(change + 8, 0xf9); f.memory().write8(change + 9, 0xff);
    require(overdue(0x11, 0x2 | 0x10) == 0, "a negative one-shot interval arms");
    f.engine.write64(timeout, 0); f.engine.write64(timeout + 8, 0);
    require(f.call(sys, "_kevent64", {queue, 0, 0, event, 4, 0, timeout}) == 1 && f.engine.read64(event) == 11 &&
            f.engine.read64(event + 16) == 1, "and fires at once, one expiration");
    f.engine.write64(timeout + 8, 30000000);
    require(f.call(sys, "_kevent64", {queue, 0, 0, event, 4, 0, timeout}) == 0, "then it is gone");
    // The same change list returning the event in the same call.
    f.engine.write64(timeout + 8, 0);
    require(f.call(sys, "_kevent64", {queue, change, 1, event, 4, 0, timeout}) == 1 && f.engine.read64(event) == 11,
            "armed and delivered by one kevent64");
    // A negative periodic interval also fires once and does not repeat (XNU, design).
    f.engine.write64(change, 12);
    require(overdue(0x1, 0x2) == 0, "a negative periodic interval arms");
    require(f.call(sys, "_kevent64", {queue, 0, 0, event, 4, 0, timeout}) == 1 && f.engine.read64(event) == 12 &&
            f.engine.read64(event + 16) == 1, "fires once");
    f.engine.write64(timeout + 8, 30000000);
    require(f.call(sys, "_kevent64", {queue, 0, 0, event, 4, 0, timeout}) == 0, "and does not repeat");
    require(arm(12, 0x2, 0, 0) == 0, "it stays registered until EV_DELETE");
    // A past absolute deadline fires at once; a negative absolute one is refused.
    require(arm(13, 0x11, 0x100 | 0x8 | 0x4, 1) == 0, "arm a past absolute deadline");
    f.engine.write64(timeout + 8, 0);
    require(f.call(sys, "_kevent64", {queue, 0, 0, event, 4, 0, timeout}) == 1 && f.engine.read64(event) == 13, "it fires at once");
    require(arm(14, 0x11, 0x8 | 0x2, 1000) == 0, "arm a past wall-clock deadline");
    require(f.call(sys, "_kevent64", {queue, 0, 0, event, 4, 0, timeout}) == 1 && f.engine.read64(event) == 14, "it fires at once");
    require(result(arm(15, 0x11, 0x8 | 0x2, static_cast<std::uint64_t>(std::int64_t{-5}))) == -1 && guest_errno(f) == 22,
            "a negative absolute deadline: EINVAL");
    f.call(sys, "_close", {queue});
}

// Darwin arm64 struct sizes and offsets of the libSystem calls that fill
// caller structs: each output is followed by a guard that must survive.
Address guarded(Fixture& f, std::size_t size) { return f.block(size + 64, 0xab); }
void require_guard(Fixture& f, Address buffer, std::size_t size, std::string_view what) {
    for (std::size_t index = 0; index < 64; ++index)
        require(f.memory().read8(buffer + size + index) == 0xab, std::string(what) + " writes past its Darwin size " + std::to_string(size));
}
void struct_layouts(Fixture& f) {
    const auto directory = scratch_directory(f, "layout");
    const auto path = f.text(directory);
    // struct statvfs: 64 bytes (unsigned long f_bsize, f_frsize; uint32 counts; unsigned long f_fsid, f_flag, f_namemax).
    auto buffer = guarded(f, 64);
    require(f.call(sys, "_statvfs", {path, buffer}) == 0, "statvfs");
    require_guard(f, buffer, 64, "statvfs");
    require(f.engine.read64(buffer) == 4096 && f.engine.read64(buffer + 8) >= 4096 && f.engine.read32(buffer + 16) > 0 &&
            f.engine.read32(buffer + 16) >= f.engine.read32(buffer + 20) && f.engine.read64(buffer + 56) == 255, "statvfs fields");
    const auto descriptor = f.call(sys, "_open", {path, 0});
    buffer = guarded(f, 64);
    require(f.call(sys, "_fstatvfs", {descriptor, buffer}) == 0, "fstatvfs");
    require_guard(f, buffer, 64, "fstatvfs");
    // struct statfs (64-bit inode): 2168 bytes; f_fstypename at 72, f_mntonname at 88, f_mntfromname at 1112.
    for (const auto name : {"_statfs", "_statfs$INODE64"}) {
        buffer = guarded(f, 2168);
        require(f.call(sys, name, {path, buffer}) == 0, name);
        require_guard(f, buffer, 2168, name);
        require(f.engine.read32(buffer) == 4096 && f.engine.read64(buffer + 8) > 0 && !f.read_text(buffer + 72).empty() &&
                f.read_text(buffer + 88).starts_with("/"), std::string(name) + " fields");
    }
    buffer = guarded(f, 2168);
    require(f.call(sys, "_fstatfs", {descriptor, buffer}) == 0, "fstatfs");
    require_guard(f, buffer, 2168, "fstatfs");
    // struct stat (64-bit inode): 144 bytes; st_mode at 4, st_size at 96, st_blksize at 112.
    const auto file = f.text(directory + "/stat.txt");
    const auto output = f.call(sys, "_creat", {file, 0644});
    require(f.call(sys, "_write", {output, f.text("12345"), 5}) == 5, "write");
    for (const auto name : {"_stat", "_stat$INODE64", "_stat64", "_lstat", "_lstat$INODE64"}) {
        buffer = guarded(f, 144);
        require(f.call(sys, name, {file, buffer}) == 0, name);
        require_guard(f, buffer, 144, name);
        require((f.memory().read8(buffer + 5) & 0xf0) == 0x80 && f.engine.read64(buffer + 96) == 5, std::string(name) + " st_mode/st_size");
    }
    for (const auto name : {"_fstat", "_fstat$INODE64", "_fstat64"}) {
        buffer = guarded(f, 144);
        require(f.call(sys, name, {output, buffer}) == 0, name);
        require_guard(f, buffer, 144, name);
        require(f.engine.read64(buffer + 96) == 5, std::string(name) + " st_size");
    }
    f.call(sys, "_close", {output});
    f.call(sys, "_close", {descriptor});
    // struct rusage: 144 bytes (two timevals, fourteen longs).
    buffer = guarded(f, 144);
    require(f.call(sys, "_getrusage", {0, buffer}) == 0, "getrusage");
    require_guard(f, buffer, 144, "getrusage");
    // struct rlimit: 16 bytes.
    buffer = guarded(f, 16);
    require(f.call(sys, "_getrlimit", {8, buffer}) == 0, "getrlimit");
    require_guard(f, buffer, 16, "getrlimit");
    // struct utsname: five 256-byte fields.
    buffer = guarded(f, 1280);
    require(f.call(sys, "_uname", {buffer}) == 0 && f.read_text(buffer) == "Darwin" && f.read_text(buffer + 1024) == "arm64", "uname");
    require_guard(f, buffer, 1280, "uname");
    // sysctl {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid}: struct kinfo_proc, 648 bytes, p_pid at 40.
    const auto mib = f.block(16), length = f.block(8);
    const auto pid = static_cast<std::uint32_t>(f.call(sys, "_getpid"));
    f.engine.write32(mib, 1); f.engine.write32(mib + 4, 14); f.engine.write32(mib + 8, 1); f.engine.write32(mib + 12, pid);
    buffer = guarded(f, 648);
    f.engine.write64(length, 648);
    require(f.call(sys, "_sysctl", {mib, 4, buffer, length, 0, 0}) == 0 && f.engine.read64(length) == 648 && f.engine.read32(buffer + 40) == pid, "kinfo_proc");
    require_guard(f, buffer, 648, "sysctl(KERN_PROC_PID)");
    static_cast<void>(f.call(sys, "_unlink", {file}));
    static_cast<void>(f.call(sys, "_rmdir", {path}));
}

// base::PlatformSharedMemoryRegion (design): a named entry from
// mach_make_memory_entry_64(MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE),
// mapped with vm_map (not mach_vm_map) twice; both views share the pages.
void vm_map_memory_entry(Fixture& f) {
    const auto self = f.engine.read32(f.symbol(sys, "_mach_task_self_"));
    const auto size = f.block(8), handle = f.block(4);
    f.engine.write64(size, 16384);
    require(f.status(sys, "_mach_make_memory_entry_64", {self, size, 0, 0x20003, handle, 0}) == 0, "mach_make_memory_entry_64");
    const auto map = [&](Address object, std::uint32_t protection) {
        const auto address = f.block(8);
        alchemy::CallRegs registers{};
        registers.x = {self, address, 16384, 0, 1 /* VM_FLAGS_ANYWHERE */, object, 0, 0};
        registers.push_stack(protection); registers.push_stack(protection); registers.push_stack(std::uint32_t{2});
        const auto result = f.regs(sys, "_vm_map", registers).x0;
        require(result == 0, "vm_map (kern_return " + std::to_string(result) + ")");
        return f.engine.read64(address);
    };
    const auto entry = f.engine.read32(handle);
    const auto first = map(entry, 3), second = map(entry, 3);
    require(first != second, "two views");
    f.engine.write32(first + 100, 0x5eed5eed);
    require(f.engine.read32(second + 100) == 0x5eed5eed, "the views share the entry's pages");
    const auto anonymous = map(0, 3);
    f.engine.write32(anonymous, 7);
    require(f.engine.read32(anonymous) == 7, "anonymous vm_map still works");
    for (const auto address : {first, second, anonymous}) require(f.status(sys, "_vm_deallocate", {self, address, 16384}) == 0, "vm_deallocate");
    require(f.status(sys, "_mach_port_deallocate", {self, entry}) == 0, "release the entry");
    // More than 256 MiB of 2 MiB entries at once (Chrome's Mojo data pipes; that much ended the browser with
    // KERN_RESOURCE_SHORTAGE): 160 named entries, each mapped once, are 640 MiB of Mach VM pages.
    std::vector<std::pair<std::uint32_t, Address>> pipes;
    for (int index = 0; index < 160; ++index) {
        f.engine.write64(size, 2 << 20);
        const auto result = f.status(sys, "_mach_make_memory_entry_64", {self, size, 0, 0x20003, handle, 0});
        require(result == 0, "2 MiB entry " + std::to_string(index) + " (kern_return " + std::to_string(result) + ")");
        const auto address = f.block(8);
        alchemy::CallRegs registers{};
        registers.x = {self, address, 2 << 20, 0, 1, f.engine.read32(handle), 0, 0};
        registers.push_stack(std::uint32_t{3}); registers.push_stack(std::uint32_t{3}); registers.push_stack(std::uint32_t{2});
        require(f.regs(sys, "_vm_map", registers).x0 == 0, "vm_map of 2 MiB entry " + std::to_string(index));
        f.engine.write64(f.engine.read64(address) + (1 << 20), static_cast<Address>(index));
        pipes.emplace_back(f.engine.read32(handle), f.engine.read64(address));
    }
    for (const auto& [entry, view] : pipes) {
        require(f.status(sys, "_vm_deallocate", {self, view, 2 << 20}) == 0 && f.status(sys, "_mach_port_deallocate", {self, entry}) == 0,
                "release a 2 MiB entry");
    }
    // vm_allocate with a VM tag (mojo's ChannelMac: VM_FLAGS_ANYWHERE | VM_MAKE_TAG(VM_MEMORY_MACH_MSG)).
    const auto slot = f.block(8);
    require(f.status(sys, "_vm_allocate", {self, slot, 16384, 0x14000001}) == 0 && f.engine.read64(slot) != 0, "vm_allocate with a VM tag");
    require(f.status(sys, "_vm_deallocate", {self, f.engine.read64(slot), 16384}) == 0, "vm_deallocate");
}

// fileport_makeport and fileport_makefd: a pipe, and a socket (Chrome's audio service sends one end of a
// socketpair to the renderer as a fileport; sockets were EBADF and Mojo's CHECK ended the process).
void fileports(Fixture& f) {
    const auto port = f.block(4), pair = f.block(8), byte = f.block(8);
    const auto round_trip = [&](std::uint32_t writer, std::uint32_t reader_end, const char* what) {
        require(f.status(sys, "_fileport_makeport", {reader_end, port}) == 0 && f.engine.read32(port) != 0, std::string("fileport_makeport of a ") + what);
        const auto name = f.engine.read32(port);
        for (int copy = 0; copy < 2; ++copy) {   // each fileport_makefd is a new descriptor of the same open file
            const auto fd = static_cast<std::int32_t>(f.call(sys, "_fileport_makefd", {name}));
            require(fd >= 0, std::string("fileport_makefd of a ") + what);
            f.memory().write8(byte, static_cast<std::uint8_t>('a' + copy));
            require(f.call(sys, "_write", {writer, byte, 1}) == 1, std::string("write to the ") + what);
            f.memory().write8(byte, 0);
            require(f.call(sys, "_read", {static_cast<std::uint32_t>(fd), byte, 1}) == 1 && f.memory().read8(byte) == 'a' + copy,
                    std::string("read through the fileport's descriptor of a ") + what);
            f.call(sys, "_close", {static_cast<std::uint32_t>(fd)});
        }
        require(f.status(sys, "_mach_port_deallocate", {f.engine.read32(f.symbol(sys, "_mach_task_self_")), name}) == 0, "release the fileport");
    };
    require(f.call(sys, "_pipe", {pair}) == 0, "pipe");
    round_trip(f.engine.read32(pair + 4), f.engine.read32(pair), "pipe");
    for (const auto offset : {0, 4}) f.call(sys, "_close", {f.engine.read32(pair + offset)});
    require(f.call(sys, "_socketpair", {1, 1, 0, pair}) == 0, "socketpair(AF_UNIX, SOCK_STREAM)");
    round_trip(f.engine.read32(pair), f.engine.read32(pair + 4), "socket");
    for (const auto offset : {0, 4}) f.call(sys, "_close", {f.engine.read32(pair + offset)});
    require(result(f.call(sys, "_fileport_makeport", {9999, port})) == -1 && guest_errno(f) == 9, "fileport_makeport of a bad descriptor: EBADF");
}

// ALCHEMY_NETWORK=loopback (alchemy/host/network_policy.hpp): every path off the
// machine is refused; loopback keeps working. 192.0.2.0/24 (TEST-NET-1) is
// never routed, so nothing would leave even if a gate failed.
Address inet4(Fixture& f, std::array<std::uint8_t, 4> address, std::uint16_t port) {
    const auto value = f.block(16);
    f.memory().write8(value, 16); f.memory().write8(value + 1, 2);
    f.memory().write8(value + 2, static_cast<std::uint8_t>(port >> 8)); f.memory().write8(value + 3, static_cast<std::uint8_t>(port));
    for (unsigned index = 0; index < 4; ++index) f.memory().write8(value + 4 + index, address[index]);
    return value;
}
// inet_addr (inet_aton's grammar) and the services database's network-order ports.
void inet_and_services(Fixture& f) {
    const auto inet_addr = [&](const char* text) { return static_cast<std::uint32_t>(f.call(sys, "_inet_addr", {f.text(text)})); };
    // The result is in network order: 1.2.3.4 is the bytes 01 02 03 04 (0x04030201 little-endian).
    require(inet_addr("1.2.3.4") == 0x04030201U && inet_addr("0.1.2.3") == 0x03020100U, "dotted quads");
    require(inet_addr("127.1") == 0x0100007fU && inet_addr("10.1.2") == 0x0200010aU && inet_addr("16909060") == 0x04030201U, "short forms");
    require(inet_addr("0x7f.0.0.01") == 0x0100007fU && inet_addr("010.0.0.1") == 0x01000008U, "hex and octal parts");
    for (const auto* bad : {"", "1.2.3.4.5", "256.1.1.1", "1.2.3.256", "08.1.1.1", "0x", "1..2", "a.b.c.d", "1.2.3.4x"})
        require(inet_addr(bad) == 0xffffffffU, std::string("INADDR_NONE for ") + bad);
    // servent.s_port (offset 16) is the port in network order, zero-extended.
    const auto https = f.call(sys, "_getservbyname", {f.text("https"), f.text("tcp")});
    require(https && f.engine.read32(https + 16) == 0xbb01U, "getservbyname https: 443 in network order, zero-extended");
    const auto by_port = f.call(sys, "_getservbyport", {0xbb01, f.text("tcp")});
    require(by_port && f.read_text(f.engine.read64(by_port)) == "https", "getservbyport takes the port in network order");
}
void network_loopback_only(Fixture& f) {
    namespace policy = alchemy::network_policy;
    struct Restore { ~Restore() { alchemy::network_policy::set_mode_for_testing(std::nullopt); } } restore;
    policy::set_mode_for_testing(policy::Mode::loopback);
    // The policy's host and address tests.
    for (const auto* host : {"localhost", "LOCALHOST.", "chrome.localhost", "127.0.0.1", "127.12.0.1", "::1", "[::1]", "::ffff:127.0.0.1"})
        require(policy::loopback_host(host), std::string("loopback host ") + host);
    for (const auto* host : {"update.googleapis.com", "192.0.2.1", "::2", "localhostx", "10.0.0.1", "[2001:db8::1]"})
        require(!policy::loopback_host(host), std::string("not a loopback host: ") + host);
    require(policy::url_host("https://user@Update.GoogleAPIs.com:443/service/update2") == "update.googleapis.com" &&
            policy::url_host("http://[::1]:8080/proxy.pac") == "[::1]" && policy::loopback_host(policy::url_host("http://[::1]:8080/proxy.pac")),
            "url_host (the PAC gate's host)");

    // BSD sockets: connect and sendto off the machine fail with ENETUNREACH.
    const auto tcp = f.call(sys, "_socket", {2, 1, 0});
    require(result(f.call(sys, "_connect", {tcp, inet4(f, {192, 0, 2, 1}, 80), 16})) == -1 && guest_errno(f) == 51, "connect to 192.0.2.1: ENETUNREACH");
    f.call(sys, "_close", {tcp});
    const auto listener = f.call(sys, "_socket", {2, 1, 0});
    require(f.call(sys, "_bind", {listener, inet4(f, {127, 0, 0, 1}, 0), 16}) == 0 && f.call(sys, "_listen", {listener, 1}) == 0, "loopback listener");
    const auto bound = f.block(16), bound_length = f.block(4);
    f.engine.write32(bound_length, 16);
    require(f.call(sys, "_getsockname", {listener, bound, bound_length}) == 0, "getsockname");
    const auto client = f.call(sys, "_socket", {2, 1, 0});
    require(f.call(sys, "_connect", {client, bound, 16}) == 0, "connect to 127.0.0.1 is allowed");
    for (const auto descriptor : {client, listener}) f.call(sys, "_close", {descriptor});
    const auto udp = f.call(sys, "_socket", {2, 2, 0}), receiver = f.call(sys, "_socket", {2, 2, 0});
    const auto payload = f.text("ping");
    require(result(f.call(sys, "_sendto", {udp, payload, 4, 0, inet4(f, {192, 0, 2, 1}, 53), 16})) == -1 && guest_errno(f) == 51, "sendto 192.0.2.1: ENETUNREACH");
    require(f.call(sys, "_bind", {receiver, inet4(f, {127, 0, 0, 1}, 0), 16}) == 0, "bind receiver");
    f.engine.write32(bound_length, 16);
    require(f.call(sys, "_getsockname", {receiver, bound, bound_length}) == 0 && f.call(sys, "_sendto", {udp, payload, 4, 0, bound, 16}) == 4,
            "sendto 127.0.0.1 is allowed");
    const auto iov = f.block(16), header = f.block(48);
    f.engine.write64(iov, payload); f.engine.write64(iov + 8, 4);
    f.engine.write64(header, inet4(f, {192, 0, 2, 1}, 53)); f.engine.write32(header + 8, 16);
    f.engine.write64(header + 16, iov); f.engine.write32(header + 24, 1);
    require(result(f.call(sys, "_sendmsg", {udp, header, 0})) == -1 && guest_errno(f) == 51, "sendmsg to 192.0.2.1: ENETUNREACH");
    for (const auto descriptor : {udp, receiver}) f.call(sys, "_close", {descriptor});

    // Name resolution: only localhost (and numeric literals, which need no DNS).
    const auto list = f.block(8);
    require(f.call(sys, "_getaddrinfo", {f.text("update.googleapis.com"), f.text("443"), 0, list}) == 8, "getaddrinfo of a remote name: EAI_NONAME");
    require(f.call(sys, "_getaddrinfo", {f.text("localhost"), 0, 0, list}) == 0, "getaddrinfo localhost");
    f.call(sys, "_freeaddrinfo", {f.engine.read64(list)});
    require(f.call(sys, "_getaddrinfo", {f.text("192.0.2.1"), 0, 0, list}) == 0, "getaddrinfo of a numeric address");
    f.call(sys, "_freeaddrinfo", {f.engine.read64(list)});
    require(f.call(sys, "_gethostbyname", {f.text("update.googleapis.com")}) == 0 && f.engine.read32(f.symbol(sys, "_h_errno")) == 1,
            "gethostbyname of a remote name: HOST_NOT_FOUND");
    const auto host = f.block(256);
    require(f.call(sys, "_getnameinfo", {inet4(f, {192, 0, 2, 1}, 0), 16, host, 256, 0, 0, 4}) == 8, "getnameinfo NI_NAMEREQD: EAI_NONAME");
    require(f.call(sys, "_getnameinfo", {inet4(f, {192, 0, 2, 1}, 0), 16, host, 256, 0, 0, 0}) == 0 && f.read_text(host) == "192.0.2.1",
            "getnameinfo falls back to the numeric form");
    // libresolv sends every query to an external DNS server.
    constexpr std::string_view resolv = "/usr/lib/libresolv.9.dylib";
    const auto state = f.block(552), answer = f.block(512);
    require(f.status(resolv, "_res_9_ninit", {state}) == 0, "res_ninit");
    require(f.status(resolv, "_res_9_nquery", {state, f.text("update.googleapis.com"), 1, 1, answer, 512}) == -1 &&
            f.engine.read32(state + 496) == 1, "res_nquery: HOST_NOT_FOUND");
    // dns_sd: Bonjour and DNSServiceGetAddrInfo.
    const auto reference = f.block(8), callback = f.symbol(sys, "_free");
    require(f.status(sys, "_DNSServiceGetAddrInfo", {reference, 0, 0, 0, f.text("update.googleapis.com"), callback, 0}) == -65538,
            "DNSServiceGetAddrInfo of a remote name: kDNSServiceErr_NoSuchName");
    require(f.status(sys, "_DNSServiceBrowse", {reference, 0, 0, f.text("_http._tcp"), 0, callback, 0}) == -65553,
            "DNSServiceBrowse: kDNSServiceErr_Refused");
    // DNSServiceRegister(sdRef*, flags, interface, name, regtype, domain, host, port, txtLen, txtRecord, callBack, context):
    // txtLen (uint16), txtRecord, callBack and context are packed stack arguments.
    alchemy::CallRegs registration{};
    registration.x = {reference, 0, 0, f.text("alchemy"), f.text("_http._tcp"), 0, 0, 0x5000};
    registration.push_stack(std::uint16_t{0});
    registration.push_stack(Address{0});
    registration.push_stack(callback);
    registration.push_stack(Address{0});
    require(static_cast<std::int32_t>(f.regs(sys, "_DNSServiceRegister", registration).x0) == -65553, "DNSServiceRegister: kDNSServiceErr_Refused");

    // HTTP (NSURLSession, NSURLConnection and CFNetwork share HttpExchange::send).
    using namespace alchemy::gui::network;
    const auto http = HttpClient::create("Alchemy test", ProxyConfiguration{});
    HttpRequest request;
    request.url = *parse_url("https://update.googleapis.com/service/update2/json");
    request.method = "POST";
    request.timeout = 5;
    auto exchange = std::make_shared<HttpExchange>(http, request, [](const auto&) { return true; });
    const auto outcome = exchange->send();
    require(std::holds_alternative<HttpFailure>(outcome) && std::get<HttpFailure>(outcome).code == -1009,
            "WinHTTP to a remote host: NSURLErrorNotConnectedToInternet");
}
// Syscall-style services return x1 = 0, as XNU's syscall return leaves it (the call's second value). Chrome calls
// _sendto as a two-register function on macOS 13.3-26.0 and fails the write with ERR_NETWORK_CHANGED when x1 is
// not 0 (its crbug.com/400642488 workaround); x1 held the buffer argument before. Over a loopback datagram pair.
void syscall_second_return(Fixture& f) {
    const auto sender = f.call(sys, "_socket", {2, 2, 0}), receiver = f.call(sys, "_socket", {2, 2, 0});
    const auto bound = f.block(16), bound_length = f.block(4);
    require(f.call(sys, "_bind", {receiver, inet4(f, {127, 0, 0, 1}, 0), 16}) == 0, "bind receiver");
    f.engine.write32(bound_length, 16);
    require(f.call(sys, "_getsockname", {receiver, bound, bound_length}) == 0, "getsockname");
    const auto payload = f.text("ping");
    alchemy::CallRegs send{};
    send.x = {sender, payload, 4, 0, bound, 16};
    const auto sent = f.regs(sys, "_sendto", send);
    require(sent.complete && sent.x0 == 4 && sent.x1 == 0, "sendto: 4 bytes, x1 = 0 (was the buffer address)");
    alchemy::CallRegs connect{};
    connect.x = {sender, bound, 16};
    const auto connected = f.regs(sys, "_connect", connect);
    require(connected.complete && connected.x0 == 0 && connected.x1 == 0, "connect: x1 = 0");
    alchemy::CallRegs write{};
    write.x = {sender, payload, 4};
    const auto written = f.regs(sys, "_send", write);
    require(written.complete && written.x0 == 4 && written.x1 == 0, "send: x1 = 0");
    // A failure leaves x1 = 0 as well (errno set, x0 = -1).
    alchemy::CallRegs bad{};
    bad.x = {9999, payload, 4, 0, bound, 16};
    const auto failed = f.regs(sys, "_sendto", bad);
    require(failed.complete && result(failed.x0) == -1 && failed.x1 == 0 && guest_errno(f) == 9, "sendto on a bad descriptor: EBADF, x1 = 0");
    for (const auto descriptor : {sender, receiver}) f.call(sys, "_close", {descriptor});
}
// strftime %z and %Z read the Darwin tm's tm_gmtoff and tm_zone. Chrome's tracing service formats a trace's
// time with %z; it used to stop the process.
void strftime_time_zones(Fixture& f) {
    const auto tm = f.block(56);
    const std::int32_t fields[] = {7, 6, 15, 20, 9, 126, 2, 292, 1};   // 2026-10-20 15:06:07, Tuesday, DST
    for (std::size_t at = 0; at < std::size(fields); ++at) f.engine.write32(tm + at * 4, static_cast<std::uint32_t>(fields[at]));
    f.engine.write64(tm + 40, static_cast<std::uint64_t>(std::int64_t{-25200}));
    f.engine.write64(tm + 48, f.text("PDT"));
    const auto out = f.block(64);
    const auto format = [&](const char* specification, std::uint64_t size = 64) {
        const auto written = f.call(sys, "_strftime", {out, size, f.text(specification), tm});
        return written ? f.read_text(out) : std::string("<0>");
    };
    require(format("%Y-%m-%dT%H:%M:%S%z") == "2026-10-20T15:06:07-0700", "%z west of UTC");
    require(format("%Z|%%z|%%Z|%z%%") == "PDT|%z|%Z|-0700%", "%Z, and %% before z and Z stays literal");
    f.engine.write64(tm + 40, static_cast<std::uint64_t>(std::int64_t{19800}));
    f.engine.write64(tm + 48, f.text("I%T"));
    require(format("%z %Z") == "+0530 I%T", "%z east of UTC with minutes; a % in tm_zone is literal");
    f.engine.write64(tm + 48, 0);
    require(format("[%Z]") == "[]", "no tm_zone: %Z is empty");
    f.engine.write64(tm + 40, static_cast<std::uint64_t>(std::int64_t{90000}));
    require(result(f.call(sys, "_strftime", {out, 64, f.text("%z"), tm})) == 0 && guest_errno(f) == 22, "an offset beyond a day: EINVAL");
}
// ALCHEMY_NETWORK_DENY (alchemy/host/network_policy.hpp) with ALCHEMY_NETWORK=full: the listed hosts fail at
// every gate as they do under loopback. Only listed names and TEST-NET addresses are used, so a broken gate
// would not reach a real host either.
void network_deny_list(Fixture& f) {
    namespace policy = alchemy::network_policy;
    struct Restore {
        ~Restore() { policy::set_mode_for_testing(std::nullopt); policy::set_deny_list_for_testing(std::nullopt); }
    } restore;
    policy::set_mode_for_testing(policy::Mode::full);
    policy::set_deny_list_for_testing("update.example, *.Updates.Example. 192.0.2.7,,[2001:DB8::7]");
    // Matching: exact names, subdomains of "*." entries (not the domain itself), case, a trailing dot, literals.
    for (const auto* host : {"update.example", "UPDATE.example.", "a.updates.example", "x.y.updates.example", "192.0.2.7",
                             "2001:db8::7", "[2001:db8:0::7]", "::ffff:192.0.2.7"})
        require(policy::listed_host(host), std::string("listed host ") + host);
    for (const auto* host : {"updates.example", "update.example.com", "xupdate.example", "localhost", "192.0.2.8", "2001:db8::8", ""})
        require(!policy::listed_host(host), std::string("not a listed host: ") + host);
    require(policy::listed_address(*alchemy::host::net::parse_numeric("192.0.2.7", 443)) &&
            policy::listed_address(*alchemy::host::net::parse_numeric("::ffff:192.0.2.7", 80)) &&
            policy::listed_address(*alchemy::host::net::parse_numeric("[2001:db8::7]", 53)) &&
            !policy::listed_address(*alchemy::host::net::parse_numeric("192.0.2.8", 443)) &&
            !policy::listed_address(alchemy::host::net::Address::local("/tmp/s")), "listed addresses, any port");

    // Name gates.
    const auto list = f.block(8);
    require(f.call(sys, "_getaddrinfo", {f.text("a.updates.example"), f.text("443"), 0, list}) == 8, "getaddrinfo of a listed name: EAI_NONAME");
    require(f.call(sys, "_getaddrinfo", {f.text("192.0.2.7"), 0, 0, list}) == 8, "getaddrinfo of a listed literal: EAI_NONAME");
    require(f.call(sys, "_getaddrinfo", {f.text("localhost"), 0, 0, list}) == 0, "getaddrinfo localhost");
    f.call(sys, "_freeaddrinfo", {f.engine.read64(list)});
    require(f.call(sys, "_gethostbyname", {f.text("update.example")}) == 0 && f.engine.read32(f.symbol(sys, "_h_errno")) == 1,
            "gethostbyname of a listed name: HOST_NOT_FOUND");
    constexpr std::string_view resolv = "/usr/lib/libresolv.9.dylib";
    const auto state = f.block(552), answer = f.block(512);
    require(f.status(resolv, "_res_9_ninit", {state}) == 0, "res_ninit");
    require(f.status(resolv, "_res_9_nquery", {state, f.text("update.example"), 1, 1, answer, 512}) == -1 &&
            f.engine.read32(state + 496) == 1, "res_nquery of a listed name: HOST_NOT_FOUND");
    const auto reference = f.block(8), callback = f.symbol(sys, "_free");
    require(f.status(sys, "_DNSServiceGetAddrInfo", {reference, 0, 0, 0, f.text("b.updates.example"), callback, 0}) == -65538,
            "DNSServiceGetAddrInfo of a listed name: kDNSServiceErr_NoSuchName");

    // Address gates: connect, sendto and sendmsg to a listed address fail with ENETUNREACH on any port.
    const auto tcp = f.call(sys, "_socket", {2, 1, 0});
    require(result(f.call(sys, "_connect", {tcp, inet4(f, {192, 0, 2, 7}, 443), 16})) == -1 && guest_errno(f) == 51, "connect to a listed address: ENETUNREACH");
    f.call(sys, "_close", {tcp});
    const auto udp = f.call(sys, "_socket", {2, 2, 0});
    const auto payload = f.text("ping");
    require(result(f.call(sys, "_sendto", {udp, payload, 4, 0, inet4(f, {192, 0, 2, 7}, 53), 16})) == -1 && guest_errno(f) == 51, "sendto a listed address: ENETUNREACH");
    const auto iov = f.block(16), header = f.block(48);
    f.engine.write64(iov, payload); f.engine.write64(iov + 8, 4);
    f.engine.write64(header, inet4(f, {192, 0, 2, 7}, 9)); f.engine.write32(header + 8, 16);
    f.engine.write64(header + 16, iov); f.engine.write32(header + 24, 1);
    require(result(f.call(sys, "_sendmsg", {udp, header, 0})) == -1 && guest_errno(f) == 51, "sendmsg to a listed address: ENETUNREACH");
    f.call(sys, "_close", {udp});

    // HTTP: NSURLSession, NSURLConnection and CFNetwork share HttpExchange::send.
    using namespace alchemy::gui::network;
    const auto http = HttpClient::create("Alchemy test", ProxyConfiguration{});
    HttpRequest request;
    request.url = *parse_url("https://c.updates.example/service/update2/json");
    request.method = "POST";
    request.timeout = 5;
    auto exchange = std::make_shared<HttpExchange>(http, request, [](const auto&) { return true; });
    const auto outcome = exchange->send();
    require(std::holds_alternative<HttpFailure>(outcome) && std::get<HttpFailure>(outcome).code == -1009,
            "WinHTTP to a listed host: NSURLErrorNotConnectedToInternet");

    // Without a list nothing is listed.
    policy::set_deny_list_for_testing("");
    require(!policy::listed_host("update.example") && !policy::listed_address(*alchemy::host::net::parse_numeric("192.0.2.7")), "an empty list");
}

// Extended attributes over NTFS alternate data streams (crashpad marks its database this way).
void extended_attributes(Fixture& f) {
    const auto directory = scratch_directory(f, "xattr");
    const auto path = f.text(directory + "/settings.dat");
    alchemy::CallRegs open_registers{};
    open_registers.x[0] = path; open_registers.x[1] = 0x0202;   // O_RDWR | O_CREAT
    open_registers.push_variadic(std::uint64_t{0644});
    const auto descriptor = f.regs(sys, "_open", open_registers).x0;
    require(result(descriptor) >= 0, "open");
    const auto name = f.text("org.chromium.crashpad.database.initialized"), value = f.text("1");
    const auto set_status = f.call(sys, "_setxattr", {path, name, value, 1, 0, 0});
    require(set_status == 0, "setxattr (" + std::to_string(result(set_status)) + ", errno " + std::to_string(guest_errno(f)) + ")");
    require(f.call(sys, "_getxattr", {path, name, 0, 0, 0, 0}) == 1, "getxattr size");
    const auto buffer = f.block(64);
    require(f.call(sys, "_fgetxattr", {descriptor, name, buffer, 64, 0, 0}) == 1 && f.memory().read8(buffer) == '1', "fgetxattr value");
    require(result(f.call(sys, "_setxattr", {path, name, value, 1, 0, 2})) == -1 && guest_errno(f) == 17, "XATTR_CREATE on an existing attribute");
    const auto list = f.block(256);
    require(f.call(sys, "_listxattr", {path, list, 256, 0}) == 43 && f.read_text(list) == "org.chromium.crashpad.database.initialized", "listxattr");
    require(f.call(sys, "_removexattr", {path, name, 0}) == 0, "removexattr");
    require(result(f.call(sys, "_getxattr", {path, name, buffer, 64, 0, 0})) == -1 && guest_errno(f) == 93, "missing attribute: ENOATTR");
    require(f.call(sys, "_listxattr", {path, 0, 0, 0}) == 0, "no attributes left");
    f.call(sys, "_close", {descriptor});
    static_cast<void>(f.call(sys, "_unlink", {path}));
    static_cast<void>(f.call(sys, "_rmdir", {f.text(directory)}));
}

// fsync and F_FULLFSYNC of a directory descriptor (LevelDB's SyncParent).
void directory_fsync(Fixture& f) {
    const auto directory = scratch_directory(f, "dirsync");
    const auto descriptor = f.call(sys, "_open", {f.text(directory), 0});
    require(result(descriptor) >= 0, "open a directory read-only");
    require(f.call(sys, "_fsync", {descriptor}) == 0, "fsync(directory) (errno " + std::to_string(guest_errno(f)) + ")");
    alchemy::CallRegs registers{};
    registers.x[0] = descriptor; registers.x[1] = 51;   // F_FULLFSYNC
    registers.push_variadic(std::uint64_t{0});
    require(f.regs(sys, "_fcntl", registers).x0 == 0, "F_FULLFSYNC(directory)");
    f.call(sys, "_close", {descriptor});
    static_cast<void>(f.call(sys, "_rmdir", {f.text(directory)}));
}

// errno of the host-refused cases (docs/ALCHEMY_REFACTOR_FOLLOWUPS.md, "errno
// residuals"): fcopyfile that must shrink a MAP_SHARED destination answers
// EBUSY, as ftruncate does.
void errno_residuals(Fixture& f) {
    const auto directory = scratch_directory(f, "errno");
    const auto source_path = f.text(directory + "/source"), target_path = f.text(directory + "/target");
    const auto create = [&](Address path) {
        alchemy::CallRegs registers{};
        registers.x[0] = path; registers.x[1] = 0x0602;   // O_RDWR | O_CREAT | O_TRUNC
        registers.push_variadic(std::uint64_t{0644});
        return f.regs(sys, "_open", registers).x0;
    };
    const auto source = create(source_path), target = create(target_path);
    require(result(source) >= 0 && result(target) >= 0, "open the copy's ends");
    const auto data = f.block(65536, 0x5a);
    require(f.call(sys, "_write", {source, data, 16}) == 16 && f.call(sys, "_write", {target, data, 65536}) == 65536, "fill both files");
    const auto view = f.call(sys, "_mmap", {0, 65536, 3, 1, target, 0});   // PROT_READ | PROT_WRITE, MAP_SHARED
    require(result(view) != -1, "map the destination shared");
    require(result(f.call(sys, "_fcopyfile", {source, target, 0, 8})) == -1 && guest_errno(f) == 16,
            "fcopyfile shrinking a mapped destination: EBUSY (errno " + std::to_string(guest_errno(f)) + ")");
    require(f.call(sys, "_munmap", {view, 65536}) == 0, "unmap the destination");
    require(f.call(sys, "_fcopyfile", {source, target, 0, 8}) == 0, "fcopyfile once the destination is unmapped");
    f.call(sys, "_close", {source});
    f.call(sys, "_close", {target});
    static_cast<void>(f.call(sys, "_unlink", {source_path}));
    static_cast<void>(f.call(sys, "_unlink", {target_path}));
    static_cast<void>(f.call(sys, "_rmdir", {f.text(directory)}));
}

// confstr's _CS_DARWIN_USER_DIR/_TEMP_DIR/_CACHE_DIR (65536..65538): Chrome's
// sandbox parameters PCHECK them when launching any child (EINVAL before).
void darwin_user_directories(Fixture& f) {
    const auto buffer = f.block(1024);
    require(f.call(sys, "_confstr", {65537, buffer, 1024}) > 2, "confstr(_CS_DARWIN_USER_TEMP_DIR) (errno " + std::to_string(guest_errno(f)) + ")");
    const auto temp = f.read_text(buffer);
    require(temp.starts_with("/") && temp.ends_with("/"), "an absolute directory with a trailing slash: " + temp);
    if (const auto tmpdir = f.call(sys, "_getenv", {f.text("TMPDIR")})) {
        auto expected = f.read_text(tmpdir);
        if (!expected.ends_with("/")) expected += '/';
        require(temp == expected, "the temporary directory is TMPDIR");
    }
    for (const std::uint64_t name : {65536U, 65538U}) {
        require(f.call(sys, "_confstr", {name, buffer, 1024}) == temp.size() + 1 && f.read_text(buffer) == temp,
                "user and cache directories: " + std::to_string(name));
    }
    const auto info = f.block(144);
    require(f.call(sys, "_stat", {f.text(temp), info}) == 0, "the directory exists");
    // Truncation keeps the full length as the answer; a null buffer asks the size.
    require(f.call(sys, "_confstr", {65537, buffer, 3}) == temp.size() + 1 && f.read_text(buffer) == temp.substr(0, 2), "truncated");
    require(f.call(sys, "_confstr", {65537, 0, 0}) == temp.size() + 1, "size query");
    require(f.call(sys, "_confstr", {65539, buffer, 1024}) == 0 && guest_errno(f) == 22, "an unknown name: EINVAL");
    // _dirhelper(DIRHELPER_USER_LOCAL_TEMP) answers the same directory.
    require(f.call(sys, "__dirhelper", {1, buffer, 1024}) == buffer && f.read_text(buffer) == temp, "_dirhelper temp");
    // tmpfile creates its anonymous file in the same directory.
    const auto stream = f.call(sys, "_tmpfile", {});
    require(stream != 0, "tmpfile (errno " + std::to_string(guest_errno(f)) + ")");
    require(f.call(sys, "_fwrite", {f.text("x"), 1, 1, stream}) == 1 && f.call(sys, "_fclose", {stream}) == 0, "write and close it");
}

// strnlen's maxlen only bounds the scan: strnlen(p, SIZE_MAX) is strlen(p)
// (Dawn measures null-terminated WGPUStringView labels this way; EINVAL
// and -1 before, which ended Chrome's GPU process in a release check).
// strlcat with a huge size the same.
void unbounded_string_lengths(Fixture& f) {
    const auto label = f.text("Skia_TransferForStaticBuffer");
    require(f.call(sys, "_strnlen", {label, ~0ULL}) == 28, "strnlen(p, SIZE_MAX) == strlen(p) (errno " + std::to_string(guest_errno(f)) + ")");
    require(f.call(sys, "_strnlen", {label, 1ULL << 40}) == 28, "strnlen with a bound past the checked limit");
    require(f.call(sys, "_strnlen", {label, 4}) == 4 && f.call(sys, "_strnlen", {label, 0}) == 0, "a bound below the length");
    require(f.call(sys, "_strnlen", {label, 28}) == 28 && f.call(sys, "_strnlen", {label, 29}) == 28, "a bound at the length");
    const auto target = f.block(64);
    f.engine.write_bytes(target, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>("ab"), 3));
    require(f.call(sys, "_strlcat", {target, f.text("cd"), ~0ULL}) == 4 && f.read_text(target) == "abcd", "strlcat with size SIZE_MAX");
    // A string whose NUL is the last byte of a page followed by an
    // inaccessible page: a huge bound reads nothing past the NUL.
    constexpr std::uint64_t page = 16384;
    const auto pages = f.call(sys, "_mmap", {0, 2 * page, 3, 0x1002, ~0ULL, 0});   // PROT_READ|WRITE, MAP_PRIVATE|MAP_ANON
    require(result(pages) != -1 && f.call(sys, "_mprotect", {pages + page, page, 0}) == 0, "a page followed by PROT_NONE");
    const std::vector<std::uint8_t> tail(9, 'x');
    f.engine.write_bytes(pages + page - 10, tail);
    f.engine.write_bytes(pages + page - 1, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(""), 1));
    require(f.call(sys, "_strnlen", {pages + page - 10, ~0ULL}) == 9, "a string ending at a page boundary");
    // No NUL before the boundary, bound exactly to it: the bound wins.
    f.engine.write_bytes(pages + page - 10, std::vector<std::uint8_t>(10, 'y'));
    require(f.call(sys, "_strnlen", {pages + page - 10, 10}) == 10, "a bound that ends at the boundary");
    static_cast<void>(f.call(sys, "_munmap", {pages, 2 * page}));
}

// TCP_KEEPALIVE, TCP_KEEPINTVL and TCP_KEEPCNT map to the Windows options.
void tcp_keepalive(Fixture& f) {
    const auto socket = f.call(sys, "_socket", {2, 1, 0});
    require(result(socket) >= 0, "socket");
    const auto value = f.block(4), size = f.block(4);
    for (const auto& [option, wanted] : std::array<std::pair<Address, std::uint32_t>, 3>{{{0x10, 45}, {0x101, 7}, {0x102, 4}}}) {
        f.engine.write32(value, wanted);
        require(f.call(sys, "_setsockopt", {socket, 6, option, value, 4}) == 0,
            "setsockopt IPPROTO_TCP " + std::to_string(option) + " (errno " + std::to_string(guest_errno(f)) + ")");
        f.engine.write32(value, 0); f.engine.write32(size, 4);
        require(f.call(sys, "_getsockopt", {socket, 6, option, value, size}) == 0 && f.engine.read32(value) == wanted,
            "getsockopt IPPROTO_TCP " + std::to_string(option) + " = " + std::to_string(f.engine.read32(value)));
    }
    f.engine.write32(value, 1);
    require(result(f.call(sys, "_setsockopt", {socket, 6, 0x201, value, 4})) == -1 && guest_errno(f) == 42, "TCP_NOTSENT_LOWAT: ENOPROTOOPT");
    f.call(sys, "_close", {socket});
}

// The process tree (alchemy/process_tree.hpp): this process's record, and a
// second process (this test program as a helper) found by pid, watched and
// seen to end.
void process_tree_api(Fixture& f) {
    namespace tree = alchemy::process_tree;
    const auto own_pid = static_cast<std::uint32_t>(f.call(sys, "_getpid", {}));
    const auto own = tree::self();
    require(own && own->pid == own_pid && own->pidversion != 0, "DarwinServices published this process");
    require(tree::lookup(own_pid) && tree::pidversion(own_pid) == own->pidversion, "lookup of this process");
    require(tree::alive(own_pid, own->pidversion) && !tree::alive(own_pid, own->pidversion ^ 1), "alive with the generation");
    require(!tree::lookup(4) && !tree::alive(4), "a host process outside the tree is not found");
    // The helper: publishes a record and lives until its input closes.
    wchar_t program[MAX_PATH]{};
    GetModuleFileNameW(nullptr, program, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(program) + L"\" --process-tree-helper";
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE input_read{}, input_write{}, output_read{}, output_write{};
    require(CreatePipe(&input_read, &input_write, &inherit, 0) && CreatePipe(&output_read, &output_write, &inherit, 0), "pipes");
    SetHandleInformation(input_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input_read; startup.hStdOutput = output_write; startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION helper{};
    require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &helper), "start the helper");
    CloseHandle(input_read); CloseHandle(output_write);
    char ready[16]{}; DWORD got = 0;
    require(ReadFile(output_read, ready, 6, &got, nullptr) && got == 6 && std::string_view(ready, 5) == "ready", "helper ready");
    const auto pid = static_cast<std::uint32_t>(helper.dwProcessId);
    const auto record = tree::lookup(pid);
    require(record && record->path == "/Applications/Helper.app/Contents/MacOS/Helper" && record->name == "Helper" && record->ppid == own_pid &&
        record->uid == 501 && record->start_seconds > 1600000000ULL, "lookup of the helper");
    const auto version = record->pidversion;
    require(version && version != own->pidversion && tree::pidversion(pid) == version, "the helper's generation");
    // Watches: one fires at exit, one is cancelled, one has a stale generation.
    std::mutex mutex; std::condition_variable signal;
    int fired = 0, cancelled_fired = 0, stale = 0; std::uint32_t fired_pid = 0;
    const auto token = tree::watch_exit(pid, version, [&](std::uint32_t value) {
        std::lock_guard lock(mutex); ++fired; fired_pid = value; signal.notify_all();
    });
    const auto cancelled = tree::watch_exit(pid, 0, [&](std::uint32_t) { std::lock_guard lock(mutex); ++cancelled_fired; });
    require(token && cancelled && token != cancelled, "watch_exit tokens");
    require(tree::watch_exit(pid, version ^ 0x40, [&](std::uint32_t) { ++stale; }) == 0 && stale == 1, "a stale generation fires at once");
    require(tree::unwatch_exit(cancelled) && !tree::unwatch_exit(cancelled), "unwatch_exit");
    CloseHandle(input_write);   // the helper ends
    {
        std::unique_lock lock(mutex);
        require(signal.wait_for(lock, std::chrono::seconds(20), [&] { return fired != 0; }), "the exit watch fired");
    }
    WaitForSingleObject(helper.hProcess, 10000);
    require(fired == 1 && fired_pid == pid && cancelled_fired == 0, "fired once; the cancelled watch never");
    require(!tree::unwatch_exit(token), "unwatch after firing");
    require(!tree::lookup(pid) && !tree::alive(pid) && tree::pidversion(pid) == 0, "the helper is gone");
    int late = 0;
    require(tree::watch_exit(pid, 0, [&](std::uint32_t) { ++late; }) == 0 && late == 1, "watching a gone process fires at once");
    CloseHandle(output_read); CloseHandle(helper.hThread); CloseHandle(helper.hProcess);
}

int process_tree_helper() {
    alchemy::process_tree::Record record;
    record.ppid = GetCurrentProcessId();
    record.uid = 501; record.gid = 20;
    record.path = "/Applications/Helper.app/Contents/MacOS/Helper";
    // The parent is the test process.
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    for (auto ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry))
        if (entry.th32ProcessID == GetCurrentProcessId()) { record.ppid = entry.th32ParentProcessID; break; }
    CloseHandle(snapshot);
    static_cast<void>(alchemy::process_tree::publish(record));
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "ready\n", 6, &written, nullptr);
    char buffer[64]; DWORD got = 0;
    while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer), &got, nullptr) && got) {}
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--process-tree-helper") return process_tree_helper();
    return run("chrome_libsystem", {
        {"system_zone", system_zone},
        {"guest_default_zone", guest_default_zone},
        {"mstats", mstats},
        {"getopt_long", getopt_long},
        {"getopt_long_only", getopt_long_only},
        {"err_hooks", err_hooks},
        {"byte_swap", byte_swap},
        {"bz2_stream", bz2_stream},
        {"misc", misc},
        {"pipe_fionbio", pipe_fionbio},
        {"sendfile_and_unix_eof", sendfile_and_unix_eof},
        {"unix_sockets", unix_sockets},
        {"symlink_records", symlink_records},
        {"extended_attributes", extended_attributes},
        {"kevent_timers", kevent_timers},
        {"struct_layouts", struct_layouts},
        {"vm_map_memory_entry", vm_map_memory_entry},
        {"fileports", fileports},
        {"inet_and_services", inet_and_services},
        {"network_loopback_only", network_loopback_only},
        {"network_deny_list", network_deny_list},
        {"syscall_second_return", syscall_second_return},
        {"strftime_time_zones", strftime_time_zones},
        {"tcp_keepalive", tcp_keepalive},
        {"directory_fsync", directory_fsync},
        {"errno_residuals", errno_residuals},
        {"darwin_user_directories", darwin_user_directories},
        {"unbounded_string_lengths", unbounded_string_lengths},
        {"process_tree_api", process_tree_api},
    });
}
