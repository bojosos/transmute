#include "alchemy/msl/compiler.hpp"

#include "cache.hpp"
#include "frontend.hpp"
#include "llvm_ir.hpp"
#include "lower.hpp"
#include "prelude.hpp"
#include "rewriter.hpp"

#include "alchemy/perf.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

// MSL -> SPIR-V compiler driver (owner: A6). See include/alchemy/msl/compiler.hpp
// and README.md: clang -E -> rewriter -> clang -O2 -emit-llvm -> IR parser ->
// per entry point structurizer + lowering.
namespace alchemy::msl {
namespace {

Diagnostic error(std::string message, std::uint32_t line = 0) {
    Diagnostic result;
    result.severity = Diagnostic::Severity::error;
    result.message = std::move(message);
    result.line = line;
    return result;
}

void append(std::vector<Diagnostic>& into, std::vector<Diagnostic> from) {
    for (auto& diagnostic : from) into.push_back(std::move(diagnostic));
}

bool has_error(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics)
        if (diagnostic.severity == Diagnostic::Severity::error) return true;
    return false;
}

// ALCHEMY_MSL_KEEP=<directory>: keep each compile's intermediate files
// (<label>-<hash>.{pre,rewritten.cpp,ll}) for debugging the compiler.
void keep(std::string_view source, const CompileOptions& options, const char* suffix, std::string_view text) {
    static const std::string directory = detail::environment("ALCHEMY_MSL_KEEP");
    if (directory.empty()) return;
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    std::ostringstream name;
    name << (options.label.empty() ? std::string("library") : options.label) << '-' << std::hex << detail::fnv1a(source)
         << suffix;
    std::ofstream(std::filesystem::path(directory) / name.str(), std::ios::binary) << text;
}

// ALCHEMY_MSL_TIMING=1: one stderr line per uncached compile with the time
// spent in each phase.
class PhaseTimer {
public:
    explicit PhaseTimer(const CompileOptions& options) : options_(options) {}
    ~PhaseTimer() {
        if (!enabled()) return;
        std::ostringstream line;
        line << "alchemy-msl: " << (options_.label.empty() ? std::string("library") : options_.label);
        for (const auto& [phase, ms] : phases_) line << ' ' << phase << '=' << ms << "ms";
        line << '\n';
        std::cerr << line.str();
    }
    void mark(const char* phase) {
        const auto now = std::chrono::steady_clock::now();
        phases_.emplace_back(phase, std::chrono::duration<double, std::milli>(now - last_).count());
        last_ = now;
    }
    static bool enabled() {
        static const bool value = !detail::environment("ALCHEMY_MSL_TIMING").empty();
        return value;
    }

private:
    const CompileOptions& options_;
    std::chrono::steady_clock::time_point last_{std::chrono::steady_clock::now()};
    std::vector<std::pair<const char*, double>> phases_;
};

CompileResult compile_uncached(std::string_view source, const CompileOptions& options) {
    CompileResult result;
    PhaseTimer timer(options);
    const auto preprocessed = detail::preprocess(source, options);
    timer.mark("preprocess");
    append(result.diagnostics, preprocessed.diagnostics);
    if (!preprocessed.ok) {
        if (!has_error(result.diagnostics)) result.diagnostics.push_back(error("preprocessing failed"));
        return result;
    }
    keep(source, options, ".pre", preprocessed.output);
    auto rewritten = detail::rewrite(preprocessed.output);
    timer.mark("rewrite");
    keep(source, options, ".rewritten.cpp", rewritten.source);
    append(result.diagnostics, std::move(rewritten.diagnostics));
    if (has_error(result.diagnostics)) return result;
    const auto ir_text = detail::compile_rewritten(rewritten, options);
    timer.mark("clang");
    append(result.diagnostics, ir_text.diagnostics);
    if (!ir_text.ok) {
        if (!has_error(result.diagnostics)) result.diagnostics.push_back(error("compilation failed"));
        return result;
    }
    keep(source, options, ".ll", ir_text.output);
    std::unique_ptr<ir::Module> module;
    try {
        module = ir::parse(ir_text.output);
        timer.mark("parse");
    } catch (const ir::ParseError& failure) {
        result.diagnostics.push_back(error(std::string("internal: unsupported LLVM IR from the front end: ") + failure.what()));
        return result;
    }
    for (const auto& entry : rewritten.entries) {
        auto lowered = detail::lower(*module, entry, rewritten.function_constants, options);
        const bool failed = has_error(lowered.diagnostics);
        append(result.diagnostics, std::move(lowered.diagnostics));
        if (failed) continue;
        Function function;
        function.name = entry.name;
        function.stage = entry.stage;
        function.spirv = std::move(lowered.spirv);
        function.reflection = std::move(lowered.reflection);
        result.functions.push_back(std::move(function));
    }
    timer.mark("lower");
    return result;
}

// ---- SPIR-V patching for link() -------------------------------------------------------
constexpr std::uint32_t spv_op_decorate = 71, spv_op_variable = 59, spv_decoration_location = 30;
constexpr std::uint32_t spv_storage_input = 1;

// Rewrites the Location decorations of Input variables: old location -> new.
void remap_input_locations(std::vector<std::uint32_t>& spirv, const std::map<std::uint32_t, std::uint32_t>& remap) {
    if (spirv.size() < 5 || remap.empty()) return;
    std::map<std::uint32_t, bool> inputs;   // variable id -> is Input
    for (std::size_t i = 5; i < spirv.size();) {
        const auto count = spirv[i] >> 16, opcode = spirv[i] & 0xffff;
        if (count == 0) break;
        if (opcode == spv_op_variable && count >= 4 && spirv[i + 3] == spv_storage_input) inputs[spirv[i + 2]] = true;
        i += count;
    }
    for (std::size_t i = 5; i < spirv.size();) {
        const auto count = spirv[i] >> 16, opcode = spirv[i] & 0xffff;
        if (count == 0) break;
        if (opcode == spv_op_decorate && count == 4 && spirv[i + 2] == spv_decoration_location &&
            inputs.count(spirv[i + 1])) {
            const auto found = remap.find(spirv[i + 3]);
            if (found != remap.end()) spirv[i + 3] = found->second;
        }
        i += count;
    }
}

bool same_shape(const Type& a, const Type& b) {
    return a.kind == b.kind && a.components == b.components && a.columns == b.columns;
}

} // namespace

