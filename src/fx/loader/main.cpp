// transmute-fx: runs an ARM64 Windows program on x64 with the fx engine.
//
//   transmute-fx [--dll-dir <dir>]... [--no-cache] [--profile] [--aot <image>]
//                [--stack-size <bytes|NNk|NNm>] [--verbose] [--] <program.exe> [args...]

#include "fx/cache/cache.hpp"
#include "fx/loader/command_line.hpp"
#include "fx/loader/loader.hpp"
#include "fx/runtime/runtime.hpp"
#include "fx/translator/translator.hpp"
#include "transmute/fx/process.hpp"
#include "fx/host/env.hpp"
#include "fx/host/file.hpp"
#include "fx/host/process.hpp"

#include <cstdio>
#include <exception>

namespace {
double startup_age_ms() { return transmute::fx::host::process_age_ms(); }

// Process power-throttling (QoS) state: `eco` = EcoQoS, otherwise HighQoS.
void set_process_qos(bool eco) { transmute::fx::host::set_process_qos(eco); }

// Guest process QoS (TRANSMUTE_FX_QOS=high (default) | default | eco).
// Windows gives a new process launch QoS for its first ~3 s; afterwards the
// threads of a process without a foreground window (Blender -b started from
// a hidden or background console) get low QoS, and the balanced hybrid policy
// moves them to efficiency cores. Native renders of a few seconds finish
// inside the launch window; the same work under fx outlasts it and ran ~80%
// on E-cores. HighQoS keeps guest threads eligible for performance cores, as
// a foreground application is. The guest's own SetProcessInformation /
// SetThreadInformation calls reach the host and still override it; runtime
// background threads opt into EcoQoS (TRANSMUTE_FX_BACKGROUND_QOS).
void apply_guest_qos() {
    wchar_t value[16] = {};
    const auto n = transmute::fx::host::get_env(L"TRANSMUTE_FX_QOS", value, 16);
    const bool set = n == 0 || n >= 16 || _wcsicmp(value, L"high") == 0 || _wcsicmp(value, L"eco") == 0;
    if (!set) return;
    const bool eco = n > 0 && n < 16 && _wcsicmp(value, L"eco") == 0;
    // An explicit execution-speed state set before we started (a parent's
    // job or launcher) wins.
    if (transmute::fx::host::process_qos_explicit()) return;
    set_process_qos(eco);
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    using namespace transmute::fx;
    const double wmain_ms = startup_age_ms();
    loader::CliOptions cli;
    std::wstring error;
    if (!loader::parse_cli(argc, argv, cli, error)) {
        std::fwprintf(stderr, L"transmute-fx: %ls\n%ls", error.c_str(), loader::cli_usage());
        return 2;
    }
    if (cli.help) {
        std::fwprintf(stdout, L"%ls", loader::cli_usage());
        return 0;
    }
    try {
        // Instruction families beyond the core (FP, NEON/vector memory,
        // atomics/system). Without them those instructions take the
        // one-instruction interpreter fallback.
        register_fp_emitters();
        register_neon_emitters();
        register_system_emitters();
        if (!cli.aot_images.empty() && (!cli.cache_directory.empty() || cli.profile)) {
            // The key must match what the guest process's JIT uses (--profile is part of it).
            CacheConfig config;
            config.directory = cli.cache_directory;
            config.translate_options.profile = cli.profile;
            cache_configure(config);
        }
        if (cli.aot_background) {
            // Helper started by background AOT: idle CPU priority only. Not
            // PROCESS_MODE_BACKGROUND_BEGIN: its very low memory priority makes
            // the multi-GB working set of a large image thrash (blender.exe:
            // 476 s of CPU instead of 10 s).
            // EcoQoS: efficiency cores, leaving performance cores to the guest.
            host::set_process_idle_priority();
            set_process_qos(true);
        }
        AotOptions aot_options;
        aot_options.threads = cli.aot_threads;
        aot_options.cold_file = cli.aot_background;
        bool aot_ok = true;
        for (const auto& image : cli.aot_images) {
            std::wstring aot_error;
            AotStats stats;
            aot_translate_image(image, aot_options, &stats, &aot_error);
            if (!aot_error.empty()) {
                std::fwprintf(stderr, L"transmute-fx: AOT %ls: %ls\n", image.c_str(), aot_error.c_str());
                aot_ok = false;
            } else {
                std::fwprintf(stderr, L"transmute-fx: AOT %ls: %ls\n", image.c_str(),
                              aot_format_stats(stats).c_str());
            }
        }
        if (cli.program.empty()) return aot_ok ? 0 : 1;

        if (cli.verbose) std::fprintf(stderr, "transmute-fx: wmain at %.1f ms after process creation\n", wmain_ms);
        apply_guest_qos();
        LoaderConfig config;
        config.verbose = cli.verbose;
        loader_configure(config);

        const std::wstring full = host::file::full_path(cli.program);
        LaunchOptions options;
        options.image_path = !full.empty() ? full : cli.program;
        options.command_line = loader::build_guest_command_line(options.image_path, cli.arguments);
        options.dll_directories = cli.dll_directories;
        options.runtime.enable_translation_cache = !cli.no_cache;
        options.runtime.profile = cli.profile;
        options.runtime.cache_directory = cli.cache_directory;
        if (!cli.no_cache) {
            // Modules without a translation cache yet get one built by an idle
            // helper process (this executable with --aot-background).
            const std::wstring self = host::executable_path();
            if (!self.empty()) cache_enable_background_aot(self, cache_default_background_aot_threads());
        }
        options.stack_reserve = cli.stack_reserve;   // 0: TRANSMUTE_FX_STACK_SIZE or the EXE header
        return launch_guest_process(options);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "transmute-fx: fatal: %s\n", e.what());
        // Returning from wmain ends in ExitProcess (guest DLL_PROCESS_DETACH):
        // stop background work first, as loader_process_exit does.
        runtime_prepare_exit();
        return 3;
    }
}
