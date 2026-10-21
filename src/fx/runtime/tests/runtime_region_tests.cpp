// Hand-assembled x64 "translated regions" exercising linking, the lookup
// stub, the RSB, invalidation, fault sites, SMC and unwinding with real code.

#include "fx/runtime/tests/test_support.hpp"
#include "fx/testing/test.hpp"

#include <windows.h>

#include <cstdio>
#include <stdexcept>

using namespace rtt;
namespace rt = transmute::fx::rt;
using x64::Alu;

namespace {

void init() { runtime_initialize(RuntimeOptions{}); }

constexpr std::uint32_t kRet = 0xd65f03c0;
constexpr std::uint32_t movz(unsigned rd, std::uint32_t imm) { return 0xd2800000u | (imm << 5) | rd; }

void add_x0(RegionBuilder& b, std::int32_t value) { b.as().alu(Alu::add, 64, ptr(Gp::r15, x_disp(0)), value); }

std::int32_t rel32_at(const void* host, std::uint32_t offset) {
    std::int32_t value;
    std::memcpy(&value, static_cast<const std::uint8_t*>(host) + offset, 4);
    return value;
}
const void* rel32_target(const void* host, std::uint32_t offset) {
    return static_cast<const std::uint8_t*>(host) + offset + 4 + rel32_at(host, offset);
}

RuntimeStats delta(const RuntimeStats& a, const RuntimeStats& b) {
    RuntimeStats d;
    for (unsigned i = 0; i < 16; ++i) d.exits[i] = b.exits[i] - a.exits[i];
    d.links = b.links - a.links;
    d.unlinks = b.unlinks - a.unlinks;
    d.translations = b.translations - a.translations;
    d.invalidated_regions = b.invalidated_regions - a.invalidated_regions;
    d.smc_write_faults = b.smc_write_faults - a.smc_write_faults;
    d.cache_hits = b.cache_hits - a.cache_hits;
    return d;
}
std::uint64_t total_exits(const RuntimeStats& d) {
    std::uint64_t n = 0;
    for (auto e : d.exits) n += e;
    return n;
}
constexpr unsigned kLink = static_cast<unsigned>(ExitReason::link);
constexpr unsigned kMiss = static_cast<unsigned>(ExitReason::lookup_miss);
constexpr unsigned kReturn = static_cast<unsigned>(ExitReason::return_to_host);

// A at pa: x0 += 1; b pb.   B at pb: x0 += 10; ret (via x30).
struct LinkedPair {
    GuestCode code{{}};
    std::uint64_t pa = code.at(0), pb = code.at(16);
    std::uint32_t field = 0;
    LinkedPair() {
        set_region_factory(pa, [this](std::uint64_t) {
            RegionBuilder b(pa);
            b.entry(pa);
            add_x0(b, 1);
            field = b.direct_exit(pb);
            return b.finish();
        });
        set_region_factory(pb, [this](std::uint64_t) {
            RegionBuilder b(pb);
            b.entry(pb);
            add_x0(b, 10);
            b.return_via_x30();
            return b.finish();
        });
    }
    ~LinkedPair() {
        set_region_factory(pa, nullptr);
        set_region_factory(pb, nullptr);
    }
};

[[noreturn]] void throw_from_host() { throw std::runtime_error("thrown by a host function"); }

} // namespace

FX_TEST(direct_branch_links_on_first_exit) {
    init();
    LinkedPair p;
    const auto s0 = runtime_stats();
    const std::uint64_t args[] = {0};
    FX_CHECK_EQ(call_guest(p.pa, args), 11u);
    const auto d1 = delta(s0, runtime_stats());
    FX_CHECK_EQ(d1.exits[kLink], 1u);
    FX_CHECK_EQ(d1.links, 1u);
    FX_CHECK_EQ(d1.translations, 2u);
    const void* a = rt::find_entry(p.pa);
    const void* b = rt::find_entry(p.pb);
    FX_REQUIRE(a && b);
    FX_CHECK(rel32_target(a, p.field) == b);
    // Steady state: linked jump, then RET through the lookup stub to the return stub.
    const auto s1 = runtime_stats();
    FX_CHECK_EQ(call_guest(p.pa, args), 11u);
    const auto d2 = delta(s1, runtime_stats());
    FX_CHECK_EQ(total_exits(d2), 1u);
    FX_CHECK_EQ(d2.exits[kReturn], 1u);
}

FX_TEST(existing_targets_link_at_install) {
    init();
    LinkedPair p;
    const std::uint64_t args[] = {0};
    FX_CHECK_EQ(call_guest(p.pb, args), 10u);    // translate B first
    const auto s0 = runtime_stats();
    FX_CHECK_EQ(call_guest(p.pa, args), 11u);
    const auto d = delta(s0, runtime_stats());
    FX_CHECK_EQ(d.exits[kLink], 0u);
    FX_CHECK_EQ(d.links, 1u);
}

FX_TEST(rsb_call_and_return_hit) {
    init();
    GuestCode code({});
    const auto pm = code.at(0), cont = code.at(2), pf = code.at(8);
    set_region_factory(pm, [&](std::uint64_t) {
        RegionBuilder b(pm);
        b.entry(pm);
        auto& a = b.as();
        a.mov(64, Gp::rax, ptr(Gp::r15, x_disp(30)));       // keep the caller's x30 in x19
        a.mov(64, ptr(Gp::r15, x_disp(19)), Gp::rax);
        const Label continuation = a.new_label();
        b.call(cont, continuation, pf);
        a.bind(continuation);
        b.entry(cont);
        add_x0(b, 100);
        a.mov(64, Gp::rax, ptr(Gp::r15, x_disp(19)));
        a.mov(64, ptr(Gp::r15, x_disp(30)), Gp::rax);
        b.ret(9);
        b.cover(pm, pm + 12);
        return b.finish();
    });
    set_region_factory(pf, [&](std::uint64_t) {
        RegionBuilder b(pf);
        b.entry(pf);
        add_x0(b, 1);
        b.ret(9);
        return b.finish();
    });
    GuestContext& ctx = ensure_thread_context();
    for (int run = 0; run < 3; ++run) {
        const auto s0 = runtime_stats();
        ctx.x[0] = 0;
        ctx.x[9] = 0;
        call_guest_prepared(ctx, pm);
        FX_CHECK_EQ(ctx.x[0], 101u);
        FX_CHECK_EQ(ctx.x[9], 2u);   // F returned to M through the RSB, M to the host
        if (run > 0) {
            const auto d = delta(s0, runtime_stats());
            FX_CHECK_EQ(total_exits(d), 1u);
            FX_CHECK_EQ(d.exits[kReturn], 1u);
        }
    }

    // Invalidating F unlinks M's call; invalidating M scrubs RSB entries into M.
    const void* m = rt::find_entry(pm);
    FX_REQUIRE(m != nullptr);
    const auto s1 = runtime_stats();
    invalidate_guest_code(pf, 4);
    FX_CHECK(rt::find_entry(pf) == nullptr);
    auto d = delta(s1, runtime_stats());
    FX_CHECK_EQ(d.invalidated_regions, 1u);
    FX_CHECK_EQ(d.unlinks, 1u);
    bool rsb_into_m = false;
    const auto m_begin = reinterpret_cast<std::uint64_t>(m);
    for (const auto& e : ctx.rsb) {
        const auto h = reinterpret_cast<std::uint64_t>(e.host_entry);
        rsb_into_m |= h >= m_begin && h < m_begin + 0x1000;
    }
    FX_CHECK(rsb_into_m);
    invalidate_guest_code(pm, 12);
    bool redispatch = false;
    for (const auto& e : ctx.rsb) {
        const auto h = reinterpret_cast<std::uint64_t>(e.host_entry);
        FX_CHECK(!(h >= m_begin && h < m_begin + 0x100));
        redispatch |= e.host_entry == rt::g_stubs.rsb_redispatch;
    }
    FX_CHECK(redispatch);
    // Everything is retranslated and still correct.
    const auto s2 = runtime_stats();
    ctx.x[0] = 0;
    call_guest_prepared(ctx, pm);
    FX_CHECK_EQ(ctx.x[0], 101u);
    FX_CHECK_EQ(delta(s2, runtime_stats()).translations, 2u);
    set_region_factory(pm, nullptr);
    set_region_factory(pf, nullptr);
}

