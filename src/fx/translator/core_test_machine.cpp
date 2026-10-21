#include "fx/translator/core_test_machine.hpp"

#include "fx/common/interpreter.hpp"
#include "fx/common/x64_assembler.hpp"
#include "fx/runtime/runtime.hpp"   // lookup table encoding (constants only)
#include "fx/testing/harness.hpp"
#include "fx/testing/test.hpp"

#include <cstdio>

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace transmute::fx::coretest {

using x64::Gp;
using x64::ptr;

bool BufferCode::read(std::uint64_t address, std::uint32_t& word) const {
    if (address < begin_ || address + 4 > end_ || (address & 3) != 0) return false;
    std::memcpy(&word, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), 4);
    return true;
}

bool BufferCode::has_entry(std::uint64_t pc) const {
    for (const auto c : covered)
        if (c == pc) return true;
    return false;
}

bool BufferCode::function_bounds(std::uint64_t address, std::uint64_t& begin, std::uint64_t& end) const {
    if (fn_begin == 0 || address < fn_begin || address >= fn_end) return false;
    begin = fn_begin;
    end = fn_end;
    return true;
}

namespace {

void helper_interpret(GuestContext* ctx) {
    try {
        interpret_one(*ctx);
    } catch (...) {
        ctx->pc += 4;
    }
}

} // namespace

TestMachine::TestMachine(TranslateOptions opts, std::size_t arena_bytes) : options(opts), size_(arena_bytes) {
    arena_ = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, size_, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!arena_) throw std::runtime_error("TestMachine: cannot allocate code arena");
    helpers_[helper_interpret_one] = reinterpret_cast<const void*>(&helper_interpret);

    x64::Assembler a;
    const auto host_rsp = ctx_disp(offsetof(GuestContext, host_rsp));
    const auto exit_reason = ctx_disp(offsetof(GuestContext, exit_reason));
    const auto exit_arg = ctx_disp(offsetof(GuestContext, exit_arg));
    constexpr Gp saved[] = {Gp::rbx, Gp::rbp, Gp::rdi, Gp::rsi, Gp::r12, Gp::r13, Gp::r14, Gp::r15};
    constexpr int kFrame = 32 + 160 + 8;   // shadow space + xmm6..15 + pad
    const auto ret_label = a.new_label();
    // Cross-region register convention (translator.hpp): every stub here is
    // entered from translated code, so the convention registers are in their
    // hosts there; store them like the runtime's stubs do. `enter` loads them.
    const auto& conv = translator_register_convention();
    const auto store_globals = [&] {
        for (unsigned g = 0; g < 32; ++g)
            if ((conv.guest_mask >> g) & 1u) a.mov(64, ptr(Gp::r15, x_disp(g)), x64::gp(static_cast<unsigned>(conv.host[g])));
    };
    // Arena offset 0: the lookup miss stub (empty lookup slots decode here,
    // as in the runtime; see fx/runtime/runtime.hpp).
    const auto indirect = a.size();
    store_globals();
    a.mov(32, ptr(Gp::r15, exit_reason), static_cast<std::int32_t>(ExitReason::lookup_miss));
    a.jmp(ret_label);
    // enter(rcx = biased context, rdx = host code)
    a.align(16);
    const auto enter = a.size();
    for (auto r : saved) a.push(r);
    a.sub(64, Gp::rsp, kFrame);
    for (unsigned i = 0; i < 10; ++i) a.movdqu(ptr(Gp::rsp, 32 + 16 * static_cast<int>(i)), x64::xmm(6 + i));
    a.mov(64, Gp::r15, Gp::rcx);
    a.mov(64, ptr(Gp::r15, host_rsp), Gp::rsp);
    for (unsigned g = 0; g < 32; ++g)
        if ((conv.guest_mask >> g) & 1u) a.mov(64, x64::gp(static_cast<unsigned>(conv.host[g])), ptr(Gp::r15, x_disp(g)));
    a.jmp(Gp::rdx);
    // common return
    a.align(16);
    a.bind(ret_label);
    a.mov(64, Gp::rsp, ptr(Gp::r15, host_rsp));
    for (unsigned i = 0; i < 10; ++i) a.movdqu(x64::xmm(6 + i), ptr(Gp::rsp, 32 + 16 * static_cast<int>(i)));
    a.add(64, Gp::rsp, kFrame);
    for (int i = 7; i >= 0; --i) a.pop(saved[i]);
    a.ret();
    a.align(16);
    const auto link_stub = a.size();
    store_globals();
    a.mov(32, ptr(Gp::r15, exit_reason), static_cast<std::int32_t>(ExitReason::link));
    a.mov(64, ptr(Gp::r15, exit_arg), Gp::rax);
    a.jmp(ret_label);
    a.align(16);
    // Translated exits set exit_reason first. Fault tests resume here from
    // their handler with exit_reason still 0 and the state rebuilt from the
    // pins (the runtime's VEH resumes at its non-storing dispatch exit):
    // nothing to store then.
    const auto dispatch_exit = a.size();
    a.cmp(32, ptr(Gp::r15, exit_reason), 0);
    a.jcc(x64::Cond::e, ret_label);
    store_globals();
    a.jmp(ret_label);
    a.finalize();
    lookup_.assign(kLookupSlots, 0);
    auto* stubs = allocate(a.size());
    if (stubs != arena_) throw std::logic_error("TestMachine: stubs must start the arena");
    std::memcpy(stubs, a.data(), a.size());
    enter_ = stubs + enter;
    stub_link_ = stubs + link_stub;
    stub_indirect_ = stubs + indirect;
    stub_exit_ = stubs + dispatch_exit;
    stubs_end_ = used_;
}

