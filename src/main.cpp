#include "transmute/a64.hpp"
#include "transmute/guest_process.hpp"
#include "transmute/jit.hpp"
#include "transmute/pe_image.hpp"
#ifdef _WIN32
#include "transmute/win32_bridge.hpp"
#include "win32_native_imports.hpp"
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>
#endif

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <charconv>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

#ifdef _WIN32
class SyncDiagnostics {
    struct LastErrorGuard {
        DWORD value = GetLastError();
        ~LastErrorGuard() { SetLastError(value); }
    };

    struct Record {
        std::uint64_t token{};
        DWORD thread{};
        std::string imported;
        std::array<std::uint64_t, 4> arguments{};
        std::uint64_t lr{};
        std::optional<std::string> caller;
        std::vector<std::string> frames;
        std::chrono::steady_clock::time_point started;
    };

public:
    class Call {
    public:
        Call() = default;
        Call(SyncDiagnostics* owner, DWORD thread, std::uint64_t token)
            : owner_(owner), thread_(thread), token_(token) {}
        Call(const Call&) = delete;
        Call& operator=(const Call&) = delete;
        Call(Call&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)),
              thread_(other.thread_), token_(other.token_) {}
        ~Call() {
            if (owner_) owner_->leave(thread_, token_);
        }

    private:
        SyncDiagnostics* owner_{};
        DWORD thread_{};
        std::uint64_t token_{};
    };

    explicit SyncDiagnostics(transmute::Win32Bridge& bridge)
        : bridge_(bridge),
          monitor_([this](std::stop_token stop) { monitor(stop); }) {}
    ~SyncDiagnostics() { stop(); }

    void stop() {
        LastErrorGuard preserve_error;
        monitor_.request_stop();
        changed_.notify_all();
        if (monitor_.joinable()) monitor_.join();
    }

    Call enter(const transmute::Import& imported,
               const transmute::CpuState& state,
               transmute::GuestMemory& memory,
               const transmute::Win32Bridge& bridge) noexcept {
        LastErrorGuard preserve_error;
        if (!tracked(imported.function)) return {};
        try {
            const auto thread = GetCurrentThreadId();
            Record record;
            record.thread = thread;
            record.imported = imported.dll + '!' + imported.function;
            for (std::size_t index = 0; index < record.arguments.size(); ++index) {
                record.arguments[index] = state.x[index];
            }
            record.lr = state.x[30];
            record.started = std::chrono::steady_clock::now();
            std::uint64_t token;
            {
                std::lock_guard lock(mutex_);
                token = record.token = ++next_token_;
                active_[thread].push_back(std::move(record));
            }
            Call call(this, thread, token);
            // Register first so a wait on the loader's module lock remains visible.
            const auto caller = bridge.describe_guest_address(state.x[30]);
            std::vector<std::string> frames;
            try {
                auto frame = state.x[29];
                for (unsigned depth = 0; depth < 4; ++depth) {
                    if (frame < state.sp || frame - state.sp > 1024U * 1024U ||
                        frame > UINT64_MAX - 16 || (frame & 7U) != 0) break;
                    const auto region = memory.query_region(frame);
                    if (!region || !region->readable ||
                        frame + 16 > region->base + region->size) break;
                    const auto saved_lr = memory.read64(frame + 8);
                    if (const auto description = bridge.describe_guest_address(saved_lr)) {
                        frames.push_back(*description);
                    }
                    const auto previous = memory.read64(frame);
                    if (previous <= frame) break;
                    frame = previous;
                }
            } catch (...) {
                // A missing frame chain must not hide the synchronization call.
            }
            {
                std::lock_guard lock(mutex_);
                for (auto& active : active_.at(thread)) {
                    if (active.token == token) {
                        active.caller = caller;
                        active.frames = frames;
                    }
                }
            }
            return call;
        } catch (...) {
            return {};
        }
    }

