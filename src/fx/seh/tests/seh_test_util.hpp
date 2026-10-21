#pragma once

// Test support for the seh component: an interpreter-backed call_guest stub,
// a guest-module registry, host-entry stand-ins for guest handlers/filters,
// and a tiny ARM64 encoder for hand-assembled prologs/epilogs.

#include "fx/seh/seh.hpp"
#include "transmute/fx/process.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace fxseh_test {

using transmute::fx::GuestContext;
using transmute::fx::GuestModule;
using transmute::fx::GuestResolution;

// Registers a guest module for find_guest_module / guest_modules and the
// function-table lookup. `info` is copied; its .pdata must stay alive.
void register_module(const GuestModule& module);
void clear_modules();

// A host-entry stand-in for a guest routine (handler, filter, callback,
// RtlUnwindEx, ...). When the interpreter reaches `pc`, the function runs on
// the GuestContext instead. Returns how execution proceeds.
using HostEntry = std::function<GuestResolution(GuestContext&)>;
void register_host_entry(std::uint64_t pc, HostEntry entry);
void clear_host_entries();

// Resets modules, host entries, dynamic tables and dispatcher state.
void reset_all();

// rearm_guest_stack_guard stub state (runtime API used by _resetstkoflw).
extern int g_rearm_calls;
extern std::uint64_t g_rearm_sp;
extern bool g_rearm_result;

// Makes `ctx` the calling thread's guest context for the duration of the
// scope (current_context(); call_guest runs on it like the runtime does).
class ThreadContextScope {
public:
    explicit ThreadContextScope(GuestContext& ctx);
    ~ThreadContextScope();
    ThreadContextScope(const ThreadContextScope&) = delete;
    ThreadContextScope& operator=(const ThreadContextScope&) = delete;

private:
    GuestContext* previous_;
};

// Guest memory arena (page-aligned RW). Returned addresses are guest == host.
std::uint64_t alloc_guest(std::size_t size, std::size_t alignment = 16);
void free_all_guest();

// A region of guest stack memory for call_guest frames.
std::uint64_t guest_stack_base();   // highest address (exclusive)
std::uint64_t guest_stack_limit();  // lowest

// ---- minimal ARM64 encoder (little-endian words) ----
namespace a64 {
inline std::uint32_t ret() { return 0xD65F03C0u; }
inline std::uint32_t ret(unsigned reg) { return 0xD65F0000u | (reg << 5); }
inline std::uint32_t nop() { return 0xD503201Fu; }
inline std::uint32_t mov_x_sp(unsigned xd) { return 0x91000000u | (31u << 5) | xd; }  // add xd, sp, #0
// movz/movk building a 64-bit immediate into xd.
std::vector<std::uint32_t> mov_imm(unsigned xd, std::uint64_t value);
inline std::uint32_t movz_w(unsigned wd, std::uint16_t imm) {
    return 0x52800000u | (static_cast<std::uint32_t>(imm) << 5) | wd;
}
std::uint32_t add_sp_imm(std::int32_t imm);   // add sp, sp, #imm  (imm multiple of 1, 0..4095)
std::uint32_t sub_sp_imm(std::int32_t imm);   // sub sp, sp, #imm
std::uint32_t stp_x(unsigned t1, unsigned t2, unsigned base, std::int32_t imm, bool preindex, bool writeback);
std::uint32_t ldp_x(unsigned t1, unsigned t2, unsigned base, std::int32_t imm, bool postindex, bool writeback);
std::uint32_t str_x(unsigned t, unsigned base, std::int32_t imm);  // str xt,[base,#imm]
std::uint32_t ldr_x(unsigned t, unsigned base, std::int32_t imm);  // ldr xt,[base,#imm]
std::uint32_t bl(std::int64_t byte_offset);                        // bl #off
std::uint32_t b(std::int64_t byte_offset);                         // b #off
std::uint32_t brk(std::uint16_t imm);                              // brk #imm
} // namespace a64

// Assembles words into the guest arena and returns the base address.
std::uint64_t emit_code(const std::vector<std::uint32_t>& words);

} // namespace fxseh_test
