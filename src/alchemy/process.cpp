#include "alchemy/process.hpp"
#include "alchemy/alchemy_root.hpp"
#include "alchemy/bundle.hpp"
#include "alchemy/darwin_providers.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/darwin_process.hpp"
#include "alchemy/darwin_dispatch.hpp"
#include "alchemy/darwin_mach_vm.hpp"
#include "alchemy/darwin_native.hpp"
#include "alchemy/darwin_numeric.hpp"
#include "alchemy/darwin_threads.hpp"
#include "alchemy/module_loader.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/path.hpp"
#include "alchemy/perf.hpp"
#include "alchemy/process_context.hpp"
#include "alchemy/host/debug.hpp"
#include "alchemy/host/files.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/registration_audit.hpp"

#include "alchemy/engine_adapter.hpp"
#ifdef ALCHEMY_WITH_GUI
#include "alchemy/gui/install.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <cstdio>

namespace alchemy {
namespace {

// "0x<address> <image>+0x<offset>" for diagnostics.
std::string describe_guest_address(std::span<const ImageRuntimeInfo> images, std::uint64_t address) {
    std::ostringstream text;
    text << "0x" << std::hex << address;
    for (const auto& module : images)
        if (address >= module.begin && address < module.end)
            text << " " << std::filesystem::path(module.name).filename().string() << "+0x" << (address - module.begin);
    return text.str();
}

// Frame-pointer backtrace (AAPCS64 frame records {fp, lr}), as module+offset.
void append_guest_backtrace(std::ostream& out, transmute::GuestMemory& memory,
                            std::span<const ImageRuntimeInfo> images, std::uint64_t lr, std::uint64_t frame) {
    out << "\n  lr " << describe_guest_address(images, lr);
    for (int depth = 0; depth < 48 && frame && (frame & 7) == 0; ++depth) {
        std::uint64_t next = 0, ret = 0;
        try { next = memory.read64(frame); ret = memory.read64(frame + 8); } catch (...) { break; }
        if (!ret) break;
        out << "\n  #" << std::dec << depth << ' ' << describe_guest_address(images, ret);
        if (next <= frame) break;
        frame = next;
    }
}

// ALCHEMY_HANG_DUMP=<host path>: every process of the tree watches that file
// (once a second) and, each time it is created or touched, prints every guest
// thread to stderr: the provider call it is in, its first arguments and its
// guest backtrace. A harness touches it when the guest stops answering, so a
// hang reports where each process waits (web_visit.ps1 does).
class HangDump {
public:
    HangDump(ProcessContext& context, transmute::GuestMemory& memory) : context_(context), memory_(memory) {
        const auto path = host::process::environment("ALCHEMY_HANG_DUMP");
        if (!path || path->empty()) return;
        trigger_ = utf8_path(*path);
        last_ = stamp();
        thread_ = std::thread([this] { watch(); });
    }
    HangDump(const HangDump&) = delete;
    HangDump& operator=(const HangDump&) = delete;
    ~HangDump() {
        if (!thread_.joinable()) return;
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }

private:
    [[nodiscard]] std::filesystem::file_time_type stamp() const {
        std::error_code error;
        const auto time = std::filesystem::last_write_time(trigger_, error);
        return error ? std::filesystem::file_time_type::min() : time;
    }
    void watch() {
        std::unique_lock lock(mutex_);
        while (!wake_.wait_for(lock, std::chrono::seconds(1), [this] { return stop_; })) {
            const auto now = stamp();
            if (now == last_ || now == std::filesystem::file_time_type::min()) continue;
            last_ = now;
            try { dump(); } catch (...) {}
        }
    }
    void dump() {
        const auto threads = context_.thread_snapshots();
        const auto images = context_.images();
        std::ostringstream out;
        // The process's role: Chrome's --type=..., else the program's name.
        std::string role;
        for (const auto& argument : context_.arguments)
            if (argument.rfind("--type=", 0) == 0) { role = " " + argument; break; }
        if (role.empty() && !context_.arguments.empty())
            role = " " + std::filesystem::path(context_.arguments.front()).filename().string();
        out << "alchemy: hang dump (pid " << host::process::current_pid() << ")" << role << ": " << threads.size()
            << " guest thread(s)";
        for (const auto& thread : threads) {
            const auto& s = thread.state;
            const auto name = context_.export_name(s.pc);
            out << "\n thread 0x" << std::hex << thread.identity;
            if (thread.host_thread) out << " (host " << std::dec << thread.host_thread << std::hex << ")";
            out << ": "
                << (name.empty() ? "pc " + describe_guest_address(images, s.pc) : "in " + name)
                << " x0 0x" << s.x[0] << " x1 0x" << s.x[1] << " x2 0x" << s.x[2] << " x3 0x" << s.x[3];
            // mach_msg: the ports of its header (remote +8, local +12, id +20) and
            // which half it does (MACH_SEND_MSG 1, MACH_RCV_MSG 2).
            if (name.rfind("_mach_msg ", 0) == 0 && s.x[0]) {
                try {
                    out << ((s.x[1] & 1) ? " send" : "") << ((s.x[1] & 2) ? " receive" : "") << " remote 0x"
                        << memory_.read32(s.x[0] + 8) << " local 0x" << memory_.read32(s.x[0] + 12) << " id 0x"
                        << memory_.read32(s.x[0] + 20);
                } catch (...) {}
            }
            try { append_guest_backtrace(out, memory_, images, s.x[30], s.x[29]); } catch (...) {}
            out << std::dec;
        }
        out << '\n' << context_.hang_dump_sections();
        // One write, so the dumps of several processes sharing stderr do not interleave.
        const auto text = out.str();
        static_cast<void>(host::files::write(host::process::standard_handle(host::process::Standard::error),
                                             std::as_bytes(std::span(text.data(), text.size()))));
        // The host stacks (where a provider call waits) go to hang-dump-<pid>.txt
        // next to the trigger, after the guest dump: walking them suspends each
        // thread in turn. Offsets symbolize with the build's pdb (llvm-symbolizer).
        std::ofstream file(trigger_.parent_path() / ("hang-dump-" + std::to_string(host::process::current_pid()) + ".txt"),
                           std::ios::app);
        file << text;
        for (const auto& stack : host::debug::other_thread_stacks()) {
            file << " host thread " << stack.thread << " (cpu " << stack.cpu_ms << " ms):\n";
            for (std::size_t depth = 0; depth < stack.frames.size(); ++depth)
                file << "  host #" << depth << ' ' << host::debug::describe_address(stack.frames[depth]) << '\n';
        }
    }

