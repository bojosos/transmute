#include "alchemy/darwin_services.hpp"
#include "alchemy/process.hpp"
#include "alchemy/path.hpp"

#include <array>
#include <bit>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr std::string_view system_library = "/usr/lib/libSystem.B.dylib";
void require(bool value, std::string_view description) {
    if (!value) throw std::runtime_error(std::string(description));
}
void put(transmute::GuestMemory& memory, std::uint64_t address, std::string_view value) {
    memory.write_bytes(address, std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
    memory.write8(address + value.size(), 0);
}
std::string get(const transmute::GuestMemory& memory, std::uint64_t address) {
    const auto bytes = memory.read_string(address, 1, 4096);
    return std::string(bytes.begin(), bytes.end());
}
std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}
struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    std::string output;
    alchemy::ProcessContext context{engine, [this](unsigned descriptor, auto bytes) {
        require(descriptor == 1 || descriptor == 2, "wrong output descriptor");
        output.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return alchemy::OutputResult{bytes.size(), 0};
    }};
    alchemy::DarwinServices services{context};
    Fixture() { context.working_directory = std::filesystem::current_path(); }
    std::uint64_t call(std::string_view name, std::initializer_list<std::uint64_t> args) {
        const auto symbol = context.resolve(system_library, name);
        require(symbol.has_value(), "required service symbol missing");
        return engine.invoke_callback(symbol->address, std::span(args.begin(), args.size()));
    }
    std::uint64_t string(std::string_view value) { const auto address = context.allocate_guest(value.size() + 1); put(engine, address, value); return address; }
    void slot(unsigned index, std::uint64_t value) { engine.write64(((engine.state().sp - 256) & ~15ULL) + index * 8, value); }
};

void memory_and_format() {
    Fixture f;
    const auto memory = f.call("_calloc", {8, 8});
    for (unsigned at = 0; at < 64; ++at) require(static_cast<transmute::GuestMemory&>(f.engine).read8(memory + at) == 0, "calloc failed to zero");
    f.engine.write64(memory, 0x1020304050607080ULL);
    const auto enlarged = f.call("_realloc", {memory, 128});
    require(f.engine.read64(enlarged) == 0x1020304050607080ULL, "realloc lost prefix");
    require(!f.engine.query_region(memory), "realloc did not release old storage");
    const auto cell = f.context.allocate_guest(8);
    require(f.call("_posix_memalign", {cell, 256, 64}) == 0 && f.engine.read64(cell) % 256 == 0, "aligned allocation failed");
    const auto aligned = f.engine.read64(cell);
    require(f.call("_posix_memalign", {cell, 3, 64}) == 22 && f.engine.read64(cell) == aligned, "invalid alignment changed result");
    require(f.call("_calloc", {UINT64_MAX, 2}) == 0, "calloc overflow accepted");
    const auto errno_address = f.call("___error", {});
    require(f.engine.read32(errno_address) == 12, "calloc errno wrong");
    const auto message = f.string("%s:%04d:%lx:%.2f:%%");
    f.slot(0, f.string("guest")); f.slot(1, 27); f.slot(2, 0xabc); f.slot(3, std::bit_cast<std::uint64_t>(1.25));
    const auto destination = f.context.allocate_guest(128);
    require(f.call("_snprintf", {destination, 128, message}) == 21, "snprintf return wrong");
    require(get(f.engine, destination) == "guest:0027:abc:1.25:%", "format slots/numeric types wrong");
    f.slot(0, 12345);
    require(f.call("_snprintf", {destination, 3, f.string("%d")}) == 5 && get(f.engine, destination) == "12", "snprintf truncation wrong");
    bool rejected = false;
    try { static_cast<void>(f.call("_printf", {f.string("before %q")})); }
    catch (const std::runtime_error& error) { rejected = std::string_view(error.what()).find("unsupported") != std::string_view::npos; }
    require(rejected && f.output.empty(), "unsupported format produced output");
    static_cast<void>(f.call("_free", {enlarged})); static_cast<void>(f.call("_free", {aligned}));
    require(!f.engine.query_region(enlarged), "free left memory accessible");
}