TestMachine::~TestMachine() {
    if (arena_) VirtualFree(arena_, 0, MEM_RELEASE);
}

void TestMachine::reset() {
    entries_.clear();
    std::fill(lookup_.begin(), lookup_.end(), 0);
    regions.clear();
    used_ = stubs_end_;
}

std::uint8_t* TestMachine::allocate(std::size_t bytes) {
    used_ = (used_ + 63) & ~std::size_t{63};
    if (used_ + bytes > size_) throw std::runtime_error("TestMachine: code arena exhausted");
    auto* p = arena_ + used_;
    used_ += bytes;
    return p;
}

void TestMachine::prepare(GuestContext& ctx) const {
    ctx.stub_link = stub_link_;
    ctx.stub_indirect = stub_indirect_;
    ctx.stub_indirect_live = stub_indirect_;   // only translated code jumps there
    ctx.stub_host_return = nullptr;            // no host entries
    ctx.stub_dispatch_exit = stub_exit_;
    // Inline indirect-branch lookups (same encoding as the runtime).
    ctx.lookup_table = lookup_.data();
    ctx.lookup_mask = kLookupSlots - 1;
    ctx.arena_base = inline_lookup ? arena_ : nullptr;
    for (unsigned i = 0; i < kHelperSlots; ++i) ctx.helpers[i] = helpers_[i];
}

const void* TestMachine::entry_for(std::uint64_t pc, const CodeSource& code) {
    if (const auto it = entries_.find(pc); it != entries_.end()) return it->second;
    std::uint32_t word;
    if (!code.read(pc, word)) return nullptr;
    return install(translate_region(pc, code, options), false);
}

