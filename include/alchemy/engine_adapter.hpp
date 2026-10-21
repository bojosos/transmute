#pragma once

#include "alchemy/call_regs.hpp"
#include "transmute/a64.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace alchemy {

// A Darwin signal's default action ended the guest process: abort() raises
// SIGABRT, unhandled guest faults raise SIGSEGV, SIGBUS, SIGILL or SIGTRAP.
// The process ends with the shell-visible status 128 + signal; other guest
// threads stop without running more guest code (no atexit, no TSD destructors).
class GuestSignalTermination : public std::runtime_error {
public:
    GuestSignalTermination(int signal, const std::string& reason) : std::runtime_error(reason), signal_(signal) {}
    [[nodiscard]] int signal() const noexcept { return signal_; }
    [[nodiscard]] std::uint32_t status() const noexcept { return 128U + static_cast<std::uint32_t>(signal_); }
    [[nodiscard]] static const char* name(int signal) noexcept {
        switch (signal) {
        case 4: return "SIGILL";
        case 5: return "SIGTRAP";
        case 6: return "SIGABRT";
        case 8: return "SIGFPE";
        case 9: return "SIGKILL";
        case 10: return "SIGBUS";
        case 11: return "SIGSEGV";
        case 15: return "SIGTERM";
        default: return "signal";
        }
    }
private:
    int signal_;
};

// The default action of a fatal Darwin signal raised by the calling guest
// thread (abort(), raise() with SIG_DFL). Throw from a synthetic call handler.
[[noreturn]] inline void terminate_by_signal(int signal, const std::string& reason) {
    throw GuestSignalTermination(signal, reason);
}

// Optional engine capability: page-granular protection inside an existing
// mapping (Darwin partial mprotect). False when the engine cannot express it.
template <class Engine>
bool protect_engine_range(Engine& engine, std::uint64_t address, std::uint64_t size,
                          bool readable, bool writable, bool executable) {
    if constexpr (requires { engine.protect_range(address, size, readable, writable, executable); }) {
        engine.protect_range(address, size, readable, writable, executable);
        return true;
    } else {
        return false;
    }
}

// Optional engine capability: reserve address space whose pages are committed
// later by protect_engine_range (Darwin PROT_NONE or very large anonymous
// mmap). Empty when the engine cannot express it.
template <class Engine>
std::optional<std::uint64_t> reserve_engine_range(Engine& engine, std::uint64_t size, std::uint64_t alignment,
                                                  bool readable, bool writable) {
    if constexpr (requires { engine.reserve_anywhere(size, alignment, readable, writable); }) {
        return engine.reserve_anywhere(size, alignment, readable, writable);
    } else {
        return std::nullopt;
    }
}

// Optional engine capabilities for Darwin VM semantics (darwin_vm_space.cpp):
// placement at an exact address, partial unmap, MAP_FIXED replacement,
// MADV_FREE discard and region walks. Engines without them keep Alchemy's
// whole-mapping model (hints ignored, MAP_FIXED and partial munmap refused).
template <class Engine>
inline constexpr bool engine_has_vm_ranges = requires(Engine& engine, std::uint64_t value) {
    engine.reserve_at(value, value, true, true);
    engine.unmap_range(value, value);
    engine.replace_range(value, value, true, true, true);
    engine.discard_range(value, value);
    engine.region_from(value);
};
template <class Engine>
bool engine_reserve_at(Engine& engine, std::uint64_t base, std::uint64_t size, bool readable, bool writable) {
    if constexpr (engine_has_vm_ranges<Engine>) return engine.reserve_at(base, size, readable, writable);
    else return false;
}
template <class Engine>
void engine_unmap_range(Engine& engine, std::uint64_t address, std::uint64_t size) {
    if constexpr (engine_has_vm_ranges<Engine>) engine.unmap_range(address, size);
    else throw std::logic_error("engine has no partial unmap");
}
template <class Engine>
void engine_replace_range(Engine& engine, std::uint64_t address, std::uint64_t size, bool readable, bool writable,
                          bool executable) {
    if constexpr (engine_has_vm_ranges<Engine>) engine.replace_range(address, size, readable, writable, executable);
    else throw std::logic_error("engine has no range replacement");
}
template <class Engine>
void engine_discard_range(Engine& engine, std::uint64_t address, std::uint64_t size) {
    if constexpr (engine_has_vm_ranges<Engine>) engine.discard_range(address, size);
}
// The region containing `address` or the lowest one above it; without the
// capability, only the containing region (query_region).
template <class Engine>
std::optional<transmute::GuestMemory::RegionInfo> engine_region_from(Engine& engine, std::uint64_t address) {
    if constexpr (engine_has_vm_ranges<Engine>) return engine.region_from(address);
    else return engine.query_region(address);
}

