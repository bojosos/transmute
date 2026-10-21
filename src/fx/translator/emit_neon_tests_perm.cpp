// Differential tests: moves, permutes, EXT, TBL/TBX and modified immediates
// (owner: neon).
#include "fx/translator/emit_neon_test_util.hpp"

#include <cstdio>
#include <cstdlib>

using namespace transmute::fx;
using namespace transmute::fx::neon_test;

namespace {

void report_declined(const char* label, std::uint32_t word, Outcome outcome) {
    char buffer[112];
    std::snprintf(buffer, sizeof(buffer), "%s 0x%08x: valid encoding declined (oracle %s)", label, word,
                  outcome == Outcome::declined ? "implements it" : "declines too");
    testing::report_failure(__FILE__, __LINE__, buffer);
}

// Random rd/rn/rm (rm only when `with_m`), `seeds` states.
void sweep(std::uint32_t base, unsigned size, unsigned seeds, const char* label, bool expect_valid,
           bool with_m = true, bool with_n = true) {
    std::uint64_t state = base * 0x9e3779b97f4a7c15ULL + 5;
    for (unsigned s = 0; s < seeds; ++s) {
        unsigned rd = static_cast<unsigned>(rnd(state) % 32), rn = static_cast<unsigned>(rnd(state) % 32);
        unsigned rm = static_cast<unsigned>(rnd(state) % 32);
        switch (rnd(state) % 6) {
        case 0: rn = rd; break;
        case 1: rm = rd; break;
        case 2: rm = rn; break;
        default: break;
        }
        std::uint32_t word = base | rd;
        if (with_n) word |= rn << 5;
        if (with_m) word |= rm << 16;
        const auto ctx = special_context(rnd(state), size);
        const auto outcome = check(emit_neon, ctx, word, label);
        if (outcome == Outcome::declined || outcome == Outcome::both_declined) {
            if (expect_valid) report_declined(label, word, outcome);
            return;
        }
        if (outcome != Outcome::ok) return;
    }
}

unsigned imm5_size(unsigned imm5) {
    for (unsigned s = 0; s < 4; ++s)
        if (imm5 & (1u << s)) return s;
    return 4;
}

} // namespace

FX_TEST(neon_copy_group) {
    for (unsigned imm5 = 0; imm5 < 32; ++imm5) {
        const unsigned size = imm5_size(imm5);
        for (unsigned q = 0; q < 2; ++q) {
            const std::uint32_t qb = q << 30, i5 = imm5 << 16;
            sweep(0x0e000400u | qb | i5, size & 3, 4, "dup_element", size < 3 || (size == 3 && q), false);
            sweep(0x0e000c00u | qb | i5, size & 3, 4, "dup_general", size < 3 || (size == 3 && q), false);
            sweep(0x0e002c00u | qb | i5, size & 3, 4, "smov", size < 2 || (size == 2 && q), false);
            sweep(0x0e003c00u | qb | i5, size & 3, 4, "umov", q ? size == 3 : size < 3, false);
        }
        sweep(0x4e001c00u | (imm5 << 16), size & 3, 4, "ins_general", size < 4, false);
        for (unsigned imm4 = 0; imm4 < 16; ++imm4)
            sweep(0x6e000400u | (imm5 << 16) | (imm4 << 11), size & 3, 2, "ins_element", size < 4, false);
        sweep(0x5e000400u | (imm5 << 16), size & 3, 4, "dup_scalar", size < 4, false);
    }
    print_stats("after copy");
}

FX_TEST(neon_permute) {
    for (const unsigned opc : {1u, 2u, 3u, 5u, 6u, 7u})
        for (unsigned size = 0; size < 4; ++size)
            for (unsigned q = 0; q < 2; ++q)
                sweep(0x0e000800u | (q << 30) | (size << 22) | (opc << 12), size, 16, "permute", size != 3 || q);
    print_stats("after permute");
}

FX_TEST(neon_ext) {
    for (unsigned q = 0; q < 2; ++q)
        for (unsigned imm4 = 0; imm4 < 16; ++imm4)
            sweep(0x2e000000u | (q << 30) | (imm4 << 11), 0, 12, "ext", q || imm4 < 8);
    print_stats("after ext");
}

FX_TEST(neon_tbl_tbx) {
    for (unsigned op = 0; op < 2; ++op)
        for (unsigned len = 0; len < 4; ++len)
            for (unsigned q = 0; q < 2; ++q) {
                const std::uint32_t base = 0x0e000000u | (q << 30) | (len << 13) | (op << 12);
                std::uint64_t state = base + 3;
                for (unsigned s = 0; s < 48; ++s) {
                    const unsigned rd = static_cast<unsigned>(rnd(state) % 32);
                    unsigned rn = static_cast<unsigned>(rnd(state) % 32);
                    unsigned rm = static_cast<unsigned>(rnd(state) % 32);
                    if (rnd(state) % 5 == 0) rm = rd;
                    if (rnd(state) % 5 == 0) rn = rd;   // destination inside the table
                    if (s == 0) rn = 30;                // table wraps V31 -> V0
                    auto ctx = special_context(rnd(state), 0);
                    // Index bytes: mostly in range, some just past the end, some huge.
                    auto* idx = reinterpret_cast<std::uint8_t*>(ctx.v[rm]);
                    const unsigned limit = 16 * (len + 1);
                    for (unsigned i = 0; i < 16; ++i) {
                        switch (rnd(state) % 4) {
                        case 0: idx[i] = static_cast<std::uint8_t>(rnd(state)); break;
                        case 1: idx[i] = static_cast<std::uint8_t>(limit + rnd(state) % 8); break;
                        default: idx[i] = static_cast<std::uint8_t>(rnd(state) % limit); break;
                        }
                    }
                    const std::uint32_t word = base | (rm << 16) | (rn << 5) | rd;
                    const auto outcome = check(emit_neon, ctx, word, "tbl");
                    if (outcome == Outcome::declined || outcome == Outcome::both_declined) {
                        report_declined("tbl", word, outcome);
                        break;
                    }
                    if (outcome != Outcome::ok) break;
                }
            }
    print_stats("after tbl");
}