// Copies `region` into the arena. A tier-0 region keeps its own counter slot
// (the arena is writable). `replace`: its entries take over existing ones (a
// tier-up; the old code stays valid for links into it).
const void* TestMachine::install(TranslatedRegion region, bool replace) {
    auto* host = allocate(region.code.size());
    std::memcpy(host, region.code.data(), region.code.size());
    // Fixups are identity in the JIT: the values are already absolute.
    for (const auto& e : region.entries) {
        if (replace) entries_[e.guest_pc] = host + e.offset;
        else entries_.emplace(e.guest_pc, host + e.offset);
        const auto slot = ((e.guest_pc >> 2) ^ (e.guest_pc >> kLookupTagShift)) & (kLookupSlots - 1);
        lookup_[slot] = ((e.guest_pc >> kLookupTagShift) << 32) | static_cast<std::uint32_t>(host + e.offset - arena_);
    }
    FlushInstructionCache(GetCurrentProcess(), host, region.code.size());
    regions.push_back(std::move(region));
    return host + regions.back().entries[0].offset;
}

void TestMachine::enter(GuestContext& ctx, const void* host) const {
    using EnterFn = void (*)(void*, const void*);
    reinterpret_cast<EnterFn>(const_cast<void*>(enter_))(reinterpret_cast<char*>(&ctx) + kContextBias, host);
}

RunResult TestMachine::run(GuestContext& ctx, const CodeSource& code, std::uint64_t stop_pc, unsigned max_dispatches) {
    prepare(ctx);
    RunResult r;
    for (; r.dispatches < max_dispatches; ++r.dispatches) {
        if (ctx.pc == stop_pc) {
            r.ok = true;
            r.reason = ExitReason::return_to_host;
            return r;
        }
        const auto before = regions.size();
        const auto* host = entry_for(ctx.pc, code);
        r.translations += static_cast<unsigned>(regions.size() - before);
        if (!host) {
            r.error = "unreadable guest pc";
            return r;
        }
        ctx.exit_reason = 0;
        enter(ctx, host);
        switch (static_cast<ExitReason>(ctx.exit_reason)) {
        case ExitReason::link: {
            ++r.links;
            if (link && ctx.pc != stop_pc) {
                const auto before2 = regions.size();
                if (const auto* target = entry_for(ctx.pc, code))
                    x64::Assembler::patch_rel32(reinterpret_cast<void*>(static_cast<std::uintptr_t>(ctx.exit_arg)), target);
                r.translations += static_cast<unsigned>(regions.size() - before2);
            }
            break;
        }
        case ExitReason::lookup_miss: ++r.lookups; break;
        case ExitReason::interpret:
            ++r.interprets;
            try {
                interpret_one(ctx);
            } catch (const std::exception& e) {
                r.error = std::string("interpret failed: ") + e.what();
                return r;
            }
            break;
        case ExitReason::interrupt:
            ++r.interrupts;
            ctx.interrupt_pending = 0;
            break;
        case ExitReason::tier_up: {
            // A tier-0 region ran out of entries: translate pc eagerly and
            // send future entries (lookups, new links) there.
            ++r.tier_ups;
            TranslateOptions eager = options;
            eager.lazy = false;
            const auto before2 = regions.size();
            install(translate_region(ctx.pc, code, eager), true);
            r.translations += static_cast<unsigned>(regions.size() - before2);
            break;
        }
        case ExitReason::tier_up_loop: {
            // A loop of the counted region at exit_arg is hot: that region is
            // translated eagerly; the loop resumes at its header (ctx.pc).
            ++r.tier_ups;
            TranslateOptions eager = options;
            eager.lazy = false;
            const auto before2 = regions.size();
            install(translate_region(ctx.exit_arg, code, eager), true);
            r.translations += static_cast<unsigned>(regions.size() - before2);
            break;
        }
        case ExitReason::brk:
        case ExitReason::svc:
            r.ok = true;
            r.reason = static_cast<ExitReason>(ctx.exit_reason);
            r.arg = ctx.exit_arg;
            return r;
        default:
            r.error = "unexpected exit reason " + std::to_string(ctx.exit_reason);
            return r;
        }
    }
    r.error = "dispatch limit";
    return r;
}

