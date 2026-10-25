#include "fx/common/interpreter.hpp"

#include "fx/host/vm.hpp"
#include "transmute/a64.hpp"

#include <intrin.h>

#include <atomic>
#include <cstring>
#include <type_traits>
#include <memory>
#include <stdexcept>

namespace transmute::fx {
namespace {

// The reference engine with its checked memory replaced by direct host
// memory (guest VA == host VA). Only the memory entry points used by
// ReferenceEngine::execute are overridden.
class DirectMemoryEngine final : public ReferenceEngine {
public:
    DirectMemoryEngine() : ReferenceEngine(ExecutionStart{0}) {}

    std::uint32_t read32(std::uint64_t address) const override {
        std::uint32_t v; std::memcpy(&v, ptr(address), 4); return v;
    }
    std::uint64_t read64(std::uint64_t address) const override {
        std::uint64_t v; std::memcpy(&v, ptr(address), 8); return v;
    }
    void write32(std::uint64_t address, std::uint32_t value) override {
        std::memcpy(ptr(address), &value, 4);
    }
    void write64(std::uint64_t address, std::uint64_t value) override {
        std::memcpy(ptr(address), &value, 8);
    }
    std::optional<RegionInfo> query_region(std::uint64_t address) const override {
        namespace vm = host::vm;
        vm::RegionInfo info;
        if (!vm::query(ptr(address), info) || !info.committed()) return std::nullopt;
        const auto protect = info.protect & 0xff;
        const bool writable = protect == vm::kReadWrite || protect == vm::kWriteCopy ||
                              protect == vm::kExecuteReadWrite || protect == vm::kExecuteWriteCopy;
        const bool readable = protect != vm::kNoAccess && protect != vm::kExecute;
        return RegionInfo{info.base, info.size, readable, writable, false, false, true};
    }

protected:
    std::uint8_t read8(std::uint64_t address) const override {
        return *static_cast<const volatile std::uint8_t*>(ptr(address));
    }
    void write8(std::uint64_t address, std::uint8_t value) override {
        *static_cast<volatile std::uint8_t*>(ptr(address)) = value;
    }

private:
    static void* ptr(std::uint64_t address) {
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
    }
};

DirectMemoryEngine& engine() {
    thread_local std::unique_ptr<DirectMemoryEngine> instance;
    if (!instance) instance = std::make_unique<DirectMemoryEngine>();
    return *instance;
}

// ---- atomics on direct memory ------------------------------------------------------
//
// The reference engine performs exclusives, LSE and CAS byte by byte and keeps
// its exclusive monitor in the (thread-local) engine. On direct host memory
// shared by host threads that is neither atomic nor visible to translated
// code, so these instructions are executed here with host interlocked
// operations on the guest address (guest VA == host VA):
//  * LDADD/LDCLR/LDEOR/LDSET/SWP: lock xadd / and / xor / or / xchg; the
//    signed and unsigned min/max: lock cmpxchg loops (the store is always
//    performed, as on ARM, even when the value does not change).
//  * CAS: lock cmpxchg; CASP W: 64-bit lock cmpxchg; CASP X: lock cmpxchg16b.
//  * LDXR/LDXP record (address, value, size) in GuestContext::excl_*, STXR/
//    STXP succeed iff address and size match and a lock cmpxchg (cmpxchg16b
//    for LDXP/STXP X) against the recorded value succeeds; the reservation is
//    cleared either way; CLREX clears it. This is exactly the translated
//    path's monitor (emit_atomic.cpp), so interpreted and translated code
//    may operate on the same lock word, and a reservation taken by one is
//    honoured by the other. ABA is accepted, as in translated code.
// Every locked instruction is a full barrier and x64 loads are acquire, so
// ordering is at least as strong as the ARM acquire/release variants.
// Encodings the reference engine rejects (unaligned addresses, invalid
// register pairs) are left to it, so it reports them as before. Memory
// faults propagate before ctx is modified.

constexpr std::uint64_t size_mask(unsigned bytes) {
    return bytes >= 8 ? ~std::uint64_t{0} : (std::uint64_t{1} << (bytes * 8)) - 1;
}

// Rt/Rs/Rt2 operands: 31 is XZR.
std::uint64_t operand(const GuestContext& ctx, unsigned index) { return index == 31 ? 0 : ctx.x[index]; }
void set_operand(GuestContext& ctx, unsigned index, std::uint64_t value, bool wide) {
    if (index != 31) ctx.x[index] = wide ? value : static_cast<std::uint32_t>(value);
}

template <class T>
std::atomic_ref<T> atomic_at(std::uint64_t address) {
    return std::atomic_ref<T>(*reinterpret_cast<T*>(static_cast<std::uintptr_t>(address)));
}

template <class T>
std::uint64_t load_t(std::uint64_t address) {
    return atomic_at<T>(address).load(std::memory_order_acquire);
}
std::uint64_t atomic_load(std::uint64_t address, unsigned bytes) {
    switch (bytes) {
    case 1: return load_t<std::uint8_t>(address);
    case 2: return load_t<std::uint16_t>(address);
    case 4: return load_t<std::uint32_t>(address);
    default: return load_t<std::uint64_t>(address);
    }
}

// Returns the old value; *success reports whether it equalled `expected`.
template <class T>
std::uint64_t cas_t(std::uint64_t address, std::uint64_t expected, std::uint64_t desired, bool* success) {
    T old = static_cast<T>(expected);
    const bool ok = atomic_at<T>(address).compare_exchange_strong(old, static_cast<T>(desired));
    if (success) *success = ok;
    return old;
}
std::uint64_t atomic_cas(std::uint64_t address, unsigned bytes, std::uint64_t expected, std::uint64_t desired,
                         bool* success = nullptr) {
    switch (bytes) {
    case 1: return cas_t<std::uint8_t>(address, expected, desired, success);
    case 2: return cas_t<std::uint16_t>(address, expected, desired, success);
    case 4: return cas_t<std::uint32_t>(address, expected, desired, success);
    default: return cas_t<std::uint64_t>(address, expected, desired, success);
    }
}

// 128-bit compare-exchange (lock cmpxchg16b). value[0] = low, value[1] = high.
// On return `expected` holds the old memory value.
bool atomic_cas128(std::uint64_t address, std::uint64_t expected[2], const std::uint64_t desired[2]) {
    auto* target = reinterpret_cast<volatile long long*>(static_cast<std::uintptr_t>(address));
    long long comparand[2] = {static_cast<long long>(expected[0]), static_cast<long long>(expected[1])};
    const bool ok = _InterlockedCompareExchange128(target, static_cast<long long>(desired[1]),
                                                   static_cast<long long>(desired[0]), comparand) != 0;
    expected[0] = static_cast<std::uint64_t>(comparand[0]);
    expected[1] = static_cast<std::uint64_t>(comparand[1]);
    return ok;
}

// op: 0 ADD 1 CLR 2 EOR 3 SET 4 SMAX 5 SMIN 6 UMAX 7 UMIN, 8 SWP. Returns the old value.
template <class T>
std::uint64_t rmw_t(std::uint64_t address, unsigned op, std::uint64_t source_bits) {
    using S = std::make_signed_t<T>;
    const T source = static_cast<T>(source_bits);
    auto ref = atomic_at<T>(address);
    switch (op) {
    case 0: return ref.fetch_add(source);
    case 1: return ref.fetch_and(static_cast<T>(~source));
    case 2: return ref.fetch_xor(source);
    case 3: return ref.fetch_or(source);
    case 8: return ref.exchange(source);
    default: break;
    }
    T old = ref.load(std::memory_order_relaxed);
    for (;;) {
        T replacement;
        switch (op) {
        case 4: replacement = static_cast<S>(old) >= static_cast<S>(source) ? old : source; break;
        case 5: replacement = static_cast<S>(old) <= static_cast<S>(source) ? old : source; break;
        case 6: replacement = old >= source ? old : source; break;
        default: replacement = old <= source ? old : source; break;
        }
        // Always a locked write (also when replacement == old): the
        // instruction is a read-modify-write with release semantics.
        if (ref.compare_exchange_weak(old, replacement)) return old;
    }
}
std::uint64_t atomic_rmw(std::uint64_t address, unsigned bytes, unsigned op, std::uint64_t source) {
    switch (bytes) {
    case 1: return rmw_t<std::uint8_t>(address, op, source);
    case 2: return rmw_t<std::uint16_t>(address, op, source);
    case 4: return rmw_t<std::uint32_t>(address, op, source);
    default: return rmw_t<std::uint64_t>(address, op, source);
    }
}

void clear_reservation(GuestContext& ctx) { ctx.excl_size = 0; }

// Executes `word` if it is an exclusive, CLREX, CAS(P) or LSE atomic that is
// handled natively. Returns false to leave the instruction to the reference
// engine. The decode order and class masks follow ReferenceEngine::execute.
bool interpret_atomic(GuestContext& ctx, std::uint32_t word) {
    const unsigned rt = word & 31u;
    const unsigned rn = (word >> 5) & 31u;
    const unsigned rs = (word >> 16) & 31u;
    const std::uint64_t address = ctx.x[rn];   // Rn == 31 is SP (ctx.x[31])

    // LDXP/LDAXP/STXP/STLXP
    if ((word & 0xbfa00000u) == 0x88200000u) {
        const bool load = (word & 0x00400000u) != 0;
        const bool wide = (word & 0x40000000u) != 0;
        const unsigned rt2 = (word >> 10) & 31u;
        const unsigned bytes = wide ? 16u : 8u;
        if (load ? (rs != 31u || rt == rt2) : (rs == rt || rs == rt2 || (rs == rn && rn != 31u))) return false;
        if ((address & (bytes - 1u)) != 0) return false;
        if (load) {
            if (wide) {
                // Two 64-bit loads; STXP's cmpxchg16b validates both halves.
                const std::uint64_t low = load_t<std::uint64_t>(address);
                const std::uint64_t high = load_t<std::uint64_t>(address + 8);
                set_operand(ctx, rt, low, true);
                set_operand(ctx, rt2, high, true);
                ctx.excl_address = address;
                ctx.excl_value[0] = low;
                ctx.excl_value[1] = high;
                ctx.excl_size = 16;
            } else {
                const std::uint64_t value = load_t<std::uint64_t>(address);
                set_operand(ctx, rt, value, false);
                set_operand(ctx, rt2, value >> 32, false);
                ctx.excl_address = address;
                ctx.excl_value[0] = value;
                ctx.excl_size = 8;
            }
        } else {
            std::uint32_t status = 1;
            if (ctx.excl_size == bytes && ctx.excl_address == address) {
                if (wide) {
                    std::uint64_t expected[2] = {ctx.excl_value[0], ctx.excl_value[1]};
                    const std::uint64_t desired[2] = {operand(ctx, rt), operand(ctx, rt2)};
                    status = atomic_cas128(address, expected, desired) ? 0 : 1;
                } else {
                    const std::uint64_t desired = (operand(ctx, rt) & 0xffffffffu) | (operand(ctx, rt2) << 32);
                    bool ok = false;
                    atomic_cas(address, 8, ctx.excl_value[0], desired, &ok);
                    status = ok ? 0 : 1;
                }
            }
            clear_reservation(ctx);
            set_operand(ctx, rs, status, false);
        }
        ctx.pc += 4;
        return true;
    }

    const std::uint32_t exclusive_class = word & 0x3fe07c00u;
    // LDXR/LDAXR
    if (exclusive_class == 0x08407c00u && rs == 31u) {
        const unsigned bytes = 1u << ((word >> 30) & 3u);
        if ((address & (bytes - 1u)) != 0) return false;
        const std::uint64_t value = atomic_load(address, bytes);
        set_operand(ctx, rt, value, bytes == 8);
        ctx.excl_address = address;
        ctx.excl_value[0] = value;
        ctx.excl_size = bytes;
        ctx.pc += 4;
        return true;
    }
    // STXR/STLXR
    if (exclusive_class == 0x08007c00u) {
        const unsigned bytes = 1u << ((word >> 30) & 3u);
        if ((address & (bytes - 1u)) != 0) return false;
        std::uint32_t status = 1;
        if (ctx.excl_size == bytes && ctx.excl_address == address) {
            bool ok = false;
            atomic_cas(address, bytes, ctx.excl_value[0] & size_mask(bytes), operand(ctx, rt) & size_mask(bytes), &ok);
            status = ok ? 0 : 1;
        }
        clear_reservation(ctx);
        set_operand(ctx, rs, status, false);
        ctx.pc += 4;
        return true;
    }
    // CAS/CASA/CASL/CASAL (all sizes), CASP/CASPA/CASPL/CASPAL
    const std::uint32_t cas_class = word & 0x3fa07c00u;
    if (cas_class == 0x08a07c00u || cas_class == 0x08207c00u) {
        const bool pair = cas_class == 0x08207c00u;
        if (pair && (word & 0x80000000u) != 0) return false;
        const unsigned bytes = pair ? ((word & 0x40000000u) != 0 ? 8u : 4u) : 1u << ((word >> 30) & 3u);
        if ((address & (bytes * (pair ? 2u : 1u) - 1u)) != 0) return false;
        if (pair && ((rs | rt) & 1u) != 0) return false;
        const bool wide = bytes == 8;
        const std::uint64_t mask = size_mask(bytes);
        if (!pair) {
            const std::uint64_t old = atomic_cas(address, bytes, operand(ctx, rs) & mask, operand(ctx, rt) & mask);
            set_operand(ctx, rs, old, wide);
        } else if (!wide) {
            const std::uint64_t expected = (operand(ctx, rs) & mask) | (operand(ctx, rs + 1) << 32);
            const std::uint64_t desired = (operand(ctx, rt) & mask) | (operand(ctx, rt + 1) << 32);
            const std::uint64_t old = atomic_cas(address, 8, expected, desired);
            set_operand(ctx, rs, old, false);
            set_operand(ctx, rs + 1, old >> 32, false);
        } else {
            std::uint64_t expected[2] = {operand(ctx, rs), operand(ctx, rs + 1)};
            const std::uint64_t desired[2] = {operand(ctx, rt), operand(ctx, rt + 1)};
            atomic_cas128(address, expected, desired);
            set_operand(ctx, rs, expected[0], true);
            set_operand(ctx, rs + 1, expected[1], true);
        }
        ctx.pc += 4;
        return true;
    }
    // LDADD/LDCLR/LDEOR/LDSET/LDSMAX/LDSMIN/LDUMAX/LDUMIN/SWP (and ST* aliases)
    const std::uint32_t rmw_class = word & 0x3f208c00u;
    const bool swap = rmw_class == 0x38208000u && (word & 0x00007000u) == 0;
    if (rmw_class == 0x38200000u || swap) {
        const unsigned bytes = 1u << ((word >> 30) & 3u);
        if ((address & (bytes - 1u)) != 0) return false;
        const unsigned op = swap ? 8u : (word >> 12) & 7u;
        const std::uint64_t old = atomic_rmw(address, bytes, op, operand(ctx, rs) & size_mask(bytes));
        set_operand(ctx, rt, old, bytes == 8);
        ctx.pc += 4;
        return true;
    }
    // CLREX
    if ((word & 0xfffff0ffu) == 0xd503305fu) {
        clear_reservation(ctx);
        ctx.pc += 4;
        return true;
    }
    return false;
}

} // namespace

void context_to_cpu_state(const GuestContext& ctx, CpuState& state) {
    for (unsigned i = 0; i < 31; ++i) state.x[i] = ctx.x[i];
    state.sp = ctx.x[31];
    state.pc = ctx.pc;
    state.nzcv = static_cast<std::uint32_t>(nzcv_value(ctx)) & 0xf0000000u;   // also a region exit's raw form
    state.fpcr = ctx.fpcr;
    state.fpsr = ctx.fpsr;
    for (unsigned i = 0; i < 32; ++i) {
        state.v[i][0] = ctx.v[i][0];
        state.v[i][1] = ctx.v[i][1];
    }
}

void cpu_state_to_context(const CpuState& state, GuestContext& ctx) {
    for (unsigned i = 0; i < 31; ++i) ctx.x[i] = state.x[i];
    ctx.x[31] = state.sp;
    ctx.pc = state.pc;
    ctx.nzcv = state.nzcv & 0xf0000000u;
    ctx.fpcr = state.fpcr;
    ctx.fpsr = state.fpsr;
    for (unsigned i = 0; i < 32; ++i) {
        ctx.v[i][0] = state.v[i][0];
        ctx.v[i][1] = state.v[i][1];
    }
}

bool interpret_instruction(GuestContext& ctx, std::uint32_t word) {
    if (interpret_atomic(ctx, word)) return false;
    CpuState state{};
    context_to_cpu_state(ctx, state);
    const bool exit = engine().step_external(state, word);
    cpu_state_to_context(state, ctx);
    return exit;
}

bool interpret_one(GuestContext& ctx) {
    std::uint32_t word;
    std::memcpy(&word, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(ctx.pc)), 4);
    return interpret_instruction(ctx, word);
}

} // namespace transmute::fx