FX_TEST(invalidation_restores_unlinked_tail) {
    init();
    LinkedPair p;
    const std::uint64_t args[] = {0};
    FX_CHECK_EQ(call_guest(p.pa, args), 11u);
    const void* a = rt::find_entry(p.pa);
    const void* b = rt::find_entry(p.pb);
    FX_CHECK(rel32_target(a, p.field) == b);
    FX_CHECK_EQ(rt::live_regions_on_page(p.pa), 2u);
    invalidate_guest_code(p.pb, 4);
    FX_CHECK_EQ(rt::live_regions_on_page(p.pa), 1u);
    // The exit jumps back to A's own unlinked tail.
    const auto* target = static_cast<const std::uint8_t*>(rel32_target(a, p.field));
    FX_CHECK(target > static_cast<const std::uint8_t*>(a) && target < static_cast<const std::uint8_t*>(a) + 0x100);
    const auto s0 = runtime_stats();
    FX_CHECK_EQ(call_guest(p.pa, args), 11u);
    const auto d = delta(s0, runtime_stats());
    FX_CHECK_EQ(d.exits[kLink], 1u);
    FX_CHECK_EQ(d.translations, 1u);
}

FX_TEST(lookup_stub_hits_and_aliased_slots) {
    init();
    GuestCode code({});
    const auto p = code.at(0);
    // X and Y share a lookup slot; X jumps to Y through the lookup stub.
    const std::uint64_t x = 0x0000'0222'0000'0000ULL;
    std::uint64_t y = x + (std::uint64_t{1} << kLookupTagShift);
    while (rt::lookup_index(y) != rt::lookup_index(x)) y += 4;
    {
        x64::Assembler a;
        a.alu(Alu::add, 64, ptr(Gp::r15, x_disp(0)), 1);
        a.mov_imm64(Gp::rax, y);
        a.mov(64, ptr(Gp::r15, d_pc), Gp::rax);
        a.jmp(ptr(Gp::r15, d_stub_indirect));
        a.finalize();
        void* code_x = allocate_code(a.size());
        std::memcpy(code_x, a.data(), a.size());
        register_host_entry(x, code_x);
    }
    register_host_entry(y, make_add_host_entry(10));
    set_region_factory(p, [&](std::uint64_t) {
        RegionBuilder b(p);
        b.entry(p);
        b.indirect_exit(x);
        return b.finish();
    });
    const std::uint64_t args[] = {0};
    for (int i = 0; i < 3; ++i) FX_CHECK_EQ(call_guest(p, args), 11u);
    const auto s0 = runtime_stats();
    FX_CHECK_EQ(call_guest(p, args), 11u);
    const auto d = delta(s0, runtime_stats());
    FX_CHECK_EQ(d.exits[kMiss], 2u);    // the two aliased pcs evict each other
    FX_CHECK_EQ(d.exits[kReturn], 1u);

    // A non-aliased target hits every time.
    const std::uint64_t z = 0x0000'0333'0000'0010ULL;
    register_host_entry(z, make_add_host_entry(5));
    const auto q = code.at(4);
    set_region_factory(q, [&](std::uint64_t) {
        RegionBuilder b(q);
        b.entry(q);
        b.indirect_exit(z);
        return b.finish();
    });
    FX_CHECK_EQ(call_guest(q, args), 5u);
    const auto s1 = runtime_stats();
    FX_CHECK_EQ(call_guest(q, args), 5u);
    const auto d1 = delta(s1, runtime_stats());
    FX_CHECK_EQ(total_exits(d1), 1u);
    set_region_factory(p, nullptr);
    set_region_factory(q, nullptr);
}

FX_TEST(fault_in_translated_code_recovers_pins_and_flags) {
    init();
    GuestCode code({});
    const auto p = code.at(0);
    set_region_factory(p, [&](std::uint64_t) {
        RegionBuilder b(p);
        b.entry(p);
        auto& a = b.as();
        a.mov(64, Gp::rbx, ptr(Gp::r15, x_disp(1)));   // pin x1 -> rbx (null)
        a.mov_imm(Gp::rsi, 0x1234);                      // pin x5 -> rsi (dirty)
        a.cmp(64, Gp::rsi, 0x1234);                      // Z=1, C(no borrow)=1
        const auto begin = a.size();
        a.mov(64, Gp::rax, ptr(Gp::rbx, 0x40));          // faults
        const auto end = a.size();
        a.mov(64, ptr(Gp::r15, x_disp(0)), Gp::rax);
        b.return_via_x30();
        b.fault_site(begin, end, p + 8, FlagsLocation::host_sub);
        b.pin(1, Gp::rbx);
        b.pin(5, Gp::rsi);
        return b.finish();
    });
    GuestContext seen{};
    GuestExceptionRecord record{};
    set_exception_handler([&](GuestContext& ctx, const GuestExceptionRecord& r) {
        seen = ctx;
        record = r;
        ctx.x[0] = 77;
        ctx.pc = kReturnToHostPc;
    });
    GuestContext& ctx = ensure_thread_context();
    ctx.x[1] = 0;
    ctx.x[5] = 0;
    ctx.nzcv = 0;
    call_guest_prepared(ctx, p);
    set_exception_handler(nullptr);
    FX_CHECK_EQ(ctx.x[0], 77u);
    FX_CHECK_EQ(record.code, 0xC0000005u);
    FX_CHECK_EQ(record.address, p + 8);
    FX_CHECK_EQ(record.parameters[0], 0u);
    FX_CHECK_EQ(record.parameters[1], 0x40u);
    FX_CHECK_EQ(seen.pc, p + 8);
    FX_CHECK_EQ(seen.x[5], 0x1234u);
    FX_CHECK_EQ(seen.nzcv, 0x60000000u);
    set_region_factory(p, nullptr);
}

// A fault site's map names guest registers whose current value is in a host
// register (dirty write-back cache entries); entries override pins, and
// kFaultMapContext entries say the context holds the value.
FX_TEST(fault_map_overrides_pins_and_context) {
    init();
    GuestCode code({});
    const auto p = code.at(0);
    set_region_factory(p, [&](std::uint64_t) {
        RegionBuilder b(p);
        b.entry(p);
        auto& a = b.as();
        a.mov(64, Gp::rbx, ptr(Gp::r15, x_disp(1)));   // pin x1 -> rbx (null)
        a.mov_imm(Gp::rsi, 0x9999);                      // pin x5 -> rsi, but x5 lives in the context here
        a.mov_imm(Gp::r9, 0x7777);                       // x7 dirty in r9
        a.movq(x64::Xmm::xmm2, Gp::r9);                  // v3 dirty in xmm2
        const auto begin = a.size();
        a.mov(64, Gp::rax, ptr(Gp::rbx, 0x40));          // faults
        const auto end = a.size();
        a.mov(64, ptr(Gp::r15, x_disp(0)), Gp::rax);
        b.return_via_x30();
        b.fault_site(begin, end, p + 4);
        b.pin(1, Gp::rbx);
        b.pin(5, Gp::rsi);
        b.fault_map({{7, static_cast<std::uint8_t>(x64::id(Gp::r9)), false}, {5, kFaultMapContext, false},
                     {3, 2, true}});
        return b.finish();
    });
    GuestContext seen{};
    set_exception_handler([&](GuestContext& ctx, const GuestExceptionRecord&) {
        seen = ctx;
        ctx.x[0] = 78;
        ctx.pc = kReturnToHostPc;
    });
    GuestContext& ctx = ensure_thread_context();
    ctx.x[1] = 0;
    ctx.x[5] = 0x55;
    ctx.x[7] = 1;
    ctx.v[3][0] = 2;
    ctx.v[3][1] = 3;
    call_guest_prepared(ctx, p);
    set_exception_handler(nullptr);
    FX_CHECK_EQ(ctx.x[0], 78u);
    FX_CHECK_EQ(seen.pc, p + 4);
    FX_CHECK_EQ(seen.x[1], 0u);
    FX_CHECK_EQ(seen.x[5], 0x55u);
    FX_CHECK_EQ(seen.x[7], 0x7777u);
    FX_CHECK_EQ(seen.v[3][0], 0x7777u);
    FX_CHECK_EQ(seen.v[3][1], 0u);
    set_region_factory(p, nullptr);
}

