#pragma once

// Loader API beyond transmute/fx/process.hpp (owner: loader). The abi
// component binds the corresponding kernel32 exports (DisableThreadLibraryCalls,
// AddDllDirectory, ResolveDelayLoadedAPI, ...) to these functions; tests and
// tools use the configuration and inspection hooks.

#include "transmute/fx/process.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace transmute::fx {

struct LoaderConfig {
    // Guest EXE path. Its directory is the application directory of the guest
    // search path; its manifest (embedded or <exe>.manifest) supplies private
    // side-by-side assemblies (e.g. Blender's blender.shared / blender.crt).
    std::wstring application_path;
    // Extra guest DLL directories searched after the application directory.
    std::vector<std::wstring> dll_directories;
    // false: map, relocate and bind only; no guest code (TLS callbacks,
    // DllMain) runs. Used by parse-only tests and offline tools.
    bool run_initializers = true;
    // Map every image away from its preferred base (tests).
    bool force_relocation = false;
    // Print diagnostics (missing imports, cycles) to stderr as they happen.
    bool verbose = false;
};
// Applies a configuration. launch_guest_process configures from LaunchOptions.
void loader_configure(const LoaderConfig& config);

struct LoaderDiagnostic {
    enum class Kind : std::uint8_t {
        missing_dll,        // no guest or host DLL of that name; imports bound to reporting thunks
        missing_function,   // export not found; import bound to a reporting thunk
        import_cycle,       // circular static imports (resolved, informational)
        bad_image,          // ARM64 file found but could not be mapped
        init_failed,        // DllMain(DLL_PROCESS_ATTACH) returned FALSE
        note,
    };
    Kind kind{};
    std::wstring module;    // importing module (file name)
    std::string dll;        // imported DLL as written in the import table
    std::string function;   // function name or "#ordinal"
    std::string message;
};
std::vector<LoaderDiagnostic> loader_diagnostics();
void loader_clear_diagnostics();

struct LoaderStats {
    std::uint64_t modules_mapped = 0;
    std::uint64_t modules_relocated = 0;
    std::uint64_t guest_imports = 0;       // IAT slots bound to guest exports
    std::uint64_t host_imports = 0;        // IAT slots bound through bind_host_function
    std::uint64_t replaced_imports = 0;    // guest imports replaced by abi_guest_import_replacement
    std::uint64_t missing_imports = 0;     // IAT slots bound to missing-import thunks
    std::uint64_t forwarders = 0;          // forwarded exports resolved
    std::uint64_t host_dlls = 0;           // distinct host DLLs loaded for imports
    double map_seconds = 0;                // image view or file read + copy, relocation
    double bind_seconds = 0;               // import resolution (excluding nested mapping)
    double init_seconds = 0;               // TLS callbacks + DllMain(PROCESS_ATTACH), outermost only
    double protect_seconds = 0;            // final section protections
};
LoaderStats loader_stats();

// DisableThreadLibraryCalls for a guest module.
bool guest_disable_thread_library_calls(std::uint64_t module);

// AddDllDirectory / RemoveDllDirectory / SetDllDirectoryW for the guest
// search path. The returned cookie is nonzero on success.
std::uint64_t guest_add_dll_directory(std::wstring_view directory);
bool guest_remove_dll_directory(std::uint64_t cookie);
bool guest_set_dll_directory(std::wstring_view directory);   // empty = clear

// GetModuleFileNameW for guest modules (module 0 = guest EXE) and host
// modules. Empty if unknown.
std::wstring guest_module_file_name(std::uint64_t module);

// Base of the guest module containing `address` (GetModuleHandleExW with
// GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS), 0 if none.
std::uint64_t guest_module_from_address(std::uint64_t address) noexcept;

// ResolveDelayLoadedAPI for guest images: loads the DLL named by the
// IMAGE_DELAYLOAD_DESCRIPTOR at `descriptor` (in `parent_module`), resolves
// the import whose IAT slot is `thunk_address`, stores it there and returns
// it. On failure calls the guest failure hook (if any) and returns its result.
std::uint64_t guest_resolve_delay_loaded_api(std::uint64_t parent_module, std::uint64_t descriptor,
                                             std::uint64_t failure_dll_hook,
                                             std::uint64_t failure_system_hook,
                                             std::uint64_t thunk_address, std::uint32_t flags);

// Activation context holding the shared side-by-side assemblies the guest
// EXE manifest requests (Microsoft.Windows.Common-Controls 6.0, GDI+, ...),
// or null. launch_guest_process activates it on the main thread (threads
// created from there inherit it); the runtime should activate it on host
// threads that enter guest code so host comctl32/gdiplus resolve to the
// versions the guest expects.
void* guest_activation_context() noexcept;

// Lazy TLS check: grows the calling thread's static TLS array (shadow TEB
// +0x58) so it covers every loaded module. Cheap when already current.
void loader_refresh_thread_tls(GuestContext& ctx);

// Handler run when guest code calls an import that could not be resolved.
// Returns the value placed in x0. The default prints "<dll>!<function>" once
// to stderr and returns 0 (TRANSMUTE_FX_MISSING_IMPORT=abort terminates the
// process with STATUS_ENTRYPOINT_NOT_FOUND instead).
using MissingImportHandler = std::uint64_t (*)(const char* dll, const char* function);
void loader_set_missing_import_handler(MissingImportHandler handler);

// Tests: unmaps every guest module without running guest code and resets
// the loader (modules, TLS slots, search configuration, diagnostics, stats).
void loader_reset_for_testing();

} // namespace transmute::fx
