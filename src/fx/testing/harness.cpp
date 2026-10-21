#include "fx/testing/harness.hpp"

#include "fx/common/interpreter.hpp"
#include "fx/testing/test.hpp"

#include <windows.h>

#include <cstring>
#include <sstream>
#include <stdexcept>

namespace transmute::fx::testing {

using x64::Gp;
using x64::Xmm;
using x64::ptr;

namespace {

constexpr Gp kGprPool[] = {Gp::rax, Gp::rcx, Gp::rdx, Gp::rbx, Gp::rbp, Gp::rsi, Gp::rdi,
                           Gp::r8, Gp::r9, Gp::r10, Gp::r11, Gp::r12, Gp::r13, Gp::r14};
constexpr Gp kCtx = Gp::r15;

x64::Mem ctx_field(std::size_t offset) { return ptr(kCtx, ctx_disp(offset)); }

constexpr x64::Cond kSubConditions[14] = {
    x64::Cond::e, x64::Cond::ne, x64::Cond::ae, x64::Cond::b, x64::Cond::s, x64::Cond::ns,
    x64::Cond::o, x64::Cond::no, x64::Cond::a, x64::Cond::be, x64::Cond::ge, x64::Cond::l,
    x64::Cond::g, x64::Cond::le,
};

using EntryFn = void (*)(void* biased_context, const void* code, std::uint32_t mxcsr);

EntryFn entry_trampoline() {
    static EntryFn entry = [] {
        x64::Assembler a;
        const Gp saved[] = {Gp::rbx, Gp::rbp, Gp::rdi, Gp::rsi, Gp::r12, Gp::r13, Gp::r14, Gp::r15};
        for (auto r : saved) a.push(r);
        // [rsp, +32) shadow, +32 host MXCSR, +40 guest MXCSR, +48 xmm6-15
        // (callee-saved on Win64). 8 pushes + return address leave rsp at
        // 8 mod 16, so 216 restores 16-byte alignment.
        a.sub(64, Gp::rsp, 216);
        for (unsigned i = 0; i < 10; ++i) a.movdqu(ptr(Gp::rsp, 48 + 16 * static_cast<std::int32_t>(i)), x64::xmm(6 + i));
        a.stmxcsr(ptr(Gp::rsp, 32));
        a.mov(32, ptr(Gp::rsp, 40), Gp::r8);
        a.ldmxcsr(ptr(Gp::rsp, 40));
        a.mov(64, Gp::r15, Gp::rcx);
        a.call(Gp::rdx);
        a.ldmxcsr(ptr(Gp::rsp, 32));
        for (unsigned i = 0; i < 10; ++i) a.movdqu(x64::xmm(6 + i), ptr(Gp::rsp, 48 + 16 * static_cast<std::int32_t>(i)));
        a.add(64, Gp::rsp, 216);
        for (int i = 7; i >= 0; --i) a.pop(saved[i]);
        a.ret();
        a.finalize();
        auto* memory = static_cast<std::uint8_t*>(VirtualAlloc(
            nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!memory) throw std::runtime_error("cannot allocate test trampoline");
        std::memcpy(memory, a.data(), a.size());
        FlushInstructionCache(GetCurrentProcess(), memory, a.size());
        return reinterpret_cast<EntryFn>(memory);
    }();
    return entry;
}

} // namespace

// ---- MemoryEmitContext ------------------------------------------------------

MemoryEmitContext::MemoryEmitContext(x64::Assembler& assembler, std::uint64_t pc)
    : as_(assembler), pc_(pc) {}

Gp MemoryEmitContext::allocate_gpr() {
    if (next_gpr_ >= std::size(kGprPool)) throw std::logic_error("MemoryEmitContext: out of GPR temps");
    return kGprPool[next_gpr_++];
}

Xmm MemoryEmitContext::allocate_vec() {
    if (next_vec_ >= 16) throw std::logic_error("MemoryEmitContext: out of XMM temps");
    return x64::xmm(next_vec_++);
}

Gp MemoryEmitContext::read_gpr(unsigned index, bool sp) {
    const auto reg = allocate_gpr();
    if (index == 31 && !sp) as_.mov_imm(reg, 0);
    else as_.mov(64, reg, ptr(kCtx, x_disp(index)));
    return reg;
}

Gp MemoryEmitContext::write_gpr(unsigned index, bool sp) {
    const auto reg = allocate_gpr();
    if (index != 31 || sp) pending_gpr_.push_back({index, reg});
    return reg;
}

Gp MemoryEmitContext::copy_gpr(unsigned index, bool sp) { return read_gpr(index, sp); }

Gp MemoryEmitContext::temp_gpr() { return allocate_gpr(); }

Xmm MemoryEmitContext::read_vec(unsigned index) {
    const auto reg = allocate_vec();
    as_.movdqu(reg, ptr(kCtx, v_disp(index)));
    return reg;
}

Xmm MemoryEmitContext::write_vec(unsigned index) {
    const auto reg = allocate_vec();
    pending_vec_.push_back({index, reg});
    return reg;
}

Xmm MemoryEmitContext::temp_vec() { return allocate_vec(); }

void MemoryEmitContext::set_flags(FlagsProducer producer) {
    const auto flags = allocate_gpr();
    const auto result = allocate_gpr();
    const auto bit = allocate_gpr();
    as_.pushfq();
    as_.pop(flags);
    // N: SF (bit 7) -> 31; Z: ZF (bit 6) -> 30
    as_.mov(32, result, flags);
    as_.and_(32, result, 0x80);
    as_.shl(32, result, 24);
    as_.mov(32, bit, flags);
    as_.and_(32, bit, 0x40);
    as_.shl(32, bit, 24);
    as_.or_(32, result, bit);
    if (producer != FlagsProducer::logic) {
        // C: CF (bit 0) -> 29, inverted for subtraction.
        as_.mov(32, bit, flags);
        as_.and_(32, bit, 1);
        if (producer == FlagsProducer::sub) as_.xor_(32, bit, 1);
        as_.shl(32, bit, 29);
        as_.or_(32, result, bit);
        // V: OF (bit 11) -> 28
        as_.mov(32, bit, flags);
        as_.and_(32, bit, 0x800);
        as_.shl(32, bit, 17);
        as_.or_(32, result, bit);
    }
    as_.mov(64, ctx_field(offsetof(GuestContext, nzcv)), result);
}

void MemoryEmitContext::set_nzcv(Gp value) {
    as_.mov(64, ctx_field(offsetof(GuestContext, nzcv)), value);
}

Gp MemoryEmitContext::get_nzcv() {
    const auto reg = allocate_gpr();
    as_.mov(32, reg, ctx_field(offsetof(GuestContext, nzcv)));
    return reg;
}

x64::Cond MemoryEmitContext::condition(unsigned arm_condition) {
    if (arm_condition >= 14) throw std::logic_error("condition(): AL/NV must be handled by the emitter");
    const auto nzcv = allocate_gpr();
    const auto image = allocate_gpr();
    const auto bit = allocate_gpr();
    as_.mov(32, nzcv, ctx_field(offsetof(GuestContext, nzcv)));
    as_.mov_imm(image, 2);                 // reserved bit 1
    as_.mov(32, bit, nzcv);                // N -> SF(7), Z -> ZF(6)
    as_.shr(32, bit, 24);
    as_.and_(32, bit, 0xc0);
    as_.or_(32, image, bit);
    as_.mov(32, bit, nzcv);                // C -> CF(0), inverted (sub convention)
    as_.shr(32, bit, 29);
    as_.and_(32, bit, 1);
    as_.xor_(32, bit, 1);
    as_.or_(32, image, bit);
    as_.mov(32, bit, nzcv);                // V -> OF(11)
    as_.shr(32, bit, 17);
    as_.and_(32, bit, 0x800);
    as_.or_(32, image, bit);
    as_.push(image);
    as_.popfq();
    return kSubConditions[arm_condition];
}

Gp MemoryEmitContext::call_helper(unsigned id, std::initializer_list<HelperArg> args,
                                  HelperEffects) {
    if (!pending_gpr_.empty() || !pending_vec_.empty())
        throw std::logic_error("call_helper after write_gpr/write_vec in the same instruction");
    if (args.size() > 3) throw std::logic_error("call_helper supports at most 3 arguments");
    unsigned slot = 0;
    for (const auto& arg : args) {   // save host-register arguments first
        if (arg.kind == HelperArg::Kind::host) as_.mov(64, ptr(kCtx, spill_disp(slot)), arg.reg);
        ++slot;
    }
    slot = 0;
    for (const auto& arg : args) {
        if (arg.kind == HelperArg::Kind::immediate) {
            as_.mov_imm(Gp::r11, arg.value);
            as_.mov(64, ptr(kCtx, spill_disp(slot)), Gp::r11);
        } else if (arg.kind == HelperArg::Kind::guest_gpr) {
            if (arg.value == 31) as_.mov_imm(Gp::r11, 0);
            else as_.mov(64, Gp::r11, ptr(kCtx, x_disp(static_cast<unsigned>(arg.value))));
            as_.mov(64, ptr(kCtx, spill_disp(slot)), Gp::r11);
        }
        ++slot;
    }
    const Gp arg_regs[] = {Gp::rdx, Gp::r8, Gp::r9};
    for (unsigned i = 0; i < args.size(); ++i) as_.mov(64, arg_regs[i], ptr(kCtx, spill_disp(i)));
    as_.lea(64, Gp::rcx, ptr(kCtx, -kContextBias));
    as_.call(ptr(kCtx, helper_disp(id)));
    next_gpr_ = 0;
    next_vec_ = 0;
    const auto result = allocate_gpr();   // rax
    if (result != Gp::rax) as_.mov(64, result, Gp::rax);
    return result;
}

void MemoryEmitContext::end_instruction() {
    for (const auto& w : pending_gpr_) as_.mov(64, ptr(kCtx, x_disp(w.guest)), w.host);
    for (const auto& w : pending_vec_) as_.movdqu(ptr(kCtx, v_disp(w.guest)), w.host);
    pending_gpr_.clear();
    pending_vec_.clear();
}

void MemoryEmitContext::begin_instruction(std::uint64_t pc) {
    pc_ = pc;
    next_gpr_ = 0;
    next_vec_ = 0;
    pending_gpr_.clear();
    pending_vec_.clear();
}

// ---- Snippet ---------------------------------------------------------------

Snippet::Snippet(const std::vector<std::uint8_t>& code) : size_(code.size()) {
    code_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size_ + 16, MEM_COMMIT | MEM_RESERVE,
                                                    PAGE_EXECUTE_READWRITE));
    if (!code_) throw std::runtime_error("cannot allocate snippet memory");
    std::memcpy(code_, code.data(), size_);
    FlushInstructionCache(GetCurrentProcess(), code_, size_);
}

