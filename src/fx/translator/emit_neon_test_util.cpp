// Test support for the NEON family (owner: neon).
#include "fx/translator/emit_neon_test_util.hpp"

#include "fx/common/cpu_features.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace transmute::fx::neon_test {

using x64::Gp;
using x64::Xmm;
using x64::ptr;

namespace {

constexpr Gp kGprPool[] = {Gp::rax, Gp::rcx, Gp::rdx, Gp::rbx, Gp::rbp, Gp::rsi, Gp::rdi,
                           Gp::r8, Gp::r9, Gp::r10, Gp::r11, Gp::r12, Gp::r13, Gp::r14};
constexpr Gp kCtx = Gp::r15;

// Pinned-register style context: a guest register keeps one host register for
// the whole instruction, so a write returns the same host register as an
// earlier read of that guest register. Reading a register after writing it
// (a rule violation) throws.
class AliasingEmitContext final : public EmitContext {
public:
    AliasingEmitContext(x64::Assembler& a, std::uint64_t pc, bool mem_sources = false, std::uint32_t zero32 = 0,
                        std::uint32_t zero64 = 0)
        : as_(a), pc_(pc), mem_sources_(mem_sources), zero32_(zero32), zero64_(zero64), cf_start_(a.control_flow_count()) {}
    // Known-zero upper bits (claims derived from the start state; only for
    // the first instruction of a run, like a region forgetting at labels).
    [[nodiscard]] std::uint32_t vec_zero_above(unsigned width) const override {
        return width <= 32 ? zero32_ : zero64_;
    }
    x64::Assembler& as() override { return as_; }
    [[nodiscard]] std::uint64_t pc() const override { return pc_; }

    // Memory sources: registers not yet in a host register this instruction
    // are handed out as their GuestContext slot (unchanged until end()).
    VecSrc vsrc(unsigned index) override {
        if (mem_sources_ && !vec_[index].has) return {Xmm{}, ptr(kCtx, v_disp(index)), true};
        return {read_vec(index), {}, false};
    }
    GprSrc gsrc(unsigned index, bool sp) override {
        if (mem_sources_ && (index != 31 || sp) && !gpr_[index].has) return {Gp::none, ptr(kCtx, x_disp(index)), true};
        return {read_gpr(index, sp), {}, false};
    }
    // Memory-source mode: every slot holds the value from before the
    // instruction (registers handed out are clean copies until end()).
    bool vec_slot(unsigned index, x64::Mem& slot) override {
        if (!mem_sources_ || index >= 32) return false;
        slot = ptr(kCtx, v_disp(index));
        return true;
    }
    // Recorded and checked against the expected state by the harness.
    void note_vec_zero_above(unsigned index, unsigned width) override {
        if (index >= 32 || !vec_[index].written)
            throw std::logic_error("note_vec_zero_above for a register the instruction did not write");
        notes.push_back({index, width});
    }
    std::vector<ZeroNote> notes;

    Gp read_gpr(unsigned index, bool sp) override {
        if (index == 31 && !sp) {
            const Gp r = alloc_gpr();
            as_.mov_imm(r, 0);
            return r;
        }
        auto& e = gpr_[index];
        if (e.written) throw std::logic_error("read_gpr after write_gpr of the same register");
        if (!e.has) {
            e.host = alloc_gpr();
            e.has = true;
            as_.mov(64, e.host, ptr(kCtx, x_disp(index)));
        }
        return e.host;
    }
    Gp write_gpr(unsigned index, bool sp) override {
        if (index == 31 && !sp) return alloc_gpr();
        auto& e = gpr_[index];
        if (!e.has) {
            e.host = alloc_gpr();
            e.has = true;
        }
        e.written = true;
        return e.host;
    }
    Gp copy_gpr(unsigned index, bool sp) override {
        const Gp src = read_gpr(index, sp);
        const Gp r = alloc_gpr();
        as_.mov(64, r, src);
        return r;
    }
    Gp temp_gpr() override { return alloc_gpr(); }

