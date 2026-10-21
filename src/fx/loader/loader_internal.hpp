#pragma once

// Loader internals shared by the loader's translation units (owner: loader).

#include "fx/loader/image.hpp"
#include "fx/loader/loader.hpp"
#include "transmute/fx/process.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace transmute::fx::loader {

inline constexpr std::uint32_t kNoTls = UINT32_MAX;
inline constexpr std::size_t kTebTlsPointerOffset = 0x58;   // TEB::ThreadLocalStoragePointer

struct TlsTemplate {
    std::uint64_t start = 0;          // template [start, end) (absolute)
    std::uint64_t end = 0;
    std::uint32_t zero_fill = 0;
    std::uint32_t alignment = 16;
    std::uint64_t index_address = 0;  // AddressOfIndex (absolute)
    std::uint64_t callbacks = 0;      // AddressOfCallBacks (absolute, null-terminated)
};

struct Module {
    GuestModule info;
    MappedImage image;
    std::string name_utf8;            // file name, original case
    std::wstring key;                 // lower-case file name
    std::wstring path_key;            // lower-case full path
    std::wstring directory;           // directory of the file (no trailing separator)
    int refcount = 0;
    bool pinned = false;              // EXE and its static closure
    bool is_exe = false;
    bool imports_bound = false;       // import resolution finished
    bool attach_called = false;       // DllMain(PROCESS_ATTACH) was invoked
    bool initialized = false;         // PROCESS_ATTACH completed successfully
    bool thread_calls = true;         // DisableThreadLibraryCalls not called
    bool has_tls = false;
    bool static_load = false;         // part of the process startup set
    TlsTemplate tls;
    std::vector<Module*> deps;        // guest modules this one imports (unique)
    struct LoadTransaction* tx = nullptr;   // owning transaction while loading
    ExportView exports;
    bool exports_opened = false;
};

struct ThreadRecord {
    GuestContext* ctx = nullptr;
    bool attached = false;            // DLL_THREAD_ATTACH delivered (or deliberately skipped)
    bool notify = true;               // false: re-entered during thread exit, no notifications
    void** array = nullptr;           // current TLS array (published at shadow TEB + 0x58)
    std::size_t array_size = 0;
    std::vector<void*> blocks;        // per slot: this thread's block (or null)
    std::vector<void**> retired;      // replaced arrays; freed at detach
};

struct ModuleTable {
    struct Entry {
        std::uint64_t base;
        std::uint64_t end;
        const GuestModule* module;
    };
    std::vector<Entry> entries;       // sorted by base
};

struct SearchContext {
    std::uint32_t flags = 0;          // LoadLibraryExW flags
    std::wstring dll_load_dir;        // directory of the DLL being loaded by path
};

struct LoadTransaction {
    std::vector<Module*> created;     // dependency order (dependencies first)
    std::vector<Module*> retained;    // pre-existing modules referenced (one entry per reference)
    std::vector<Module*> loading;     // modules whose imports are being resolved (cycle detection)
    bool static_load = false;
    bool skip_imports = false;        // DONT_RESOLVE_DLL_REFERENCES
};

struct Dependency {
    Module* guest = nullptr;
    HMODULE host = nullptr;
    std::string host_name;            // host module file name (UTF-8) for bind_host_function
};

// The loader lock (recursive). Guest DllMain / TLS callbacks run under it.
// It is deliberately NOT the host loader lock: holding LdrLockLoaderLock
// across LoadLibraryExW deadlocks the Windows 10+ parallel loader against any
// other thread that has started a load. Code that already runs inside the
// host loader lock (the runtime's thread teardown is a TLS callback inside
// LdrShutdownThread) must not block indefinitely on this lock: a holder may
// itself be waiting for the host loader lock. It uses lock_bounded.
class LoaderMutex {
public:
    void lock();
    void unlock();
    bool try_lock();
    // try_lock for up to `milliseconds`; false if still busy.
    bool lock_bounded(unsigned milliseconds);

private:
    std::recursive_mutex inner_;
};

// True if the calling thread holds the host loader lock (PEB->LoaderLock).
bool host_loader_lock_held() noexcept;

struct State {
    LoaderMutex lock;
    // Protects `threads` and ThreadRecord contents. Innermost: never wait on
    // anything (guest code, host loader, the loader lock) while holding it.
    std::mutex threads_lock;
    LoaderConfig config;
    bool profile = false;          // print runtime stats at exit (--profile)
    bool configured = false;
    std::wstring app_dir;
    std::vector<std::wstring> dll_dirs;
    std::vector<std::pair<std::uint64_t, std::wstring>> user_dirs;   // AddDllDirectory
    std::uint64_t next_cookie = 1;
    std::wstring set_dll_directory;
    std::unordered_map<std::wstring, std::wstring> sxs;               // lower file name -> path
    bool sxs_loaded = false;
    HANDLE host_actctx = INVALID_HANDLE_VALUE;                       // shared assemblies (comctl32 v6)
    std::unordered_set<std::wstring> known_dlls;
    bool known_dlls_loaded = false;

