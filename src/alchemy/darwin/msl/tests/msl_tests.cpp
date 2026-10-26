// MSL compiler tests (owner: A6). One executable, one ctest per group:
//   rewriter    source rewriter and type-spelling unit tests (no clang)
//   fixtures    every tests/fixtures/*.metal compiles; each function's SPIR-V
//               passes spirv-val (vulkan1.3); per-shader compile times and a
//               parallel compile of the whole set
//   reflection  per-fixture reflection against the fixed resource model
//   link        vertex/fragment varying linking (remap and errors)
//   cache       memory/disk result cache and its serialisation
//   dump        ALCHEMY_MSL_DUMP: sources with their options, metallibs byte for byte
//   preprocessor the in-process preprocessor equals clang -E (tokens and
//               lines) on synthetic cases, the fixtures and the corpus
//   corpus      every library of the dumped application corpus (outside the
//               repository: ALCHEMY_MSL_CORPUS) compiles and passes spirv-val,
//               with per-library cold compile times; skipped without a corpus
//   chrome_corpus the same for Chrome's dumps (ALCHEMY_MSL_CHROME_CORPUS: one
//               directory per run below it, deduplicated by fingerprint)
//   vulkan     a compute fixture run end to end on a Vulkan device
//               (msl_vulkan_test.cpp; skipped with a message without one)
// `alchemy_msl_tests compile <file.metal> [out-dir]` compiles a file (with its
// ALCHEMY_MSL_DUMP .options when present) and prints diagnostics, reflection
// and timings (debugging aid).
#include "msl_test_support.hpp"

#include "cache.hpp"
#include "frontend.hpp"
#include "prelude.hpp"
#include "rewriter.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>

namespace msl = alchemy::msl;
namespace fs = std::filesystem;
using namespace alchemy::msl::test;

