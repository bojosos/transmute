// Interpreter-backed stubs for the cross-component symbols the seh library
// uses (call_guest, call_guest_prepared, find_guest_module, guest_modules),
// plus the test harness declared in seh_test_util.hpp.

#include "fx/seh/tests/seh_test_util.hpp"

#include "fx/common/interpreter.hpp"
#include "fx/runtime/runtime.hpp"
#include "fx/seh/seh.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

namespace transmute::fx {

// --- symbols owned by other components, stubbed for the seh test binary ---

static std::vector<std::unique_ptr<GuestModule>>& module_registry() {
    static std::vector<std::unique_ptr<GuestModule>> instance;
    return instance;
}

const GuestModule* find_guest_module(std::uint64_t address) noexcept {
    for (const auto& module : module_registry())
        if (address >= module->base && address < module->base + module->size) return module.get();
    return nullptr;
}

std::vector<const GuestModule*> guest_modules() {
    std::vector<const GuestModule*> result;
    for (const auto& module : module_registry()) result.push_back(module.get());
    return result;
}

} // namespace transmute::fx

namespace fxseh_test {
namespace {

constexpr std::uint64_t kReturnToHost = transmute::fx::kReturnToHostPc;

std::map<std::uint64_t, HostEntry>& host_entries() {
    static std::map<std::uint64_t, HostEntry> instance;
    return instance;
}

struct Arena {
    std::uint8_t* base = nullptr;
    std::size_t capacity = 0;
    std::size_t used = 0;
};
Arena& arena() {
    static Arena instance;
    if (!instance.base) {
        instance.capacity = 16u << 20;
        instance.base = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, instance.capacity, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    }
    return instance;
}

struct Stack {
    std::uint8_t* base = nullptr;
    std::size_t size = 1u << 20;
    std::size_t used = 0;
};
Stack& stack() {
    static Stack instance;
    if (!instance.base)
        instance.base = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, instance.size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    return instance;
}

// Runs guest code at ctx.pc until the return-to-host sentinel, dispatching to
// registered host entries. Re-entrant.
void run_guest(GuestContext& ctx) {
    const auto& entries = host_entries();
    for (int steps = 0; steps < 10'000'000; ++steps) {
        if (ctx.pc == kReturnToHost) return;
        const auto it = entries.find(ctx.pc);
        if (it != entries.end()) {
            const GuestResolution resolution = it->second(ctx);
            if (resolution == GuestResolution::continue_at_caller) ctx.pc = ctx.x[30];
            // continue_at_pc / terminated: the entry already set ctx.pc.
            continue;
        }
        transmute::fx::interpret_one(ctx);
    }
    throw std::runtime_error("run_guest: step limit exceeded (runaway guest code)");
}

GuestContext make_context(std::uint64_t sp) {
    GuestContext ctx{};
    ctx.x[31] = sp;
    ctx.x[30] = kReturnToHost;
    ctx.pc = 0;
    ctx.guest_stack_base = guest_stack_base();
    ctx.guest_stack_limit = guest_stack_limit();
    return ctx;
}

} // namespace

std::uint64_t guest_stack_base() { return reinterpret_cast<std::uint64_t>(stack().base) + stack().size; }
std::uint64_t guest_stack_limit() { return reinterpret_cast<std::uint64_t>(stack().base); }

// Carves a down-growing frame from the shared guest stack for one call_guest
// invocation (nested calls get disjoint slices). Returns the initial SP.
std::uint64_t push_guest_frame(std::size_t size) {
    Stack& s = stack();
    s.used += size;
    if (s.used > s.size) throw std::runtime_error("test guest stack exhausted");
    return reinterpret_cast<std::uint64_t>(s.base) + (s.size - s.used);
}
void pop_guest_frame(std::size_t size) { stack().used -= size; }

void run_guest_context(GuestContext& ctx) { run_guest(ctx); }

GuestContext run_call(std::uint64_t entry, std::span<const std::uint64_t> args) {
    constexpr std::size_t kFrame = 64u * 1024u;
    const std::uint64_t top = push_guest_frame(kFrame);
    GuestContext ctx = make_context(top);
    for (std::size_t i = 0; i < args.size() && i < 8; ++i) ctx.x[i] = args[i];
    // Arguments beyond x7 would be spilled to the guest stack; tests use <= 8.
    ctx.pc = entry;
    try {
        run_guest(ctx);
    } catch (...) {
        pop_guest_frame(kFrame);
        throw;
    }
    pop_guest_frame(kFrame);
    return ctx;
}

std::uint64_t alloc_guest(std::size_t size, std::size_t alignment) {
    Arena& a = arena();
    a.used = (a.used + alignment - 1) & ~(alignment - 1);
    if (a.used + size > a.capacity) throw std::runtime_error("test arena exhausted");
    const std::uint64_t address = reinterpret_cast<std::uint64_t>(a.base + a.used);
    a.used += size;
    return address;
}

void free_all_guest() {
    arena().used = 0;
    stack().used = 0;
}

void register_module(const GuestModule& module) {
    transmute::fx::module_registry().push_back(std::make_unique<GuestModule>(module));
}
void clear_modules() { transmute::fx::module_registry().clear(); }

void register_host_entry(std::uint64_t pc, HostEntry entry) { host_entries()[pc] = std::move(entry); }
void clear_host_entries() { host_entries().clear(); }

void reset_all() {
    clear_modules();
    clear_host_entries();
    free_all_guest();
    transmute::fx::seh::reset_dynamic_function_tables();
    transmute::fx::seh::reset_process_state();
    transmute::fx::seh::reset_thread_state();
}

std::uint64_t emit_code(const std::vector<std::uint32_t>& words) {
    const std::uint64_t address = alloc_guest(words.size() * 4 + 4, 16);
    for (std::size_t i = 0; i < words.size(); ++i) {
        const std::uint32_t word = words[i];
        std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(address + i * 4)), &word, 4);
    }
    return address;
}

