#include "alchemy/process.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    try {
        if (argc < 3) throw std::runtime_error("expected guest fixture and stdout line");
        const auto executable = std::filesystem::path(argv[1]);
        const auto expected = std::string(argv[2]) + '\n';
        std::vector<std::string> arguments;
        for (int index = 3; index < argc; ++index) arguments.emplace_back(argv[index]);
        for (const bool jit : {false, true}) for (const bool rebased : {false, true}) {
            alchemy::RunOptions options;
            options.jit = jit;
            options.jit_stats = jit;
            options.instruction_limit = 100'000'000;
            if (rebased) options.load_base = 0x120000000ULL;
            std::string output, errors;
            const auto result = alchemy::run_program(executable, arguments, options,
                [&](unsigned descriptor, std::span<const std::uint8_t> bytes) -> alchemy::OutputResult {
                    auto& destination = descriptor == 1 ? output : errors;
                    destination.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                    return {bytes.size(), 0};
                });
            if (result.exit_code != 0 || output != expected || !errors.empty())
                throw std::runtime_error("guest personality result mismatch, status " + std::to_string(result.exit_code) + ": " + output + errors);
            if (jit && result.jit.native_guest_instructions == 0)
                throw std::runtime_error("Objective-C/CoreFoundation JIT fixture did not execute translated instructions");
        }
        std::cout << "Alchemy personality integration tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
