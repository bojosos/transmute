#include "alchemy/process.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
void check(const std::filesystem::path& executable, const std::filesystem::path& runtimes,
           const std::filesystem::path& output, bool full, bool jit, bool rebased) {
    alchemy::RunOptions options;
    options.jit = jit;
    options.jit_stats = jit;
    options.runtime_directories.push_back(runtimes);
    options.instruction_limit = 100'000'000;
    if (rebased) options.load_base = 0x120000000ULL;
    std::string stdout_text, stderr_text;
    const auto writer = [&](unsigned descriptor, std::span<const std::uint8_t> bytes) -> alchemy::OutputResult {
        auto& text = descriptor == 1 ? stdout_text : stderr_text;
        text.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return {bytes.size(), 0};
    };
    std::vector<std::string> arguments;
    if (full) arguments.push_back(output.string());
    const auto result = alchemy::run_program(executable, arguments, options, writer);
    const auto expected = full ? "Guest C++ runtime: 69\n" : "Guest C++ exception and RTTI: 27 42\n";
    if (result.exit_code != 0 || stdout_text != expected || !stderr_text.empty())
        throw std::runtime_error("guest C++ result mismatch: status " + std::to_string(result.exit_code) + " output " + stdout_text + stderr_text);
    if (jit && result.jit.native_guest_instructions == 0) throw std::runtime_error("C++ JIT code did not execute");
    if (full) {
        std::ifstream file(output, std::ios::binary);
        const std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if (contents != expected) throw std::runtime_error("C++ output file mismatch");
    }
}
}
int main(int argc, char** argv) {
    try {
        if (argc < 3) throw std::runtime_error("expected exception fixture and LLVM runtime directory");
        for (bool jit : {false, true}) for (bool rebased : {false, true})
            check(argv[1], argv[2], {}, false, jit, rebased);
        if (argc == 5) for (bool jit : {false, true}) for (bool rebased : {false, true})
            check(argv[3], argv[2], std::filesystem::path(argv[4]) / (std::string("cpp-") + (jit ? "jit-" : "interpreter-") + (rebased ? "rebased.txt" : "default.txt")), true, jit, rebased);
        std::cout << "Alchemy guest C++ tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
