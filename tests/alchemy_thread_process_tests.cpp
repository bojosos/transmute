#include "alchemy/process.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void check(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
           std::string_view expected, bool jit, bool rebased) {
    alchemy::RunOptions options;
    options.jit = jit;
    options.jit_stats = jit;
    if (rebased) options.load_base = 0x120000000ULL;
    std::string output, errors;
    const auto result = alchemy::run_program(executable, arguments, options,
        [&](unsigned fd, std::span<const std::uint8_t> bytes) -> alchemy::OutputResult {
            (fd == 1 ? output : errors).append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            return {bytes.size(), 0};
        });
    if (result.exit_code != 0 || output != expected || !errors.empty())
        throw std::runtime_error("threaded process result mismatch: " + output + errors + " status " + std::to_string(result.exit_code));
    if (jit && result.jit.native_guest_instructions == 0) throw std::runtime_error("threaded process JIT did not execute");
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 4) throw std::runtime_error("expected threaded fixture, TLS application and TLS plugin");
        for (bool jit : {false, true}) for (bool rebased : {false, true}) {
            check(argv[1], {}, "Darwin threads: 27 42\n", jit, rebased);
            check(argv[2], {argv[3]}, "Late dylib TLS: 35 50 8\n", jit, rebased);
        }
        std::cout << "Alchemy threaded process tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