FX_TEST(smc_write_invalidates_translation) {
    init();
    set_mini_translator(true);
    GuestCode code({movz(0, 1), kRet});
    FX_CHECK_EQ(call_guest(code.at(0), {}), 1u);
    const auto s0 = runtime_stats();
    FX_CHECK_EQ(call_guest(code.at(0), {}), 1u);
    FX_CHECK_EQ(delta(s0, runtime_stats()).translations, 0u);   // translated, not interpreted
    // The page is write-protected now; this store faults, invalidates and retries.
    code.words()[0] = movz(0, 2);
    const auto d = delta(s0, runtime_stats());
    FX_CHECK_EQ(d.smc_write_faults, 1u);
    FX_CHECK(d.invalidated_regions >= 1u);
    FX_CHECK_EQ(code.words()[0], movz(0, 2));
    FX_CHECK_EQ(call_guest(code.at(0), {}), 2u);
    // The retranslation protected the page again: a data write on the same
    // page faults once more and invalidates; execution stays correct.
    code.words()[100] = 0;
    FX_CHECK_EQ(delta(s0, runtime_stats()).smc_write_faults, 2u);
    FX_CHECK_EQ(call_guest(code.at(0), {}), 2u);
    set_mini_translator(false);
}

FX_TEST(cpp_exception_unwinds_through_translated_code) {
    init();
    GuestCode code({});
    const auto p = code.at(0);
    set_region_factory(p, [&](std::uint64_t) {
        RegionBuilder b(p);
        b.entry(p);
        b.as().mov_imm64(Gp::rax, reinterpret_cast<std::uint64_t>(&throw_from_host));
        b.as().call(Gp::rax);
        b.return_via_x30();
        return b.finish();
    });
    GuestContext& ctx = ensure_thread_context();
    const auto nesting = ctx.nesting;
    const auto host_rsp = ctx.host_rsp;
    bool caught = false;
    try {
        call_guest(p, {});
    } catch (const std::runtime_error&) {
        caught = true;
    }
    FX_CHECK(caught);
    FX_CHECK_EQ(ctx.nesting, nesting);
    FX_CHECK_EQ(ctx.host_rsp, host_rsp);
    LinkedPair pair;   // the runtime keeps working afterwards
    const std::uint64_t args[] = {0};
    FX_CHECK_EQ(call_guest(pair.pa, args), 11u);
    set_region_factory(p, nullptr);
}

FX_TEST(interpret_helper_runs_in_place_and_raises) {
    init();
    clear_exception_log();
    GuestCode code({0x8b010000 /* add x0, x0, x1 */, 0x00000000 /* udf */});
    const auto p = code.at(0), q = code.at(1);
    const auto helper_region = [](std::uint64_t pc) {
        RegionBuilder b(pc);
        b.entry(pc);
        b.set_pc(pc);
        b.as().lea(64, Gp::rcx, ptr(Gp::r15, -kContextBias));
        b.as().call(ptr(Gp::r15, helper_disp(helper_interpret_one)));
        b.return_via_x30();
        return b.finish();
    };
    set_region_factory(p, helper_region);
    set_region_factory(q, helper_region);
    const std::uint64_t args[] = {30, 12};
    const auto s0 = runtime_stats();
    FX_CHECK_EQ(call_guest(p, args), 42u);
    FX_CHECK_EQ(delta(s0, runtime_stats()).exits[static_cast<unsigned>(ExitReason::interpret)], 0u);
    FX_CHECK_EQ(call_guest(q, args), kUnhandledMarker | 0xC000001Du);
    const auto log = exception_log();
    FX_REQUIRE(log.size() == 1);
    FX_CHECK_EQ(log[0].address, q);
    set_region_factory(p, nullptr);
    set_region_factory(q, nullptr);
}

namespace {
[[noreturn]] void throw_from_thunk() { throw std::runtime_error("thrown below an abi frame"); }

// sub rsp, frame; call throw_from_thunk; add rsp, frame; then `tail`.
void* make_framed_code(std::int32_t frame, bool entered_by_call, AbiCodeFrame& info) {
    x64::Assembler a;
    a.sub(64, Gp::rsp, frame);
    const auto prolog = a.size();
    a.mov_imm64(Gp::rax, reinterpret_cast<std::uint64_t>(&throw_from_thunk));
    a.call(Gp::rax);
    const auto epilog = a.size();
    a.add(64, Gp::rsp, frame);
    if (entered_by_call) {
        a.ret();
    } else {
        a.mov(64, Gp::rax, ptr(Gp::r15, x_disp(30)));
        a.mov(64, ptr(Gp::r15, d_pc), Gp::rax);
        a.jmp(ptr(Gp::r15, d_stub_indirect));
    }
    a.finalize();
    auto* code = static_cast<std::uint8_t*>(allocate_code(a.size()));
    std::memcpy(code, a.data(), a.size());
    info.begin = reinterpret_cast<std::uint64_t>(code);
    info.end = info.begin + a.size();
    info.prolog_bytes = static_cast<std::uint32_t>(prolog);
    info.epilog_offset = static_cast<std::uint32_t>(epilog);
    info.frame_bytes = static_cast<std::uint32_t>(frame);
    info.entered_by_call = entered_by_call;
    return code;
}
} // namespace

FX_TEST(unwind_through_abi_thunk_and_trampoline_frames) {
    init();
    // Guest-callable thunk entered by jmp (rsp == host_rsp at entry), with a
    // 0x30-byte frame around a host call that throws.
    AbiCodeFrame thunk_frame;
    void* thunk = make_framed_code(0x30, false, thunk_frame);
    add_abi_frame(thunk_frame);
    const std::uint64_t pc = 0x0000'0444'0000'0000ULL;
    register_host_entry(pc, thunk);
    GuestContext& ctx = ensure_thread_context();
    const auto nesting = ctx.nesting;
    bool caught = false;
    try {
        call_guest(pc, {});
    } catch (const std::runtime_error&) {
        caught = true;
    }
    FX_CHECK(caught);
    FX_CHECK_EQ(ctx.nesting, nesting);
    DWORD64 image = 0;
    const auto* entry = RtlLookupFunctionEntry(thunk_frame.begin + thunk_frame.prolog_bytes, &image, nullptr);
    FX_REQUIRE(entry != nullptr);
    FX_CHECK_EQ(image + entry->BeginAddress, thunk_frame.begin + thunk_frame.prolog_bytes);

    // Callback trampoline: an ordinary Win64 function with a 0x28-byte frame.
    AbiCodeFrame tramp_frame;
    void* tramp = make_framed_code(0x28, true, tramp_frame);
    add_abi_frame(tramp_frame);
    caught = false;
    try {
        reinterpret_cast<void (*)()>(tramp)();
    } catch (const std::runtime_error&) {
        caught = true;
    }
    FX_CHECK(caught);
    // Outside its body a called trampoline is a leaf ([rsp] = return address).
    FX_CHECK(RtlLookupFunctionEntry(tramp_frame.begin, &image, nullptr) == nullptr);
}