Snippet::~Snippet() { if (code_) VirtualFree(code_, 0, MEM_RELEASE); }

void Snippet::run(GuestContext& ctx, std::uint32_t mxcsr) const {
    entry_trampoline()(reinterpret_cast<char*>(&ctx) + kContextBias, code_, mxcsr);
}

bool run_family(GuestContext& ctx, FamilyEmitFn family, std::span<const std::uint32_t> words,
                std::uint32_t mxcsr) {
    x64::Assembler a;
    a.sub(64, Gp::rsp, 40);   // keep rsp 16-aligned with shadow space inside the snippet
    MemoryEmitContext emit(a, ctx.pc);
    std::size_t index = 0;
    while (index < words.size()) {
        emit.begin_instruction(ctx.pc + index * 4);
        const auto consumed = family(emit, words.subspan(index));
        if (consumed == 0) {
            std::ostringstream message;
            message << "family declined word 0x" << std::hex << words[index];
            report_failure(__FILE__, __LINE__, message.str());
            return false;
        }
        emit.end_instruction();
        index += consumed;
    }
    a.add(64, Gp::rsp, 40);
    a.ret();
    a.finalize();
    Snippet snippet(a.bytes());
    snippet.run(ctx, mxcsr);
    ctx.pc += words.size() * 4;
    return true;
}

