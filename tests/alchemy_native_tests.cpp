#include "alchemy/darwin_native.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/path.hpp"
#include "transmute/jit.hpp"

#include <array>
#include <bit>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext context{engine, [](unsigned, auto bytes) { return alchemy::OutputResult{bytes.size(), 0}; }};
    alchemy::DarwinServices services{context};
    explicit Fixture(bool jit) {
        alchemy::install_darwin_native(context);
        engine.set_jit_enabled(jit); engine.set_jit_profiling(jit);
    }
    std::uint64_t call(const char* name, std::initializer_list<std::uint64_t> args = {}) {
        const auto value = context.resolve("/usr/lib/libSystem.B.dylib", name);
        require(value.has_value(), "missing native provider");
        return engine.invoke_callback(value->address, std::span(args.begin(), args.size()));
    }
    std::uint64_t string(std::string_view value) {
        const auto at = context.allocate_guest(value.size() + 1);
        engine.write_bytes(at, {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
        return at;
    }
    std::string text(std::uint64_t at, const char* label) {
        require(at != 0, label);
        const auto value = engine.read_string(at, 1, 4096); return {value.begin(), value.end()};
    }
    void scan_args(std::initializer_list<std::uint64_t> args) {
        const auto base = (engine.state().sp - 256) & ~15ULL;
        unsigned index{}; for (const auto value : args) engine.write64(base + index++ * 8, value);
    }
};
void run(bool jit) {
    Fixture f(jit);
    const auto input = f.string(",one;two,,three"), commas = f.string(",;"), semicolon = f.string(";");
    require(f.text(f.call("_strtok", {input, commas}), "first token null") == "one", "token one");
    require(f.text(f.call("_strtok", {0, commas}), "second token null") == "two", "token two");
    require(f.text(f.call("_strtok", {0, semicolon}), "changed delimiters returned null") == ",three", "changed separators");
    require(f.call("_strtok", {0, commas}) == 0, "token exhaustion");
    const auto saved = f.context.allocate_guest(8), second = f.string("alpha beta");
    f.engine.write64(saved, UINT64_MAX);
    require(f.text(f.call("_strtok_r", {second, f.string(" "), saved}), "reentrant first token null") == "alpha", "reentrant first call ignores saved input");
    require(f.text(f.call("_strtok_r", {0, f.string(""), saved}), "empty delimiters returned null") == "beta", "empty delimiters");
    require(f.call("_strtok_r", {0, commas, saved}) == 0, "reentrant exhaustion");
    require(f.call("_strtok_r", {input, commas, 1}) == 0, "invalid token state");
    require(f.engine.read32(f.context.current_thread()->errno_address) == 14, "token EFAULT");

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() / ("alchemy-scanner-" + std::to_string(stamp) + ".txt");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); } } cleanup{path};
    const auto file = f.call("_fopen", {f.string(alchemy::guest_path_string(path)), f.string("w+")});
    require(file != 0, "scanner fixture fopen");
    require(f.call("_fputs", {f.string(" 27,13.5 word! 12e+;7"), file}) != UINT32_MAX, "scanner fixture data");
    require(f.call("_fseek", {file, 0, 0}) == 0, "scanner rewind");
    const auto value = f.context.allocate_guest(4), floating = f.context.allocate_guest(8);
    const auto word = f.context.allocate_guest(8), count = f.context.allocate_guest(4), letter = f.context.allocate_guest(1);
    f.scan_args({value, floating, word, count, letter});
    require(f.call("_fscanf", {file, f.string("%d,%lf %4s%n%c")}) == 4, "scanner assignment count");
    require(f.engine.read32(value) == 27, "scanner integer");
    require(std::bit_cast<double>(f.engine.read64(floating)) == 13.5, "scanner double");
    require(f.text(word, "word buffer null") == "word" && static_cast<transmute::GuestMemory&>(f.engine).read8(letter) == '!', "scanner string width and literal lookahead");
    require(f.engine.read32(count) == 13, "scanner n includes whitespace but no assignment");
    f.scan_args({floating});
    require(f.call("_fscanf", {file, f.string("%lf")}) == 0, "incomplete exponent matching failure");
    require(f.call("_fgetc", {file}) == ';', "scanner retains first unmatched character");
    f.scan_args({value});
    require(f.call("_fscanf", {file, f.string("%d")}) == 1 && f.engine.read32(value) == 7, "scanner resumes after failure");
    f.scan_args({value});
    require(f.call("_fscanf", {file, f.string("%d")}) == UINT32_MAX, "scanner EOF");
    require(f.call("_fclose", {file}) == 0, "scanner close");

    const auto scanner_stream = [&](std::string_view contents) {
        const auto stream = f.call("_fopen", {f.string(alchemy::guest_path_string(path)), f.string("w+")});
        require(stream != 0, "scanner regression fopen");
        require(f.call("_fputs", {f.string(contents), stream}) != UINT32_MAX, "scanner regression data");
        require(f.call("_fseek", {stream, 0, 0}) == 0, "scanner regression rewind");
        return stream;
    };
    const auto percent = scanner_stream(" %Z");
    require(f.call("_fscanf", {percent, f.string("%%")}) == 0,
            "literal percent unexpectedly converted leading whitespace");
    require(f.call("_fgetc", {percent}) == ' ' && f.call("_fgetc", {percent}) == '%',
            "literal percent consumed whitespace or its first mismatched character");
    require(f.call("_fclose", {percent}) == 0, "literal percent regression close");

    const auto empty = scanner_stream("");
    f.engine.write32(count, 0x11223344); f.engine.write32(value, 27);
    f.scan_args({count, value});
    require(f.call("_fscanf", {empty, f.string("%n%d")}) == UINT32_MAX &&
            f.engine.read32(count) == 0 && f.engine.read32(value) == 27,
            "percent n changed EOF handling or wrote an unsuccessful conversion");
    require(f.call("_fclose", {empty}) == 0, "percent n regression close");

    const auto sign = scanner_stream("+");
    f.engine.write32(value, 27); f.scan_args({value});
    require(f.call("_fscanf", {sign, f.string("%d")}) == 0 && f.engine.read32(value) == 27,
            "sign-only integer at EOF did not return matching failure without assignment");
    require(static_cast<std::uint32_t>(f.call("_fgetc", {sign})) == UINT32_MAX,
            "sign-only integer candidate remained in the input stream");
    require(f.call("_fclose", {sign}) == 0, "sign-only integer regression close");

    const auto exponent = scanner_stream("12e+");
    f.engine.write64(floating, std::bit_cast<std::uint64_t>(27.0)); f.scan_args({floating});
    require(f.call("_fscanf", {exponent, f.string("%lf")}) == 0 &&
            std::bit_cast<double>(f.engine.read64(floating)) == 27.0,
            "incomplete float at EOF did not return matching failure without assignment");
    require(static_cast<std::uint32_t>(f.call("_fgetc", {exponent})) == UINT32_MAX,
            "incomplete float candidate remained in the input stream");
    f.scan_args({floating});
    require(f.call("_fscanf", {exponent, f.string("%lf")}) == UINT32_MAX,
            "scanner did not report EOF after consuming a rejected candidate");
    require(f.call("_fclose", {exponent}) == 0, "incomplete float regression close");

    const auto addresses = f.context.allocate_guest(32);
    require(f.call("_backtrace", {addresses, 0}) == 0, "empty backtrace");
    require(f.call("_backtrace", {addresses, 1025}) == 0, "bounded backtrace");
    constexpr std::uint64_t bridge = 0x100000000ULL;
    f.engine.map_memory(bridge, 4096);
    f.engine.write32(bridge, 0xa9bf7bfd); // stp x29,x30,[sp,#-16]!
    f.engine.write32(bridge + 4, 0x910003fd); // mov x29,sp
    f.engine.write32(bridge + 8, 0x9400000e); // bl bridge+64
    f.engine.write32(bridge + 12, 0xa8c17bfd); // ldp x29,x30,[sp],#16
    f.engine.write32(bridge + 16, 0xd65f03c0); // ret
    f.engine.write32(bridge + 64, 0xa9bf7bfd); // stp x29,x30,[sp,#-16]!
    f.engine.write32(bridge + 68, 0x910003fd); // mov x29,sp
    f.engine.write32(bridge + 72, 0x580000d0); // ldr x16,literal at bridge+96
    f.engine.write32(bridge + 76, 0xd63f0200); // blr x16
    f.engine.write32(bridge + 80, 0xa8c17bfd); // ldp x29,x30,[sp],#16
    f.engine.write32(bridge + 84, 0xd65f03c0); // ret
    f.engine.write64(bridge + 96, f.context.resolve("/usr/lib/libSystem.B.dylib", "_backtrace")->address);
    f.engine.set_memory_access(bridge, true, false, true);
    f.engine.write64(addresses + 16, 0x1122334455667788ULL);
    f.engine.write64(addresses + 24, 0x8877665544332211ULL);
    const std::array<std::uint64_t, 2> trace_arguments{addresses, 4};
    require(f.engine.invoke_callback(bridge, trace_arguments) == 2 &&
            f.engine.read64(addresses) == bridge + 80 &&
            f.engine.read64(addresses + 8) == bridge + 12,
            "backtrace did not report the real ARM64 call and parent frame");
    require(f.engine.read64(addresses + 16) == 0x1122334455667788ULL &&
            f.engine.read64(addresses + 24) == 0x8877665544332211ULL,
            "backtrace wrote beyond its actual frame count");
    if (jit) require(f.engine.jit_stats().native_guest_instructions != 0,
                     "backtrace bridge did not execute translated guest code");
    // longjmp is implemented (darwin_libsystem_extra); an invalid jmp_buf is a diagnostic.
    bool rejected{};
    try { static_cast<void>(f.call("_longjmp", {0, 1})); }
    catch (const std::runtime_error& error) { rejected = std::string(error.what()).find("invalid jmp_buf") != std::string::npos; }
    require(rejected, "longjmp must reject an invalid jmp_buf");
    require(alchemy::deferred_native_api("_longjmp").empty(), "doctor no longer defers longjmp");
}
} // namespace

int main() {
    try { run(false); run(true); std::cout << "Native string/stream contracts passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