void files_stdio_syscalls() {
    Fixture f;
    const auto filename = std::filesystem::temp_directory_path() /
        ("alchemy-services-" + std::to_string(f.call("_getpid", {})) + ".bin");
    const auto pathname = f.string(utf8(filename));
    f.slot(0, 0600);
    const auto descriptor = f.call("_open", {pathname, 2 | 0x200 | 0x400});
    require(descriptor >= 3 && descriptor != UINT64_MAX, "open failed");
    const std::array<std::uint8_t, 5> payload{0, 1, 2, 255, 27};
    const auto bytes = f.context.allocate_guest(64);
    f.engine.write_bytes(bytes, payload);
    require(f.call("_write", {descriptor, bytes, payload.size()}) == payload.size(), "binary write failed");
    require(f.call("_lseek", {descriptor, 0, 0}) == 0, "seek failed");
    const auto copied = f.context.allocate_guest(64);
    require(f.call("_read", {descriptor, copied, 64}) == payload.size(), "read byte count wrong");
    for (unsigned at = 0; at < payload.size(); ++at) require(static_cast<transmute::GuestMemory&>(f.engine).read8(copied + at) == payload[at], "binary read changed bytes");
    const auto metadata = f.context.allocate_guest(144);
    require(f.call("_fstat", {descriptor, metadata}) == 0 && f.engine.read64(metadata + 96) == payload.size(), "Darwin stat layout wrong");
    const auto mapped_file = f.call("_mmap", {0, 1, 3, 2, descriptor, 0});
    require(mapped_file != UINT64_MAX && f.engine.query_region(mapped_file)->size == alchemy::darwin_arm64_page_size &&
        static_cast<transmute::GuestMemory&>(f.engine).read8(mapped_file + 4) == 27 &&
        static_cast<transmute::GuestMemory&>(f.engine).read8(mapped_file + 5) == 0,
        "private file mapping lost rounded page contents/zero-fill");
    static_cast<transmute::GuestMemory&>(f.engine).write8(mapped_file + 4, 99);
    require(f.call("_lseek", {descriptor, 0, 1}) == payload.size(), "mmap changed descriptor offset");
    require(f.call("_munmap", {mapped_file, 1}) == 0 &&
        f.call("_mmap", {0, 1, 1, 2, descriptor, 4096}) == UINT64_MAX &&
        f.engine.read32(f.call("___error", {})) == 22, "file mmap accepted 4 KiB offset");
    require(f.call("_lseek", {descriptor, 4, 0}) == 4 && f.call("_read", {descriptor, copied, 1}) == 1 &&
        static_cast<transmute::GuestMemory&>(f.engine).read8(copied) == 27,
        "private mapping wrote into original file");
    const auto alias = filename.parent_path() / (filename.filename().string() + ".hardlink");
    const auto alias_pathname = f.string(utf8(alias));
    require(f.call("_link", {pathname, alias_pathname}) == 0, "hardlink creation failed");
    const auto alias_metadata = f.context.allocate_guest(144);
    require(f.call("_stat", {alias_pathname, alias_metadata}) == 0 &&
        f.call("_fstat", {descriptor, metadata}) == 0, "file identity metadata failed");
    require(f.engine.read64(metadata + 8) != 0 &&
        f.engine.read64(metadata + 8) == f.engine.read64(alias_metadata + 8) &&
        f.engine.read32(metadata) == f.engine.read32(alias_metadata),
        "hardlinks do not share stable Darwin file identity");
    require(std::filesystem::equivalent(filename, alias) &&
        f.engine.read64(alias_metadata + 96) == std::filesystem::file_size(alias),
        "file_size/equivalent differ from native filesystem");
    require(((f.engine.read32(metadata + 4) & 0xffff) & 0170000) == 0100000 &&
        (f.engine.read32(metadata + 4) >> 16) >= 2, "regular file kind/link count wrong");
    for (const std::uint64_t offset : {32ULL, 48ULL, 64ULL, 80ULL})
        require(f.engine.read64(metadata + offset + 8) < 1000000000, "file timestamp nanoseconds invalid");
#ifdef _WIN32
    require(f.engine.read64(metadata + 80) > 0, "file birth timestamp missing");
#endif
    require(f.call("_stat", {f.string(utf8(filename.parent_path())), alias_metadata}) == 0 &&
        (f.engine.read32(alias_metadata + 4) & 0170000) == 0040000, "directory classified as regular file");
    const auto symbolic = filename.parent_path() / (filename.filename().string() + ".symlink");
    std::error_code symlink_error;
    std::filesystem::create_symlink(filename, symbolic, symlink_error);
    if (!symlink_error) {
        const auto symbolic_pathname = f.string(utf8(symbolic));
        require(f.call("_stat", {symbolic_pathname, alias_metadata}) == 0 &&
            (f.engine.read32(alias_metadata + 4) & 0170000) == 0100000 &&
            f.engine.read64(alias_metadata + 8) == f.engine.read64(metadata + 8),
            "stat failed to follow symbolic link");
        require(f.call("_lstat", {symbolic_pathname, alias_metadata}) == 0 &&
            (f.engine.read32(alias_metadata + 4) & 0170000) == 0120000 &&
            f.engine.read64(alias_metadata + 8) != f.engine.read64(metadata + 8),
            "lstat did not preserve symbolic link identity/kind");
        require(f.call("_unlink", {symbolic_pathname}) == 0, "symbolic link cleanup failed");
    } else {
        bool permission_or_unsupported = symlink_error == std::errc::operation_not_permitted ||
            symlink_error == std::errc::permission_denied || symlink_error == std::errc::not_supported;
#ifdef _WIN32
        // ERROR_PRIVILEGE_NOT_HELD is not mapped to std::errc by MSVC STL.
        permission_or_unsupported = permission_or_unsupported ||
            (symlink_error.category() == std::system_category() && symlink_error.value() == 1314);
        // Without the privilege the link is a symlink record (no Alchemy root here):
        // readlink and lstat see the link, unlink removes it.
        if (symlink_error.category() == std::system_category() && symlink_error.value() == 1314) {
            const auto link = f.string(utf8(symbolic)), text = f.context.allocate_guest(4096), info = f.context.allocate_guest(144);
            require(f.call("_symlink", {pathname, link}) == 0, "symbolic link without host privilege did not become a record");
            require(f.call("_readlink", {link, text, 4096}) == utf8(filename).size(), "readlink of a symlink record");
            require(f.call("_lstat", {link, info}) == 0 && (f.engine.read32(info + 4) & 0170000) == 0120000, "lstat of a symlink record");
            require(f.call("_unlink", {link}) == 0, "unlink of a symlink record");
        }
#endif
        require(permission_or_unsupported, "unexpected native symbolic link creation failure: " +
            std::to_string(symlink_error.value()) + " " + symlink_error.message());
    }
    require(f.call("_unlink", {alias_pathname}) == 0, "hardlink cleanup failed");
    transmute::CpuState state{};
    state.x[16] = 199; state.x[0] = descriptor; state.x[1] = 0; state.x[2] = 0;
    static_cast<void>(f.services.supervisor_call({0, 0x80}, state, f.engine));
    require(state.x[0] == 0 && !(state.nzcv & (1U << 29)), "direct seek syscall failed");
    state.x[16] = 3; state.x[0] = descriptor; state.x[1] = 0; state.x[2] = 1;
    static_cast<void>(f.services.supervisor_call({0, 0x80}, state, f.engine));
    require(state.x[0] == 14 && (state.nzcv & (1U << 29)), "direct invalid-buffer error wrong");
    require(f.call("_close", {descriptor}) == 0, "close failed");
    require(f.call("_read", {descriptor, copied, 1}) == UINT64_MAX && f.engine.read32(f.call("___error", {})) == 9, "closed fd remained valid");
    const auto stream = f.call("_fopen", {pathname, f.string("rb")});
    require(stream && f.call("_fread", {copied, 1, 64, stream}) == 5 && f.call("_feof", {stream}) == 1, "stdio EOF wrong");
    require(f.call("_fputc", {'Q', stream}) == UINT64_MAX && f.call("_ferror", {stream}) == 1,
        "failed stream output did not set error indicator");
    require(f.call("_clearerr", {stream}) == 0 && f.call("_ferror", {stream}) == 0 &&
        f.call("_feof", {stream}) == 0, "clearerr did not clear stream indicators");
    require(f.call("_fseek", {stream, 0, 0}) == 0 && f.call("_feof", {stream}) == 0, "stdio seek did not clear EOF");
    require(f.call("_ungetc", {'Q', stream}) == 'Q' && f.call("_getc", {stream}) == 'Q' &&
        f.call("_getc", {stream}) == 0, "stdio pushback lost a character");
    require(f.call("_fclose", {stream}) == 0 && f.call("_unlink", {pathname}) == 0, "stdio/file cleanup failed");
}

