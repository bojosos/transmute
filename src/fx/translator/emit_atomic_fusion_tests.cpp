// LL/SC loop fusion: every fused shape must leave exactly the state the loop
// leaves (registers, flags, memory, reservation), checked against the oracle
// and the unfused translation (owner: atomics/system).
#include "fx/translator/emit_system_test_util.hpp"

#include "fx/testing/test.hpp"

#include <cstring>
#include <string>

using namespace transmute::fx;
using namespace transmute::fx::systest;
using testing::Buffer;

namespace {

struct FusionScope {
    explicit FusionScope(bool on) { system_set_fusion(on); }
    ~FusionScope() { system_set_fusion(true); }
};

unsigned first_span(const std::vector<std::uint32_t>& words) {
    Program p(words);
    return p.family_spans().empty() ? 0u : p.family_spans().front();
}

// Checks oracle == fused == unfused for `words`. `fused_span` is the number
// of words the family must consume at its first call when fusion is on
// (0 = do not check). `unfused` = the mini translator can run the unfused form.
// `min_span`: trailing movs/barrier may be left unconsumed when the worst-case
// register budget would be exceeded (0 = must be exactly fused_span).
void check_shape(const std::vector<std::uint32_t>& words, const GuestContext& ctx, Buffer& buffer,
                 const std::string& label, unsigned fused_span, bool unfused = true, unsigned min_span = 0) {
    {
        FusionScope scope(true);
        differential(words, ctx, buffer, label + " [fused]");
        if (fused_span) {
            const auto span = first_span(words);
            if (span != fused_span && span != min_span) {
                testing::report_failure(__FILE__, __LINE__, label + ": fused span " + std::to_string(span) +
                                        " expected " + std::to_string(fused_span));
            }
        }
    }
    if (unfused) {
        FusionScope scope(false);
        differential(words, ctx, buffer, label + " [unfused]");
        FX_CHECK_EQ(first_span(words), 1u);
    }
}

std::uint32_t logical_imm(unsigned opc, bool x, unsigned n, unsigned immr, unsigned imms, unsigned rd, unsigned rn) {
    return (x ? 0x80000000u : 0u) | (opc << 29) | 0x12000000u | (n << 22) | (immr << 16) | (imms << 10) | (rn << 5) | rd;
}

struct AluCase {
    const char* name;
    std::uint32_t (*make)(bool x, unsigned rd);   // old value in w8/x8, operand in x1
    bool unfused;                                 // mini translator supports it
};

const AluCase kAluCases[] = {
    {"add#1", [](bool x, unsigned rd) { return a64::add_imm(x, rd, 8, 1); }, true},
    {"add#4095<<12", [](bool x, unsigned rd) { return a64::add_imm(x, rd, 8, 4095, true); }, true},
    {"sub#3", [](bool x, unsigned rd) { return a64::sub_imm(x, rd, 8, 3); }, true},
    {"add reg", [](bool x, unsigned rd) { return a64::alu_reg(0x0b, x, rd, 8, 1); }, true},
    {"add reg rev", [](bool x, unsigned rd) { return a64::alu_reg(0x0b, x, rd, 1, 8); }, true},
    {"add lsl3", [](bool x, unsigned rd) { return a64::alu_reg(0x0b, x, rd, 8, 1, 0, 3); }, true},
    {"add lsr5", [](bool x, unsigned rd) { return a64::alu_reg(0x0b, x, rd, 8, 1, 1, 5); }, false},
    {"add asr7", [](bool x, unsigned rd) { return a64::alu_reg(0x0b, x, rd, 8, 1, 2, 7); }, false},
    {"sub reg", [](bool x, unsigned rd) { return a64::alu_reg(0x4b, x, rd, 8, 1); }, true},
    {"rsub reg", [](bool x, unsigned rd) { return a64::alu_reg(0x4b, x, rd, 1, 8); }, true},
    {"and reg", [](bool x, unsigned rd) { return a64::alu_reg(0x0a, x, rd, 8, 1); }, true},
    {"orr reg", [](bool x, unsigned rd) { return a64::alu_reg(0x2a, x, rd, 8, 1); }, true},
    {"eor reg", [](bool x, unsigned rd) { return a64::alu_reg(0x4a, x, rd, 8, 1); }, true},
    {"orr reg rev", [](bool x, unsigned rd) { return a64::alu_reg(0x2a, x, rd, 1, 8); }, true},
    {"eor ror9", [](bool x, unsigned rd) { return a64::alu_reg(0x4a, x, rd, 8, 1, 3, 9); }, false},
    {"bic", [](bool x, unsigned rd) { return a64::alu_reg(0x0a, x, rd, 8, 1) | 0x00200000u; }, false},
    {"orn lsl1", [](bool x, unsigned rd) { return a64::alu_reg(0x2a, x, rd, 8, 1, 0, 1) | 0x00200000u; }, false},
    {"eon", [](bool x, unsigned rd) { return a64::alu_reg(0x4a, x, rd, 8, 1) | 0x00200000u; }, false},
    {"and #0xff", [](bool x, unsigned rd) { return logical_imm(0, x, x ? 1 : 0, 0, 7, rd, 8); }, false},
    {"orr #topbit", [](bool x, unsigned rd) { return logical_imm(1, x, x ? 1 : 0, 1, 0, rd, 8); }, false},
    {"eor #0x55..", [](bool x, unsigned rd) { return logical_imm(2, x, 0, 0, 0x3c, rd, 8); }, false},
    {"mov (orr zr)", [](bool x, unsigned rd) { return a64::alu_reg(0x2a, x, rd, 31, 8); }, true},
};

} // namespace