    Xmm read_vec(unsigned index) override {
        auto& e = vec_[index];
        if (e.written) throw std::logic_error("read_vec after write_vec of the same register");
        if (!e.has) {
            e.host = alloc_vec();
            e.has = true;
            as_.movdqu(e.host, ptr(kCtx, v_disp(index)));
        }
        return e.host;
    }
    Xmm write_vec(unsigned index) override {
        auto& e = vec_[index];
        if (!e.has) {
            e.host = alloc_vec();
            e.has = true;
        }
        e.written = true;
        return e.host;
    }
    Xmm temp_vec() override {
        ++temps_;
        return alloc_vec();
    }

    void clobber_host_flags() override {}
    void set_flags(FlagsProducer) override { throw std::logic_error("neon: unexpected set_flags"); }
    void set_nzcv(Gp value) override { as_.mov(64, ptr(kCtx, ctx_disp(offsetof(GuestContext, nzcv))), value); }
    Gp get_nzcv() override {
        const Gp r = alloc_gpr();
        as_.mov(32, r, ptr(kCtx, ctx_disp(offsetof(GuestContext, nzcv))));
        return r;
    }
    x64::Cond condition(unsigned) override { throw std::logic_error("neon: unexpected condition()"); }
    void mark_may_fault() override { ++fault_marks_; }
    Gp call_helper(unsigned, std::initializer_list<HelperArg>, HelperEffects) override {
        throw std::logic_error("neon: unexpected call_helper");
    }

    // XMM temps / total XMM registers used by the last instruction.
    [[nodiscard]] unsigned temps() const { return temps_; }
    [[nodiscard]] unsigned xmm_used() const { return next_vec_; }