void environment_clock_mapping() {
    Fixture f;
    const auto name = f.string("ALCHEMY_TEST"), value = f.string("value");
    require(f.call("_setenv", {name, value, 1}) == 0 && get(f.engine, f.call("_getenv", {name})) == "value", "virtual environment failed");
    const auto environment_export = f.context.resolve(system_library, "_environ");
    require(environment_export && get(f.engine, f.engine.read64(f.engine.read64(environment_export->address))) == "ALCHEMY_TEST=value", "environ data export wrong");
    require(f.call("_unsetenv", {name}) == 0 && f.call("_getenv", {name}) == 0, "unsetenv failed");
    const auto timestamp = f.context.allocate_guest(16);
    require(f.call("_clock_gettime", {6, timestamp}) == 0 && f.engine.read64(timestamp + 8) < 1000000000, "monotonic clock malformed");
    const auto monotonic = f.engine.read64(timestamp) * 1000000000 + f.engine.read64(timestamp + 8);
    require(f.call("_clock_gettime_nsec_np", {6}) >= monotonic, "monotonic clock moved backwards");
    const auto uptime = f.call("_clock_gettime_nsec_np", {8});
    require(uptime > 0 && f.call("_clock_gettime_nsec_np", {4}) >= uptime,
        "continuous and uptime-raw clock profiles inconsistent");
    require(f.call("_clock_gettime", {UINT64_MAX, timestamp}) == UINT64_MAX &&
        f.engine.read32(f.call("___error", {})) == 22, "unsupported clock did not return EINVAL");
    // gettimeofday(&tv, &tz) fills both; struct timezone is {minuteswest, dsttime}.
    const auto timeval = f.context.allocate_guest(16), zone = f.context.allocate_guest(8), calendar = f.context.allocate_guest(64);
    f.engine.fill_bytes(timeval, 0xcc, 16); f.engine.fill_bytes(zone, 0xcc, 8);
    const auto wall = f.call("_time", {0});
    require(f.call("_gettimeofday", {timeval, zone}) == 0, "gettimeofday with timezone failed");
    const auto seconds = f.engine.read64(timeval);
    require(seconds + 2 >= wall && seconds <= wall + 2 && f.engine.read32(timeval + 8) < 1000000 &&
        f.engine.read32(timeval + 12) == 0, "gettimeofday timeval with timezone malformed");
    const auto west = std::bit_cast<std::int32_t>(f.engine.read32(zone));
    const auto dst = f.engine.read32(zone + 4);
    require(west >= -24 * 60 && west <= 24 * 60 && dst <= 1, "gettimeofday timezone out of range");
    f.engine.write64(timestamp, seconds);
    require(f.call("_localtime_r", {timestamp, calendar}) == calendar &&
        (f.engine.read32(calendar + 32) != 0) == (dst == 1) &&
        std::bit_cast<std::int64_t>(f.engine.read64(calendar + 40)) == -60LL * (west - (dst ? 60 : 0)),
        "gettimeofday timezone disagrees with localtime_r");
    require(f.call("_gettimeofday", {0, zone}) == 0 && f.call("_gettimeofday", {0, 0}) == 0, "gettimeofday optional arguments");
    const auto tz_name = f.string("TZ"), utc_value = f.string("UTC");
    require(f.call("_setenv", {tz_name, utc_value, 1}) == 0 && f.call("_gettimeofday", {timeval, zone}) == 0 &&
        f.engine.read32(zone) == 0 && f.engine.read32(zone + 4) == 0, "gettimeofday TZ=UTC zone");
    require(f.call("_unsetenv", {tz_name}) == 0, "TZ cleanup");
    constexpr auto page_size = alchemy::darwin_arm64_page_size;
    require(page_size == 16384 && f.call("_getpagesize", {}) == page_size &&
        f.call("_sysconf", {29}) == page_size, "ARM64 page queries inconsistent");
    const auto length = f.context.allocate_guest(8), query = f.context.allocate_guest(8);
    f.engine.write64(length, 8);
    require(f.call("_sysctlbyname", {f.string("hw.pagesize"), query, length, 0, 0}) == 0 &&
        f.engine.read64(query) == page_size && f.engine.read64(length) == 8,
        "named page query wrong size/value");
    const auto mib = f.context.allocate_guest(8);
    f.engine.write32(mib, 6); f.engine.write32(mib + 4, 7); f.engine.write64(length, 4);
    require(f.call("_sysctl", {mib, 2, query, length, 0, 0}) == 0 &&
        f.engine.read32(query) == page_size && f.engine.read64(length) == 4,
        "numeric page query wrong size/value");
    f.engine.write64(length, 1);
    require(f.call("_sysctlbyname", {f.string("hw.pagesize"), query, length, 0, 0}) == UINT64_MAX &&
        f.engine.read64(length) == 8 && f.engine.read32(f.call("___error", {})) == 12,
        "short sysctl buffer did not return Darwin ENOMEM");
    transmute::CpuState state{};
    f.engine.write64(length, 4);
    state.x[16] = 202; state.x[0] = mib; state.x[1] = 2; state.x[2] = query; state.x[3] = length;
    static_cast<void>(f.services.supervisor_call({0, 0x80}, state, f.engine));
    require(state.x[0] == 0 && !(state.nzcv & (1U << 29)) && f.engine.read32(query) == page_size,
        "sysctl direct syscall disagrees with library query");
    const auto mapping = f.call("_mmap", {0, 4096, 3, 0x1002, UINT64_MAX, 0});
    require(mapping != UINT64_MAX && mapping % page_size == 0 && f.engine.read64(mapping) == 0 &&
        f.engine.query_region(mapping)->size == page_size, "anonymous mmap failed ARM64 page rounding");
    f.engine.write64(mapping, 27);
    require(f.call("_mprotect", {mapping, 4096, 1}) == 0, "whole-map protection failed");
    require(!f.engine.query_region(mapping)->writable && f.engine.read64(mapping) == 27, "protection lost data");
    bool unsupported = false;
    try { static_cast<void>(f.call("_mprotect", {mapping, 1, 1})); }
    catch (const std::runtime_error&) { unsupported = true; }
    // A one-byte request rounds to one page, so remains a whole-map operation.
    require(!unsupported, "page-rounded whole-map protection rejected");
    require(f.call("_munmap", {mapping, 4096}) == 0 && !f.engine.query_region(mapping), "whole-map unmap failed");
    const auto large_mapping = f.call("_mmap", {0, page_size + 1, 3, 0x1002, UINT64_MAX, 0});
    require(f.engine.query_region(large_mapping)->size == 2 * page_size, "multi-page mmap rounding wrong");
    require(f.call("_mprotect", {large_mapping + 4096, page_size, 1}) == UINT64_MAX &&
        f.engine.read32(f.call("___error", {})) == 22, "host 4 KiB alignment leaked into Darwin VM");
    unsupported = false;
    try { static_cast<void>(f.call("_munmap", {large_mapping, page_size})); }
    catch (const std::runtime_error& error) {
        unsupported = std::string_view(error.what()).find("range unmapping") != std::string_view::npos;
    }
    require(unsupported && f.engine.query_region(large_mapping), "partial unmap lost explicit engine requirement");
    require(f.call("_munmap", {large_mapping, page_size + 1}) == 0, "multi-page unmap failed");
    // Without engine VM ranges: an address is only a hint, MAP_NORESERVE and
    // MAP_JIT are accepted, MAP_FIXED fails explicitly (EINVAL).
    const auto hinted = f.call("_mmap", {0x5a0000000000ULL, page_size, 7, 0x1002 | 0x40 | 0x800, UINT64_MAX, 0});
    require(hinted != UINT64_MAX && hinted % page_size == 0, "hinted MAP_JIT mmap failed");
    require(f.call("_madvise", {hinted, page_size, 7}) == 0 && f.call("_madvise", {hinted, page_size, 8}) == 0,
        "MADV_FREE_REUSABLE/REUSE failed");
    require(f.call("_madvise", {hinted, page_size, 99}) == UINT64_MAX && f.engine.read32(f.call("___error", {})) == 22,
        "invalid madvise advice accepted");
    require(f.call("_mmap", {hinted, page_size, 3, 0x1012, UINT64_MAX, 0}) == UINT64_MAX &&
        f.engine.read32(f.call("___error", {})) == 22, "MAP_FIXED without engine VM ranges did not fail with EINVAL");
    require(f.call("_munmap", {hinted, page_size}) == 0, "hinted unmap failed");
}

