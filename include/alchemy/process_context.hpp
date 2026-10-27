#pragma once

#include "alchemy/darwin_process.hpp"
#include "alchemy/darwin_runtime.hpp"
#include "alchemy/hash_map.hpp"

#include <atomic>
#include <filesystem>
#include <map>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy {

class ObjectStore;
struct ChildLaunch;

// The child side of an emulated fork (darwin_services, docs/ALCHEMY_PROCESS.md)
// runs on a temporary guest thread that shares the parent's memory until it
// calls exec. While a thread runs it, every synthetic call passes this guard
// first; calls outside the descriptor/signal/exec subset throw.
class ForkChildGuard {
public:
    virtual ~ForkChildGuard() = default;
    virtual void check_call(std::string_view library, std::string_view symbol) = 0;
};

enum class ExportKind { function, data, thread_local_descriptor };
struct RuntimeExport { std::uint64_t address{}; ExportKind kind{ExportKind::function}; };
struct ExecutionTotals {
    std::uint64_t instructions{}, native_guest_instructions{}, native_blocks{}, fallback_instructions{};
};

struct TlsTemplate {
    std::uint64_t module_id{};
    std::vector<std::uint8_t> initial_data;
    std::uint64_t zero_fill_size{};
    std::uint64_t alignment{16};
    std::vector<std::uint64_t> descriptors;
    std::vector<std::uint64_t> initializers;
};

struct ImageRuntimeInfo {
    struct Section { std::string segment, name; std::uint64_t address{}, size{}; std::uint32_t flags{}; };
    struct CodeRange { std::uint64_t begin{}, end{}; };
    std::uint64_t module_id{};
    std::uint64_t header{};
    std::uint64_t begin{};
    std::uint64_t end{};
    std::int64_t slide{};
    std::string name;
    std::uint64_t eh_frame{};
    std::uint64_t eh_frame_size{};
    std::uint64_t compact_unwind{};
    std::uint64_t compact_unwind_size{};
    std::vector<TlsTemplate> tls;
    std::vector<Section> sections;
    std::vector<CodeRange> code_ranges;
};

struct AutoreleasePool { std::uint64_t token{}; std::vector<std::uint64_t> objects; };

struct ThreadContext {
    std::uint64_t identity{};
    std::uint64_t host_thread{};   // the host thread id, when its own thread made the context (diagnostics)
    transmute::ReferenceEngine* engine{};
    std::uint64_t errno_address{};
    std::uint64_t dlerror_address{};
    std::string dlerror;
    std::map<std::uint64_t, std::uint64_t> tls_blocks;
    // TLV descriptor -> this thread's variable address (owner thread only;
    // cleared with tls_blocks).
    hash_map<std::uint64_t, std::uint64_t, address_hash> tlv_addresses;
    // Guest table (module id -> this thread's TLS block) read by the guest
    // __tlv_bootstrap fast path through TPIDR_EL0 (engines with a thread
    // pointer); 0 until the thread's first TLV access.
    std::uint64_t tlv_table{};
    std::map<std::uint64_t, std::uint64_t> key_values;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> finalizers;
    std::optional<std::uint64_t> exit_value;
    std::vector<AutoreleasePool> autorelease_pools;
    std::vector<std::uint64_t> unpooled_autoreleases;
};

// Owns compatibility state; engine execution and memory internals remain Transmute's.
// A guest descriptor described for another process or for later (Mach
// fileports): kind as ChildFdKind (1 handle, 2 directory, 3 random, 5
// socket), a Windows handle valid in the target process (kind 1, owned by
// whoever holds the description), Darwin status flags (O_ACCMODE, O_APPEND,
// O_NONBLOCK), the UTF-8 host path, and socket protocol information.
struct PortableDescriptor {
    std::uint32_t kind{};
    std::uint64_t handle{};
    std::uint64_t flags{};
    std::string path;
    std::vector<std::uint8_t> blob;
    std::int32_t socket_domain{}, socket_type{}, socket_protocol{};
};

class ProcessContext {
public:
    explicit ProcessContext(transmute::ReferenceEngine& engine, OutputWriter writer);
    ~ProcessContext();
    ProcessContext(const ProcessContext&) = delete;
    ProcessContext& operator=(const ProcessContext&) = delete;