    void begin(std::uint64_t pc) {
        if (pc != pc_) zero32_ = zero64_ = 0;
        pc_ = pc;
        temps_ = 0;
        next_gpr_ = next_vec_ = 0;
        cf_start_ = as_.control_flow_count();
        for (auto& e : gpr_) e = {};
        for (auto& e : vec_) e = {};
    }
    // Every register obtained is stored back, read-only ones included: an
    // emitter that writes into a source register (which may be a pinned or
    // cached guest register in translated regions) then fails the tests.
    void end() {
        for (unsigned i = 0; i < 32; ++i)
            if (gpr_[i].has) as_.mov(64, ptr(kCtx, x_disp(i)), gpr_[i].host);
        for (unsigned i = 0; i < 32; ++i)
            if (vec_[i].has) as_.movdqu(ptr(kCtx, v_disp(i)), vec_[i].host);
    }

private:
    // Rule: every register before the instruction's first label or branch.
    // In a region an allocation there may evict a dirty cached register, whose
    // store would run on one path only: the region emitter then retries the
    // whole region without write-back (every result stored at once).
    void check_no_control_flow() const {
        if (as_.control_flow_count() != cf_start_)
            throw std::logic_error("AliasingEmitContext: register allocated after the instruction's control flow");
    }
    Gp alloc_gpr() {
        check_no_control_flow();
        if (next_gpr_ >= std::size(kGprPool)) throw std::logic_error("AliasingEmitContext: out of GPRs");
        return kGprPool[next_gpr_++];
    }
    Xmm alloc_vec() {
        check_no_control_flow();
        if (next_vec_ >= 16) throw std::logic_error("AliasingEmitContext: out of XMM registers");
        return x64::xmm(next_vec_++);
    }
    struct GprEntry { Gp host{Gp::none}; bool has{}; bool written{}; };
    struct VecEntry { Xmm host{}; bool has{}; bool written{}; };
    x64::Assembler& as_;
    std::uint64_t pc_;
    bool mem_sources_{};
    std::uint32_t zero32_{}, zero64_{};
    unsigned next_gpr_{}, next_vec_{}, fault_marks_{}, temps_{};
    std::uint32_t cf_start_{};
    std::array<GprEntry, 32> gpr_{};
    std::array<VecEntry, 32> vec_{};
};

// Context for instruction counting: guest registers are "pinned" to fixed
// host registers and no loads/stores are emitted for them.
class CountingContext final : public EmitContext {
public:
    explicit CountingContext(x64::Assembler& a) : as_(a) {}
    x64::Assembler& as() override { return as_; }
    [[nodiscard]] std::uint64_t pc() const override { return 0x140001000ULL; }
    Gp read_gpr(unsigned index, bool) override { return kGprPool[index % 6]; }
    Gp write_gpr(unsigned index, bool) override { return kGprPool[index % 6]; }
    Gp copy_gpr(unsigned index, bool sp) override {
        const Gp r = temp_gpr();
        as_.mov(64, r, read_gpr(index, sp));
        return r;
    }
    Gp temp_gpr() override { return kGprPool[6 + (next_gpr_++ % 8)]; }
    Xmm read_vec(unsigned index) override { return x64::xmm(index % 8); }
    Xmm write_vec(unsigned index) override { return x64::xmm(index % 8); }
    Xmm temp_vec() override { return x64::xmm(8 + (next_vec_++ % 8)); }
    void clobber_host_flags() override {}
    void set_flags(FlagsProducer) override {}
    void set_nzcv(Gp) override {}
    Gp get_nzcv() override { return Gp::rax; }
    x64::Cond condition(unsigned) override { return x64::Cond::e; }
    void mark_may_fault() override {}
    Gp call_helper(unsigned, std::initializer_list<HelperArg>, HelperEffects) override { return Gp::rax; }

private:
    x64::Assembler& as_;
    unsigned next_gpr_{}, next_vec_{};
};


// Snippet frame: 32 bytes shadow + 8 pad + xmm6-xmm15 (Win64 callee-saved;
// testing::Snippet's trampoline does not preserve them).
constexpr std::int32_t kFrame = 40 + 160;

void prologue(x64::Assembler& a) {
    a.sub(64, Gp::rsp, kFrame);
    for (unsigned i = 0; i < 10; ++i) a.movdqu(ptr(Gp::rsp, static_cast<std::int32_t>(40 + 16 * i)), x64::xmm(6 + i));
}

void epilogue(x64::Assembler& a) {
    for (unsigned i = 0; i < 10; ++i) a.movdqu(x64::xmm(6 + i), ptr(Gp::rsp, static_cast<std::int32_t>(40 + 16 * i)));
    a.add(64, Gp::rsp, kFrame);
    a.ret();
}

std::vector<Usage>& usage_table() {
    static std::vector<Usage> table;
    return table;
}

void record_usage(std::uint32_t word, unsigned temps, unsigned xmm) {
    auto& table = usage_table();
    // Keep the 16 heaviest distinct words.
    for (auto& u : table)
        if (u.word == word) return;
    table.push_back({word, temps, xmm});
    std::sort(table.begin(), table.end(), [](const Usage& a, const Usage& b) { return a.xmm > b.xmm; });
    if (table.size() > 16) table.pop_back();
}

// Registers of ctx whose bits above 32 / 64 are zero (truthful claims).
void zero_claims(const GuestContext& ctx, std::uint32_t& zero32, std::uint32_t& zero64) {
    zero32 = zero64 = 0;
    for (unsigned r = 0; r < 32; ++r) {
        if (ctx.v[r][1] != 0) continue;
        zero64 |= 1u << r;
        if ((ctx.v[r][0] >> 32) == 0) zero32 |= 1u << r;
    }
}

bool run_aliasing(GuestContext& ctx, FamilyEmitFn family, std::span<const std::uint32_t> words,
                  bool region_pool, bool mem_sources = false, std::vector<ZeroNote>* notes = nullptr,
                  bool claims = false) {
    x64::Assembler a;
    prologue(a);
    if (region_pool) neon_begin_region(a);
    std::uint32_t z32 = 0, z64 = 0;
    if (claims) zero_claims(ctx, z32, z64);
    AliasingEmitContext emit(a, ctx.pc, mem_sources, z32, z64);
    std::size_t index = 0;
    while (index < words.size()) {
        emit.begin(ctx.pc + index * 4);
        const auto consumed = family(emit, words.subspan(index));
        if (consumed == 0) return false;
        record_usage(words[index], emit.temps(), emit.xmm_used());
        emit.end();
        index += consumed;
    }
    epilogue(a);
    if (region_pool) neon_end_region(a);
    a.finalize();
    testing::Snippet snippet(a.bytes());
    snippet.run(ctx);
    ctx.pc += words.size() * 4;
    if (notes) *notes = emit.notes;
    return true;
}

// Every note_vec_zero_above of a single instruction must hold in the expected
// state: bits [127:width] of the noted register are zero.
bool check_notes(const std::vector<ZeroNote>& notes, const GuestContext& expected, const std::string& label) {
    bool ok = true;
    for (const auto& n : notes) {
        const std::uint64_t lo = n.width <= 32 ? expected.v[n.index][0] >> 32 : 0;
        const std::uint64_t hi = n.width < 128 ? expected.v[n.index][1] : 0;
        if (hi != 0 || lo != 0) {
            std::ostringstream msg;
            msg << label << ": noted v" << n.index << " zero above bit " << n.width << " but expected v"
                << n.index << " = 0x" << std::hex << expected.v[n.index][1] << ":" << expected.v[n.index][0];
            testing::report_failure(__FILE__, __LINE__, msg.str());
            ok = false;
        }
    }
    return ok;
}

// testing::run_family equivalent (MemoryEmitContext, inline constants) that
// preserves xmm6-xmm15.
bool run_memory(GuestContext& ctx, FamilyEmitFn family, std::span<const std::uint32_t> words) {
    x64::Assembler a;
    prologue(a);
    testing::MemoryEmitContext emit(a, ctx.pc);
    std::size_t index = 0;
    while (index < words.size()) {
        emit.begin_instruction(ctx.pc + index * 4);
        const auto consumed = family(emit, words.subspan(index));
        if (consumed == 0) return false;
        emit.end_instruction();
        index += consumed;
    }
    epilogue(a);
    a.finalize();
    testing::Snippet snippet(a.bytes());
    snippet.run(ctx);
    ctx.pc += words.size() * 4;
    return true;
}

bool accepts(FamilyEmitFn family, std::uint32_t word) {
    x64::Assembler a;
    testing::MemoryEmitContext emit(a, 0x140001000ULL);
    emit.begin_instruction(0x140001000ULL);
    return family(emit, std::span<const std::uint32_t>(&word, 1)) != 0;
}

std::string hex(std::uint32_t word) {
    std::ostringstream out;
    out << "0x" << std::hex << word;
    return out.str();
}

bool trace() {
    static const bool enabled = std::getenv("NEON_TRACE") != nullptr;
    return enabled;
}

struct LevelGuard {
    ~LevelGuard() { neon_set_feature_level(-1); }
};

Outcome compare_all(FamilyEmitFn family, const GuestContext& start, std::uint32_t word,
                    const std::string& label, MemoryRange memory, const GuestContext& expected,
                    const std::vector<std::uint8_t>& expected_memory,
                    const std::vector<std::uint8_t>& initial_memory,
                    const testing::CompareOptions& options) {
    LevelGuard guard;
    static const char* const mode_names[] = {"memory/inline", "aliasing/pool", "aliasing/inline",
                                             "aliasing/pool/memory sources", "aliasing/pool/known-zero upper"};
    std::uint32_t claim32 = 0, claim64 = 0;
    zero_claims(start, claim32, claim64);
    for (const int level : levels()) {
        neon_set_feature_level(level);
        for (unsigned mode = 0; mode < 5; ++mode) {
            if (mode == 4 && claim64 == 0) continue;   // same as mode 1
            GuestContext actual = start;
            if (memory.data) std::memcpy(memory.data, initial_memory.data(), memory.size);
            if (trace()) { std::printf("  run level %d mode %u\n", level, mode); std::fflush(stdout); }
            bool ran = false;
            std::vector<ZeroNote> notes;
            try {
                if (mode == 0) ran = run_memory(actual, family, std::span<const std::uint32_t>(&word, 1));
                else ran = run_aliasing(actual, family, std::span<const std::uint32_t>(&word, 1), mode != 2,
                                        mode == 3, &notes, mode == 4);
            } catch (const std::exception& e) {
                testing::report_failure(__FILE__, __LINE__,
                                        label + " " + hex(word) + ": emitter threw: " + e.what());
                return Outcome::mismatch;
            }
            if (!ran) {
                testing::report_failure(__FILE__, __LINE__, label + " " + hex(word) + ": declined in run");
                return Outcome::mismatch;
            }
            std::ostringstream config;
            config << label << " " << hex(word) << " [level " << level << ", " << mode_names[mode] << "]";
            bool equal = testing::compare_contexts(expected, actual, config.str(), options);
            equal = check_notes(notes, expected, config.str()) && equal;
            if (memory.data && std::memcmp(memory.data, expected_memory.data(), memory.size) != 0) {
                for (std::size_t i = 0; i < memory.size; ++i) {
                    if (memory.data[i] != expected_memory[i]) {
                        std::ostringstream msg;
                        msg << config.str() << ": memory byte +" << i << " expected 0x" << std::hex
                            << unsigned(expected_memory[i]) << " got 0x" << unsigned(memory.data[i]);
                        testing::report_failure(__FILE__, __LINE__, msg.str());
                        break;
                    }
                }
                equal = false;
            }
            if (!equal) {
                if (memory.data) std::memcpy(memory.data, initial_memory.data(), memory.size);
                return Outcome::mismatch;
            }
        }
    }
    if (memory.data) std::memcpy(memory.data, initial_memory.data(), memory.size);
    return Outcome::ok;
}

} // namespace

