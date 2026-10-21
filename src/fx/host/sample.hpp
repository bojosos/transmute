#pragma once

// Host area "sample": diagnostic sampling of host threads (docs/FX_HOST_SEAMS.md,
// "Diagnostics"): the sampling profiler (runtime/profiler.cpp) and the
// translator's image-audit tool stop a thread for a moment, read its
// registers and the top of its stack, and name host code addresses.
//
// win32: OpenThread, SuspendThread, GetThreadContext, ResumeThread, Win64
// unwind data (RtlVirtualUnwind) over a copy of the stack, dbghelp. A Linux
// backend signals the thread (tgkill + a SIGPROF handler that copies its
// ucontext_t and stack), walks frame pointers or DWARF CFI, and uses dladdr.
// None of this is on a measured path: it runs only when a profile is asked for.

#include "fx/host/fault.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace transmute::fx::host::sample {

using Thread = void*;   // a host thread opened for sampling; nullptr = none

// Opens thread `id` for suspend, resume and register reads; nullptr if it
// cannot. Sampler handles stay open for the process lifetime.
Thread open_thread(std::uint32_t id) noexcept;
Thread open_current_thread() noexcept;   // a real handle to the calling thread
bool suspend(Thread thread) noexcept;
void resume(Thread thread) noexcept;

// A suspended thread's registers (the host CpuContext; fault.hpp accessors).
struct Registers {
    alignas(16) unsigned char storage[kCpuContextBytes];
    CpuContext& context() noexcept { return *reinterpret_cast<CpuContext*>(storage); }
    const CpuContext& context() const noexcept { return *reinterpret_cast<const CpuContext*>(storage); }
};
// `integer`: all general registers (for unwinding), else control registers only.
bool capture(Thread thread, Registers& registers, bool integer) noexcept;

// Copies up to `size` bytes of a suspended thread's stack from `sp`, inside
// its committed region; returns the bytes copied (0 on failure).
std::size_t copy_stack(std::uint64_t sp, std::uint8_t* out, std::size_t size) noexcept;

// The code arena's reclaim (runtime/regions.cpp) also uses this area: it
// stops every guest thread for a moment and looks for code addresses in its
// registers and stack. Closes a handle from open_thread.
void close_thread(Thread thread) noexcept;
// Every register of a suspended thread that can hold an address, as 64-bit
// words: the general registers, the instruction and stack pointers and both
// halves of each vector register. Writes up to `max` words to `out` and the
// stack pointer to `sp`; returns the number of words, 0 on failure.
std::size_t capture_words(Thread thread, std::uint64_t* out, std::size_t max, std::uint64_t& sp) noexcept;
// Calls word(user, value) for every 8-byte word of a suspended thread's stack
// from `sp` (rounded down to 8) to the end of the committed region holding
// it, i.e. its whole live stack. `word` must not allocate or lock (the thread
// may hold any lock). False if `sp` is not committed or a read faulted.
bool scan_stack(std::uint64_t sp, void (*word)(void* user, std::uint64_t value), void* user) noexcept;
// Host return addresses of a stack copy taken at registers' sp (the first is
// the pc). Stops at a pc for which `stop(pc)` is true (translated code), a
// frame outside the copy, or a failed read.
unsigned unwind_copy(const Registers& registers, const std::uint8_t* copy, std::size_t size, std::uint64_t* frames,
                     unsigned max, bool (*stop)(std::uint64_t pc)) noexcept;

// Loads symbols for host modules (once, from the sampler thread); false if
// none are available.
bool load_symbols() noexcept;
// "module!symbol" with symbols, else "module+0xoffset", else "?".
std::string describe(std::uint64_t pc, bool symbols);
// The symbol at `pc` alone (empty if none).
std::string symbol(std::uint64_t pc);
// Source file (without its directory) and line of `pc`; false if unknown.
bool source_line(std::uint64_t pc, std::string& file, unsigned& line);
// [begin, end) of the host module where blocked threads wait (ntdll on win32).
bool wait_module(std::uint64_t& begin, std::uint64_t& end) noexcept;

// A high-resolution one-shot timer for the sampler loop; nullptr if the host
// has none (wait() then sleeps 1 ms).
using Timer = void*;
Timer create_timer() noexcept;
void wait(Timer timer, std::uint32_t microseconds) noexcept;

} // namespace transmute::fx::host::sample