FX_TEST(cache_configured_with_jit_translate_options) {
    init();
    FX_CHECK_EQ(g_cache_configures.load(), 1);
    FX_CHECK(g_cache_config.enabled);
    FX_CHECK_EQ(g_cache_config.translate_options.profile, rt::jit_translate_options().profile);
    FX_CHECK_EQ(g_cache_config.translate_options.max_instructions, rt::jit_translate_options().max_instructions);
    GuestCode code({0x8b010000, 0xd65f03c0});
    const int stores = g_cache_stores.load();
    const std::uint64_t args[] = {1, 2};
    FX_CHECK_EQ(call_guest(code.at(0), args), 3u);
    FX_CHECK(g_cache_stores.load() > stores);   // enabled: JIT regions are offered to the cache
}

// ---- eager linking and preloading -------------------------------------------------------------

namespace {

// A at pa: x0 += 1; ret -- with a never-taken direct exit to pb after the
// return. B at pb: x0 += 10; ret.
struct DormantExit {
    GuestCode code{{}};
    std::uint64_t pa = code.at(0), pb = code.at(32);
    std::uint32_t field = 0;
    static TranslatedRegion region_a(std::uint64_t pa, std::uint64_t pb, std::uint32_t& field) {
        RegionBuilder b(pa);
        b.entry(pa);
        add_x0(b, 1);
        b.return_via_x30();
        field = b.direct_exit(pb);
        return b.finish();
    }
    static TranslatedRegion region_b(std::uint64_t pb) {
        RegionBuilder b(pb);
        b.entry(pb);
        add_x0(b, 10);
        b.return_via_x30();
        return b.finish();
    }
};

bool wait_for(const std::function<bool()>& done) {
    for (int i = 0; i < 5000; ++i) {
        if (done()) return true;
        Sleep(1);
    }
    return done();
}

} // namespace

FX_TEST(pending_exit_links_when_its_target_is_installed) {
    init();
    DormantExit p;
    set_region_factory(p.pa, [&](std::uint64_t) { return DormantExit::region_a(p.pa, p.pb, p.field); });
    set_region_factory(p.pb, [&](std::uint64_t) { return DormantExit::region_b(p.pb); });
    const std::uint64_t args[] = {0};
    FX_CHECK_EQ(call_guest(p.pa, args), 1u);   // A installed; its exit to B never ran
    const void* a = rt::find_entry(p.pa);
    FX_REQUIRE(a);
    FX_CHECK(rel32_target(a, p.field) != rt::find_entry(p.pb));
    const auto s0 = runtime_stats();
    FX_CHECK_EQ(call_guest(p.pb, args), 10u);  // installing B links A's pending exit
    const auto d = delta(s0, runtime_stats());
    FX_CHECK_EQ(d.links, 1u);
    FX_CHECK_EQ(d.exits[kLink], 0u);
    const void* b = rt::find_entry(p.pb);
    FX_REQUIRE(b);
    FX_CHECK(rel32_target(a, p.field) == b);
    // B invalidated: the exit returns to A's tail, then relinks to the new B.
    invalidate_guest_code(p.pb, 4);
    const auto* tail = static_cast<const std::uint8_t*>(rel32_target(a, p.field));
    FX_CHECK(tail > static_cast<const std::uint8_t*>(a) && tail < static_cast<const std::uint8_t*>(a) + 0x100);
    FX_CHECK_EQ(call_guest(p.pb, args), 10u);
    const void* b2 = rt::find_entry(p.pb);
    FX_REQUIRE(b2 && b2 != b);
    FX_CHECK(rel32_target(a, p.field) == b2);
    set_region_factory(p.pa, nullptr);
    set_region_factory(p.pb, nullptr);
}

FX_TEST(dead_region_exits_are_never_linked) {
    init();
    DormantExit p;
    set_region_factory(p.pa, [&](std::uint64_t) { return DormantExit::region_a(p.pa, p.pb, p.field); });
    set_region_factory(p.pb, [&](std::uint64_t) { return DormantExit::region_b(p.pb); });
    const std::uint64_t args[] = {0};
    FX_CHECK_EQ(call_guest(p.pa, args), 1u);
    const void* a = rt::find_entry(p.pa);
    FX_REQUIRE(a);
    const std::int32_t unlinked = rel32_at(a, p.field);
    invalidate_guest_code(p.pa, 4);            // A dies with its exit still pending
    const auto s0 = runtime_stats();
    FX_CHECK_EQ(call_guest(p.pb, args), 10u);
    FX_CHECK_EQ(delta(s0, runtime_stats()).links, 0u);
    FX_CHECK_EQ(rel32_at(a, p.field), unlinked);
    set_region_factory(p.pa, nullptr);
    set_region_factory(p.pb, nullptr);
}

// How many preloader threads: TRANSMUTE_FX_PRELOAD decides either way, so it can turn
// preloading on for a host that leaves it off by default (Alchemy).
FX_TEST(preload_thread_count) {
    RuntimeOptions on, off, uncached;
    off.preload_cached_regions = false;
    uncached.enable_translation_cache = false;
    FX_CHECK_EQ(rt::preload_threads(on, nullptr, 16), 3u);
    FX_CHECK_EQ(rt::preload_threads(on, nullptr, 8), 2u);
    FX_CHECK_EQ(rt::preload_threads(on, nullptr, 2), 1u);
    FX_CHECK_EQ(rt::preload_threads(on, nullptr, 0), 1u);
    FX_CHECK_EQ(rt::preload_threads(off, nullptr, 16), 0u);
    FX_CHECK_EQ(rt::preload_threads(off, "2", 16), 2u);
    FX_CHECK_EQ(rt::preload_threads(off, "20", 16), 8u);
    FX_CHECK_EQ(rt::preload_threads(on, "0", 16), 0u);
    FX_CHECK_EQ(rt::preload_threads(on, "", 16), 3u);
    FX_CHECK_EQ(rt::preload_threads(uncached, "2", 16), 0u);
}

FX_TEST(preloader_installs_and_links_cached_regions_ahead_of_use) {
    init();
    DormantExit p;
    // A takes its exit to B here: x0 += 1; b pb.
    set_cached_region(p.pa, [&](std::uint64_t) {
        RegionBuilder b(p.pa);
        b.entry(p.pa);
        add_x0(b, 1);
        p.field = b.direct_exit(p.pb);
        return b.finish();
    });
    set_cached_region(p.pb, [&](std::uint64_t) { return DormantExit::region_b(p.pb); });
    set_cached_starts(p.code.base(), {p.pa, p.pb});
    const auto s0 = runtime_stats();
    const int translations = g_translate_calls.load();
    FX_REQUIRE(notify_module_ready(p.code.base(), 1));
    FX_REQUIRE(wait_for([&] { return rt::find_entry(p.pa) && rt::find_entry(p.pb); }));
    FX_REQUIRE(wait_for([&] { return runtime_stats().preloaded - s0.preloaded == 2; }));
    const void* a = rt::find_entry(p.pa);
    const void* b = rt::find_entry(p.pb);
    FX_CHECK(rel32_target(a, p.field) == b);   // linked although A never ran
    const auto s1 = runtime_stats();
    const std::uint64_t args[] = {0};
    FX_CHECK_EQ(call_guest(p.pa, args), 11u);
    const auto d = delta(s1, runtime_stats());
    FX_CHECK_EQ(d.exits[kLink], 0u);
    FX_CHECK_EQ(d.exits[kMiss], 0u);
    FX_CHECK_EQ(g_translate_calls.load(), translations);
    // A module that is no longer (or never was) cached is skipped.
    FX_REQUIRE(notify_module_ready(p.code.base() + 0x100000, 2));
    set_cached_starts(p.code.base(), {});
    set_cached_region(p.pa, nullptr);
    set_cached_region(p.pb, nullptr);
}

