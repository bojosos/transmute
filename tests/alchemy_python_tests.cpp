#include "alchemy/path.hpp"
#include "alchemy/process.hpp"

#include <charconv>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    try {
        if (argc < 6 || argc > 8)
            throw std::runtime_error("expected launcher, Python home, script, extension directory, stdout line, optional command and exit status");
        const auto launcher = std::filesystem::path(argv[1]);
        const auto python_home = std::filesystem::path(argv[2]);
        const auto script = std::filesystem::path(argv[3]);
        const auto extensions = std::filesystem::path(argv[4]);
        const auto expected = std::string(argv[5]) + '\n';
        const bool site = script == "--site-command";
        const bool command = script == "--command" || site;
        if (command != (argc >= 7))
            throw std::runtime_error("command mode requires one Python code argument");
        unsigned expected_exit = 0;
        if (argc == 8) {
            const std::string_view value(argv[7]);
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), expected_exit);
            if (error != std::errc{} || end != value.data() + value.size() || expected_exit > 255)
                throw std::runtime_error("expected exit status must be an integer from 0 to 255");
        }
        const std::vector<std::string> environment{
            "PYTHONHOME=" + alchemy::guest_path_string(python_home),
            "PYTHONNOUSERSITE=1", "PYTHONDONTWRITEBYTECODE=1", "PYTHONUTF8=1", "LC_ALL=C"};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (const bool jit : {false, true}) for (const bool relocated : {false, true}) {
            alchemy::RunOptions options;
            options.jit = jit;
            options.jit_stats = jit;
            options.dylib_directories.push_back(extensions);
            options.instruction_limit = script.filename() == "python_numpy.py" ? 1'000'000'000 : 300'000'000;
            if (relocated) options.load_base = 0x120000000ULL;
            const auto output_file = std::filesystem::temp_directory_path() /
                ("alchemy-python-" + std::to_string(stamp) + (jit ? "-jit" : "-interp") +
                 (relocated ? "-relocated.bin" : "-default.bin"));
            std::vector<std::string> arguments = command
                ? std::vector<std::string>{"-B", "-c", argv[6]}
                : script == "--version"
                    ? std::vector<std::string>{"--version"}
                    : std::vector<std::string>{"-S", "-B", alchemy::guest_path_string(script),
                        alchemy::guest_path_string(script.filename() == "python_workload.py" ? output_file : extensions)};
            if (command && !site) arguments.insert(arguments.begin(), "-S");
            std::string output, errors;
            alchemy::ProcessResult result;
            try {
                result = alchemy::run_program(launcher, arguments, options,
                    [&](unsigned descriptor, std::span<const std::uint8_t> bytes) -> alchemy::OutputResult {
                        auto& destination = descriptor == 1 ? output : errors;
                        destination.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                        return {bytes.size(), 0};
                    }, environment);
            } catch (...) {
                std::error_code ignored;
                std::filesystem::remove(output_file, ignored);
                if (!output.empty() || !errors.empty()) std::cerr << output << errors;
                throw;
            }
            if (result.exit_code != expected_exit || output != expected || !errors.empty()) {
                std::error_code ignored;
                std::filesystem::remove(output_file, ignored);
                throw std::runtime_error("guest CPython result mismatch, status " +
                    std::to_string(result.exit_code) + ": " + output + errors);
            }
            if (std::filesystem::exists(output_file)) {
                std::filesystem::remove(output_file);
                throw std::runtime_error("guest CPython did not remove its workload file");
            }
            if (jit && result.jit.native_guest_instructions == 0)
                throw std::runtime_error("guest CPython did not execute translated ARM64 code");
            std::cout << (jit ? "JIT" : "Interpreter") << (relocated ? " relocated" : " default")
                      << ": " << result.jit.native_guest_instructions << " native instructions\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
