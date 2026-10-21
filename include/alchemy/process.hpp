#pragma once

#include "alchemy/darwin_process.hpp"
#include "alchemy/darwin_runtime.hpp"
#include "transmute/jit.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace alchemy {

struct ChildLaunch;

struct RunOptions {
    bool jit{};
    bool jit_stats{};
    std::optional<std::uint64_t> load_base;
    std::vector<std::filesystem::path> dylib_directories;
    std::vector<std::filesystem::path> runtime_directories;
    std::uint64_t instruction_limit{10'000'000};
    // Command-line options that guest children repeat (darwin_process.hpp).
    std::vector<std::string> launch_options;
    // This process is an Alchemy child: argv (with argv[0]), environment,
    // working directory, identity and descriptors come from its launch.
    std::shared_ptr<const ChildLaunch> child;
    // Descriptors 3 and up the launcher hands the guest (--fd, open_root_descriptor);
    // their handles are owned by the run.
    std::vector<ChildFd> descriptors;
};

struct ProcessResult {
    std::uint32_t exit_code{};
    std::uint64_t instructions{};
    transmute::JitStats jit;
    int signal{};   // nonzero: a Darwin signal's default action ended the guest
};

// TRANSMUTE_FX_PRELOAD for a process (`current`: its value, nullopt or empty when
// unset). An Alchemy child of a multi-process app gets "0": its app's translation
// cache holds what every process of the tree ever ran, and preloading copied all
// of it into each child's code arena (docs/ALCHEMY_MEMORY.md). An explicit setting
// is kept, and a top-level process keeps the default (nullopt: leave it).
[[nodiscard]] std::optional<std::string> child_preload_setting(bool child, const std::optional<std::string>& current);

// arguments excludes argv[0]; the executable path supplies it. No host guest pointers escape.
[[nodiscard]] ProcessResult run_program(
    const std::filesystem::path& executable,
    std::span<const std::string> arguments = {}, const RunOptions& options = {},
    OutputWriter writer = {}, std::span<const std::string> environment = {});

} // namespace alchemy