FX_TEST(fusion_alu_rmw_loops_match_loop_semantics) {
    Buffer buffer(1 << 16);
    std::uint64_t seed = 10;
    const std::vector<std::vector<std::uint32_t>> trailers = {
        {},
        {a64::kDmbIsh},
        {a64::mov_reg(false, 0, 8), a64::kDmbIsh},
        {a64::mov_reg(true, 0, 10), a64::mov_reg(false, 2, 0), a64::kDmbSy},
        {a64::mov_reg(false, 0, 8)},                          // no barrier: not consumed
        {a64::mov_reg(true, 3, 9), a64::kDsbIsh},
        {a64::mov_reg(false, 4, 5), a64::kDmbIshld},          // source not written by the loop
    };
    const unsigned trailer_words[] = {0, 1, 2, 3, 0, 2, 2};
    for (const auto& c : kAluCases) {
        for (unsigned bytes : {1u, 2u, 4u, 8u}) {
            for (bool x : {false, true}) {
                if (bytes == 8 && !x) continue;
                for (unsigned rd : {10u, 8u}) {
                    for (unsigned t = 0; t < trailers.size(); ++t) {
                        if (rd == 8 && t == 3) continue;   // x10 not written
                        fill_random(buffer, seed);
                        auto ctx = make_context(seed++);
                        ctx.x[0] = buffer.address() + 64 + bytes * (seed % 8);
                        std::vector<std::uint32_t> words = {
                            a64::ldxr(bytes, 8, 0, seed & 1), c.make(x, rd),
                            a64::stxr(bytes, 9, rd, 0, seed & 2), a64::cbnz(false, 9, -3)};
                        words.insert(words.end(), trailers[t].begin(), trailers[t].end());
                        words.push_back(a64::add_imm(true, 11, 11, 1));
                        const bool unfused = c.unfused;
                        check_shape(words, ctx, buffer,
                                    std::string("alu ") + c.name + " bytes=" + std::to_string(bytes) +
                                    " x=" + std::to_string(x) + " rd=" + std::to_string(rd) + " t=" + std::to_string(t),
                                    4 + trailer_words[t], unfused, 4);
                    }
                }
            }
        }
    }
}

FX_TEST(fusion_exchange_and_reload_loops) {
    Buffer buffer(1 << 16);
    std::uint64_t seed = 500;
    for (unsigned bytes : {1u, 2u, 4u, 8u}) {
        for (unsigned rv : {1u, 8u, 31u}) {
            for (unsigned t = 0; t < 3; ++t) {
                fill_random(buffer, seed);
                auto ctx = make_context(seed++);
                ctx.x[0] = buffer.address() + 128 + bytes * (seed % 4);
                std::vector<std::uint32_t> words = {a64::ldxr(bytes, 8, 0, true), a64::stxr(bytes, 9, rv, 0, true),
                                                    a64::cbnz(false, 9, -2)};
                if (t == 1) words.push_back(a64::kDmbIsh);
                if (t == 2) { words.push_back(a64::mov_reg(bytes == 8, 0, 8)); words.push_back(a64::kDmbIsh); }
                words.push_back(a64::add_imm(true, 11, 11, 1));
                check_shape(words, ctx, buffer, "xchg bytes=" + std::to_string(bytes) + " rv=" + std::to_string(rv),
                            3 + t);
            }
        }
    }
}

