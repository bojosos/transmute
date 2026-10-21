// Virtual-unwind and unwind-code tests over hand-built .xdata and packed
// .pdata, including unwinding from every instruction offset of a function.

#include "fx/seh/seh.hpp"
#include "fx/seh/tests/image_builder.hpp"
#include "fx/seh/tests/seh_test_util.hpp"
#include "fx/testing/test.hpp"

#include <cstring>
#include <vector>

using namespace transmute::fx;
using namespace transmute::fx::seh;
namespace t = fxseh_test;

namespace {

using t::ImageBuilder;
using t::build_xdata;
using t::build_pdata;
using t::read_entry;

// The canonical frame-chained function from the ARM64 ABI docs (Example 2):
//   stp x19,x20,[sp,#-0x10]!      save_r19r20_x (0x22)
//   stp x29,x30,[sp,#-0x90]!      save_fplr_x   (0x91)
//   mov x29,sp                    set_fp        (0xe1)
//   ... body (nop) ...
//   mov sp,x29                    set_fp        (0xe1)
//   ldp x29,x30,[sp],#0x90        save_fplr_x   (0x91)
//   ldp x19,x20,[sp],#0x10        save_r19r20_x (0x22)
//   ret                           end           (0xe4)
// Codes (body-first): e1, 91, 22, e4.
struct Canonical {
    ImageBuilder image;
    std::uint64_t func = 0;
    std::uint64_t pdata = 0;
    std::uint32_t begin_rva = 0;
    static constexpr std::uint32_t kInstrs = 8;
    static constexpr std::uint32_t kEpilogStartInstr = 4;
};

Canonical build_canonical(std::uint32_t handler_rva = 0, const std::vector<std::uint32_t>& handler_data = {}) {
    Canonical c;
    std::vector<std::uint32_t> words(Canonical::kInstrs, t::a64::nop());
    c.func = c.image.put(words.data(), words.size() * 4, 16);
    c.begin_rva = c.image.rva(c.func);
    const std::vector<std::uint8_t> codes = {0xe1, 0x91, 0x22, 0xe4};
    const std::uint64_t xdata =
        build_xdata(c.image, Canonical::kInstrs, codes, /*epilog_in_header=*/true,
                    /*epilog_field=*/0, {}, handler_rva, handler_data);
    c.pdata = build_pdata(c.image, c.begin_rva, c.image.rva(xdata));
    return c;
}

// Sets up a stack reflecting the fully-executed prolog and returns {S0, sp1, sp2}.
struct StackLayout {
    std::uint64_t s0, sp1, sp2;
};
StackLayout build_frame(std::uint64_t x19, std::uint64_t x20, std::uint64_t fp, std::uint64_t lr) {
    const std::uint64_t region = t::alloc_guest(0x1000, 0x1000);
    const std::uint64_t s0 = region + 0x800;
    const std::uint64_t sp1 = s0 - 0x10;
    const std::uint64_t sp2 = sp1 - 0x90;
    auto store = [](std::uint64_t at, std::uint64_t v) {
        std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(at)), &v, 8);
    };
    store(sp1, x19);
    store(sp1 + 8, x20);
    store(sp2, fp);
    store(sp2 + 8, lr);
    return {s0, sp1, sp2};
}

} // namespace

FX_TEST(unwind_code_sizes) {
    FX_CHECK_EQ(unwind_code_size(0x00), 1u);  // alloc_s
    FX_CHECK_EQ(unwind_code_size(0x22), 1u);  // save_r19r20_x
    FX_CHECK_EQ(unwind_code_size(0x91), 1u);  // save_fplr_x
    FX_CHECK_EQ(unwind_code_size(0xc8), 2u);  // save_regp
    FX_CHECK_EQ(unwind_code_size(0xe0), 4u);  // alloc_l
    FX_CHECK_EQ(unwind_code_size(0xe1), 1u);  // set_fp
    FX_CHECK_EQ(unwind_code_size(0xe2), 2u);  // add_fp
    FX_CHECK_EQ(unwind_code_size(0xe7), 3u);  // save_any_reg
    FX_CHECK_EQ(unwind_code_size(0xfc), 1u);  // pac_sign_lr
}

