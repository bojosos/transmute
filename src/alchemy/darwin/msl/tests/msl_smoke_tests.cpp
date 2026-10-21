// MSL compiler smoke test (owner: A6), linked against alchemy_core_fx like
// Metal's caller: the clang front end is found, a minimal library compiles
// to one kernel, source errors are diagnostics, and ALCHEMY_MSL_DUMP writes
// a corpus file. The full suite is alchemy_msl_tests (msl_tests.cpp).
#include "alchemy/msl/compiler.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
} // namespace

int main() {
    try {
        namespace msl = alchemy::msl;
        const auto& toolchain = msl::toolchain();
        std::cout << "clang: " << (toolchain.available ? toolchain.version : toolchain.error) << '\n';
#ifdef ALCHEMY_MSL_CLANG_DEFAULT_PATH
        require(toolchain.available && toolchain.version.find("clang version") != std::string::npos,
                "configured clang is unusable: " + toolchain.error);
#endif
        if (!toolchain.available) {
            std::cout << "Alchemy MSL smoke skipped (no clang)\n";
            return 0;
        }
        const auto directory = std::filesystem::temp_directory_path() / "alchemy_msl_smoke";
        std::filesystem::remove_all(directory);
        // dump_source and the cache read their environment once, on first use.
#ifdef _MSC_VER
        _wputenv_s(L"ALCHEMY_MSL_DUMP", directory.wstring().c_str());
        _wputenv_s(L"ALCHEMY_MSL_CACHE", (directory / "cache").wstring().c_str());
#else
        setenv("ALCHEMY_MSL_DUMP", directory.string().c_str(), 1);
        setenv("ALCHEMY_MSL_CACHE", (directory / "cache").string().c_str(), 1);
#endif
        const auto result = msl::compile("kernel void k(device uint* p [[buffer(0)]]) { p[0] = 1; }\n", {});
        require(result.success(), "minimal kernel: " + msl::format_diagnostics(result.diagnostics));
        require(result.functions.size() == 1 && result.functions[0].name == "k" &&
                    result.functions[0].stage == msl::Stage::kernel && result.functions[0].spirv.size() > 5 &&
                    result.functions[0].spirv[0] == 0x07230203U,
                "one SPIR-V kernel");
        const auto broken = msl::compile("kernel void k() { undeclared = 1; }\n", {});
        require(!broken.success() && msl::format_diagnostics(broken.diagnostics).starts_with("program_source:1:"),
                "source errors are located diagnostics: " + msl::format_diagnostics(broken.diagnostics));

        msl::CompileOptions options;
        options.label = "smoke";
        options.macros = {{"ALCHEMY", "1"}};
        const auto written = msl::dump_source("kernel void k() {}\n", options);
        require(written && std::filesystem::exists(*written), "ALCHEMY_MSL_DUMP did not write the source");
        require(msl::dump_source("kernel void k() {}\n", options) == written, "dump is not deduplicated");
        std::filesystem::remove_all(directory);
        std::cout << "Alchemy MSL smoke passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