GuestContext test_context(std::uint64_t seed) {
    auto ctx = testing::random_context(seed);
    ctx.rsb_top = 0;
    for (auto& e : ctx.rsb) e = {};
    ctx.exit_reason = 0;
    ctx.interrupt_pending = 0;
    return ctx;
}

} // namespace transmute::fx::coretest

namespace transmute::fx::coretest {

DiffRunner::DiffRunner() {
    // Guest code at an address whose upper 32 bits are unlikely as an
    // immediate or displacement (e.g. a rip-relative disp32 of 0x284 next to
    // other bytes), so the relocatability check below has no false positives.
    for (std::uint64_t hint = 0x6b5a30000000ull; !code_ && hint < 0x6b5a40000000ull; hint += 0x1000000ull)
        code_ = reinterpret_cast<std::uint64_t>(VirtualAlloc(reinterpret_cast<void*>(hint), kCodeBytes,
                                                             MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!code_)
        code_ = reinterpret_cast<std::uint64_t>(VirtualAlloc(nullptr, kCodeBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    data_ = reinterpret_cast<std::uint64_t>(VirtualAlloc(nullptr, kDataBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!code_ || !data_) throw std::runtime_error("DiffRunner: cannot allocate memory");
    pattern_.resize(kDataBytes);
    std::uint64_t state = 0x5eed;
    for (std::size_t i = 0; i < kDataBytes; i += 8) {
        const auto v = testing::random_u64(state);
        std::memcpy(pattern_.data() + i, &v, 8);
    }
    // Literal pool: random words below the program.
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(code_)), pattern_.data(), kProgramOffset);
    expected_memory_.resize(kDataBytes);
    // Random programs pass flags through BL/RET: compare with the strict model.
    machine.options.abi_flags = false;
}

DiffRunner::~DiffRunner() {
    VirtualFree(reinterpret_cast<void*>(static_cast<std::uintptr_t>(code_)), 0, MEM_RELEASE);
    VirtualFree(reinterpret_cast<void*>(static_cast<std::uintptr_t>(data_)), 0, MEM_RELEASE);
}

void DiffRunner::reset_memory() {
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(data_)), pattern_.data(), kDataBytes);
}

GuestContext DiffRunner::context(std::uint64_t seed) const {
    auto ctx = test_context(seed);
    ctx.x[16] = data_ + (128u << 10);
    ctx.x[17] = data_ + (64u << 10);
    ctx.x[31] = data_ + (192u << 10);
    ctx.x[14] = ctx.x[14] & 0xff;
    ctx.x[12] = 1 + (ctx.x[12] & 3);
    ctx.x[30] = kReturnToHostPc;
    ctx.x[18] = 0;
    return ctx;
}

bool DiffRunner::run_translated(const std::vector<std::uint32_t>& words, const GuestContext& initial) {
    auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(program_base()));
    std::memcpy(program, words.data(), words.size() * 4);
    program[words.size()] = 0xd4200000u | (0xdeadu << 5);
    const BufferCode code(code_, code_ + kProgramOffset + (words.size() + 1) * 4);
    reset_memory();
    actual = initial;
    actual.pc = program_base();
    machine.reset();
    last = machine.run(actual, code, kReturnToHostPc);
    return last.ok && last.reason == ExitReason::brk && last.arg == 0xdead;
}

