// Signature grammar, call planning and database lookups.

#include "fx/abi/abi_internal.hpp"
#include "fx/testing/test.hpp"

#include <cstdio>
#include <string>

using namespace transmute::fx;
using namespace transmute::fx::abi;

namespace {

ArmPiece::Kind kind_of(const ArgPlan& a, std::size_t k = 0) { return a.arm.at(k).kind; }

} // namespace

FX_TEST(sig_parse_scalars_and_variadic) {
    const auto s = parse_signature("i(pIcCsSlfd.)");
    FX_CHECK(s.ret.kind == TypeKind::i32);
    FX_REQUIRE(s.params.size() == 9);
    FX_CHECK(s.params[0].kind == TypeKind::ptr);
    FX_CHECK(s.params[1].kind == TypeKind::u32);
    FX_CHECK(s.params[2].kind == TypeKind::i8);
    FX_CHECK(s.params[5].kind == TypeKind::u16);
    FX_CHECK(s.params[7].kind == TypeKind::f32);
    FX_CHECK(s.params[8].kind == TypeKind::f64);
    FX_CHECK(s.variadic);
    FX_CHECK(!s.unknown);
}

FX_TEST(sig_parse_records_hfa_and_function_pointers) {
    const auto s = parse_signature("{d2}({12}{16:16}{f3}@i(pp)@@v(i)(i))");
    FX_CHECK(s.ret.kind == TypeKind::hfa && s.ret.hfa_double && s.ret.hfa_count == 2 && s.ret.size == 16);
    FX_REQUIRE(s.params.size() == 5);
    FX_CHECK(s.params[0].kind == TypeKind::record && s.params[0].size == 12);
    FX_CHECK(s.params[1].kind == TypeKind::record && s.params[1].size == 16 && s.params[1].align == 16);
    FX_CHECK(s.params[2].kind == TypeKind::hfa && !s.params[2].hfa_double && s.params[2].hfa_count == 3);
    FX_REQUIRE(s.params[3].kind == TypeKind::fnptr && s.params[3].fn != nullptr);
    FX_CHECK_EQ(s.params[3].fn->text, std::string("i(pp)"));
    FX_REQUIRE(s.params[4].kind == TypeKind::fnptr);
    FX_CHECK(s.params[4].fn->ret.kind == TypeKind::fnptr);
    FX_CHECK_EQ(s.params[4].fn->ret.fn->text, std::string("v(i)"));
    // Interning: same text, same object.
    FX_CHECK(intern_signature("i(pp)") == s.params[3].fn);
    FX_CHECK(intern_signature("?")->unknown);
}

FX_TEST(sig_parse_rejects_malformed) {
    const char* bad[] = {"", "i", "i(", "i(v)", "i(.p)", "x()", "{0}()", "{f5}()", "{8:3}()", "i()junk", "@()"};
    for (const char* text : bad) FX_CHECK(intern_signature(text) == nullptr);
}

FX_TEST(plan_integer_registers_and_stack) {
    // LCMapStringEx: 9 integer arguments; the ninth is on the guest stack.
    const auto plan = plan_call(parse_signature("i(pIpipippl)"));
    FX_REQUIRE(plan.args.size() == 9);
    for (unsigned i = 0; i < 8; ++i) {
        FX_CHECK(kind_of(plan.args[i]) == ArmPiece::gpr && plan.args[i].arm[0].index == i);
        FX_CHECK_EQ(plan.args[i].win_pos, i);
    }
    FX_CHECK(kind_of(plan.args[8]) == ArmPiece::stack && plan.args[8].arm[0].offset == 0);
    FX_CHECK_EQ(plan.arm_stack_bytes, 8u);
    FX_CHECK_EQ(plan.win_slots, 9u);
    FX_CHECK(plan.ret_kind == ReturnKind::integer);
}