void termination_wrappers() {
    for (const auto normal : {false, true}) {
        Fixture f;
        unsigned finalized = 0, cleaned = 0;
        const auto finalizer = f.context.install_function("_service_test_finalizer",
            [&](auto&, auto&) { ++finalized; });
        f.context.register_finalizer(finalizer, 0, 0);
        f.context.cleanup_thread = [&](auto&) { ++cleaned; };
        f.engine.set_supervisor_call_handler([&](const auto& call, auto& state, auto&) {
            require(call.immediate == 0x80 && state.x[16] == 1, "termination did not issue Darwin exit");
            f.context.request_exit(static_cast<std::uint32_t>(state.x[0]));
            return std::optional(static_cast<std::uint32_t>(state.x[0]));
        });
        require(f.call(normal ? "_exit" : "__exit", {7}) == 7 &&
            f.context.requested_exit() == 7, "termination status lost");
        require(finalized == (normal ? 1U : 0U) && cleaned == (normal ? 1U : 0U),
            "raw and normal exit cleanup rules wrong");
    }
}

void locale_and_scanning() {
    Fixture f;
    const auto locale = f.call("_newlocale", {63, f.string("C"), 0});
    require(locale && f.call("___mb_cur_max_l", {locale}) == 1, "C locale profile missing");
    require(f.call("_isalpha_l", {'A', locale}) == 1 && f.call("_isdigit_l", {'A', locale}) == 0,
        "C character classes wrong");
    require(f.call("_newlocale", {63, f.string("unavailable.UTF-8"), 0}) == 0,
        "unsupported locale accepted");
    const auto wide = f.context.allocate_guest(32), byte = f.string("A");
    require(f.call("_mbrtowc_l", {wide, byte, 1, 0, locale}) == 1 && f.engine.read32(wide) == 'A',
        "Darwin wchar conversion wrong");
    const auto invalid = f.context.allocate_guest(1);
    static_cast<transmute::GuestMemory&>(f.engine).write8(invalid, 0xff);
    require(f.call("_mbrtowc_l", {wide, invalid, 1, 0, locale}) == UINT64_MAX &&
        f.engine.read32(f.call("___error", {})) == 92, "invalid C-locale byte accepted");
    const auto integer = f.context.allocate_guest(8), number = f.context.allocate_guest(8);
    const auto word = f.context.allocate_guest(32), count = f.context.allocate_guest(8);
    f.slot(0, integer); f.slot(1, number); f.slot(2, word); f.slot(3, count);
    require(f.call("_sscanf_l", {f.string("-27 1.25 name"), locale, f.string("%d %lf %s%n")}) == 3,
        "checked scanf assignment count wrong");
    require(f.engine.read32(integer) == static_cast<std::uint32_t>(-27) &&
        std::bit_cast<double>(f.engine.read64(number)) == 1.25 && get(f.engine, word) == "name" &&
        f.engine.read32(count) == 13, "scanf destination types/counter wrong");
    const auto wide_format = f.context.allocate_guest(16);
    f.engine.write32(wide_format, '%'); f.engine.write32(wide_format + 4, 'd'); f.engine.write32(wide_format + 8, 0);
    f.slot(0, 27);
    require(f.call("_swprintf", {wide, 8, wide_format}) == 2 && f.engine.read32(wide) == '2' &&
        f.engine.read32(wide + 4) == '7' && f.engine.read32(wide + 8) == 0, "wide formatting used Windows wchar width");
    require(f.call("_freelocale", {locale}) == 0, "locale cleanup failed");
    bool asserted = false;
    try { static_cast<void>(f.call("___assert_rtn", {f.string("main"), f.string("fixture.c"), 27, f.string("value != 0")})); }
    catch (const std::runtime_error& error) {
        const std::string_view message(error.what());
        asserted = message.find("value != 0") != std::string_view::npos && message.find("fixture.c") != std::string_view::npos && message.find("27") != std::string_view::npos;
    }
    require(asserted, "Darwin assertion did not terminate with checked diagnostic");
}

void guest_path_roundtrips() {
    Fixture f;
    const auto directory = f.call("_getcwd", {0, 0});
    const auto guest_directory = get(f.engine, directory);
    require(guest_directory.starts_with('/'), "getcwd returned a host drive-relative guest path");
    require(f.context.map_guest_path && f.context.map_guest_path(guest_directory).lexically_normal() ==
        f.context.working_directory.lexically_normal(), "cwd guestpath failed roundtrip");
    const auto canonical = f.call("_realpath", {f.string("."), 0});
    require(canonical && f.context.map_guest_path(get(f.engine, canonical)) ==
        std::filesystem::canonical(f.context.working_directory), "canonical guestpath failed roundtrip");
#ifdef _WIN32
    require(f.context.map_guest_path("/tmp").lexically_normal() ==
        std::filesystem::temp_directory_path().lexically_normal(), "temporary guest mount incorrect");
    const auto drive = f.context.working_directory.root_path();
    require(f.context.map_guest_path("/") == drive &&
        f.context.map_guest_path(alchemy::guest_path_string(drive)) == drive,
        "guest root/volume mappings incorrect");
#endif
    static_cast<void>(f.call("_free", {directory}));
    static_cast<void>(f.call("_free", {canonical}));
}

