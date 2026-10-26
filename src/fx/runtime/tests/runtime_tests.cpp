// Runtime unit tests and guest programs run through the interpret-only
// translate_region stand-in (every instruction exits to interpret_one).

#include "fx/runtime/tests/test_support.hpp"
#include "fx/testing/harness.hpp"
#include "fx/testing/test.hpp"

#include <windows.h>

#include <thread>

using namespace rtt;
namespace rt = transmute::fx::rt;

namespace {

void init() { runtime_initialize(RuntimeOptions{}); }

// A64 encodings used below.
constexpr std::uint32_t kRet = 0xd65f03c0;
constexpr std::uint32_t kAddX0X0X1 = 0x8b010000;
constexpr std::uint32_t movz(unsigned rd, std::uint32_t imm) { return 0xd2800000u | (imm << 5) | rd; }
constexpr std::uint32_t add_x0_x0(unsigned rm) { return 0x8b000000u | (rm << 16); }
constexpr std::uint32_t bl(std::int32_t words) { return 0x94000000u | (static_cast<std::uint32_t>(words) & 0x3ffffff); }

std::uint64_t host_add3(std::uint64_t a, std::uint64_t b, std::uint64_t c) { return a + b * 10 + c * 100; }

} // namespace

FX_TEST(arena_allocations_are_aligned_and_in_reach) {
    init();
    const auto base = reinterpret_cast<std::uint64_t>(runtime_arena_base());
    for (std::size_t alignment : {16u, 64u, 4096u}) {
        auto* p = allocate_code(100, alignment);
        const auto address = reinterpret_cast<std::uint64_t>(p);
        FX_CHECK_EQ(address % alignment, 0u);
        FX_CHECK(address - base < rt::kArenaReserve);
        std::memset(p, 0xcc, 100);   // writable and committed
    }
    // A large allocation commits more chunks.
    auto* big = static_cast<std::uint8_t*>(allocate_code(3 * rt::kArenaCommitChunk));
    big[3 * rt::kArenaCommitChunk - 1] = 0xc3;
    FX_CHECK(rt::in_arena(reinterpret_cast<std::uint64_t>(big)));
}

FX_TEST(arena_has_unwind_info) {
    init();
    DWORD64 image_base = 0;
    auto* code = allocate_code(16);
    const auto* function = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(code), &image_base, nullptr);
    FX_REQUIRE(function != nullptr);
    FX_CHECK_EQ(image_base, reinterpret_cast<DWORD64>(runtime_arena_base()));
    const auto* enter = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(rt::g_stubs.enter) + 2, &image_base, nullptr);
    FX_REQUIRE(enter != nullptr);
    FX_CHECK_EQ(image_base + enter->BeginAddress, reinterpret_cast<DWORD64>(rt::g_stubs.enter));
}

