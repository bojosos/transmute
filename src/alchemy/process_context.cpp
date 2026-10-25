#include "alchemy/process_context.hpp"

#include "alchemy/engine_adapter.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/object_store.hpp"
#include "alchemy/perf.hpp"
#include "alchemy/placeholder_exports.hpp"
#include "alchemy/registration_audit.hpp"
#include "transmute/jit.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

namespace alchemy {
namespace {

constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";
// libSystem's component libraries, and /usr/lib/libSystem.dylib (the symbolic
// link to libSystem.B.dylib that code dlopens by its unversioned name).
constexpr std::array system_aliases{
    "/usr/lib/libSystem.dylib", "/usr/lib/system/libsystem_c.dylib", "/usr/lib/system/libsystem_m.dylib",
    "/usr/lib/system/libsystem_malloc.dylib", "/usr/lib/system/libsystem_pthread.dylib",
    "/usr/lib/system/libsystem_platform.dylib", "/usr/lib/system/libsystem_kernel.dylib",
    "/usr/lib/system/libdyld.dylib", "/usr/lib/system/libsystem_info.dylib",
    "/usr/lib/system/libsystem_blocks.dylib", "/usr/lib/system/libdispatch.dylib"};

bool system_alias(std::string_view value) {
    return std::find(system_aliases.begin(), system_aliases.end(), value) != system_aliases.end();
}

// Frameworks that re-export each other's API: AudioUnit.framework is a thin
// re-export of AudioToolbox, where the AudioComponent / AudioUnit /
// AudioOutputUnit functions live, so an image linked against either finds a
// name registered under the other (Chrome links AudioUnit, Electron
// AudioToolbox). One hop only.
constexpr std::string_view audio_toolbox_library = "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox";
constexpr std::string_view audio_unit_library = "/System/Library/Frameworks/AudioUnit.framework/Versions/A/AudioUnit";
std::string_view reexport_partner(std::string_view library) {
    if (library == audio_toolbox_library) return audio_unit_library;
    if (library == audio_unit_library) return audio_toolbox_library;
    return {};
}

// Invalidates every thread's ProcessContext::thread cache: bumped when a
// thread context is released and when a process context is created or
// destroyed (a later one may reuse its address).
std::atomic<std::uint64_t> thread_generation{1};

// ProcessContext::exports_ key.
std::string export_key(std::string_view library, std::string_view symbol) {
    std::string key;
    key.reserve(library.size() + symbol.size() + 1);
    key.append(library).append(1, '\x1f').append(symbol);
    return key;
}

} // namespace

ProcessContext::ProcessContext(transmute::ReferenceEngine& engine, OutputWriter writer)
    : engine_(engine), writer_(std::move(writer)) {
    if (!writer_) throw std::invalid_argument("Alchemy process requires an output backend");
    objects_ = std::make_unique<ObjectStore>(*this);
    thread_generation.fetch_add(1, std::memory_order_acq_rel);
}

ProcessContext::~ProcessContext() {
    std::vector<std::function<void()>> hooks;
    { std::lock_guard lock(mutex_); hooks.swap(teardown_); }
    for (auto hook = hooks.rbegin(); hook != hooks.rend(); ++hook) {
        try { (*hook)(); } catch (...) { }
    }
    thread_generation.fetch_add(1, std::memory_order_acq_rel);
    // Worker owners must quiesce before their service state or this context dies.
    objects_.reset();
    for (const auto address : callbacks_) engine_.unregister_guest_call(address);
    for (const auto& [address, size] : allocations_) {
        static_cast<void>(size);
        try { engine_.unmap_memory(address); } catch (...) { }
    }
}

transmute::ReferenceEngine& ProcessContext::calling_engine() const {
    auto* active = transmute::ReferenceEngine::current();
    return active ? *active : engine_;
}

std::shared_ptr<ThreadContext> ProcessContext::thread(transmute::ReferenceEngine& engine) {
    // Per-thread cache of the last answer (__tlv_bootstrap, errno and every
    // libSystem call ask, thousands of times per frame): valid while no
    // thread context was released since (generation).
    struct Cache {
        const ProcessContext* owner{};
        const transmute::ReferenceEngine* engine{};
        std::uint64_t generation{};
        std::shared_ptr<ThreadContext> value;
    };
    thread_local Cache cache;
    const auto generation = thread_generation.load(std::memory_order_acquire);
    if (cache.owner == this && cache.engine == &engine && cache.generation == generation && cache.value) return cache.value;
    {
        std::lock_guard lock(mutex_);
        const auto found = threads_.find(&engine);
        if (found != threads_.end()) {
            cache = {this, &engine, generation, found->second};
            return found->second;
        }
    }
    const auto errno_address = allocate_guest(8);
    auto result = std::make_shared<ThreadContext>();
    result->engine = &engine;
    result->errno_address = errno_address;
    bool inserted = false;
    {
        std::lock_guard lock(mutex_);
        auto [position, fresh] = threads_.try_emplace(&engine, result);
        inserted = fresh;
        if (fresh) result->identity = next_thread_++;
        else result = position->second;
    }
    if (!inserted) release_guest(errno_address);
    return result;
}

std::shared_ptr<ThreadContext> ProcessContext::current_thread() { return thread(calling_engine()); }

void ProcessContext::at_teardown(std::function<void()> hook) {
    std::lock_guard lock(mutex_);
    teardown_.push_back(std::move(hook));
}

std::vector<std::uint64_t> ProcessContext::thread_identities() const {
    std::lock_guard lock(mutex_);
    std::vector<std::uint64_t> identities;
    identities.reserve(threads_.size());
    for (const auto& [engine, thread] : threads_) { static_cast<void>(engine); identities.push_back(thread->identity); }
    std::sort(identities.begin(), identities.end());
    return identities;
}

std::vector<ProcessContext::ThreadSnapshot> ProcessContext::thread_snapshots() const {
    std::lock_guard lock(mutex_);
    std::vector<ThreadSnapshot> snapshots;
    snapshots.reserve(threads_.size());
    for (const auto& [engine, thread] : threads_) snapshots.push_back({thread->identity, engine->state()});
    return snapshots;
}

std::string ProcessContext::export_name(std::uint64_t address) const {
    std::lock_guard lock(mutex_);
    for (const auto& [key, value] : exports_) {
        if (value.address != address || value.kind != ExportKind::function) continue;
        const auto split = key.find('\x1f');
        return split == std::string::npos ? key : key.substr(split + 1) + " (" + key.substr(0, split) + ")";
    }
    return {};
}

void ProcessContext::release_thread(transmute::ReferenceEngine& engine) {
    std::shared_ptr<ThreadContext> retired;
    {
        std::lock_guard lock(mutex_);
        const auto found = threads_.find(&engine);
        if (found == threads_.end()) return;
        retired = std::move(found->second);
        threads_.erase(found);
        thread_generation.fetch_add(1, std::memory_order_acq_rel);
    }
    if (retired->dlerror_address) release_guest(retired->dlerror_address);
    release_guest(retired->errno_address);
}

std::uint64_t ProcessContext::allocate_guest(std::uint64_t size, std::uint64_t alignment) {
    if (alignment == 0 || !std::has_single_bit(alignment) || alignment > (1ULL << 30)) {
        throw std::invalid_argument("invalid guest allocation alignment");
    }
    size = std::max<std::uint64_t>(size, 1);
    if (size > std::numeric_limits<std::size_t>::max() || size > (1ULL << 32)) {
        throw std::bad_alloc();
    }
    std::lock_guard lock(mutex_);
    const auto mask = alignment - 1;
    if (next_address_ > UINT64_MAX - mask) throw std::bad_alloc();
    auto address = (next_address_ + mask) & ~mask;
    for (unsigned attempt = 0;; ++attempt) {
        for (;;) {
            if (address > UINT64_MAX - size - 0x4000 - mask) throw std::bad_alloc();
            if (engine_.is_range_available(address, size)) break;
            address = (address + size + 0x4000 + mask) & ~mask;
        }
        // Another host thread (a driver, the thread pool) may take the range
        // between the check and the mapping: try the next one.
        try {
            engine_.map_memory(address, static_cast<std::size_t>(size));
            break;
        } catch (const std::exception&) {
            if (attempt >= 8 || engine_.is_range_available(address, size)) throw;
            address = (address + size + 0x4000 + mask) & ~mask;
        }
    }
    try { allocations_.emplace(address, size); }
    catch (...) { engine_.unmap_memory(address); throw; }
    next_address_ = address + size + 0x4000;
    return address;
}

void ProcessContext::release_guest(std::uint64_t address) {
    if (address == 0) return;
    std::lock_guard lock(mutex_);
    const auto found = allocations_.find(address);
    if (found == allocations_.end()) throw std::runtime_error("invalid Alchemy guest allocation");
    engine_.unmap_memory(address);
    allocations_.erase(found);
}

void ProcessContext::set_errno(std::uint32_t value) {
    const auto current = current_thread();
    calling_engine().write32(current->errno_address, value);
}

void ProcessContext::set_dlerror(std::string message) { current_thread()->dlerror = std::move(message); }

OutputResult ProcessContext::output(unsigned descriptor, std::span<const std::uint8_t> bytes) const {
    // ALCHEMY_PERF: a guest line "ALCHEMY_PERF_PHASE <name>" records a
    // startup phase (scripts/perf/startup_probe.py marks Python and UI milestones).
    if (perf::enabled()) {
        const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        constexpr std::string_view marker = "ALCHEMY_PERF_PHASE ";
        for (auto at = text.find(marker); at != std::string_view::npos; at = text.find(marker, at + 1)) {
            const auto begin = at + marker.size();
            const auto end = std::min({text.find('\n', begin), text.find(" @", begin), text.size()});
            perf::phase("guest: " + std::string(text.substr(begin, end - begin)));
        }
    }
    std::lock_guard lock(output_mutex_);
    const auto result = writer_(descriptor, bytes);
    if (result.bytes_written > bytes.size()) {
        throw std::runtime_error("Alchemy output backend returned an invalid byte count");
    }
    return result;
}

namespace {
// ALCHEMY_TRACE_CALLS=<comma-separated symbol prefixes, or *; -prefix excludes>: diagnostic log
// of provider calls (process id and thread hash, symbol, x0..x3 on entry, x0 on return)
// to stderr. ALCHEMY_TRACE_CALLS_FILTER=<text> limits it to the Alchemy
// processes whose host command line contains <text> (for example
// "--alchemy-child-spec": guest children only).
const std::vector<std::string>& traced_call_prefixes() {
    static const std::vector<std::string> prefixes = [] {
        std::vector<std::string> values;
        std::string text, filter;
#ifdef _WIN32
        char* buffer{}; std::size_t length{};
        if (_dupenv_s(&buffer, &length, "ALCHEMY_TRACE_CALLS") == 0 && buffer) { text = buffer; std::free(buffer); }
        buffer = nullptr;
        if (_dupenv_s(&buffer, &length, "ALCHEMY_TRACE_CALLS_FILTER") == 0 && buffer) { filter = buffer; std::free(buffer); }
#else
        if (const char* value = std::getenv("ALCHEMY_TRACE_CALLS")) text = value;
        if (const char* value = std::getenv("ALCHEMY_TRACE_CALLS_FILTER")) filter = value;
#endif
        if (!filter.empty()) {
            std::string line;
#ifdef _WIN32
            for (int index = 0; index < __argc; ++index) {
                if (__argv) line += std::string(__argv[index]) + ' ';
                else if (__wargv) { for (const wchar_t* c = __wargv[index]; *c; ++c) line += static_cast<char>(*c < 0x80 ? *c : '?'); line += ' '; }
            }
#endif
            if (line.find(filter) == std::string::npos) return values;
        }
        std::size_t begin = 0;
        while (begin <= text.size() && !text.empty()) {
            const auto comma = text.find(',', begin);
            auto item = text.substr(begin, comma == std::string::npos ? std::string::npos : comma - begin);
            // "-name" excludes a prefix (kept with its '-' marker).
            const bool exclude = !item.empty() && item.front() == '-';
            if (exclude) item.erase(0, 1);
            if (!item.empty()) values.push_back((exclude ? "-" : "") + (item == "*" ? std::string() : (item.front() == '_' ? item : "_" + item)));
            if (comma == std::string::npos) break;
            begin = comma + 1;
        }
        return values;
    }();
    return prefixes;
}
thread_local ForkChildGuard* current_fork_child_guard = nullptr;
} // namespace

ForkChildGuard*& ProcessContext::fork_child_guard() noexcept { return current_fork_child_guard; }

std::uint64_t ProcessContext::install_function(std::string symbol,
    transmute::GuestCallHandler handler, std::string library, const std::source_location& where) {
    registration_audit::record_export(system_alias(library) ? libsystem : std::string_view(library), symbol, where,
        system_alias(library));
    const auto& prefixes = traced_call_prefixes();
    const bool traced = !prefixes.empty() &&
        std::any_of(prefixes.begin(), prefixes.end(),
            [&](const std::string& prefix) { return prefix.empty() || (prefix.front() != '-' && symbol.starts_with(prefix)); }) &&
        std::none_of(prefixes.begin(), prefixes.end(),
            [&](const std::string& prefix) { return prefix.size() > 1 && prefix.front() == '-' && symbol.starts_with(prefix.substr(1)); });
    if (traced) {
        handler = [this, inner = std::move(handler), name = symbol](transmute::CpuState& state, transmute::GuestMemory& memory) {
            char line[256];
            const auto process = static_cast<unsigned long long>(host::process::current_pid());
            const auto thread = (process << 16) | (std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffff);
            std::snprintf(line, sizeof(line), "[call %llx] %s(0x%llx, 0x%llx, 0x%llx, 0x%llx)", thread, name.c_str(),
                static_cast<unsigned long long>(state.x[0]), static_cast<unsigned long long>(state.x[1]),
                static_cast<unsigned long long>(state.x[2]), static_cast<unsigned long long>(state.x[3]));
            std::string text = line;
            // A readable C string first argument is shown (paths, formats).
            try {
                bool terminated{};
                const auto bytes = memory.read_string(state.x[0], 1, 160, &terminated);
                if (terminated && !bytes.empty() && std::all_of(bytes.begin(), bytes.end(), [](auto c) { return c >= 0x20 && c < 0x7f; }))
                    text += " \"" + std::string(bytes.begin(), bytes.end()) + "\"";
            } catch (...) {}
            std::fprintf(stderr, "%s\n", text.c_str());
            inner(state, memory);
            // A failure result (-1 as int or pointer) shows the guest errno.
            if (static_cast<std::uint32_t>(state.x[0]) == UINT32_MAX) {
                std::uint32_t error{};
                try { if (const auto current = current_thread()) error = memory.read32(current->errno_address); } catch (...) {}
                std::fprintf(stderr, "[call %llx] %s -> 0x%llx (errno %u)\n", thread, name.c_str(), static_cast<unsigned long long>(state.x[0]), error);
                return;
            }
            std::fprintf(stderr, "[call %llx] %s -> 0x%llx\n", thread, name.c_str(), static_cast<unsigned long long>(state.x[0]));
        };
    }
    // Installing over a placeholder (an explicit-failure stub) makes this the
    // real export: later gap-filling providers must not replace it.
    static_cast<void>(take_placeholder_export(*this, library, symbol));
    // Every provider call is checked on a thread that runs a fork child.
    auto guarded = [symbol, library, inner = std::move(handler)](transmute::CpuState& state, transmute::GuestMemory& memory) {
        if (auto* guard = current_fork_child_guard) guard->check_call(library, symbol);
        inner(state, memory);
    };
    const auto address = engine_.register_guest_call(std::move(guarded));
    perf::name_address(address, symbol);
    // The hottest libSystem/libobjc calls (thousands per frame) touch no
    // SIMD/FP register: their entries copy only the integer state. A callee
    // that runs guest code (objc_release -> dealloc) is fine: the vector
    // registers it leaves behind are caller-saved or restored by the callee.
    static const std::array<std::string_view, 31> integer_only{
        "___tlv_bootstrap", "__tlv_bootstrap", "__alchemy_tlv_bootstrap_slow_path", "_malloc", "_calloc", "_free", "_realloc", "_posix_memalign", "_malloc_size",
        "_memcpy", "_memmove", "_memset", "_bzero", "_memcmp", "_memchr", "_strlen", "_strcmp", "_strncmp", "_strchr",
        "_strcpy", "_pthread_self", "_pthread_equal", "_pthread_mutex_lock", "_pthread_mutex_unlock",
        "_pthread_mutex_trylock", "_pthread_getspecific", "_pthread_setspecific", "___error", "_objc_retain",
        "_objc_release", "_objc_autoreleasePoolPush"};
    // (__tlv_bootstrap's slow path is libSystem's under an internal name.)
    if (library == libsystem || library == "/usr/lib/libobjc.A.dylib" || library == "alchemy.internal")
        if (std::find(integer_only.begin(), integer_only.end(), symbol) != integer_only.end())
            mark_engine_call_integer_only(engine_, address);
    // libSystem's pure memory/string functions may skip their handler (and the
    // fork-child check, which allows them) while no byte faults; a traced call
    // keeps its trace.
    if (library == libsystem && !traced) mark_engine_call_leaf(engine_, address, symbol);
    try {
        std::lock_guard lock(mutex_);
        callbacks_.push_back(address);
        exports_.insert_or_assign(export_key(library, symbol), RuntimeExport{address});
    } catch (...) {
        engine_.unregister_guest_call(address);
        throw;
    }
    return address;
}

void ProcessContext::install_export(std::string library, std::string symbol, RuntimeExport value,
    const std::source_location& where) {
    registration_audit::record_export(system_alias(library) ? libsystem : std::string_view(library), symbol, where,
        system_alias(library), value.address);
    static_cast<void>(take_placeholder_export(*this, library, symbol));
    std::lock_guard lock(mutex_);
    exports_.insert_or_assign(export_key(library, symbol), value);
}

std::optional<RuntimeExport> ProcessContext::resolve(std::string_view library,
                                                   std::string_view symbol) const {
    const auto selected = system_alias(library) ? libsystem : library;
    const auto partner = reexport_partner(selected);
    {
        std::lock_guard lock(mutex_);
        thread_local std::string key;
        key.assign(selected).append(1, '\x1f').append(symbol);
        if (const auto found = exports_.find(key); found != exports_.end()) return found->second;
        if (!partner.empty()) {
            key.assign(partner).append(1, '\x1f').append(symbol);
            if (const auto found = exports_.find(key); found != exports_.end()) return found->second;
        }
    }
    if (base_resolver) {
        if (const auto address = base_resolver(selected, symbol)) return RuntimeExport{*address};
        if (!partner.empty())
            if (const auto address = base_resolver(partner, symbol)) return RuntimeExport{*address};
    }
    return std::nullopt;
}

namespace {
thread_local bool runtime_lookup_active = false;
} // namespace
ProcessContext::RuntimeLookup::RuntimeLookup() noexcept : previous(runtime_lookup_active) { runtime_lookup_active = true; }
ProcessContext::RuntimeLookup::~RuntimeLookup() { runtime_lookup_active = previous; }
bool ProcessContext::in_runtime_lookup() noexcept { return runtime_lookup_active; }

ProcessContext::ProbeMode ProcessContext::probe_mode() {
    static const ProbeMode mode = [] {
        std::string value;
#ifdef _MSC_VER
        char* buffer = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&buffer, &length, "ALCHEMY_PROBE_MISSING") == 0 && buffer) {
            value = buffer;
            std::free(buffer);
        }
#else
        if (const char* buffer = std::getenv("ALCHEMY_PROBE_MISSING")) value = buffer;
#endif
        if (value == "trap") return ProbeMode::trap;
        if (value == "zero") return ProbeMode::zero;
        return ProbeMode::off;
    }();
    return mode;
}

