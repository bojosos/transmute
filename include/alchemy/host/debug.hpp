#pragma once

// Host area "debug" (owner: integrator). The rules are in include/alchemy/host/host.hpp.
// Post-mortem reports of host crashes: a fatal fault nothing else handles (a fault in Alchemy,
// a GPU driver or a system library), the host stack and module names. The guest side of a report
// (guest pc, backtrace) is the caller's.

#include "alchemy/host/host.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace alchemy::host::debug {

enum class Access : std::uint8_t { none, read, write, execute };

struct Fault {
    std::uint32_t code{};              // the host's fault code (Windows exception code, signal number)
    std::uintptr_t address{};          // the faulting instruction
    Access access{};                   // for memory faults: what the instruction did
    std::uintptr_t data_address{};     // for memory faults: the address it touched
    std::span<const std::uintptr_t> stack; // the host stack at the fault, innermost first
};

// Turns off the host's crash dialogs for this process and calls `report` on the
// faulting thread for a fatal fault that nothing handles. `report` must end the
// process (host::process::terminate_self); if it returns, the host's default
// handling continues. Process-local: system-wide crash settings are untouched.
void on_fatal_fault(void (*report)(const Fault& fault));

// Up to frames.size() return addresses of the calling thread's host stack,
// innermost first, skipping `skip` frames above the caller. Returns the count.
[[nodiscard]] std::size_t capture_stack(std::span<std::uintptr_t> frames, unsigned skip = 0) noexcept;

// Diagnostics (hang dumps): the host stack of every other thread of this
// process, innermost first, up to `depth` frames each. Each thread is
// suspended only while it is walked, and nothing is allocated meanwhile. A
// walk ends at code without unwind data (guest code the engine generated).
struct ThreadStack {
    std::uint64_t thread{};               // host thread id (host::process::current_thread_id)
    std::uint64_t cpu_ms{};               // CPU time so far (user + kernel): a spinning thread's grows between dumps
    std::vector<std::uintptr_t> frames;
};
[[nodiscard]] std::vector<ThreadStack> other_thread_stacks(std::size_t depth = 40);

// "0x<address> <module>+0x<offset>", or "0x<address> (no module)".
[[nodiscard]] std::string describe_address(std::uintptr_t address);

} // namespace alchemy::host::debug