    std::vector<std::unique_ptr<Module>> modules;      // load order
    std::vector<std::unique_ptr<Module>> retired;      // unloaded; kept for lock-free readers
    // Non-PE guest modules (register_external_module), and unregistered ones
    // kept for lock-free readers of find_guest_module.
    std::vector<std::unique_ptr<GuestModule>> external;
    std::vector<std::unique_ptr<GuestModule>> external_retired;
    std::vector<Module*> init_order;                   // PROCESS_ATTACH completion order
    std::atomic<const ModuleTable*> table{nullptr};
    std::vector<std::unique_ptr<const ModuleTable>> retired_tables;
    Module* exe = nullptr;

    std::unordered_map<std::wstring, Dependency> host_by_name;        // lower name -> host dependency
    std::unordered_map<HMODULE, int> host_guest_refs;                 // LoadLibrary refs by guest
    std::unordered_map<HMODULE, std::string> host_names;

    std::vector<Module*> tls_slots;                    // index -> module (null = free)
    std::vector<std::unique_ptr<ThreadRecord>> threads;

    std::vector<LoaderDiagnostic> diagnostics;
    LoaderStats stats;

    std::wstring command_line;
    std::wstring image_path;
    std::atomic<bool> exiting{false};
    // Run guest DLL_PROCESS_DETACH from the host process-detach hook: set once
    // the guest process started (launch) or loader_process_exit was called.
    std::atomic<bool> detach_at_exit{false};
};

State& state();

// ---- strings ------------------------------------------------------------------
std::wstring widen(std::string_view text);
std::string narrow(std::wstring_view text);
std::wstring lower(std::wstring_view text);
std::wstring file_name_of(std::wstring_view path);
std::wstring directory_of(std::wstring_view path);
std::wstring full_path(std::wstring_view path);
bool has_path_separator(std::wstring_view name);
bool is_api_set_name(std::wstring_view name);
// Adds ".dll" when the name has no extension; strips a trailing '.'.
std::wstring normalize_dll_name(std::wstring_view name);
bool file_exists(const std::wstring& path);

// ---- search (search.cpp) --------------------------------------------------------
bool is_known_dll(const std::wstring& lower_name);
// Finds an ARM64 PE for `name` on the guest search path. Empty if none.
std::wstring find_guest_file(std::wstring_view name, const SearchContext& search);
// Any file (any machine) on the guest search path, for resource-only loads.
std::wstring find_any_file(std::wstring_view name, const SearchContext& search);
void load_sxs_from_manifest(std::string_view manifest, const std::wstring& app_dir);
void release_host_activation_context();

// Activates the guest's shared-assembly activation context (e.g. Common
// Controls v6 from the EXE manifest) for the lifetime of the scope, so host
// DLL loads resolve the way they would for the native ARM64 process.
class HostActivation {
public:
    HostActivation();
    ~HostActivation();
    HostActivation(const HostActivation&) = delete;
    HostActivation& operator=(const HostActivation&) = delete;

private:
    ULONG_PTR cookie_ = 0;
    bool active_ = false;
};
void load_sxs_for_exe(const std::wstring& exe_path, const MappedImage* image);

// ---- module registry (loader.cpp) ------------------------------------------------
void publish_table();
Module* find_loaded(std::wstring_view name);          // by name or path
Module* module_by_base(std::uint64_t base);
void add_diagnostic(LoaderDiagnostic diagnostic);
std::uint64_t guest_load_internal(std::wstring_view name, std::uint32_t flags, bool static_load,
                                  std::string* error);
// Loads the process EXE and its static closure and runs initializers.
// Returns the EXE base or 0 (init_failed tells a DllMain failure apart).
std::uint64_t load_exe(const std::wstring& path, std::string& error, bool& init_failed);
void reset_registry_for_testing();

// ---- process (process.cpp) -----------------------------------------------------------
// Default guest stack: `option` (LaunchOptions::stack_reserve / --stack-size)
// if nonzero, else TRANSMUTE_FX_STACK_SIZE if valid, else the EXE's
// SizeOfStackReserve. Commit is the EXE's SizeOfStackCommit capped at the
// reserve. `source` names the winner ("option", "environment", "image").
void resolve_stack_sizes(std::uint64_t option, const PeHeaders& exe, std::uint64_t& reserve,
                         std::uint64_t& commit, const char** source = nullptr);

// ---- TLS (tls.cpp) -----------------------------------------------------------------
void assign_tls_slot(Module& module);
void release_tls_slot(Module& module);
void refresh_all_threads_tls();
void run_tls_callbacks(const Module& module, std::uint32_t reason);
ThreadRecord* find_thread(GuestContext* ctx);
void reset_tls_for_testing();

// ---- missing imports (missing.cpp) ---------------------------------------------------
std::uint64_t missing_import_thunk(const std::string& dll, const std::string& function);
void reset_missing_for_testing();

// ---- guest calls ---------------------------------------------------------------------
// Calls a guest initializer with (base, reason, reserved); returns the BOOL.
bool call_guest_init(std::uint64_t entry, std::uint64_t base, std::uint32_t reason,
                     std::uint64_t reserved);

} // namespace transmute::fx::loader
