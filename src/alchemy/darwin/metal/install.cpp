#include "alchemy/gui/install.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/path.hpp"
#include "alchemy/darwin/metal/bridge.hpp"
#include "alchemy/macho.hpp"
#include "alchemy/msl/compiler.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>

// Metal's installation (install_metal_gui, install_darwin_gui) and the MSL
// seed set it picks up (alchemy-fx msl-seed, seed_msl_corpus). Built with
// the Vulkan-Headers; without them metal_unavailable.cpp is the installer.
namespace alchemy::gui {

namespace {
std::string environment_value(const char* name) {
    return host::process::environment(name).value_or(std::string());
}
// The seed set of this executable: <root>/<LC_UUID>, root ALCHEMY_MSL_SEED
// or %LOCALAPPDATA%\Alchemy\msl-seed ($XDG_CACHE_HOME or ~/.cache/alchemy/msl-seed off Windows).
std::filesystem::path msl_seed_directory(const std::filesystem::path& root, const std::filesystem::path& executable) {
    const auto uuid = macho_uuid(executable);
    return uuid ? root / *uuid : std::filesystem::path{};
}
std::filesystem::path msl_seed_root() {
    if (auto value = environment_value("ALCHEMY_MSL_SEED"); !value.empty())
        return value == "0" ? std::filesystem::path{} : utf8_path(value);
    if (const auto local = environment_value("LOCALAPPDATA"); !local.empty()) return utf8_path(local) / "Alchemy" / "msl-seed";
#if defined(_WIN32)
    return {};
#else
    if (const auto cache = environment_value("XDG_CACHE_HOME"); !cache.empty()) return utf8_path(cache) / "alchemy" / "msl-seed";
    const auto home = environment_value("HOME");
    return home.empty() ? std::filesystem::path{} : utf8_path(home) / ".cache" / "alchemy" / "msl-seed";
#endif
}
} // namespace

void install_metal_gui(InstallContext& context) {
    namespace bridge = metal::bridge;
    bridge::set_runtime(std::make_unique<bridge::Runtime>(bridge::Runtime{context.process, context.objc, context.cf}));
    auto& runtime = bridge::runtime();
    bridge::install_descriptors(runtime);
    bridge::install_objects(runtime);
    // Precompiled results for this application binary, when generated (msl-seed).
    if (const auto root = msl_seed_root(); !root.empty() && !context.process.executable.empty()) {
        std::error_code error;
        if (const auto seed = msl_seed_directory(root, context.process.executable);
            !seed.empty() && std::filesystem::is_directory(seed / "results", error))
            msl::set_seed_directory(seed);
    }
}

int seed_msl_corpus(const std::filesystem::path& corpus, const std::filesystem::path& executable,
                    const std::filesystem::path& root, unsigned threads) {
    const auto seed = msl_seed_directory(root, executable);
    if (seed.empty()) {
        std::cerr << "alchemy msl-seed: " << executable.string() << " has no LC_UUID\n";
        return 1;
    }
    // The compiler's cache root becomes the seed directory (before its first use).
    if (!host::process::set_environment("ALCHEMY_MSL_CACHE", path_utf8(seed))) return 1;
    // The simdgroup width Metal compiles for on this device.
    std::uint32_t width = 32;
    try { width = metal::system_device()->profile().thread_execution_width; }
    catch (const std::exception& failure) { std::cerr << "alchemy msl-seed: no Metal device (" << failure.what() << "); width 32\n"; }
    std::string report;
    const auto started = std::chrono::steady_clock::now();
    const auto compiled = msl::seed_corpus(corpus, threads, width, report);
    std::cerr << report;
    std::cout << "alchemy msl-seed: " << compiled << " libraries into " << seed.string() << " (subgroup width " << width << ", "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << " s)\n";
    return report.empty() ? 0 : 2;
}

} // namespace alchemy::gui