FX_TEST(virtual_unwind_body) {
    t::reset_all();
    Canonical c = build_canonical();
    const std::uint64_t A = 0x1111, B = 0x2222, F = 0x3333'0000, L = c.func + 0x40;
    StackLayout s = build_frame(A, B, F, L);

    Arm64Context ctx{};
    ctx.ContextFlags = kCtxFull;
    ctx.Sp = s.sp2;
    ctx.Fp = s.sp2;
    ctx.Lr = L;
    ctx.Pc = c.func + 0x0c;  // body (after prolog)

    Arm64RuntimeFunction entry = read_entry(c.pdata);
    VirtualUnwindResult r = virtual_unwind(kHandlerTypeNone, c.image.base, ctx.Pc, entry, ctx, nullptr);
    FX_CHECK_EQ(static_cast<int>(r.status), static_cast<int>(UnwindStatus::ok));
    FX_CHECK_EQ(ctx.Sp, s.s0);
    FX_CHECK_EQ(ctx.Pc, L);
    FX_CHECK_EQ(ctx.X[19], A);
    FX_CHECK_EQ(ctx.X[20], B);
    FX_CHECK_EQ(ctx.Fp, F);
    FX_CHECK((ctx.ContextFlags & kCtxUnwoundToCall) != 0);
}

FX_TEST(virtual_unwind_in_prolog) {
    t::reset_all();
    Canonical c = build_canonical();
    const std::uint64_t A = 0xAAAA, B = 0xBBBB, F = 0xCCCC'0000, L = c.func + 0x40;

    // offset 0: nothing done yet -> caller == current.
    {
        StackLayout s = build_frame(A, B, F, L);
        Arm64Context ctx{};
        ctx.ContextFlags = kCtxFull;
        ctx.Sp = s.s0; ctx.Lr = L; ctx.Pc = c.func + 0x00;
        Arm64RuntimeFunction entry = read_entry(c.pdata);
        VirtualUnwindResult r = virtual_unwind(kHandlerTypeNone, c.image.base, ctx.Pc, entry, ctx, nullptr);
        FX_CHECK_EQ(static_cast<int>(r.status), static_cast<int>(UnwindStatus::ok));
        FX_CHECK(r.in_prolog);
        FX_CHECK_EQ(ctx.Sp, s.s0);
        FX_CHECK_EQ(ctx.Pc, L);
    }
    // offset 1 instruction (after stp x19,x20): sp = sp1.
    {
        StackLayout s = build_frame(A, B, F, L);
        Arm64Context ctx{};
        ctx.ContextFlags = kCtxFull;
        ctx.Sp = s.sp1; ctx.Lr = L; ctx.Pc = c.func + 0x04;
        Arm64RuntimeFunction entry = read_entry(c.pdata);
        VirtualUnwindResult r = virtual_unwind(kHandlerTypeNone, c.image.base, ctx.Pc, entry, ctx, nullptr);
        FX_CHECK_EQ(static_cast<int>(r.status), static_cast<int>(UnwindStatus::ok));
        FX_CHECK_EQ(ctx.Sp, s.s0);
        FX_CHECK_EQ(ctx.X[19], A);
        FX_CHECK_EQ(ctx.X[20], B);
    }
    // offset 2 instructions (after stp x29,x30): sp = sp2, fp not yet set.
    {
        StackLayout s = build_frame(A, B, F, L);
        Arm64Context ctx{};
        ctx.ContextFlags = kCtxFull;
        ctx.Sp = s.sp2; ctx.Lr = 0; ctx.Pc = c.func + 0x08;
        Arm64RuntimeFunction entry = read_entry(c.pdata);
        VirtualUnwindResult r = virtual_unwind(kHandlerTypeNone, c.image.base, ctx.Pc, entry, ctx, nullptr);
        FX_CHECK_EQ(static_cast<int>(r.status), static_cast<int>(UnwindStatus::ok));
        FX_CHECK_EQ(ctx.Sp, s.s0);
        FX_CHECK_EQ(ctx.Pc, L);
        FX_CHECK_EQ(ctx.Fp, F);
    }
}