FX_TEST(exit_references_follow_region_lifetimes) {
    init();
    std::size_t linked0 = 0;
    const std::size_t pending0 = rt::exit_refs_for_tests(&linked0);
    {
        DormantExit p;
        set_region_factory(p.pa, [&](std::uint64_t) { return DormantExit::region_a(p.pa, p.pb, p.field); });
        set_region_factory(p.pb, [&](std::uint64_t) { return DormantExit::region_b(p.pb); });
        const std::uint64_t args[] = {0};
        FX_CHECK_EQ(call_guest(p.pa, args), 1u);   // A's exit to B waits (pending)
        std::size_t linked = 0;
        FX_CHECK_EQ(rt::exit_refs_for_tests(&linked), pending0 + 1);
        FX_CHECK_EQ(call_guest(p.pb, args), 10u);  // linked eagerly
        FX_CHECK_EQ(rt::exit_refs_for_tests(&linked), pending0);
        FX_CHECK_EQ(linked, linked0 + 1);
        invalidate_guest_code(p.pb, 4);            // back to pending
        FX_CHECK_EQ(rt::exit_refs_for_tests(&linked), pending0 + 1);
        FX_CHECK_EQ(linked, linked0);
        invalidate_guest_code(p.pa, 4);            // a dead region keeps no reference
        FX_CHECK_EQ(rt::exit_refs_for_tests(&linked), pending0);
        FX_CHECK_EQ(linked, linked0);
        set_region_factory(p.pa, nullptr);
        set_region_factory(p.pb, nullptr);
    }
}

// Thousands of exits wait for one target (V8 code calling a common builtin
// trampoline). Each removal is O(1) through the exit's own list node, in any
// order: from the middle of the pending list, while linked, and in reverse.
// (Removal walked the target's list; an SMC kill of V8 code was quadratic.)
FX_TEST(many_exits_to_one_target_are_removed_in_any_order) {
    init();
    constexpr unsigned kRegions = 4096;
    GuestCode code({}, 0x20000);
    const auto target = code.at(kRegions * 4 + 64);
    const auto pc = [&](unsigned i) { return code.at(i * 4); };
    for (unsigned i = 0; i < kRegions; ++i) {
        set_region_factory(pc(i), [&, i](std::uint64_t) {
            RegionBuilder b(pc(i));
            b.entry(pc(i));
            add_x0(b, 1);
            b.return_via_x30();
            b.direct_exit(target);
            return b.finish();
        });
    }
    set_region_factory(target, [&](std::uint64_t) {
        RegionBuilder b(target);
        b.entry(target);
        add_x0(b, 10);
        b.return_via_x30();
        return b.finish();
    });
    std::size_t linked0 = 0;
    const std::size_t pending0 = rt::exit_refs_for_tests(&linked0);
    const std::uint64_t args[] = {0};
    for (unsigned i = 0; i < kRegions; ++i) FX_REQUIRE(call_guest(pc(i), args) == 1u);
    std::size_t linked = 0;
    FX_CHECK_EQ(rt::exit_refs_for_tests(&linked), pending0 + kRegions);
    const double t0 = now_ns();
    for (unsigned i = 1; i < kRegions; i += 2) invalidate_guest_code(pc(i), 4);   // every other one
    FX_CHECK_EQ(rt::exit_refs_for_tests(&linked), pending0 + kRegions / 2);
    FX_CHECK_EQ(call_guest(target, args), 10u);   // links the survivors
    FX_CHECK_EQ(rt::exit_refs_for_tests(&linked), pending0);
    FX_CHECK_EQ(linked, linked0 + kRegions / 2);
    for (unsigned i = 0; i < kRegions; i += 4) invalidate_guest_code(pc(i), 4);    // linked ones
    FX_CHECK_EQ(rt::exit_refs_for_tests(&linked), pending0);
    FX_CHECK_EQ(linked, linked0 + kRegions / 4);
    invalidate_guest_code(target, 4);   // the rest wait again
    FX_CHECK_EQ(rt::exit_refs_for_tests(&linked), pending0 + kRegions / 4);
    FX_CHECK_EQ(linked, linked0);
    for (unsigned i = kRegions; i-- > 0;)
        if (i % 4 == 2) invalidate_guest_code(pc(i), 4);
    FX_CHECK_EQ(rt::exit_refs_for_tests(&linked), pending0);
    FX_CHECK_EQ(linked, linked0);
    std::printf("  %u exits to one target removed in %.1f ms\n", kRegions, (now_ns() - t0) / 1e6);
    for (unsigned i = 0; i < kRegions; ++i) set_region_factory(pc(i), nullptr);
    set_region_factory(target, nullptr);
}

// An exact copy of code (V8's builtins remapped from image text into its code
// range) gets its translations from the source's cache, relocated, while its
// guest bytes equal the source's; its own translations are stored for the
// source. A copy whose bytes changed is translated itself and not stored.
FX_TEST(code_copy_uses_the_sources_cache_while_its_bytes_match) {
    init();
    set_mini_translator(true);
    GuestCode source({movz(0, 1), kRet, movz(0, 2), kRet});
    GuestCode copy({movz(0, 1), kRet, movz(0, 2), kRet});
    register_guest_code_copy(copy.base(), source.base(), 0x4000);
    // The "cached" region at the source adds 42: running it proves where it came from.
    set_cached_region(source.at(0), [&](std::uint64_t pc) {
        RegionBuilder b(pc);
        b.entry(pc);
        add_x0(b, 42);
        b.return_via_x30();
        return b.finish();
    });
    const std::uint64_t args[] = {0};
    const auto s0 = runtime_stats();
    const int translations0 = g_translate_calls.load();
    FX_CHECK_EQ(call_guest(copy.at(0), args), 42u);
    const auto s1 = runtime_stats();
    FX_CHECK_EQ(s1.copy_cache_hits - s0.copy_cache_hits, 1u);
    FX_CHECK_EQ(g_translate_calls.load(), translations0);
    // No cached region at the source: translated in the copy, stored for the source.
    const int copy_stores0 = g_cache_copy_stores.load();
    FX_CHECK_EQ(call_guest(copy.at(2), args), 2u);
    FX_CHECK_EQ(g_translate_calls.load(), translations0 + 1);
    FX_CHECK_EQ(runtime_stats().copy_translations - s1.copy_translations, 1u);
    FX_CHECK_EQ(g_cache_copy_stores.load(), copy_stores0 + 1);
    // The copy changes: its bytes no longer match, so the source's region is
    // not used and the copy's own translation is not stored.
    copy.words()[0] = movz(0, 7);
    invalidate_guest_code(copy.at(0), 16);   // also ends the pairing
    register_guest_code_copy(copy.base(), source.base(), 0x4000);
    const auto s2 = runtime_stats();
    FX_CHECK_EQ(call_guest(copy.at(0), args), 7u);
    FX_CHECK_EQ(runtime_stats().copy_cache_hits, s2.copy_cache_hits);
    FX_CHECK_EQ(g_translate_calls.load(), translations0 + 2);
    FX_CHECK_EQ(g_cache_copy_stores.load(), copy_stores0 + 1);
    invalidate_guest_code(copy.base(), 0x4000);
    set_cached_region(source.at(0), nullptr);
    set_mini_translator(false);
}