std::uint64_t guest_code(Fixture& f, std::initializer_list<std::uint32_t> instructions) {
    const auto address = f.context.allocate_guest(instructions.size() * 4);
    std::uint64_t at = address;
    for (const auto value : instructions) { f.engine.write32(at, value); at += 4; }
    f.engine.set_memory_access(address, true, false, true);
    return address;
}
std::uint64_t symbol(Fixture& f, std::string_view name) {
    const auto value = f.context.resolve(system_library, name);
    require(value.has_value(), "libSystem symbol missing");
    return value->address;
}

void jumps_and_contexts() {
    Fixture f;
    // jumper(buf, setjmp, longjmp): setjmp; longjmp(buf, 7) after clobbering x19; returns x19 - value.
    const auto jumper = guest_code(f, {0xa9bd7bfdU, 0xa90153f3U, 0xa9025bf5U, 0x910003fdU, 0xaa0003f3U, 0xaa0203f5U,
        0xd63f0020U, 0xb50000c0U, 0xaa1303e0U, 0xd2800013U, 0xd28000e1U, 0xd63f02a0U, 0xd4200020U,
        0xcb000260U, 0xd503201fU, 0xa9425bf5U, 0xa94153f3U, 0xa8c37bfdU, 0xd65f03c0U});
    for (const auto& [save, restore] : std::array<std::pair<const char*, const char*>, 3>{{
            {"_setjmp", "_longjmp"}, {"__setjmp", "__longjmp"}, {"_sigsetjmp", "_siglongjmp"}}}) {
        const auto buffer = f.context.allocate_guest(192);
        const std::array arguments{buffer, symbol(f, save), symbol(f, restore)};
        require(f.engine.invoke_callback(jumper, arguments) == buffer - 7, "longjmp did not restore callee-saved state and value");
    }
    bool rejected{};
    try { static_cast<void>(f.call("_longjmp", {f.context.allocate_guest(192), 1})); }
    catch (const std::runtime_error& error) { rejected = std::string(error.what()).find("did not initialize") != std::string::npos; }
    require(rejected, "longjmp accepted an uninitialized jmp_buf");

    // swapper(main, target, swapcontext): x19 = 5 survives a makecontext round trip.
    const auto swapper = guest_code(f, {0xa9be7bfdU, 0xa90153f3U, 0x910003fdU, 0xd28000b3U, 0xd63f0040U,
        0x8b130000U, 0xa94153f3U, 0xa8c27bfdU, 0xd65f03c0U});
    const auto main_context = f.context.allocate_guest(880), target = f.context.allocate_guest(880);
    const auto stack = f.context.allocate_guest(65536);
    std::array<std::uint64_t, 2> received{};
    const auto function = f.context.install_function("test_context_function", [&](auto& s, auto&) {
        received = {s.x[0], s.x[1]}; s.x[0] = 0;
    });
    require(f.call("_getcontext", {target}) == 0, "getcontext");
    f.engine.write64(target + 8, stack); f.engine.write64(target + 16, 65536); f.engine.write64(target + 32, main_context);
    f.slot(0, 11); f.slot(1, 22);
    static_cast<void>(f.call("_makecontext", {target, function, 2}));
    const std::array swap{main_context, target, symbol(f, "_swapcontext")};
    const auto result = f.engine.invoke_callback(swapper, swap);
    require(received[0] == 11 && received[1] == 22, "makecontext arguments");
    require(result == 5, "uc_link did not resume the swapped-out context");
}

void blocks_runtime() {
    Fixture f;
    auto& m = static_cast<transmute::GuestMemory&>(f.engine);
    const auto stack_isa = symbol(f, "__NSConcreteStackBlock"), global_isa = symbol(f, "__NSConcreteGlobalBlock");
    unsigned copies{}, disposals{};
    const auto copy_helper = f.context.install_function("test_block_copy", [&](auto& s, auto& memory) {
        ++copies; memory.write64(s.x[0] + 32, memory.read64(s.x[1] + 32) + 1); s.x[0] = 0;
    });
    const auto dispose_helper = f.context.install_function("test_block_dispose", [&](auto& s, auto&) { ++disposals; s.x[0] = 0; });
    const auto descriptor = f.context.allocate_guest(32);
    f.engine.write64(descriptor, 0); f.engine.write64(descriptor + 8, 40);
    f.engine.write64(descriptor + 16, copy_helper); f.engine.write64(descriptor + 24, dispose_helper);
    const auto block = f.context.allocate_guest(40);
    f.engine.write64(block, stack_isa); f.engine.write32(block + 8, 1U << 25); f.engine.write32(block + 12, 0);
    f.engine.write64(block + 16, 0x1234); f.engine.write64(block + 24, descriptor); f.engine.write64(block + 32, 41);
    const auto heap = f.call("__Block_copy", {block});
    require(heap && heap != block && f.engine.read64(heap) == symbol(f, "__NSConcreteMallocBlock"), "Block_copy did not move the block to the heap");
    require(f.engine.read64(heap + 32) == 42 && copies == 1, "Block_copy did not run the copy helper");
    require((f.engine.read32(heap + 8) & 0xfffe) == 2 && (f.engine.read32(heap + 8) & (1U << 24)), "heap block reference count");
    require(f.call("__Block_copy", {heap}) == heap && (f.engine.read32(heap + 8) & 0xfffe) == 4, "Block_copy of a heap block retains it");
    static_cast<void>(f.call("__Block_release", {heap}));
    require(f.engine.query_region(heap).has_value() && disposals == 0, "first release freed the block");
    static_cast<void>(f.call("__Block_release", {heap}));
    require(!f.engine.query_region(heap) && disposals == 1, "last release did not dispose the block");
    const auto global = f.context.allocate_guest(32);
    f.engine.write64(global, global_isa); f.engine.write32(global + 8, 1U << 28); f.engine.write64(global + 24, descriptor);
    require(f.call("__Block_copy", {global}) == global, "global blocks are not copied");
    static_cast<void>(f.call("__Block_release", {global}));
    // __block variable without helpers: payload moves to the heap, the stack forwards to it.
    const auto byref = f.context.allocate_guest(32), destination = f.context.allocate_guest(8);
    f.engine.write64(byref, 0); f.engine.write64(byref + 8, byref); f.engine.write32(byref + 16, 0); f.engine.write32(byref + 20, 32);
    f.engine.write64(byref + 24, 0xfeed);
    static_cast<void>(f.call("__Block_object_assign", {destination, byref, 8}));
    const auto moved = f.engine.read64(destination);
    require(moved != byref && f.engine.read64(byref + 8) == moved && f.engine.read64(moved + 8) == moved, "__block forwarding");
    require(f.engine.read64(moved + 24) == 0xfeed, "__block payload copy");
    static_cast<void>(f.call("__Block_object_assign", {destination, byref, 8}));
    require(f.engine.read64(destination) == moved, "second __block capture shares the heap copy");
    static_cast<void>(f.call("__Block_object_dispose", {moved, 8}));
    static_cast<void>(f.call("__Block_object_dispose", {moved, 8}));
    require(f.engine.query_region(moved).has_value(), "__block copy freed while the stack frame still references it");
    static_cast<void>(f.call("__Block_object_dispose", {byref, 8}));
    require(!f.engine.query_region(moved), "__block copy outlived its last reference");
    static_cast<void>(m);
}