FX_TEST(fusion_compare_exchange_msvc_shape) {
    Buffer buffer(1 << 16);
    std::uint64_t seed = 900;
    enum Kind { reg32, reg64, imm32, imm64, uxt };
    for (unsigned bytes : {1u, 2u, 4u, 8u}) {
        for (Kind kind : {reg32, reg64, imm32, imm64, uxt}) {
            if (bytes == 8 && (kind == reg32 || kind == imm32 || kind == uxt)) continue;
            if (kind == uxt && bytes > 2) continue;
            for (unsigned outcome = 0; outcome < 3; ++outcome) {   // 0 match, 1 mismatch, 2 impossible high bits
                for (bool tail_store : {false, true}) {
                    fill_random(buffer, seed);
                    auto ctx = make_context(seed++);
                    const auto address = buffer.address() + 256 + bytes * (seed % 4);
                    ctx.x[0] = address;
                    std::uint64_t memory = 0;
                    std::memcpy(&memory, reinterpret_cast<void*>(address), bytes);
                    std::uint32_t cmp = 0;
                    const bool x = kind == reg64 || kind == imm64;
                    if (kind == imm32 || kind == imm64) {
                        memory &= 0xfff;
                        std::memcpy(reinterpret_cast<void*>(address), &memory, bytes);
                        unsigned imm = static_cast<unsigned>(memory);
                        if (outcome == 1) imm ^= 1;
                        if (outcome == 2) {
                            if (bytes >= 2) continue;     // a 12-bit immediate cannot exceed 16 bits
                            imm |= 0x100;
                        }
                        cmp = a64::cmp_imm(x, 8, imm);
                    } else if (kind == uxt) {
                        ctx.x[1] = (memory ^ (outcome == 1 ? 1 : 0)) | (ctx.x[1] << 16);   // junk above
                        cmp = a64::cmp_ext(false, 8, 1, bytes == 1 ? 0 : 1);
                        if (outcome == 2) continue;
                    } else {
                        ctx.x[1] = memory ^ (outcome == 1 ? 1 : 0);
                        if (outcome == 2) {
                            if (bytes == 8 || (bytes == 4 && !x)) continue;
                            ctx.x[1] |= 1ULL << (bytes * 8);
                        }
                        cmp = a64::cmp_reg(x, 8, 1);
                    }
                    std::vector<std::uint32_t> words = {
                        a64::ldxr(bytes, 8, 0, true), cmp, a64::bcond(a64::NE, 3),
                        a64::stxr(bytes, 9, 2, 0, true), a64::cbnz(false, 9, -4), a64::kDmbIsh};
                    // A store-exclusive after the loop succeeds exactly when the
                    // compare failed (the reservation is still live).
                    if (tail_store) words.push_back(a64::stxr(bytes, 5, 6, 0));
                    words.push_back(a64::add_imm(true, 11, 11, 1));
                    check_shape(words, ctx, buffer,
                                "cas bytes=" + std::to_string(bytes) + " kind=" + std::to_string(kind) +
                                " outcome=" + std::to_string(outcome) + " tail=" + std::to_string(tail_store),
                                5, kind != uxt);
                }
            }
        }
    }
}

FX_TEST(fusion_compare_exchange_zero_shape) {
    Buffer buffer(1 << 16);
    std::uint64_t seed = 1300;
    for (unsigned bytes : {1u, 2u, 4u, 8u}) {
        for (bool zero : {true, false}) {
            for (bool x : {false, true}) {
                if (bytes == 8 && !x) continue;
                fill_random(buffer, seed);
                auto ctx = make_context(seed++);
                ctx.x[0] = buffer.address() + 64;
                if (zero) std::memset(buffer.data() + 64, 0, 8);
                else buffer.data()[64] |= 1;
                const std::vector<std::uint32_t> words = {
                    a64::ldxr(bytes, 8, 0, true), a64::cbnz(x, 8, 3), a64::stxr(bytes, 9, 2, 0, true),
                    a64::cbnz(false, 9, -3), a64::stxr(bytes, 5, 6, 0), a64::add_imm(true, 11, 11, 1)};
                check_shape(words, ctx, buffer, "cas0 bytes=" + std::to_string(bytes) + " zero=" + std::to_string(zero), 4);
            }
        }
    }
}