FX_TEST(cached_region_on_a_newly_protected_page_is_looked_up_again) {
    init();
    // The cache hashed the guest bytes before the install write-protected
    // their (writable) page; a write in between would go unseen, so the
    // install is refused once and the lookup repeated under protection.
    DormantExit p;
    set_cached_region(p.pb, [&](std::uint64_t) { return DormantExit::region_b(p.pb); });
    const auto s0 = runtime_stats();
    const int lookups = g_cache_lookups.load();
    const std::uint64_t args[] = {0};
    FX_CHECK_EQ(call_guest(p.pb, args), 10u);
    const auto d = delta(s0, runtime_stats());
    FX_CHECK_EQ(g_cache_lookups.load() - lookups, 2);
    FX_CHECK_EQ(d.cache_hits, 2u);
    FX_CHECK_EQ(d.translations, 0u);
    set_cached_region(p.pb, nullptr);
}

// ---- FPSR / MXCSR at region entry ---------------------------------------------------------------

FX_TEST(fpsr_flags_move_into_mxcsr_at_region_entry) {
    init();
    GuestCode code({});
    const auto pc = code.at(0);
    const std::int32_t d_fpsr = ctx_disp(offsetof(GuestContext, fpsr));
    // x1 = ctx.fpsr and x2 = MXCSR as seen inside the region; with x3 != 0 the
    // region then clears the MXCSR status flags (as host code such as _clearfp would).
    set_region_factory(pc, [&](std::uint64_t) {
        RegionBuilder b(pc);
        b.entry(pc);
        auto& a = b.as();
        a.mov(32, Gp::rax, ptr(Gp::r15, d_fpsr));
        a.mov(64, ptr(Gp::r15, x_disp(1)), Gp::rax);
        a.stmxcsr(ptr(Gp::r15, x_disp(2)));
        a.mov(32, ptr(Gp::r15, x_disp(2) + 4), 0);
        const Label keep = a.new_label();
        a.cmp(64, ptr(Gp::r15, x_disp(3)), 0);
        a.jcc(x64::Cond::e, keep);
        a.mov(32, Gp::rax, ptr(Gp::r15, x_disp(2)));
        a.and_(32, Gp::rax, ~0x3f);
        a.mov(32, ptr(Gp::r15, x_disp(4)), Gp::rax);
        a.ldmxcsr(ptr(Gp::r15, x_disp(4)));
        a.bind(keep);
        b.return_via_x30();
        return b.finish();
    });
    GuestContext& ctx = ensure_thread_context();
    const std::uint32_t idc = 0x80, ioc = 0x1, ixc = 0x10;
    ctx.fpsr = idc | ioc | ixc;
    ctx.x[3] = 0;
    call_guest_prepared(ctx, pc);
    FX_CHECK_EQ(ctx.x[1] & 0x9f, std::uint64_t{idc});      // mirrored flags left ctx.fpsr; IDC stays
    FX_CHECK((ctx.x[2] & 0x21) == 0x21);                     // ...and are in MXCSR (IE, PE)
    FX_CHECK_EQ(ctx.fpsr & 0x9f, idc | ioc | ixc);          // architectural FPSR unchanged after the exit
    // Host code clearing MXCSR flags during the run clears them for good.
    ctx.x[3] = 1;
    call_guest_prepared(ctx, pc);
    FX_CHECK_EQ(ctx.fpsr & 0x9f, idc);
    ctx.fpsr = 0;
    set_region_factory(pc, nullptr);
}

// ---- native host callbacks --------------------------------------------------------------------------

namespace {
struct CallbackLog {
    int calls = 0;
    std::uint64_t pc_seen = 0;
    std::uint64_t redirect = 0;   // nonzero: set ctx.pc to this
};
void host_callback(GuestContext& ctx, void* user) {
    auto& log = *static_cast<CallbackLog*>(user);
    ++log.calls;
    log.pc_seen = ctx.pc;
    ctx.x[0] = ctx.x[0] * 3 + 1;
    if (log.redirect) ctx.pc = log.redirect;
}
} // namespace

FX_TEST(host_callback_runs_from_translated_code_without_a_dispatcher_exit) {
    init();
    GuestCode code({});
    const auto pm = code.at(0), cont = code.at(2), callee = code.at(16);
    CallbackLog log;
    register_host_callback(callee, &host_callback, &log);
    // M: bl callee; x0 += 100; ret.
    set_region_factory(pm, [&](std::uint64_t) {
        RegionBuilder b(pm);
        b.entry(pm);
        auto& a = b.as();
        a.mov(64, Gp::rax, ptr(Gp::r15, x_disp(30)));
        a.mov(64, ptr(Gp::r15, x_disp(19)), Gp::rax);
        const Label continuation = a.new_label();
        b.call(cont, continuation, callee);
        a.bind(continuation);
        b.entry(cont);
        add_x0(b, 100);
        a.mov(64, Gp::rax, ptr(Gp::r15, x_disp(19)));
        a.mov(64, ptr(Gp::r15, x_disp(30)), Gp::rax);
        b.ret(9);
        b.cover(pm, pm + 12);
        return b.finish();
    });
    GuestContext& ctx = ensure_thread_context();
    for (int run = 0; run < 3; ++run) {
        const auto s0 = runtime_stats();
        ctx.x[0] = 5;
        call_guest_prepared(ctx, pm);
        FX_CHECK_EQ(ctx.x[0], 116u);           // 5 * 3 + 1 + 100
        FX_CHECK_EQ(log.pc_seen, cont);        // ctx.pc = return address on entry
        if (run > 0) {
            const auto d = delta(s0, runtime_stats());
            FX_CHECK_EQ(total_exits(d), 1u);   // only the return to the host
            FX_CHECK_EQ(d.exits[kReturn], 1u);
        }
    }
    FX_CHECK_EQ(log.calls, 3);
    // Called directly from the host (call_guest): returns to x30 = kReturnToHostPc.
    const std::uint64_t args[] = {2};
    FX_CHECK_EQ(call_guest(callee, args), 7u);
    // A callback that changes ctx.pc continues there instead of at x30.
    const auto target = code.at(32);
    set_region_factory(target, [&](std::uint64_t) {
        RegionBuilder b(target);
        b.entry(target);
        add_x0(b, 1000);
        b.return_via_x30();
        return b.finish();
    });
    log.redirect = target;
    ctx.x[0] = 5;
    call_guest_prepared(ctx, pm);
    FX_CHECK_EQ(ctx.x[0], 1116u);   // callback, then target (returns to cont), then M
    // Unregistered: M's linked call returns to its unlinked tail, the pc is translated again.
    unregister_host_entry(callee);
    FX_CHECK(rt::find_entry(callee) == nullptr);
    set_region_factory(callee, [&](std::uint64_t) {
        RegionBuilder b(callee);
        b.entry(callee);
        add_x0(b, 7);
        b.return_via_x30();
        return b.finish();
    });
    log.redirect = 0;
    const int calls = log.calls;
    ctx.x[0] = 5;
    call_guest_prepared(ctx, pm);
    FX_CHECK_EQ(ctx.x[0], 112u);
    FX_CHECK_EQ(log.calls, calls);
    set_region_factory(callee, nullptr);
    set_region_factory(pm, nullptr);
    set_region_factory(target, nullptr);
}

namespace {
int g_leaf_calls = 0;
// Even x0: halves it (handled). Odd x0: declines, changing nothing.
bool halve_even(GuestContext& ctx) {
    ++g_leaf_calls;
    if (ctx.x[0] & 1) return false;
    ctx.x[0] /= 2;
    return true;
}
} // namespace