    [[nodiscard]] transmute::ReferenceEngine& main_engine() const noexcept { return engine_; }
    [[nodiscard]] transmute::ReferenceEngine& calling_engine() const;
    [[nodiscard]] std::shared_ptr<ThreadContext> thread(transmute::ReferenceEngine& engine);
    void release_thread(transmute::ReferenceEngine& engine);
    [[nodiscard]] std::shared_ptr<ThreadContext> current_thread();
    // Identities of the threads that have a context now (Mach task_threads).
    [[nodiscard]] std::vector<std::uint64_t> thread_identities() const;
    // How many threads have had a context so far (ended ones included).
    [[nodiscard]] std::uint64_t threads_seen() const;
    // Diagnostics (ALCHEMY_HANG_DUMP): each thread with a context and the
    // guest registers its engine holds. For a thread blocked in a provider
    // call they are the call's (pc = the call's address, x30 = the caller).
    struct ThreadSnapshot {
        std::uint64_t identity{};
        std::uint64_t host_thread{};   // 0: not known
        transmute::CpuState state{};
    };
    [[nodiscard]] std::vector<ThreadSnapshot> thread_snapshots() const;
    // The provider export at `address` ("symbol (library)"), or "".
    [[nodiscard]] std::string export_name(std::uint64_t address) const;
    [[nodiscard]] std::uint64_t allocate_guest(std::uint64_t size, std::uint64_t alignment = 16);
    void release_guest(std::uint64_t address);
    void set_errno(std::uint32_t value);
    void set_dlerror(std::string message);
    [[nodiscard]] OutputResult output(unsigned descriptor, std::span<const std::uint8_t> bytes) const;
    // `where` attributes the registration for the registration audit
    // (alchemy/registration_audit.hpp); callers never pass it.
    [[nodiscard]] std::uint64_t install_function(std::string symbol,
        transmute::GuestCallHandler handler, std::string library = "/usr/lib/libSystem.B.dylib",
        const std::source_location& where = std::source_location::current());
    void install_export(std::string library, std::string symbol, RuntimeExport value,
        const std::source_location& where = std::source_location::current());
    [[nodiscard]] std::optional<RuntimeExport> resolve(std::string_view library,
                                                     std::string_view symbol) const;
    [[nodiscard]] static bool is_provider_library(std::string_view library);
    // Adds a system library identity that installed providers implement
    // (process-wide, additive; never removed). Installers call it before the
    // loader runs so the identity loads instead of being reported missing.
    static void add_provider_library(std::string library);
    // Diagnostic probe (ALCHEMY_PROBE_MISSING=trap|zero, default off): Apple
    // system libraries without a provider load as empty providers and their
    // unresolved symbols bind to named stubs, so a trace shows which missing
    // interfaces a workload reaches. Never a compatibility claim.
    enum class ProbeMode { off, trap, zero };
    [[nodiscard]] static ProbeMode probe_mode();
    // A run-time lookup (dlsym) on this thread: probe stubs only stand in for
    // symbols bound at load time; dlsym of a missing symbol answers NULL, as
    // the guest expects (it often checks for optional interfaces that way).
    struct RuntimeLookup {
        RuntimeLookup() noexcept;
        ~RuntimeLookup();
        RuntimeLookup(const RuntimeLookup&) = delete;
        RuntimeLookup& operator=(const RuntimeLookup&) = delete;
        bool previous;
    };
    [[nodiscard]] static bool in_runtime_lookup() noexcept;
    [[nodiscard]] ObjectStore& objects() noexcept;
    void publish_image(ImageRuntimeInfo image);
    [[nodiscard]] std::vector<ImageRuntimeInfo> images() const;
    void register_finalizer(std::uint64_t function, std::uint64_t argument, std::uint64_t dso);
    // Runs `hook` at the start of ~ProcessContext (last registered first),
    // while this context and its engine are still valid. A service owned by
    // its guest-call handlers can outlive the context (FxEngine keeps retired
    // handlers until process exit), so it releases engine state here, never
    // in its destructor.
    void at_teardown(std::function<void()> hook);
    // Diagnostics (ALCHEMY_HANG_DUMP): sections that subsystems add to every
    // hang dump (called on the dump's own thread, without guest calls).
    void at_hang_dump(std::function<std::string()> section);
    [[nodiscard]] std::string hang_dump_sections() const;
    void run_finalizers(std::optional<std::uint64_t> dso = {});
    void request_exit(std::uint32_t status);
    [[nodiscard]] std::optional<std::uint32_t> requested_exit() const;
    void record_worker_execution(std::uint64_t instructions, const transmute::JitStats& stats);
    [[nodiscard]] ExecutionTotals worker_execution() const;

    // ---- main-thread wake (GUI run loop) ------------------------------------
    // Work for the main thread (main dispatch queue items, run loop sources
    // signalled from other threads, Metal completions) calls
    // wake_main_thread() after queueing it, so a main thread blocked in the
    // GUI event pump (MsgWaitForMultipleObjectsEx) returns and drains it.
    // The pump registers its waker once (typically SetEvent on its wake
    // HANDLE, or PostThreadMessage) and clears it (empty function) at
    // shutdown. A replaced waker can still be running on another thread for
    // a moment, so it must hold shared ownership of whatever it touches
    // (never a raw HANDLE that is closed right after clearing). Thread-safe;
    // callable from any thread, including
    // while the main thread itself is not waiting (the wake is then a no-op
    // or a spurious return, never lost work). Without a registered waker
    // (headless process) the call does nothing: dispatch_main and
    // _dispatch_main_queue_callback_4CF poll as before.
    void set_main_thread_waker(std::function<void()> waker);
    void wake_main_thread() const;

