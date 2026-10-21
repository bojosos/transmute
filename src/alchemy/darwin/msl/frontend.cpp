#include "frontend.hpp"

#include "embedded_stdlib.hpp"

#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "alchemy/host/files.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/sync.hpp"

#include <array>
#include <cstddef>
#include <span>

// Clang front end (owner: A6). See frontend.hpp.
namespace alchemy::msl::detail {

#ifndef ALCHEMY_MSL_CLANG_DEFAULT_PATH
#define ALCHEMY_MSL_CLANG_DEFAULT_PATH ""
#endif

std::string environment(const char* name) {
#ifdef _MSC_VER
    char* buffer = nullptr;
    std::size_t length = 0;
    std::string value;
    if (_dupenv_s(&buffer, &length, name) == 0 && buffer) {
        value = buffer;
        std::free(buffer);
    }
    return value;
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}

std::uint64_t fnv1a(std::string_view data, std::uint64_t hash) {
    for (const auto character : data) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

namespace {

#ifdef _WIN32
struct ProcessResult {
    std::uint32_t exit_code{~0U};
    std::string out, err;
};

const std::string& clang_path();

// A started child process with its three pipe ends (ours): host::process::spawn_piped.
struct Spawned {
    host::process::Piped pipes{};
};

void close_spawned(Spawned& child) {
    for (const auto handle : {child.pipes.process, child.pipes.input, child.pipes.output, child.pipes.error})
        if (handle) host::close(handle);
    child = {};
}

// Starts `program args...` with piped stdin/stdout/stderr. The child inherits
// only its own three pipe ends: with every inheritable handle of the process
// (other compiles' pipes on other threads), a concurrent compile's stdout
// would stay open until this child exits.
Spawned spawn_process(const std::string& program, const std::vector<std::string>& args) {
    const auto spawned = host::process::spawn_piped(program, args);
    if (!spawned) throw std::runtime_error("cannot start " + program + " (" + std::string(host::name(spawned.error())) + ")");
    return Spawned{*spawned};
}

// Feeds `input` to a spawned child, collects stdout and stderr, waits for it.
ProcessResult finish_process(Spawned& child, std::string_view input) {
    ProcessResult result;
    auto drain = [](host::Handle handle, std::string& into) {
        std::vector<std::byte> buffer(65536);
        for (;;) {
            const auto read = host::files::read(handle, buffer);
            if (!read || !*read) break;
            into.append(reinterpret_cast<const char*>(buffer.data()), *read);
        }
    };
    std::thread err_thread(drain, child.pipes.error, std::ref(result.err));
    std::thread in_thread([&] {
        std::size_t offset = 0;
        while (offset < input.size()) {
            const auto chunk = std::min<std::size_t>(input.size() - offset, 1 << 20);
            const auto written = host::files::write(child.pipes.input, std::as_bytes(std::span(input.data() + offset, chunk)));
            if (!written || !*written) break;
            offset += *written;
        }
        host::close(child.pipes.input);
        child.pipes.input = {};
    });
    drain(child.pipes.output, result.out);
    in_thread.join();
    err_thread.join();
    const std::array<host::Handle, 1> process{child.pipes.process};
    static_cast<void>(host::sync::wait_any(process, std::nullopt));
    if (const auto code = host::process::exit_code(child.pipes.process)) result.exit_code = *code;
    close_spawned(child);
    return result;
}

// Runs `program args...` with `input` on stdin; collects stdout and stderr.
ProcessResult run_process(const std::string& program, const std::vector<std::string>& args, std::string_view input) {
    auto child = spawn_process(program, args);
    return finish_process(child, input);
}

// Warm clang processes. Starting clang.exe costs tens of milliseconds
// (image load, process creation) before it reads its source from stdin, so
// the IR compile keeps one spare process per argument list started ahead
// of time and blocked on stdin: a compile takes it, writes its source and
// starts the next spare while clang works. Spares that are never used exit
// when this process does (their stdin reaches EOF; an empty source compiles
// to nothing). At most `maximum_warm` spares exist (the oldest are ended).
// ALCHEMY_MSL_WARM=0 disables.
class WarmPool {
public:
    ProcessResult run(const std::string& program, const std::vector<std::string>& args, std::string_view input) {
        static const bool disabled = environment("ALCHEMY_MSL_WARM") == "0";
        if (disabled) return run_process(program, args, input);
        std::string key = program;
        for (const auto& arg : args) key += '\n' + arg;
        std::optional<Spawned> warm;
        {
            std::lock_guard lock(mutex_);
            for (auto it = spares_.begin(); it != spares_.end(); ++it) {
                if (it->first != key) continue;
                warm = it->second;
                spares_.erase(it);
                break;
            }
        }
        // A spare that already exited (it failed to start) is not used.
        if (warm && host::process::exit_code(warm->pipes.process).error() != host::Error::busy) {
            close_spawned(*warm);
            warm.reset();
        }
        Spawned child = warm ? *warm : spawn_process(program, args);
        try {
            add_spare(key, spawn_process(program, args));
        } catch (const std::exception&) {
            // No spare: the next compile starts clang itself.
        }
        return finish_process(child, input);
    }

private:
    static constexpr std::size_t maximum_warm = 4;
    void add_spare(const std::string& key, Spawned child) {
        std::lock_guard lock(mutex_);
        spares_.emplace_back(key, child);
        while (spares_.size() > maximum_warm) {
            auto& oldest = spares_.front().second;
            static_cast<void>(host::process::terminate(oldest.pipes.process, 1));
            close_spawned(oldest);
            spares_.erase(spares_.begin());
        }
    }
    std::mutex mutex_;
    std::vector<std::pair<std::string, Spawned>> spares_;   // oldest first
};

ProcessResult run_clang_warm(const std::vector<std::string>& args, std::string_view input) {
    static WarmPool* pool = new WarmPool;   // never destroyed: spares outlive static destruction
    return pool->run(clang_path(), args, input);
}
#endif

// "<stdin>:12:5: error: message" lines; everything else is context.
std::vector<Diagnostic> parse_diagnostics(const std::string& err, bool& had_error) {
    std::vector<Diagnostic> diagnostics;
    std::istringstream lines(err);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        Diagnostic::Severity severity{};
        std::size_t marker = std::string::npos;
        for (const auto& [text, value] : {std::pair{": error: ", Diagnostic::Severity::error},
                                          std::pair{": fatal error: ", Diagnostic::Severity::error},
                                          std::pair{": warning: ", Diagnostic::Severity::warning},
                                          std::pair{": note: ", Diagnostic::Severity::note}}) {
            marker = line.find(text);
            if (marker != std::string::npos) {
                severity = value;
                break;
            }
        }
        if (marker == std::string::npos) {
            if (line.rfind("clang: error: ", 0) == 0 || line.rfind("clang.exe: error: ", 0) == 0) {
                Diagnostic diagnostic;
                diagnostic.message = "clang: " + line.substr(line.find("error: ") + 7);
                diagnostics.push_back(diagnostic);
                had_error = true;
            }
            continue;
        }
        Diagnostic diagnostic;
        diagnostic.severity = severity;
        diagnostic.message = line.substr(line.find(": ", marker + 1) + 2);
        const auto location = line.substr(0, marker);
        // file:line:col (the file may contain ':' as in C:\...).
        const auto last = location.rfind(':');
        const auto previous = last == std::string::npos ? std::string::npos : location.rfind(':', last - 1);
        std::string file = location;
        if (previous != std::string::npos) {
            file = location.substr(0, previous);
            diagnostic.line = static_cast<std::uint32_t>(std::strtoul(location.c_str() + previous + 1, nullptr, 10));
            diagnostic.column = static_cast<std::uint32_t>(std::strtoul(location.c_str() + last + 1, nullptr, 10));
        }
        if (file != "<stdin>" && file != "program_source") {
            // Inside metal_stdlib or the generated entry wrappers (no line),
            // or a file the source names with #line (Blender's GLSL files:
            // the line is in that file).
            const auto slash = file.find_last_of("/\\");
            diagnostic.message += " (in " + (slash == std::string::npos ? file : file.substr(slash + 1)) + ")";
            const bool internal = file.empty() || file.front() == '<' ||
                                  file.rfind(stdlib_directory().string(), 0) == 0 ||
                                  file.find("stdlib-") != std::string::npos;
            if (internal) diagnostic.line = diagnostic.column = 0;
        }
        if (severity == Diagnostic::Severity::error) had_error = true;
        // Warnings from clang on the source are noise for Metal callers.
        if (severity == Diagnostic::Severity::warning) continue;
        diagnostics.push_back(std::move(diagnostic));
    }
    return diagnostics;
}

const std::string& clang_path() {
    static const std::string path = [] {
        auto value = environment("ALCHEMY_MSL_CLANG");
        if (value.empty()) value = ALCHEMY_MSL_CLANG_DEFAULT_PATH;
        // Prefer the 64-bit host clang next to a 32-bit one (VS ships both).
        const std::filesystem::path configured(value);
        if (!value.empty() && configured.parent_path().filename() == "bin" &&
            configured.parent_path().parent_path().filename() != "x64") {
            const auto x64 = configured.parent_path().parent_path() / "x64" / "bin" / configured.filename();
            std::error_code error;
            if (std::filesystem::exists(x64, error)) value = x64.string();
        }
        return value;
    }();
    return path;
}

std::vector<std::string> common_arguments(const CompileOptions& options) {
    std::vector<std::string> args = {"-x", "clcpp", "-cl-std=clc++1.0", "-cl-no-stdinc", "-target",
        "spir64-unknown-unknown", "-fms-extensions", "-Xclang", "-cl-ext=+cl_khr_fp16,+cl_khr_fp64",
        "-cl-single-precision-constant", "-fno-exceptions", "-fno-rtti", "-ferror-limit=50",
        "-fno-color-diagnostics", "-fno-caret-diagnostics", "-I", stdlib_directory().string()};
    const auto version = options.language_major * 100 + options.language_minor * 10;
    args.push_back("-D__METAL_VERSION__=" + std::to_string(version));
    args.push_back("-D__METAL_MACOS__=1");
    args.push_back("-D__ALCHEMY_MSL__=1");
    if (options.fast_math) args.push_back("-D__FAST_MATH__=1");
    for (const auto& [name, value] : options.macros) args.push_back("-D" + name + "=" + value);
    return args;
}

} // namespace

std::uint64_t stdlib_fingerprint() {
    static const std::uint64_t value = [] {
        // Bump the layout number when stdlib_directory() writes other files.
        std::uint64_t hash = fnv1a("alchemy-msl stdlib layout 2");
        for (std::size_t i = 0; i < stdlib_file_count; ++i) {
            hash = fnv1a(stdlib_files[i].path, hash);
            hash = fnv1a(std::string_view(reinterpret_cast<const char*>(stdlib_files[i].data), stdlib_files[i].size), hash);
        }
        return hash;
    }();
    return value;
}

const std::filesystem::path& cache_root() {
    static const std::filesystem::path root = [] {
        auto value = environment("ALCHEMY_MSL_CACHE");
        if (value == "0" || value == "off") return std::filesystem::path{};
        if (!value.empty()) return std::filesystem::path(value);
        const auto local = environment("LOCALAPPDATA");
        if (!local.empty()) return std::filesystem::path(local) / "Alchemy" / "msl";
        return std::filesystem::temp_directory_path() / "alchemy-msl";
    }();
    return root;
}

const std::filesystem::path& stdlib_directory() {
    static const std::filesystem::path directory = [] {
        static constexpr char digits[] = "0123456789abcdef";
        std::string hash(16, '0');
        auto value = stdlib_fingerprint();
        for (auto index = hash.size(); index-- > 0; value >>= 4) hash[index] = digits[value & 15];
        // The stdlib is needed even when the result cache is disabled.
        auto base = cache_root();
        if (base.empty()) base = std::filesystem::temp_directory_path() / "alchemy-msl";
        const auto dir = base / ("stdlib-" + hash);
        std::error_code error;
        const auto marker = dir / ".complete";
        if (!std::filesystem::exists(marker, error)) {
            // Written to a private directory and renamed, so concurrent
            // processes never see a partial library.
            const auto staging = base / ("stdlib-" + hash + ".tmp" + std::to_string(host::process::current_pid()) + "-" +
                                         std::to_string(host::process::current_thread_id()));
            std::filesystem::create_directories(staging / "__msl", error);
            for (std::size_t i = 0; i < stdlib_file_count; ++i) {
                std::ofstream file(staging / stdlib_files[i].path, std::ios::binary);
                file.write(reinterpret_cast<const char*>(stdlib_files[i].data),
                           static_cast<std::streamsize>(stdlib_files[i].size));
            }
            // The other MSL headers all resolve to metal_stdlib.
            for (const auto* name : {"metal_math", "metal_common", "metal_geometric", "metal_integer",
                     "metal_relational", "metal_matrix", "metal_pack", "metal_texture", "metal_atomic", "metal_compute",
                     "metal_graphics", "metal_simdgroup", "metal_quadgroup", "metal_types", "metal_numeric",
                     "metal_limits", "metal_type_traits", "metal_array", "metal_utility", "metal_functional",
                     "metal_initializer_list", "metal_tensor", "metal_simdgroup_matrix", "metal_uniform"}) {
                std::ofstream file(staging / name, std::ios::binary);
                file << "#include <metal_stdlib>\n";
            }
            // <simd/simd.h> (the CPU/GPU shared vector header Metal sources
            // include): its types are metal_stdlib's vector types.
            std::filesystem::create_directories(staging / "simd", error);
            for (const auto* name : {"simd/simd.h", "simd/vector_types.h", "simd/matrix_types.h"}) {
                std::ofstream file(staging / name, std::ios::binary);
                file << "#include <metal_stdlib>\n";
            }
            { std::ofstream file(staging / ".complete"); }
            std::filesystem::create_directories(base, error);
            std::filesystem::rename(staging, dir, error);
            if (error) std::filesystem::remove_all(staging, error);   // another writer won
        }
        return dir;
    }();
    return directory;
}

ClangOutput preprocess_with_clang(std::string_view source, const CompileOptions& options) {
    ClangOutput result;
#ifdef _WIN32
    if (clang_path().empty()) throw std::runtime_error("no clang configured for the MSL compiler (set ALCHEMY_MSL_CLANG)");
    auto args = common_arguments(options);
    args.push_back("-D__ALCHEMY_MSL_PREPROCESS=1");
    args.push_back("-E");
    args.push_back("-");
    const auto process = run_process(clang_path(), args, source);
    bool had_error = process.exit_code != 0;
    result.diagnostics = parse_diagnostics(process.err, had_error);
    result.ok = !had_error;
    result.output = process.out;
#else
    (void)source;
    (void)options;
    throw std::runtime_error("the MSL compiler front end is implemented for Windows hosts only");
#endif
    return result;
}

#ifdef _WIN32
namespace {

// clang's predefined macros for the preprocessing configuration (and what
// <metal_stdlib> adds to them), from `clang -dM -E` run once per (language
// version, fast math, clang) and kept next to the materialised stdlib.
struct MacroSets {
    std::shared_ptr<const MacroSet> builtins, stdlib;
};

std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

std::optional<MacroSets> predefined_macros(const CompileOptions& options) {
    const auto version = options.language_major * 100 + options.language_minor * 10;
    const auto key = std::to_string(version) + (options.fast_math ? "-fast" : "-precise");
    // One lock per configuration (see stdlib_pch).
    struct Entry { std::mutex mutex; bool done{}; std::optional<MacroSets> value; };
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<Entry>> entries;
    std::shared_ptr<Entry> entry;
    {
        std::lock_guard lock(mutex);
        auto& slot = entries[key];
        if (!slot) slot = std::make_shared<Entry>();
        entry = slot;
    }
    std::lock_guard entry_lock(entry->mutex);
    if (entry->done) return entry->value;
    entry->done = true;
    auto& sets_value = entry->value;
    std::ostringstream stem;
    stem << "macros-" << key << '-' << std::hex << fnv1a(alchemy::msl::toolchain().version + clang_path());
    const auto builtins_path = stdlib_directory() / (stem.str() + ".builtins.h");
    const auto stdlib_path = stdlib_directory() / (stem.str() + ".stdlib.h");
    std::error_code error;
    if (!std::filesystem::exists(builtins_path, error) || !std::filesystem::exists(stdlib_path, error)) {
        CompileOptions base = options;
        base.macros.clear();
        auto args = common_arguments(base);
        for (const auto* arg : {"-D__ALCHEMY_MSL_PREPROCESS=1", "-dM", "-E", "-"}) args.push_back(arg);
        const auto builtins = run_process(clang_path(), args, {});
        const auto all = run_process(clang_path(), args, "#include <metal_stdlib>\n");
        if (builtins.exit_code != 0 || all.exit_code != 0) {
            sets_value = std::nullopt;
            return std::nullopt;
        }
        // The stdlib's macros: the #define lines the include adds.
        std::set<std::string> known;
        {
            std::istringstream lines(builtins.out);
            for (std::string line; std::getline(lines, line);) known.insert(line);
        }
        std::string stdlib;
        {
            std::istringstream lines(all.out);
            for (std::string line; std::getline(lines, line);)
                if (!known.count(line)) stdlib += line + "\n";
        }
        const auto suffix = ".tmp" + std::to_string(host::process::current_pid()) + "-" + std::to_string(host::process::current_thread_id());
        for (const auto& [path, text] : {std::pair{builtins_path, builtins.out}, std::pair{stdlib_path, stdlib}}) {
            auto staging = path;
            staging += suffix;
            std::ofstream(staging, std::ios::binary) << text;
            std::filesystem::rename(staging, path, error);
            if (error) std::filesystem::remove(staging, error);
        }
    }
    MacroSets result{parse_macro_set(read_text(builtins_path)), parse_macro_set(read_text(stdlib_path))};
    if (!result.builtins || !result.stdlib || macro_count(*result.builtins) == 0) {
        sets_value = std::nullopt;
        return std::nullopt;
    }
    sets_value = result;
    return result;
}

} // namespace
#endif

InHouseOutput preprocess_in_house(std::string_view source, const CompileOptions& options) {
#ifdef _WIN32
    const auto sets = predefined_macros(options);
    if (!sets) return {false, {}, "clang's predefined macros are unavailable"};
    return preprocess_in_house(source, *sets->builtins, *sets->stdlib, options.macros);
#else
    (void)source;
    (void)options;
    return {false, {}, "not implemented on this host"};
#endif
}

ClangOutput preprocess(std::string_view source, const CompileOptions& options) {
    // ALCHEMY_MSL_PP=clang: always run clang -E.
    static const bool clang_only = environment("ALCHEMY_MSL_PP") == "clang";
    if (!clang_only) {
        auto in_house = preprocess_in_house(source, options);
        if (in_house.handled) {
            ClangOutput result;
            result.ok = true;
            result.output = std::move(in_house.output);
            return result;
        }
        static const bool trace = !environment("ALCHEMY_MSL_TIMING").empty();
        if (trace) std::cerr << "alchemy-msl: " << options.label << " preprocessed by clang: " << in_house.reason << '\n';
    }
    return preprocess_with_clang(source, options);
}

#ifdef _WIN32
namespace {

// Arguments shared by the IR compile and the precompiled metal_stdlib: a PCH
// is only valid with the language options it was built with.
std::vector<std::string> ir_arguments(const CompileOptions& options) {
    CompileOptions base = options;
    base.macros.clear();   // user macros are passed after (allowed with a PCH)
    auto args = common_arguments(base);
    // No memset/memcpy formed from loops: their lengths are often run-time
    // values (zero-filling the rest of a private array), which the lowering
    // cannot express; the loops lower as they are.
    for (const auto* arg : {"-O2", "-fno-vectorize", "-fno-slp-vectorize", "-Wno-everything", "-mllvm",
             "-disable-loop-idiom-all"})
        args.push_back(arg);
    if (options.fast_math) args.push_back("-cl-fast-relaxed-math");
    return args;
}

// metal_stdlib precompiled once per (language version, fast math) and clang
// build, next to the materialised stdlib: it saves parsing the library in
// every compile. Built under a private name and renamed (other processes may
// race); empty when it cannot be built (the compile then includes
// metal_stdlib as text). ALCHEMY_MSL_PCH=0 disables it.
std::filesystem::path stdlib_pch(const CompileOptions& options) {
    static const bool disabled = environment("ALCHEMY_MSL_PCH") == "0";
    if (disabled) return {};
    const auto version = options.language_major * 100 + options.language_minor * 10;
    const auto key = std::to_string(version) + (options.fast_math ? "-fast" : "-precise");
    // One lock per configuration: a cold build of one PCH (seconds of clang)
    // no longer blocks compiles of other language versions, nor their lookups.
    struct Built { std::mutex mutex; bool done{}; std::filesystem::path path; };
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<Built>> built;
    std::shared_ptr<Built> entry;
    {
        std::lock_guard lock(mutex);
        auto& slot = built[key];
        if (!slot) slot = std::make_shared<Built>();
        entry = slot;
    }
    std::lock_guard entry_lock(entry->mutex);
    if (entry->done) return entry->path;
    entry->done = true;   // a failed build is not retried (entry->path stays empty)
    std::ostringstream name;
    name << "metal_stdlib-" << key << '-' << std::hex << fnv1a(alchemy::msl::toolchain().version + clang_path())
         << ".pch";
    const auto path = stdlib_directory() / name.str();
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        auto staging = path;
        staging += ".tmp" + std::to_string(host::process::current_pid());
        auto args = ir_arguments(options);
        for (const auto* arg : {"-c", "-Xclang", "-emit-pch", "-o"}) args.push_back(arg);
        args.push_back(staging.string());
        args.push_back((stdlib_directory() / "metal_stdlib").string());
        const auto process = run_process(clang_path(), args, {});
        if (process.exit_code != 0) {
            std::filesystem::remove(staging, error);
            return {};
        }
        std::filesystem::rename(staging, path, error);
        if (error) std::filesystem::remove(staging, error);   // another process won
    }
    entry->path = path;
    return path;
}

} // namespace
#endif