FX_TEST(virtual_unwind_in_epilog) {
    t::reset_all();
    Canonical c = build_canonical();
    const std::uint64_t A = 0xA1, B = 0xB2, F = 0xF3'0000, L = c.func + 0x40;
    const std::uint64_t epilog = c.func + Canonical::kEpilogStartInstr * 4;  // off 0x10

    // Epilog offset 0: state still fully built (sp=sp2, fp=sp2).
    {
        StackLayout s = build_frame(A, B, F, L);
        Arm64Context ctx{};
        ctx.ContextFlags = kCtxFull;
        ctx.Sp = s.sp2; ctx.Fp = s.sp2; ctx.Lr = L; ctx.Pc = epilog + 0x00;
        Arm64RuntimeFunction entry = read_entry(c.pdata);
        VirtualUnwindResult r = virtual_unwind(kHandlerTypeNone, c.image.base, ctx.Pc, entry, ctx, nullptr);
        FX_CHECK_EQ(static_cast<int>(r.status), static_cast<int>(UnwindStatus::ok));
        FX_CHECK(r.in_epilog);
        FX_CHECK_EQ(ctx.Sp, s.s0);
        FX_CHECK_EQ(ctx.X[19], A);
    }
    // Epilog offset 2 instructions (after ldp x29,x30): sp=sp1.
    {
        StackLayout s = build_frame(A, B, F, L);
        Arm64Context ctx{};
        ctx.ContextFlags = kCtxFull;
        ctx.Sp = s.sp1; ctx.Lr = L; ctx.Pc = epilog + 0x08;
        Arm64RuntimeFunction entry = read_entry(c.pdata);
        VirtualUnwindResult r = virtual_unwind(kHandlerTypeNone, c.image.base, ctx.Pc, entry, ctx, nullptr);
        FX_CHECK_EQ(static_cast<int>(r.status), static_cast<int>(UnwindStatus::ok));
        FX_CHECK_EQ(ctx.Sp, s.s0);
        FX_CHECK_EQ(ctx.X[19], A);
        FX_CHECK_EQ(ctx.X[20], B);
    }
    // Epilog offset 3 instructions (after ldp x19,x20): sp=S0 already.
    {
        StackLayout s = build_frame(A, B, F, L);
        Arm64Context ctx{};
        ctx.ContextFlags = kCtxFull;
        ctx.Sp = s.s0; ctx.Lr = L; ctx.Pc = epilog + 0x0c;
        Arm64RuntimeFunction entry = read_entry(c.pdata);
        VirtualUnwindResult r = virtual_unwind(kHandlerTypeNone, c.image.base, ctx.Pc, entry, ctx, nullptr);
        FX_CHECK_EQ(static_cast<int>(r.status), static_cast<int>(UnwindStatus::ok));
        FX_CHECK_EQ(ctx.Sp, s.s0);
    }
}

FX_TEST(virtual_unwind_every_offset_consistent) {
    t::reset_all();
    Canonical c = build_canonical();
    const std::uint64_t A = 0x55, B = 0x66, F = 0x77'0000, L = c.func + 0x40;
    Arm64RuntimeFunction entry = read_entry(c.pdata);
    // Walk every instruction; the recovered caller SP must always be S0 when
    // we seed the SP/FP that the partially executed prolog/epilog would have.
    for (std::uint32_t instr = 0; instr < Canonical::kInstrs; ++instr) {
        StackLayout s = build_frame(A, B, F, L);
        std::uint64_t sp = s.sp2, fp = 0;
        if (instr == 0) sp = s.s0;
        else if (instr == 1) sp = s.sp1;
        else { sp = s.sp2; fp = (instr >= 3) ? s.sp2 : 0; }
        if (instr >= Canonical::kEpilogStartInstr) {
            const std::uint32_t e = instr - Canonical::kEpilogStartInstr;
            if (e == 0) { sp = s.sp2; fp = s.sp2; }
            else if (e == 1) { sp = s.sp2; fp = s.sp2; }
            else if (e == 2) sp = s.sp1;
            else sp = s.s0;
        }
        Arm64Context ctx{};
        ctx.ContextFlags = kCtxFull;
        ctx.Sp = sp; ctx.Fp = fp; ctx.Lr = L; ctx.Pc = c.func + instr * 4;
        VirtualUnwindResult r = virtual_unwind(kHandlerTypeNone, c.image.base, ctx.Pc, entry, ctx, nullptr);
        FX_CHECK_EQ(static_cast<int>(r.status), static_cast<int>(UnwindStatus::ok));
        FX_CHECK_EQ(ctx.Sp, s.s0);
        FX_CHECK_EQ(ctx.Pc, L);
    }
}

FX_TEST(virtual_unwind_reports_handler) {
    t::reset_all();
    Canonical c = build_canonical(/*handler_rva=*/0x1234, {0xdead, 0xbeef});
    const std::uint64_t L = c.func + 0x40;
    StackLayout s = build_frame(1, 2, 3, L);
    Arm64Context ctx{};
    ctx.ContextFlags = kCtxFull;
    ctx.Sp = s.sp2; ctx.Fp = s.sp2; ctx.Lr = L; ctx.Pc = c.func + 0x0c;  // body
    Arm64RuntimeFunction entry = read_entry(c.pdata);
    VirtualUnwindResult r = virtual_unwind(kHandlerTypeException, c.image.base, ctx.Pc, entry, ctx, nullptr);
    FX_CHECK_EQ(static_cast<int>(r.status), static_cast<int>(UnwindStatus::ok));
    FX_CHECK_EQ(r.handler, c.image.base + 0x1234);
    FX_CHECK(r.handler_data != 0);
}