// Optional engine capability: committed (resident) bytes of [address, address +
// size). Empty when the engine cannot tell; callers then count every
// accessible page as resident.
template <class Engine>
std::optional<std::uint64_t> engine_resident_bytes(const Engine& engine, std::uint64_t address, std::uint64_t size) {
    if constexpr (requires { engine.resident_bytes(address, size); }) return engine.resident_bytes(address, size);
    else return std::nullopt;
}

// Optional engine capability: drop translations of [address, address + size)
// after the guest rewrote code there (sys_icache_invalidate). False when the
// engine has no explicit call; such engines track guest code writes themselves.
template <class Engine>
bool invalidate_engine_code(Engine& engine, std::uint64_t address, std::uint64_t size) {
    if constexpr (requires { engine.invalidate_code(address, size); }) {
        engine.invalidate_code(address, size);
        return true;
    } else {
        return false;
    }
}

// Optional engine capability: the calling guest thread opens (open = true) or
// closes a JIT write window (pthread_jit_write_protect_np(0) / (1)). Engines
// without it need no notice: they track guest code writes themselves.
template <class Engine>
void engine_jit_write_window(Engine& engine, bool open) {
    if constexpr (requires { engine.jit_write_window(open); }) engine.jit_write_window(open);
}

// Optional engine capability: [copy, copy + size) was just filled with an
// exact copy of the guest code at [source, source + size) (vm_remap of image
// text), so the engine may reuse the source's translations there.
template <class Engine>
void engine_code_copy(Engine& engine, std::uint64_t copy, std::uint64_t source, std::uint64_t size) {
    if constexpr (requires { engine.code_copy(copy, source, size); }) engine.code_copy(copy, source, size);
}

// Optional engine capability: TPIDR_EL0 of the calling guest thread (Darwin
// user code leaves it unused; Alchemy keeps the thread's TLV table there).
template <class Engine>
inline constexpr bool engine_has_thread_pointer =
    requires(Engine& engine) { engine.set_thread_pointer(std::uint64_t{}); };

template <class Engine>
bool set_engine_thread_pointer(Engine& engine, std::uint64_t value) {
    if constexpr (requires { engine.set_thread_pointer(value); }) {
        engine.set_thread_pointer(value);
        return true;
    } else {
        return false;
    }
}

// Optional engine capability: the synthetic call at `address` uses no
// SIMD/FP register, so its entry may skip copying them.
template <class Engine>
void mark_engine_call_integer_only(Engine& engine, std::uint64_t address) {
    if constexpr (requires { engine.set_guest_call_integer_only(address); }) engine.set_guest_call_integer_only(address);
}

// Optional engine capability: the synthetic call at `address` is libSystem's
// `symbol`, untraced; an engine may run the pure memory/string functions
// among them without their handler while no guest byte faults.
template <class Engine>
void mark_engine_call_leaf(Engine& engine, std::uint64_t address, std::string_view symbol) {
    if constexpr (requires { engine.set_guest_call_leaf(address, symbol); }) engine.set_guest_call_leaf(address, symbol);
}

