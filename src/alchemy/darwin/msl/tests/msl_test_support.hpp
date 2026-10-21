#pragma once

// Shared helpers for the MSL compiler tests (owner: A6).
#include "alchemy/msl/compiler.hpp"

#include <algorithm>
#include <process.h>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace alchemy::msl::test {

inline void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// The fixture directory, from the environment (ALCHEMY_MSL_FIXTURES, which
// CTest sets to this checkout's src/alchemy/darwin/msl/tests/fixtures). It is
// not a compile definition: the shared compiler cache strips the checkout root
// when it hashes a compile, so an object built in another worktree would carry
// that worktree's path and the tests would read its fixtures.
inline std::filesystem::path fixtures_directory() {
    static const std::filesystem::path directory = [] {
        std::string value;
#ifdef _MSC_VER
        char* buffer = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&buffer, &length, "ALCHEMY_MSL_FIXTURES") == 0 && buffer) {
            value = buffer;
            std::free(buffer);
        }
#else
        if (const char* found = std::getenv("ALCHEMY_MSL_FIXTURES")) value = found;
#endif
        require(!value.empty(), "ALCHEMY_MSL_FIXTURES must name the fixture directory (CTest sets it)");
        return std::filesystem::path(value);
    }();
    return directory;
}

inline void set_environment(const char* name, const std::string& value) {
#ifdef _MSC_VER
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(static_cast<bool>(file), "cannot read " + path.string());
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

inline std::vector<std::filesystem::path> fixture_files() {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(fixtures_directory()))
        if (entry.path().extension() == ".metal") files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    return files;
}

// Compiles a fixture once per process; throws with the diagnostics on failure.
inline const CompileResult& compile_fixture(const std::string& name) {
    static std::mutex mutex;
    static std::map<std::string, CompileResult> results;
    std::lock_guard lock(mutex);
    auto found = results.find(name);
    if (found != results.end()) return found->second;
    CompileOptions options;
    options.label = std::filesystem::path(name).stem().string();
    auto result = compile(read_file(fixtures_directory() / name), options);
    require(result.success(), name + ":\n" + format_diagnostics(result.diagnostics));
    return results.emplace(name, std::move(result)).first->second;
}

// Runs spirv-val (vulkan1.3) on a module; skipped with a note when the
// Vulkan SDK's validator was not found at configure time.
inline void validate_spirv(const std::vector<std::uint32_t>& spirv, const std::string& label) {
#ifdef ALCHEMY_SPIRV_VAL
    // Per process: concurrent test runs (other build trees) use the same labels.
    const auto directory =
        std::filesystem::temp_directory_path() / ("alchemy_msl_spirv_" + std::to_string(_getpid()));
    std::filesystem::create_directories(directory);
    const auto path = directory / (label + ".spv");
    const auto log = directory / (label + ".log");
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(spirv.data()), static_cast<std::streamsize>(spirv.size() * 4));
    const std::string command = "\"\"" ALCHEMY_SPIRV_VAL "\" --target-env vulkan1.3 \"" + path.string() + "\" > \"" +
                                log.string() + "\" 2>&1\"";
    const int status = std::system(command.c_str());
    require(status == 0, label + ": spirv-val failed:\n" + read_file(log) + "(module kept at " + path.string() + ")");
    // The Vulkan validation layer checks a pipeline's module again after
    // freezing its specialization constants (spirv-opt --freeze-spec-const):
    // so does this, with spirv-opt from the same SDK.
    std::string opt = ALCHEMY_SPIRV_VAL;
    if (const auto at = opt.rfind("spirv-val"); at != std::string::npos) opt.replace(at, 9, "spirv-opt");
    std::error_code ignored;
    const auto frozen = directory / (label + ".frozen.spv");
    if (std::filesystem::exists(opt, ignored)) {
        const std::string freeze = "\"\"" + opt + "\" --freeze-spec-const \"" + path.string() + "\" -o \"" +
                                   frozen.string() + "\" > \"" + log.string() + "\" 2>&1 && \"" ALCHEMY_SPIRV_VAL
                                   "\" --target-env vulkan1.3 \"" + frozen.string() + "\" >> \"" + log.string() + "\" 2>&1\"";
        require(std::system(freeze.c_str()) == 0, label + ": spirv-val failed after freezing specialization constants:\n" +
                                                       read_file(log) + "(module kept at " + path.string() + ")");
    }
    // Another thread's spirv-val child may still hold an inherited handle:
    // a leftover temporary file is harmless.
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(frozen, ignored);
    std::filesystem::remove(log, ignored);