namespace {
std::mutex& added_provider_mutex() { static std::mutex value; return value; }
std::set<std::string, std::less<>>& added_provider_libraries() { static std::set<std::string, std::less<>> value; return value; }
} // namespace

void ProcessContext::add_provider_library(std::string library) {
    std::lock_guard lock(added_provider_mutex());
    added_provider_libraries().insert(std::move(library));
}

bool ProcessContext::is_provider_library(std::string_view library) {
    {
        std::lock_guard lock(added_provider_mutex());
        if (added_provider_libraries().contains(library)) return true;
    }
    if (probe_mode() != ProbeMode::off &&
        (library.starts_with("/System/Library/Frameworks/") || library.starts_with("/usr/lib/")))
        return true;
    return library == libsystem || system_alias(library) || library == "/usr/lib/libc++.1.dylib" ||
        library == "/usr/lib/libobjc.A.dylib" ||
        library == "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation" ||
        library == "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation" ||
        library == "/System/Library/Frameworks/Accelerate.framework/Versions/A/Accelerate" || darwin_framework_library(library);
}

ObjectStore& ProcessContext::objects() noexcept { return *objects_; }

void ProcessContext::publish_image(ImageRuntimeInfo image) {
    {
        std::uint64_t text_begin = UINT64_MAX, text_end = 0;
        for (const auto& range : image.code_ranges) {
            text_begin = std::min(text_begin, range.begin);
            text_end = std::max(text_end, range.end);
        }
        if (text_begin >= text_end) text_begin = text_end = 0;
        register_engine_image(engine_, image.header, image.begin, image.end, text_begin, text_end, image.name);
    }
    std::lock_guard lock(mutex_);
    const auto found = std::find_if(images_.begin(), images_.end(), [&](const auto& item) {
        return item.module_id == image.module_id;
    });
    if (found == images_.end()) images_.push_back(std::move(image));
    else *found = std::move(image);
}