Stats& stats() {
    static Stats s;
    return s;
}

const std::vector<int>& levels() {
    static const std::vector<int> result = [] {
        std::vector<int> r{0};
        const auto& f = cpu_features();
        if (f.avx && f.os_avx) r.push_back(1);
        if (f.avx2 && f.os_avx) r.push_back(2);
        return r;
    }();
    return result;
}

Outcome check(FamilyEmitFn family, const GuestContext& start, std::uint32_t word,
              const std::string& label, MemoryRange memory, const testing::CompareOptions& options) {
    std::vector<std::uint8_t> initial;
    if (memory.data) initial.assign(memory.data, memory.data + memory.size);
    GuestContext expected = start;
    if (trace()) { std::printf("check %s 0x%08x: oracle\n", label.c_str(), word); std::fflush(stdout); }
    const bool oracle_ok = testing::oracle_step(expected, word);
    std::vector<std::uint8_t> expected_memory;
    if (memory.data) {
        expected_memory.assign(memory.data, memory.data + memory.size);
        std::memcpy(memory.data, initial.data(), memory.size);
    }
    const bool handled = accepts(family, word);
    static const bool list = std::getenv("NEON_LIST") != nullptr;
    if (!handled) {
        if (oracle_ok) {
            if (list) std::printf("DECLINED %s 0x%08x\n", label.c_str(), word);
            ++stats().declined;
            return Outcome::declined;
        }
        ++stats().reserved;
        return Outcome::both_declined;
    }
    if (!oracle_ok) {
        // The oracle lacks it: fall back to the hand-written C++ reference.
        GuestContext reference = start;
        if (reference_step(reference, word)) {
            reference.pc += 4;
            ++stats().reference_checked;
            ++stats().checked;
            std::vector<std::uint8_t> reference_memory;
            if (memory.data) reference_memory.assign(memory.data, memory.data + memory.size);
            return compare_all(family, start, word, label, memory, reference, reference_memory, initial, options);
        }
        if (list) std::printf("MISSING %s 0x%08x\n", label.c_str(), word);
        ++stats().oracle_missing;
        return Outcome::oracle_missing;
    }
    ++stats().checked;
    return compare_all(family, start, word, label, memory, expected, expected_memory, initial, options);
}