namespace a64 {

std::vector<std::uint32_t> mov_imm(unsigned xd, std::uint64_t value) {
    std::vector<std::uint32_t> words;
    bool first = true;
    for (unsigned shift = 0; shift < 64; shift += 16) {
        const std::uint16_t chunk = static_cast<std::uint16_t>(value >> shift);
        if (chunk == 0 && !first) continue;
        if (first) {
            words.push_back(0xD2800000u | (static_cast<std::uint32_t>(chunk) << 5) | ((shift / 16) << 21) | xd);
            first = false;
        } else {
            words.push_back(0xF2800000u | (static_cast<std::uint32_t>(chunk) << 5) | ((shift / 16) << 21) | xd);
        }
    }
    if (first) words.push_back(0xD2800000u | xd);  // movz xd, #0
    return words;
}

std::uint32_t add_sp_imm(std::int32_t imm) { return 0x910003FFu | (static_cast<std::uint32_t>(imm & 0xfff) << 10); }
std::uint32_t sub_sp_imm(std::int32_t imm) { return 0xD10003FFu | (static_cast<std::uint32_t>(imm & 0xfff) << 10); }

std::uint32_t stp_x(unsigned t1, unsigned t2, unsigned base, std::int32_t imm, bool preindex, bool writeback) {
    const std::uint32_t scaled = static_cast<std::uint32_t>((imm / 8) & 0x7f);
    std::uint32_t op = 0x29000000u;  // signed offset STP (x)
    if (writeback) op = preindex ? 0x29800000u : 0x28800000u;
    return op | 0x80000000u | (scaled << 15) | (t2 << 10) | (base << 5) | t1;
}

std::uint32_t ldp_x(unsigned t1, unsigned t2, unsigned base, std::int32_t imm, bool postindex, bool writeback) {
    const std::uint32_t scaled = static_cast<std::uint32_t>((imm / 8) & 0x7f);
    std::uint32_t op = 0x29400000u;  // signed offset LDP (x)
    if (writeback) op = postindex ? 0x28C00000u : 0x29C00000u;
    return op | 0x80000000u | (scaled << 15) | (t2 << 10) | (base << 5) | t1;
}

std::uint32_t str_x(unsigned t, unsigned base, std::int32_t imm) {
    const std::uint32_t scaled = static_cast<std::uint32_t>((imm / 8) & 0xfff);
    return 0xF9000000u | (scaled << 10) | (base << 5) | t;
}
std::uint32_t ldr_x(unsigned t, unsigned base, std::int32_t imm) {
    const std::uint32_t scaled = static_cast<std::uint32_t>((imm / 8) & 0xfff);
    return 0xF9400000u | (scaled << 10) | (base << 5) | t;
}

std::uint32_t bl(std::int64_t byte_offset) {
    const std::uint32_t imm26 = static_cast<std::uint32_t>((byte_offset >> 2) & 0x3ffffff);
    return 0x94000000u | imm26;
}
std::uint32_t b(std::int64_t byte_offset) {
    const std::uint32_t imm26 = static_cast<std::uint32_t>((byte_offset >> 2) & 0x3ffffff);
    return 0x14000000u | imm26;
}
std::uint32_t brk(std::uint16_t imm) { return 0xD4200000u | (static_cast<std::uint32_t>(imm) << 5); }

} // namespace a64

} // namespace fxseh_test