// ---- process stop (Darwin exit and fatal signals) ---------------------------
// Optional engine capability: deliver a stop to every guest thread of the
// process at its next safe point (back-edge, dispatcher exit, return from a
// synthetic call). A stopped thread leaves its run() and runs no more guest
// code; outermost callbacks on any thread return 0 without running. False when
// the engine cannot interrupt guest threads (the reference engine).
template <class Engine>
inline constexpr bool engine_can_stop = requires(Engine& engine) { engine.request_stop(std::uint32_t{}); };

template <class Engine>
bool request_engine_stop(Engine& engine, std::uint32_t status) {
    if constexpr (requires { engine.request_stop(status); }) {
        engine.request_stop(status);
        return true;
    } else {
        return false;
    }
}

// True once a process stop was requested. Host code blocked on behalf of a
// guest thread (dispatch, condition, address and semaphore waits) polls this
// and then calls stop_engine_thread.
template <class Engine>
[[nodiscard]] bool engine_stop_requested(const Engine& engine) noexcept {
    if constexpr (requires { engine.stop_requested(); }) return engine.stop_requested();
    else return false;
}

// A fatal-signal termination the engine observed on any thread
// (GuestSignalTermination passing through a synthetic call), with the guest
// registers of the terminating thread for the backtrace.
struct EngineTermination {
    std::uint32_t status{};
    int signal{};
    std::string reason;
    std::uint64_t pc{}, lr{}, fp{};
    bool main_thread{};
};
template <class Engine>
[[nodiscard]] std::optional<EngineTermination> engine_termination(const Engine& engine) {
    if constexpr (requires { engine.termination(); }) {
        if (const auto value = engine.termination())
            return EngineTermination{value->status, value->signal, value->reason, value->pc, value->lr, value->fp,
                                     value->main_thread};
        return std::nullopt;
    } else {
        return std::nullopt;
    }
}

// Called once, on the thread that triggered it, when the engine itself starts
// a process stop (a fatal signal on any guest thread), so the personality can
// record its exit status before other threads observe the stop.
template <class Engine>
bool set_engine_stop_observer(Engine& engine, std::function<void(std::uint32_t)> observer) {
    if constexpr (requires { engine.set_stop_observer(std::move(observer)); }) {
        engine.set_stop_observer(std::move(observer));
        return true;
    } else {
        return false;
    }
}

// Ends the host process now with `status` (after a stop request whose guest
// threads did not stop, e.g. blocked in a host call). False when unsupported.
template <class Engine>
bool force_engine_exit(Engine& engine, std::uint32_t status, std::string_view diagnostic) {
    if constexpr (requires { engine.force_exit(status, diagnostic); }) {
        engine.force_exit(status, diagnostic);
        return true;
    } else {
        return false;
    }
}

// Optional engine capability: name a mapped image for diagnostics and the
// persistent translation cache.
template <class Engine>
void register_engine_image(Engine& engine, std::uint64_t header, std::uint64_t begin, std::uint64_t end,
                           std::uint64_t text_begin, std::uint64_t text_end, const std::string& path) {
    if constexpr (requires { engine.register_image(header, begin, end, text_begin, text_end, path); })
        engine.register_image(header, begin, end, text_begin, text_end, path);
}

// Optional engine capability: shared guest memory that can be mapped at a
// second address (Mach vm_remap of writable memory). engine_map_shared_memory
// maps zero-filled read/write memory as one region per `granule` bytes and
// returns false when the engine has no shared memory or cannot place it
// there (callers then map private memory). engine_alias_memory maps the
// shared memory at `source` again at `destination` (free, or whole shared
// regions it replaces); false when unsupported or refused.
template <class Engine>
bool engine_map_shared_memory(Engine& engine, std::uint64_t base, std::uint64_t size, std::uint64_t granule) {
    if constexpr (requires { engine.map_shared_memory(base, size, granule); }) {
        try {
            engine.map_shared_memory(base, size, granule);
            return true;
        } catch (const std::runtime_error&) {
            return false;
        }
    } else {
        return false;
    }
}
template <class Engine>
bool engine_is_shared_memory(const Engine& engine, std::uint64_t base, std::uint64_t size) {
    if constexpr (requires { engine.is_shared_memory(base, size); }) return engine.is_shared_memory(base, size);
    else return false;
}
template <class Engine>
bool engine_alias_memory(Engine& engine, std::uint64_t destination, std::uint64_t source, std::uint64_t size,
                         bool readable, bool writable, bool executable) {
    if constexpr (requires { engine.alias_memory(destination, source, size, readable, writable, executable); }) {
        try {
            engine.alias_memory(destination, source, size, readable, writable, executable);
            return true;
        } catch (const std::runtime_error&) {
            return false;
        }
    } else {
        return false;
    }
}