namespace {

// ---- helpers ---------------------------------------------------------------------------
const msl::Function& function(const msl::CompileResult& result, std::string_view name) {
    for (const auto& f : result.functions)
        if (f.name == name) return f;
    throw std::runtime_error("no function '" + std::string(name) + "' in the compile result");
}

const msl::BufferBinding& buffer(const msl::Reflection& r, std::uint32_t index, bool dynamic = false) {
    for (const auto& b : r.buffers)
        if (b.index == index && b.index_function_constant.has_value() == dynamic) return b;
    throw std::runtime_error("no buffer(" + std::to_string(index) + ") in reflection");
}
const msl::TextureBinding& texture(const msl::Reflection& r, std::uint32_t index) {
    for (const auto& t : r.textures)
        if (t.index == index) return t;
    throw std::runtime_error("no texture(" + std::to_string(index) + ") in reflection");
}
const msl::Varying& varying(const std::vector<msl::Varying>& list, std::string_view name) {
    for (const auto& v : list)
        if (v.name == name) return v;
    throw std::runtime_error("no varying '" + std::string(name) + "'");
}
bool has_builtin(const msl::Reflection& r, msl::Builtin b) {
    return std::find(r.builtins.begin(), r.builtins.end(), b) != r.builtins.end();
}
const msl::Type& type_of(const msl::Reflection& r, std::uint32_t index) { return r.types.at(index); }
bool has_constant(const msl::Reflection& r, std::uint32_t index) {
    for (const auto& c : r.function_constants)
        if (c.index == index) return true;
    return false;
}

bool spirv_has_opcode(const std::vector<std::uint32_t>& spirv, std::uint32_t opcode) {
    for (std::size_t i = 5; i < spirv.size();) {
        const auto count = spirv[i] >> 16;
        if (!count) break;
        if ((spirv[i] & 0xffff) == opcode) return true;
        i += count;
    }
    return false;
}

// Every SpecId decoration's value (OpDecorate <id> SpecId n).
std::set<std::uint32_t> spirv_spec_ids(const std::vector<std::uint32_t>& spirv) {
    std::set<std::uint32_t> ids;
    for (std::size_t i = 5; i < spirv.size();) {
        const auto count = spirv[i] >> 16, op = spirv[i] & 0xffff;
        if (!count) break;
        if (op == 71 && count == 4 && spirv[i + 2] == 1) ids.insert(spirv[i + 3]);
        i += count;
    }
    return ids;
}

// Location decorations of the Input (storage class 1) variables, by variable id.
std::map<std::uint32_t, std::uint32_t> input_locations(const std::vector<std::uint32_t>& spirv) {
    std::set<std::uint32_t> inputs;
    std::map<std::uint32_t, std::uint32_t> locations, result;
    for (std::size_t i = 5; i < spirv.size();) {
        const auto count = spirv[i] >> 16, op = spirv[i] & 0xffff;
        if (!count) break;
        if (op == 59 && spirv[i + 3] == 1) inputs.insert(spirv[i + 2]);
        if (op == 71 && count == 4 && spirv[i + 2] == 30) locations[spirv[i + 1]] = spirv[i + 3];
        i += count;
    }
    for (const auto& [id, location] : locations)
        if (inputs.count(id)) result[id] = location;
    return result;
}

// ---- rewriter --------------------------------------------------------------------------
void test_rewriter() {
    using namespace alchemy::msl::detail;
    {
        const auto t = parse_type_spelling("float4");
        require(t.kind == msl::Type::Kind::vector && t.scalar == msl::Scalar::float32 && t.components == 4, "float4");
        const auto p = parse_type_spelling("packed_half3");
        require(p.kind == msl::Type::Kind::vector && p.packed && p.scalar == msl::Scalar::half && p.components == 3,
                "packed_half3");
        const auto m = parse_type_spelling("float3x4");
        require(m.kind == msl::Type::Kind::matrix && m.columns == 3 && m.components == 4, "float3x4 is 3 columns of 4");
        const auto tex = parse_type_spelling("texture2d_array<half, access::read>");
        require(tex.kind == msl::Type::Kind::texture && tex.texture == msl::TextureType::texture2d_array &&
                    tex.scalar == msl::Scalar::half && tex.access == msl::Access::read,
                "texture2d_array<half, access::read>");
        const auto depth = parse_type_spelling("depth2d<float>");
        require(depth.texture == msl::TextureType::depth2d && depth.access == msl::Access::sample, "depth2d<float>");
        const auto arr = parse_type_spelling("array<texturecube<float>, 4>");
        require(arr.kind == msl::Type::Kind::texture && arr.texture == msl::TextureType::texture_cube &&
                    arr.array_length == 4,
                "array<texturecube<float>, 4>");
        require(parse_type_spelling("const device uint&").scalar == msl::Scalar::uint32, "cv/address space stripped");
    }
    {
        // Already "preprocessed" source: the rewriter runs on clang -E output.
        const char* source =
            "struct P { float4 v; };\n"
            "constant int base [[function_constant(4)]];\n"
            "constant bool fancy [[function_constant(7)]];\n"
            "struct VIn { float3 pos [[attribute(0)]]; uint id [[attribute(3)]]; };\n"
            "struct VOut { float4 p [[position]]; float2 uv [[user(texcoord)]]; int f [[flat]]; };\n"
            "vertex VOut vs(VIn in [[stage_in]], constant P& p [[buffer(base + 1)]],\n"
            "               device float* data [[buffer(2), function_constant(fancy)]],\n"
            "               uint vid [[vertex_id]]) {\n"
            "    VOut o; o.p = p.v; o.uv = float2(in.pos.xy); o.f = int(vid); return o;\n"
            "}\n"
            "kernel void k(device atomic_uint* c [[buffer(0)]], texture2d<half, access::read_write> t [[texture(3)]],\n"
            "              sampler s [[sampler(1)]], threadgroup float* tg [[threadgroup(2)]],\n"
            "              uint3 gid [[thread_position_in_grid]]) {\n"
            "    int read_write = 1; (void)read_write;\n"
            "}\n";
        const auto result = rewrite(source);
        require(result.diagnostics.empty(), "rewrite diagnostics: " + msl::format_diagnostics(result.diagnostics));
        require(result.entries.size() == 2, "two entry points");
        const auto& vs = result.entries[0];
        require(vs.stage == msl::Stage::vertex && vs.name == "vs" && vs.line == 6, "vertex entry name/line");
        require(vs.attributes.size() == 2 && vs.attributes[1].location == 3, "stage_in attribute locations");
        require(vs.buffers.size() == 2, "vertex buffers");
        require(vs.buffers[0].index == 1 && vs.buffers[0].index_constant == 4u && vs.buffers[0].reference &&
                    vs.buffers[0].space == msl::BufferBinding::Space::constant,
                "[[buffer(fc + n)]] parameter");
        require(vs.buffers[1].index == 2 && vs.buffers[1].function_constant == 7u && !vs.buffers[1].reference,
                "[[function_constant]] buffer parameter");
        require(vs.outputs.size() == 2 && vs.outputs[0].name == "texcoord" && vs.outputs[1].name == "f" &&
                    vs.outputs[1].interpolation == msl::Interpolation::flat,
                "vertex varyings named by [[user]] or member name, flat");
        require(vs.builtin_inputs.size() == 1 && vs.builtin_inputs[0].builtin == msl::Builtin::vertex_id, "vertex_id");
        const auto& k = result.entries[1];
        require(k.stage == msl::Stage::kernel && k.textures.size() == 1 && k.textures[0].index == 3 &&
                    k.textures[0].type.access == msl::Access::read_write,
                "kernel texture access::read_write");
        require(k.samplers.size() == 1 && k.samplers[0].index == 1, "sampler index");
        require(k.threadgroups.size() == 1 && k.threadgroups[0].index == 2, "threadgroup parameter");
        require(result.function_constants.size() == 2 && result.function_constants[0].index == 4, "function constants");
        const auto& out = result.source;
        require(out.find("[[") == std::string::npos, "attributes are consumed");
        require(out.find("__msl_entry_0") != std::string::npos && out.find("__msl_entry_1") != std::string::npos,
                "entry wrappers generated");
        require(out.find("__msl_id_read_write") != std::string::npos, "OpenCL keyword renamed");
        require(out.find("__msl_id_atomic_uint") != std::string::npos, "OpenCL atomic typedef renamed");
        require(out.find("__global") != std::string::npos, "device/constant address spaces mapped");
        // User code stays in the global namespace (as in Metal, so user
        // overloads of stdlib names do not hide metal::*); internal linkage
        // comes from a clang attribute pragma.
        require(out.find("namespace {") == std::string::npos, "user code is not wrapped in a namespace");
        require(out.find("#pragma clang attribute push") != std::string::npos &&
                    out.find("#pragma clang attribute pop") < out.find("__msl_entry_0"),
                "user code has internal linkage, the wrappers do not");
        // Entry 0 is the vertex function, entry 1 the kernel.
        const auto kernel_wrapper = out.find("void __msl_entry_1");
        const auto guard = out.find("__msl_kernel_outside_grid");
        require(kernel_wrapper != std::string::npos && guard != std::string::npos && guard > kernel_wrapper,
                "only kernel wrappers start with the grid guard");
    }
    {
        // Conditional operands become prvalues; a top-level '<'/'>' in the
        // third operand leaves that conditional alone.
        const auto r = rewrite("float2 f(bool c, int i) { const float2 a[2] = {float2(0), float2(1)};\n"
                               "  float2 p = c ? float2(0.0f) : a[i];\n"
                               "  int n = c ? 1 : i > 2 ? 3 : 4;\n"
                               "  bool b = c ? i : i < 2;\n"
                               "  return p; }\n");
        require(r.diagnostics.empty(), "conditional rewrite diagnostics");
        const auto& out = r.source;
        require(out.find("c ? (metal::__msl_rv{}, (float2(0.0f))) : (metal::__msl_rv{}, (a[i]))") != std::string::npos,
                "class prvalue / lvalue conditional operands wrapped");
        require(out.find("(metal::__msl_rv{}, (3)) : (metal::__msl_rv{}, (4))") != std::string::npos,
                "nested conditional wrapped");
        require(out.find("c ? i : i < 2") != std::string::npos, "conditional with a top-level '<' left alone");
    }
    {
        const auto bad = rewrite("kernel void k(device float* a) {}\n");
        require(!bad.diagnostics.empty() && bad.diagnostics[0].line == 1 &&
                    bad.diagnostics[0].message.find("has no MSL attribute") != std::string::npos,
                "parameter without attribute is an error");
        const auto stage = rewrite("kernel void k(uint v [[vertex_id]]) {}\n");
        require(!stage.diagnostics.empty() &&
                    stage.diagnostics[0].message.find("not an input of this stage") != std::string::npos,
                "builtin on the wrong stage is an error");
    }
    {
        // constant and device both become __global: Tint's tint_array pair of
        // operator[] overloads collapses into one; different bodies fail.
        const auto r = rewrite("template<typename T, size_t N> struct tint_array {\n"
                               "  const constant T& operator[](size_t i) const constant { return elements[i]; }\n"
                               "  const device T& operator[](size_t i) const device { return elements[i]; }\n"
                               "  T elements[N];\n"
                               "};\n"
                               "float f(const constant float* p) { return p[0]; }\n"
                               "float f(const device float* p) { return p[0]; }\n");
        require(r.diagnostics.empty(), "constant/device overloads: " + msl::format_diagnostics(r.diagnostics));
        const auto& out = r.source;
        auto count = [&](std::string_view needle) {
            std::size_t n = 0;
            for (auto at = out.find(needle); at != std::string::npos; at = out.find(needle, at + 1)) ++n;
            return n;
        };
        require(count("operator[]") == 1 && count("float f(") == 1, "the device duplicate of each overload is dropped");
        const auto bad = rewrite("float f(const constant float* p) { return p[0]; }\n"
                                 "float f(const device float* p) { return p[1]; }\n");
        require(!bad.diagnostics.empty() && bad.diagnostics[0].line == 2 &&
                    bad.diagnostics[0].message.find("constant and device") != std::string::npos,
                "constant/device overloads with different bodies are an explicit error");
    }
    {
        // A program-scope constant computed from function constants is an
        // inline function at every use, including [[function_constant(...)]].
        const auto r = rewrite("constant bool a [[function_constant(0)]];\n"
                               "constant bool b [[function_constant(1)]];\n"
                               "constant bool either = a || b;\n"
                               "constant bool both = either && is_function_constant_defined(b);\n"
                               "constant float plain = 2.0f;\n"
                               "struct O { float4 c [[color(0)]]; uint m [[sample_mask, function_constant(either)]];\n"
                               "  float d [[depth(any), function_constant(a)]]; };\n"
                               "fragment O fs() { O o; o.c = float4(both ? 1.0f : plain); o.m = 1u; return o; }\n");
        require(r.diagnostics.empty(), "derived constants: " + msl::format_diagnostics(r.diagnostics));
        const auto& out = r.source;
        require(out.find("inline bool __msl_fc_derived_either() { return (  a || b); }") != std::string::npos,
                "derived constant becomes an inline function");
        require(out.find("return (  __msl_fc_derived_either() && __msl_fc_defined(1u)); }") != std::string::npos,
                "a derived constant used by another derived constant is called");
        require(out.find("float4(__msl_fc_derived_both() ?") != std::string::npos, "uses call the function");
        require(out.find("((__msl_fc_derived_either())) ? uint(__r.m) : 0xffffffffu") != std::string::npos,
                "[[function_constant(derived)]] sample mask: all samples when absent");
        require(out.find("((__msl_fc_defined(0u) && a)) ? float(__r.d) : metal::__msl_builtin_in<metal::float4, ") !=
                    std::string::npos,
                "[[depth, function_constant]]: the fragment's own depth when absent");
        require(out.find("__global const float plain = 2.0f;") != std::string::npos,
                "constants without function constants stay globals");
        require(r.function_constants.size() == 2, "derived constants are not function constants");
    }
    std::cout << "rewriter: ok\n";
}

// ---- fixtures ---------------------------------------------------------------------------
void test_fixtures() {
    const auto files = fixture_files();
    require(files.size() >= 6, "fixture directory " + fixtures_directory().string() + " has too few .metal files");
    std::size_t functions = 0;
    double total = 0;
    for (const auto& file : files) {
        msl::CompileOptions options;
        options.label = file.stem().string();
        const auto start = std::chrono::steady_clock::now();
        const auto result = msl::compile(read_file(file), options);
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        total += ms;
        require(result.success(), file.filename().string() + ":\n" + msl::format_diagnostics(result.diagnostics));
        require(!result.functions.empty(), file.filename().string() + ": no entry points");
        std::cout << file.filename().string() << ": " << result.functions.size() << " function(s), " << ms << " ms\n";
        for (const auto& f : result.functions) {
            validate_spirv(f.spirv, options.label + "." + f.name);
            std::cout << "  " << f.name << ": " << f.spirv.size() * 4 << " bytes SPIR-V, valid\n";
            ++functions;
        }
    }
    std::cout << "fixtures: " << functions << " functions, " << total << " ms serial";
    // Thread safety and throughput: the same sources with a distinct macro
    // (a cache miss) on one thread per fixture.
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::future<msl::CompileResult>> jobs;
    for (const auto& file : files) {
        jobs.push_back(std::async(std::launch::async, [file] {
            msl::CompileOptions options;
            options.macros = {{"ALCHEMY_MSL_PARALLEL_TEST", "1"}};
            return msl::compile(read_file(file), options);
        }));
    }
    for (auto& job : jobs) require(job.get().success(), "parallel compile failed");
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::cout << ", " << ms << " ms parallel (" << files.size() << " threads)\n";
}

// ---- corpus -----------------------------------------------------------------------------
// The shader corpus dumped from real applications (ALCHEMY_MSL_DUMP): every
// <name>.metal with its <name>.options (language, fast_math, defines) must
// compile and every function must pass spirv-val. The corpus is application
// output (Blender's generated MSL is GPL), so it lives outside the repository:
// the CMake cache path ALCHEMY_MSL_CORPUS, or the environment variable of the
// same name at run time. Without one the group is skipped explicitly.
struct CorpusEntry {
    fs::path file;
    msl::CompileOptions options;
};

// A dumped library's compile options: <name>.options next to <name>.metal
// (language, fast_math, preserve_invariance, define NAME=VALUE lines).
msl::CompileOptions dumped_options(const fs::path& file) {
    msl::CompileOptions options;
    options.label = file.stem().string();
    auto options_path = file;
    options_path.replace_extension(".options");
    if (!fs::exists(options_path)) return options;
    std::istringstream lines(read_file(options_path));
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto space = line.find(' ');
        if (space == std::string::npos) continue;
        const auto key = line.substr(0, space), value = line.substr(space + 1);
        if (key == "language") {
            const auto dot = value.find('.');
            options.language_major = static_cast<std::uint32_t>(std::stoul(value.substr(0, dot)));
            options.language_minor =
                dot == std::string::npos ? 0 : static_cast<std::uint32_t>(std::stoul(value.substr(dot + 1)));
        } else if (key == "fast_math") {
            options.fast_math = value == "1";
        } else if (key == "preserve_invariance") {
            options.preserve_invariance = value == "1";
        } else if (key == "define") {
            const auto equals = value.find('=');
            options.macros.emplace_back(value.substr(0, equals), equals == std::string::npos ? "1" : value.substr(equals + 1));
        }
    }
    return options;
}

