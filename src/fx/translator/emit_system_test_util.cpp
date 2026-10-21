#include "fx/translator/emit_system_test_util.hpp"
#include "fx/translator/emit_system_internal.hpp"

#include "fx/testing/test.hpp"
#include "transmute/fx/process.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace transmute::fx {

// Stub for the runtime-owned invalidation entry point (the tests do not link
// fx_runtime).
void invalidate_guest_code(std::uint64_t address, std::uint64_t size) {
    static std::mutex lock;
    std::lock_guard guard(lock);
    systest::invalidate_calls().push_back({address, size});
}

} // namespace transmute::fx

namespace transmute::fx::systest {

using testing::MemoryEmitContext;
using x64::Cond;
using x64::Gp;

std::vector<InvalidateCall>& invalidate_calls() {
    static std::vector<InvalidateCall> calls;
    return calls;
}

void prepare(GuestContext& ctx) { install_system_helpers(ctx); }

GuestContext make_context(std::uint64_t seed) {
    auto ctx = testing::random_context(seed);
    prepare(ctx);
    return ctx;
}

void oracle_clear_monitor() {
    GuestContext scratch{};
    testing::oracle_step(scratch, a64::kClrex);
}

bool oracle_run(GuestContext& ctx, const std::vector<std::uint32_t>& words, unsigned max_steps) {
    const auto base = ctx.pc;
    for (unsigned step = 0; step < max_steps; ++step) {
        if (ctx.pc < base || ctx.pc >= base + words.size() * 4) return true;
        if (!testing::oracle_step(ctx, words[(ctx.pc - base) / 4])) return false;
    }
    return false;
}

namespace {

constexpr Cond kSub[14] = {Cond::e, Cond::ne, Cond::ae, Cond::b, Cond::s, Cond::ns, Cond::o,
                           Cond::no, Cond::a, Cond::be, Cond::ge, Cond::l, Cond::g, Cond::le};

std::int32_t sext(std::uint32_t v, unsigned bits) {
    const std::uint32_t m = 1u << (bits - 1);
    return static_cast<std::int32_t>((v ^ m) - m);
}

// Branch target (word index relative to i) or false.
bool branch_offset(std::uint32_t w, std::int32_t& offset) {
    if ((w & 0xfc000000u) == 0x14000000u) { offset = sext(w & 0x3ffffffu, 26); return true; }
    if ((w & 0xff000010u) == 0x54000000u) { offset = sext((w >> 5) & 0x7ffffu, 19); return true; }
    if ((w & 0x7e000000u) == 0x34000000u) { offset = sext((w >> 5) & 0x7ffffu, 19); return true; }
    return false;
}

struct Flagged {   // last SUBS emitted by the test subset, for cmp+b.cond
    bool valid{};
    bool is64{};
    bool imm{};
    unsigned rn{}, rm{};
    std::uint32_t immediate{};
};

ProgramMode& mode_storage() {
    static ProgramMode mode;
    return mode;
}

// Emission driver: the context plus its per-instruction protocol.
class Driver {
public:
    virtual ~Driver() = default;
    virtual EmitContext& ctx() = 0;
    virtual void begin(std::uint64_t pc) = 0;
    virtual void end() = 0;
    virtual void prologue() {}
    virtual void epilogue() {}
};

class MemoryDriver final : public Driver {
public:
    MemoryDriver(x64::Assembler& a, std::uint64_t pc) : e_(a, pc) {}
    EmitContext& ctx() override { return e_; }
    void begin(std::uint64_t pc) override { e_.begin_instruction(pc); }
    void end() override { e_.end_instruction(); }
private:
    MemoryEmitContext e_;
};

class CoreDriver final : public Driver {
public:
    CoreDriver(x64::Assembler& a, std::uint64_t pc, const CoreLikeOptions& o) : e_(a, pc, o) {}
    EmitContext& ctx() override { return e_; }
    void begin(std::uint64_t pc) override { e_.begin_instruction(pc); }
    void end() override { e_.end_instruction(); }
    void prologue() override { e_.load_pins(); }
    void epilogue() override { e_.flush_pins(); }
private:
    CoreLikeContext e_;
};

// Test-only integer subset. Returns false for unknown words.
bool emit_integer(EmitContext& e, std::uint32_t w, Flagged& flagged) {
    auto& a = e.as();
    const bool x = (w & 0x80000000u) != 0;
    const unsigned width = x ? 64 : 32;
    const unsigned rd = w & 31u, rn = (w >> 5) & 31u, rm = (w >> 16) & 31u;
    flagged.valid = false;
    if ((w & 0x1f800000u) == 0x11000000u) {   // ADD/ADDS/SUB/SUBS imm
        const bool sub = (w & 0x40000000u) != 0, s = (w & 0x20000000u) != 0;
        const std::uint32_t imm = ((w >> 10) & 0xfffu) << ((w & (1u << 22)) ? 12 : 0);
        const Gp n = e.read_gpr(rn, true);
        const Gp t = e.temp_gpr();
        a.mov(64, t, n);
        a.alu(sub ? x64::Alu::sub : x64::Alu::add, width, t, static_cast<std::int32_t>(imm));
        if (s) {
            e.set_flags(sub ? FlagsProducer::sub : FlagsProducer::add);
            if (sub && rd == 31) flagged = {true, x, true, rn, 0, imm};
        }
        const Gp d = e.write_gpr(rd, !s);
        a.mov(64, d, t);
        return true;
    }
    if ((w & 0x1f200000u) == 0x0b000000u) {   // ADD/ADDS/SUB/SUBS shifted (LSL only)
        if (((w >> 22) & 3u) != 0) return false;
        const bool sub = (w & 0x40000000u) != 0, s = (w & 0x20000000u) != 0;
        const unsigned amount = (w >> 10) & 63u;
        const Gp n = e.read_gpr(rn);
        const Gp m = e.read_gpr(rm);
        const Gp t = e.temp_gpr(), u = e.temp_gpr();
        a.mov(64, u, m);
        if (amount) a.shl(width, u, static_cast<std::uint8_t>(amount));
        a.mov(64, t, n);
        a.alu(sub ? x64::Alu::sub : x64::Alu::add, width, t, u);
        if (s) {
            e.set_flags(sub ? FlagsProducer::sub : FlagsProducer::add);
            if (sub && amount == 0 && rd == 31) flagged = {true, x, false, rn, rm, 0};
        }
        const Gp d = e.write_gpr(rd);
        a.mov(64, d, t);
        return true;
    }
    if ((w & 0x1f200000u) == 0x0a000000u && ((w >> 29) & 3u) != 3u) {   // AND/ORR/EOR shifted (LSL)
        if (((w >> 22) & 3u) != 0) return false;
        const unsigned opc = (w >> 29) & 3u, amount = (w >> 10) & 63u;
        const Gp n = e.read_gpr(rn);
        const Gp m = e.read_gpr(rm);
        const Gp t = e.temp_gpr(), u = e.temp_gpr();
        a.mov(64, u, m);
        if (amount) a.shl(width, u, static_cast<std::uint8_t>(amount));
        a.mov(64, t, n);
        a.alu(opc == 0 ? x64::Alu::and_ : opc == 1 ? x64::Alu::or_ : x64::Alu::xor_, width, t, u);
        if (width == 32) a.mov(32, t, t);
        const Gp d = e.write_gpr(rd);
        a.mov(64, d, t);
        return true;
    }
    if ((w & 0x7f800000u) == 0x12000000u && rd != 31) {   // AND imm (non-flag; Rd 31 = SP: unsupported)
        std::uint64_t value = 0;
        if (!system_detail::decode_bit_mask((w >> 22) & 1u, (w >> 10) & 63u, (w >> 16) & 63u, x, value)) return false;
        const Gp n = e.read_gpr(rn);
        const Gp t = e.temp_gpr(), u = e.temp_gpr();
        e.clobber_host_flags();
        a.mov_imm(u, value);
        a.mov(64, t, n);
        a.alu(x64::Alu::and_, width, t, u);
        const Gp d = e.write_gpr(rd);
        a.mov(64, d, t);
        return true;
    }
    if ((w & 0x7f800000u) == 0x52800000u) {   // MOVZ
        const unsigned hw = (w >> 21) & 3u;
        const Gp d = e.write_gpr(rd);
        a.mov_imm(d, static_cast<std::uint64_t>((w >> 5) & 0xffffu) << (16 * hw));
        return true;
    }
    if ((w & 0xffc00000u) == 0xf9400000u || (w & 0xffc00000u) == 0xf9000000u) {   // LDR/STR X imm
        const bool load = (w & 0x00400000u) != 0;
        const std::int32_t off = static_cast<std::int32_t>(((w >> 10) & 0xfffu) * 8);
        const Gp base = e.read_gpr(rn, true);
        if (load) {
            const Gp t = e.temp_gpr();
            a.mov(64, t, x64::ptr(base, off));
            const Gp d = e.write_gpr(rd);
            a.mov(64, d, t);
        } else {
            const Gp v = e.read_gpr(rd);
            a.mov(64, x64::ptr(base, off), v);
        }
        return true;
    }
    return false;
}

} // namespace

ProgramModeScope::ProgramModeScope(const ProgramMode& mode) : saved_(mode_storage()) { mode_storage() = mode; }
ProgramModeScope::~ProgramModeScope() { mode_storage() = saved_; }
const ProgramMode& current_program_mode() { return mode_storage(); }

Program::Program(std::vector<std::uint32_t> words, std::uint64_t base_pc)
    : words_(std::move(words)), base_(base_pc) {
    const auto n = static_cast<std::int32_t>(words_.size());
    std::vector<bool> target(words_.size() + 1, false);
    for (std::int32_t i = 0; i < n; ++i) {
        std::int32_t off;
        if (branch_offset(words_[i], off)) {
            const auto t = i + off;
            if (t < 0 || t > n) throw std::logic_error("Program: branch leaves the program");
            target[t] = true;
        }
    }
    // The shared Snippet trampoline does not preserve the Win64 callee-saved
    // xmm6-xmm15 while MemoryEmitContext hands them out as temps: save them in
    // this frame (rsp stays 16-aligned; [rsp, rsp+32) is shadow space).
    x64::Assembler a;
    a.sub(64, Gp::rsp, 40 + 160);
    for (unsigned i = 0; i < 10; ++i) a.movdqu(x64::ptr(Gp::rsp, 32 + 16 * static_cast<int>(i)), x64::xmm(6 + i));
    const auto& mode = current_program_mode();
    std::unique_ptr<Driver> driver;
    if (mode.core_like) driver = std::make_unique<CoreDriver>(a, base_, mode.core);
    else driver = std::make_unique<MemoryDriver>(a, base_);
    EmitContext& emit = driver->ctx();
    driver->prologue();
    std::vector<x64::Label> labels(words_.size() + 1);
    for (auto& l : labels) l = a.new_label();
    Flagged flagged;
    std::int32_t i = 0;
    while (i < n) {
        a.bind(labels[i]);
        const auto w = words_[i];
        const auto pc = base_ + static_cast<std::uint64_t>(i) * 4;
        driver->begin(pc);
        std::int32_t off;
        if (branch_offset(w, off)) {
            const auto dest = labels[i + off];
            if ((w & 0xfc000000u) == 0x14000000u) {
                a.jmp(dest);
            } else if ((w & 0xff000010u) == 0x54000000u) {
                const unsigned cond = w & 15u;
                if (cond >= 14) {
                    a.jmp(dest);
                } else if (flagged.valid && !target[i]) {
                    // cmp + b.cond: re-run the compare to get host flags directly.
                    const Gp l = emit.read_gpr(flagged.rn);
                    if (flagged.imm) a.cmp(flagged.is64 ? 64 : 32, l, static_cast<std::int32_t>(flagged.immediate));
                    else a.cmp(flagged.is64 ? 64 : 32, l, emit.read_gpr(flagged.rm));
                    a.jcc(kSub[cond], dest);
                } else {
                    a.jcc(emit.condition(cond), dest);
                }
            } else {
                const bool x = (w & 0x80000000u) != 0;
                const Gp r = emit.read_gpr(w & 31u);
                a.test(x ? 64 : 32, r, r);
                a.jcc((w & 0x01000000u) ? Cond::ne : Cond::e, dest);
            }
            driver->end();
            flagged.valid = false;
            ++i;
            continue;
        }
        std::int32_t end = i + 1;
        while (end < n && !target[end]) ++end;
        unsigned consumed = 0;
        try {
            consumed = emit_system(emit, std::span(words_).subspan(i, end - i));
        } catch (const std::exception& error) {
            std::ostringstream message;
            message << error.what() << " at word " << i << " (0x" << std::hex << w << ") of";
            for (auto x : words_) message << ' ' << std::hex << x;
            message << " [" << mode.name << "]";
            throw std::runtime_error(message.str());
        }
        if (consumed) {
            spans_.push_back(consumed);
            flagged.valid = false;
        } else {
            if (!emit_integer(emit, w, flagged)) {
                std::ostringstream message;
                message << "Program: unsupported word 0x" << std::hex << w;
                throw std::logic_error(message.str());
            }
            consumed = 1;
        }
        driver->end();
        i += static_cast<std::int32_t>(consumed);
    }
    a.bind(labels[words_.size()]);
    driver->epilogue();
    for (unsigned k = 0; k < 10; ++k) a.movdqu(x64::xmm(6 + k), x64::ptr(Gp::rsp, 32 + 16 * static_cast<int>(k)));
    a.add(64, Gp::rsp, 40 + 160);
    a.ret();
    a.finalize();
    code_size_ = a.size();
    snippet_ = std::make_unique<testing::Snippet>(a.bytes());
}

void Program::run(GuestContext& ctx) const {
    ctx.pc = base_;
    snippet_->run(ctx);
    ctx.pc = base_ + words_.size() * 4;
}

bool differential(const std::vector<std::uint32_t>& words, const GuestContext& ctx,
                  testing::Buffer& buffer, const std::string& label, std::size_t memory_bytes,
                  GuestContext* actual_out, const testing::CompareOptions& options) {
    return differential(words, words, ctx, buffer, label, memory_bytes, actual_out, options);
}

bool differential(const std::vector<std::uint32_t>& reference, const std::vector<std::uint32_t>& words,
                  const GuestContext& ctx, testing::Buffer& buffer, const std::string& label,
                  std::size_t memory_bytes, GuestContext* actual_out, const testing::CompareOptions& options) {
    std::vector<std::uint8_t> initial(buffer.data(), buffer.data() + memory_bytes);
    auto expected = ctx;
    expected.pc = 0x0000'0001'4000'1000ULL;
    oracle_clear_monitor();
    if (!oracle_run(expected, reference)) {
        testing::report_failure(__FILE__, __LINE__, label + ": oracle declined");
        return false;
    }
    std::vector<std::uint8_t> expected_memory(buffer.data(), buffer.data() + memory_bytes);
    std::memcpy(buffer.data(), initial.data(), memory_bytes);
    auto actual = ctx;
    Program program(words);
    program.run(actual);
    if (reference.size() != words.size()) actual.pc = expected.pc;
    bool ok = testing::compare_contexts(expected, actual, label, options);
    for (std::size_t i = 0; i < memory_bytes; ++i) {
        if (buffer.data()[i] != expected_memory[i]) {
            std::ostringstream message;
            message << label << ": memory byte " << i << " expected 0x" << std::hex
                    << unsigned(expected_memory[i]) << " got 0x" << unsigned(buffer.data()[i]);
            testing::report_failure(__FILE__, __LINE__, message.str());
            ok = false;
            break;
        }
    }
    if (actual_out) *actual_out = actual;
    return ok;
}

void fill_random(testing::Buffer& buffer, std::uint64_t seed) {
    std::uint64_t state = seed * 0x9e3779b97f4a7c15ULL + 7;
    for (std::size_t i = 0; i + 8 <= buffer.size(); i += 8) {
        const auto v = testing::random_u64(state);
        std::memcpy(buffer.data() + i, &v, 8);
    }
}

double run_threads(unsigned threads, const std::function<void(unsigned)>& body) {
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    std::atomic<unsigned> done{0};
    std::vector<std::thread> pool;
    std::chrono::steady_clock::time_point end_time;
    std::mutex end_lock;
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) _mm_pause();
            body(t);
            if (done.fetch_add(1) + 1 == threads) {
                std::lock_guard guard(end_lock);
                end_time = std::chrono::steady_clock::now();
            }
        });
    }
    while (ready.load() != threads) std::this_thread::yield();
    const auto start = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& th : pool) th.join();
    return std::chrono::duration<double>(end_time - start).count();
}

double median_of(unsigned runs, const std::function<double()>& measure) {
    std::vector<double> values;
    for (unsigned i = 0; i < runs; ++i) values.push_back(measure());
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

} // namespace transmute::fx::systest