    ProcessContext& context_;
    transmute::GuestMemory& memory_;
    std::filesystem::path trigger_;
    std::filesystem::file_time_type last_{};
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_{};
    std::thread thread_;
};

// ALCHEMY_GUI_NO_CRASH_DIALOG=1: a host crash (a fault in alchemy-fx, a GPU
// driver or a system DLL that nothing handles) ends the process with a report
// on stderr instead of the host's crash dialog (Windows Error Reporting), so
// unattended GUI runs never block on it. Process-local only
// (host::debug::on_fatal_fault); system-wide settings are untouched.
struct CrashReport {
    ProcessContext* context{};
    std::atomic<bool> reported{};
    bool selftest{};   // ends with status 3 (ctest treats exception codes as crashes)
};
CrashReport g_crash;

unsigned long long host_thread() { return static_cast<unsigned long long>(host::process::current_thread_id()); }

void report_host_crash(const host::debug::Fault& fault) {
    if (g_crash.reported.exchange(true)) return;
    std::fprintf(stderr, "alchemy: host crash: exception 0x%08lx at %s, thread %llu", static_cast<unsigned long>(fault.code),
                 host::debug::describe_address(fault.address).c_str(), host_thread());
    if (fault.access != host::debug::Access::none)
        std::fprintf(stderr, " (%s 0x%llx)", fault.access == host::debug::Access::write ? "write" : fault.access == host::debug::Access::execute ? "execute" : "read",
                     static_cast<unsigned long long>(fault.data_address));
    std::fputc('\n', stderr);
    // Host stack, unwound from the faulting context, as module+offset.
    for (std::size_t depth = 0; depth < fault.stack.size(); ++depth)
        std::fprintf(stderr, "  host #%d %s\n", static_cast<int>(depth), host::debug::describe_address(fault.stack[depth]).c_str());
    // The guest thread this host thread runs, if any.
    if constexpr (requires { transmute::ReferenceEngine::current(); }) {
        if (auto* engine = transmute::ReferenceEngine::current()) {
            const auto& state = engine->state();
            std::ostringstream guest;
            guest << "  guest pc 0x" << std::hex << state.pc << " lr 0x" << state.x[30] << " fp 0x" << state.x[29];
            if (g_crash.context) {
                try { append_guest_backtrace(guest, *engine, g_crash.context->images(), state.x[30], state.x[29]); } catch (...) {}
            }
            std::fprintf(stderr, "%s\n", guest.str().c_str());
        } else {
            std::fprintf(stderr, "  (not a guest thread)\n");
        }
    }
    std::fflush(stderr);
    host::process::terminate_self(g_crash.selftest ? 3U : fault.code);
}

// abort() and CRT invalid-parameter reports end in a fast-fail the fault hook
// never sees: report them with the host stack first.
void report_fast_fail(const char* what) {
    std::fprintf(stderr, "alchemy: host %s on thread %llu\n", what, host_thread());
    std::uintptr_t frames[48]{};
    const auto count = host::debug::capture_stack(frames, 1);
    for (std::size_t index = 0; index < count; ++index)
        std::fprintf(stderr, "  host #%u %s\n", static_cast<unsigned>(index), host::debug::describe_address(frames[index]).c_str());
    std::fflush(stderr);
}

void install_crash_report(ProcessContext& context) {
    if (host::process::environment("ALCHEMY_GUI_NO_CRASH_DIALOG").value_or(std::string()) != "1") return;
    g_crash.context = &context;
    host::debug::on_fatal_fault(report_host_crash);
    std::signal(SIGABRT, [](int) { report_fast_fail("abort()"); host::process::terminate_self(3); });
#ifdef _MSC_VER
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
        report_fast_fail("CRT invalid parameter");
        host::process::terminate_self(3);
    });
#endif
    // std::terminate (an exception leaving a thread or a noexcept function)
    // ends in a fast-fail the fault hook never sees: report the exception first.
    std::set_terminate([] {
        std::string what = "no active exception";
        if (const auto current = std::current_exception()) {
            try { std::rethrow_exception(current); }
            catch (const std::exception& error) { what = error.what(); }
            catch (...) { what = "a non-standard exception"; }
        }
        std::fprintf(stderr, "alchemy: host std::terminate on thread %llu: %s\n", host_thread(), what.c_str());
        if constexpr (requires { transmute::ReferenceEngine::current(); }) {
            if (auto* engine = transmute::ReferenceEngine::current()) {
                const auto& state = engine->state();
                std::fprintf(stderr, "  guest pc 0x%llx lr 0x%llx\n", static_cast<unsigned long long>(state.pc),
                             static_cast<unsigned long long>(state.x[30]));
            }
        }
        std::fflush(stderr);
        host::process::terminate_self(3);
    });
    // Regression hook (ctest alchemy_fx_crash_report): a host null write.
    if (host::process::environment("ALCHEMY_GUI_CRASH_SELFTEST").value_or(std::string()) == "1") {
        g_crash.selftest = true;
        volatile int* null_pointer = nullptr;
        *null_pointer = 1;
    }
}