FX_TEST(fusion_pair_load_and_exchange) {
    Buffer buffer(1 << 16);
    std::uint64_t seed = 1700;
    for (bool wide : {false, true}) {
        for (bool reload : {true, false}) {
            for (unsigned t = 0; t < 3; ++t) {
                fill_random(buffer, seed);
                auto ctx = make_context(seed++);
                ctx.x[0] = buffer.address() + 512;
                std::vector<std::uint32_t> words = {
                    a64::ldxp(wide, 8, 9, 0, true),
                    reload ? a64::stxp(wide, 10, 8, 9, 0, true) : a64::stxp(wide, 10, 2, 3, 0, true),
                    a64::cbnz(false, 10, -2)};
                if (t == 1) words.push_back(a64::kDmbIsh);
                if (t == 2) { words.push_back(a64::mov_reg(true, 0, 9)); words.push_back(a64::kDmbIsh); }
                words.push_back(a64::add_imm(true, 11, 11, 1));
                // 128-bit exchange is not fused (register budget); only the
                // atomic-load form is.
                check_shape(words, ctx, buffer,
                            "pair wide=" + std::to_string(wide) + " reload=" + std::to_string(reload) + " t=" +
                            std::to_string(t), (wide && !reload) ? 1 : 3 + t, true, (wide && !reload) ? 1 : 3);
            }
        }
    }
}

FX_TEST(fusion_rejects_shapes_it_cannot_prove) {
    Buffer buffer(1 << 16);
    const auto run_unfused_shape = [&](const std::vector<std::uint32_t>& words, const char* label) {
        fill_random(buffer, 42);
        auto ctx = make_context(42);
        ctx.x[0] = buffer.address() + 64;
        ctx.x[3] = buffer.address() + 64;   // same address through another register
        FX_CHECK_EQ(first_span(words), 1u);
        differential(words, ctx, buffer, label);
    };
    // 32-bit op on a 64-bit exclusive (the store writes the zero-extended value).
    run_unfused_shape({a64::ldxr(8, 8, 0), a64::add_imm(false, 9, 8, 1), a64::stxr(8, 10, 9, 0), a64::cbnz(false, 10, -3)},
                      "w op on x load");
    {   // flag-setting op without extended fusion (fusion_flag_setting_bodies covers it)
        ExtendedFusionScope old(false);
        run_unfused_shape({a64::ldxr(4, 8, 0), a64::subs_imm(false, 9, 8, 1), a64::stxr(4, 10, 9, 0),
                           a64::cbnz(false, 10, -3)}, "subs in loop (oldfusion)");
    }
    // SUBS with the loaded value as the second operand (flags of Rm - old)
    run_unfused_shape({a64::ldxr(4, 8, 0), a64::alu_reg(0x6b, false, 9, 1, 8), a64::stxr(4, 10, 9, 0),
                       a64::cbnz(false, 10, -3)}, "rsubs in loop");
    // store to a different base register
    run_unfused_shape({a64::ldxr(4, 8, 0), a64::add_imm(false, 9, 8, 1), a64::stxr(4, 10, 9, 3), a64::cbnz(false, 10, -3)},
                      "other base");
    // size mismatch (never succeeds: not executed)
    FX_CHECK_EQ(first_span({a64::ldxr(4, 8, 0), a64::add_imm(false, 9, 8, 1), a64::stxr(2, 10, 9, 0),
                            a64::cbnz(false, 10, -3)}), 1u);
    // cbnz tests a register other than the status
    run_unfused_shape({a64::ldxr(4, 8, 0), a64::add_imm(false, 9, 8, 1), a64::stxr(4, 10, 9, 0), a64::cbnz(false, 9, 1)},
                      "cbnz other");
    // ALU writes the address register (not executed: the loop would store elsewhere)
    FX_CHECK_EQ(first_span({a64::ldxr(8, 8, 0), a64::add_imm(true, 0, 8, 1), a64::stxr(8, 10, 0, 0),
                            a64::cbnz(false, 10, -3)}), 1u);
    // status register equal to the stored register (constrained unpredictable)
    FX_CHECK_EQ(first_span({a64::ldxr(4, 8, 0), a64::add_imm(false, 9, 8, 1), a64::stxr(4, 9, 9, 0),
                            a64::cbnz(false, 9, -3)}), 1u);
    // operand uses the loaded value twice
    run_unfused_shape({a64::ldxr(4, 8, 0), a64::alu_reg(0x0b, false, 9, 8, 8), a64::stxr(4, 10, 9, 0), a64::cbnz(false, 10, -3)},
                      "old twice");
    // clang's compare-exchange: b.ne leaves the loop to a CLREX, not to the end
    run_unfused_shape({a64::ldxr(4, 8, 0, true), a64::cmp_reg(false, 8, 2), a64::bcond(a64::NE, 5),
                       a64::stxr(4, 9, 1, 0, true), a64::cbnz(false, 9, -4), a64::kDmbIsh, a64::b(2), a64::kClrex},
                      "clang cas");
}