std::vector<CorpusEntry> corpus_entries(const fs::path& directory) {
    std::vector<CorpusEntry> entries;
    for (const auto& item : fs::directory_iterator(directory)) {
        if (item.path().extension() != ".metal") continue;
        entries.push_back({item.path(), dumped_options(item.path())});
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.file < b.file; });
    return entries;
}

// Every run directory below `directory` (one ALCHEMY_MSL_DUMP directory per
// Chrome run), deduplicated by file name: dump names end in the source's
// FNV-1a fingerprint, so one library dumped by several runs compiles once.
// Options come from the dump's own reader (subgroup size included).
std::vector<CorpusEntry> recursive_corpus_entries(const fs::path& directory) {
    std::map<std::string, CorpusEntry> unique;
    for (const auto& item : fs::recursive_directory_iterator(directory)) {
        if (!item.is_regular_file() || item.path().extension() != ".metal") continue;
        const auto name = item.path().filename().string();
        if (!unique.count(name)) unique.emplace(name, CorpusEntry{item.path(), msl::read_dump_options(item.path())});
    }
    std::vector<CorpusEntry> entries;
    for (auto& [name, entry] : unique) entries.push_back(std::move(entry));
    return entries;
}

// A dumped corpus: `variable` (environment) overrides `configured` (the CMake
// cache path). Without a directory, or with no .metal files in it, the group
// is skipped explicitly.
struct CorpusSource {
    const char* group;          // output prefix
    const char* variable;       // environment override
    std::string configured;     // CMake cache path ("" when unset)
    bool recursive;             // run directories below the root
};