// register_host_leaf: the leaf runs from translated code without a dispatcher
// exit and returns through the RSB; when it declines, the callback runs as a
// register_host_callback entry would (ctx.pc = return address).
FX_TEST(host_leaf_runs_or_falls_back_to_its_callback) {
    init();
    GuestCode code({});
    const auto pm = code.at(0), cont = code.at(2), callee = code.at(16);
    CallbackLog log;
    register_host_leaf(callee, &halve_even, &host_callback, &log);
    // M: bl callee; x0 += 100; ret.
    set_region_factory(pm, [&](std::uint64_t) {
        RegionBuilder b(pm);
        b.entry(pm);
        auto& a = b.as();
        a.mov(64, Gp::rax, ptr(Gp::r15, x_disp(30)));
        a.mov(64, ptr(Gp::r15, x_disp(19)), Gp::rax);
        const Label continuation = a.new_label();
        b.call(cont, continuation, callee);
        a.bind(continuation);
        b.entry(cont);
        add_x0(b, 100);
        a.mov(64, Gp::rax, ptr(Gp::r15, x_disp(19)));
        a.mov(64, ptr(Gp::r15, x_disp(30)), Gp::rax);
        b.ret(9);
        b.cover(pm, pm + 12);
        return b.finish();
    });
    GuestContext& ctx = ensure_thread_context();
    g_leaf_calls = 0;
    for (int run = 0; run < 3; ++run) {
        const auto s0 = runtime_stats();
        ctx.x[0] = 10;
        call_guest_prepared(ctx, pm);
        FX_CHECK_EQ(ctx.x[0], 105u);           // 10 / 2 + 100: the leaf
        if (run > 0) {
            const auto d = delta(s0, runtime_stats());
            FX_CHECK_EQ(total_exits(d), 1u);   // only the return to the host
        }
    }
    FX_CHECK_EQ(log.calls, 0);
    ctx.x[0] = 5;
    call_guest_prepared(ctx, pm);
    FX_CHECK_EQ(ctx.x[0], 116u);              // 5 * 3 + 1 + 100: the callback
    FX_CHECK_EQ(log.calls, 1);
    FX_CHECK_EQ(log.pc_seen, cont);
    FX_CHECK_EQ(g_leaf_calls, 4);
    // Called directly from the host (call_guest): returns to x30 = kReturnToHostPc.
    const std::uint64_t even[] = {8}, odd[] = {3};
    FX_CHECK_EQ(call_guest(callee, even), 4u);
    FX_CHECK_EQ(call_guest(callee, odd), 10u);
    FX_CHECK_EQ(log.calls, 2);
    unregister_host_entry(callee);
    set_region_factory(pm, nullptr);
}

// ---- process exit ---------------------------------------------------------------------------------
// Keep last: runtime_prepare_exit stops the preloaders for good.

namespace {

struct InstallGate {
    static inline std::atomic<std::uint64_t> pc{0};
    static inline std::atomic<DWORD> thread{0};
    static inline HANDLE entered = nullptr;
    static inline HANDLE release = nullptr;
    static void hook(std::uint64_t at) {
        if (at != pc.load()) return;
        thread.store(GetCurrentThreadId());
        SetEvent(entered);
        WaitForSingleObject(release, INFINITE);
    }
    explicit InstallGate(std::uint64_t at) {
        entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        pc.store(at);
        rt::set_install_hook_for_tests(&hook);
    }
    ~InstallGate() {
        rt::set_install_hook_for_tests(nullptr);
        SetEvent(release);
        pc.store(0);
        CloseHandle(entered);
        CloseHandle(release);
    }
};

HANDLE start_thread(std::function<void()>* fn) {
    return CreateThread(nullptr, 0, [](void* p) -> DWORD {
        (*static_cast<std::function<void()>*>(p))();
        return 0;
    }, fn, 0, nullptr);
}

} // namespace

FX_TEST(process_exit_parks_a_preloader_holding_the_map_lock) {
    init();
    DormantExit p;
    set_cached_region(p.pa, [&](std::uint64_t) { return DormantExit::region_b(p.pa); });
    set_cached_region(p.pb, [&](std::uint64_t) { return DormantExit::region_b(p.pb); });
    set_cached_starts(p.code.base(), {p.pa, p.pb});
    const auto s0 = runtime_stats();
    {
        // A preloader stops inside install (exclusive map lock held) when exit starts.
        InstallGate gate(p.pa);
        FX_REQUIRE(notify_module_ready(p.code.base(), 100));
        FX_REQUIRE(WaitForSingleObject(InstallGate::entered, 5000) == WAIT_OBJECT_0);
        FX_CHECK(!rt::region_locks_free());
        std::function<void()> prepare = [] { runtime_prepare_exit(); };
        HANDLE exiting = start_thread(&prepare);
        // runtime_prepare_exit waits for the preloader to leave the region...
        FX_CHECK_EQ(WaitForSingleObject(exiting, 100), static_cast<DWORD>(WAIT_TIMEOUT));
        SetEvent(InstallGate::release);
        // ...and returns once it did, before it starts another region.
        FX_CHECK_EQ(WaitForSingleObject(exiting, 5000), static_cast<DWORD>(WAIT_OBJECT_0));
        CloseHandle(exiting);
    }
    // ExitProcess may now terminate the preloaders: nothing is left held.
    FX_CHECK(rt::region_locks_free());
    FX_CHECK(runtime_exit_safe());
    FX_CHECK_EQ(runtime_stats().preloaded - s0.preloaded, 1u);
    Sleep(50);
    FX_CHECK(!rt::find_entry(p.pb));               // stopped: pb was never preloaded
    FX_CHECK(!notify_module_ready(p.code.base(), 101) || !rt::find_entry(p.pb));
    set_cached_starts(p.code.base(), {});
    set_cached_region(p.pa, nullptr);
    set_cached_region(p.pb, nullptr);
}

FX_TEST(process_exit_detects_a_lock_left_by_a_terminated_thread) {
    init();
    // Without parking, ExitProcess terminates a thread wherever it is. Here a
    // guest thread is stopped (as ExitProcess would leave it) inside install,
    // holding the map lock: the next translation miss would block forever,
    // which is what guest DLL_PROCESS_DETACH would run into.
    DormantExit p;
    set_region_factory(p.pa, [&](std::uint64_t) { return DormantExit::region_b(p.pa); });
    set_region_factory(p.pb, [&](std::uint64_t) { return DormantExit::region_b(p.pb); });
    InstallGate gate(p.pa);
    std::function<void()> guest = [&] {
        const std::uint64_t args[] = {0};
        call_guest(p.pa, args);
    };
    HANDLE victim = start_thread(&guest);
    FX_REQUIRE(WaitForSingleObject(InstallGate::entered, 5000) == WAIT_OBJECT_0);
    HANDLE suspended = OpenThread(THREAD_SUSPEND_RESUME, FALSE, InstallGate::thread.load());
    FX_REQUIRE(suspended && SuspendThread(suspended) != static_cast<DWORD>(-1));
    FX_CHECK(!runtime_exit_safe());                 // reported, never waited for
    std::function<void()> detach = [&] { (void)rt::find_entry(p.pb); };
    HANDLE detach_thread = start_thread(&detach);
    FX_CHECK_EQ(WaitForSingleObject(detach_thread, 200), static_cast<DWORD>(WAIT_TIMEOUT));   // the deadlock
    ResumeThread(suspended);
    SetEvent(InstallGate::release);
    FX_CHECK_EQ(WaitForSingleObject(victim, 5000), static_cast<DWORD>(WAIT_OBJECT_0));
    FX_CHECK_EQ(WaitForSingleObject(detach_thread, 5000), static_cast<DWORD>(WAIT_OBJECT_0));
    FX_CHECK(runtime_exit_safe());
    CloseHandle(suspended);
    CloseHandle(victim);
    CloseHandle(detach_thread);
    set_region_factory(p.pa, nullptr);
    set_region_factory(p.pb, nullptr);
}