FX_TEST(fusion_compiler_output_shapes) {
    Buffer buffer(1 << 16);
    std::uint64_t seed = 3100;
    struct Shape { const char* name; std::vector<std::uint32_t> words; unsigned first; };
    const Shape shapes[] = {
        // clang-cl -O2 _InterlockedIncrement / _InterlockedIncrement64 / 16
        {"inc32", {0x885ffc08, 0x11000508, 0x8809fc08, 0x35ffffa9, 0x2a0803e0, 0xd5033bbf}, 6},
        {"inc64", {0xc85ffc08, 0x91000508, 0xc809fc08, 0x35ffffa9, 0xaa0803e0, 0xd5033bbf}, 6},
        {"inc16", {0x485ffc08, 0x11000508, 0x4809fc08, 0x35ffffa9, 0x2a0803e0, 0xd5033bbf}, 6},
        // _InterlockedExchangeAdd8: mov x8, x0 precedes the loop
        {"add8", {0xaa0003e8, 0x085ffd00, 0x0b010009, 0x080afd09, 0x35ffffaa, 0xd5033bbf}, 5},
        // _InterlockedExchange16
        {"xchg16", {0xaa0003e8, 0x485ffd00, 0x4809fd01, 0x35ffffc9, 0xd5033bbf}, 4},
        // _InterlockedOr64 / _InterlockedAnd16 (compare-exchange loops)
        {"or64", {0xaa0003e8, 0xc85ffd00, 0xaa010009, 0xc80afd09, 0x35ffffaa, 0xd5033bbf}, 5},
        {"and16", {0xaa0003e8, 0x485ffd00, 0x0a010009, 0x480afd09, 0x35ffffaa, 0xd5033bbf}, 5},
        // MSVC-style _InterlockedCompareExchange (b.ne to the instruction after cbnz)
        {"cas32 msvc", {0x885ffc08, 0x6b02011f, 0x54000061, 0x8809fc01, 0x35ffff89, 0xd5033bbf, 0x2a0803e0}, 5},
    };
    for (const auto& s : shapes) {
        for (int variant = 0; variant < 4; ++variant) {
            fill_random(buffer, seed);
            auto ctx = make_context(seed++);
            ctx.x[0] = buffer.address() + 64 + 8 * variant;
            if (variant == 1) std::memcpy(&ctx.x[2], reinterpret_cast<void*>(ctx.x[0]), 4);   // cas hit
            check_shape(s.words, ctx, buffer, s.name, s.first);
        }
    }
}