int test_corpus(const CorpusSource& source) {
    std::string directory = alchemy::msl::detail::environment(source.variable);
    if (directory.empty()) directory = source.configured;
    const std::string group = source.group;
    if (directory.empty() || !fs::is_directory(directory)) {
        std::cout << group << ": SKIPPED (no corpus directory" << (directory.empty() ? "" : " at " + directory)
                  << "; set the " << source.variable << " cache path or environment variable to a directory of "
                     "ALCHEMY_MSL_DUMP output)\n";
        return 0;
    }
    const auto entries = source.recursive ? recursive_corpus_entries(directory) : corpus_entries(directory);
    if (entries.empty()) {
        std::cout << group << ": SKIPPED (no .metal files in " << directory << ")\n";
        return 0;
    }
    struct Outcome {
        double ms{};
        std::size_t functions{};
        std::string failure;
    };
    std::vector<Outcome> outcomes(entries.size());
    std::atomic<std::size_t> next{0};
    auto threads = std::max(1U, std::thread::hardware_concurrency());
    if (const auto value = alchemy::msl::detail::environment("ALCHEMY_MSL_CORPUS_THREADS"); !value.empty())
        threads = std::max(1U, static_cast<unsigned>(std::stoul(value)));
    threads = std::min<unsigned>(threads, static_cast<unsigned>(entries.size()));
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            for (auto i = next++; i < entries.size(); i = next++) {
                auto& outcome = outcomes[i];
                const auto& entry = entries[i];
                try {
                    const auto begin = std::chrono::steady_clock::now();
                    const auto result = msl::compile(read_file(entry.file), entry.options);
                    outcome.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
                    if (!result.success()) {
                        outcome.failure = msl::format_diagnostics(result.diagnostics);
                        continue;
                    }
                    if (result.functions.empty()) {
                        outcome.failure = "no entry points\n";
                        continue;
                    }
                    for (const auto& f : result.functions) {
                        validate_spirv(f.spirv, entry.options.label + "." + f.name);
                        ++outcome.functions;
                    }
                } catch (const std::exception& error) {
                    outcome.failure += std::string(error.what()) + '\n';
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();
    const auto wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::size_t passed = 0, functions = 0;
    double serial = 0, slowest = 0;
    std::ostringstream failures;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& outcome = outcomes[i];
        serial += outcome.ms;
        slowest = std::max(slowest, outcome.ms);
        std::cout << entries[i].file.filename().string() << ": ";
        if (outcome.failure.empty()) {
            ++passed;
            functions += outcome.functions;
            std::cout << outcome.functions << " function(s), " << outcome.ms << " ms\n";
        } else {
            std::cout << "FAILED (" << outcome.ms << " ms)\n";
            failures << "--- " << entries[i].file.filename().string() << '\n' << outcome.failure;
        }
    }
    std::cout << group << ": " << passed << '/' << entries.size() << " libraries, " << functions
              << " functions spirv-val clean; " << serial << " ms summed compile time, slowest " << slowest
              << " ms, " << wall << " ms wall on " << threads << " threads (" << directory << ")\n";
    if (passed != entries.size()) {
        std::cerr << failures.str();
        throw std::runtime_error(std::to_string(entries.size() - passed) + " " + group + " libraries failed");
    }
    // Precompiled preludes change nothing: a sample of libraries compiles to
    // the same SPIR-V through a prelude PCH and as whole sources.
    const auto stats = alchemy::msl::detail::prelude_statistics();
    std::cout << group << ": preludes used by " << stats.hits << " compiles, " << stats.misses << " whole-source compiles, "
              << stats.built << " precompiled, " << stats.rejected << " rejected\n";
    if (stats.hits) {
        std::size_t compared = 0;
        for (std::size_t i = 0; i < entries.size() && compared < 8; i += std::max<std::size_t>(1, entries.size() / 8)) {
            const auto source = read_file(entries[i].file);
            alchemy::msl::detail::clear_memory_cache();   // the results are cached in memory
            const auto before = alchemy::msl::detail::prelude_statistics().hits;
            const auto with = msl::compile(source, entries[i].options);
            if (alchemy::msl::detail::prelude_statistics().hits == before) continue;   // no prelude for this one
            alchemy::msl::detail::set_preludes_enabled(false);
            alchemy::msl::detail::clear_memory_cache();
            const auto without = msl::compile(source, entries[i].options);
            alchemy::msl::detail::set_preludes_enabled(true);
            require(with.functions.size() == without.functions.size(), "prelude: same functions");
            for (std::size_t f = 0; f < with.functions.size(); ++f)
                require(with.functions[f].spirv == without.functions[f].spirv,
                        entries[i].file.filename().string() + ": the prelude PCH changes the SPIR-V of " +
                            with.functions[f].name);
            ++compared;
        }
        require(compared > 0, "prelude: at least one library compared");
        std::cout << group << ": " << compared
                  << " libraries compile to identical SPIR-V with and without their prelude\n";
    }
    return 0;
}

// Blender's corpus (artifacts/alchemy-blender/msl-corpus, flat).
int test_blender_corpus() {
#ifdef ALCHEMY_MSL_CORPUS_DIR
    const std::string configured = ALCHEMY_MSL_CORPUS_DIR;
#else
    const std::string configured;
#endif
    return test_corpus({"corpus", "ALCHEMY_MSL_CORPUS", configured, false});
}

// Chrome's corpus (artifacts/chrome-msl-corpus/<run>/: what Dawn/Tint, Skia
// Graphite and ANGLE hand to newLibraryWithSource:, one ALCHEMY_MSL_DUMP
// directory per Chrome run). Chrome's generated shaders are application
// output and never enter the repository either.
int test_chrome_corpus() {
#ifdef ALCHEMY_MSL_CHROME_CORPUS_DIR
    const std::string configured = ALCHEMY_MSL_CHROME_CORPUS_DIR;
#else
    const std::string configured;
#endif
    return test_corpus({"chrome_corpus", "ALCHEMY_MSL_CHROME_CORPUS", configured, true});
}

// ---- preprocessor -----------------------------------------------------------------------
// The in-process preprocessor against clang -E: the same tokens on the same
// lines of the same files (line markers interpreted), for synthetic cases,
// every fixture and (when configured) every corpus library.
struct PpToken {
    std::string file;
    std::uint32_t line{};
    std::string text;
    bool operator==(const PpToken& o) const { return file == o.file && line == o.line && text == o.text; }
};

std::vector<PpToken> pp_tokens(const std::string& text) {
    static const char* const puncts[] = {"<<=", ">>=", "...", "->*", "::", "->", "++", "--", "<<", ">>", "<=", ">=",
        "==", "!=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##", ".*"};
    std::vector<PpToken> out;
    std::string file = "<stdin>";
    std::uint32_t line = 1;
    std::size_t i = 0;
    bool bol = true;
    auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$'; };
    while (i < text.size()) {
        const char c = text[i];
        if (c == '\n') { ++line; ++i; bol = true; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
        if (bol && c == '#') {
            // "# N "file" flags": the next line is N of file.
            std::size_t j = i + 1;
            while (j < text.size() && text[j] == ' ') ++j;
            if (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) {
                std::size_t k = j;
                while (k < text.size() && std::isdigit(static_cast<unsigned char>(text[k]))) ++k;
                const auto number = static_cast<std::uint32_t>(std::stoul(text.substr(j, k - j)));
                const auto q = text.find('"', k);
                const auto e = text.find('\n', k);
                if (q != std::string::npos && q < e) file = text.substr(q + 1, text.find('"', q + 1) - q - 1);
                i = e == std::string::npos ? text.size() : e + 1;
                line = number;
                continue;
            }
        }
        bol = false;
        std::size_t j = i + 1;
        if (ident(c)) {
            while (j < text.size() && ident(text[j])) ++j;
            // pp-number exponent signs
            while (j < text.size() && std::isdigit(static_cast<unsigned char>(c)) && (text[j] == '+' || text[j] == '-') &&
                   (text[j - 1] == 'e' || text[j - 1] == 'E' || text[j - 1] == 'p' || text[j - 1] == 'P')) {
                ++j;
                while (j < text.size() && (ident(text[j]) || text[j] == '.')) ++j;
            }
        } else if (c == '"' || c == '\'') {
            while (j < text.size() && text[j] != c) { if (text[j] == '\\') ++j; ++j; }
            ++j;
        } else if (c == '.' && j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) {
            while (j < text.size() && (ident(text[j]) || text[j] == '.')) ++j;
        } else {
            for (const auto* p : puncts)
                if (text.compare(i, std::strlen(p), p) == 0) { j = i + std::strlen(p); break; }
        }
        // Numbers with '.' (1.0f) as one token.
        if (std::isdigit(static_cast<unsigned char>(c)))
            while (j < text.size() && (ident(text[j]) || text[j] == '.')) ++j;
        out.push_back({file, line, text.substr(i, j - i)});
        i = j;
    }
    return out;
}

// Compares both preprocessors on `source`; returns "" when equal.
std::string compare_preprocessors(const std::string& source, const msl::CompileOptions& options, bool must_handle) {
    using namespace alchemy::msl::detail;
    const auto clang = preprocess_with_clang(source, options);
    const auto mine = preprocess_in_house(source, options);
    if (!mine.handled) {
        if (must_handle || clang.ok) return "in-house declined: " + mine.reason;
        return {};
    }
    if (!clang.ok) return "clang -E failed but the in-house preprocessor accepted the source";
    const auto a = pp_tokens(clang.output), b = pp_tokens(mine.output);
    for (std::size_t k = 0; k < std::max(a.size(), b.size()); ++k) {
        if (k < a.size() && k < b.size() && a[k] == b[k]) continue;
        auto show = [](const std::vector<PpToken>& v, std::size_t at) {
            if (at >= v.size()) return std::string("<end>");
            return v[at].file + ":" + std::to_string(v[at].line) + " '" + v[at].text + "'";
        };
        return "token " + std::to_string(k) + ": clang " + show(a, k) + ", in-house " + show(b, k);
    }
    return {};
}

void test_preprocessor() {
    using namespace alchemy::msl::detail;
    msl::CompileOptions options;
    options.label = "pp";
    // Synthetic cases (each must be handled and equal to clang -E).
    const char* cases[] = {
        "#define A 1\n#define B(x) (x + A)\nint v = B(B(2));\n",
        "#define STR(x) #x\n#define XSTR(x) STR(x)\n#define V 42\nconst char* s = STR(a \"q\\n\" 'c' +  b); const char* t = XSTR(V);\n",
        "#define CAT(a, b) a##b\n#define CAT3(a, b, c) a##b##c\nint CAT(x, 1) = CAT3(1, 2, 3); int CAT(, y) = CAT(z,);\n",
        "#define F(...) f(__VA_ARGS__)\n#define G(a, ...) g(a, __VA_ARGS__)\n#define H(a, ...) h(a, ## __VA_ARGS__)\n"
        "F(); F(1); F(1, (2, 3)); G(1, 2, 3); H(1); H(1, 2);\n",
        "#define foo foo\n#define bar baz\n#define baz bar\nfoo bar baz;\n",
        "#define f(x) x f\nint a = f(1)(2)(3);\n#define g(x) x\n#define h g(\nint b = h 5);\n",
        "#define M(a, b) a + b\nint x = M(\n  1,\n  2) + 3;\nint y = 4;\n",
        "#define FN(x) x\nint FN\n;\nint w = FN\n(7);\n",
        "#if 1 + 2 * 3 == 7 && (-1 < 0) && (-1 > 0u) && (1 << 4) == 16 && !0 && ~0 == -1 && 'a' == 97\nint ok;\n#else\nint bad;\n#endif\n",
        "#define X 3\n#if defined X && defined(X) && !defined Y && X > 2 ? 1 : 0\nint ok;\n#elif 1\nint bad;\n#endif\n"
        "#ifdef X\nint a;\n#endif\n#ifndef Z\nint b;\n#endif\n#if 0\n#error never\n#elif 0\n#else\nint c;\n#endif\n",
        "#define BODY(t) struct t { int v; }\nBODY(S); /* comment\nspanning */ int after;\n// line\nint z;\n",
        "#line 100 \"file.glsl\"\nint a = __LINE__;\nconst char* f = __FILE__;\n#define L __LINE__\nint b = L;\n",
        "#pragma clang diagnostic push\nint a;\n#pragma clang diagnostic pop\n#\nint b;\n",
        "#define LONG_MACRO(a) \\\n  a + \\\n  a\nint v = LONG_MACRO(2);\nint after_splice;\n",
        "#include <metal_stdlib>\nfloat pi = M_PI_F;\n#undef M_PI_F\nfloat x = M_PI_F;\n",
        "#define EMPTY\n#define E2() \nint EMPTY a E2() ;\n#if __cplusplus\nint cpp;\n#endif\n#if __METAL_VERSION__ >= 200\nint m2;\n#endif\n",
        "#define ARR(...) { __VA_ARGS__ }\nfloat a[] = ARR(1.0f, 2.0f,\n 3.0f);\nint after;\n",
        "#define APPLY(m, x) m(x)\n#define SQ(x) ((x) * (x))\nint s = APPLY(SQ, 3) + APPLY(APPLY, SQ)(2);\n",
    };
    std::size_t index = 0;
    for (const auto* source : cases) {
        const auto problem = compare_preprocessors(source, options, true);
        require(problem.empty(), "preprocessor case " + std::to_string(index) + ": " + problem + "\n" + source);
        ++index;
    }
    // User macros (MTLCompileOptions.preprocessorMacros).
    {
        auto with_macros = options;
        with_macros.macros = {{"INPUT_TYPE", "float"}, {"COUNT", "4"}};
        require(compare_preprocessors("INPUT_TYPE v[COUNT];\n#if COUNT == 4\nint four;\n#endif\n", with_macros, true).empty(),
                "user macros");
    }
    // Declines (clang -E takes over).
    const char* declines[] = {"#error stop\n", "#include <stdio.h>\n", "int c = __COUNTER__;\n",
        "const char* r = R\"(raw)\";\n", "#if __has_include(<x>)\n#endif\n"};
    for (const auto* source : declines) {
        const auto mine = preprocess_in_house(source, options);
        require(!mine.handled, std::string("expected the in-house preprocessor to decline: ") + source);
    }
    // Fixtures.
    std::size_t files = 0;
    for (const auto& file : fixture_files()) {
        msl::CompileOptions fixture_options;
        fixture_options.label = file.stem().string();
        const auto problem = compare_preprocessors(read_file(file), fixture_options, true);
        require(problem.empty(), file.filename().string() + ": " + problem);
        ++files;
    }
    // Corpus.
    std::string directory = alchemy::msl::detail::environment("ALCHEMY_MSL_CORPUS");
#ifdef ALCHEMY_MSL_CORPUS_DIR
    if (directory.empty()) directory = ALCHEMY_MSL_CORPUS_DIR;
#endif
    std::size_t corpus = 0;
    double clang_ms = 0, mine_ms = 0;
    if (!directory.empty() && fs::is_directory(directory)) {
        for (const auto& entry : corpus_entries(directory)) {
            const auto source = read_file(entry.file);
            const auto problem = compare_preprocessors(source, entry.options, true);
            require(problem.empty(), entry.file.filename().string() + ": " + problem);
            auto t0 = std::chrono::steady_clock::now();
            (void)preprocess_with_clang(source, entry.options);
            auto t1 = std::chrono::steady_clock::now();
            (void)preprocess_in_house(source, entry.options);
            auto t2 = std::chrono::steady_clock::now();
            clang_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
            mine_ms += std::chrono::duration<double, std::milli>(t2 - t1).count();
            ++corpus;
        }
    }
    std::cout << "preprocessor: ok (" << index << " cases, " << files << " fixtures, " << corpus
              << " corpus libraries equal to clang -E";
    if (corpus) std::cout << "; corpus clang -E " << clang_ms << " ms, in-process " << mine_ms << " ms";
    std::cout << ")\n";
}

// ---- reflection -------------------------------------------------------------------------
// Signedness of the interface variable at `location` in `storage` (1 Input,
// 3 Output): 1 signed int, 0 unsigned int, -1 float, -2 not found.
int interface_signedness(const std::vector<std::uint32_t>& spirv, std::uint32_t storage, std::uint32_t location) {
    std::map<std::uint32_t, std::vector<std::uint32_t>> types;   // result id -> instruction words
    std::map<std::uint32_t, std::uint32_t> var_type, locations;
    for (std::size_t i = 5; i < spirv.size();) {
        const auto count = spirv[i] >> 16, op = spirv[i] & 0xffff;
        if (!count) break;
        if (op >= 19 && op <= 32 && count >= 2) types[spirv[i + 1]] = {spirv.begin() + i, spirv.begin() + i + count};
        if (op == 59 && spirv[i + 3] == storage) var_type[spirv[i + 2]] = spirv[i + 1];
        if (op == 71 && count == 4 && spirv[i + 2] == 30) locations[spirv[i + 1]] = spirv[i + 3];
        i += count;
    }
    for (const auto& [var, pointer] : var_type) {
        const auto found = locations.find(var);
        if (found == locations.end() || found->second != location) continue;
        auto type = types[pointer].at(3);                  // OpTypePointer %id Storage %pointee
        if (types[type].at(0) % 65536 == 23) type = types[type].at(2);   // OpTypeVector %id %component N
        const auto& scalar = types[type];
        if ((scalar.at(0) & 0xffff) == 22) return -1;      // OpTypeFloat
        return static_cast<int>(scalar.at(3));             // OpTypeInt %id width signedness
    }
    return -2;
}

// Integer interface variables take their signedness from the declared MSL
// type through aliases (Blender: `using ivec2 = int2;`), as Vulkan matches
// them against SINT/UINT vertex formats and attachments; reflection resolves
// the aliases too, and texture access is reflected exactly.
void test_interface_signedness() {
    const char* source = R"(
#include <metal_stdlib>
using namespace metal;
using ivec2 = int2;
typedef uint uvec_t;
using ivec4 = vec<int, 4>;
using rw_image = texture2d<float, access::read_write>;
struct VIn { float2 pos [[attribute(0)]]; ivec2 glyph_size [[attribute(2)]]; uvec_t flags [[attribute(3)]];
             short level [[attribute(4)]]; packed_int3 packed [[attribute(5)]]; };
struct VOut { float4 p [[position]]; ivec2 size [[user(size), flat]]; uint flags [[user(flags), flat]];
              int3 packed [[user(packed), flat]]; };
vertex VOut vs(VIn in [[stage_in]]) {
    VOut o;
    o.p = float4(in.pos, float(in.level), 1.0);
    o.size = in.glyph_size;
    o.flags = in.flags;
    o.packed = int3(in.packed);
    return o;
}
struct FOut { ivec4 id [[color(0)]]; uint4 mask [[color(1)]]; float4 c [[color(2)]]; };
fragment FOut fs(VOut in [[stage_in]]) {
    FOut o;
    o.id = ivec4(in.size, in.packed.x, 0);
    o.mask = uint4(in.flags);
    o.c = float4(1.0);
    return o;
}
kernel void k(texture2d<float, access::read> r [[texture(0)]], texture2d<float, access::write> w [[texture(1)]],
              rw_image rw [[texture(2)]], texture2d<float> s [[texture(3)]],
              texture_buffer<float, access::read> tb [[texture(4)]],
              texture_buffer<float, access::read_write> tbw [[texture(5)]], sampler smp [[sampler(0)]],
              uint2 gid [[thread_position_in_grid]]) {
    const float4 v = r.read(gid) + rw.read(gid) + s.sample(smp, float2(gid), level(0)) + tb.read(gid.x);
    w.write(v, gid);
    rw.write(v, gid);
    tbw.write(v, gid.x);
}
)";
    msl::CompileOptions options;
    options.label = "signedness";
    const auto r = msl::compile(source, options);
    require(r.success(), "signedness source: " + msl::format_diagnostics(r.diagnostics));
    const auto& vs = function(r, "vs");
    const auto& fs = function(r, "fs");
    validate_spirv(vs.spirv, "signedness.vs");
    validate_spirv(fs.spirv, "signedness.fs");
    require(interface_signedness(vs.spirv, 1, 0) == -1, "float2 attribute is float");
    require(interface_signedness(vs.spirv, 1, 2) == 1, "ivec2 (using alias of int2) attribute is a signed int input");
    require(interface_signedness(vs.spirv, 1, 3) == 0, "uvec_t (typedef of uint) attribute is unsigned");
    require(interface_signedness(vs.spirv, 1, 4) == 1, "short attribute is a signed int input");
    require(interface_signedness(vs.spirv, 1, 5) == 1, "packed_int3 attribute is a signed int input");
    const auto& vr = vs.reflection;
    for (const auto& a : vr.stage_in) {
        const auto& t = type_of(vr, a.type);
        if (a.index == 2)
            require(t.kind == msl::Type::Kind::vector && t.scalar == msl::Scalar::int32 && t.components == 2,
                    "reflection resolves ivec2 to int2");
        if (a.index == 3) require(t.kind == msl::Type::Kind::scalar && t.scalar == msl::Scalar::uint32, "uvec_t is uint");
    }
    const auto size = varying(vr.outputs, "size");
    require(interface_signedness(vs.spirv, 3, size.location) == 1, "flat ivec2 vertex output is signed");
    require(interface_signedness(vs.spirv, 3, varying(vr.outputs, "flags").location) == 0, "flat uint output is unsigned");
    require(interface_signedness(fs.spirv, 1, varying(fs.reflection.inputs, "size").location) == 1,
            "flat ivec2 fragment input is signed");
    require(interface_signedness(fs.spirv, 3, 0) == 1, "ivec4 (alias of vec<int, 4>) colour output is signed");
    require(interface_signedness(fs.spirv, 3, 1) == 0, "uint4 colour output is unsigned");
    require(interface_signedness(fs.spirv, 3, 2) == -1, "float4 colour output is float");
    require(type_of(fs.reflection, fs.reflection.color_outputs[0].type).scalar == msl::Scalar::int32,
            "reflection: ivec4 colour is int");
    const auto linked = msl::link(vs, fs);
    require(linked.success(), "signedness link: " + msl::format_diagnostics(linked.diagnostics));

    // Texture access decides the descriptor type: sample and read are sampled
    // images (read() is OpImageFetch), write and read_write storage images;
    // texture buffers are uniform / storage texel buffers likewise.
    const auto& k = function(r, "k");
    validate_spirv(k.spirv, "signedness.k");
    using msl::DescriptorType;
    const auto& kr = k.reflection;
    require(texture(kr, 0).access == msl::Access::read && texture(kr, 0).descriptor.type == DescriptorType::sampled_image,
            "access::read -> sampled image");
    require(texture(kr, 1).access == msl::Access::write && texture(kr, 1).descriptor.type == DescriptorType::storage_image,
            "access::write -> storage image");
    require(texture(kr, 2).access == msl::Access::read_write &&
                texture(kr, 2).descriptor.type == DescriptorType::storage_image,
            "access::read_write through an alias -> storage image");
    require(texture(kr, 3).access == msl::Access::sample && texture(kr, 3).descriptor.type == DescriptorType::sampled_image,
            "default access::sample -> sampled image");
    require(texture(kr, 4).access == msl::Access::read &&
                texture(kr, 4).descriptor.type == DescriptorType::uniform_texel_buffer,
            "texture_buffer access::read -> uniform texel buffer");
    require(texture(kr, 5).access == msl::Access::read_write &&
                texture(kr, 5).descriptor.type == DescriptorType::storage_texel_buffer,
            "texture_buffer access::read_write -> storage texel buffer");
}

void test_reflection() {
    using msl::Builtin;
    using msl::DescriptorType;
    using msl::Stage;
    {
        const auto r = compile_fixture("blender_ui_widget.metal");
        const auto& v = function(r, "vertex_function_entry").reflection;
        require(v.stage == Stage::vertex, "ui vertex stage");
        require(v.stage_in.size() == 3 && v.stage_in[0].index == 0 && v.stage_in[2].index == 2 &&
                    type_of(v, v.stage_in[0].type).components == 2 &&
                    type_of(v, v.stage_in[2].type).scalar == msl::Scalar::uint32,
                "ui stage_in attributes");
        const auto& uniforms = buffer(v, 0, true);
        require(uniforms.index_function_constant == 0u && uniforms.space == msl::BufferBinding::Space::constant &&
                    uniforms.used && v.argument_table.set == 0 && v.argument_table.binding == 0,
                "ui uniforms at [[buffer(MTL_uniform_buffer_base_index)]]");
        require(v.argument_table_size == 256, "dynamic buffer index uses the whole argument table");
        require(varying(v.outputs, "locn2").interpolation == msl::Interpolation::flat &&
                    varying(v.outputs, "locn0").location != varying(v.outputs, "locn1").location,
                "ui vertex varyings");
        // MTL_global_pointsize (1) folds away: gl_PointSize is 1.0 on both paths.
        require(has_constant(v, 0), "ui vertex function constants");
        const auto& f = function(r, "fragment_function_entry").reflection;
        const auto& image = texture(f, 0);
        require(image.texture_type == msl::TextureType::texture2d && image.access == msl::Access::sample &&
                    image.descriptor.set == 1 && image.descriptor.binding == 16 &&
                    image.descriptor.type == DescriptorType::sampled_image && image.used,
                "ui texture(0) descriptor");
        require(f.samplers.size() == 1 && f.samplers[0].descriptor.set == 1 && f.samplers[0].descriptor.binding == 160,
                "ui sampler(0) descriptor");
        require(f.color_outputs.size() == 1 && f.color_outputs[0].color == 0, "ui color(0)");
        require(f.discards && has_builtin(f, Builtin::front_facing), "ui discard and front_facing");
        require(has_constant(f, 2), "ui SPEC_shadow_samples function constant");
        for (const auto& in : f.inputs) require(in.name.rfind("locn", 0) == 0, "ui fragment inputs named locnN");
    }
    {
        const auto r = compile_fixture("compute_reduce.metal");
        const auto& k = function(r, "reduce_sum").reflection;
        require(k.stage == Stage::kernel && k.argument_table.set == 0 && k.argument_table_size == 32,
                "reduce argument table (buffers 0..3)");
        require(buffer(k, 0).access == msl::Access::read && buffer(k, 2).access == msl::Access::read_write,
                "reduce buffer access");
        require(buffer(k, 3).reference && buffer(k, 3).space == msl::BufferBinding::Space::constant, "constant Params&");
        require(k.threadgroup_memory.size() == 1 && k.threadgroup_memory[0].index == 0 &&
                    k.threadgroup_memory[0].spec_id == msl::reserved_spec_id::threadgroup_memory_base,
                "reduce threadgroup(0)");
        require(k.uses_simdgroup_operations && has_constant(k, 1), "reduce simd ops / use_simd");
        require(has_builtin(k, Builtin::thread_index_in_simdgroup) && has_builtin(k, Builtin::threads_per_threadgroup),
                "reduce builtins");
        const auto& h = function(r, "histogram").reflection;
        require(texture(h, 0).access == msl::Access::read && texture(h, 0).descriptor.type == DescriptorType::sampled_image,
                "histogram read texture is a sampled image (OpImageFetch)");
    }
    {
        const auto r = compile_fixture("textures.metal");
        const auto& f = function(r, "composite_frag").reflection;
        require(texture(f, 1).texture_type == msl::TextureType::depth2d, "depth2d");
        require(texture(f, 2).texture_type == msl::TextureType::texture2d_array, "texture2d_array");
        require(texture(f, 3).texture_type == msl::TextureType::texture_cube, "texturecube");
        require(texture(f, 4).texture_type == msl::TextureType::texture3d && texture(f, 4).sample_scalar == msl::Scalar::half,
                "texture3d<half>");
        require(texture(f, 5).sample_scalar == msl::Scalar::uint32, "texture2d<uint>");
        require(texture(f, 6).array_length == 4 && texture(f, 6).descriptor.count == 4 &&
                    texture(f, 6).descriptor.binding == 16 + 6,
                "array<texture2d<float>, 4> is one binding with 4 descriptors");
        require(texture(f, 10).texture_type == msl::TextureType::texture2d_ms, "texture2d_ms");
        require(f.constexpr_samplers.size() == 2, "two constexpr samplers");
        const msl::ConstexprSampler* linear = nullptr;
        const msl::ConstexprSampler* shadow = nullptr;
        for (const auto& s : f.constexpr_samplers) (s.compare_function ? shadow : linear) = &s;
        require(linear && shadow, "constexpr sampler kinds");
        require(linear->min_filter == 1 && linear->mag_filter == 1 && linear->mip_filter == 2 &&
                    linear->address == std::array<std::uint32_t, 3>{0, 0, 0} && linear->normalized_coordinates,
                "linear_clamp state (MTLSampler enums)");
        require(shadow->compare_function == 3, "compare_func::less_equal = MTLCompareFunctionLessEqual");
        for (const auto& s : f.constexpr_samplers)
            require(s.descriptor.set == 1 && s.descriptor.binding >= msl::constexpr_sampler_binding_base &&
                        s.descriptor.binding < msl::constexpr_sampler_binding_base + 2,
                    "constexpr sampler bindings 176+k");
        require(f.color_outputs.size() == 2 && f.depth == msl::DepthOutput::any, "two colors and depth(any)");
        const auto& fetch = function(r, "texel_fetch_frag");
        require(fetch.reflection.constexpr_samplers.empty() && fetch.reflection.samplers.empty(),
                "pixel-coordinate nearest sampling needs no sampler (OpImageFetch)");
        require(spirv_has_opcode(fetch.spirv, 95) && !spirv_has_opcode(fetch.spirv, 88),
                "texel fetch lowers to OpImageFetch, not OpImageSampleExplicitLod");
        const auto& k = function(r, "blur_image").reflection;
        require(texture(k, 1).descriptor.type == DescriptorType::storage_image && texture(k, 1).access == msl::Access::write,
                "write texture is a storage image");
        require(texture(k, 2).access == msl::Access::read_write && texture(k, 3).descriptor.type == DescriptorType::storage_image,
                "read_write / atomic textures are storage images");
    }
    {
        const auto r = compile_fixture("control_flow.metal");
        const auto& v = function(r, "mesh_vert").reflection;
        require(buffer(v, 2).function_constant == 4u, "[[buffer(2), function_constant(has_shadows)]]");
        require(varying(v.outputs, "nor").interpolation == msl::Interpolation::no_perspective &&
                    varying(v.outputs, "world").location != varying(v.outputs, "nor").location,
                "user-named varyings");
        require(type_of(v, varying(v.outputs, "uv").type).scalar == msl::Scalar::half, "half2 varying");
        require(has_builtin(v, Builtin::instance_id) && has_builtin(v, Builtin::base_instance), "instance builtins");
        const auto& f = function(r, "mesh_frag").reflection;
        require(has_builtin(f, Builtin::sample_id), "sample_id");
        require(f.color_outputs.size() == 1 && type_of(f, f.color_outputs[0].type).scalar == msl::Scalar::half,
                "half4 color output");
        for (const auto& c : f.function_constants)
            if (c.index == 3 || c.index == 5) require(c.required, "value-used function constants are required");
    }
    {
        const auto r = compile_fixture("blender_overlay.metal");
        const auto& v = function(r, "overlay_vert").reflection;
        require(buffer(v, 2, true).index_function_constant == 1u && buffer(v, 0, true).index_function_constant == 1u,
                "storage buffers at MTL_storage_buffer_base_index + n");
        require(has_builtin(v, Builtin::point_size) && has_builtin(v, Builtin::vertex_id), "point_size output");
        require(function(r, "prepass_frag").reflection.early_fragment_tests, "[[early_fragment_tests]]");
        const auto& f = function(r, "outline_frag").reflection;
        bool dual = false;
        for (const auto& c : f.color_outputs) dual = dual || (c.color == 0 && c.index == 1);
        require(dual, "dual-source [[color(0), index(1)]]");
        require(f.writes_sample_mask && f.depth == msl::DepthOutput::less, "sample mask and depth(less) outputs");
        require(f.uses_simdgroup_operations && has_builtin(f, Builtin::thread_index_in_simdgroup), "fragment simd ops");
        const auto& k = function(r, "prefix_sum").reflection;
        require(k.max_total_threads_per_threadgroup == 256 && k.uses_simdgroup_operations, "prefix_sum kernel attributes");
    }
    {
        const auto r = compile_fixture("present_blit.metal");
        const auto& f = function(r, "blit_fragment").reflection;
        require(f.constexpr_samplers.size() == 1 && f.constexpr_samplers[0].min_filter == 0 &&
                    f.constexpr_samplers[0].address == std::array<std::uint32_t, 3>{0, 0, 0} &&
                    f.constexpr_samplers[0].normalized_coordinates,
                "default constexpr sampler state");
        require(varying(function(r, "blit_vertex").reflection.outputs, "uv").location == varying(f.inputs, "uv").location,
                "blit varying");
    }
    {
        // Argument buffer of samplers: no argument-table slot, one sampler
        // array binding, elements by [[id]].
        const auto r = compile_fixture("eevee_style.metal");
        const auto& f = function(r, "eevee_style_frag");
        const auto& fr = f.reflection;
        require(fr.argument_buffers.size() == 1, "one argument buffer of samplers");
        const auto& ab = fr.argument_buffers[0];
        require(ab.index == 1 && ab.index_function_constant == 0u && ab.name == "samplers" && ab.type == "SStruct" &&
                    ab.count == 18 && ab.members.size() == 1 && ab.members[0].name == "sampler_args" &&
                    ab.members[0].id == 0 && ab.members[0].count == 18 && ab.used,
                "argument buffer reflection");
        require(ab.descriptor.set == 1 && ab.descriptor.binding == msl::argument_buffer_sampler_binding_base &&
                    ab.descriptor.type == DescriptorType::sampler && ab.descriptor.count == 18,
                "argument buffer samplers are one binding with 18 descriptors");
        for (const auto& b : fr.buffers) require(b.name != "samplers", "the argument buffer takes no table slot");
        require(fr.samplers.empty(), "no [[sampler(n)]] bindings");
        // OpDecorate %var Binding 192 on an OpTypeArray of OpTypeSampler.
        bool array_binding = false;
        for (std::size_t i = 5; i < f.spirv.size();) {
            const auto count = f.spirv[i] >> 16;
            if (!count) break;
            if ((f.spirv[i] & 0xffff) == 71 && count == 4 && f.spirv[i + 2] == 33 &&
                f.spirv[i + 3] == msl::argument_buffer_sampler_binding_base)
                array_binding = true;
            i += count;
        }
        require(array_binding, "SPIR-V binds the sampler array at 192");
        // Framebuffer fetch: input attachments 0, 1 and 3 (signed ints for int4).
        const auto& tile = function(r, "tile_classify_frag");
        const auto& tr = tile.reflection;
        require(tr.color_inputs.size() == 3, "three [[color(n)]] inputs");
        for (const auto& c : tr.color_inputs) {
            require(c.descriptor.set == 1 && c.descriptor.binding == msl::color_input_binding_base + c.color &&
                        c.descriptor.type == DescriptorType::input_attachment,
                    "color input descriptor");
            const auto& t = type_of(tr, c.type);
            if (c.color == 1) require(t.scalar == msl::Scalar::uint32 && t.components == 1, "uint gbuffer header input");
            if (c.color == 3) require(t.scalar == msl::Scalar::int32 && t.components == 4, "int4 input");
        }
        require(spirv_has_opcode(tile.spirv, 98), "framebuffer fetch is OpImageRead");
    }
    {
        // A program-scope constant with a dynamic initializer (not computed
        // from function constants) is an explicit error, not zeros.
        const auto r = msl::compile("#include <metal_stdlib>\nusing namespace metal;\n"
                                    "constant float k = sqrt(2.0f) * 3.0f;\n"
                                    "kernel void dyn(device float* o [[buffer(0)]]) { o[0] = k; }\n",
                                    msl::CompileOptions{});
        require(!r.success() && msl::format_diagnostics(r.diagnostics).find("dynamic initializer") != std::string::npos,
                "dynamic initializer is an explicit error: " + msl::format_diagnostics(r.diagnostics));
    }
    // Dawn/Tint and ANGLE style shaders (Chrome's GPU process, design).
    {
        const auto r = compile_fixture("tint_render.metal");
        const auto& v = function(r, "vs").reflection;
        const auto& u = buffer(v, 0);
        require(u.space == msl::BufferBinding::Space::constant && !u.reference && u.used, "tint uniforms pointer");
        require(v.stage_in.size() == 4 && type_of(v, v.stage_in[3].type).scalar == msl::Scalar::uint32,
                "tint flattened stage_in struct");
        require(varying(v.outputs, "locn2").interpolation == msl::Interpolation::flat &&
                    varying(v.outputs, "locn3").interpolation == msl::Interpolation::centroid_no_perspective,
                "tint flat / linear-centroid varyings");
        const auto& f = function(r, "fs").reflection;
        require(texture(f, 0).used && f.samplers.size() == 1 && f.discards && has_builtin(f, Builtin::front_facing),
                "tint fragment: texture, sampler, discard through the continue_execution flag");
        const auto& mrt = function(r, "mrt_fs").reflection;
        require(mrt.color_outputs.size() == 2 && type_of(mrt, mrt.color_outputs[1].type).scalar == msl::Scalar::uint32 &&
                    mrt.depth == msl::DepthOutput::any && mrt.writes_sample_mask && has_builtin(mrt, Builtin::sample_id),
                "tint MRT, depth and sample mask outputs");
    }
    {
        const auto r = compile_fixture("tint_compute.metal");
        const auto& k = function(r, "tint_main").reflection;
        for (const std::uint32_t index : {0U, 1U, 2U, 30U}) require(buffer(k, index).used, "tint buffers incl. sizes at 30");
        require(buffer(k, 30).space == msl::BufferBinding::Space::constant, "tint storage buffer sizes are constant");
        require(k.threadgroup_memory.size() == 1 && k.threadgroup_memory[0].index == 0, "tint workgroup struct");
        require(has_constant(k, 0) && has_constant(k, 1), "override constants, one through a derived constant");
        const auto& m = function(r, "math_main").reflection;
        require(texture(m, 1).access == msl::Access::read_write && m.uses_simdgroup_operations, "tint math kernel");
    }
    {
        const auto r = compile_fixture("tint_textures.metal");
        const auto& f = function(r, "fs").reflection;
        require(f.textures.size() == 9 && f.samplers.size() == 2, "tint texture kinds");
        require(texture(f, 6).texture_type == msl::TextureType::texture2d_ms &&
                    texture(f, 8).texture_type == msl::TextureType::texture_cube_array &&
                    texture(f, 7).texture_type == msl::TextureType::depth2d_array,
                "tint multisampled, cube array and depth array textures");
        const auto& s = function(r, "storage_main").reflection;
        require(texture(s, 4).texture_type == msl::TextureType::texture1d && texture(s, 4).access == msl::Access::read_write,
                "tint read_write 1D storage texture");
    }
    {
        const auto r = compile_fixture("angle_style.metal");
        const auto& v = function(r, "main0").reflection;
        require(buffer(v, 19).reference && buffer(v, 20).reference && v.stage_in.size() == 3, "ANGLE vertex interface");
        require(has_constant(v, 2), "ANGLERasterizerDisabled");
        const auto& f = function(r, "main1");
        const auto& fr = f.reflection;
        require(fr.textures.size() == 3 && fr.samplers.size() == 3 && fr.discards, "ANGLE texture environments");
        require(fr.depth == msl::DepthOutput::any && fr.writes_sample_mask, "ANGLE guarded depth and sample mask");
        for (const std::uint32_t index : {3U, 4U, 5U, 6U}) require(has_constant(fr, index), "ANGLE fragment constants");
        // ANGLESampleMaskWriteEnabled = ANGLEMultisampledRendering || ANGLEWriteHelperSampleMask is read from
        // spec constants 3 and 6; the guarded depth falls back on spec constant 4.
        const auto ids = spirv_spec_ids(f.spirv);
        for (const std::uint32_t id : {3U, 4U, 6U}) require(ids.count(id) != 0, "ANGLE spec id " + std::to_string(id));
    }
    {
        // ANGLE's program prelude: unqualified as_type<> in templates ahead of
        // `using namespace metal`, and get_num_samples() as the reserved
        // raster-sample-count specialization constant.
        const auto r = compile_fixture("angle_helpers.metal");
        const auto& f = function(r, "helpers_main");
        require(f.reflection.writes_sample_mask, "ANGLE helpers: sample mask output");
        require(spirv_spec_ids(f.spirv).count(msl::reserved_spec_id::raster_sample_count) != 0,
                "get_num_samples() is the raster sample count specialization constant");
        // array<sampler, 2> [[sampler(1)]]: the samplers of slots 1 and 2, both used.
        const auto& arrays = function(r, "sampler_arrays_main").reflection;
        require(arrays.samplers.size() == 2 && arrays.samplers[0].index == 1 && arrays.samplers[1].index == 2 &&
                    arrays.samplers[0].used && arrays.samplers[1].used,
                "ANGLE sampler arrays: one sampler per slot");
        require(arrays.textures.size() == 1 && arrays.textures[0].array_length == 2, "ANGLE texture arrays");
        validate_spirv(function(r, "sampler_arrays_main").spirv, "angle_helpers.sampler_arrays_main");
    }
    {
        const auto r = compile_fixture("tint_extras.metal");
        const auto& clip = function(r, "clip_vs");
        require(has_builtin(clip.reflection, Builtin::clip_distance) &&
                    has_builtin(clip.reflection, Builtin::render_target_array_index) && buffer(clip.reflection, 5).used,
                "tint clip distances (tint_array<float, 2>), layer and immediates");
        validate_spirv(clip.spirv, "tint_extras.clip_vs");
        const auto& blend = function(r, "blend_fs").reflection;
        require(blend.color_outputs.size() == 2 && blend.color_outputs[1].index == 1 &&
                    has_builtin(blend, Builtin::primitive_id),
                "tint dual-source blending and primitive id");
        require(function(r, "fetch_fs").reflection.color_inputs.size() == 1, "tint framebuffer fetch");
    }
    {
        // ANGLE's uniform blocks in an argument buffer of constant pointers.
        const auto r = compile_fixture("angle_argument_buffers.metal");
        const auto& v = function(r, "ab_vertex").reflection;
        const auto& ab = buffer(v, 0);
        require(ab.argument_buffer && ab.reference && ab.arguments.size() == 3 && ab.argument_length == 32 &&
                    ab.argument_alignment == 8,
                "argument buffer of block pointers: three [[id]] members, 32 bytes");
        require(ab.arguments[0].id == 0 && ab.arguments[0].offset == 0 && ab.arguments[0].pointer &&
                    ab.arguments[1].id == 1 && ab.arguments[1].offset == 8 && ab.arguments[2].id == 2 &&
                    ab.arguments[2].count == 2 && ab.arguments[2].offset == 16 && ab.arguments[2].stride == 8,
                "[[id]] layout: an array member takes ids 2 and 3");
        // Data members take their MSL size and alignment (checked against
        // clang by the wrappers' static_asserts); textures are refused.
        const auto data = msl::compile("#include <metal_stdlib>\nusing namespace metal;\n"
                                       "struct A { device float* p [[id(0)]]; float s [[id(1)]]; float4 v [[id(2)]]; };\n"
                                       "kernel void k(constant A& a [[buffer(1)]]) { a.p[0] = a.s + a.v.x; }\n",
                                       msl::CompileOptions{});
        require(data.success(), "argument buffer with data: " + msl::format_diagnostics(data.diagnostics));
        const auto& d = buffer(function(data, "k").reflection, 1);
        require(d.arguments.size() == 3 && d.arguments[1].offset == 8 && !d.arguments[1].pointer &&
                    d.arguments[2].offset == 16 && d.arguments[2].stride == 16 && d.argument_length == 32 &&
                    d.argument_alignment == 16,
                "argument buffer data members");
        const auto textures = msl::compile("#include <metal_stdlib>\nusing namespace metal;\n"
                                           "struct T { device float* p [[id(0)]]; texture2d<float> t [[id(1)]]; };\n"
                                           "kernel void k(constant T& a [[buffer(0)]]) { a.p[0] = 1.0f; }\n",
                                           msl::CompileOptions{});
        require(!textures.success() &&
                    msl::format_diagnostics(textures.diagnostics).find("argument buffer 'T' member 't'") != std::string::npos,
                "a texture in an argument buffer of buffers is an explicit error: " +
                    msl::format_diagnostics(textures.diagnostics));
    }
    test_interface_signedness();
    std::cout << "reflection: ok\n";
}

// ---- link -------------------------------------------------------------------------------
void test_link() {
    {
        const auto r = compile_fixture("blender_ui_widget.metal");
        const auto linked = msl::link(function(r, "vertex_function_entry"), function(r, "fragment_function_entry"));
        require(linked.success(), "ui link: " + msl::format_diagnostics(linked.diagnostics));
        for (const auto& in : linked.fragment.reflection.inputs)
            require(in.location == varying(linked.vertex.reflection.outputs, in.name).location, "ui locations agree");
    }
    // The fragment declares the varyings in another order and reads only
    // some of them: link() rewrites its Location decorations.
    const char* source = R"(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 p [[position]]; float a [[user(alpha)]]; float2 b [[user(beta)]]; float4 c [[user(gamma)]]; };
struct FIn { float4 c [[user(gamma)]]; float a [[user(alpha)]]; };
vertex VOut vs(uint id [[vertex_id]]) {
    VOut o; o.p = float4(float(id)); o.a = 1.0; o.b = float2(2.0); o.c = float4(3.0); return o;
}
fragment float4 fs(FIn in [[stage_in]]) { return in.c * in.a; }
struct Wrong { float4 p [[position]]; float3 d [[user(delta)]]; };
fragment float4 fs_missing(Wrong in [[stage_in]]) { return float4(in.d, 1.0); }
struct Mismatch { float4 p [[position]]; float4 b [[user(beta)]]; };
fragment float4 fs_mismatch(Mismatch in [[stage_in]]) { return in.b; }
)";
    msl::CompileOptions options;
    options.label = "link";
    const auto r = msl::compile(source, options);
    require(r.success(), "link source: " + msl::format_diagnostics(r.diagnostics));
    const auto& vs = function(r, "vs");
    const auto& fs = function(r, "fs");
    const auto before = input_locations(fs.spirv);
    const auto linked = msl::link(vs, fs);
    require(linked.success(), "link: " + msl::format_diagnostics(linked.diagnostics));
    const auto after = input_locations(linked.fragment.spirv);
    require(before.size() == 2 && after.size() == 2, "two fragment inputs");
    std::set<std::uint32_t> expected = {varying(vs.reflection.outputs, "alpha").location,
                                        varying(vs.reflection.outputs, "gamma").location};
    std::set<std::uint32_t> got;
    for (const auto& [id, location] : after) got.insert(location);
    require(got == expected, "fragment Location decorations follow the vertex outputs");
    require(before != after, "the fixture exercises a real remap");
    validate_spirv(linked.fragment.spirv, "link.fs");
    const auto missing = msl::link(vs, function(r, "fs_missing"));
    require(!missing.success() && msl::format_diagnostics(missing.diagnostics).find("'delta'") != std::string::npos,
            "unwritten fragment input is an error");
    const auto mismatch = msl::link(vs, function(r, "fs_mismatch"));
    require(!mismatch.success(), "varying type mismatch is an error");
    require(!msl::link(fs, vs).success(), "link() checks stages");
    std::cout << "link: ok\n";
}