namespace fxseh_test {

thread_local GuestContext* t_thread_context = nullptr;
int g_rearm_calls = 0;
std::uint64_t g_rearm_sp = 0;
bool g_rearm_result = true;

ThreadContextScope::ThreadContextScope(GuestContext& ctx) : previous_(t_thread_context) { t_thread_context = &ctx; }
ThreadContextScope::~ThreadContextScope() { t_thread_context = previous_; }

} // namespace fxseh_test

namespace transmute::fx {

GuestContext* current_context() noexcept { return fxseh_test::t_thread_context; }

// Runtime guard re-arm (src/fx/runtime): recorded for the _resetstkoflw test.
bool rearm_guest_stack_guard(GuestContext& ctx) {
    ++fxseh_test::g_rearm_calls;
    fxseh_test::g_rearm_sp = ctx.x[31];
    return fxseh_test::g_rearm_result;
}

// Mirrors the runtime: with a thread context, the callee runs on it at the
// current (16-aligned) guest SP with x30 = kReturnToHostPc, and x18-x31, pc,
// nzcv and d8-d15 are restored afterwards (x0 holds the result). Without one
// (plain unit tests) each call gets a fresh context on its own stack slice.
std::uint64_t call_guest(std::uint64_t entry, std::span<const std::uint64_t> args) {
    GuestContext* ctx = fxseh_test::t_thread_context;
    if (!ctx) return fxseh_test::run_call(entry, args).x[0];
    std::uint64_t saved_x[14];
    std::memcpy(saved_x, &ctx->x[18], sizeof(saved_x));
    const std::uint64_t saved_pc = ctx->pc, saved_nzcv = ctx->nzcv;
    std::uint64_t saved_d[8];
    for (unsigned i = 0; i < 8; ++i) saved_d[i] = ctx->v[8 + i][0];
    ctx->x[31] &= ~std::uint64_t{15};
    for (std::size_t i = 0; i < args.size() && i < 8; ++i) ctx->x[i] = args[i];
    ctx->x[30] = kReturnToHostPc;
    ctx->pc = entry;
    struct Restore {
        GuestContext* ctx;
        std::uint64_t* x;
        std::uint64_t pc, nzcv;
        std::uint64_t* d;
        ~Restore() {
            std::memcpy(&ctx->x[18], x, 14 * sizeof(std::uint64_t));
            ctx->pc = pc;
            ctx->nzcv = nzcv;
            for (unsigned i = 0; i < 8; ++i) ctx->v[8 + i][0] = d[i];
        }
    } restore{ctx, saved_x, saved_pc, saved_nzcv, saved_d};
    fxseh_test::run_guest_context(*ctx);
    return ctx->x[0];
}

void call_guest_prepared(GuestContext& ctx, std::uint64_t entry) {
    ctx.x[30] = kReturnToHostPc;
    ctx.pc = entry;
    fxseh_test::run_guest_context(ctx);
}

} // namespace transmute::fx