Outcome check_with(FamilyEmitFn family, const GuestContext& start, std::uint32_t word,
                   const std::string& label, const std::function<void(GuestContext&)>& reference,
                   MemoryRange memory) {
    std::vector<std::uint8_t> initial;
    if (memory.data) initial.assign(memory.data, memory.data + memory.size);
    GuestContext expected = start;
    reference(expected);
    expected.pc += 4;
    std::vector<std::uint8_t> expected_memory;
    if (memory.data) {
        expected_memory.assign(memory.data, memory.data + memory.size);
        std::memcpy(memory.data, initial.data(), memory.size);
    }
    if (!accepts(family, word)) {
        testing::report_failure(__FILE__, __LINE__, label + " " + hex(word) + ": declined");
        return Outcome::declined;
    }
    ++stats().checked;
    return compare_all(family, start, word, label, memory, expected, expected_memory, initial, {});
}

bool run(GuestContext& ctx, FamilyEmitFn family, std::span<const std::uint32_t> words) {
    return run_memory(ctx, family, words);
}

std::vector<std::uint8_t> emit_bytes(FamilyEmitFn family, std::uint32_t word, int level, bool region_pool) {
    LevelGuard guard;
    neon_set_feature_level(level);
    x64::Assembler a;
    if (region_pool) neon_begin_region(a);
    CountingContext emit(a);
    family(emit, std::span<const std::uint32_t>(&word, 1));
    const auto code_size = region_pool ? a.size() : a.size();
    if (region_pool) neon_end_region(a);
    a.finalize();
    std::vector<std::uint8_t> bytes(a.bytes().begin(), a.bytes().begin() + static_cast<std::ptrdiff_t>(code_size));
    return bytes;
}