// ADDS/SUBS bodies (MSVC _InterlockedDecrement compared with zero: subs w8,
// w8, #1) fuse into lock xadd plus an x64 add/sub that produces NZCV.
FX_TEST(fusion_flag_setting_bodies) {
    Buffer buffer(1 << 16);
    std::uint64_t seed = 4100;
    struct Body { const char* name; std::uint32_t (*make)(bool x, unsigned rd); bool unfused; };
    const Body bodies[] = {
        {"subs#1", [](bool x, unsigned rd) { return a64::subs_imm(x, rd, 8, 1); }, true},
        {"subs#0", [](bool x, unsigned rd) { return a64::subs_imm(x, rd, 8, 0); }, true},
        {"subs#4095<<12", [](bool x, unsigned rd) { return a64::subs_imm(x, rd, 8, 4095) | (1u << 22); }, true},
        {"adds#1", [](bool x, unsigned rd) { return a64::add_imm(x, rd, 8, 1) | 0x20000000u; }, true},
        {"adds#0", [](bool x, unsigned rd) { return a64::add_imm(x, rd, 8, 0) | 0x20000000u; }, true},
        {"adds reg", [](bool x, unsigned rd) { return a64::alu_reg(0x2b, x, rd, 8, 1); }, true},
        {"adds reg rev", [](bool x, unsigned rd) { return a64::alu_reg(0x2b, x, rd, 1, 8); }, true},
        {"subs reg", [](bool x, unsigned rd) { return a64::alu_reg(0x6b, x, rd, 8, 1); }, true},
        {"subs lsl3", [](bool x, unsigned rd) { return a64::alu_reg(0x6b, x, rd, 8, 1, 0, 3); }, true},
        {"adds asr2", [](bool x, unsigned rd) { return a64::alu_reg(0x2b, x, rd, 8, 1, 2, 2); }, false},
    };
    // trailer words, words consumed when they fit
    struct Trailer { std::vector<std::uint32_t> words; unsigned consumed; bool unfused; };
    const Trailer trailers[] = {
        {{}, 0, true},
        {{a64::kDmbIsh}, 1, true},
        {{a64::mov_reg(true, 1, 22), a64::mov_reg(true, 0, 21), a64::kDmbIsh}, 3, true},
        {{a64::mov_reg(false, 0, 8), a64::kDmbIsh}, 2, true},
        {{a64::cmp_imm(false, 8, 0), a64::kDmbIsh}, 2, true},   // a later flag setter
        {{logical_imm(0, false, 0, 0, 30, 4, 8), a64::kDmbIsh}, 0, false},   // AND would clobber NZCV: not consumed
        {{logical_imm(0, false, 0, 0, 30, 4, 8), a64::cmp_reg(false, 4, 1), a64::kDmbIsh}, 3, false},
    };
    for (const auto& body : bodies) {
        for (unsigned bytes : {1u, 2u, 4u, 8u}) {
            for (bool x : {false, true}) {
                if (bytes == 8 && !x) continue;
                for (unsigned rd : {8u, 10u}) {
                    for (const auto& t : trailers) {
                        for (int variant = 0; variant < 2; ++variant) {
                            fill_random(buffer, seed);
                            auto ctx = make_context(seed++);
                            ctx.x[0] = buffer.address() + 64 + bytes * (seed % 8);
                            if (variant == 1) {   // results at the flag boundaries
                                std::memset(reinterpret_cast<void*>(ctx.x[0]), seed & 2 ? 0x00 : 0xff, 8);
                                if (seed & 4) reinterpret_cast<std::uint8_t*>(ctx.x[0])[bytes - 1] = 0x80;
                                ctx.x[1] = seed & 8 ? 1 : 0x7fffffff;
                            }
                            std::vector<std::uint32_t> words = {a64::ldxr(bytes, 8, 0, seed & 1), body.make(x, rd),
                                                                a64::stxr(bytes, 9, rd, 0, seed & 2),
                                                                a64::cbnz(false, 9, -3)};
                            words.insert(words.end(), t.words.begin(), t.words.end());
                            words.push_back(a64::add_imm(true, 11, 11, 1));
                            check_shape(words, ctx, buffer,
                                        std::string("flags ") + body.name + " bytes=" + std::to_string(bytes) +
                                        " x=" + std::to_string(x) + " rd=" + std::to_string(rd) +
                                        " trailer=" + std::to_string(&t - trailers),
                                        4 + t.consumed, body.unfused && t.unfused, 4);
                        }
                    }
                }
            }
        }
    }
    {   // without extended fusion the loop is not fused
        ExtendedFusionScope old(false);
        FX_CHECK_EQ(first_span({a64::ldxr(4, 8, 0), a64::subs_imm(false, 8, 8, 1), a64::stxr(4, 9, 8, 0),
                                a64::cbnz(false, 9, -3)}), 1u);
    }
}