// ---- tiering -------------------------------------------------------------------------------
namespace {

// A tier-0 region at pc: counts entries (dec [rip+counter]; jz -> tier_up
// exit), then x0 += add; ret through the RSB. The counter's rel32 initially
// addresses a slot at the end of the code (the runtime repoints it).
TranslatedRegion tier0_region(std::uint64_t pc, std::int32_t add) {
    RegionBuilder b(pc);
    b.entry(pc);
    auto& a = b.as();
    a.db(0xff);
    a.db(0x0d);
    const auto counter = static_cast<std::uint32_t>(a.size());
    a.dd(0);
    const Label up = a.new_label();
    a.jcc(x64::Cond::e, up);
    add_x0(b, add);
    b.ret(9);
    a.bind(up);
    b.set_pc(pc);
    a.mov(32, ptr(Gp::r15, d_exit_reason), static_cast<std::int32_t>(ExitReason::tier_up));
    a.jmp(ptr(Gp::r15, d_stub_exit));
    a.align(4);
    const auto slot = static_cast<std::uint32_t>(a.size());
    a.dd(3);
    b.cover(pc, pc + 8);
    TranslatedRegion r = b.finish();
    const auto rel = static_cast<std::int32_t>(slot) - static_cast<std::int32_t>(counter + 4);
    std::memcpy(r.code.data() + counter, &rel, 4);
    r.tier = 0;
    r.tier_counter = counter;
    r.tier_counter_slot = slot;
    return r;
}

TranslatedRegion tier1_region(std::uint64_t pc, std::int32_t add) {
    RegionBuilder b(pc);
    b.entry(pc);
    add_x0(b, add);
    b.ret(9);
    b.cover(pc, pc + 8);
    return b.finish();
}

} // namespace

FX_TEST(tier_up_replaces_entries_links_lookup_and_rsb) {
    init();
    GuestCode code({});
    // M at pm: x19 = x30; bl F; x30 = x19; x0 += 100; ret.   F at pf: x0 += 1 (tier 0) / 2 (tier 1).
    const auto pm = code.at(0), cont = code.at(2), pf = code.at(16);
    std::uint32_t call_field = 0;
    set_region_factory(pm, [&](std::uint64_t) {
        RegionBuilder b(pm);
        b.entry(pm);
        auto& a = b.as();
        a.mov(64, Gp::rax, ptr(Gp::r15, x_disp(30)));
        a.mov(64, ptr(Gp::r15, x_disp(19)), Gp::rax);
        const Label continuation = a.new_label();
        b.call(cont, continuation, pf);
        call_field = b.as().size() >= 4 ? static_cast<std::uint32_t>(b.as().size() - 4) : 0;
        a.bind(continuation);
        b.entry(cont);
        add_x0(b, 100);
        a.mov(64, Gp::rax, ptr(Gp::r15, x_disp(19)));
        a.mov(64, ptr(Gp::r15, x_disp(30)), Gp::rax);
        b.ret(9);
        b.cover(pm, pm + 12);
        return b.finish();
    });
    int tier0_built = 0, tier1_built = 0;
    set_region_factory(pf, [&](std::uint64_t) {
        if (g_last_lazy.load()) {   // first translations are lazy (tier 0)
            ++tier0_built;
            return tier0_region(pf, 1);
        }
        ++tier1_built;
        return tier1_region(pf, 2);
    });
    GuestContext& ctx = ensure_thread_context();
    const auto run = [&] {
        ctx.x[0] = 0;
        call_guest_prepared(ctx, pm);
        return ctx.x[0];
    };
    FX_CHECK_EQ(run(), 101u);
    const void* f0 = rt::find_entry(pf);
    FX_REQUIRE(f0 != nullptr);
    FX_CHECK_EQ(tier0_built, 1);
    // The counter lives outside the region's code (never next to code).
    {
        const auto r0 = tier0_region(pf, 1);
        const auto* counter = static_cast<const std::uint8_t*>(rel32_target(f0, r0.tier_counter));
        const auto* begin = static_cast<const std::uint8_t*>(f0);
        FX_CHECK(counter < begin || counter >= begin + r0.code.size());
    }
    // A stale RSB entry returning into F's tier-0 entry is moved with it.
    const auto slot = (ctx.rsb_top + 5) % kRsbEntries;
    ctx.rsb[slot] = {pf, f0};
    const auto s0 = runtime_stats();
    std::uint64_t first_tier1 = 0;
    for (std::uint64_t i = 1; i <= 5000 && !first_tier1; ++i)
        if (run() == 102u) first_tier1 = i;
    FX_CHECK(first_tier1 >= 4000 && first_tier1 <= 4200);   // default threshold: 4096 entries
    const auto s1 = runtime_stats();
    FX_CHECK_EQ(s1.tier_ups - s0.tier_ups, 1u);
    // F's tier-0 region lost its only entry: retired (its chunk counts it as
    // dead, so reclaim may take the chunk back), not killed.
    FX_CHECK_EQ(s1.retired_regions - s0.retired_regions, 1u);
    FX_CHECK_EQ(s1.invalidated_regions, s0.invalidated_regions);
    FX_CHECK_EQ(tier1_built, 1);
    FX_CHECK_EQ(s1.exits[static_cast<unsigned>(ExitReason::tier_up)] - s0.exits[static_cast<unsigned>(ExitReason::tier_up)], 1u);
    const void* f1 = rt::find_entry(pf);
    FX_REQUIRE(f1 != nullptr && f1 != f0);
    FX_CHECK(rt::lookup_probe(pf) == f1);
    FX_CHECK(ctx.rsb[slot].host_entry == f1);
    // M's call is linked to the tier-1 entry; steady state has no exits but the return.
    const void* m = rt::find_entry(pm);
    FX_REQUIRE(m != nullptr);
    std::size_t linked_to_f1 = 0;
    for (std::uint32_t off = 0; off + 4 <= 0x200; off += 4)
        if (rel32_target(m, off) == f1) ++linked_to_f1;
    FX_CHECK(linked_to_f1 >= 1u);
    const auto s2 = runtime_stats();
    for (int i = 0; i < 10; ++i) FX_CHECK_EQ(run(), 102u);
    const auto d = delta(s2, runtime_stats());
    FX_CHECK_EQ(total_exits(d), 10u);
    FX_CHECK_EQ(d.exits[kReturn], 10u);
    FX_CHECK_EQ(d.translations, 0u);
    // SMC invalidation reaches both tiers (the retired tier-0 region stays registered on its pages).
    FX_CHECK_EQ(rt::live_regions_on_page(pf), 3u);
    const auto s3 = runtime_stats();
    invalidate_guest_code(pf, 4);
    FX_CHECK_EQ(delta(s3, runtime_stats()).invalidated_regions, 2u);
    FX_CHECK_EQ(rt::live_regions_on_page(pf), 1u);
    FX_CHECK(rt::find_entry(pf) == nullptr);
    FX_CHECK_EQ(run(), 101u);   // retranslated as tier 0 again
    FX_CHECK_EQ(tier0_built, 2);
    (void)call_field;
    set_region_factory(pm, nullptr);
    set_region_factory(pf, nullptr);
}

FX_TEST(tier1_install_keeps_entries_of_equal_or_higher_tiers) {
    init();
    GuestCode code({});
    const auto pa = code.at(0), pb = code.at(8);
    // Tier-1 region A also provides an entry for pb; B (tier 1) was installed first and keeps pb.
    set_region_factory(pb, [&](std::uint64_t) { return tier1_region(pb, 10); });
    set_region_factory(pa, [&](std::uint64_t) {
        RegionBuilder b(pa);
        b.entry(pa);
        add_x0(b, 1);
        b.ret(9);
        b.entry(pb);
        add_x0(b, 20);
        b.ret(9);
        b.cover(pa, pa + 12);
        return b.finish();
    });
    const std::uint64_t args[] = {0};
    FX_CHECK_EQ(call_guest(pb, args), 10u);
    FX_CHECK_EQ(call_guest(pa, args), 1u);
    FX_CHECK_EQ(call_guest(pb, args), 10u);   // the first translation of pb keeps it
    set_region_factory(pa, nullptr);
    set_region_factory(pb, nullptr);
}