// Joins guest workers after a process stop. Darwin ends every thread at exit;
// a worker blocked in a host call that never observes the stop would keep the
// host process alive, so after a grace period the engine ends it with `status`.
template <class Engine>
void join_stopped_workers(DarwinThreads& threads, Engine& engine, std::uint32_t status) {
    if constexpr (engine_can_stop<Engine>) {
        if (engine_stop_requested(engine)) {
            std::mutex gate;
            std::condition_variable changed;
            bool joined = false;
            std::thread watchdog([&] {
                std::unique_lock lock(gate);
                if (!changed.wait_for(lock, std::chrono::seconds(5), [&] { return joined; }))
                    force_engine_exit(engine, status,
                        "alchemy: guest threads did not stop within 5 s of process exit (blocked in host calls); "
                        "ending the process");
            });
            struct Release {
                std::mutex& gate; std::condition_variable& changed; bool& joined; std::thread& watchdog;
                ~Release() {
                    { std::lock_guard lock(gate); joined = true; }
                    changed.notify_all();
                    watchdog.join();
                }
            } release{gate, changed, joined, watchdog};
            threads.join_workers();
            return;
        }
    }
    threads.join_workers();
}

// Probe-only classification of an unresolved import as data rather than code.
bool probe_data_symbol(std::string_view name) {
    if (name.starts_with("_OBJC_") || name.starts_with("__NSConcrete") || name.starts_with("___kCF") ||
        name == "_NSApp" || name == "_sys_nerr" || name == "__dispatch_main_q")
        return true;
    if (name.size() > 2 && name[0] == '_' && name[1] == 'k' && name[2] >= 'A' && name[2] <= 'Z') return true;
    for (const std::string_view suffix : {"Key", "Notification", "Mode", "Type", "Name", "Space", "Point",
                                          "Rect", "Dark", "Light", "Items", "Profiles", "ProfileID", "Class"})
        if (name.ends_with(suffix) && name.starts_with("_NS")) return true;
    return false;
}

} // namespace