std::uint64_t rnd(std::uint64_t& state) { return testing::random_u64(state); }

const std::vector<Usage>& heaviest_instructions() { return usage_table(); }

bool check_sequence(FamilyEmitFn family, const GuestContext& start, std::span<const std::uint32_t> words,
                    const std::string& label, MemoryRange memory) {
    std::vector<std::uint8_t> initial;
    if (memory.data) initial.assign(memory.data, memory.data + memory.size);
    GuestContext expected = start;
    for (const auto w : words) {
        if (!testing::oracle_step(expected, w)) {
            testing::report_failure(__FILE__, __LINE__, label + ": oracle lacks " + hex(w));
            return false;
        }
    }
    std::vector<std::uint8_t> expected_memory;
    if (memory.data) {
        expected_memory.assign(memory.data, memory.data + memory.size);
        std::memcpy(memory.data, initial.data(), memory.size);
    }
    LevelGuard guard;
    bool ok = true;
    for (const int level : levels()) {
        neon_set_feature_level(level);
        for (unsigned mode = 0; mode < 4 && ok; ++mode) {
            GuestContext actual = start;
            if (memory.data) std::memcpy(memory.data, initial.data(), memory.size);
            bool ran = false;
            try {
                ran = mode == 0 ? run_memory(actual, family, words)
                                : run_aliasing(actual, family, words, mode != 2, mode == 3);
            } catch (const std::exception& e) {
                testing::report_failure(__FILE__, __LINE__, label + ": emitter threw: " + e.what());
                return false;
            }
            if (!ran) {
                testing::report_failure(__FILE__, __LINE__, label + ": a word was declined");
                return false;
            }
            std::ostringstream config;
            config << label << " [level " << level << ", mode " << mode << "]";
            ok = testing::compare_contexts(expected, actual, config.str());
            if (memory.data && std::memcmp(memory.data, expected_memory.data(), memory.size) != 0) {
                testing::report_failure(__FILE__, __LINE__, config.str() + ": memory differs");
                ok = false;
            }
        }
    }
    if (memory.data) std::memcpy(memory.data, initial.data(), memory.size);
    return ok;
}

GuestContext special_context(std::uint64_t seed, unsigned size) {
    GuestContext ctx = testing::random_context(seed);
    std::uint64_t state = seed * 0x9e3779b97f4a7c15ULL + 17;
    const unsigned esize = 8u << size;
    const std::uint64_t mask = esize == 64 ? ~0ULL : (1ULL << esize) - 1;
    const std::uint64_t smin = 1ULL << (esize - 1);
    const std::uint64_t smax = smin - 1;
    const std::uint64_t specials[] = {0, 1, 2, 3, smax, smax - 1, smin, smin + 1, mask, mask - 1,
                                      smin >> 1, (smin >> 1) - 1, smax >> 1, 0x55, 0x80, 0xff, 0x7f};
    for (unsigned r = 0; r < 32; ++r) {
        if (rnd(state) % 3 == 0) continue;   // keep fully random registers too
        for (unsigned lane = 0; lane < 128 / esize; ++lane) {
            std::uint64_t value;
            switch (rnd(state) % 4) {
            case 0: case 1: value = specials[rnd(state) % std::size(specials)]; break;
            case 2: value = rnd(state) % 33 - 16; break;
            default: value = rnd(state); break;
            }
            value &= mask;
            const unsigned bit = lane * esize;
            auto& word = ctx.v[r][bit / 64];
            const unsigned shift = bit % 64;
            word = (word & ~(mask << shift)) | (value << shift);
        }
    }
    return ctx;
}

void print_stats(const char* name) {
    const auto& s = stats();
    std::printf("[neon] %s: %u checked (%u by C++ reference), %u unverified, %u declined, %u reserved\n",
                name, s.checked, s.reference_checked, s.oracle_missing, s.declined, s.reserved);
}

} // namespace transmute::fx::neon_test