bool DiffRunner::run(const std::vector<std::uint32_t>& words, const GuestContext& initial, const std::string& label,
                     unsigned max_steps) {
    auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(program_base()));
    std::memcpy(program, words.data(), words.size() * 4);
    program[words.size()] = 0xd4200000u | (0xdeadu << 5);   // brk #0xdead
    const auto stop = program_base() + words.size() * 4;
    const BufferCode code(code_, code_ + kProgramOffset + (words.size() + 1) * 4);

    // Oracle.
    reset_memory();
    expected = initial;
    expected.pc = program_base();
    oracle_steps = 0;
    bool oracle_ok = true;
    std::string oracle_error;
    while (expected.pc != stop) {
        if (oracle_steps++ >= max_steps) { oracle_ok = false; oracle_error = "oracle step limit"; break; }
        if (expected.pc == kReturnToHostPc) break;
        if (expected.pc < program_base() || expected.pc >= stop) { oracle_ok = false; oracle_error = "oracle left the program"; break; }
        const auto word = *reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(expected.pc));
        if (!testing::oracle_step(expected, word)) {
            oracle_ok = false;
            char buf[64];
            std::snprintf(buf, sizeof buf, "oracle rejects 0x%08x", word);
            oracle_error = buf;
            break;
        }
    }
    if (!oracle_ok) {
        if (report) testing::report_failure(__FILE__, __LINE__, label + ": " + oracle_error);
        return false;
    }
    std::memcpy(expected_memory_.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(data_)), kDataBytes);

    // Translated.
    reset_memory();
    actual = initial;
    actual.pc = program_base();
    machine.reset();
    last = machine.run(actual, code, kReturnToHostPc);
    if (!last.ok) {
        if (report) testing::report_failure(__FILE__, __LINE__, label + ": translated run failed: " + last.error);
        return false;
    }
    if (expected.pc == kReturnToHostPc) {
        if (last.reason != ExitReason::return_to_host) {
            if (report) testing::report_failure(__FILE__, __LINE__, label + ": expected return to host");
            return false;
        }
    } else if (last.reason != ExitReason::brk || last.arg != 0xdead) {
        if (report) testing::report_failure(__FILE__, __LINE__, label + ": translated run did not reach the final BRK");
        return false;
    }
    bool equal;
    if (check_relocatable) {
        for (const auto& region : machine.regions) {
            const auto& bytes = region.code;
            for (std::size_t at = 0; at + 8 <= bytes.size(); ++at) {
                bool in_fixup = false;
                for (const auto& f : region.fixups)
                    in_fixup |= at + 8 > f.offset && at < std::size_t{f.offset} + 8;
                if (in_fixup) continue;
                std::uint64_t value;
                std::memcpy(&value, bytes.data() + at, 8);
                // Guest code addresses, and windows that would look like addresses in
                // images at the default EXE/DLL bases (0x1_4xxxxxxx, 0x1_8xxxxxxx).
                const bool image_like = (value >> 28) == 0x14 || (value >> 28) == 0x18;
                if (value - code_ < kCodeBytes || image_like) {
                    ++unrecorded_addresses;
                    if (report) {
                        char buf[160];
                        std::snprintf(buf, sizeof buf, ": region 0x%llx embeds guest address 0x%llx at code+0x%zx without a fixup",
                                      static_cast<unsigned long long>(region.guest_start),
                                      static_cast<unsigned long long>(value), at);
                        testing::report_failure(__FILE__, __LINE__, label + buf);
                    }
                    break;
                }
            }
        }
    }
    if (report) {
        equal = testing::compare_contexts(expected, actual, label);
    } else {
        equal = std::memcmp(expected.x, actual.x, sizeof expected.x) == 0 && expected.pc == actual.pc &&
                (expected.nzcv & 0xf0000000u) == (actual.nzcv & 0xf0000000u);
    }
    if (compare_memory &&
        std::memcmp(expected_memory_.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(data_)), kDataBytes) != 0) {
        if (report) {
            const auto* got = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(data_));
            std::size_t at = 0;
            while (at < kDataBytes && got[at] == expected_memory_[at]) ++at;
            char buf[128];
            std::snprintf(buf, sizeof buf, ": memory differs at data+0x%zx (expected %02x got %02x)", at,
                          static_cast<unsigned>(expected_memory_[at]), static_cast<unsigned>(got[at]));
            testing::report_failure(__FILE__, __LINE__, label + buf);
        }
        equal = false;
    }
    return equal;
}

} // namespace transmute::fx::coretest