#else
    static bool noted = false;
    if (!noted) std::cout << "note: spirv-val not configured (ALCHEMY_SPIRV_VAL); SPIR-V validation skipped\n";
    noted = true;
    require(spirv.size() > 5 && spirv[0] == 0x07230203U, label + ": not a SPIR-V module");
#endif
}

inline std::string describe(const Function& function) {
    static const char* stages[] = {"vertex", "fragment", "kernel"};
    const auto& r = function.reflection;
    std::ostringstream out;
    auto type = [&](std::uint32_t index) { return index < r.types.size() ? r.types[index].name : std::string("?"); };
    out << stages[static_cast<int>(function.stage)] << ' ' << function.name << ": " << function.spirv.size() * 4
        << " bytes\n";
    if (r.argument_table_size)
        out << "  argument table set " << r.argument_table.set << " binding " << r.argument_table.binding << ", "
            << r.argument_table_size << " bytes\n";
    for (const auto& b : r.buffers) {
        out << "  buffer(" << (b.index_function_constant ? "fc" + std::to_string(*b.index_function_constant) + "+" : "")
            << b.index << ") " << b.name << ": " << (b.space == BufferBinding::Space::constant ? "constant " : "device ")
            << type(b.type) << (b.reference ? "&" : "*") << (b.used ? "" : " (unused)");
        if (b.function_constant) out << " [fc " << *b.function_constant << "]";
        out << '\n';
    }
    for (const auto& t : r.textures)
        out << "  texture(" << t.index << ") " << t.name << ": type " << static_cast<int>(t.texture_type) << " access "
            << static_cast<int>(t.access) << " set " << t.descriptor.set << " binding " << t.descriptor.binding
            << " count " << t.descriptor.count << (t.used ? "" : " (unused)") << '\n';
    for (const auto& s : r.samplers)
        out << "  sampler(" << s.index << ") " << s.name << ": binding " << s.descriptor.binding << '\n';
    for (const auto& a : r.argument_buffers)
        out << "  argument buffer(" << (a.index_function_constant ? "fc" + std::to_string(*a.index_function_constant) + "+" : "")
            << a.index << ") " << a.name << ": " << a.count << " samplers at binding " << a.descriptor.binding
            << (a.used ? "" : " (unused)") << '\n';
    for (const auto& s : r.constexpr_samplers)
        out << "  constexpr sampler binding " << s.descriptor.binding << " filters " << s.min_filter << '/'
            << s.mag_filter << '/' << s.mip_filter << " compare " << s.compare_function << '\n';
    for (const auto& c : r.function_constants)
        out << "  function_constant(" << c.index << ") " << c.name << ": " << type(c.type)
            << (c.required ? " required" : "") << '\n';
    for (const auto& a : r.stage_in) out << "  attribute(" << a.index << ") " << a.name << ": " << type(a.type) << '\n';
    for (const auto& v : r.outputs)
        out << "  out " << v.name << " location " << v.location << ' ' << type(v.type) << " interp "
            << static_cast<int>(v.interpolation) << '\n';
    for (const auto& v : r.inputs)
        out << "  in " << v.name << " location " << v.location << ' ' << type(v.type) << " interp "
            << static_cast<int>(v.interpolation) << '\n';
    for (const auto& c : r.color_outputs) out << "  color(" << c.color << ") index " << c.index << ' ' << type(c.type) << '\n';
    for (const auto& g : r.threadgroup_memory) out << "  threadgroup(" << g.index << ") spec id " << g.spec_id << '\n';
    out << "  builtins:";
    for (const auto b : r.builtins) out << ' ' << static_cast<int>(b);
    out << "\n  depth " << static_cast<int>(r.depth) << " discards " << r.discards << " simd "
        << r.uses_simdgroup_operations << " early " << r.early_fragment_tests << '\n';
    for (const auto& note : r.notes) out << "  note: " << note << '\n';
    return out.str();
}

// msl_vulkan_test.cpp
int run_vulkan_test();

} // namespace alchemy::msl::test