std::uint64_t ir_configuration_hash(const CompileOptions& options) {
#ifdef _WIN32
    std::uint64_t hash = fnv1a(alchemy::msl::toolchain().version + "\n" + clang_path());
    for (const auto& arg : ir_arguments(options)) hash = fnv1a(arg + "\n", hash);
    return hash;
#else
    (void)options;
    return 0;
#endif
}

ClangOutput build_pch(std::string_view header, const std::filesystem::path& output, const CompileOptions& options) {
    ClangOutput result;
#ifdef _WIN32
    auto staging = output;
    staging += ".tmp" + std::to_string(host::process::current_pid()) + "-" + std::to_string(host::process::current_thread_id());
    auto args = ir_arguments(options);
    for (const auto* arg : {"-c", "-Xclang", "-emit-pch", "-o"}) args.push_back(arg);
    args.push_back(staging.string());
    args.push_back("-");
    const auto process = run_process(clang_path(), args, header);
    bool had_error = process.exit_code != 0;
    result.diagnostics = parse_diagnostics(process.err, had_error);
    std::error_code error;
    if (had_error) {
        std::filesystem::remove(staging, error);
        return result;
    }
    std::filesystem::rename(staging, output, error);
    if (error) {
        std::filesystem::remove(staging, error);   // another process won (or uses it): keep theirs
        if (!std::filesystem::exists(output, error)) return result;
    }
    result.ok = true;
#else
    (void)header;
    (void)output;
    (void)options;
#endif
    return result;
}

