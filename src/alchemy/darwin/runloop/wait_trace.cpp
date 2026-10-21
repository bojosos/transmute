#include "alchemy/darwin/kit/support/support_kit.hpp"

#include "alchemy/gui/runloop.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/ui.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <thread>
#include <typeinfo>

// ALCHEMY_WAIT_DUMP=<seconds>: a bring-up diagnostic for hangs. The
// blocking waits of libSystem (pthread_cond_wait, pthread_cond_timedwait,
// dispatch_semaphore_wait, semaphore_wait, __ulock_wait, pthread_join) are
// wrapped; each wait in progress is recorded with its caller's frame-pointer
// backtrace (module+offset). After <seconds> and then every 30 s a host
// thread prints every wait that is still in progress to stderr as
//   [alchemy support] waiting: <function> since <n> s (host thread <id>)
//     #k <module>+0x<offset>
// The original functions keep their behaviour.
namespace alchemy::gui::support_kit {
namespace {

struct Wait {
    std::string function;
    std::chrono::steady_clock::time_point since;
    std::vector<std::uint64_t> frames;
};
struct Last {
    std::string function;
    std::chrono::steady_clock::time_point since;
    std::vector<std::uint64_t> frames;
    bool inside{};
};
struct Table {
    std::mutex mutex;
    std::map<std::uint64_t, Last> last;   // host thread id -> its latest sampled libSystem call
    std::map<std::uint64_t, Wait> waits;   // host thread id -> its innermost wait
    std::map<std::pair<std::string, std::uint64_t>, std::uint64_t> calls;   // (function, caller's caller) -> count
};
Table& table() { static Table value; return value; }

std::string describe(Support& support, std::uint64_t address) {
    for (const auto& image : support.process.images())
        if (address >= image.begin && address < image.end) {
            auto name = image.name;
            if (const auto slash = name.find_last_of("/\\"); slash != std::string::npos) name = name.substr(slash + 1);
            char text[256];
            std::snprintf(text, sizeof text, "%s+0x%llx", name.c_str(), static_cast<unsigned long long>(address - image.begin));
            return text;
        }
    char text[32];
    std::snprintf(text, sizeof text, "0x%llx", static_cast<unsigned long long>(address));
    return text;
}

// A host thread's state and host stack (module+offset per frame, unwound
// with the host unwind tables until a frame without them, i.e. translated
// guest code): which host function an idle-looking guest thread is in.
std::string host_stack(std::uint64_t id) {
    const auto frames = alchemy::host::ui::sample_thread_stack(id, 24);
    if (!frames) return frames.error() == alchemy::host::Error::shut_down ? " [host thread exited]" : " [host thread gone]";
    std::string text = " [host stack:";
    for (const auto& frame : *frames) {
        char item[300];
        if (!frame.module.empty())
            std::snprintf(item, sizeof item, " %s+0x%llx", frame.module.c_str(), static_cast<unsigned long long>(frame.offset));
        else
            std::snprintf(item, sizeof item, " 0x%llx", static_cast<unsigned long long>(frame.address));
        text += item;
    }
    return text + "]";
}

} // namespace

void install_wait_trace(Support& support) {
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, "ALCHEMY_WAIT_DUMP") != 0 || value == nullptr) return;
    const auto seconds = std::max(1L, std::strtol(value, nullptr, 10));
    std::free(value);
    auto* s = &support;
    for (const auto* name : {"_pthread_cond_wait", "_pthread_cond_timedwait", "_dispatch_semaphore_wait", "_semaphore_wait",
                             "___ulock_wait", "_pthread_join", "_dispatch_group_wait", "_usleep", "_nanosleep", "_sched_yield",
                             "_select", "_poll", "_kevent", "_mach_wait_until", "_read", "_recv", "_recvfrom", "_accept",
                             "_sem_wait", "_mach_msg", "_waitpid", "_sigwait", "_kevent64", "_pselect", "_dispatch_sync", "_dispatch_sync_f",
                             "_dispatch_barrier_sync", "_dispatch_barrier_sync_f", "_dispatch_once", "_dispatch_once_f", "_pthread_once",
                             "_dispatch_main", "_sleep"}) {
        const auto original = support.process.resolve(libsystem_library, name);
        if (!original) continue;
        const auto target = original->address;
        const std::string function = name + 1;
        static_cast<void>(support.process.install_function(name, [s, target, function](CpuState& c, GuestMemory& m) {
            Wait wait{function, std::chrono::steady_clock::now(), {}};
            wait.frames.push_back(c.x[30]);
            for (std::uint64_t frame = c.x[29]; frame && (frame & 7) == 0 && wait.frames.size() < 24;) {
                std::uint64_t next{}, ret{};
                try { next = m.read64(frame); ret = m.read64(frame + 8); } catch (...) { break; }
                if (!ret) break;
                wait.frames.push_back(ret);
                if (next <= frame) break;
                frame = next;
            }
            const auto id = alchemy::host::process::current_thread_id();
            {
                std::lock_guard lock(table().mutex);
                ++table().calls[{function, wait.frames.size() > 1 ? wait.frames[1] : wait.frames[0]}];
                table().waits[id] = std::move(wait);
            }
            struct Done { std::uint64_t id; ~Done() { std::lock_guard lock(table().mutex); table().waits.erase(id); } } done{id};
            CallRegs forward;
            for (unsigned index = 0; index < 8; ++index) forward.x[index] = c.x[index];
            const auto result = EngineAdapter(s->process.calling_engine()).call_regs(target, forward);
            c.x[0] = result.x0;
        }, std::string(libsystem_library)));
    }
    // CFRunLoopSource perform callbacks are counted: the source's context is
    // rewritten so perform goes through a trampoline holding {info, perform, index}.
    struct Perform { std::uint64_t perform{}, count{}, active{}; std::chrono::steady_clock::time_point since{}; };
    static std::vector<Perform> performs;   // guarded by table().mutex
    static_cast<void>(support.process.install_function("__alchemy_support_perform_trampoline", [s](CpuState& c, GuestMemory& m) {
        const auto block = c.x[0];
        const auto info = m.read64(block), perform = m.read64(block + 8), index = m.read64(block + 16);
        {
            std::lock_guard lock(table().mutex);
            if (index < performs.size()) { ++performs[index].count; ++performs[index].active; performs[index].since = std::chrono::steady_clock::now(); }
        }
        struct Leave { std::uint64_t index; ~Leave() { std::lock_guard lock(table().mutex); if (index < performs.size()) --performs[index].active; } } leave{index};
        CallRegs call;
        call.x[0] = info;
        static_cast<void>(EngineAdapter(s->process.calling_engine()).call_regs(perform, call));
    }, "alchemy.internal"));
    if (const auto original = support.process.resolve("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "_CFRunLoopSourceCreate")) {
        const auto target = original->address;
        static_cast<void>(support.process.install_function("_CFRunLoopSourceCreate", [s, target](CpuState& c, GuestMemory& m) {
            CallRegs forward;
            for (unsigned index = 0; index < 8; ++index) forward.x[index] = c.x[index];
            const auto context = c.x[2];
            Address copy{};
            if (context && m.read64(context) == 0 && m.read64(context + 72)) {   // version 0 with perform
                copy = s->process.allocate_guest(80, 16);
                std::vector<std::uint8_t> bytes(80);
                m.read_bytes(context, bytes);
                m.write_bytes(copy, bytes);
                const auto block = s->process.allocate_guest(24, 16);
                std::uint64_t index{};
                {
                    std::lock_guard lock(table().mutex);
                    index = performs.size();
                    performs.push_back({m.read64(context + 72), 0});
                }
                m.write64(block, m.read64(context + 8));
                m.write64(block + 8, m.read64(context + 72));
                m.write64(block + 16, index);
                m.write64(copy + 8, block);
                for (const auto offset : {16, 24, 32, 40, 48}) m.write64(copy + offset, 0);   // retain/release/copyDescription/equal/hash
                m.write64(copy + 72, s->process.resolve("alchemy.internal", "__alchemy_support_perform_trampoline")->address);
                forward.x[2] = copy;
            }
            c.x[0] = EngineAdapter(s->process.calling_engine()).call_regs(target, forward).x0;
        }, "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation"));
    }
    // Main-thread call-site sampling: frequent libSystem calls made on the
    // main thread after <seconds>/2 are counted by their backtrace.
    static std::map<std::vector<std::uint64_t>, std::uint64_t> samples;   // guarded by table().mutex
    static const auto sample_from = std::chrono::steady_clock::now() + std::chrono::seconds(seconds / 2);
    // Every thread's latest sampled call is kept too (function, backtrace,
    // whether it is still inside it), so a thread stuck in a lock or spinning
    // shows up in the dump with its stack.
    for (const auto* name : {"_pthread_getspecific", "_mach_absolute_time", "_clock_gettime", "_gettimeofday", "_sched_yield",
                             "_pthread_mutex_lock", "_pthread_mutex_unlock", "_pthread_mutex_trylock", "_os_unfair_lock_lock",
                             "_pthread_rwlock_rdlock", "_pthread_rwlock_wrlock", "_malloc", "_free", "_memcpy", "_memmove", "_memset"}) {
        const auto original = support.process.resolve(libsystem_library, name);
        if (!original) continue;
        const auto target = original->address;
        const bool any_thread = std::string_view(name) == "_sched_yield";
        const std::string function = name + 1;
        static_cast<void>(support.process.install_function(name, [s, target, any_thread, function](CpuState& c, GuestMemory& m) {
            const bool sampling = std::chrono::steady_clock::now() > sample_from;
            const auto id = alchemy::host::process::current_thread_id();
            if (sampling) {
                std::vector<std::uint64_t> frames{c.x[30]};
                for (std::uint64_t frame = c.x[29]; frame && (frame & 7) == 0 && frames.size() < 24;) {
                    std::uint64_t next{}, ret{};
                    try { next = m.read64(frame); ret = m.read64(frame + 8); } catch (...) { break; }
                    if (!ret) break;
                    frames.push_back(ret);
                    if (next <= frame) break;
                    frame = next;
                }
                std::lock_guard lock(table().mutex);
                if (any_thread || s->on_main_thread()) ++samples[frames];
                table().last[id] = Last{function, std::chrono::steady_clock::now(), std::move(frames), true};
            }
            CallRegs forward;
            for (unsigned index = 0; index < 8; ++index) forward.x[index] = c.x[index];
            const auto result = EngineAdapter(s->process.calling_engine()).call_regs(target, forward);
            if (sampling) {
                std::lock_guard lock(table().mutex);
                if (const auto found = table().last.find(id); found != table().last.end()) found->second.inside = false;
            }
            c.x[0] = result.x0;
            c.x[1] = result.x1;
        }, std::string(libsystem_library)));
    }
    // ALCHEMY_WRITE_MARK=<text>: a write(2) whose bytes contain <text>
    // prints the writing host thread and its guest backtrace (which thread
    // logged a given line).
    char* mark_value = nullptr;
    std::size_t mark_length = 0;
    if (_dupenv_s(&mark_value, &mark_length, "ALCHEMY_WRITE_MARK") == 0 && mark_value) {
        const std::string mark = mark_value;
        std::free(mark_value);
        // (function, register holding the bytes, length: register index or -1 for a C string, element-size register or -1)
        struct Writer { const char* name; int bytes; int length; int size; };
        for (const auto writer : {Writer{"_write", 1, 2, -1}, Writer{"_fwrite", 0, 2, 1}, Writer{"_fputs", 0, -1, -1}, Writer{"_puts", 0, -1, -1}}) {
            const auto original = support.process.resolve(libsystem_library, writer.name);
            if (!original) continue;
            const auto target = original->address;
            static_cast<void>(support.process.install_function(writer.name, [s, target, mark, writer](CpuState& c, GuestMemory& m) {
                std::uint64_t length = 0;
                if (writer.length >= 0) length = c.x[writer.length] * (writer.size >= 0 ? c.x[writer.size] : 1);
                else
                    try { while (length < 4096 && m.read8(c.x[writer.bytes] + length)) ++length; } catch (...) { length = 0; }
                if (length && length < 65536) {
                    std::string bytes(length, '\0');
                    try { m.read_bytes(c.x[writer.bytes], std::span(reinterpret_cast<std::uint8_t*>(bytes.data()), bytes.size())); } catch (...) { bytes.clear(); }
                    if (bytes.find(mark) != std::string::npos) {
                        std::ostringstream out;
                        out << "[alchemy support] write mark on host thread " << alchemy::host::process::current_thread_id() << (s->on_main_thread() ? " (main)" : "") << '\n';
                        out << "    " << describe(*s, c.x[30]) << '\n';
                        for (std::uint64_t frame = c.x[29], depth = 0; frame && (frame & 7) == 0 && depth < 40; ++depth) {
                            std::uint64_t next{}, ret{};
                            try { next = m.read64(frame); ret = m.read64(frame + 8); } catch (...) { break; }
                            if (!ret) break;
                            out << "    " << describe(*s, ret) << '\n';
                            if (next <= frame) break;
                            frame = next;
                        }
                        std::fputs(out.str().c_str(), stderr);
                        std::fflush(stderr);
                    }
                }
                CallRegs forward;
                for (unsigned index = 0; index < 8; ++index) forward.x[index] = c.x[index];
                c.x[0] = EngineAdapter(s->process.calling_engine()).call_regs(target, forward).x0;
            }, std::string(libsystem_library)));
        }
    }
    // CFRunLoopSourceSignal counts per (source, main thread or not).
    static std::map<std::pair<Address, bool>, std::uint64_t> signals;   // guarded by table().mutex
    if (const auto original = support.process.resolve("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "_CFRunLoopSourceSignal")) {
        const auto target = original->address;
        static_cast<void>(support.process.install_function("_CFRunLoopSourceSignal", [s, target](CpuState& c, GuestMemory&) {
            {
                std::lock_guard lock(table().mutex);
                const auto count = ++signals[{c.x[0], s->on_main_thread()}];
                static_cast<void>(count);
            }
            CallRegs forward;
            forward.x[0] = c.x[0];
            static_cast<void>(EngineAdapter(s->process.calling_engine()).call_regs(target, forward));
        }, "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation"));
    }
    std::thread([s, seconds] {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        for (;;) {
            std::ostringstream out;
            {
                std::lock_guard lock(table().mutex);
                const auto now = std::chrono::steady_clock::now();
                for (const auto& [id, wait] : table().waits) {
                    out << "[alchemy support] waiting: " << wait.function << " since "
                        << std::chrono::duration_cast<std::chrono::seconds>(now - wait.since).count() << " s (host thread " << id << ")\n";
                    for (std::size_t index = 0; index < wait.frames.size(); ++index)
                        out << "    #" << index << ' ' << describe(*s, wait.frames[index]) << '\n';
                }
                for (const auto& [id, last] : table().last) {
                    if (table().waits.count(id)) continue;
                    out << "[alchemy support] thread " << id << " last " << last.function << (last.inside ? " (inside)" : "") << ' '
                        << std::chrono::duration_cast<std::chrono::milliseconds>(now - last.since).count() << " ms ago"
                        << (now - last.since > std::chrono::seconds(2) ? host_stack(id) : std::string()) << '\n';
                    for (const auto frame : last.frames) out << "    " << describe(*s, frame) << '\n';
                }
                {
                    std::vector<std::pair<std::uint64_t, const std::vector<std::uint64_t>*>> top;
                    for (const auto& [frames, count] : samples) top.emplace_back(count, &frames);
                    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                    for (std::size_t index = 0; index < top.size() && index < 14; ++index) {
                        out << "[alchemy support] main sample x" << top[index].first << '\n';
                        for (const auto frame : *top[index].second) out << "    " << describe(*s, frame) << '\n';
                    }
                }
                if (main_run_loop_available())
                    out << "[alchemy support] main run loop mode '" << main_run_loop().current_mode() << "' waiting "
                        << main_run_loop().is_waiting() << '\n';
                for (const auto& [key, count] : signals) {
                    const auto record = s->process.objects().find(key.first);
                    out << "[alchemy support] source 0x" << std::hex << key.first << std::dec << " record " << (record ? "alive" : "GONE");
                    if (record) out << " refs " << record->references.load() << " payload " << (record->payload ? typeid(*record->payload).name() : "none")
                                    << " destroying " << record->destroying.load();
                    out << '\n';
                }
                for (const auto& [key, count] : signals)
                    out << "[alchemy support] source signal 0x" << std::hex << key.first << std::dec << (key.second ? " main" : " other")
                        << " x" << count << '\n';
                for (const auto& perform : performs)
                    out << "[alchemy support] source perform " << describe(*s, perform.perform) << " x" << perform.count
                        << (perform.active ? " ACTIVE since " + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                                 std::chrono::steady_clock::now() - perform.since).count()) + " s"
                                           : std::string())
                        << '\n';
                for (const auto& [key, count] : table().calls)
                    if (count >= 100) out << "[alchemy support] calls: " << key.first << " x" << count << " from " << describe(*s, key.second) << '\n';
            }
            std::fputs(out.str().c_str(), stderr);
            std::fflush(stderr);
            std::this_thread::sleep_for(std::chrono::seconds(30));
        }
    }).detach();
}

} // namespace alchemy::gui::support_kit