private:
    static bool tracked(std::string_view name) {
        return name == "WaitForSingleObject" || name == "WaitForSingleObjectEx" ||
               name == "WaitForMultipleObjects" || name == "WaitForMultipleObjectsEx" ||
               name == "SignalObjectAndWait" || name == "AcquireSRWLockExclusive" ||
               name == "AcquireSRWLockShared" || name == "SleepConditionVariableSRW" ||
               name == "SleepConditionVariableCS" || name == "EnterCriticalSection" ||
               name == "InitOnceBeginInitialize" || name == "WaitOnAddress" ||
               name == "LoadLibraryA" || name == "LoadLibraryW" ||
               name == "LoadLibraryExA" || name == "LoadLibraryExW";
    }

    void leave(DWORD thread, std::uint64_t token) noexcept {
        LastErrorGuard preserve_error;
        try {
            std::lock_guard lock(mutex_);
            const auto found = active_.find(thread);
            if (found == active_.end()) return;
            auto& records = found->second;
            for (auto entry = records.begin(); entry != records.end(); ++entry) {
                if (entry->token != token) continue;
                records.erase(entry);
                break;
            }
            if (records.empty()) active_.erase(found);
        } catch (...) {
            // Diagnostics must not replace a guest result or exception.
        }
    }

    void monitor(std::stop_token stop) noexcept {
        try {
            std::string reported_worker_failure;
            std::unique_lock lock(mutex_);
            while (!stop.stop_requested()) {
                changed_.wait_for(lock, std::chrono::seconds(15), [&] {
                    return stop.stop_requested();
                });
                if (stop.stop_requested()) break;
                std::vector<Record> snapshot;
                for (const auto& [thread, records] : active_) {
                    static_cast<void>(thread);
                    snapshot.insert(snapshot.end(), records.begin(), records.end());
                }
                lock.unlock();
                LastErrorGuard preserve_error;
                std::ostringstream report;
                try {
                    bridge_.check_worker_failures();
                } catch (const std::exception& error) {
                    if (reported_worker_failure != error.what()) {
                        reported_worker_failure = error.what();
                        report << "[transmute-sync] " << error.what() << '\n';
                    }
                }
                if (!snapshot.empty()) {
                    const auto now = std::chrono::steady_clock::now();
                    report << "[transmute-sync] guest synchronization calls in progress: "
                           << snapshot.size() << '\n';
                    for (const auto& record : snapshot) {
                        const auto elapsed = std::chrono::duration<double>(
                            now - record.started).count();
                        report << "  thread " << record.thread << ' '
                               << record.imported << " for " << elapsed << "s";
                        for (std::size_t index = 0; index < record.arguments.size(); ++index) {
                            report << " x" << index << "=0x" << std::hex
                                   << record.arguments[index] << std::dec;
                        }
                        report << " LR=0x" << std::hex << record.lr << std::dec;
                        if (record.caller) report << " caller " << *record.caller;
                        for (const auto& frame : record.frames) {
                            report << " <- " << frame;
                        }
                        report << '\n';
                    }
                }
                std::cerr << report.str();
                lock.lock();
            }
        } catch (...) {
            // Keep a diagnostic allocation or output failure off the guest path.
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::unordered_map<DWORD, std::vector<Record>> active_;
    std::uint64_t next_token_{};
    transmute::Win32Bridge& bridge_;
    std::jthread monitor_;
};

std::wstring quote_windows_argument(std::wstring_view argument) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto character : argument) {
        if (character == L'\\') {
            ++slashes;
        } else if (character == L'"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(L'"');
            slashes = 0;
        } else {
            result.append(slashes, L'\\');
            result.push_back(character);
            slashes = 0;
        }
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}
#endif

} // namespace