std::string compiler_version() {
    // Bump the leading number on any lowering change; the stdlib fingerprint
    // and the front end's version are folded in automatically.
    static const std::string value = [] {
        std::ostringstream out;
        out << "alchemy-msl 8 stdlib-" << std::hex << detail::stdlib_fingerprint();
        const auto& tools = toolchain();
        if (tools.available) out << ' ' << tools.version;
        return out.str();
    }();
    return value;
}

CompileResult compile(std::string_view source, const CompileOptions& options) {
    (void)dump_source(source, options);
    perf::phase("first MSL compile requested");
    std::string key;
    std::optional<CompileResult> cached;
    {
        perf::Scope timed(perf::msl_lookup_ticks);
        key = detail::cache_key(source, options);
        cached = detail::cache_lookup(key);
    }
    if (cached) {
        perf::add(perf::msl_cache_hits);
        return std::move(*cached);
    }
    if (!toolchain().available) throw std::runtime_error("MSL compiler toolchain unavailable: " + toolchain().error);
    perf::Scope timed(perf::msl_compile_ticks, perf::msl_compiles);
    auto result = compile_uncached(source, options);
    detail::cache_store(key, result);
    return result;
}

std::size_t preload_cache() { return detail::cache_preload(); }

void set_seed_directory(const std::filesystem::path& directory) { detail::set_seed_directory(directory); }

std::size_t seed_corpus(const std::filesystem::path& corpus, unsigned threads, std::uint32_t subgroup_size,
                        std::string& report) {
    std::vector<std::filesystem::path> sources;
    for (const auto& item : std::filesystem::directory_iterator(corpus))
        if (item.path().extension() == ".metal") sources.push_back(item.path());
    std::sort(sources.begin(), sources.end());
    std::atomic<std::size_t> next{0}, compiled{0};
    std::mutex report_mutex;
    const auto work = [&] {
        for (;;) {
            const auto index = next.fetch_add(1);
            if (index >= sources.size()) return;
            const auto& source = sources[index];
            try {
                auto options = read_dump_options(source);
                auto options_path = source;
                options_path.replace_extension(".options");
                std::ifstream options_file(options_path, std::ios::binary);
                const std::string options_text((std::istreambuf_iterator<char>(options_file)), std::istreambuf_iterator<char>());
                if (options_text.find("subgroup_size ") == std::string::npos) options.subgroup_size = subgroup_size;
                std::ifstream file(source, std::ios::binary);
                const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                const auto result = compile(text, options);
                if (result.success()) ++compiled;
                else {
                    std::lock_guard lock(report_mutex);
                    report += source.filename().string() + ": " + format_diagnostics(result.diagnostics);
                }
            } catch (const std::exception& failure) {
                std::lock_guard lock(report_mutex);
                report += source.filename().string() + ": " + failure.what() + "\n";
            }
        }
    };
    std::vector<std::thread> pool;
    for (unsigned i = 1; i < std::max(1U, threads); ++i) pool.emplace_back(work);
    work();
    for (auto& thread : pool) thread.join();
    return compiled.load();
}

LinkResult link(const Function& vertex, const Function& fragment) {
    LinkResult result{vertex, fragment, {}};
    if (vertex.stage != Stage::vertex || fragment.stage != Stage::fragment) {
        result.diagnostics.push_back(error("link() needs a vertex and a fragment function"));
        return result;
    }
    const auto& outputs = vertex.reflection.outputs;
    std::map<std::uint32_t, std::uint32_t> remap;
    for (auto& input : result.fragment.reflection.inputs) {
        const Varying* match = nullptr;
        for (const auto& output : outputs)
            if (output.name == input.name) match = &output;
        if (!match) {
            result.diagnostics.push_back(error("fragment input '" + input.name + "' of '" + fragment.name +
                                               "' is not written by vertex function '" + vertex.name + "'"));
            continue;
        }
        const auto& in_types = fragment.reflection.types;
        const auto& out_types = vertex.reflection.types;
        if (input.type < in_types.size() && match->type < out_types.size() &&
            !same_shape(in_types[input.type], out_types[match->type])) {
            result.diagnostics.push_back(error("varying '" + input.name + "' is " + out_types[match->type].name +
                                               " in '" + vertex.name + "' but " + in_types[input.type].name + " in '" +
                                               fragment.name + "'"));
            continue;
        }
        if (input.location != match->location) remap[input.location] = match->location;
        input.location = match->location;
    }
    if (!has_error(result.diagnostics)) remap_input_locations(result.fragment.spirv, remap);
    return result;
}

std::string format_diagnostics(const std::vector<Diagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << "program_source";
        if (diagnostic.line) out << ':' << diagnostic.line << ':' << diagnostic.column;
        switch (diagnostic.severity) {
        case Diagnostic::Severity::note: out << ": note: "; break;
        case Diagnostic::Severity::warning: out << ": warning: "; break;
        case Diagnostic::Severity::error: out << ": error: "; break;
        }
        out << diagnostic.message << '\n';
    }
    return out.str();
}

} // namespace alchemy::msl