void mach_and_identity() {
    Fixture f;
    const std::uint64_t task = 0xa1c1; // mach_task_self_
    const auto name = f.context.allocate_guest(8);
    require(f.call("_semaphore_create", {task, name, 0, 1}) == 0, "semaphore_create");
    const auto semaphore = f.engine.read32(name);
    require(f.call("_semaphore_wait", {semaphore}) == 0, "semaphore permit");
    require(f.call("_semaphore_timedwait", {semaphore, 0}) == 49, "semaphore timed wait expiry");
    require(f.call("_semaphore_signal", {semaphore}) == 0 && f.call("_semaphore_timedwait", {semaphore, 0}) == 0, "semaphore signal");
    require(f.call("_semaphore_destroy", {task, semaphore}) == 0 && f.call("_semaphore_signal", {semaphore}) == 4, "semaphore destroy");
    const auto info = f.context.allocate_guest(64), count = f.context.allocate_guest(4);
    f.engine.write32(count, 12);
    require(f.call("_task_info", {task, 20, info, count}) == 0 && f.engine.read32(count) == 12 && f.engine.read64(info + 8) > 0, "MACH_TASK_BASIC_INFO");
    f.engine.write32(count, 2);
    require(f.call("_task_info", {task, 20, info, count}) == 4, "task_info accepted a short count");
    const auto clock = f.context.allocate_guest(4), time = f.context.allocate_guest(8);
    require(f.call("_host_get_clock_service", {f.call("_mach_host_self", {}), 1, clock}) == 0 &&
        f.call("_clock_get_time", {f.engine.read32(clock), time}) == 0 && f.engine.read32(time) > 1'600'000'000U, "calendar clock");
    require(f.call("_thread_policy_set", {f.call("_mach_thread_self", {}), 4, info, 1}) == 46, "affinity policy is unsupported");
    require(f.call("_task_set_exception_ports", {task, 0x1fe, 0, 1, 0}) == 0 && f.call("_task_set_exception_ports", {task, 0x1fe, 77, 1, 0}) == 46,
        "exception port registration");
    require(f.call("_mach_port_deallocate", {task, f.call("_mach_thread_self", {})}) == 0, "port deallocate");
    const auto versions = f.context.allocate_guest(16);
    f.engine.write32(versions, 1); f.engine.write32(versions + 4, 0x000e0000);
    require(f.call("__availability_version_check", {1, versions}) == 1, "macOS 14.0 available");
    f.engine.write32(versions + 4, 0x000f0000);
    require(f.call("__availability_version_check", {1, versions}) == 0, "macOS 15.0 unavailable");
    const auto uuid = f.context.allocate_guest(16), uuid_text = f.context.allocate_guest(40);
    for (unsigned index = 0; index < 16; ++index) static_cast<transmute::GuestMemory&>(f.engine).write8(uuid + index, static_cast<std::uint8_t>(index * 17));
    static_cast<void>(f.call("_uuid_unparse", {uuid, uuid_text}));
    require(get(f.engine, uuid_text) == "00112233-4455-6677-8899-AABBCCDDEEFF", "uuid_unparse");
    require(f.call("_uuid_parse", {uuid_text, info}) == 0 && f.call("_uuid_compare", {uuid, info}) == 0, "uuid_parse round trip");
    const auto host = f.context.allocate_guest(16);
    require(f.call("_gethostuuid", {host, 0}) == 0 && f.call("_uuid_is_null", {host}) == 0, "gethostuuid");
    // sysctl: values USD and TBB validate.
    const auto value = f.context.allocate_guest(64), length = f.context.allocate_guest(8);
    f.engine.write64(length, 8);
    require(f.call("_sysctlbyname", {f.string("hw.cachelinesize"), value, length, 0, 0}) == 0 &&
        f.engine.read64(value) == 128 && f.engine.read64(length) == 8, "hw.cachelinesize");
    f.engine.write64(length, 4);
    require(f.call("_sysctlbyname", {f.string("hw.ncpu"), value, length, 0, 0}) == 0 && f.engine.read32(value) >= 1, "hw.ncpu");
    const auto mib = f.context.allocate_guest(8), mib_count = f.context.allocate_guest(8);
    f.engine.write64(mib_count, 2);
    require(f.call("_sysctlnametomib", {f.string("hw.physicalcpu"), mib, mib_count}) == 0, "sysctlnametomib");
    f.engine.write64(length, 4);
    require(f.call("_sysctl", {mib, 2, value, length, 0, 0}) == 0 && f.engine.read32(value) >= 1, "sysctl private MIB");
    require(f.call("_sysctlbyname", {f.string("hw.nonexistent"), value, length, 0, 0}) == UINT64_MAX, "unknown sysctl");
    // passwd for the virtual user.
    const auto record = f.call("_getpwuid", {f.call("_getuid", {})});
    require(record && get(f.engine, f.engine.read64(record)) == "alchemy" && !get(f.engine, f.engine.read64(record + 48)).empty(), "getpwuid");
    const auto storage = f.context.allocate_guest(4096), passwd = f.context.allocate_guest(72), out = f.context.allocate_guest(8);
    require(f.call("_getpwuid_r", {1000, passwd, storage, 4096, out}) == 0 && f.engine.read64(out) == passwd, "getpwuid_r");
    require(f.call("_getpwuid_r", {1000, passwd, storage, 4, out}) == 34, "getpwuid_r ERANGE");
}

void patterns_and_directories() {
    Fixture f;
    require(f.call("_fnmatch", {f.string("*.txt"), f.string("a.txt"), 0}) == 0, "fnmatch star");
    require(f.call("_fnmatch", {f.string("*"), f.string("a/b"), 2}) == 1, "FNM_PATHNAME star stops at slash");
    require(f.call("_fnmatch", {f.string("*/b"), f.string("a/b"), 2}) == 0, "FNM_PATHNAME components");
    require(f.call("_fnmatch", {f.string("*"), f.string(".hidden"), 4}) == 1, "FNM_PERIOD");
    require(f.call("_fnmatch", {f.string("*/*"), f.string("a/.b"), 6}) == 1, "FNM_PERIOD after a separator");
    require(f.call("_fnmatch", {f.string("[a-c]x[!0-9]"), f.string("bxz"), 0}) == 0, "fnmatch brackets");
    require(f.call("_fnmatch", {f.string("A*"), f.string("abc"), 16}) == 0, "FNM_CASEFOLD");
    const auto regex = f.context.allocate_guest(32), matches = f.context.allocate_guest(64);
    require(f.call("_regcomp", {regex, f.string("^a(b+)c$"), 1}) == 0 && f.engine.read64(regex + 8) == 1, "ERE compile");
    require(f.call("_regexec", {regex, f.string("abbbc"), 2, matches, 0}) == 0, "ERE match");
    require(f.engine.read64(matches + 16) == 1 && f.engine.read64(matches + 24) == 4, "ERE group offsets");
    require(f.call("_regexec", {regex, f.string("ac"), 0, 0, 0}) == 1, "REG_NOMATCH");
    static_cast<void>(f.call("_regfree", {regex}));
    require(f.call("_regcomp", {regex, f.string("a\\(b*\\)c"), 0}) == 0 && f.call("_regexec", {regex, f.string("xabbc"), 0, 0, 0}) == 0, "BRE");
    static_cast<void>(f.call("_regfree", {regex}));
    require(f.call("_regcomp", {regex, f.string("a("), 1}) == 8, "REG_EPAREN");
    const auto message = f.context.allocate_guest(128);
    require(f.call("_regerror", {8, regex, message, 128}) > 1 && get(f.engine, message) == "parentheses not balanced", "regerror");
    require(f.call("_regcomp", {regex, f.string("^b"), 9}) == 0 && f.call("_regexec", {regex, f.string("a\nb"), 0, 0, 0}) == 0, "REG_NEWLINE anchors");
    static_cast<void>(f.call("_regfree", {regex}));

    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto directory = std::filesystem::temp_directory_path() / ("alchemy-glob-" + unique);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); } } cleanup{directory};
    std::filesystem::create_directories(directory / "sub");
    for (const auto name : {"b.txt", "a.txt", "c.dat", ".hidden.txt"}) { std::ofstream file(directory / name); file << name; }
    const auto guest = alchemy::guest_path_string(directory);
    const auto results = f.context.allocate_guest(88);
    f.engine.fill_bytes(results, 0, 88);
    { const auto code = f.call("_glob", {f.string(guest + "/*.txt"), 0, 0, results}); require(code == 0 && f.engine.read64(results) == 2, "glob matches " + std::to_string(code) + " " + std::to_string(f.engine.read64(results)) + " " + guest); }
    const auto paths = f.engine.read64(results + 32);
    require(get(f.engine, f.engine.read64(paths)) == guest + "/a.txt" && get(f.engine, f.engine.read64(paths + 8)) == guest + "/b.txt" &&
        f.engine.read64(paths + 16) == 0, "glob sorted results");
    require(f.call("_glob", {f.string(guest + "/*.dat"), 1, 0, results}) == 0 && f.engine.read64(results) == 3, "GLOB_APPEND");
    require(f.call("_glob", {f.string(guest + "/{a,c}.*"), 0x80 | 1, 0, results}) == 0 && f.engine.read64(results) == 5, "GLOB_BRACE");
    static_cast<void>(f.call("_globfree", {results}));
    require(f.engine.read64(results) == 0 && f.engine.read64(results + 32) == 0, "globfree");
    require(f.call("_glob", {f.string(guest + "/*.none"), 0, 0, results}) == static_cast<std::uint32_t>(-3), "GLOB_NOMATCH");
    require(f.call("_glob", {f.string(guest + "/*/"), 8, 0, results}) == 0 && f.engine.read64(results) == 1 &&
        get(f.engine, f.engine.read64(f.engine.read64(results + 32))) == guest + "/sub/", "glob directories with GLOB_MARK");
    static_cast<void>(f.call("_globfree", {results}));

    const auto list = f.context.allocate_guest(8);
    const auto count = f.call("_scandir", {f.string(guest), list, 0, symbol(f, "_alphasort")});
    require(count == 5, "scandir entry count");
    const auto array = f.engine.read64(list);
    require(get(f.engine, f.engine.read64(array) + 21) == ".hidden.txt" && get(f.engine, f.engine.read64(array + 32) + 21) == "sub", "scandir alphasort order");
    for (std::uint64_t index = 0; index < count; ++index) static_cast<void>(f.call("_free", {f.engine.read64(array + index * 8)}));
    static_cast<void>(f.call("_free", {array}));

    const auto stats = f.context.allocate_guest(2168);
    require(f.call("_statfs", {f.string(guest), stats}) == 0 && f.engine.read32(stats) == 4096 && f.engine.read64(stats + 8) > 0 &&
        !get(f.engine, stats + 72).empty() && !get(f.engine, stats + 88).empty(), "statfs");
    require(f.call("_renamex_np", {f.string(guest + "/a.txt"), f.string(guest + "/b.txt"), 4}) == UINT64_MAX &&
        f.engine.read32(f.context.current_thread()->errno_address) == 17, "RENAME_EXCL refuses an existing target");
    require(f.call("_renamex_np", {f.string(guest + "/a.txt"), f.string(guest + "/d.txt"), 4}) == 0 &&
        std::filesystem::exists(directory / "d.txt"), "RENAME_EXCL rename");
    const auto times = f.context.allocate_guest(32);
    f.engine.write64(times, 1'000'000'000); f.engine.write64(times + 8, 0); f.engine.write64(times + 16, 1'500'000'000); f.engine.write64(times + 24, 0);
    const auto descriptor = f.call("_open", {f.string(guest + "/d.txt"), 0});
    require(f.call("_futimens", {descriptor, times}) == 0 && f.call("_close", {descriptor}) == 0, "futimens");
    const auto modified = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::clock_cast<std::chrono::system_clock>(std::filesystem::last_write_time(directory / "d.txt")).time_since_epoch()).count();
    require(modified == 1'500'000'000, "futimens modification time");
    require(f.call("_copyfile", {f.string(guest + "/d.txt"), f.string(guest + "/e.txt"), 0, 8}) == 0 && std::filesystem::file_size(directory / "e.txt") == 5, "copyfile data");
#ifdef _WIN32
    // Extended attributes are NTFS streams: a fresh file has none.
    require(f.call("_listxattr", {f.string(guest + "/d.txt"), 0, 0, 0}) == 0, "listxattr of a file without attributes");
#else
    require(f.call("_listxattr", {f.string(guest + "/d.txt"), 0, 0, 0}) == UINT64_MAX &&
        f.engine.read32(f.context.current_thread()->errno_address) == 45, "listxattr ENOTSUP");
#endif
    const auto directory_stream = f.call("_opendir", {f.string(guest)});
    const auto entry = f.context.allocate_guest(1048), result = f.context.allocate_guest(8);
    unsigned entries{};
    while (f.call("_readdir_r", {directory_stream, entry, result}) == 0 && f.engine.read64(result)) ++entries;
    require(entries == 6 && f.call("_closedir", {directory_stream}) == 0, "readdir_r");
}