FX_TEST(context_pointers_track_saves) {
    t::reset_all();
    Canonical c = build_canonical();
    const std::uint64_t A = 0x9, B = 0xA, F = 0xB, L = c.func + 0x40;
    StackLayout s = build_frame(A, B, F, L);
    Arm64Context ctx{};
    ctx.ContextFlags = kCtxFull;
    ctx.Sp = s.sp2; ctx.Fp = s.sp2; ctx.Lr = L; ctx.Pc = c.func + 0x0c;
    Arm64ContextPointers pointers{};
    Arm64RuntimeFunction entry = read_entry(c.pdata);
    virtual_unwind(kHandlerTypeNone, c.image.base, ctx.Pc, entry, ctx, &pointers);
    FX_REQUIRE(pointers.X19 != nullptr);
    FX_CHECK_EQ(reinterpret_cast<std::uint64_t>(pointers.X19), s.sp1);
    FX_CHECK_EQ(*pointers.X19, A);
    FX_REQUIRE(pointers.Fp != nullptr);
    FX_CHECK_EQ(*pointers.Fp, F);
}

// ---- packed .pdata ----

FX_TEST(packed_unchained_alloc_only) {
    t::reset_all();
    ImageBuilder image(16 * 1024);
    std::vector<std::uint32_t> words(16, t::a64::nop());
    const std::uint64_t func = image.put(words.data(), words.size() * 4, 16);
    const std::uint32_t begin = image.rva(func);
    // Flag=1, FunctionLength=10, RegF=0, RegI=0, H=0, CR=0, FrameSize=4 (64 bytes).
    Arm64RuntimeFunction entry{};
    entry.BeginAddress = begin;
    entry.Flag = 1;
    entry.FunctionLength = 10;
    entry.FrameSize = 4;  // 64 bytes

    FunctionUnwindInfo info{};
    FX_CHECK_EQ(static_cast<int>(describe_function(image.base, entry, info)), static_cast<int>(UnwindStatus::ok));
    FX_CHECK(info.packed);
    FX_CHECK_EQ(static_cast<int>(validate_function(info)), static_cast<int>(UnwindStatus::ok));

    // Body unwind pops 64 bytes.
    const std::uint64_t region = t::alloc_guest(0x1000, 0x1000);
    const std::uint64_t s0 = region + 0x800;
    Arm64Context ctx{};
    ctx.ContextFlags = kCtxFull;
    ctx.Sp = s0 - 64; ctx.Lr = 0x1234; ctx.Pc = func + 0x20;
    VirtualUnwindResult r = virtual_unwind(kHandlerTypeNone, image.base, ctx.Pc, entry, ctx, nullptr);
    FX_CHECK_EQ(static_cast<int>(r.status), static_cast<int>(UnwindStatus::ok));
    FX_CHECK_EQ(ctx.Sp, s0);
    FX_CHECK_EQ(ctx.Pc, 0x1234u);
}

FX_TEST(packed_chained_frame) {
    t::reset_all();
    ImageBuilder image(16 * 1024);
    std::vector<std::uint32_t> words(20, t::a64::nop());
    const std::uint64_t func = image.put(words.data(), words.size() * 4, 16);
    const std::uint32_t begin = image.rva(func);
    // Flag=1, RegI=2, CR=3 (chained <x29,lr>), FrameSize=4 (64 bytes), H=0, RegF=0.
    Arm64RuntimeFunction entry{};
    entry.BeginAddress = begin;
    entry.Flag = 1;
    entry.FunctionLength = 18;
    entry.RegF = 0;
    entry.RegI = 2;
    entry.H = 0;
    entry.CR = 3;
    entry.FrameSize = 4;  // 64 bytes

    FunctionUnwindInfo info{};
    FX_CHECK_EQ(static_cast<int>(describe_function(image.base, entry, info)), static_cast<int>(UnwindStatus::ok));
    FX_CHECK_EQ(static_cast<int>(validate_function(info)), static_cast<int>(UnwindStatus::ok));
    FX_CHECK(info.epilog_count == 1u);

    // save_size = 2*8 (RegI) rounded to 16 = 16; local = 64-16 = 48.
    // Prolog execution:
    //   stp x19,x20,[sp,#-16]!   -> sp = S0-16
    //   stp x29,lr,[sp,#-48]!    -> sp = S0-64 ; store fp,lr
    //   mov x29,sp
    const std::uint64_t region = t::alloc_guest(0x1000, 0x1000);
    const std::uint64_t s0 = region + 0x800;
    const std::uint64_t sp_regs = s0 - 16;
    const std::uint64_t sp_frame = sp_regs - 48;
    auto store = [](std::uint64_t at, std::uint64_t v) {
        std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(at)), &v, 8);
    };
    const std::uint64_t A = 0x191, B = 0x202, F = 0x29F, L = 0x30C;
    store(sp_regs, A);
    store(sp_regs + 8, B);
    store(sp_frame, F);
    store(sp_frame + 8, L);

    Arm64Context ctx{};
    ctx.ContextFlags = kCtxFull;
    ctx.Sp = sp_frame; ctx.Fp = sp_frame; ctx.Pc = func + 0x20;
    VirtualUnwindResult r = virtual_unwind(kHandlerTypeNone, image.base, ctx.Pc, entry, ctx, nullptr);
    FX_CHECK_EQ(static_cast<int>(r.status), static_cast<int>(UnwindStatus::ok));
    FX_CHECK_EQ(ctx.Sp, s0);
    FX_CHECK_EQ(ctx.Pc, L);
    FX_CHECK_EQ(ctx.X[19], A);
    FX_CHECK_EQ(ctx.X[20], B);
    FX_CHECK_EQ(ctx.Fp, F);
}