// CMP/CMN/AND/MOV between a fused loop and its barrier (blender.exe: ~6000
// `sub; stlxr; cbnz; cmp w9, #0 [; mov]; dmb ish` sites).
FX_TEST(fusion_trailing_compare_and_mask) {
    Buffer buffer(1 << 16);
    std::uint64_t seed = 5100;
    const std::uint32_t cmn_x9_1 = a64::add_imm(true, 31, 9, 1) | 0x20000000u;
    struct Case { const char* name; std::vector<std::uint32_t> loop; std::vector<std::uint32_t> trailer; unsigned consumed; };
    // ldaxr w9, [x0]; sub w10, w9, #1; stlxr w11, w10, [x0]; cbnz w11
    const std::vector<std::uint32_t> dec = {a64::ldxr(4, 9, 0, true), a64::sub_imm(false, 10, 9, 1),
                                            a64::stxr(4, 11, 10, 0, true), a64::cbnz(false, 11, -3)};
    // ldaxr w8, [x0]; add w10, w8, #1; stlxr w11, w10, [x0]; cbnz w11
    const std::vector<std::uint32_t> inc = {a64::ldxr(4, 8, 0, true), a64::add_imm(false, 10, 8, 1),
                                            a64::stxr(4, 11, 10, 0, true), a64::cbnz(false, 11, -3)};
    // ldaxr x9, [x0]; add x10, x9, #1; stlxr w11, x10, [x0]; cbnz w11
    const std::vector<std::uint32_t> inc64 = {a64::ldxr(8, 9, 0, true), a64::add_imm(true, 10, 9, 1),
                                              a64::stxr(8, 11, 10, 0, true), a64::cbnz(false, 11, -3)};
    // ldaxr w9, [x0]; sub w9, w9, #1; stlxr w10, w9, [x0]; cbnz w10
    const std::vector<std::uint32_t> dec_in = {a64::ldxr(4, 9, 0, true), a64::sub_imm(false, 9, 9, 1),
                                               a64::stxr(4, 10, 9, 0, true), a64::cbnz(false, 10, -3)};
    // ldaxr w8, [x0]; stlxr w9, w2, [x0]; cbnz w9
    const std::vector<std::uint32_t> xchg = {a64::ldxr(4, 8, 0, true), a64::stxr(4, 9, 2, 0, true),
                                             a64::cbnz(false, 9, -2)};
    const Case cases[] = {
        {"dec cmp#0", dec_in, {a64::cmp_imm(false, 9, 0), a64::kDmbIsh}, 2},
        {"dec cmp#0 mov", dec_in, {a64::cmp_imm(false, 9, 0), a64::mov_reg(true, 1, 22), a64::kDmbIsh}, 3},
        {"dec mov cmp#0", dec_in, {a64::mov_reg(true, 22, 23), a64::cmp_imm(false, 9, 0), a64::kDmbIsh}, 3},
        {"dec cmp#1", dec, {a64::cmp_imm(false, 9, 1), a64::kDmbIsh}, 2},
        {"dec and", dec, {logical_imm(0, false, 0, 0, 30, 8, 9), a64::kDmbIsh}, 2},
        {"dec and cmp mov", dec, {logical_imm(0, false, 0, 0, 30, 8, 9), a64::cmp_imm(false, 8, 0),
                                  a64::mov_reg(false, 3, 8), a64::kDmbIsh}, 4},
        {"inc cmp reg", inc, {a64::cmp_reg(false, 8, 23), a64::kDmbIsh}, 2},
        {"inc cmp reg movz", inc, {a64::cmp_reg(false, 8, 23), a64::mov_reg(false, 20, 31), a64::kDmbIsh}, 3},
        {"inc cmp written", inc, {a64::cmp_reg(false, 10, 8), a64::kDmbIsh}, 2},
        {"inc64 cmn#1", inc64, {cmn_x9_1, a64::kDmbIsh}, 2},
        {"inc64 cmp x", inc64, {a64::cmp_imm(true, 10, 4095), a64::kDmbSy}, 2},
        {"xchg cmp#0", xchg, {a64::cmp_imm(false, 8, 0), a64::kDmbIsh}, 2},
        {"cmp status (zero): not consumed", dec, {a64::cmp_imm(false, 11, 0), a64::kDmbIsh}, 0},
        {"no barrier: not consumed", dec, {a64::cmp_imm(false, 9, 0)}, 0},
        {"4 ops: not consumed", dec, {a64::mov_reg(true, 1, 2), a64::mov_reg(true, 3, 4), a64::cmp_imm(false, 9, 0),
                                      a64::mov_reg(true, 5, 6), a64::kDmbIsh}, 0},
    };
    for (const auto& c : cases) {
        for (int variant = 0; variant < 6; ++variant) {
            fill_random(buffer, seed);
            auto ctx = make_context(seed++);
            ctx.x[0] = buffer.address() + 64 + 8 * variant;   // exclusives must be aligned
            auto* p = reinterpret_cast<std::uint32_t*>(ctx.x[0]);
            if (variant == 1) *p = 1;            // decrement to zero
            if (variant == 2) *p = 0;
            if (variant == 3) { *p = 0x7fffffff; ctx.x[23] = 0x7fffffff; }
            if (variant == 4) { std::memset(p, 0xff, 8); ctx.x[23] = 0xffffffff; }
            std::vector<std::uint32_t> words = c.loop;
            words.insert(words.end(), c.trailer.begin(), c.trailer.end());
            words.push_back(a64::add_imm(true, 12, 12, 1));
            const bool has_and = std::string(c.name).find("and") != std::string::npos;
            check_shape(words, ctx, buffer, std::string("trailing ") + c.name + " v=" + std::to_string(variant),
                        static_cast<unsigned>(c.loop.size()) + c.consumed, !has_and,
                        static_cast<unsigned>(c.loop.size()));
        }
    }
    {   // without extended fusion only MOVs are consumed
        ExtendedFusionScope old(false);
        std::vector<std::uint32_t> words = dec_in;
        words.insert(words.end(), {a64::cmp_imm(false, 9, 0), a64::kDmbIsh});
        FX_CHECK_EQ(first_span(words), 4u);
    }
}