int main(int argc, char** argv) {
    try {
        int argument = 1;
        const bool doctor = argument < argc && std::string_view(argv[argument]) == "doctor";
        if (doctor) ++argument;
        std::optional<std::uint64_t> load_base;
        std::optional<std::uint64_t> instruction_limit;
        bool jit = false;
        bool jit_stats = false;
        bool dependencies = false;
        bool diagnose_exceptions = false;
        bool diagnose_sync = false;
        std::vector<std::filesystem::path> dll_directories;
#ifdef _WIN32
        std::optional<std::pair<std::uint64_t, std::uint32_t>> child_bootstrap;
#endif
        while (argument < argc) {
            const std::string_view option(argv[argument]);
            if (option == "--load-base") {
                if (++argument >= argc) throw std::runtime_error("missing load base");
                std::string_view value = argv[argument++];
                const auto radix = value.starts_with("0x") ? 16 : 10;
                if (radix == 16) value.remove_prefix(2);
                std::uint64_t parsed = 0;
                const auto [end, error] = std::from_chars(value.data(),
                                                           value.data() + value.size(),
                                                           parsed, radix);
                if (error != std::errc{} || end != value.data() + value.size()) {
                    throw std::runtime_error("invalid load base");
                }
                load_base = parsed;
            } else if (option == "--dll-dir") {
                if (++argument >= argc) throw std::runtime_error("missing DLL directory");
                dll_directories.emplace_back(argv[argument++]);
            } else if (option == "--instruction-limit") {
                if (++argument >= argc) {
                    throw std::runtime_error("missing instruction limit");
                }
                const std::string_view value(argv[argument++]);
                std::uint64_t parsed = 0;
                const auto [end, error] = std::from_chars(
                    value.data(), value.data() + value.size(), parsed);
                if (error != std::errc{} ||
                    end != value.data() + value.size() || parsed == 0 ||
                    parsed == UINT64_MAX) {
                    throw std::runtime_error("invalid instruction limit");
                }
                instruction_limit = parsed;
            } else if (option == "--jit") {
                jit = true;
                ++argument;
            } else if (option == "--jit-stats") {
                jit = true;
                jit_stats = true;
                ++argument;
            } else if (option == "--deps") {
                if (!doctor) throw std::runtime_error("--deps requires doctor");
                dependencies = true;
                ++argument;
            } else if (option == "--diagnose-exceptions") {
                diagnose_exceptions = true;
                ++argument;
            } else if (option == "--diagnose-sync") {
                diagnose_sync = true;
                ++argument;
#ifdef _WIN32
            } else if (option == "--child-bootstrap") {
                if (argument + 2 >= argc || child_bootstrap) {
                    throw std::runtime_error("invalid child bootstrap arguments");
                }
                const std::string_view handle_text(argv[++argument]);
                const std::string_view size_text(argv[++argument]);
                std::uint64_t handle = 0;
                std::uint32_t size = 0;
                const auto [handle_end, handle_error] = std::from_chars(
                    handle_text.data(), handle_text.data() + handle_text.size(),
                    handle);
                const auto [size_end, size_error] = std::from_chars(
                    size_text.data(), size_text.data() + size_text.size(), size);
                if (handle_error != std::errc{} || size_error != std::errc{} ||
                    handle_end != handle_text.data() + handle_text.size() ||
                    size_end != size_text.data() + size_text.size()) {
                    throw std::runtime_error("invalid child bootstrap numbers");
                }
                child_bootstrap.emplace(handle, size);
                ++argument;
#endif
            } else {
                break;
            }
        }
        if (argc - argument < 1 || (doctor && argc - argument != 1)) {
            std::cerr << "usage: transmute [doctor] [--load-base <address>] "
                         "[--dll-dir <directory>]... "
                         "[--instruction-limit <count>] [--jit] [--jit-stats] "
                         "[--deps (doctor only)] [--diagnose-exceptions] [--diagnose-sync] "
                         "<arm64.exe> "
                         "[guest args...]\n";
            return 2;
        }
        std::filesystem::path path = argv[argument];
#ifdef _WIN32
        int wide_argc = 0;
        auto** wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
        if (!wide_argv || wide_argc != argc) {
            if (wide_argv) LocalFree(wide_argv);
            throw std::runtime_error("cannot recover Unicode command-line arguments");
        }
        path = wide_argv[argument];
        std::wstring guest_command_line = quote_windows_argument(
            std::filesystem::absolute(path).wstring());
        for (int index = argument + 1; index < argc; ++index) {
            guest_command_line.push_back(L' ');
            guest_command_line += quote_windows_argument(wide_argv[index]);
        }
        LocalFree(wide_argv);
        std::optional<transmute::ChildBootstrap> inherited_bootstrap;
        if (child_bootstrap) {
            inherited_bootstrap = transmute::read_child_bootstrap(
                child_bootstrap->first, child_bootstrap->second);
            CloseHandle(reinterpret_cast<HANDLE>(child_bootstrap->first));
        }
#endif
        if (doctor) {
            if (dependencies) {
                transmute::GuestProcess process(path, load_base, dll_directories);
                for (const auto& module : process.modules()) {
                    std::cout << "module 0x" << std::hex << module->base()
                              << "..0x" << module->base() + module->size()
                              << std::dec << ' ' << module->source() << '\n';
                }
                return 0;
            }
            std::ifstream input(path, std::ios::binary);
            if (!input) throw std::runtime_error("cannot open executable");
            const std::vector<std::uint8_t> file{std::istreambuf_iterator<char>(input),
                                                  std::istreambuf_iterator<char>()};
            auto image = transmute::PeImage::parse(file, load_base);
            std::cout << "ARM64 PE image; entry RVA: 0x" << std::hex << image.entry_rva()
                      << "; preferred base: 0x" << image.preferred_base()
                      << "; load base: 0x" << image.base()
                      << "; mapped bytes: 0x" << image.size() << std::dec
                      << "; imports: " << (image.has_imports() ? "yes" : "no") << '\n';
            for (const auto& section : image.sections()) {
                std::cout << "  " << section.name << " RVA 0x" << std::hex << section.rva
                          << " size 0x" << section.size << std::dec << '\n';
            }
            for (const auto& imported : image.imports()) {
                std::cout << "  import " << imported.dll << '!' << imported.function << '\n';
            }
            return 0;
        }
        transmute::GuestProcess process(path, load_base, dll_directories);
#ifdef _WIN32
        transmute::Win32Bridge bridge;
        std::array<wchar_t, 32768> host_executable{};
        const auto host_executable_length = GetModuleFileNameW(
            nullptr, host_executable.data(),
            static_cast<DWORD>(host_executable.size()));
        if (host_executable_length == 0 ||
            host_executable_length >= host_executable.size()) {
            throw std::runtime_error("cannot locate Transmute child launcher");
        }
        bridge.configure_child_launch(
            std::filesystem::path(host_executable.data()), dll_directories,
            instruction_limit, jit);
        if (inherited_bootstrap) {
            bridge.apply_child_bootstrap(std::move(*inherited_bootstrap));
        } else {
            bridge.set_guest_command_line(std::move(guest_command_line));
        }
        auto sync_diagnostics = diagnose_sync
            ? std::make_shared<SyncDiagnostics>(bridge) : nullptr;
        struct StopSyncDiagnostics {
            std::shared_ptr<SyncDiagnostics> diagnostics;
            ~StopSyncDiagnostics() {
                if (diagnostics) diagnostics->stop();
            }
        } stop_sync_diagnostics{sync_diagnostics};
        transmute::ReferenceEngine engine(
            process.executable(),
            [&bridge, diagnose_exceptions, sync_diagnostics](const transmute::Import& imported,
                      transmute::CpuState& state,
                      transmute::GuestMemory& memory) {
                auto sync_call = sync_diagnostics
                    ? sync_diagnostics->enter(imported, state, memory, bridge)
                    : SyncDiagnostics::Call{};
                if (diagnose_exceptions && imported.function == "_beginthreadex") {
                    std::cerr << "guest _beginthreadex args:";
                    for (unsigned index = 0; index < 6; ++index) {
                        std::cerr << " 0x" << std::hex << state.x[index];
                    }
                    std::cerr << std::dec << '\n';
                }
                if (diagnose_exceptions && imported.function == "RaiseException" &&
                    state.x[0] == 0xe06d7363 && state.x[2] >= 4 && state.x[3]) {
                    try {
                        const auto record = state.x[3];
                        std::cerr << "MSVC C++ exception parameters:";
                        for (unsigned index = 0; index < 4; ++index) {
                            std::cerr << " 0x" << std::hex << memory.read64(record + 8 * index);
                        }
                        std::cerr << std::dec << '\n';
                        std::cerr << "MSVC throw caller (saved LR): 0x" << std::hex
                                  << memory.read64(state.sp + 8)
                                  << "; outer saved LR: 0x"
                                  << memory.read64(state.sp + 0x80)
                                  << "; TBB saved LR: 0x"
                                  << memory.read64(state.sp + 0xa8)
                                  << "; TBB entry LR: 0x"
                                  << memory.read64(state.sp + 0x1d0)
                                  << "; TBB error: 0x"
                                  << memory.read64(state.sp + 0x40)
                                  << std::dec << '\n';
                        const auto object = memory.read64(record + 8);
                        const auto message = memory.read64(object + 8);
                        std::cerr << "MSVC exception object words: 0x" << std::hex
                                  << memory.read64(object) << " 0x" << message
                                  << " 0x" << memory.read64(object + 16)
                                  << std::dec << '\n';
                        if (memory.query_region(message)) {
                            std::string value;
                            for (unsigned index = 0; index < 256; ++index) {
                                const auto character = memory.read8(message + index);
                                if (character == 0) break;
                                if (character < 32 || character > 126) {
                                    value.clear();
                                    break;
                                }
                                value.push_back(static_cast<char>(character));
                            }
                            if (!value.empty()) {
                                std::cerr << "MSVC exception object message: " << value << '\n';
                            }
                        }
                    } catch (const std::exception& error) {
                        std::cerr << "MSVC C++ exception parameter read failed: "
                                  << error.what() << '\n';
                    }
                }
                const bool allocation = imported.function == "malloc" ||
                    imported.function == "calloc" ||
                    imported.function == "realloc" ||
                    imported.function == "_aligned_malloc" ||
                    imported.function == "VirtualAlloc" ||
                    imported.function == "HeapAlloc";
                const auto requested = state.x[0];
                const auto secondary = state.x[1];
                const auto result = [&] {
                    try {
                        return bridge.dispatch(imported, state, memory);
                    } catch (const std::runtime_error& error) {
                        if (!std::string_view(error.what()).starts_with(
                                "unsupported Windows import ")) {
                            throw;
                        }
                        if (const auto caller =
                                bridge.describe_guest_address(state.x[30])) {
                            throw std::runtime_error(
                                std::string(error.what()) +
                                " called from " + *caller);
                        }
                        throw;
                    }
                }();
                if (diagnose_exceptions && allocation && state.x[0] == 0) {
                    std::cerr << "guest allocation failed " << imported.dll << '!'
                              << imported.function << " args 0x" << std::hex
                              << requested << " 0x" << secondary << std::dec << '\n';
                }
                return result;
            });
        bridge.set_guest_dll_loader(
            [&process, &bridge](
                const std::filesystem::path& requested,
                const std::vector<std::filesystem::path>& directories,
                bool include_requested_directory,
                transmute::ReferenceEngine& caller) {
                return bridge.load_guest_dll(process, requested, directories,
                                             include_requested_directory, caller);
            });
        bridge.set_guest_callback_invoker(
            [&engine](std::uint64_t address, std::uint64_t argument) {
                auto* current = transmute::ReferenceEngine::current();
                return (current == nullptr ? &engine : current)->invoke_callback(
                    address, argument);
            });
        bridge.set_guest_tls_callback_invoker(
            [&engine](std::uint64_t address, std::uint64_t module,
                      std::uint64_t reason, std::uint64_t reserved) {
                const std::array arguments{module, reason, reserved};
                auto* current = transmute::ReferenceEngine::current();
                return (current == nullptr ? &engine : current)->invoke_callback(
                    address, std::span<const std::uint64_t>(arguments));
            });
        bridge.set_guest_window_callback_invoker(
            [&engine](std::uint64_t address, std::uint64_t window,
                      std::uint64_t message, std::uint64_t wparam,
                      std::uint64_t lparam) {
                const std::array arguments{window, message, wparam, lparam};
                auto* current = transmute::ReferenceEngine::current();
                return (current == nullptr ? &engine : current)->invoke_callback(
                    address, std::span<const std::uint64_t>(arguments));
            });
        bridge.set_guest_binary_callback_invoker(
            [&engine](std::uint64_t address, std::uint64_t left,
                      std::uint64_t right) {
                const std::array arguments{left, right};
                auto* current = transmute::ReferenceEngine::current();
                return (current == nullptr ? &engine : current)->invoke_callback(
                    address, std::span<const std::uint64_t>(arguments));
            });
        bridge.register_guest_module(process.executable());
        engine.set_native_import_resolver(transmute::resolve_direct_win32_import);
        for (const auto* module : process.attachment_order()) {
            bridge.register_guest_module(*module);
        }
        bridge.initialize_thread_environment(engine, process.executable().base());
#else
        transmute::ReferenceEngine engine(process.executable());
#endif
        if (jit) engine.set_jit_enabled(true);
        if (jit_stats) engine.set_jit_profiling(true);
        if (instruction_limit) engine.set_instruction_limit(*instruction_limit);
        for (std::size_t index = 1; index < process.modules().size(); ++index) {
            engine.add_module(*process.modules()[index]);
        }
#ifdef _WIN32
        bridge.initialize_static_tls(engine);
#endif
#ifdef _WIN32
        bridge.initialize_guest_dlls(engine, process.attachment_order());
#else
        engine.initialize_dlls(process.attachment_order());
#endif
        const auto report_jit_stats = [&] {
            if (!jit_stats) return;
            const auto stats = engine.jit_stats();
            std::cerr << "JIT main-thread blocks: compiled " << stats.compiled_blocks
                      << ", live " << stats.live_blocks
                      << ", executed " << stats.native_blocks
                      << ", fast hits " << stats.fast_hits
                      << ", native guest instructions "
                      << stats.native_guest_instructions
                      << ", compiled native regions " << stats.compiled_native_regions
                      << ", native region instructions " << stats.native_region_instructions
                      << ", native region alias hits " << stats.native_region_alias_hits
                      << ", native chain edges " << stats.native_chain_edges
                      << ", compiled register spans " << stats.compiled_register_spans
                      << ", inherited native regions " << stats.inherited_native_regions
                      << ", inherited code bytes " << stats.inherited_code_bytes
                      << ", inherited chunk bytes " << stats.inherited_chunk_bytes
                      << ", native read misses " << stats.native_read_misses
                      << ", native store misses " << stats.native_store_misses
                      << ", native journal exits " << stats.native_journal_exits
                      << ", native serialized access exits " << stats.native_serialized_access_exits
                      << ", front cache hits " << stats.front_cache_hits
                      << ", map cache hits " << stats.map_cache_hits
                      << ", arena reclamations " << stats.arena_reclamations
                      << ", interpreter fallback instructions "
                      << stats.fallback_instructions
                      << ", negative fallback entries "
                      << stats.negative_cache_entries
                      << ", negative fallback hits "
                      << stats.negative_cache_hits
                      << ", fallback profile observations "
                      << stats.fallback_profile_observations
                      << ", emitted code bytes " << stats.emitted_code_bytes
                      << ", cumulative code allocation bytes "
                      << stats.allocated_code_bytes
                      << ", live code bytes " << stats.live_code_bytes
                      << ", compilation seconds " << stats.compilation_seconds
                      << ", decoding seconds " << stats.decoding_seconds
                      << '\n';
            const auto report_refills = [](std::string_view access,
                                            const transmute::JitSpanRefillStats& refills) {
                constexpr std::array<std::string_view,
                    static_cast<std::size_t>(transmute::NativeSpanMissKind::count)>
                    causes{"empty", "stale", "collision", "range", "covered"};
                constexpr std::array<std::string_view,
                    static_cast<std::size_t>(transmute::NativeSpanRefillRejection::count)>
                    rejections{"none", "unsupported width", "unmapped", "permissions",
                               "external", "boundary", "image", "executable"};
                std::cerr << "  native " << access << " refills: successful "
                          << refills.successful << ", rejected " << refills.rejected
                          << "; causes";
                for (std::size_t index = 0; index < causes.size(); ++index)
                    std::cerr << ' ' << causes[index] << '=' << refills.causes[index];
                std::cerr << "; rejections";
                for (std::size_t index = 1; index < rejections.size(); ++index)
                    std::cerr << ' ' << rejections[index] << '=' << refills.rejections[index];
                std::cerr << '\n';
            };
            report_refills("read", stats.native_read_refills);
            report_refills("store", stats.native_store_refills);
            const auto topology = engine.native_topology_stats();
            std::cerr << "  main-thread native topology: selective refreshes "
                      << topology.selective_refreshes
                      << ", replayed changes " << topology.replayed_changes
                      << ", retained read entries " << topology.retained_read_entries
                      << ", retained store entries " << topology.retained_write_entries
                      << ", evicted read entries " << topology.evicted_read_entries
                      << ", evicted store entries " << topology.evicted_write_entries
                      << ", full flushes " << topology.full_flushes << '\n';
            const auto concurrency = engine.native_concurrency_stats();
            std::cerr << "  process native shared leases: "
                      << concurrency.shared_read_leases << ", peak overlapping "
                      << concurrency.peak_shared_read_leases << ", profiled worker caches "
                      << concurrency.profiled_worker_caches << ", inherited worker native regions "
                      << concurrency.inherited_worker_native_regions << '\n';
            for (const auto& block : stats.hottest_blocks) {
                std::cerr << "  guest PC 0x" << std::hex << block.guest_pc
                          << std::dec << ": " << block.executions
                          << " executions, " << block.instructions
                          << " guest instructions/block\n";
            }
            for (const auto& fallback : stats.hottest_fallbacks) {
                std::cerr << "  fallback PC 0x" << std::hex << fallback.guest_pc
                          << " opcode 0x" << fallback.opcode << std::dec
                          << ": " << fallback.samples
                          << " sampled interpreter instructions\n";
            }
        };
        transmute::RunResult result{};
#ifdef _WIN32
        try {
            result = engine.run();
        } catch (...) {
            if (sync_diagnostics) sync_diagnostics->stop();
            if (diagnose_exceptions) {
                const auto& fault = engine.state();
                std::cerr << "guest fault registers: PC=0x" << std::hex << fault.pc
                          << " LR=0x" << fault.x[30] << " SP=0x" << fault.sp
                          << " X0=0x" << fault.x[0] << " X1=0x" << fault.x[1]
                          << " X8=0x" << fault.x[8] << std::dec << '\n';
            }
            report_jit_stats();
            bridge.check_worker_failures();
            throw;
        }
        if (sync_diagnostics) sync_diagnostics->stop();
        report_jit_stats();
        bridge.check_worker_failures();
        if (diagnose_exceptions) {
            std::cerr << "guest main returned exit " << result.exit_code
                      << " after " << result.instructions << " instructions\n";
        }
#else
        result = engine.run();
        report_jit_stats();
#endif
#ifdef _WIN32
        bridge.cleanup_process_exit();
        bridge.cleanup_fls();
#endif
        return static_cast<int>(result.exit_code);
    } catch (const std::exception& error) {
        std::cerr << "transmute: " << error.what() << '\n';
        return 1;
    }
}