FX_TEST(save_next_and_fp_pairs) {
    t::reset_all();
    ImageBuilder image(16 * 1024);
    std::vector<std::uint32_t> words(12, t::a64::nop());
    const std::uint64_t func = image.put(words.data(), words.size() * 4, 16);
    const std::uint32_t begin = image.rva(func);
    // Prolog: sub sp,#48 ; stp x19,x20,[sp] ; stp x21,x22,[sp,#16] (save_next).
    // Unwind order (reverse): save_next (0xe6), save_regp x19 @ [sp,#0]
    // (0xc8,0x00) which then covers x19..x22, alloc_s 48 (0x03), end.
    const std::vector<std::uint8_t> codes = {0xe6, 0xc8, 0x00, 0x03, 0xe4};
    const std::uint64_t xdata = build_xdata(image, 12, codes, false, 0, {});
    const std::uint64_t pdata = build_pdata(image, begin, image.rva(xdata));
    Arm64RuntimeFunction entry = read_entry(pdata);

    FunctionUnwindInfo info{};
    FX_CHECK_EQ(static_cast<int>(describe_function(image.base, entry, info)), static_cast<int>(UnwindStatus::ok));
    FX_CHECK_EQ(static_cast<int>(validate_function(info)), static_cast<int>(UnwindStatus::ok));

    const std::uint64_t region = t::alloc_guest(0x1000, 0x1000);
    const std::uint64_t s0 = region + 0x800;
    const std::uint64_t spv = s0 - 48;
    auto store = [](std::uint64_t at, std::uint64_t v) {
        std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(at)), &v, 8);
    };
    store(spv, 0x19); store(spv + 8, 0x20);
    store(spv + 16, 0x21); store(spv + 24, 0x22);
    Arm64Context ctx{};
    ctx.ContextFlags = kCtxFull;
    ctx.Sp = spv; ctx.Lr = 0xABC; ctx.Pc = func + 0x20;
    VirtualUnwindResult r = virtual_unwind(kHandlerTypeNone, image.base, ctx.Pc, entry, ctx, nullptr);
    FX_CHECK_EQ(static_cast<int>(r.status), static_cast<int>(UnwindStatus::ok));
    FX_CHECK_EQ(ctx.X[19], 0x19u);
    FX_CHECK_EQ(ctx.X[20], 0x20u);
    FX_CHECK_EQ(ctx.X[21], 0x21u);
    FX_CHECK_EQ(ctx.X[22], 0x22u);
    FX_CHECK_EQ(ctx.Sp, s0);
}

FX_TEST(reserved_opcode_rejected) {
    t::reset_all();
    ImageBuilder image(4096);
    std::vector<std::uint32_t> words(4, t::a64::nop());
    const std::uint64_t func = image.put(words.data(), words.size() * 4, 16);
    const std::uint32_t begin = image.rva(func);
    const std::vector<std::uint8_t> codes = {0xed, 0xe4};  // 0xed is reserved
    const std::uint64_t xdata = build_xdata(image, 4, codes, false, 0, {});
    const std::uint64_t pdata = build_pdata(image, begin, image.rva(xdata));
    Arm64RuntimeFunction entry = read_entry(pdata);
    FunctionUnwindInfo info{};
    describe_function(image.base, entry, info);
    FX_CHECK(validate_function(info) != UnwindStatus::ok);
}