FX_TEST(neon_modified_immediate) {
    for (unsigned op = 0; op < 2; ++op)
        for (unsigned q = 0; q < 2; ++q)
            for (unsigned cmode = 0; cmode < 15; ++cmode)
                for (unsigned imm8 : {0x00u, 0xffu, 0x01u, 0x80u, 0x5au, 0xa5u, 0x7fu, 0x3cu}) {
                    const std::uint32_t word = 0x0f000400u | (q << 30) | (op << 29) | ((imm8 >> 5) << 16) |
                                               (cmode << 12) | ((imm8 & 31) << 5);
                    const bool valid = !(cmode == 14 && op == 1 && !q && false);
                    sweep(word, 0, 3, "modified_immediate", valid, false, false);
                }
    print_stats("after modified_immediate");
}

// Opt-in (NEON_DUMP=1): prints "<name>|<level>|<hex>" for the hot forms so
// their x64 sequences can be disassembled and counted.
FX_TEST(neon_zz_dump_hot_forms) {
    if (!std::getenv("NEON_DUMP")) return;
    struct Form { const char* name; std::uint32_t word; bool memory; };
    static constexpr Form forms[] = {
        {"ldp_q", 0xad400440u, true},      {"stp_q", 0xad000440u, true},
        {"ldr_q_uimm", 0x3dc00440u, true}, {"str_q_uimm", 0x3d800440u, true},
        {"ldr_q_post", 0x3cc10440u, true}, {"ld1_4reg_16b", 0x4c402040u, true},
        {"ld2_16b", 0x4c408040u, true},    {"ld4_4s", 0x4c400840u, true},
        {"ld1r_4s", 0x4d40c840u, true},    {"ld1_lane_s", 0x4d408040u, true},
        {"add_4s", 0x4ea28420u, false},    {"and_16b", 0x4e221c20u, false},
        {"orr_16b", 0x4ea21c20u, false},   {"eor_16b", 0x6e221c20u, false},
        {"cmeq_4s", 0x6ea28c20u, false},   {"cmeq_16b", 0x6e228c20u, false},
        {"cmhi_4s", 0x6ea23420u, false},   {"bsl_16b", 0x6e621c20u, false},
        {"mul_4s", 0x4ea29c20u, false},    {"sqadd_8h", 0x4e620c20u, false},
        {"dup_elem_4s", 0x4e0c0420u, false}, {"dup_gen_4s", 0x4e040c20u, false},
        {"dup_gen_16b", 0x4e010c20u, false}, {"zip1_4s", 0x4e823820u, false},
        {"zip1_16b", 0x4e023820u, false},  {"uzp1_4s", 0x4e821820u, false},
        {"uzp1_16b", 0x4e021820u, false},  {"uzp2_16b", 0x4e025820u, false},
        {"trn1_4s", 0x4e822820u, false},   {"tbl1_16b", 0x4e020020u, false},
        {"tbl2_16b", 0x4e022020u, false},  {"ext_16b_4", 0x6e022020u, false},
        {"addv_4s", 0x4eb1b820u, false},   {"addv_16b", 0x4e31b820u, false},
        {"umaxv_4s", 0x6eb0a820u, false},  {"ushr_4s_3", 0x6f3d0420u, false},
        {"ushr_16b_3", 0x6f0d0420u, false}, {"sshr_4s_3", 0x4f3d0420u, false},
        {"shl_4s_3", 0x4f235420u, false},  {"movi_4s", 0x4f000420u, false},
        {"umov_w_s1", 0x0e0c3c20u, false}, {"ins_s_gpr", 0x4e0c1c20u, false},
        {"smull_4s", 0x0e62c020u, false},  {"sqrdmulh_8h", 0x6e62b420u, false},
        {"xtn_8b", 0x0e212820u, false},    {"ushl_4s", 0x6ea24420u, false},
        {"cmge_4s", 0x4ea23c20u, false},   {"cmtst_16b", 0x4e228c20u, false},
        {"addp_4s", 0x4ea2bc20u, false},   {"smaxp_4s", 0x4ea2a420u, false},
        {"umull_2d", 0x2ea2c020u, false},  {"uzp1_8h", 0x4e421820u, false},
        {"dup_elem_16b", 0x4e050420u, false}, {"sqxtn_8b", 0x0e214820u, false},
        {"shrn_8b_3", 0x0f0d8420u, false}, {"add_2s", 0x0ea28420u, false},
        {"st1_4reg_16b", 0x4c002040u, true}, {"st4_4s", 0x4c000840u, true},
        {"ldr_d_reg_lsl", 0xfc627840u, true}, {"ldr_literal_q", 0x9c000040u, true},
    };
    for (const auto& f : forms)
        for (const int level : levels()) {
            const auto bytes = emit_bytes(f.memory ? emit_vector_memory : emit_neon, f.word, level, true);
            std::printf("DUMP|%s|%d|", f.name, level);
            for (const auto b : bytes) std::printf("%02x", b);
            std::printf("\n");
        }
}