bool oracle_step(GuestContext& ctx, std::uint32_t word) {
    try {
        interpret_instruction(ctx, word);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::uint64_t random_u64(std::uint64_t& state) {
    state += 0x9e3779b97f4a7c15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

GuestContext random_context(std::uint64_t seed) {
    GuestContext ctx{};
    std::uint64_t state = seed * 0x2545f4914f6cdd1dULL + 1;
    static constexpr std::uint64_t specials[] = {
        0, 1, 2, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000ULL,
        0xffffffffULL, 0x100000000ULL, 0x7fffffffffffffffULL, 0x8000000000000000ULL,
        0xffffffffffffffffULL, 0xfffffffffffffffeULL,
    };
    for (unsigned i = 0; i < 32; ++i) {
        const auto r = random_u64(state);
        switch (r % 4) {
        case 0: ctx.x[i] = specials[random_u64(state) % std::size(specials)]; break;
        case 1: ctx.x[i] = random_u64(state) & 0xffff; break;
        default: ctx.x[i] = random_u64(state); break;
        }
    }
    for (unsigned i = 0; i < 32; ++i) {
        ctx.v[i][0] = random_u64(state);
        ctx.v[i][1] = random_u64(state);
    }
    ctx.nzcv = (random_u64(state) & 0xf) << 28;
    ctx.pc = 0x0000'0001'4000'1000ULL;
    return ctx;
}

bool compare_contexts(const GuestContext& expected, const GuestContext& actual,
                      const std::string& label, const CompareOptions& options) {
    bool equal = true;
    const auto fail = [&](const std::string& what, std::uint64_t e, std::uint64_t a) {
        std::ostringstream message;
        message << label << ": " << what << " expected 0x" << std::hex << e << " got 0x" << a;
        report_failure(__FILE__, __LINE__, message.str());
        equal = false;
    };
    for (unsigned i = 0; i < 32; ++i)
        if (expected.x[i] != actual.x[i]) fail(i == 31 ? "sp" : "x" + std::to_string(i), expected.x[i], actual.x[i]);
    if (expected.pc != actual.pc) fail("pc", expected.pc, actual.pc);
    if (options.compare_flags && (expected.nzcv & 0xf0000000u) != (actual.nzcv & 0xf0000000u))
        fail("nzcv", expected.nzcv & 0xf0000000u, actual.nzcv & 0xf0000000u);
    if (options.compare_vectors) {
        for (unsigned i = 0; i < 32; ++i) {
            if (expected.v[i][0] != actual.v[i][0]) fail("v" + std::to_string(i) + ".lo", expected.v[i][0], actual.v[i][0]);
            if (expected.v[i][1] != actual.v[i][1]) fail("v" + std::to_string(i) + ".hi", expected.v[i][1], actual.v[i][1]);
        }
    }
    if (options.compare_fpsr && expected.fpsr != actual.fpsr) fail("fpsr", expected.fpsr, actual.fpsr);
    return equal;
}

Buffer::Buffer(std::size_t size) : size_(size) {
    data_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!data_) throw std::runtime_error("cannot allocate test buffer");
}

Buffer::~Buffer() { if (data_) VirtualFree(data_, 0, MEM_RELEASE); }

} // namespace transmute::fx::testing