std::vector<ImageRuntimeInfo> ProcessContext::images() const {
    std::lock_guard lock(mutex_);
    return images_;
}

void ProcessContext::register_finalizer(std::uint64_t function, std::uint64_t argument,
                                        std::uint64_t dso) {
    if (function == 0) throw std::invalid_argument("null guest finalizer");
    std::lock_guard lock(mutex_);
    finalizers_.push_back({function, argument, dso});
}

void ProcessContext::run_finalizers(std::optional<std::uint64_t> dso) {
    for (;;) {
        if (requested_exit()) return;
        std::optional<Finalizer> selected;
        {
            std::lock_guard lock(mutex_);
            const auto found = std::find_if(finalizers_.rbegin(), finalizers_.rend(), [&](const auto& item) {
                return !dso || *dso == 0 || item.dso == *dso;
            });
            if (found == finalizers_.rend()) return;
            selected = *found;
            finalizers_.erase(std::next(found).base());
        }
        const std::array callback_arguments{selected->argument};
        static_cast<void>(EngineAdapter(calling_engine()).call(selected->function, callback_arguments));
    }
}

void ProcessContext::request_exit(std::uint32_t status) {
    std::lock_guard lock(mutex_);
    if (!exit_) exit_ = status;
}

std::optional<std::uint32_t> ProcessContext::requested_exit() const {
    std::lock_guard lock(mutex_);
    return exit_;
}

void ProcessContext::record_worker_execution(std::uint64_t instructions, const transmute::JitStats& stats) {
    std::lock_guard lock(mutex_);
    worker_totals_.instructions += instructions;
    worker_totals_.native_guest_instructions += stats.native_guest_instructions;
    worker_totals_.native_blocks += stats.native_blocks;
    worker_totals_.fallback_instructions += stats.fallback_instructions;
}

ExecutionTotals ProcessContext::worker_execution() const {
    std::lock_guard lock(mutex_);
    return worker_totals_;
}

void ProcessContext::set_main_thread_waker(std::function<void()> waker) {
    auto value = waker ? std::make_shared<const std::function<void()>>(std::move(waker)) : nullptr;
    std::lock_guard lock(waker_mutex_);
    main_waker_ = std::move(value);
}

void ProcessContext::wake_main_thread() const {
    std::shared_ptr<const std::function<void()>> waker;
    {
        std::lock_guard lock(waker_mutex_);
        waker = main_waker_;
    }
    // Called outside the lock: a waker may itself queue work or log.
    if (waker) (*waker)();
}

} // namespace alchemy