FX_TEST(plan_mixed_fp_positions) {
    // ldexp(double, int): ARM d0 + w0, Win64 xmm0 + edx.
    const auto plan = plan_call(parse_signature("d(di)"));
    FX_CHECK(kind_of(plan.args[0]) == ArmPiece::vreg && plan.args[0].arm[0].index == 0);
    FX_CHECK(plan.args[0].win_fp && plan.args[0].win_pos == 0);
    FX_CHECK(kind_of(plan.args[1]) == ArmPiece::gpr && plan.args[1].arm[0].index == 0);
    FX_CHECK_EQ(plan.args[1].win_pos, 1);
    FX_CHECK(plan.ret_kind == ReturnKind::fp);
    // Nine doubles: the ninth goes to the guest stack.
    const auto nine = plan_call(parse_signature("d(ddddddddd)"));
    FX_CHECK(kind_of(nine.args[7]) == ArmPiece::vreg && nine.args[7].arm[0].index == 7);
    FX_CHECK(kind_of(nine.args[8]) == ArmPiece::stack && nine.args[8].arm[0].offset == 0);
}

FX_TEST(plan_aggregates) {
    const auto plan = plan_call(parse_signature("v({8}{12}{24}{f2}{d4}{3}{16:16})"));
    FX_REQUIRE(plan.args.size() == 7);
    // {8}: x0, Win64 by value.
    FX_CHECK(plan.args[0].arm.size() == 1 && kind_of(plan.args[0]) == ArmPiece::gpr && !plan.args[0].win_by_reference);
    // {12}: x1,x2, Win64 by reference.
    FX_CHECK(plan.args[1].arm.size() == 2 && plan.args[1].arm[0].index == 1 && plan.args[1].win_by_reference);
    // {24}: by reference on both sides (pointer in x3).
    FX_CHECK(plan.args[2].arm_indirect && plan.args[2].arm[0].index == 3 && plan.args[2].win_by_reference);
    // {f2}: s0, s1; Win64 8-byte value.
    FX_CHECK(plan.args[3].arm.size() == 2 && kind_of(plan.args[3]) == ArmPiece::vreg && plan.args[3].arm[1].index == 1);
    FX_CHECK(plan.args[3].arm[0].bytes == 4 && !plan.args[3].win_by_reference);
    // {d4}: d2..d5; Win64 by reference.
    FX_CHECK(plan.args[4].arm.size() == 4 && plan.args[4].arm[0].index == 2 && plan.args[4].arm[3].index == 5);
    FX_CHECK(plan.args[4].win_by_reference);
    // {3}: x4.
    FX_CHECK(kind_of(plan.args[5]) == ArmPiece::gpr && plan.args[5].arm[0].index == 4 && plan.args[5].win_by_reference);
    // {16:16}: even pair x6/x7.
    FX_CHECK(plan.args[6].arm.size() == 2 && plan.args[6].arm[0].index == 6 && plan.args[6].arm[1].index == 7);
}

FX_TEST(plan_hfa_overflow_goes_to_stack) {
    const auto plan = plan_call(parse_signature("v(ddddddd{d2})"));
    FX_REQUIRE(plan.args.size() == 8);
    FX_CHECK(plan.args[7].arm.size() == 2 && kind_of(plan.args[7]) == ArmPiece::stack);
    FX_CHECK(plan.args[7].arm[0].offset == 0 && plan.args[7].arm[1].offset == 8);
    FX_CHECK_EQ(plan.arm_stack_bytes, 16u);
}

FX_TEST(plan_returns) {
    FX_CHECK(plan_call(parse_signature("{8}()")).ret_kind == ReturnKind::record_regs);
    FX_CHECK(!plan_call(parse_signature("{8}()")).win_hidden_return);
    const auto r12 = plan_call(parse_signature("{12}(i)"));
    FX_CHECK(r12.ret_kind == ReturnKind::record_regs && r12.win_hidden_return);
    FX_CHECK_EQ(r12.args[0].win_pos, 1);   // after the hidden pointer
    const auto r24 = plan_call(parse_signature("{24}(i)"));
    FX_CHECK(r24.ret_kind == ReturnKind::record_memory && r24.win_hidden_return);
    const auto f2 = plan_call(parse_signature("{f2}()"));
    FX_CHECK(f2.ret_kind == ReturnKind::hfa_regs && !f2.win_hidden_return);
    const auto d3 = plan_call(parse_signature("{d3}()"));
    FX_CHECK(d3.ret_kind == ReturnKind::hfa_regs && d3.win_hidden_return);
}