// A barrier after an LSE instruction (clang-cl: `ldaddal; [add;] dmb ish`) is
// consumed through register-only trailing ops; the locked instruction is the
// full barrier.
FX_TEST(fusion_lse_consumes_following_barrier) {
    Buffer buffer(1 << 16);
    std::uint64_t seed = 6100;
    struct Op { const char* name; std::uint32_t word; };
    const Op ops[] = {
        {"ldaddal w", a64::lse(0, 4, true, true, 1, 10, 0)},
        {"ldaddal x", a64::lse(0, 8, true, true, 1, 10, 0)},
        {"stadd (rt=zr)", a64::lse(0, 4, false, false, 1, 31, 0)},
        {"ldseth", a64::lse(3, 2, true, false, 1, 10, 0)},
        {"stclr", a64::lse(1, 8, false, true, 1, 31, 0)},
        {"ldsmaxal", a64::lse(4, 4, true, true, 1, 10, 0)},
        {"ldumin b", a64::lse(7, 1, false, false, 1, 10, 0)},
        {"swpal", a64::lse(8, 8, true, true, 1, 10, 0)},
        {"casal w", a64::cas(4, true, true, 10, 1, 0)},
        {"casal x", a64::cas(8, true, true, 10, 1, 0)},
    };
    struct Trailer { std::vector<std::uint32_t> words; unsigned consumed; };
    const Trailer trailers[] = {
        {{}, 0},
        {{a64::kDmbIsh}, 1},
        {{a64::add_imm(true, 8, 8, 1), a64::kDmbIsh}, 2},                         // clang-cl loop counter
        {{a64::sub_imm(false, 12, 10, 4), a64::kDmbSy}, 2},                       // reads the result
        {{a64::cmp_reg(false, 10, 2), a64::mov_reg(true, 3, 10), a64::kDmbIsh}, 3},
        {{a64::add_imm(true, 8, 8, 1)}, 0},                                       // no barrier
    };
    for (const auto& op : ops) {
        for (const auto& t : trailers) {
            for (int variant = 0; variant < 2; ++variant) {
                fill_random(buffer, seed);
                auto ctx = make_context(seed++);
                ctx.x[0] = buffer.address() + 128;
                if (variant == 1) std::memcpy(&ctx.x[10], reinterpret_cast<void*>(ctx.x[0]), 8);   // cas hit
                std::vector<std::uint32_t> words = {op.word};
                words.insert(words.end(), t.words.begin(), t.words.end());
                words.push_back(a64::add_imm(true, 11, 11, 1));
                check_shape(words, ctx, buffer, std::string("lse ") + op.name + " trailer=" +
                            std::to_string(&t - trailers), 1 + t.consumed, true, 1);
            }
        }
    }
}