// Optional engine capability: a host section (Windows file-mapping HANDLE)
// viewed at a guest address, shared with every other process viewing the same
// file or named section (Darwin mmap(MAP_SHARED)). `view_size` bytes from
// `offset` at the start of a `size`-byte region whose tail is inaccessible;
// base == 0 lets the engine choose. Empty when the engine has no section
// mappings (the reference engine); a refused placement throws.
template <class Engine>
std::optional<std::uint64_t> engine_map_section(Engine& engine, std::uint64_t base, std::uint64_t size, void* section,
                                                std::uint64_t offset, std::uint64_t view_size, bool writable_section,
                                                bool readable, bool writable) {
    if constexpr (requires { engine.map_section(base, size, section, offset, view_size, writable_section, readable, writable); })
        return engine.map_section(base, size, section, offset, view_size, writable_section, readable, writable);
    else
        return std::nullopt;
}

// Optional engine capability: a host section viewed as `granule`-byte shared
// regions at [base, base + size) (cross-process memory). False when the
// engine has no section views; a refused placement throws.
template <class Engine>
bool engine_map_section_pages(Engine& engine, std::uint64_t base, std::uint64_t size, void* section,
                              std::uint64_t offset, std::uint64_t granule, bool writable_section, bool readable,
                              bool writable) {
    if constexpr (requires { engine.map_section_pages(base, size, section, offset, granule, writable_section, readable, writable); }) {
        engine.map_section_pages(base, size, section, offset, granule, writable_section, readable, writable);
        return true;
    } else {
        return false;
    }
}
// Optional engine capability: a duplicate of the host section handle behind
// shared guest memory at `address` (the caller closes it), the address's
// offset in it, the section's known size and writability. Null otherwise.
template <class Engine>
void* engine_duplicate_section(Engine& engine, std::uint64_t address, std::uint64_t& offset, std::uint64_t& size,
                               bool& writable) {
    if constexpr (requires { engine.duplicate_section(address, offset, size, writable); })
        return engine.duplicate_section(address, offset, size, writable);
    else
        return nullptr;
}

// Optional engine capability: hold the main executable's load range from the
// start of the process (alchemy-fx: host memory and guest memory share one
// address space). Called first thing in main.
template <class Engine>
void engine_hold_load_range(std::optional<std::uint64_t> load_base) {
    if constexpr (requires { Engine::hold_load_range(load_base); }) Engine::hold_load_range(load_base);
}

// Optional engine capability: another base for a position-independent main
// executable whose preferred range is taken (alchemy-fx: by host memory
// placed before main). nullopt: load at `preferred`.
template <class Engine>
std::optional<std::uint64_t> engine_slide_main_image(std::uint64_t preferred, std::uint64_t size) {
    if constexpr (requires { Engine::slide_main_image(preferred, size); }) return Engine::slide_main_image(preferred, size);
    else return std::nullopt;
}

// Optional engine capability: write pending translations (process exit).
template <class Engine>
void flush_engine_translations(Engine& engine) {
    if constexpr (requires { engine.flush_translations(); }) engine.flush_translations();
}