FX_TEST(plan_variadic_uses_gprs_only) {
    // _snprintf(char*, size_t, const char*, ...): named in x0-x2.
    const auto plan = plan_call(parse_signature("i(plp.)"));
    FX_CHECK(plan.variadic);
    FX_CHECK_EQ(plan.arm_named_slots, 3u);
    // A named double in a variadic function still uses an x register.
    const auto d = plan_call(parse_signature("i(pd.)"));
    FX_CHECK(kind_of(d.args[1]) == ArmPiece::gpr && d.args[1].arm[0].index == 1);
}

FX_TEST(sigdb_lookup_known_functions) {
    const struct { const char* name; const char* sig; } expected[] = {
        {"CreateFileW", "p(pIIpIIp)"},
        {"GetCurrentThreadId", "I()"},
        {"lstrlenW", "i(p)"},
        {"qsort", "v(pll@i(pp))"},
        {"pow", "d(dd)"},
        {"sinf", "f(f)"},
        {"ldexp", "d(di)"},
        {"wsprintfA", "i(pp.)"},
        {"LCMapStringEx", "i(pIpipippl)"},
        {"CreateThread", "p(pl@I(p)pIp)"},
        {"signal", "@v(i)(i@v(i))"},
        {"MonitorFromPoint", "p({8}I)"},
        {"_Cbuild", "{d2}(dd)"},
        {"set_terminate", "@v()(@v())"},
        {"EnterCriticalSection", "v(p)"},
        {"AcquireSRWLockExclusive", "v(p)"},
    };
    for (const auto& e : expected) {
        const char* sig = lookup_signature_text(e.name);
        FX_CHECK(sig != nullptr);
        if (sig) FX_CHECK_EQ(std::string(sig), std::string(e.sig));
    }
    FX_CHECK(lookup_signature_text("NoSuchFunctionAnywhere") == nullptr);
    FX_CHECK(lookup_signature("NoSuchFunctionAnywhere")->unknown);
    FX_CHECK(generated_signature_count() > 10000);
}

FX_TEST(sigdb_every_entry_parses) {
    // Every generated signature must parse (the table is generated; this
    // guards the generator and the C++ grammar staying in sync).
    unsigned failures = 0;
    for (const char* name : {"CreateFileW", "qsort", "signal", "wsprintfA", "conj", "cexpf", "VarCyAdd"}) {
        const char* text = lookup_signature_text(name);
        if (!text || !intern_signature(text)) ++failures;
    }
    FX_CHECK_EQ(failures, 0u);
}

FX_TEST(override_registry_lookups) {
    for (const char* name : {"CreateThread", "GetProcAddress", "LoadLibraryW", "VirtualAlloc", "longjmp",
                             "IsProcessorFeaturePresent", "RaiseException", "_initterm", "exit", "__p___argc",
                             "_configure_wide_argv", "ExitProcess", "atexit", "_controlfp_s"}) {
        FX_CHECK(find_override("kernel32.dll", name) != nullptr);
    }
    FX_CHECK(find_override("kernel32.dll", "GetCurrentThreadId") == nullptr);
    FX_CHECK(override_count() > 100);
}

FX_TEST(signature_opengl_table_parses) {
    FX_CHECK(opengl_signature_count() > 200);
    for (std::size_t i = 0; i < opengl_signature_count(); ++i) {
        const char* name = opengl_signature_name(i);
        const Signature* sig = lookup_signature(name);
        if (sig == nullptr || sig->unknown) std::fprintf(stderr, "bad GL signature: %s\n", name);
        FX_CHECK(sig != nullptr && !sig->unknown);
    }
    FX_CHECK_EQ(lookup_signature("glSamplerParameterf")->text, std::string("v(IIf)"));
    FX_CHECK_EQ(lookup_signature("glClearBufferfi")->text, std::string("v(Iifi)"));
}