FX_TEST(lookup_table_install_probe_alias_remove) {
    init();
    const std::uint64_t pc = 0x0000'0123'4560'0000ULL;
    const void* a = rt::make_trampoline(&host_add3);
    const void* b = rt::make_trampoline(&host_add3);
    rt::lookup_install(pc, a);
    FX_CHECK(rt::lookup_probe(pc) == a);
    FX_CHECK(rt::lookup_probe(pc + 2) == nullptr);       // misaligned never hits
    // Find another pc with the same slot: it evicts, and neither aliases.
    std::uint64_t alias = pc + (std::uint64_t{1} << kLookupTagShift);
    while (rt::lookup_index(alias) != rt::lookup_index(pc)) alias += 4;
    FX_CHECK(rt::lookup_probe(alias) == nullptr);
    rt::lookup_install(alias, b);
    FX_CHECK(rt::lookup_probe(alias) == b);
    FX_CHECK(rt::lookup_probe(pc) == nullptr);
    rt::lookup_remove(pc, a);                              // not present: no effect
    FX_CHECK(rt::lookup_probe(alias) == b);
    rt::lookup_remove(alias, b);
    FX_CHECK(rt::lookup_probe(alias) == nullptr);
    // Non-canonical pcs (tag beyond 32 bits) are never installed.
    rt::lookup_install(0x8000'0000'0000'0000ULL, a);
    FX_CHECK(rt::lookup_probe(0x8000'0000'0000'0000ULL) == nullptr);
}

FX_TEST(mxcsr_mapping) {
    FX_CHECK_EQ(mxcsr_for_guest(0, 0), 0x1f80u);
    FX_CHECK_EQ(mxcsr_for_guest(1u << 22, 0), 0x5f80u);    // RP -> round up
    FX_CHECK_EQ(mxcsr_for_guest(2u << 22, 0), 0x3f80u);    // RM -> round down
    FX_CHECK_EQ(mxcsr_for_guest(3u << 22, 0), 0x7f80u);    // RZ
    FX_CHECK_EQ(mxcsr_for_guest(1u << 24, 0), 0x9fc0u);    // FZ -> FTZ | DAZ
    FX_CHECK_EQ(mxcsr_for_guest(0, 0x1f), 0x1f80u | 0x3du);
    FX_CHECK_EQ(fpsr_with_mxcsr_flags(0x08000080u, 0x1f80u | 0x3fu), 0x0800009fu);
    FX_CHECK_EQ(fpsr_with_mxcsr_flags(0x11u, 0x1f80u | 0x04u), 0x13u);
    FX_CHECK_EQ(fpsr_with_mxcsr_flags(0, 0x1f80u | 0x02u), 0u);   // DE has no FPSR bit
}

FX_TEST(pdata_function_bounds_packed_and_xdata) {
    init();
    transmute::fx::testing::Buffer image(0x2000);
    auto* words = reinterpret_cast<std::uint32_t*>(image.data());
    // .pdata at 0x1000: {0x100, packed 0x10 words}, {0x200, xdata at 0x800}
    words[0x1000 / 4 + 0] = 0x100;
    words[0x1000 / 4 + 1] = (0x10u << 2) | 1;
    words[0x1000 / 4 + 2] = 0x200;
    words[0x1000 / 4 + 3] = 0x800;
    words[0x800 / 4] = 0x20 | (1u << 20);   // FunctionLength = 0x20 words, other header bits set
    GuestModule module;
    module.base = image.address();
    module.size = image.size();
    module.pdata_rva = 0x1000;
    module.pdata_size = 16;
    std::uint64_t begin = 0, end = 0;
    FX_CHECK(rt::pdata_function_bounds(module, module.base + 0x110, begin, end));
    FX_CHECK_EQ(begin, module.base + 0x100);
    FX_CHECK_EQ(end, module.base + 0x140);
    FX_CHECK(!rt::pdata_function_bounds(module, module.base + 0x150, begin, end));
    FX_CHECK(rt::pdata_function_bounds(module, module.base + 0x27c, begin, end));
    FX_CHECK_EQ(begin, module.base + 0x200);
    FX_CHECK_EQ(end, module.base + 0x280);
    FX_CHECK(!rt::pdata_function_bounds(module, module.base + 0x80, begin, end));
    // Through the live CodeSource and find_guest_module.
    module.text_begin = module.base;
    module.text_end = module.base + 0x1000;
    set_fake_module(&module);
    rt::LiveCodeSource source;
    FX_CHECK(source.function_bounds(module.base + 0x204, begin, end));
    FX_CHECK_EQ(begin, module.base + 0x200);
    std::uint32_t word = 0;
    FX_CHECK(source.read(module.base + 0x800, word));
    FX_CHECK_EQ(word, 0x20u | (1u << 20));
    FX_CHECK(!source.read(module.base + 0x1000, word));   // outside text
    set_fake_module(nullptr);
}

// CodeSource::image_code for target_flags_dead: a module's text is image code
// also when the personality registers it as guest code memory (Alchemy maps
// Mach-O __TEXT that way); its data and other registered code memory (the
// guest's JIT) are not.
FX_TEST(live_code_source_image_code) {
    init();
    GuestModule module;
    module.base = 0x0000'0556'0000'0000ULL;
    module.size = 0x4000;
    module.text_begin = module.base + 0x1000;
    module.text_end = module.base + 0x3000;
    set_fake_module(&module);
    const std::uint64_t jit = 0x0000'0557'0000'0000ULL;
    register_guest_code_memory(module.base, module.size);
    register_guest_code_memory(jit, 0x2000);
    rt::LiveCodeSource source;
    FX_CHECK(source.image_code(module.text_begin));
    FX_CHECK(source.image_code(module.text_end - 4));
    FX_CHECK(!source.image_code(module.text_end));   // registered, but the module's data
    FX_CHECK(!source.image_code(module.base));
    FX_CHECK(!source.image_code(jit));               // registered code memory outside any module
    unregister_guest_code_memory(module.base, module.size);
    unregister_guest_code_memory(jit, 0x2000);
    set_fake_module(nullptr);
}

FX_TEST(guest_code_ranges) {
    init();
    const std::uint64_t base = 0x0000'0555'0000'0000ULL;
    FX_CHECK(!is_guest_code(base));
    register_guest_code_memory(base, 0x3000);
    register_guest_code_memory(base + 0x2000, 0x2000);    // overlapping: merged
    FX_CHECK(is_guest_code(base));
    FX_CHECK(is_guest_code(base + 0x3ffc));
    FX_CHECK(!is_guest_code(base + 0x4000));
    unregister_guest_code_memory(base + 0x1000, 0x1000);  // punch a hole
    FX_CHECK(is_guest_code(base + 0xffc));
    FX_CHECK(!is_guest_code(base + 0x1000));
    FX_CHECK(is_guest_code(base + 0x2000));
    unregister_guest_code_memory(base, 0x4000);
    FX_CHECK(!is_guest_code(base));
    FX_CHECK(!is_guest_code(base + 0x2000));
}

FX_TEST(interpret_add_and_return) {
    init();
    GuestCode code({kAddX0X0X1, kRet});
    const std::uint64_t args[] = {2, 40};
    FX_CHECK_EQ(call_guest(code.at(0), args), 42u);
}

FX_TEST(interpret_loop_sum) {
    init();
    GuestCode code({
        movz(0, 0),        // x0 = 0
        add_x0_x0(2),      // loop: x0 += x2
        0xd1000442,        // sub x2, x2, #1
        0xb5ffffc2,        // cbnz x2, loop
        kRet,
    });
    const std::uint64_t args[] = {0, 0, 100};
    FX_CHECK_EQ(call_guest(code.at(0), args), 5050u);
}

FX_TEST(interpret_bl_to_leaf) {
    init();
    GuestCode code({
        0xa9bf7bfd,        // stp x29, x30, [sp, #-16]!
        0x910003fd,        // mov x29, sp
        bl(3),             // bl leaf
        0xa8c17bfd,        // ldp x29, x30, [sp], #16
        kRet,
        kAddX0X0X1,        // leaf: add x0, x0, x1
        kRet,
    });
    GuestContext& ctx = ensure_thread_context();
    const auto sp = ctx.x[31];
    const std::uint64_t args[] = {7, 8};
    FX_CHECK_EQ(call_guest(code.at(0), args), 15u);
    FX_CHECK_EQ(ctx.x[31], sp);
}

FX_TEST(call_guest_stack_arguments) {
    init();
    GuestCode code({
        add_x0_x0(1), add_x0_x0(2), add_x0_x0(3), add_x0_x0(4), add_x0_x0(5), add_x0_x0(6), add_x0_x0(7),
        0xf94003e9,        // ldr x9, [sp]
        add_x0_x0(9),
        0xf94007e9,        // ldr x9, [sp, #8]
        add_x0_x0(9),
        kRet,
    });
    const std::uint64_t args[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    FX_CHECK_EQ(call_guest(code.at(0), args), 55u);
}

FX_TEST(call_guest_preserves_callee_saved_state) {
    init();
    GuestCode code({
        movz(19, 7), movz(20, 7), movz(29, 7), movz(30, 7),   // clobber callee-saved (ABI violation)
        0x910403ff,        // add sp, sp, #0x100
        0x1e601008,        // fmov d8, #2.0
        movz(0, 1),
        0xd61f0200,        // br x16 (x30 is clobbered)
    });
    GuestContext& ctx = ensure_thread_context();
    ctx.x[19] = 0x1919;
    ctx.x[20] = 0x2020;
    ctx.x[29] = 0x2929;
    ctx.v[8][0] = 0x8888;
    ctx.x[16] = kReturnToHostPc;
    const auto sp = ctx.x[31];
    const auto pc = ctx.pc;
    call_guest_prepared(ctx, code.at(0));
    FX_CHECK_EQ(ctx.x[0], 1u);
    FX_CHECK_EQ(ctx.x[19], 0x1919u);
    FX_CHECK_EQ(ctx.x[20], 0x2020u);
    FX_CHECK_EQ(ctx.x[29], 0x2929u);
    FX_CHECK_EQ(ctx.v[8][0], 0x8888u);
    FX_CHECK_EQ(ctx.x[31], sp);
    FX_CHECK_EQ(ctx.pc, pc);
}

FX_TEST(guest_exception_illegal_instruction) {
    init();
    clear_exception_log();
    GuestCode code({0x00000000 /* udf #0 */, kRet});
    const auto result = call_guest(code.at(0), {});
    FX_CHECK_EQ(result, kUnhandledMarker | 0xC000001Du);
    const auto log = exception_log();
    FX_REQUIRE(log.size() == 1);
    FX_CHECK_EQ(log[0].code, 0xC000001Du);
    FX_CHECK_EQ(log[0].address, code.at(0));
}

FX_TEST(guest_exception_breakpoint_resumes) {
    init();
    clear_exception_log();
    GuestCode code({0xd43e0000 /* brk #0xf000 */, movz(0, 5), kRet});
    set_exception_handler([](GuestContext& ctx, const GuestExceptionRecord& record) {
        if (record.code == 0x80000003u) ctx.pc += 4;   // continue after the breakpoint
    });
    FX_CHECK_EQ(call_guest(code.at(0), {}), 5u);
    set_exception_handler(nullptr);
    const auto log = exception_log();
    FX_REQUIRE(log.size() == 1);
    FX_CHECK_EQ(log[0].code, 0x80000003u);
    FX_CHECK_EQ(log[0].address, code.at(0));
}

FX_TEST(guest_exception_interpreted_access_violation) {
    init();
    clear_exception_log();
    GuestCode code({0xf9400020 /* ldr x0, [x1] */, kRet});
    const std::uint64_t args[] = {0, 0x10};
    FX_CHECK_EQ(call_guest(code.at(0), args), kUnhandledMarker | 0xC0000005u);
    const auto log = exception_log();
    FX_REQUIRE(log.size() == 1);
    FX_CHECK_EQ(log[0].code, 0xC0000005u);
    FX_CHECK_EQ(log[0].address, code.at(0));
    FX_CHECK_EQ(log[0].parameter_count, 2u);
    FX_CHECK_EQ(log[0].parameters[0], 0u);     // read
    FX_CHECK_EQ(log[0].parameters[1], 0x10u);
}

FX_TEST(guest_exception_execute_non_code) {
    init();
    clear_exception_log();
    transmute::fx::testing::Buffer data(0x1000);
    GuestCode code({0xd61f0100 /* br x8 */});
    GuestContext& ctx = ensure_thread_context();
    ctx.x[8] = data.address();
    call_guest_prepared(ctx, code.at(0));
    FX_CHECK_EQ(ctx.x[0], kUnhandledMarker | 0xC0000005u);
    const auto log = exception_log();
    FX_REQUIRE(log.size() == 1);
    FX_CHECK_EQ(log[0].address, data.address());
    FX_CHECK_EQ(log[0].parameters[0], 8u);     // execute
    FX_CHECK_EQ(log[0].parameters[1], data.address());
}

FX_TEST(guest_exception_misaligned_pc) {
    init();
    clear_exception_log();
    GuestCode code({0xd61f0100 /* br x8 */});
    GuestContext& ctx = ensure_thread_context();
    ctx.x[8] = code.at(4) + 2;
    call_guest_prepared(ctx, code.at(0));
    FX_CHECK_EQ(ctx.x[0], kUnhandledMarker | 0x80000002u);
}

FX_TEST(system_registers_in_interpreter) {
    init();
    GuestCode code({0xd51bd041 /* msr tpidr_el0, x1 */, 0xd53bd040 /* mrs x0, tpidr_el0 */, kRet});
    const std::uint64_t args[] = {0, 0xabcdef};
    FX_CHECK_EQ(call_guest(code.at(0), args), 0xabcdefu);
    FX_CHECK_EQ(current_context()->tpidr_el0, 0xabcdefu);
}

FX_TEST(unknown_host_target_is_bound) {
    init();
    GuestCode code({
        0xa9bf7bfd,        // stp x29, x30, [sp, #-16]!
        0xd63f0100,        // blr x8 (host function)
        0xa8c17bfd,        // ldp x29, x30, [sp], #16
        kRet,
    });
    GuestContext& ctx = ensure_thread_context();
    ctx.x[0] = 1;
    ctx.x[1] = 2;
    ctx.x[2] = 3;
    ctx.x[8] = reinterpret_cast<std::uint64_t>(&host_add3);
    call_guest_prepared(ctx, code.at(0));
    FX_CHECK_EQ(ctx.x[0], 321u);
    // Second call goes straight through the registered host entry.
    ctx.x[0] = 4; ctx.x[1] = 0; ctx.x[2] = 0;
    call_guest_prepared(ctx, code.at(0));
    FX_CHECK_EQ(ctx.x[0], 4u);
}

namespace {
std::uint64_t g_leaf = 0;
std::uint64_t host_calls_guest(std::uint64_t a) {
    const std::uint64_t args[] = {a, 5};
    return call_guest(g_leaf, args) * 2;
}
} // namespace

FX_TEST(nested_call_guest_from_host_callback) {
    init();
    GuestCode leaf({movz(19, 7), kAddX0X0X1, kRet});     // clobbers x19: restored by call_guest
    g_leaf = leaf.at(0);
    GuestCode code({
        0xa9bf7bfd,        // stp x29, x30, [sp, #-16]!
        movz(19, 0x1919),
        0xd63f0100,        // blr x8 -> host_calls_guest -> call_guest(leaf)
        add_x0_x0(19),
        0xa8c17bfd,        // ldp x29, x30, [sp], #16
        kRet,
    });
    GuestContext& ctx = ensure_thread_context();
    ctx.x[0] = 1;
    ctx.x[8] = reinterpret_cast<std::uint64_t>(&host_calls_guest);
    const auto nesting = ctx.nesting;
    call_guest_prepared(ctx, code.at(0));
    FX_CHECK_EQ(ctx.x[0], (1u + 5u) * 2u + 0x1919u);
    FX_CHECK_EQ(ctx.nesting, nesting);
}

FX_TEST(host_call_into_guest_code_becomes_callback) {
    init();
    GuestCode code({kAddX0X0X1, kRet});
    const auto before = runtime_stats().host_callbacks;
    using Fn = std::uint64_t (*)(std::uint64_t, std::uint64_t);
    auto fn = reinterpret_cast<Fn>(code.at(0));
    FX_CHECK_EQ(fn(40, 2), 42u);
    FX_CHECK_EQ(runtime_stats().host_callbacks, before + 1);
    // Six arguments: two come from the host caller's stack.
    GuestCode sum6({add_x0_x0(1), add_x0_x0(2), add_x0_x0(3), add_x0_x0(4), add_x0_x0(5), kRet});
    using Fn6 = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                  std::uint64_t);
    FX_CHECK_EQ(reinterpret_cast<Fn6>(sum6.at(0))(1, 2, 3, 4, 5, 6), 21u);
    // From a host thread that never ran guest code.
    std::uint64_t result = 0;
    const int attach = g_attach_calls.load();
    std::thread([&] { result = fn(1, 2); }).join();
    FX_CHECK_EQ(result, 3u);
    FX_CHECK_EQ(g_attach_calls.load(), attach + 1);
}

FX_TEST(interrupt_request_runs_on_owner_thread) {
    init();
    std::uint64_t flag = 0;
    GuestCode code({
        0xf9400001,        // loop: ldr x1, [x0]
        0xb4ffffe1,        // cbz x1, loop
        kRet,
    });
    std::atomic<GuestContext*> target{nullptr};
    DWORD ran_on = 0;
    DWORD worker_id = 0;
    std::thread worker([&] {
        worker_id = GetCurrentThreadId();
        target.store(&ensure_thread_context());
        const std::uint64_t args[] = {reinterpret_cast<std::uint64_t>(&flag)};
        call_guest(code.at(0), args);
    });
    while (!target.load()) Sleep(0);
    request_interrupt(*target.load(), [&](GuestContext&) {
        ran_on = GetCurrentThreadId();
        flag = 1;
    });
    worker.join();
    FX_CHECK_EQ(flag, 1u);
    FX_CHECK_EQ(ran_on, worker_id);
}

namespace {
std::uint64_t g_svc_seen = 0;
void svc_handler(GuestContext& ctx, std::uint32_t immediate) {
    g_svc_seen = immediate;
    ctx.x[0] = immediate * 2;
}
void dummy_helper(GuestContext*) {}
} // namespace

FX_TEST(svc_hook_and_default) {
    init();
    clear_exception_log();
    GuestCode code({0xd40000a1 /* svc #5 */, kRet});
    set_svc_handler(&svc_handler);
    FX_CHECK_EQ(call_guest(code.at(0), {}), 10u);
    FX_CHECK_EQ(g_svc_seen, 5u);
    set_svc_handler(nullptr);
    FX_CHECK_EQ(call_guest(code.at(0), {}), kUnhandledMarker | 0xC000001Du);
    const auto log = exception_log();
    FX_REQUIRE(log.size() == 1);
    FX_CHECK_EQ(log[0].address, code.at(0));
}

FX_TEST(register_helper_reaches_all_contexts) {
    init();
    GuestContext& ctx = ensure_thread_context();
    FX_CHECK(ctx.helpers[helper_interpret_one] != nullptr);
    FX_CHECK(ctx.stub_indirect == rt::g_stubs.indirect && ctx.stub_link == rt::g_stubs.link &&
             ctx.stub_dispatch_exit == rt::g_stubs.dispatch_exit);
    FX_CHECK(ctx.lookup_table == rt::lookup_table() && ctx.lookup_mask == rt::kLookupMask);
    const unsigned id = helper_misc_first + 3;
    register_helper(id, reinterpret_cast<const void*>(&dummy_helper));
    FX_CHECK(ctx.helpers[id] == reinterpret_cast<const void*>(&dummy_helper));
    const void* seen = nullptr;
    std::thread([&] { seen = ensure_thread_context().helpers[id]; }).join();
    FX_CHECK(seen == reinterpret_cast<const void*>(&dummy_helper));
    register_helper(id, nullptr);
}

FX_TEST(contexts_get_system_helpers_and_code_source_reports_entries) {
    init();
    GuestContext* seen = nullptr;
    const void* first = nullptr;
    const void* last = nullptr;
    std::thread([&] {
        seen = &ensure_thread_context();
        first = seen->helpers[helper_system_first];
        last = seen->helpers[helper_system_first + 15];
    }).join();
    FX_CHECK(seen != nullptr);
    FX_CHECK(first == reinterpret_cast<const void*>(kSystemHelperMarker + helper_system_first));
    FX_CHECK(last == reinterpret_cast<const void*>(kSystemHelperMarker + helper_system_first + 15));

    GuestCode code({kAddX0X0X1, kRet});
    rt::LiveCodeSource source;
    FX_CHECK(!source.has_entry(code.at(0)));
    const std::uint64_t args[] = {1, 1};
    FX_CHECK_EQ(call_guest(code.at(0), args), 2u);
    FX_CHECK(source.has_entry(code.at(0)));      // translated: a new region would stop and link here
    FX_CHECK(source.has_entry(code.at(1)));      // the ret was interpreted after its own region
    FX_CHECK(!source.has_entry(code.at(2)));
}

namespace {
// Demand commit (set_demand_commit_hook): a 64 KiB reservation whose pages
// the hook commits read/write on first touch.
struct DemandRange {
    std::uint64_t base{};
    std::uint64_t size{0x10000};
    unsigned calls{};
    std::uint32_t last_access{~0u};
};
bool demand_hook(std::uint64_t address, std::uint32_t access, void* user) {
    auto& range = *static_cast<DemandRange*>(user);
    ++range.calls;
    range.last_access = access;
    if (address - range.base >= range.size) return false;
    const auto page = address & ~std::uint64_t{0xfff};
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(page), &info, sizeof(info)) || info.State == MEM_COMMIT) return false;
    return VirtualAlloc(reinterpret_cast<void*>(page), 0x1000, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}
bool committed(std::uint64_t address) {
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) && info.State == MEM_COMMIT;
}
} // namespace

FX_TEST(demand_commit_hook_commits_on_first_touch) {
    init();
    DemandRange range;
    range.base = reinterpret_cast<std::uint64_t>(VirtualAlloc(nullptr, range.size, MEM_RESERVE, PAGE_NOACCESS));
    FX_CHECK(range.base != 0);
    set_demand_commit_hook(&demand_hook, &range);
    // A host write and a host read (unguarded, as provider code would) commit
    // only the page they touch.
    *reinterpret_cast<volatile std::uint32_t*>(range.base + 0x2010) = 0x1234;
    FX_CHECK_EQ(range.calls, 1u);
    FX_CHECK_EQ(range.last_access, 1u);
    FX_CHECK_EQ(*reinterpret_cast<volatile std::uint32_t*>(range.base + 0x2010), 0x1234u);
    FX_CHECK_EQ(*reinterpret_cast<volatile std::uint32_t*>(range.base + 0x5000), 0u);
    FX_CHECK_EQ(range.calls, 2u);
    FX_CHECK_EQ(range.last_access, 0u);
    FX_CHECK(committed(range.base + 0x2000) && committed(range.base + 0x5000));
    FX_CHECK(!committed(range.base) && !committed(range.base + 0x3000));
    // The JIT's code reads ask the hook for an uncommitted guest code page.
    register_guest_code_memory(range.base + 0x8000, 0x1000);
    rt::LiveCodeSource source;
    std::uint32_t word = 1;
    FX_CHECK(source.read(range.base + 0x8000, word));
    FX_CHECK_EQ(word, 0u);
    FX_CHECK_EQ(range.last_access, 8u);
    FX_CHECK(committed(range.base + 0x8000));
    unregister_guest_code_memory(range.base + 0x8000, 0x1000);
    // A refused page still faults (guarded reads report it).
    std::uint32_t value = 0;
    range.size = 0x9000;   // the hook refuses pages from here on
    FX_CHECK(!rt::safe_read32(range.base + 0xa000, value));
    FX_CHECK(!committed(range.base + 0xa000));
    set_demand_commit_hook(nullptr, nullptr);
    FX_CHECK(!rt::safe_read32(range.base + 0x1000, value));   // no hook: no commit
    VirtualFree(reinterpret_cast<void*>(range.base), 0, MEM_RELEASE);
}