// ---- cache ------------------------------------------------------------------------------
void test_cache() {
    // ALCHEMY_MSL_CACHE was pointed at a fresh directory by main().
    const auto source = read_file(fixtures_directory() / "compute_reduce.metal");
    msl::CompileOptions options;
    options.label = "cache";
    const auto start = std::chrono::steady_clock::now();
    const auto first = msl::compile(source, options);
    const auto cold = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    require(first.success(), "cache compile");
    const auto bytes = alchemy::msl::detail::serialize(first);
    const auto restored = alchemy::msl::detail::deserialize(bytes);
    require(restored && alchemy::msl::detail::serialize(*restored) == bytes, "serialisation round trip");
    auto truncated = bytes;
    truncated.resize(bytes.size() / 2);
    require(!alchemy::msl::detail::deserialize(truncated), "truncated cache file is rejected");

    const auto stats0 = alchemy::msl::detail::cache_statistics();
    const auto memory_hit = msl::compile(source, options);
    const auto stats1 = alchemy::msl::detail::cache_statistics();
    require(stats1.memory_hits == stats0.memory_hits + 1, "second compile is a memory hit");
    alchemy::msl::detail::clear_memory_cache();
    const auto warm_start = std::chrono::steady_clock::now();
    const auto disk_hit = msl::compile(source, options);
    const auto warm = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - warm_start).count();
    const auto stats2 = alchemy::msl::detail::cache_statistics();
    require(stats2.disk_hits == stats1.disk_hits + 1, "compile after clearing memory is a disk hit");
    require(alchemy::msl::detail::serialize(disk_hit) == bytes && alchemy::msl::detail::serialize(memory_hit) == bytes,
            "cache hits equal the fresh result");
    // Any option change is a different key.
    options.fast_math = false;
    const auto stats3 = alchemy::msl::detail::cache_statistics();
    require(msl::compile(source, options).success(), "precise-math compile");
    require(alchemy::msl::detail::cache_statistics().misses == stats3.misses + 1, "options are part of the key");
    require(alchemy::msl::detail::cache_key(source, options) != alchemy::msl::detail::cache_key(source + " ", options),
            "source is part of the key");
    // Failed compiles are not written to disk.
    const auto broken = msl::compile("kernel void k( {", options);
    require(!broken.success(), "broken source fails");
    std::cout << "cache: ok (cold " << cold << " ms, disk hit " << warm << " ms)\n";
}

