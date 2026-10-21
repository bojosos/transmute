#include "alchemy/process.hpp"
#include "test_runner.hpp"

#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void run_fixture(const std::filesystem::path& path, std::string_view expected,
                 std::uint32_t exit_code, bool jit, bool relocate = false,
                 std::span<const std::string> arguments = {},
                 std::span<const std::string> environment = {}) {
    std::string output;
    alchemy::RunOptions options;
    options.jit = jit;
    options.jit_stats = jit;
    if (relocate) options.load_base = 0x180000000ULL;
    const auto result = alchemy::run_program(path, arguments, options,
        [&](unsigned fd, std::span<const std::uint8_t> bytes) {
            require(fd == 1, "unexpected stderr output");
            output.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            return alchemy::OutputResult{bytes.size(), 0};
        }, environment);
    if (output != expected) {
        throw std::runtime_error("guest output differs: expected [" + std::string(expected) +
            "], received [" + output + "], exit " + std::to_string(result.exit_code));
    }
    require(result.exit_code == exit_code, "guest exit code differs");
    if (jit) require(result.jit.native_guest_instructions > 0, "JIT executed no translated instructions");
}
}

int main(int argc, char** argv) {
    try {
        transmute::test_detail::TestRunner runner;
        runner.configure(argc, argv);
        if (argc != 9 && argc != 10) throw std::runtime_error("expected eight fixture paths and optional external binary");
        // Children of a multi-process app do not preload their app's translation cache unless asked to.
        runner.run("child_preload_setting", [] {
            require(alchemy::child_preload_setting(true, std::nullopt) == "0", "a child turns preloading off");
            require(alchemy::child_preload_setting(true, std::string()) == "0", "an empty setting counts as unset");
            require(!alchemy::child_preload_setting(true, std::string("2")), "an explicit setting is kept");
            require(!alchemy::child_preload_setting(false, std::nullopt), "a top-level process keeps the default");
            require(!alchemy::child_preload_setting(false, std::string("0")), "a top-level process keeps its setting");
        });
        for (const bool jit : {false, true}) {
            const std::string mode = jit ? "jit" : "interpreter";
            runner.run("hello_" + mode, [&] { run_fixture(argv[1], "Hello World from MacOS: 27\n", 0, jit); });
            runner.run("hello_rebased_" + mode, [&] { run_fixture(argv[1], "Hello World from MacOS: 27\n", 0, jit, true); });
            runner.run("guest_dylib_initializer_" + mode, [&] { run_fixture(argv[2], "dylib 42\n", 0, jit); });
            runner.run("guest_dylib_rebased_" + mode, [&] { run_fixture(argv[2], "dylib 42\n", 0, jit, true); });
            runner.run("raw_syscalls_" + mode, [&] { run_fixture(argv[3], "syscall hello\n", 0, jit); });
            runner.run("startup_" + mode, [&] {
                const std::array<std::string, 2> arguments{"first", "second"};
                const std::array<std::string, 1> environment{"ALCHEMY_TEST=present"};
                run_fixture(argv[4], "first\nargc 3 % second\n", 7, jit, false, arguments, environment);
            });
            runner.run("constructor_exit_" + mode, [&] { run_fixture(argv[5], "", 9, jit); });
            runner.run("unsupported_format_" + mode, [&] {
                bool rejected = false;
                try { run_fixture(argv[6], "", 0, jit); }
                catch (const std::exception& error) {
                    rejected = std::string_view(error.what()).find("unsupported") != std::string_view::npos;
                }
                require(rejected, "unsupported format did not fail explicitly");
            });
            runner.run("unsupported_strong_import_" + mode, [&] {
                bool rejected = false;
                try { run_fixture(argv[7], "", 0, jit); }
                catch (const std::exception& error) {
                    rejected = std::string_view(error.what()).find("alchemy_missing_export") != std::string_view::npos;
                }
                require(rejected, "missing strong system export did not fail before execution");
            });
            runner.run("unresolved_weak_import_" + mode, [&] { run_fixture(argv[8], "weak import null\n", 0, jit); });
            runner.run("missing_guest_dylib_" + mode, [&] {
                const auto directory = std::filesystem::path(argv[2]).parent_path() / "missing_guest_dependency";
                std::filesystem::create_directory(directory);
                const auto copy = directory / "dylib_app";
                std::filesystem::copy_file(argv[2], copy, std::filesystem::copy_options::overwrite_existing);
                bool rejected = false;
                try { run_fixture(copy, "", 0, jit); }
                catch (const std::exception& error) {
                    rejected = std::string_view(error.what()).find("missing guest dylib") != std::string_view::npos;
                }
                std::filesystem::remove(copy);
                std::filesystem::remove(directory);
                require(rejected, "missing guest dependency did not fail before execution");
            });
            if (argc == 10) {
                runner.run("external_sample_" + mode, [&] { run_fixture(argv[9], "Hello World from MacOS: 27\n", 0, jit); });
                runner.run("external_sample_rebased_" + mode, [&] { run_fixture(argv[9], "Hello World from MacOS: 27\n", 0, jit, true); });
            }
        }
        runner.finish();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