void libc_leftovers() {
    Fixture f;
    require(f.call("_abs", {static_cast<std::uint32_t>(-5)}) == 5, "abs");
    require(f.call("_div", {static_cast<std::uint32_t>(-7), 2}) == (static_cast<std::uint32_t>(-3) | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(-1)) << 32)), "div");
    static_cast<void>(f.call("_srand", {1}));
    require(f.call("_rand", {}) == 16807 && f.call("_rand", {}) == 282475249, "Darwin rand sequence");
    const auto clock = f.context.allocate_guest(8), buffer = f.context.allocate_guest(32);
    f.engine.write64(clock, 0);
    require(f.call("_ctime_r", {clock, buffer}) == buffer && get(f.engine, buffer).size() == 25, "ctime_r");
    const auto tm = f.context.allocate_guest(64);
    f.engine.fill_bytes(tm, 0, 64);
    f.engine.write32(tm + 12, 16); f.engine.write32(tm + 16, 8); f.engine.write32(tm + 20, 73); f.engine.write32(tm + 24, 0);
    f.engine.write32(tm + 8, 1); f.engine.write32(tm + 4, 3); f.engine.write32(tm, 52);
    require(get(f.engine, f.call("_asctime", {tm})) == "Sun Sep 16 01:03:52 1973\n", "asctime");
    require(f.engine.read32(symbol(f, "_sys_nerr")) == 107, "sys_nerr");
    require(f.call("_strtoumax", {f.string("18446744073709551615"), 0, 10}) == UINT64_MAX, "strtoumax");
    const auto block = f.call("_malloc", {100});
    const auto stats = f.context.allocate_guest(32);
    static_cast<void>(f.call("_malloc_zone_statistics", {f.call("_malloc_default_zone", {}), stats}));
    require(f.engine.read32(stats) >= 1 && f.engine.read64(stats + 8) >= 100, "malloc_zone_statistics");
    static_cast<void>(f.call("_free", {block}));
    // popen runs the built-in shell in a child Alchemy process (alchemy_process_model
    // covers that); this host test checks argument validation only.
    require(f.call("_popen", {f.string("ls"), f.string("x")}) == 0 && f.engine.read32(f.context.current_thread()->errno_address) == 22, "popen invalid mode EINVAL");
    const auto frames = f.context.allocate_guest(16);
    f.engine.write64(frames, 0x1234); f.engine.write64(frames + 8, 0x5678);
    const auto symbols = f.call("_backtrace_symbols", {frames, 2});
    require(symbols && get(f.engine, f.engine.read64(symbols + 8)).find("0x0000000000005678") != std::string::npos, "backtrace_symbols");
    static_cast<void>(f.call("_free", {symbols}));
    const auto region = f.context.allocate_guest(65536), residency = f.context.allocate_guest(4);
    const auto cache = (region + 16383) & ~16383ULL;
    require(f.call("_mincore", {cache, 1, residency}) == 0 && static_cast<transmute::GuestMemory&>(f.engine).read8(residency) == 1, "mincore");
    require(f.call("_mincore", {cache + 1, 1, residency}) == UINT64_MAX, "mincore alignment");
    require(f.call("_posix_madvise", {cache, 16384, 4}) == 0 && f.call("_posix_madvise", {cache, 16384, 9}) == 22, "posix_madvise");
    static_cast<void>(f.call("_sys_icache_invalidate", {cache, 16}));
}