    std::function<std::optional<std::uint64_t>(std::string_view, std::string_view)> base_resolver;
    std::function<std::optional<std::uint32_t>(const transmute::SupervisorCall&,
        transmute::CpuState&, transmute::GuestMemory&)> syscall_extension;
    std::function<void()> join_workers;
    std::function<void(ThreadContext&)> cleanup_thread;
    std::function<void(ThreadContext&)> cleanup_objects;
    std::function<bool(std::uint64_t)> adopt_object;
    std::function<void(std::span<const ImageRuntimeInfo>)> prepare_images;
    std::function<void(std::span<const std::uint64_t>)> rollback_images;
    // A signal delivered to this process (libSystem's kill/raise to itself), before its action
    // runs, whether that action is a handler, SIG_IGN or the default: dispatch signal sources.
    std::function<void(int)> signal_observed;
    // Called once the main executable's segments are mapped at their fixed
    // addresses (before its dependencies load): work that may allocate host
    // memory on other threads starts here, never before.
    std::function<void()> main_image_mapped;
    std::function<void(std::uint64_t)> initialize_image;
    std::function<std::filesystem::path(std::string_view)> map_guest_path;
    // The process-independent name of the memory at a guest address inside a
    // MAP_SHARED file or shm_open mapping ("<volume>-<file index>" of the host
    // file plus the byte offset in it), or empty for private memory. Waits on
    // shared memory (ulock, process-shared pthread objects) key their
    // cross-process wake-ups by it. Set by DarwinServices.
    struct SharedLocation { std::string object; std::uint64_t offset{}; };
    std::function<std::optional<SharedLocation>(std::uint64_t)> shared_location;
    // The state of a child process of this process (dispatch PROC sources):
    // empty when `pid` is not a live or unreaped child, else whether it exited.
    // Set by DarwinServices.
    std::function<std::optional<bool>(std::uint32_t)> child_exited;
    // posix_spawn (darwin_process.inc) calls these around creating the child,
    // with the posix_spawnattr_t token (0: none) and the child's pid. The Mach
    // IPC provider passes the child its special ports this way.
    std::function<void(std::uint64_t)> spawn_starting;
    std::function<void(std::uint64_t, std::uint32_t)> spawn_started;
    // Set by DarwinServices (Mach fileports): guest descriptor `fd` described
    // for process `target` (a Windows pid; 0 is this process; empty for a bad
    // or untransferable descriptor), and a new guest descriptor made from a
    // description (which it consumes; -1 on failure).
    std::function<std::optional<PortableDescriptor>(int fd, std::uint32_t target)> export_descriptor;
    std::function<int(const PortableDescriptor&)> import_descriptor;
    // This thread's fork-child guard (null outside an emulated fork's child side).
    [[nodiscard]] static ForkChildGuard*& fork_child_guard() noexcept;
    // Set when this process is an Alchemy child (alchemy_process.hpp): its
    // argv, environment, identity and inherited descriptors.
    std::shared_ptr<const ChildLaunch> child_launch;
    // Descriptors the launcher handed this process (RunOptions::descriptors),
    // installed after the inherited ones; the descriptor table owns their handles.
    std::vector<ChildFd> host_descriptors;
    // Runtime options a child of this process repeats on its command line
    // (--runtime-dir, --dylib-dir, --load-base, --jit, ...).
    std::vector<std::string> launch_options;
    std::filesystem::path executable;
    std::filesystem::path working_directory;
    // Guest argv, including argv[0], available before module constructors run.
    std::vector<std::string> arguments;
    // The argc/argv main receives (guest stack; argv is followed by envp and
    // the apple[] vector as on Darwin). Zero until startup is initialized.
    std::uint64_t startup_argc{}, startup_argv{};
    std::vector<std::string> environment;
    std::uint64_t instruction_limit{10'000'000};
    bool jit{};
    bool jit_stats{};

private:
    struct Finalizer { std::uint64_t function{}, argument{}, dso{}; };
    transmute::ReferenceEngine& engine_;
    OutputWriter writer_;
    mutable std::mutex output_mutex_;
    mutable std::mutex mutex_;
    std::map<transmute::ReferenceEngine*, std::shared_ptr<ThreadContext>> threads_;
    // "<library>\x1f<symbol>" -> export (every import binding looks one up).
    string_hash_map<RuntimeExport> exports_;
    std::vector<std::uint64_t> callbacks_;
    std::map<std::uint64_t, std::uint64_t> allocations_;
    std::vector<ImageRuntimeInfo> images_;
    std::vector<Finalizer> finalizers_;
    std::uint64_t next_address_{0x600000000ULL};
    std::uint64_t next_thread_{1};
    std::optional<std::uint32_t> exit_;
    ExecutionTotals worker_totals_;
    std::unique_ptr<ObjectStore> objects_;
    mutable std::mutex waker_mutex_;
    std::shared_ptr<const std::function<void()>> main_waker_;
    std::vector<std::function<void()>> teardown_;
    std::vector<std::function<std::string()>> hang_dump_sections_;
};

} // namespace alchemy