std::optional<std::string> child_preload_setting(bool child, const std::optional<std::string>& current) {
    if (!child || (current && !current->empty())) return std::nullopt;
    return std::string("0");
}

ProcessResult run_program(const std::filesystem::path& executable,
                          std::span<const std::string> arguments,
                          const RunOptions& options, OutputWriter writer,
                          std::span<const std::string> environment) {
    const auto source = discover_program(executable);
    perf::phase("run_program");
    auto image = MachOImage::load(source);
    perf::phase("main executable read and parsed");
#ifdef ALCHEMY_WITH_GUI
    // Vulkan device creation overlaps loading (gui/install.hpp). Not for
    // background/command-line runs (Blender -b, --version), which may never
    // ask for a device.
    const bool background = std::any_of(arguments.begin(), arguments.end(), [](const std::string& argument) {
        return argument == "-b" || argument == "--background" || argument == "-v" || argument == "--version" ||
               argument == "-h" || argument == "--help";
    });
    const bool links_metal = std::any_of(image.dependencies().begin(), image.dependencies().end(), [](const auto& dependency) {
        return dependency.install_name.find("/Metal.framework/") != std::string::npos;
    });
    // Joins the prefetch threads when run_program returns (declared first:
    // released last).
    std::shared_ptr<void> metal_prefetch;
#endif
    if (image.is_dylib() || image.is_bundle()) throw std::runtime_error("cannot launch a dylib or bundle as an executable");
    // A position-independent executable slides, as on Darwin, when host memory
    // placed before main sits in its preferred range (engine_slide_main_image).
    auto load_base = options.load_base;
    if (!load_base && image.position_independent())
        if (const auto slid = engine_slide_main_image<transmute::ReferenceEngine>(image.preferred_base(), image.image_size())) {
            std::cerr << "alchemy: the executable's range at 0x" << std::hex << image.preferred_base()
                      << " is taken by host memory; it loads at 0x" << *slid << std::dec << std::endl;
            load_base = slid;
        }
    const auto base = load_base.value_or(image.preferred_base());
    // Read when the first engine starts the fx runtime.
    if (const auto preload = child_preload_setting(options.child != nullptr, host::process::environment("TRANSMUTE_FX_PRELOAD")))
        static_cast<void>(host::process::set_environment("TRANSMUTE_FX_PRELOAD", *preload));
    transmute::ReferenceEngine engine(transmute::ExecutionStart{image.entry(base), image.stack_size()});
    // Destroyed after the workers are joined: persist translations at the end.
    struct FlushTranslations {
        transmute::ReferenceEngine& engine;
        ~FlushTranslations() {
            try { flush_engine_translations(engine); } catch (...) {}
        }
    } flush_translations{engine};
    engine.set_instruction_limit(options.instruction_limit);
    engine.set_jit_enabled(options.jit);
    engine.set_jit_profiling(options.jit_stats);
    DarwinRuntime runtime(engine, std::move(writer));
    ProcessContext context(engine, runtime.output_writer());
    perf::phase("engine created");
    install_crash_report(context);
    context.executable = source;
    context.working_directory = std::filesystem::current_path();
    context.arguments = {guest_path_string(source)};
    context.arguments.insert(context.arguments.end(), arguments.begin(), arguments.end());
    context.environment.assign(environment.begin(), environment.end());
    context.launch_options = options.launch_options;
    context.host_descriptors = options.descriptors;
    if (options.child) {
        // An Alchemy child: exec's argv (argv[0] as given), envp and directory.
        context.child_launch = options.child;
        if (!options.child->argv.empty()) context.arguments = options.child->argv;
        context.environment = options.child->envp;
        if (!options.child->cwd.empty()) context.working_directory = options.child->cwd;
    }
    context.instruction_limit = options.instruction_limit;
    context.jit = options.jit;
    context.jit_stats = options.jit_stats;
    context.base_resolver = [&](std::string_view library, std::string_view symbol) {
        return runtime.resolve(library, symbol);
    };
    DarwinProviders providers(context);
    HangDump hang_dump(context, engine);   // ALCHEMY_HANG_DUMP (destroyed before the providers)
    auto& services = providers.services;
    auto& threads = providers.threads;
    auto& objc = providers.objc;
    perf::phase("providers installed");
    const auto probe = ProcessContext::probe_mode();
    std::mutex probe_mutex;
    std::set<std::string> probe_reported;
    const auto probe_note = [&](const std::string& message) {
        std::lock_guard lock(probe_mutex);
        if (probe_reported.insert(message).second) std::cerr << "[alchemy probe] " << message << std::endl;
    };
    context.base_resolver = [&](std::string_view library, std::string_view symbol) -> std::optional<std::uint64_t> {
        if (auto found = runtime.resolve(library, symbol)) return found;
        if (probe == ProcessContext::ProbeMode::off || ProcessContext::in_runtime_lookup()) return std::nullopt;
        // Alchemy's own synthetic libraries ("alchemy.internal", ...) are
        // looked up by providers that install the function when it is absent
        // (the completion thread entry, Qt trampolines): never a probe stub.
        if (library.starts_with("alchemy.")) return std::nullopt;
        const std::string name(symbol), owner(library);
        for (const std::string_view prefix : {"_OBJC_CLASS_$_", "_OBJC_METACLASS_$_"}) {
            if (!name.starts_with(prefix)) continue;
            const auto class_name = name.substr(prefix.size());
            auto cls = objc.find_class(class_name);
            if (!cls) {
                probe_note("class " + owner + " " + class_name);
                cls = objc.register_native_class(class_name, objc.find_class("NSObject"), 8);
            }
            return prefix == "_OBJC_CLASS_$_" ? cls : engine.read64(cls);
        }
        if (probe_data_symbol(name)) {
            probe_note("data " + owner + " " + name);
            return context.allocate_guest(4096);
        }
        const auto stub = context.install_function(name, [&probe_note, probe, owner, name](transmute::CpuState& state, transmute::GuestMemory&) {
            if (probe == ProcessContext::ProbeMode::trap)
                throw std::runtime_error("Alchemy probe: called missing " + owner + " " + name);
            probe_note("zero-return " + owner + " " + name);
            state.x[0] = 0;
            state.v[0] = {0, 0};
        }, owner);
        // The stub's address, so a symbol the guest uses as data (not
        // classified as such above) can be recognised in later failures.
        std::ostringstream where;
        where << "function " << owner << " " << name << " at 0x" << std::hex << stub;
        probe_note(where.str());
        return stub;
    };
    ModuleLoader loader(context, source, load_base, options.dylib_directories, options.runtime_directories);
    engine.set_supervisor_call_handler([&](const transmute::SupervisorCall& call,
                                          transmute::CpuState& state,
                                          transmute::GuestMemory& memory) -> std::optional<std::uint32_t> {
        if (call.immediate == 0xa1) {
            if (!context.syscall_extension) throw std::runtime_error("missing Alchemy thread-exit handler");
            return context.syscall_extension(call, state, memory);
        }
        const auto result = call.immediate == 0x80 && services.handles_syscall(state.x[16])
            ? services.supervisor_call(call, state, memory) : runtime.supervisor_call(call, state, memory);
        if (result) {
            // Darwin exit: the process ends, so every other guest thread stops.
            context.request_exit(*result);
            request_engine_stop(engine, *result);
        }
        return result;
    });
    // A fatal signal raised on any guest thread records the process status.
    set_engine_stop_observer(engine, [&context](std::uint32_t status) { context.request_exit(status); });
    const auto report_termination = [&](const EngineTermination& termination) {
        std::ostringstream report;
        report << "alchemy: guest terminated by " << GuestSignalTermination::name(termination.signal)
               << " (signal " << termination.signal << ", status " << termination.status << "): "
               << termination.reason << (termination.main_thread ? " [main thread]" : " [worker thread]");
        const auto images = context.images();
        report << "\n  pc " << describe_guest_address(images, termination.pc);
        append_guest_backtrace(report, engine, images, termination.lr, termination.fp);
        std::cerr << report.str() << std::endl;
    };
#ifdef ALCHEMY_WITH_GUI
    // The driver allocates host memory: only once the main image holds its
    // fixed range (dependencies take any free range and retry on a race).
    if (!background && links_metal)
        context.main_image_mapped = [&metal_prefetch] { metal_prefetch = gui::prefetch_metal_device(); };
#endif
    perf::phase("load main image and dependencies: start");
    static_cast<void>(loader.load_main(std::move(image)));   // parsed once, above
    perf::phase("load main image and dependencies: done (mapped, bound, fixed up, ObjC prepared)");
    // The environment as the guest sees it (DarwinServices normalized PATH and temporary paths).
    runtime.initialize_startup(context.arguments, context.environment, guest_path_string(source));
    const std::array<std::uint64_t, 4> startup{
        engine.state().x[0], engine.state().x[1], engine.state().x[2], engine.state().x[3]};
    context.startup_argc = startup[0];
    context.startup_argv = startup[1];
    // Diagnostic: ALCHEMY_DEBUG_START_DELAY=<ms>[,<text>] holds the process for <ms> before its
    // initializers and main (only processes with an argument containing <text>): a slow start
    // (a cold translation cache, a loaded machine) on demand, at any machine load.
    if (const auto delay = host::process::environment("ALCHEMY_DEBUG_START_DELAY"); delay && !delay->empty()) {
        const auto comma = delay->find(',');
        const auto filter = comma == std::string::npos ? std::string() : delay->substr(comma + 1);
        const bool selected = filter.empty() || std::any_of(context.arguments.begin(), context.arguments.end(),
                                                            [&](const std::string& argument) { return argument.find(filter) != std::string::npos; });
        if (selected) std::this_thread::sleep_for(std::chrono::milliseconds(std::strtoull(delay->c_str(), nullptr, 10)));
    }
    try {
        loader.initialize(startup);
        perf::phase("initializers done: guest main starts");
        std::uint32_t exit = 0;
        std::uint64_t instructions = 0;
        if (const auto requested = context.requested_exit()) exit = *requested;
        else {
            const auto result = engine.run();
            exit = context.requested_exit().value_or(result.exit_code);
            instructions = result.instructions;
            if constexpr (engine_can_stop<transmute::ReferenceEngine>) {
                // Returning from main is exit(status): atexit handlers run while
                // other threads still run, then every thread ends (pthread_exit
                // on the main thread instead waits for the last thread).
                if (!context.requested_exit() && !context.thread(engine)->exit_value) {
                    threads.cleanup_thread(*context.thread(engine));
                    context.run_finalizers();
                    objc.cleanup_thread(*context.thread(engine));
                    context.request_exit(exit);
                    request_engine_stop(engine, exit);
                }
            }
        }
        join_stopped_workers(threads, engine, context.requested_exit().value_or(exit));
        if (!context.requested_exit()) {
            threads.cleanup_thread(*context.thread(engine));
            context.run_finalizers();
            objc.cleanup_thread(*context.thread(engine));
        }
        auto stats = engine.jit_stats();
        const auto worker_stats = context.worker_execution();
        stats.native_guest_instructions += worker_stats.native_guest_instructions;
        stats.native_blocks += worker_stats.native_blocks;
        stats.fallback_instructions += worker_stats.fallback_instructions;
        // A fatal signal on a worker stopped this thread too.
        if (const auto termination = engine_termination(engine)) {
            report_termination(*termination);
            return {termination->status, instructions + worker_stats.instructions, std::move(stats), termination->signal};
        }
        return {context.requested_exit().value_or(exit), instructions + worker_stats.instructions, std::move(stats)};
    } catch (const std::exception& error) {
        // A fatal Darwin signal: raised on this thread, or recorded by the
        // engine on any thread (a worker's arrives here through pthread_join).
        auto termination = engine_termination(engine);
        if (const auto* signal = dynamic_cast<const GuestSignalTermination*>(&error); signal && !termination)
            termination = EngineTermination{signal->status(), signal->signal(), signal->what(), engine.state().pc,
                                            engine.state().x[30], engine.state().x[29], true};
        if (termination) {
            context.request_exit(termination->status);
            request_engine_stop(engine, termination->status);
            try { join_stopped_workers(threads, engine, termination->status); } catch (const std::exception&) {}
            report_termination(*termination);
            return {termination->status, 0, engine.jit_stats(), termination->signal};
        }
        std::ostringstream diagnostic;
        diagnostic << error.what() << " [guest pc 0x" << std::hex << engine.state().pc;
        for (const auto& module : context.images()) {
            if (engine.state().pc >= module.begin && engine.state().pc < module.end) {
                diagnostic << ", module " << module.name;
                break;
            }
        }
        diagnostic << ", thread " << std::dec << context.thread(engine)->identity << ']';
        if (probe != ProcessContext::ProbeMode::off) {
            // Probe-only frame-pointer backtrace, as module+offset.
            append_guest_backtrace(diagnostic, engine, context.images(), engine.state().x[30], engine.state().x[29]);
        }
        // Report before stopping: a worker that fails while the process
        // stops (an exception through a host library's callback frames) can
        // end the process before the caller prints this.
        std::cerr << "alchemy: stopping after: " << diagnostic.str() << std::endl;
        context.request_exit(1);
        request_engine_stop(engine, 1);
        // Keep the loader and every runtime service alive until workers stop.
        try { join_stopped_workers(threads, engine, 1); }
        catch (const std::exception& worker_error) { diagnostic << "; worker shutdown: " << worker_error.what(); }
        throw std::runtime_error(diagnostic.str());
    }
}

} // namespace alchemy