ClangOutput compile_to_ir(std::string_view source, const CompileOptions& options, const std::filesystem::path& prelude) {
    ClangOutput result;
#ifdef _WIN32
    auto args = ir_arguments(options);
    for (const auto& [name, value] : options.macros) args.push_back("-D" + name + "=" + value);
    for (const auto* arg : {"-S", "-emit-llvm", "-o", "-"}) args.push_back(arg);
    if (!prelude.empty()) {
        args.push_back("-include-pch");
        args.push_back(prelude.string());
    } else if (const auto pch = stdlib_pch(options); !pch.empty()) {
        args.push_back("-include-pch");
        args.push_back(pch.string());
    } else {
        args.push_back("-include");
        args.push_back("metal_stdlib");
    }
    // ALCHEMY_MSL_CLANG_EXTRA: extra IR-compile arguments (compiler debugging).
    static const std::string extra = environment("ALCHEMY_MSL_CLANG_EXTRA");
    for (std::size_t at = 0; at < extra.size();) {
        const auto end = std::min(extra.find(' ', at), extra.size());
        if (end > at) args.push_back(extra.substr(at, end - at));
        at = end + 1;
    }
    args.push_back("-");
    auto process = run_clang_warm(args, source);
    if (!prelude.empty()) {
        if (process.exit_code != 0 && (process.err.find("precompiled header") != std::string::npos ||
                                       process.err.find("PCH file") != std::string::npos ||
                                       process.err.find("AST file") != std::string::npos)) {
            result.pch_rejected = true;
            return result;
        }
    } else if (process.exit_code != 0 && process.err.find("precompiled header") != std::string::npos) {
        // A PCH clang refuses (built by another clang): include the text.
        const auto at = std::find(args.begin(), args.end(), "-include-pch");
        if (at != args.end()) {
            *at = "-include";
            *(at + 1) = "metal_stdlib";
            process = run_process(clang_path(), args, source);
        }
    }
    // A clang that ended without a diagnostic (killed or crashed, or a spare
    // that failed while it waited) runs once more, cold; if it fails again its
    // exit code and stderr are the diagnostic.
    const auto has_error_diagnostic = [](const std::vector<Diagnostic>& diagnostics) {
        return std::any_of(diagnostics.begin(), diagnostics.end(),
                           [](const auto& d) { return d.severity == Diagnostic::Severity::error; });
    };
    bool had_error = process.exit_code != 0;
    result.diagnostics = parse_diagnostics(process.err, had_error);
    if (process.exit_code != 0 && !has_error_diagnostic(result.diagnostics)) {
        std::cerr << "alchemy-msl: " << (options.label.empty() ? std::string("library") : options.label)
                  << ": clang exited with code " << process.exit_code << " without a diagnostic; running it again\n";
        process = run_process(clang_path(), args, source);
        had_error = process.exit_code != 0;
        result.diagnostics = parse_diagnostics(process.err, had_error);
        if (process.exit_code != 0 && !has_error_diagnostic(result.diagnostics)) {
            Diagnostic diagnostic;
            diagnostic.message = "clang exited with code " + std::to_string(process.exit_code) +
                                 " without a diagnostic: " + process.err.substr(0, 400);
            result.diagnostics.push_back(std::move(diagnostic));
        }
    }
    result.ok = !had_error;
    result.output = process.out;
#else
    (void)source;
    (void)options;
    throw std::runtime_error("the MSL compiler front end is implemented for Windows hosts only");
#endif
    return result;
}

} // namespace alchemy::msl::detail

namespace alchemy::msl {

const Toolchain& toolchain() {
    static const Toolchain value = [] {
        Toolchain result;
        result.path = detail::clang_path();
        if (result.path.empty()) {
            result.error = "no clang configured (set ALCHEMY_MSL_CLANG or the ALCHEMY_CLANG CMake cache entry)";
            return result;
        }
#ifdef _WIN32
        try {
            const auto process = detail::run_process(result.path, {"--version"}, {});
            if (process.exit_code != 0) {
                result.error = result.path + " --version failed";
                return result;
            }
            result.version = process.out.substr(0, process.out.find_first_of("\r\n"));
            result.available = result.version.find("clang version") != std::string::npos;
            if (!result.available) result.error = result.path + " is not clang";
        } catch (const std::exception& failure) {
            result.error = failure.what();
        }
#else
        result.error = "the MSL compiler front end is implemented for Windows hosts only";
#endif
        return result;
    }();
    return value;
}

} // namespace alchemy::msl