void compiled_fixture(const std::filesystem::path& program) {
    for (const auto jit : {false, true}) for (const auto rebased : {false, true}) {
        std::string output, error_output;
        alchemy::RunOptions options;
        options.jit = jit;
        options.jit_stats = jit;
        options.load_base = rebased ? std::optional<std::uint64_t>(0x180000000ULL) : std::nullopt;
        const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto path = std::filesystem::temp_directory_path() / ("alchemy-services-guest-" + unique + ".bin");
        const std::array arguments{utf8(path)};
        const auto result = alchemy::run_program(program, arguments, options,
            [&output, &error_output](unsigned descriptor, auto bytes) {
                require(descriptor == 1 || descriptor == 2, "compiled service output descriptor wrong");
                (descriptor == 1 ? output : error_output).append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                return alchemy::OutputResult{bytes.size(), 0};
            });
        require(result.exit_code == 0, "compiled C service fixture failed with exit " + std::to_string(result.exit_code));
        require(output == "services passed\nservices finalized\n", "compiled service/finalizer output wrong");
        require(error_output.empty(), "compiled service fixture wrote stderr");
        if (jit) require(result.jit.native_guest_instructions != 0, "service fixture did not execute translated code");
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        memory_and_format(); files_stdio_syscalls(); environment_clock_mapping(); termination_wrappers(); locale_and_scanning(); guest_path_roundtrips();
        jumps_and_contexts(); blocks_runtime(); mach_and_identity(); patterns_and_directories(); libc_leftovers();
        if (argc == 2) compiled_fixture(argv[1]);
        std::cout << "Alchemy Darwin services tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