// Optional engine capability: a zero-filled general-purpose heap for guest
// malloc (alignment <= 16). Empty when the engine has none or for larger
// alignments; callers then map a guest region instead.
template <class Engine>
std::optional<std::uint64_t> engine_heap_allocate(Engine& engine, std::uint64_t size, std::uint64_t alignment) {
    if constexpr (requires { engine.heap_allocate(size, alignment); }) return engine.heap_allocate(size, alignment);
    else return std::nullopt;
}
template <class Engine>
void engine_heap_release(Engine& engine, std::uint64_t address) {
    if constexpr (requires { engine.heap_release(address); }) engine.heap_release(address);
    else throw std::logic_error("engine has no guest heap");
}

// Optional engine capability: a host->guest call with the full Apple ARM64
// register image (x0..x8, q0..q7, stack arguments) returning x0/x1 and
// q0..q3 (alchemy/call_regs.hpp). Engines without it (the reference engine)
// run integer-only calls through invoke_callback and report x0 alone
// (CallResult::complete == false); vector or stack arguments, or an indirect
// result, fail explicitly there.
template <class Engine>
CallResult engine_call_regs(Engine& engine, std::uint64_t entry, const CallRegs& registers) {
    if constexpr (requires { engine.invoke_callback_regs(entry, registers); }) {
        return engine.invoke_callback_regs(entry, registers);
    } else {
        if (registers.uses_vectors() || !registers.stack.empty() || registers.indirect_result)
            throw std::runtime_error(
                "Alchemy requires Transmute capability: guest callback with vector, stack or indirect-result registers");
        CallResult result{};
        result.x0 = engine.invoke_callback(entry, std::span<const std::uint64_t>(registers.x));
        return result;
    }
}

// Worker and callback calls use existing public engine contracts. Missing
// capabilities are reported for the separately owned Transmute implementation.
class EngineAdapter {
public:
    // Matches the supplied engine's public configurable-stack contract.
    static constexpr std::uint64_t maximum_stack_size = 1024ULL * 1024 * 1024;
    explicit EngineAdapter(transmute::ReferenceEngine& engine) : engine_(engine) {}
    [[nodiscard]] std::unique_ptr<transmute::ReferenceEngine> create_worker(
        std::uint64_t entry, std::uint64_t argument, std::uint64_t stack_size = 0) {
        return engine_.fork_thread(entry, argument, stack_size);
    }
    [[nodiscard]] std::uint64_t default_worker_stack_size() const noexcept {
        return engine_.default_worker_stack_size();
    }
    [[nodiscard]] std::uint64_t call(std::uint64_t entry,
        std::span<const std::uint64_t> arguments = {}) { return engine_.invoke_callback(entry, arguments); }
    // Full register image call; see engine_call_regs.
    [[nodiscard]] CallResult call_regs(std::uint64_t entry, const CallRegs& registers) {
        return engine_call_regs(engine_, entry, registers);
    }
    // Engines that can unwind the calling guest thread to its run() entry
    // provide two of these capabilities themselves: a cooperative stop (the
    // process is stopping; requested by a blocked host wait) and a thread exit
    // from a nested guest callback (pthread_exit below a host frame). They
    // unwind instead of failing; the thread's exit value is already recorded.
    template <class Engine = transmute::ReferenceEngine>
    [[noreturn]] static void require(std::string_view capability) {
        if constexpr (requires { Engine::unwind_current_thread(false); }) {
            if (capability.starts_with("cooperative guest")) Engine::unwind_current_thread(true);
            else if (capability == "thread exit from a nested guest callback") Engine::unwind_current_thread(false);
        }
        throw std::runtime_error("Alchemy requires Transmute capability: " + std::string(capability));
    }
    // Unwinds the calling guest thread because its process is stopping
    // (engine_stop_requested). Engines without the capability report it.
    template <class Engine>
    [[noreturn]] static void stop_thread(Engine&, std::string_view where) {
        require<Engine>("cooperative guest stop during " + std::string(where));
    }
private:
    transmute::ReferenceEngine& engine_;
};

} // namespace alchemy