// ---- dump --------------------------------------------------------------------------------
// ALCHEMY_MSL_DUMP: a source is written once as <label>-<hash>.metal with its
// .options; a precompiled library (newLibraryWithData:) as
// metallib-<hash>.metallib, byte for byte, once.
void test_dump(const fs::path& directory) {
    set_environment("ALCHEMY_MSL_DUMP", directory.string());   // before the first dump reads it
    msl::CompileOptions options;
    options.label = "dumped";
    options.macros.emplace_back("SCALE", "2.0f");
    const auto source = msl::dump_source("kernel void k() {}\n", options);
    require(source && fs::exists(*source) && read_file(*source) == "kernel void k() {}\n", "source dumped");
    const auto read_back = msl::read_dump_options(*source);
    require(read_back.macros.size() == 1 && read_back.macros[0].second == "2.0f", "options dumped");
    std::string bytes("MTLB\0\x01\x02\xff binary", 15);
    const auto library = msl::dump_library_data(bytes);
    require(library && fs::path(*library).filename().string().rfind("metallib-", 0) == 0 &&
                fs::path(*library).extension() == ".metallib",
            "metallib dumped as metallib-<hash>.metallib");
    require(read_file(*library) == bytes, "metallib bytes kept exactly (embedded NULs too)");
    require(msl::dump_library_data(bytes) == library, "the same library is written once");
    std::cout << "dump: ok (" << directory.string() << ")\n";
}

int compile_command(int argc, char** argv) {
    const fs::path file = argv[2];
    // A dumped library compiles with its .options (macros, language) when present.
    const auto options = dumped_options(file);
    const auto start = std::chrono::steady_clock::now();
    const auto result = msl::compile(read_file(file), options);
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::cout << msl::format_diagnostics(result.diagnostics);
    const fs::path out = argc > 3 ? fs::path(argv[3]) : file.parent_path();
    for (const auto& f : result.functions) {
        std::cout << describe(f);
        const auto path = out / (file.stem().string() + "." + f.name + ".spv");
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(f.spirv.data()), static_cast<std::streamsize>(f.spirv.size() * 4));
        std::cout << "  -> " << path.string() << '\n';
    }
    std::cout << ms << " ms\n";
    return result.success() ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: alchemy_msl_tests rewriter|fixtures|reflection|link|cache|dump|corpus|chrome_corpus|preprocessor|vulkan"
                     " | compile <file> [dir]\n";
        return 2;
    }
    const std::string group = argv[1];
    try {
        // Each group runs in its own process: give it a private result cache
        // (fixtures and corpus measure real compile times, so they run uncached).
        const auto cache = fs::temp_directory_path() / ("alchemy_msl_tests_" + group);
        std::error_code ignored;
        fs::remove_all(cache, ignored);
        const bool uncached = group == "fixtures" || group == "corpus" || group == "chrome_corpus" || group == "preprocessor";
        set_environment("ALCHEMY_MSL_CACHE", uncached ? "0" : cache.string());
        if (group == "compile" && argc >= 3) return compile_command(argc, argv);
        if (group == "rewriter") test_rewriter();
        else if (group == "fixtures") test_fixtures();
        else if (group == "reflection") test_reflection();
        else if (group == "link") test_link();
        else if (group == "cache") test_cache();
        else if (group == "dump") test_dump(cache / "dump");
        else if (group == "corpus") test_blender_corpus();
        else if (group == "chrome_corpus") test_chrome_corpus();
        else if (group == "preprocessor") test_preprocessor();
        else if (group == "vulkan") return run_vulkan_test();
        else {
            std::cerr << "unknown group " << group << '\n';
            return 2;
        }
        fs::remove_all(cache, ignored);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED (" << group << "): " << error.what() << '\n';
        return 1;
    }
}
