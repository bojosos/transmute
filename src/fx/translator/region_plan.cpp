#include "fx/translator/region_plan.hpp"

#include "fx/translator/core_isel.hpp"   // codegen_p2_knob_bits (fingerprint, flags at BR)

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>

namespace transmute::fx {

namespace {
constexpr unsigned kMaxFamilies = 32;
FamilyEmitter g_families[kMaxFamilies];
std::atomic<unsigned> g_family_count{0};
std::mutex g_family_mutex;
} // namespace

void register_family_emitter(const FamilyEmitter& family) {
    std::lock_guard lock(g_family_mutex);
    const auto n = g_family_count.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < n; ++i)
        if (g_families[i].emit == family.emit) return;   // idempotent
    if (n >= kMaxFamilies) throw std::logic_error("too many instruction families");
    g_families[n] = family;
    g_family_count.store(n + 1, std::memory_order_release);
}

std::uint64_t translator_family_fingerprint() {
    const auto n = g_family_count.load(std::memory_order_acquire);
    std::uint64_t h = 0xcbf29ce484222325ULL;
    const auto mix = [&](unsigned char c) { h = (h ^ c) * 0x100000001b3ULL; };
    for (unsigned i = 0; i < n; ++i) {
        for (const char* p = g_families[i].name ? g_families[i].name : ""; *p; ++p) mix(static_cast<unsigned char>(*p));
        mix(0);
    }
    mix(static_cast<unsigned char>(n));
    if (const auto knobs = core::codegen_knob_bits(); knobs != 0) {   // debugging knobs change the code
        mix(0xff);
        mix(static_cast<unsigned char>(knobs));
        mix(static_cast<unsigned char>(knobs >> 8));
        if (knobs >> 16) mix(static_cast<unsigned char>(knobs >> 16));
        if (knobs >> 24) mix(static_cast<unsigned char>(knobs >> 24));
    }
    {   // the cross-region register convention (translator.hpp)
        const auto& conv = translator_register_convention();
        mix(0xfd);
        for (unsigned b = 0; b < 32; b += 8) mix(static_cast<unsigned char>(conv.guest_mask >> b));
        for (unsigned g = 0; g < 32; ++g)
            if ((conv.guest_mask >> g) & 1u) mix(static_cast<unsigned char>(conv.host[g]));
    }
    if (const auto layout = core::codegen_layout_bits(); layout != 0) {
        mix(0xfc);   // code layout knobs
        mix(static_cast<unsigned char>(layout));
    }
    if (const auto p2 = core::codegen_p2_knob_bits(); p2 != 0) {
        mix(0xfa);   // core_isel.hpp CodegenKnob (fx perf wave p2)
        mix(static_cast<unsigned char>(p2));
        if (p2 >> 8) mix(static_cast<unsigned char>(p2 >> 8));
    }
    if (const auto t2 = core::codegen_tier2_bits(); t2 != 0) {
        mix(0xfb);   // tier-2 feature knobs
        mix(static_cast<unsigned char>(t2));
    }
    if (const auto limit = core::codegen_inline_max(); limit != core::PlanOverrides::kDefaultInlineMax) {
        mix(0xfe);   // call inlining size limit
        mix(static_cast<unsigned char>(limit));
        mix(static_cast<unsigned char>(limit >> 8));
    }
    return h;
}

namespace core {

FamilyTable registered_families() {
    return {g_families, g_family_count.load(std::memory_order_acquire)};
}

std::int32_t Plan::find(std::uint64_t pc) const {
    if (inlined_calls != 0) {   // inlined copies interleave: segment 0 is sorted but not contiguous
        for (std::size_t i = 0; i < insts.size(); ++i)
            if (insts[i].pc == pc && insts[i].segment == 0) return static_cast<std::int32_t>(i);
        return -1;
    }
    std::size_t lo = 0, hi = insts.size();
    while (lo < hi) {
        const auto mid = (lo + hi) / 2;
        if (insts[mid].pc < pc) lo = mid + 1;
        else hi = mid;
    }
    return lo < insts.size() && insts[lo].pc == pc ? static_cast<std::int32_t>(lo) : -1;
}

namespace {

// Open-addressing set of guest pcs used during discovery (key and generation
// stamp share a slot: one cache line per probe). It starts small and doubles
// at half load: most regions hold a few dozen instructions, and a table sized
// for the largest one would miss the cache on every probe.
class PcSet {
public:
    void reset(std::size_t capacity) {
        (void)capacity;
        if (slots_.size() < kInitial) {
            slots_.assign(kInitial, Slot{});
            generation_ = 0;
        }
        size_ = kInitial;   // use the first kInitial slots; grow() widens
        mask_ = size_ - 1;
        count_ = 0;
        if (++generation_ == 0) {
            for (auto& s : slots_) s.stamp = 0;
            generation_ = 1;
        }
    }
    // Slot holding `pc`, or the empty slot where insert_at() would put it
    // (valid until the next insertion); *found tells which.
    [[nodiscard]] std::size_t find_slot(std::uint64_t pc, bool* found) const {
        auto slot = hash(pc) & mask_;
        while (slots_[slot].stamp == generation_) {
            if (slots_[slot].key == pc) {
                *found = true;
                return slot;
            }
            slot = (slot + 1) & mask_;
        }
        *found = false;
        return slot;
    }
    void insert_at(std::size_t slot, std::uint64_t pc) {
        slots_[slot].stamp = generation_;
        slots_[slot].key = pc;
        if (++count_ * 2 > size_) grow();
    }
    // Returns true if inserted (was absent).
    bool insert(std::uint64_t pc) {
        bool found;
        const auto slot = find_slot(pc, &found);
        if (found) return false;
        insert_at(slot, pc);
        return true;
    }
    [[nodiscard]] bool contains(std::uint64_t pc) const {
        bool found;
        (void)find_slot(pc, &found);
        return found;
    }

private:
    struct Slot {
        std::uint64_t key{};
        std::uint32_t stamp{};
    };
    static constexpr std::size_t kInitial = 1024;
    static std::size_t hash(std::uint64_t pc) {
        return static_cast<std::size_t>((pc >> 2) * 0x9e3779b97f4a7c15ULL >> 20);
    }
    // Rehashes the live keys into twice the slots (the generation changes, so
    // stale slots of earlier plans never count).
    void grow() {
        scratch_.clear();
        for (std::size_t i = 0; i < size_; ++i)
            if (slots_[i].stamp == generation_) scratch_.push_back(slots_[i].key);
        size_ *= 2;
        mask_ = size_ - 1;
        if (slots_.size() < size_) slots_.resize(size_, Slot{});
        if (++generation_ == 0) {
            for (auto& s : slots_) s.stamp = 0;
            generation_ = 1;
        }
        for (const auto key : scratch_) {
            auto slot = hash(key) & mask_;
            while (slots_[slot].stamp == generation_) slot = (slot + 1) & mask_;
            slots_[slot].stamp = generation_;
            slots_[slot].key = key;
        }
    }
    std::vector<Slot> slots_;
    std::vector<std::uint64_t> scratch_;
    std::size_t size_{};
    std::size_t mask_{};
    std::size_t count_{};
    std::uint32_t generation_{};
};

// Family describe() results by instruction word (describe is a pure function
// of the word; some families describe by emitting into a recording context,
// which costs far more than a lookup). Per thread, direct mapped, tagged with
// the family count (registration appends families).
struct DescribeMemo {
    std::uint32_t word;
    std::uint16_t families;   // registered_families().count + 1 (0 = empty slot)
    std::int16_t family;      // accepting family, -1 = none
    InstructionUse use;
};
constexpr unsigned kDescribeMemoSlots = 2048;

Flow flow_of(const Inst& inst) {
    switch (inst.kind) {
    case Kind::branch: return Flow::jump;
    case Kind::branch_cond: return inst.cond >= 14 ? Flow::jump : Flow::cond;
    case Kind::compare_branch:
    case Kind::test_branch: return Flow::cond;
    case Kind::branch_link:
    case Kind::branch_link_reg: return Flow::call;
    case Kind::branch_reg:
    case Kind::ret: return Flow::indirect;
    case Kind::brk:
    case Kind::hlt: return Flow::trap;
    case Kind::svc: return Flow::svc;
    case Kind::udf: return Flow::interpret;
    default: return Flow::normal;
    }
}

bool contains(const std::vector<std::uint64_t>& list, std::uint64_t pc) {
    return std::find(list.begin(), list.end(), pc) != list.end();
}

// Host registers for pins, in assignment order. The first seven are host
// callee-saved (they survive helper calls); r11 and r10 are volatile: the
// emitter saves and reloads them around helper calls, and a region whose
// family instructions run out of scratch registers is retranslated without
// them (PlanOverrides::no_extra_pins).
constexpr std::uint8_t kPinHosts[] = {3 /*rbx*/, 6 /*rsi*/, 7 /*rdi*/, 14 /*r14*/, 12 /*r12*/, 13 /*r13*/, 5 /*rbp*/,
                                      11 /*r11*/, 10 /*r10*/};
constexpr unsigned kCalleeSavedPins = 7;

// ---- jump tables ---------------------------------------------------------------
// MSVC ARM64 switch lowering (JumpTable in region_plan.hpp):
//   [cmp wI, #n; b.hi default | uxtb wI, ...]  ...
//   adr xT, table;  ldr{sw,sb,sh,b,h} xO, [xT, wI, uxtw #size]
//   [adr xB, base]; add xN, xB|xT, xO, lsl #s;  br xN
// LLVM's lowering (AArch64AsmPrinter::LowerJumpTableDest; clang, Mach-O):
//   [cmp wI, #n; b.hi default]  ...  [adrp xT, table; add xT, xT, :lo12:table]
//   adr xB, base;  ldr{b,h,sw} wO, [xT, xI{, lsl #size}];  add xN, xB, xO, lsl #2;  br xN
// where the table address is often set up before a loop (out of the
// straight-line window: a further backward scan finds it) and xI may be
// reused between the load and the BR. Those dispatch on the target
// (JumpTable::by_target).
// Found by a backward linear scan from the BR (straight-line code only). The
// result is a hint: the emitter checks the computed target against each
// entry at run time, so a wrong match or bound only costs speed.
constexpr unsigned kTableScan = 12;        // instructions scanned back from the BR
constexpr unsigned kTableBoundScan = 16;   // further back from the first ADR for the bound
constexpr unsigned kTableFarScan = 256;    // further back for a table address set up before the window
constexpr unsigned kMaxTableEntries = 1024;
// by_target tables: the dense host table covers the case pcs' span (4 bytes per guest instruction).
constexpr std::uint64_t kMaxTargetSpan = 4u * 4096u;

struct TableMatch {
    std::uint8_t index_reg;
    std::uint64_t table, base;
    unsigned size;       // log2 entry bytes
    bool sign;           // entries are sign-extended
    bool sign32;         // ... to 32 bits (W destination), then zero-extended
    unsigned shift;      // target = base + (entry << shift)
    unsigned count;
    bool by_target;      // the index is not usable at the BR: dispatch on the target
    bool absolute;       // 8-byte entries holding the targets themselves (label tables)
    const char* fail;    // diagnostics: the step that did not match
};

// Decodes the straight-line instruction at `pc` (false: unreadable or not core).
bool decode_at(const CodeSource& code, std::uint64_t pc, Inst& out) {
    std::uint32_t word = 0;
    if (!code.read(pc, word)) return false;
    decode(word, pc, out);
    return out.kind != Kind::unknown;
}

bool writes_reg(const Inst& d, unsigned r) { return ((usage(d).write >> r) & 1u) != 0; }
bool is_branch(const Inst& d) { return flow_of(d) != Flow::normal; }

// Value of an ADR / ADRP+ADD writing xR at `j` (insts[j]), else false.
bool adr_value(const Inst* insts, int j, unsigned r, std::uint64_t& value) {
    const auto& d = insts[j];
    if (d.kind == Kind::adr && d.rd == r) {
        value = static_cast<std::uint64_t>(d.imm);
        return true;
    }
    if (d.kind == Kind::add_sub_imm && d.op == 0 && !d.s && d.sf && d.rd == r && d.rn == r && j > 0 &&
        insts[j - 1].kind == Kind::adr && insts[j - 1].rd == r) {
        value = static_cast<std::uint64_t>(insts[j - 1].imm + d.imm);
        return true;
    }
    return false;
}

// Value of xR from its nearest writer before `pc` in address order (an ADR,
// or an ADD #imm of an ADRP), scanning back at most kTableFarScan
// instructions. Control flow is ignored, and instructions the core does not
// decode (FP/SIMD, hints) are skipped: a hint only (see above).
bool far_adr_value(const CodeSource& code, std::uint64_t pc, unsigned r, std::uint64_t& value) {
    std::uint64_t offset = 0;
    bool add_seen = false;
    for (unsigned k = 0; k < kTableFarScan && pc >= 4; ++k) {
        pc -= 4;
        std::uint32_t word = 0;
        if (!code.read(pc, word)) return false;
        Inst d;
        decode(word, pc, d);
        if (d.kind == Kind::unknown || !writes_reg(d, r)) continue;
        if (d.kind == Kind::adr && d.rd == r) {
            value = static_cast<std::uint64_t>(d.imm) + offset;
            return true;
        }
        // ADD xR, xS, #imm: then xS's own writer (an ADRP), once.
        if (add_seen || d.kind != Kind::add_sub_imm || d.op != 0 || d.s || !d.sf || d.rd != r || d.rn == 31) return false;
        add_seen = true;
        offset = static_cast<std::uint64_t>(d.imm);
        r = d.rn;
    }
    return false;
}

// The number of table entries the index wI can select, from the code before
// the table load at `ld_pc` (at most `limit` instructions back): cmp + b.hi/b.hs
// past the default, or the index's own range. 0: none found.
unsigned scan_bound(const CodeSource& code, std::uint64_t ld_pc, unsigned limit, unsigned ri) {
    std::uint64_t pc = ld_pc;
    unsigned count = 0;
    for (unsigned k = 0; k < limit && count == 0; ++k) {
        pc -= 4;
        Inst d;
        if (!decode_at(code, pc, d)) break;
        if (d.kind == Kind::branch_cond && (d.cond == 8 || d.cond == 2)) {   // b.hi / b.hs past the default
            for (unsigned c = 1; c <= 4; ++c) {
                Inst f;
                if (!decode_at(code, pc - 4u * c, f) || is_branch(f)) break;
                const auto u = usage(f);
                if (f.kind == Kind::add_sub_imm && f.op == 1 && f.s && f.rd == 31 && f.rn == ri) {   // cmp wI/xI, #imm
                    const auto imm = static_cast<std::uint64_t>(f.imm);
                    count = static_cast<unsigned>(std::min<std::uint64_t>(d.cond == 8 ? imm + 1 : imm, kMaxTableEntries + 1));
                    break;
                }
                if (u.writes_flags || ((u.write >> ri) & 1u)) break;
            }
            break;
        }
        if (is_branch(d)) break;
        if (writes_reg(d, ri)) {
            // The index's own range: UBFX/UXTB/LSR (a field of imms - immr + 1 bits), AND with a low
            // mask, or an unsigned byte load (switch on a char read from memory).
            if (d.kind == Kind::bitfield && d.op == 2 && d.imms >= d.amount && d.imms - d.amount < 10)
                count = 1u << (d.imms - d.amount + 1);
            else if (d.kind == Kind::logic_imm && d.op == 0 && (static_cast<std::uint64_t>(d.imm) & (static_cast<std::uint64_t>(d.imm) + 1)) == 0)
                count = static_cast<unsigned>(std::min<std::uint64_t>(static_cast<std::uint64_t>(d.imm) + 1, kMaxTableEntries + 1));
            else if (d.kind == Kind::load_store && d.mem == MemOp::load && d.size == 0 && d.rd == ri)
                count = 256;
            break;
        }
    }
    return count;
}

// A label table (computed goto: `goto *labels[i]`, or a dispatch table):
//   [adrp xT, labels; add xT, xT, :lo12:labels]  ...  ldr xN, [xT, xI, lsl #3];  br xN
// with absolute 8-byte entries in read-only data (CodeSource::read_data). It
// dispatches on the target (by_target). With no bound the caller sizes it from
// its contents. A table whose address is not an ADR/ADRP+ADD (V8's Ignition
// dispatch table, in the heap) is not one.
bool match_label_table(const Inst* insts, int lo, int ld, std::uint64_t br_pc, const CodeSource& code, TableMatch& m) {
    const auto& l = insts[ld];
    const bool extend_ok = l.shift == 2 || l.shift == 3 || l.shift == 6 || l.shift == 7;
    // The index scaled by 8 (lsl #3), or already a byte offset (amount 0).
    if (l.mem != MemOp::load || l.rn == 31 || l.rm == 31 || (l.amount != 3 && l.amount != 0) || !extend_ok) {
        m.fail = "xN loaded from [xT, xI]: not a label table load";
        return false;
    }
    const unsigned rt = l.rn, ri = l.rm;
    int tdef = -1;
    for (int j = ld - 1; j >= lo && tdef < 0; --j)
        if (writes_reg(insts[j], rt)) tdef = j;
    const auto window_pc = br_pc - 4u * (kTableScan - static_cast<unsigned>(lo));
    if (tdef >= 0 ? !adr_value(insts, tdef, rt, m.table) : !far_adr_value(code, window_pc, rt, m.table)) {
        m.fail = "xN loaded from [xT, xI]: table not at an ADR/ADRP+ADD (a dispatch table in memory)";
        return false;
    }
    m.index_reg = static_cast<std::uint8_t>(ri);
    m.size = 3;
    m.sign = m.sign32 = false;
    m.shift = 0;
    m.base = 0;
    m.absolute = true;
    m.by_target = true;
    int first = tdef >= 0 ? std::min(ld, tdef) : ld;
    if (first > 0 && insts[first].kind == Kind::add_sub_imm) --first;
    m.count = scan_bound(code, br_pc - 4u * (kTableScan - static_cast<unsigned>(ld)),
                         kTableBoundScan + static_cast<unsigned>(ld - first), ri);
    if (l.amount == 0) m.count = (m.count + 7) / 8;   // a byte-offset bound
    return m.count <= kMaxTableEntries;   // 0: sized from the contents (build_plan)
}

// A BR on a table shadow (PlanInst::shadow_base): xN's last writer before the
// BR is `ldr xN, [xB, xI, lsl #3]` (or `uxtw #3`), possibly through register
// copies (`mov x17, xN; br x17`: V8's tail calls through its CFI scratch
// register), with neither xB nor xI written after the load. Any table: a
// label table takes the BR first (match_table), and the target is compared
// either way. Returns nullptr, or why not (TRANSMUTE_FX_TRACE_TABLES).
const char* match_shadow_load(std::uint64_t br_pc, unsigned rn, const CodeSource& code, std::int8_t& base,
                              std::int8_t& index, bool& index32) {
    if (rn >= 31 || br_pc < 4 * kTableScan) return "no register";
    constexpr unsigned kMaxCopies = 2;
    unsigned want = rn, copies = 0;
    std::uint32_t written = 0;   // registers written between the load and the BR
    for (unsigned back = 1; back <= kTableScan; ++back) {
        Inst d;
        if (!decode_at(code, br_pc - 4u * back, d) || is_branch(d)) return "no writer of the target in the block";
        if (!writes_reg(d, want)) {
            written |= usage(d).write;
            continue;
        }
        written |= usage(d).write;
        if (d.kind == Kind::logic_shift && d.op == 1 && d.sf && !d.invert && d.rn == 31 && d.amount == 0 &&
            d.rd == want && d.rm != 31) {   // mov xT, xS: follow xS
            if (++copies > kMaxCopies) return "more register copies than followed";
            want = d.rm;
            continue;
        }
        if (d.kind != Kind::load_store || d.mode != AddrMode::reg || d.mem != MemOp::load || d.rd != want)
            return "the target's writer is not a register-offset load";
        if (d.size != 3 || d.amount != 3 || (d.shift != 2 && d.shift != 3)) return "not an 8-byte load scaled by lsl/uxtw #3";
        if (d.rn == 31 || d.rm == 31 || d.rn == d.rd || d.rm == d.rd) return "base or index is sp, zr or the load's target";
        if (((written >> d.rn) & 1u) != 0 || ((written >> d.rm) & 1u) != 0) return "base or index written after the load";
        base = static_cast<std::int8_t>(d.rn);
        index = static_cast<std::int8_t>(d.rm);
        index32 = d.shift == 2;
        return nullptr;
    }
    return "no writer of the target within the scan";
}

bool match_table(std::uint64_t br_pc, unsigned rn, const CodeSource& code, TableMatch& m) {
    const auto no = [&](const char* why) {
        m.fail = why;
        return false;
    };
    if (rn >= 31 || br_pc < 4 * kTableScan) return no("no register");
    // insts[0] is the furthest back; insts[kTableScan - 1] precedes the BR.
    Inst insts[kTableScan];
    int lo = static_cast<int>(kTableScan);
    for (int j = static_cast<int>(kTableScan) - 1; j >= 0; --j) {
        const auto pc = br_pc - 4u * (kTableScan - static_cast<unsigned>(j));
        if (!decode_at(code, pc, insts[j]) || is_branch(insts[j])) break;
        lo = j;
    }
    // The writer of xN: add xN, xB, xO, lsl #s.
    int add = -1;
    for (int j = static_cast<int>(kTableScan) - 1; j >= lo && add < 0; --j)
        if (writes_reg(insts[j], rn)) add = j;
    if (add < 0) return no("no writer of xN");
    const auto& a = insts[add];
    if (a.kind != Kind::add_sub_shift || a.op != 0 || a.s || !a.sf || a.shift != 0 || a.amount > 4 || a.rn == 31 ||
        a.rm == 31 || a.rd != rn) {
        if (a.kind == Kind::load_store && a.mode == AddrMode::reg && a.size == 3 && a.rd == rn)
            return match_label_table(insts, lo, add, br_pc, code, m);
        return no("xN not ADD xB, xO, LSL");
    }
    const unsigned rb = a.rn, ro = a.rm;
    // The writer of xO: the table load.
    int ld = -1;
    for (int j = add - 1; j >= lo && ld < 0; --j)
        if (writes_reg(insts[j], ro)) ld = j;
    if (ld < 0) return no("no writer of xO");
    const auto& l = insts[ld];
    // Index extend: UXTW (MSVC), LSL/UXTX or SXTW/SXTX (LLVM).
    const bool extend_ok = l.shift == 2 || l.shift == 3 || l.shift == 6 || l.shift == 7;
    if (l.kind != Kind::load_store || l.mode != AddrMode::reg || l.rd != ro || l.rn == 31 || l.rm == 31 ||
        !extend_ok || l.size > 2 || (l.amount != l.size && !(l.amount == 0 && l.size == 0)))
        return no("xO not a register-offset load");
    if (l.mem != MemOp::load && l.mem != MemOp::load_signed64 && l.mem != MemOp::load_signed32) return no("xO load kind");
    const unsigned rt = l.rn, ri = l.rm;
    // Index dispatch needs wI (UXTW) to be the index until the BR, else the
    // table dispatches on the target.
    bool by_target = l.shift != 2;
    for (int j = ld + 1; j < static_cast<int>(kTableScan); ++j)
        if (writes_reg(insts[j], ri)) by_target = true;
    // The table address: ADR xT (or ADRP+ADD) before the load, xT unchanged in
    // between; or, with no writer in the window, set up further back.
    int tdef = -1;
    for (int j = ld - 1; j >= lo && tdef < 0; --j)
        if (writes_reg(insts[j], rt)) tdef = j;
    const auto window_pc = br_pc - 4u * (kTableScan - static_cast<unsigned>(lo));   // insts[lo]
    if (tdef >= 0) {
        if (!adr_value(insts, tdef, rt, m.table)) return no("table: not ADR/ADRP+ADD");
    } else {
        if (!far_adr_value(code, window_pc, rt, m.table)) return no("table: no ADR/ADRP+ADD found");
        by_target = true;
    }
    // The base: xB's writer before the add (likewise).
    int bdef = -1;
    for (int j = add - 1; j >= lo && bdef < 0; --j)
        if (writes_reg(insts[j], rb)) bdef = j;
    if (bdef >= 0) {
        if (!adr_value(insts, bdef, rb, m.base)) return no("base: not ADR/ADRP+ADD");
    } else {
        if (!far_adr_value(code, window_pc, rb, m.base)) return no("base: no ADR/ADRP+ADD found");
        by_target = true;
    }
    m.index_reg = static_cast<std::uint8_t>(ri);
    m.size = l.size;
    m.sign = l.mem != MemOp::load;
    m.sign32 = l.mem == MemOp::load_signed32;
    m.shift = a.amount;
    m.by_target = by_target;
    // The bound: scan back from the table load, kTableBoundScan instructions past the first
    // instruction of the pattern in the window (the table and base setup between them never
    // writes wI). A table address set up before a loop may be early in the window, before wI's
    // writer, so the scan starts at the load.
    int first = ld;
    if (tdef >= 0) first = std::min(first, tdef);
    if (bdef >= 0) first = std::min(first, bdef);
    if (first > 0 && insts[first].kind == Kind::add_sub_imm) --first;   // ADRP of an ADRP+ADD
    m.count = scan_bound(code, br_pc - 4u * (kTableScan - static_cast<unsigned>(ld)),
                         kTableBoundScan + static_cast<unsigned>(ld - first), ri);
    // 0: no bound found (the caller may size an MSVC table from its contents;
    // LLVM places tables in a data section, so by_target ones need a bound).
    if (m.by_target && m.count == 0) return no("no bound");
    return m.count <= kMaxTableEntries;
}

// Entry k's guest target (false: the table is unreadable there).
bool table_target(const CodeSource& code, const TableMatch& m, unsigned k, std::uint64_t& target) {
    const auto at = m.table + (static_cast<std::uint64_t>(k) << m.size);
    std::uint32_t word = 0;
    if (m.absolute) {
        std::uint32_t high = 0;
        if ((at & 3) != 0 || !code.read_data(at, word) || !code.read_data(at + 4, high)) return false;
        target = (static_cast<std::uint64_t>(high) << 32) | word;
        return true;
    }
    if (!code.read_data(at & ~std::uint64_t{3}, word)) return false;
    std::uint64_t raw = word >> (8 * (at & 3));
    std::int64_t value;
    switch (m.size) {
    case 0: value = m.sign ? static_cast<std::int8_t>(raw) : static_cast<std::int64_t>(raw & 0xff); break;
    case 1:
        if ((at & 1) != 0) return false;
        value = m.sign ? static_cast<std::int16_t>(raw) : static_cast<std::int64_t>(raw & 0xffff);
        break;
    default:
        if ((at & 3) != 0) return false;
        value = m.sign ? static_cast<std::int32_t>(word) : static_cast<std::int64_t>(word);
        break;
    }
    auto v = static_cast<std::uint64_t>(value);
    if (m.sign32) v &= 0xffffffffu;   // LDRSB/LDRSH Wt: sign-extended to 32 bits only
    target = m.base + (v << m.shift);
    return true;
}

} // namespace

// p1 replaces this with a straight-line scan of image code from target_pc
// (the first NZCV write before any read, branch or exit means dead; the
// scanned bytes become a dependency of the region). Until then every target
// is assumed to read the flags.
bool target_flags_dead(const CodeSource& code, std::uint64_t target_pc, Plan& plan) {
    (void)code;
    (void)target_pc;
    (void)plan;
    return false;
}

void build_plan(std::uint64_t guest_pc, const CodeSource& code, const TranslateOptions& options,
                const PlanOverrides& overrides, Plan& plan) {
    thread_local PcSet visited;
    thread_local std::vector<std::uint64_t> stack;
    thread_local std::vector<std::uint64_t> continuations;
    thread_local std::vector<std::int32_t> depth;
    thread_local std::vector<std::pair<std::uint64_t, std::uint32_t>> order;
    thread_local std::vector<PlanInst> sorted;
    // Stack entries pushed as branch targets or continuations carry this bit:
    // only there can another region's entry start (has_entry may be costly,
    // e.g. a runtime lookup), so fall-through successors skip the check.
    constexpr std::uint64_t kCheckEntry = 1ULL << 63;

    const bool lazy = options.lazy && !overrides.eager;
    const unsigned full_insts = std::max(1u, options.max_instructions);
    const unsigned max_insts = lazy ? std::max(1u, std::min(full_insts, options.lazy_max_instructions)) : full_insts;
    plan.truncated = false;
    plan.lookup_return_stubs = overrides.eager;
    bool skipped_continuation = false;
    plan.start = guest_pc;
    plan.insts.clear();
    plan.words.clear();
    plan.entries.clear();
    plan.interpret_count = 0;
    plan.inlined_calls = 0;
    plan.inlined_insts = 0;
    plan.blocks = 0;
    plan.reg_cache = !overrides.no_reg_cache;
    plan.fcmp_fusion = !overrides.no_fcmp_fusion;
    plan.rbit_fusion = !overrides.no_rbit_fusion;
    plan.write_back = !overrides.no_write_back;
    plan.reuse_dest = !overrides.no_reuse_dest;
    plan.vec_facts = !overrides.no_vec_facts;
    plan.vzero_join = !overrides.no_vzero_join;
    const bool tier2 = options.optimize > 0 || (options.optimize < 0 && overrides.tier2);
    plan.count_hot = tier2 && options.lazy;
    plan.count_loops = plan.count_hot && (overrides.t2_loops || options.optimize == 2);
    plan.optimize = tier2 && !options.lazy;
    plan.vec_alias = plan.optimize && !overrides.t2_no_alias && !overrides.no_vec_alias;
    plan.t2_checks = plan.optimize && !overrides.t2_no_checks;
    plan.t2_align = plan.optimize && !overrides.t2_no_align;
    plan.t2_alloc = plan.optimize && !overrides.t2_no_alloc;
    plan.t2_dead = plan.optimize && !overrides.t2_no_dead;
    plan.t2_lend = plan.optimize && overrides.t2_lend && !overrides.no_pins;
    plan.t2_galias = plan.optimize && !overrides.t2_no_galias && !overrides.no_galias;
    plan.t2_derived = plan.optimize && !overrides.t2_no_derived;
    // Tier-up: counted first-tier regions count loop iterations at their loop
    // headers (a region without loops is final), or, with t2entries (the
    // prototype), their entries (see RegionEmitter::run).
    plan.count_loop_headers = plan.count_hot && !overrides.t2_entries && options.optimize != 3 && !plan.count_loops;
    plan.wb_fault_flush = !overrides.wb_unsafe;
    plan.fault_maps = !overrides.no_fault_maps;
    plan.carry = !overrides.no_carry;
    plan.belady = !overrides.lru;
    plan.cold_layout = !overrides.old_layout;
    plan.inline_entries = !overrides.cold_entries;
    plan.align_loops = overrides.align_loops;
    plan.align_hot = overrides.align_hot;
    plan.loop_alignment = overrides.align32 ? 32u : 16u;
    plan.carry_clean = !overrides.carry_dirty && !(plan.optimize && overrides.t2_carry);   // t2carry: dirty across loop edges
    stack.clear();
    continuations.clear();
    visited.reset(std::min<std::size_t>(max_insts, 1u << 16) + 16);

    std::uint64_t bound_begin = 0, bound_end = ~0ULL;
    bool function_bounded = false;   // bounds of a real function (.pdata), not a synthetic window
    {
        std::uint64_t b = 0, e = 0;
        if (code.function_bounds(guest_pc, b, e) && b <= guest_pc && guest_pc < e) {
            bound_begin = b;
            bound_end = e;
            function_bounded = true;
        } else if (!overrides.eager && (options.window_forward != 0 || options.window_backward != 0)) {
            // No known bounds: a window around the start keeps regions of
            // neighbouring functions from covering each other's code.
            bound_begin = guest_pc > options.window_backward ? guest_pc - options.window_backward : 0;
            bound_end = guest_pc + std::max<std::uint64_t>(4, options.window_forward);
            if (bound_end < guest_pc) bound_end = ~0ULL;
        }
    }
    const auto families = registered_families();

    thread_local DescribeMemo memo[kDescribeMemoSlots];
    const auto memo_tag = static_cast<std::uint16_t>(families.count + 1);
    std::uint64_t min_pc = ~0ULL, max_pc = 0;

    // Decodes `word` at `pc` into `pi`: flow, family, register usage (with
    // the retry overrides applied).
    const auto describe_inst = [&](PlanInst& pi, std::uint64_t pc, std::uint32_t word) {
        pi.pc = pc;
        decode(word, pc, pi.inst);
        pi.flow = flow_of(pi.inst);
        if ((pc & 3) != 0) pi.flow = Flow::interpret;
        if (pi.inst.kind == Kind::unknown && pi.flow == Flow::normal) {
            pi.flow = Flow::interpret;
            pi.try_family = (pc & 3) == 0 && families.count != 0;
            DescribeMemo& m = memo[(word ^ (word >> 11) ^ (word >> 22)) & (kDescribeMemoSlots - 1)];
            if (m.families != memo_tag || m.word != word) {
                m.word = word;
                m.families = memo_tag;
                m.family = -1;
                m.use = {};
                for (unsigned f = 0; f < families.count; ++f) {
                    InstructionUse use{};
                    if (families.families[f].describe && families.families[f].describe(word, use)) {
                        m.family = static_cast<std::int16_t>(f);
                        m.use = use;
                        break;
                    }
                }
            }
            if (m.family >= 0) {
                const InstructionUse& use = m.use;
                pi.family = m.family;
                pi.flow = Flow::normal;
                pi.reads = use.gpr_read;
                pi.writes = use.gpr_write;
                pi.vreads = use.vec_read;
                pi.vwrites = use.vec_write;
                pi.reads_flags = use.reads_flags;
                pi.writes_flags = use.writes_flags;
                pi.may_fault = use.may_fault;
            }
        } else if (pi.inst.kind != Kind::unknown) {
            const auto u = usage(pi.inst);
            pi.reads = u.read;
            pi.writes = u.write;
            pi.reads_flags = u.reads_flags;
            pi.writes_flags = u.writes_flags;
            switch (pi.inst.kind) {
            case Kind::load_store:
            case Kind::load_store_pair:
            case Kind::load_literal:
            case Kind::udf:
            case Kind::brk:
            case Kind::hlt:
            case Kind::svc: pi.may_fault = true; break;
            default: pi.may_fault = false; break;
            }
        }
        if (!overrides.interpret.empty() && contains(overrides.interpret, pc)) {
            pi.flow = Flow::interpret;
            pi.family = -1;
            pi.try_family = false;
            pi.inst.kind = pi.inst.kind == Kind::udf ? Kind::udf : Kind::unknown;
        }
        if (pi.family >= 0 && !overrides.conservative.empty() && contains(overrides.conservative, pc)) {
            pi.conservative_writes = true;
            pi.may_fault = true;
            pi.writes = 0xffffffffu;
            pi.reads = 0xffffffffu;
            pi.vwrites = 0xffffffffu;
            pi.vreads = 0xffffffffu;
            pi.reads_flags = true;
            pi.writes_flags = false;
        }
        if (pi.flow == Flow::interpret) {
            pi.may_fault = true;
            pi.reads = 0xffffffffu;
            pi.writes = 0;
            pi.vreads = 0xffffffffu;
            pi.vwrites = 0;
            pi.reads_flags = true;
            pi.writes_flags = false;
        }
    };

    // ---- call inlining ---------------------------------------------------------
    // A BL target is inlined when its body is a small leaf: every instruction
    // reachable from the target through direct control flow (inside its .pdata
    // bounds, which must start at the target, else a window after it) is at
    // most overrides.inline_max instructions of straight-line code, branches
    // and loops, with no calls, SVC/BRK/HLT, interpret exits or indirect
    // branches other than RET X30, and no other instruction reads or writes
    // x30. x30 then holds the return address throughout the body (the BL
    // writes it), so RET X30 transfers to the BL's continuation: the copy
    // needs no RSB push or pop, and fault sites and exits inside it see the
    // exact guest state (callee pcs, x30, SP). Direct branches out of the body
    // (tail calls) become ordinary direct exits. Bodies are copied after their
    // BL in layout (segment k), so a call is straight-line code.
    thread_local std::vector<PlanInst> bodies;            // callee instructions, sorted by pc per body
    struct Body {
        std::uint64_t target;
        std::uint32_t first, count;   // bodies[first, first + count), or count 0: not inlinable
    };
    thread_local std::vector<Body> body_list;
    thread_local std::vector<std::uint64_t> body_stack;
    bodies.clear();
    body_list.clear();
    const bool inline_on = !overrides.no_inline && overrides.inline_max != 0;
    const bool defer_inline = lazy && overrides.inline_hot;   // tier 0: counted, inlined at tier 1
    bool deferred_inline = false;
    unsigned inlined_total = 0;
    constexpr unsigned kMaxInlinedPerRegion = 1024;
    const auto inline_body = [&](std::uint64_t target) -> std::int32_t {
        for (std::size_t k = 0; k < body_list.size(); ++k)
            if (body_list[k].target == target) return body_list[k].count ? static_cast<std::int32_t>(k) : -1;
        const auto first = static_cast<std::uint32_t>(bodies.size());
        const auto reject = [&] {
            bodies.resize(first);
            body_list.push_back({target, first, 0});
            return std::int32_t{-1};
        };
        const unsigned limit = overrides.inline_max;
        std::uint64_t lo = target, hi = target + 8ull * limit;
        if ((target & 3) != 0) return reject();
        {
            std::uint64_t b = 0, e = 0;
            if (code.function_bounds(target, b, e)) {
                if (b != target || e <= b || e - b > 64ull * limit) return reject();   // not a function start / far too big
                hi = e;
            }
        }
        body_stack.clear();
        body_stack.push_back(target);
        bool has_ret = false;
        while (!body_stack.empty()) {
            const auto pc = body_stack.back();
            body_stack.pop_back();
            bool seen = false;
            for (std::uint32_t k = first; k < bodies.size() && !seen; ++k) seen = bodies[k].pc == pc;
            if (seen) continue;
            if (bodies.size() - first >= limit) return reject();
            std::uint32_t word = 0;
            if (!code.read(pc, word)) return reject();
            PlanInst& pi = bodies.emplace_back();
            describe_inst(pi, pc, word);
            const bool is_ret = pi.inst.kind == Kind::ret && pi.inst.rn == 30;
            if (pi.conservative_writes || (((pi.reads | pi.writes) >> 30) & 1u) != 0u) {
                if (!is_ret || (pi.writes >> 30) & 1u) return reject();   // the body uses x30 itself
            }
            const auto t = static_cast<std::uint64_t>(pi.inst.imm);
            const auto inside = [&](std::uint64_t a) { return a >= lo && a < hi && (a & 3) == 0; };
            switch (pi.flow) {
            case Flow::normal:
                if (!inside(pc + 4)) return reject();   // falls out of the body
                body_stack.push_back(pc + 4);
                break;
            case Flow::jump:
                if (inside(t)) body_stack.push_back(t);   // else a tail call: a direct exit
                break;
            case Flow::cond:
                if (inside(t)) body_stack.push_back(t);
                if (!inside(pc + 4)) return reject();
                body_stack.push_back(pc + 4);
                break;
            case Flow::indirect:
                if (!is_ret) return reject();
                has_ret = true;
                break;
            default: return reject();   // calls, SVC, traps, interpret exits
            }
        }
        if (!has_ret) return reject();
        std::sort(bodies.begin() + first, bodies.end(),
                  [](const PlanInst& a, const PlanInst& b) { return a.pc < b.pc; });
        body_list.push_back({target, first, static_cast<std::uint32_t>(bodies.size() - first)});
        return static_cast<std::int32_t>(body_list.size() - 1);
    };

    // Jump tables found at BRs (switch dispatch): their targets are followed
    // like branch targets. A region holding one may grow to kTableRegionScale
    // times the instruction limit (interpreter loops: one region per switch).
    struct FoundTable {
        std::uint64_t br_pc;
        std::uint32_t first, count;
        std::uint8_t index_reg;
        bool by_target;
    };
    thread_local std::vector<FoundTable> found_tables;
    thread_local std::vector<std::uint64_t> found_pcs;
    // BRs on table shadows (PlanInst::shadow_base), unless a table takes them.
    struct FoundShadow {
        std::uint64_t br_pc;
        std::int8_t base, index;
        bool index32;
    };
    thread_local std::vector<FoundShadow> found_shadows;
    found_shadows.clear();
    found_tables.clear();
    found_pcs.clear();
    plan.tables.clear();
    plan.table_pcs.clear();
    plan.table_targets.clear();
    constexpr unsigned kTableRegionScale = 4;
    constexpr unsigned kTableRegionMax = 16384;
    std::size_t insn_limit = max_insts;
    // A region may cover its whole .pdata function (up to kTableRegionMax
    // instructions) rather than the first max_instructions it finds. Cut at
    // the limit, a large function was translated again from every exit into
    // its untranslated part, and the depth-first walk of each such region
    // re-covered most of the first one: Classroom's integrate_surface had
    // two hot regions sharing 93% of their 4096 instructions. Not for a
    // first-tier budget below the limit (lazy experiments), small limits
    // (tests that force region splits), nor with TRANSMUTE_FX_CORE=oldcap.
    if (function_bounded && !overrides.old_cap && max_insts == full_insts && full_insts >= 1024)
        insn_limit = std::max<std::size_t>(insn_limit, std::min<std::uint64_t>(kTableRegionMax, (bound_end - bound_begin) / 4));

    stack.push_back(guest_pc);
    while (!stack.empty() && plan.insts.size() < insn_limit) {
        const auto item = stack.back();
        stack.pop_back();
        const auto pc = item & ~kCheckEntry;
        bool seen;
        const auto slot = visited.find_slot(pc, &seen);   // insert_at below: no insertion in between
        if (seen) continue;
        if (pc != guest_pc && ((pc & 3) != 0 || pc < bound_begin || pc >= bound_end)) continue;
        if (pc != guest_pc && (item & kCheckEntry) && code.has_entry(pc)) continue;
        std::uint32_t word = 0;
        if (!code.read(pc, word)) {
            if (pc == guest_pc) throw std::runtime_error("translate_region: guest pc is not readable");
            continue;
        }
        visited.insert_at(slot, pc);
        min_pc = std::min(min_pc, pc);
        max_pc = std::max(max_pc, pc);
        PlanInst& pi = plan.insts.emplace_back();
        describe_inst(pi, pc, word);
        if (pi.flow == Flow::interpret) ++plan.interpret_count;
        const auto target = static_cast<std::uint64_t>(pi.inst.imm);
        if (pi.flow == Flow::call && pi.inst.kind == Kind::branch_link && inline_on &&
            inlined_total < kMaxInlinedPerRegion) {
            const auto body = inline_body(target);
            if (body >= 0 && defer_inline) {
                deferred_inline = true;   // tier 0 stays a plain call; the tier-1 translation inlines it
            } else if (body >= 0 && inlined_total + body_list[static_cast<std::size_t>(body)].count <= kMaxInlinedPerRegion) {
                inlined_total += body_list[static_cast<std::size_t>(body)].count;
                pi.inline_call = true;
                pi.inline_body = body;
                pi.link_pc = pc + 4;
                pi.flow = Flow::normal;   // falls into the callee copy (edges set below)
                if (pc + 4 >= bound_begin && pc + 4 < bound_end) stack.push_back((pc + 4) | kCheckEntry);
                continue;
            }
        }
        switch (pi.flow) {
        case Flow::normal: stack.push_back(pc + 4); break;
        case Flow::jump: stack.push_back(target | kCheckEntry); break;
        case Flow::cond:
            stack.push_back(target | kCheckEntry);
            stack.push_back(pc + 4);
            break;
        case Flow::call:
            // First tier: code after a call runs only once the call returns
            // here; it becomes its own region then (the RSB entry is a direct
            // exit, linked to it).
            if (lazy && options.lazy_split_calls) {
                skipped_continuation |= pc + 4 >= bound_begin && pc + 4 < bound_end;
                break;
            }
            [[fallthrough]];
        case Flow::svc:
        case Flow::interpret:
            if (pc + 4 >= bound_begin && pc + 4 < bound_end) {
                continuations.push_back(pc + 4);
                stack.push_back((pc + 4) | kCheckEntry);
            }
            break;
        case Flow::indirect:
            if (pi.inst.kind == Kind::branch_reg && !overrides.no_shadows) {
                FoundShadow s{pc, -1, -1, false};
                const char* why = match_shadow_load(pc, pi.inst.rn, code, s.base, s.index, s.index32);
                if (!why) found_shadows.push_back(s);
                static const bool trace = std::getenv("TRANSMUTE_FX_TRACE_TABLES") != nullptr;
                if (trace && !why)
                    std::fprintf(stderr, "table: br %llx: table shadow candidate (x%d, x%d%s)\n",
                                 static_cast<unsigned long long>(pc), s.base, s.index, s.index32 ? " uxtw" : "");
                else if (trace)
                    std::fprintf(stderr, "table: br %llx: no table shadow: %s\n", static_cast<unsigned long long>(pc), why);
            }
            if (pi.inst.kind == Kind::branch_reg && !overrides.no_tables) {
                TableMatch m{};
                const bool matched = match_table(pc, pi.inst.rn, code, m);
                // Diagnostics: TRANSMUTE_FX_TRACE_TABLES=1 prints every BR (table or not).
                static const bool trace_tables = std::getenv("TRANSMUTE_FX_TRACE_TABLES") != nullptr;
                if (trace_tables)
                    std::fprintf(stderr, "table: br %llx %s %u entries\n", static_cast<unsigned long long>(pc),
                                 matched ? (m.absolute ? "matched label table" : m.by_target ? "matched by target" : "matched")
                                         : "not matched",
                                 matched ? m.count : 0u);
                if (trace_tables && !matched && m.fail)
                    std::fprintf(stderr, "table: br %llx: %s\n", static_cast<unsigned long long>(pc), m.fail);
                if (matched && m.absolute) {
                    // Label tables: the entries while they are guest code (within the bound, if
                    // any); a computed goto usually has no bound check at all.
                    const unsigned limit = m.count ? m.count : kMaxTableEntries;
                    unsigned k = 0;
                    std::uint32_t word = 0;
                    for (std::uint64_t t = 0; k < limit; ++k)
                        if (!table_target(code, m, k, t) || (t & 3) != 0 || !code.read(t, word)) break;
                    m.count = k >= 2 ? k : 0;
                    if (trace_tables && m.count == 0)
                        std::fprintf(stderr, "table: br %llx: label table without code entries\n", static_cast<unsigned long long>(pc));
                } else if (matched && m.count == 0 && bound_end != ~0ULL && m.table >= bound_begin && m.table < bound_end) {
                    // No bound in sight (checked elsewhere): MSVC places the
                    // tables after the function's code, so entries run while
                    // they point into the function below the table.
                    unsigned k = 0;
                    for (std::uint64_t t = 0; k < kMaxTableEntries; ++k)
                        if (!table_target(code, m, k, t) || (t & 3) != 0 || t < bound_begin || t >= m.table) break;
                    m.count = k >= 2 ? k : 0;
                }
                if (matched && m.count != 0) {
                    const auto first = static_cast<std::uint32_t>(found_pcs.size());
                    unsigned inside = 0;
                    bool ok = true;
                    for (unsigned k = 0; k < m.count && ok; ++k) {
                        std::uint64_t t = 0;
                        ok = table_target(code, m, k, t);
                        found_pcs.push_back(t);
                        if (ok && (t & 3) == 0 && t >= bound_begin && t < bound_end) ++inside;
                    }
                    if (!ok || inside * 2 < m.count) {   // not a table of this function
                        if (trace_tables)
                            std::fprintf(stderr, "table: br %llx: %s\n", static_cast<unsigned long long>(pc),
                                         ok ? "cases outside the function" : "table unreadable");
                        found_pcs.resize(first);
                        break;
                    }
                    if (m.by_target) {   // the dense host table spans the aligned case pcs
                        std::uint64_t lo_t = ~0ULL, hi_t = 0;
                        for (unsigned k = 0; k < m.count; ++k) {
                            const auto t = found_pcs[first + k];
                            if ((t & 3) != 0) continue;
                            lo_t = std::min(lo_t, t);
                            hi_t = std::max(hi_t, t);
                        }
                        if (lo_t > hi_t || hi_t - lo_t >= kMaxTargetSpan) {
                            if (trace_tables)
                                std::fprintf(stderr, "table: br %llx: cases span too far\n", static_cast<unsigned long long>(pc));
                            found_pcs.resize(first);
                            break;
                        }
                    }
                    found_tables.push_back({pc, first, m.count, m.index_reg, m.by_target});
                    if (overrides.table_exits) break;   // every entry a linkable direct exit (cases are own regions)
                    insn_limit = std::max<std::size_t>(insn_limit, std::min(kTableRegionMax, kTableRegionScale * max_insts));
                    // Pushed in reverse: the depth-first walk then lays out low cases first.
                    for (unsigned k = m.count; k-- > 0;) {
                        const auto t = found_pcs[first + k];
                        if ((t & 3) == 0 && t >= bound_begin && t < bound_end) stack.push_back(t | kCheckEntry);
                    }
                }
            }
            break;
        case Flow::trap: break;
        }
    }

    if (lazy) {
        // Cut short: a larger translation would cover more. The region is
        // tier 0 (counted, replaced once hot).
        bool truncated = !stack.empty() && max_insts < full_insts;
        if (!truncated && skipped_continuation) {
            for (const auto& pi : plan.insts) {
                if (pi.flow != Flow::call) continue;
                const auto next = pi.pc + 4;
                if (next < bound_begin || next >= bound_end || visited.contains(next)) continue;
                if (code.has_entry(next)) continue;   // covered by another region either way
                truncated = true;
                break;
            }
        }
        // TRANSMUTE_FX_CORE=inlinehot: a region that would inline calls is
        // tier 0 (counted); its tier-1 translation inlines them.
        plan.truncated = truncated || deferred_inline;
    }

    // Layout in guest address order. Regions are mostly dense in their pc
    // range: place instructions by (pc - min) / 4, which also gives O(1) pc ->
    // index lookups below. Sparse ones sort (pc, index) pairs, then permute
    // once (PlanInst is large; sorting it directly is dominated by moves).
    thread_local std::vector<std::int32_t> slot_of;   // (pc - min_pc) / 4 -> index (dense layout)
    const auto count = plan.insts.size();
    const std::uint64_t span = count ? (max_pc - min_pc) / 4 + 1 : 0;
    const bool dense = count != 0 && (min_pc & 3) == 0 && (max_pc & 3) == 0 && span <= 4 * count + 64;
    if (dense) {
        slot_of.assign(static_cast<std::size_t>(span), -1);
        for (std::uint32_t i = 0; i < count; ++i)
            slot_of[static_cast<std::size_t>((plan.insts[i].pc - min_pc) / 4)] = static_cast<std::int32_t>(i);
        bool in_order = true;
        std::int32_t next = 0;
        for (const auto i : slot_of) {
            if (i < 0) continue;
            in_order &= i == next;
            ++next;
        }
        if (!in_order) {
            sorted.clear();
            sorted.reserve(count);
            for (const auto i : slot_of)
                if (i >= 0) sorted.push_back(plan.insts[static_cast<std::size_t>(i)]);
            plan.insts.swap(sorted);
        }
        std::int32_t k = 0;
        for (auto& i : slot_of)
            if (i >= 0) i = k++;
    } else {
        order.clear();
        for (std::uint32_t i = 0; i < count; ++i) order.emplace_back(plan.insts[i].pc, i);
        bool in_order = true;
        for (std::size_t i = 1; i < order.size() && in_order; ++i) in_order = order[i - 1].first < order[i].first;
        if (!in_order) {
            std::sort(order.begin(), order.end());
            sorted.clear();
            sorted.reserve(count);
            for (const auto& [pc, i] : order) sorted.push_back(plan.insts[i]);
            plan.insts.swap(sorted);
        }
    }
    // Inlined callee copies follow their BL (segment k = k-th copy); main
    // indices are remapped. Each copy is sorted by pc.
    thread_local std::vector<std::int32_t> remap;          // main index -> final index
    thread_local std::vector<std::uint64_t> main_pcs;      // sparse layout: main pcs (sorted)
    struct Segment {
        std::int32_t begin, count;
    };
    thread_local std::vector<Segment> segments;            // segments[k - 1]: copy k
    remap.clear();
    segments.clear();
    if (inlined_total != 0) {
        if (!dense) {
            main_pcs.clear();
            for (const auto& pi : plan.insts) main_pcs.push_back(pi.pc);
        }
        sorted.clear();
        sorted.reserve(count + inlined_total);
        remap.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            remap[i] = static_cast<std::int32_t>(sorted.size());
            sorted.push_back(plan.insts[i]);
            if (!plan.insts[i].inline_call) continue;
            const auto& body = body_list[static_cast<std::size_t>(plan.insts[i].inline_body)];
            const auto seg = static_cast<std::uint16_t>(segments.size() + 1);
            segments.push_back({static_cast<std::int32_t>(sorted.size()), static_cast<std::int32_t>(body.count)});
            for (std::uint32_t k = 0; k < body.count; ++k) {
                PlanInst copy = bodies[body.first + k];
                copy.segment = seg;
                if (copy.inst.kind == Kind::ret && copy.flow == Flow::indirect) {
                    copy.inline_ret = true;
                    copy.link_pc = plan.insts[i].link_pc;
                    copy.reads = 0;   // emits no code
                    copy.may_fault = false;
                }
                sorted.push_back(copy);
            }
            ++plan.inlined_calls;
            plan.inlined_insts += body.count;
        }
        plan.insts.swap(sorted);
    }
    const auto find_index = [&](std::uint64_t pc) -> std::int32_t {
        std::int32_t k;
        if (!dense) {
            if (remap.empty()) return plan.find(pc);
            const auto it = std::lower_bound(main_pcs.begin(), main_pcs.end(), pc);
            if (it == main_pcs.end() || *it != pc) return -1;
            k = static_cast<std::int32_t>(it - main_pcs.begin());
        } else {
            if (pc < min_pc || pc > max_pc || (pc & 3) != 0) return -1;
            k = slot_of[static_cast<std::size_t>((pc - min_pc) / 4)];
        }
        return k < 0 || remap.empty() ? k : remap[static_cast<std::size_t>(k)];
    };
    // Branch target inside segment `seg` (0: the main region).
    const auto find_in = [&](std::uint16_t seg, std::uint64_t pc) -> std::int32_t {
        if (seg == 0) return find_index(pc);
        const auto& s = segments[seg - 1u];
        std::int32_t lo = s.begin, hi = s.begin + s.count;
        while (lo < hi) {
            const auto mid = (lo + hi) / 2;
            if (plan.insts[static_cast<std::size_t>(mid)].pc < pc) lo = mid + 1;
            else hi = mid;
        }
        return lo < s.begin + s.count && plan.insts[static_cast<std::size_t>(lo)].pc == pc ? lo : -1;
    };
    const auto n = static_cast<std::int32_t>(plan.insts.size());
    plan.words.resize(plan.insts.size());
    for (std::int32_t i = 0; i < n; ++i) plan.words[i] = plan.insts[i].inst.word;
    plan.uses.resize(plan.insts.size());   // filled once leaders are known (below)

    const auto add_entry = [&](std::int32_t index) {
        if (index < 0 || plan.insts[index].entry) return;
        plan.insts[index].entry = true;
        plan.insts[index].leader = true;
        plan.insts[index].entry_slot = static_cast<std::int32_t>(plan.entries.size());
        plan.entries.push_back({index});
    };
    add_entry(find_index(guest_pc));

    // Jump tables: attach to their BR, resolve targets to instruction indices.
    for (const auto& ft : found_tables) {
        const auto br = find_index(ft.br_pc);
        if (br < 0) continue;
        auto& pi = plan.insts[static_cast<std::size_t>(br)];
        pi.table = static_cast<std::int32_t>(plan.tables.size());
        plan.tables.push_back({static_cast<std::uint32_t>(plan.table_pcs.size()), ft.count, ft.index_reg, ft.by_target});
        for (std::uint32_t k = 0; k < ft.count; ++k) {
            const auto t = found_pcs[ft.first + k];
            plan.table_pcs.push_back(t);
            plan.table_targets.push_back((t & 3) == 0 ? find_index(t) : -1);
        }
    }
    for (const auto& fs : found_shadows) {
        const auto br = find_index(fs.br_pc);
        if (br < 0) continue;
        auto& pi = plan.insts[static_cast<std::size_t>(br)];
        if (pi.table >= 0) continue;
        pi.shadow_base = fs.base;
        pi.shadow_index = fs.index;
        pi.shadow_index32 = fs.index32;
    }

    bool back_edges = false;   // dataflow below needs iteration only with back edges
    const auto add_edge = [&](std::int32_t from, std::int32_t to) {
        auto& t = plan.insts[to];
        if (t.first_pred < 0) { t.first_pred = from; t.preds = 1; }
        else if (t.first_pred != from && t.preds < 2) t.preds = 2;
        if (from >= to) {
            t.loop_header = true;
            back_edges = true;
        }
    };
    for (std::int32_t i = 0; i < n; ++i) {
        auto& pi = plan.insts[i];
        const auto target = static_cast<std::uint64_t>(pi.inst.imm);
        const auto next = (i + 1 < n && plan.insts[i + 1].pc == pi.pc + 4 && plan.insts[i + 1].segment == pi.segment)
                              ? i + 1 : -1;
        if (pi.inline_call) {
            pi.fall = i + 1;   // the callee copy (its first instruction is the target)
        } else if (pi.inline_ret) {
            // The continuation (main region): straight-line when laid out next.
            const auto c = find_index(pi.link_pc);
            if (c == i + 1) {
                pi.flow = Flow::normal;
                pi.fall = c;
            } else {
                pi.flow = Flow::jump;
                pi.taken = c;   // -1: a direct exit to link_pc
            }
        } else {
            switch (pi.flow) {
            case Flow::normal: pi.fall = next; break;
            case Flow::jump: pi.taken = find_in(pi.segment, target); break;
            case Flow::cond:
                pi.taken = find_in(pi.segment, target);
                pi.fall = next;
                break;
            case Flow::call:
            case Flow::svc:
            case Flow::interpret:
                pi.continuation = next;
                break;
            default: break;
            }
        }
        if (pi.taken >= 0) {
            plan.insts[pi.taken].leader = true;
            add_edge(i, pi.taken);
        }
        if (pi.fall >= 0) add_edge(i, pi.fall);
        if (pi.table >= 0) {
            const auto& t = plan.tables[static_cast<std::size_t>(pi.table)];
            for (std::uint32_t k = 0; k < t.count; ++k) {
                const auto s = plan.table_targets[t.first + k];
                if (s < 0) continue;
                plan.insts[static_cast<std::size_t>(s)].leader = true;
                add_edge(i, s);
            }
        }
    }
    for (std::int32_t i = 0; i < n; ++i) {
        if (plan.insts[i].continuation >= 0) add_entry(plan.insts[i].continuation);
        if (i > 0 && plan.insts[i - 1].fall != i) plan.insts[i].leader = true;
    }
    if (n > 0) plan.insts[0].leader = true;
    for (std::int32_t i = 0; i < n; ++i) {
        auto& pi = plan.insts[i];
        // A block whose only predecessor precedes it in layout inherits the
        // predecessor's exact flags state (no materialization on the edge).
        pi.inherit_flags = pi.leader && !pi.entry && pi.preds == 1 && pi.first_pred < i;
        if (pi.leader) ++plan.blocks;
        const auto bits = static_cast<std::uint8_t>(
            (pi.leader ? kUseLeader : 0) | (pi.flow == Flow::interpret || pi.conservative_writes ? kUseOpaque : 0) |
            (pi.flow == Flow::normal || pi.flow == Flow::cond ? kUseStraight : 0) |
            (pi.flow == Flow::normal ? kUseNormal : 0));
        plan.uses[static_cast<std::size_t>(i)] = {pi.reads, pi.writes, pi.vreads, pi.vwrites, bits};
    }

    // Flags liveness (backward). Exits treat flags as live, except calls and
    // returns: AAPCS64 leaves NZCV undefined on entry to and return from a
    // public interface, so compiled code never passes flags through BL/BLR or
    // RET (TranslateOptions::abi_flags; PlanOverrides::exact_flags restores
    // the strict model).
    // (Fields start false/0 from construction. Without back edges every
    // successor follows in layout, so one pass in the right direction is the
    // fixed point.)
    // TRANSMUTE_FX_CORE=abiflags (opt-in measurement knob, core_isel.hpp) turns
    // the AAPCS64 model on without TranslateOptions.
    const bool strict_flags = overrides.exact_flags ||
                              !(options.abi_flags || (core::codegen_p2_knob_bits() & core::knob_abi_flags) != 0);
    // TRANSMUTE_FX_CORE=brdeadflags (opt-in, core_isel.hpp): a BR that does
    // not look intra-function is flag-dead too. Tail calls, veneers and
    // interpreter dispatch enter code that cannot assume incoming flags; a
    // recognised switch table or an ADR/ADRP in the BR's block (clang switch
    // tables, computed gotos) keeps them live.
    const bool br_flags_dead = !strict_flags && (core::codegen_p2_knob_bits() & core::knob_br_flags_dead) != 0;
    const auto br_live = [&](std::int32_t i) {
        if (!br_flags_dead || plan.insts[i].table >= 0) return true;
        for (std::int32_t j = i; j >= 0; --j) {
            if (plan.insts[j].inst.kind == Kind::adr) return true;
            if (plan.insts[j].leader) break;
        }
        return false;
    };
    // An exit to a known target that writes NZCV before reading it
    // (target_flags_dead: p1's scan of image code) is flag-dead, also under
    // exact flags. Each target is asked once (the loop revisits exits).
    // TRANSMUTE_FX_CORE=nodeadtargets (core_isel.hpp) keeps every such exit live.
    const bool dead_targets = (core::codegen_p2_knob_bits() & core::knob_no_dead_targets) == 0;
    std::vector<std::pair<std::uint64_t, bool>> dead_memo;
    const auto exit_live = [&](std::uint64_t target) {
        if (!dead_targets) return true;
        for (const auto& [pc, dead] : dead_memo)
            if (pc == target) return !dead;
        const bool dead = target_flags_dead(code, target, plan);
        dead_memo.emplace_back(target, dead);
        return !dead;
    };
    const auto direct = [](const PlanInst& pi) {
        const auto k = pi.inst.kind;
        return k == Kind::branch || k == Kind::branch_cond || k == Kind::compare_branch || k == Kind::test_branch;
    };
    // The out-of-region side of a jump, conditional branch or region cut.
    const auto taken_exit = [&](const PlanInst& pi) {
        return direct(pi) ? exit_live(static_cast<std::uint64_t>(pi.inst.imm)) : true;
    };
    const auto fall_exit = [&](const PlanInst& pi) { return exit_live(pi.pc + 4); };
    for (bool changed = true; changed; changed = changed && back_edges) {
        changed = false;
        for (std::int32_t i = n - 1; i >= 0; --i) {
            auto& pi = plan.insts[i];
            bool out = false;
            switch (pi.flow) {
            case Flow::normal: out = pi.fall < 0 ? fall_exit(pi) : plan.insts[pi.fall].flags_live_in; break;
            case Flow::jump: out = pi.taken < 0 ? taken_exit(pi) : plan.insts[pi.taken].flags_live_in; break;
            case Flow::cond:
                out = (pi.taken < 0 ? taken_exit(pi) : plan.insts[pi.taken].flags_live_in) ||
                      (pi.fall < 0 ? fall_exit(pi) : plan.insts[pi.fall].flags_live_in);
                break;
            case Flow::call:
                out = strict_flags && (pi.inst.kind != Kind::branch_link || exit_live(static_cast<std::uint64_t>(pi.inst.imm)));
                break;
            case Flow::indirect: out = strict_flags || (pi.inst.kind != Kind::ret && br_live(i)); break;
            default: out = true; break;
            }
            const bool in = pi.reads_flags || (out && !pi.writes_flags);
            if (in != pi.flags_live_in || out != pi.flags_live_out) {
                pi.flags_live_in = in;
                pi.flags_live_out = out;
                changed = true;
            }
        }
    }

    // Possibly-dirty guest registers (forward). Entries start clean.
    for (bool changed = true; changed; changed = changed && back_edges) {
        changed = false;
        for (std::int32_t i = 0; i < n; ++i) {
            const auto& pi = plan.insts[i];
            const auto out = pi.dirty_in | pi.writes;
            const auto vout = pi.vdirty_in | pi.vwrites;
            plan.for_each_successor(i, [&](std::int32_t s) {
                auto& t = plan.insts[static_cast<std::size_t>(s)];
                if ((t.dirty_in | out) != t.dirty_in || (t.vdirty_in | vout) != t.vdirty_in) {
                    t.dirty_in |= out;
                    t.vdirty_in |= vout;
                    changed = true;
                }
            });
        }
    }

    // Region-static pinning: hottest guest registers (loop-weighted).
    std::fill(std::begin(plan.pin_host), std::end(plan.pin_host), static_cast<std::int8_t>(-1));
    std::fill(std::begin(plan.vpin_host), std::end(plan.vpin_host), static_cast<std::int8_t>(-1));
    plan.pinned = 0;
    plan.pin_hosts = 0;
    plan.extra_pins = false;
    plan.vpinned = 0;
    plan.vpin_hosts = 0;
    const auto& conv = translator_register_convention();
    plan.globals = 0;
    plan.evicted = conv.guest_mask;   // without pins: every convention register lives in the context
    if (!overrides.no_pins && n > 0) {
        bool family_free = true;
        if (overrides.family_pins7)
            for (const auto& pi : plan.insts) family_free &= pi.family < 0 && !pi.try_family;
        const unsigned max_pins = !overrides.no_extra_pins && family_free ? std::size(kPinHosts) : kCalleeSavedPins;
        depth.assign(static_cast<std::size_t>(n) + 1, 0);
        for (std::int32_t i = 0; i < n; ++i) {
            plan.for_each_successor(i, [&](std::int32_t s) {
                if (s <= i) { ++depth[static_cast<std::size_t>(s)]; --depth[static_cast<std::size_t>(i) + 1]; }
            });
        }
        // benefit: loop-weighted uses. cost: a load at every entry and a store
        // at every exit where the register may be dirty. Unconditional exits
        // inside loops (calls, returns) are weighted like their instruction;
        // conditional exits are assumed rare.
        std::int32_t benefit[32]{};
        std::int32_t vbenefit[32]{};
        std::int32_t level = 0;
        // The register cache serves repeated uses within a block, so only a
        // block's first read and first write of a register count fully;
        // other uses count a quarter (evictions under pressure).
        // PlanOverrides::old_pins: every use counts fully.
        std::uint32_t seen = 0, wseen = 0, vseen = 0, vwseen = 0;
        const auto add_to = [](std::int32_t* b, std::uint32_t mask, std::int32_t w) {
            while (mask) {
                const auto r = static_cast<unsigned>(std::countr_zero(mask));
                mask &= mask - 1;
                b[r] += w;
            }
        };
        const auto add = [&](std::uint32_t mask, std::int32_t w) { add_to(benefit, mask, w); };
        const auto vadd = [&](std::uint32_t mask, std::int32_t w) { add_to(vbenefit, mask, w); };
        // Entries charge every register alike, and exit masks (all registers
        // possibly dirty there) are dense: sum entries as a scalar and count
        // exit masks per weight in bit-sliced counters (O(1) per mask), then
        // fold both into the benefits after the walk. Same totals as adding
        // each mask's bits one by one.
        struct BitCounter {
            std::uint32_t plane[24]{};
            unsigned planes = 0;
            void add(std::uint32_t m) {
                for (unsigned k = 0; m; ++k) {
                    const auto carry = plane[k] & m;
                    plane[k] ^= m;
                    m = carry;
                    if (k >= planes) planes = k + 1;
                }
            }
            void fold(std::int32_t* b, std::int32_t w) const {
                for (unsigned k = 0; k < planes; ++k) add_bits(b, plane[k], w * (std::int32_t{1} << k));
            }
            static void add_bits(std::int32_t* b, std::uint32_t mask, std::int32_t w) {
                while (mask) {
                    b[std::countr_zero(mask)] += w;
                    mask &= mask - 1;
                }
            }
        };
        BitCounter exits[6], vexits[6];   // weight lw[level] (level 0..4), then weight 1 (conditional exits)
        // Loop weights: 1 + 8 * level; tier 2: 8^level (an inner loop runs
        // about eight times per entry, so its uses outweigh the enclosing
        // loop's, e.g. evicting a convention register used only around it).
        static constexpr std::int32_t kLinear[5] = {1, 9, 17, 25, 33}, kExp[5] = {1, 8, 64, 512, 4096};
        const std::int32_t* lw = plan.optimize ? kExp : kLinear;
        std::int32_t entry_sum = 0;
        std::int32_t exit_sum = 0;        // all exits, weighted (reloads of evicted convention registers)
        for (std::int32_t i = 0; i < n; ++i) {
            level += depth[i];
            const auto& pi = plan.insts[i];
            const auto clamped = std::min(level, 4);
            const auto w = lw[clamped];
            if (pi.entry) entry_sum += pi.entry_slot == 0 ? -1 : -w;   // the region start runs once per entry
            const auto dirty_out = pi.dirty_in | pi.writes;
            const auto vdirty_out = pi.vdirty_in | pi.vwrites;
            int exit_class = -1;   // exits[] index (weight -w, or -1 for class 5)
            switch (pi.flow) {
            case Flow::indirect:
                if (pi.table >= 0) break;   // switch dispatch: stays in the region (the fallback is rare)
                exit_class = clamped;
                break;
            case Flow::call:
            case Flow::trap:
            case Flow::svc:
            case Flow::interpret: exit_class = clamped; break;
            case Flow::jump: if (pi.taken < 0) exit_class = clamped; break;
            case Flow::normal: if (pi.fall < 0) exit_class = clamped; break;
            case Flow::cond: if (pi.taken < 0 || pi.fall < 0) exit_class = 5; break;
            }
            if (exit_class >= 0) {
                exits[exit_class].add(dirty_out);
                vexits[exit_class].add(vdirty_out);
                exit_sum += exit_class == 5 ? 1 : lw[exit_class];
            }
            if (pi.leader) seen = wseen = vseen = vwseen = 0;
            if (pi.flow == Flow::interpret || pi.conservative_writes) continue;
            if (!overrides.old_pins) {
                const auto first = (pi.reads & ~seen) | (pi.writes & ~wseen);
                const auto vfirst = (pi.vreads & ~vseen) | (pi.vwrites & ~vwseen);
                const auto rest_w = std::max<std::int32_t>(1, w / 4);
                add(first, w);
                add((pi.reads | pi.writes) & ~first, rest_w);
                vadd(vfirst, w);
                vadd((pi.vreads | pi.vwrites) & ~vfirst, rest_w);
                seen |= pi.reads | pi.writes;
                wseen |= pi.writes;
                vseen |= pi.vreads | pi.vwrites;
                vwseen |= pi.vwrites;
            } else {
                add(pi.reads | pi.writes, w);
                vadd(pi.vreads | pi.vwrites, w);
            }
        }
        std::int32_t use_benefit[32];   // uses only (convention registers pay nothing at entries and exits)
        std::copy(std::begin(benefit), std::end(benefit), std::begin(use_benefit));
        for (int c = 0; c < 6; ++c) {
            const std::int32_t cw = c == 5 ? -1 : -lw[c];
            exits[c].fold(benefit, cw);
            vexits[c].fold(vbenefit, cw);
        }
        for (unsigned r = 0; r < 32; ++r) {
            benefit[r] += entry_sum;
            vbenefit[r] += entry_sum;
        }
        std::int32_t weight[32];
        for (unsigned r = 0; r < 32; ++r) weight[r] = benefit[r];
        unsigned rank[32];
        for (unsigned r = 0; r < 32; ++r) {   // insertion sort by weight (stable, no allocation)
            unsigned j = r;
            while (j > 0 && weight[rank[j - 1]] < weight[r]) {
                rank[j] = rank[j - 1];
                --j;
            }
            rank[j] = r;
        }
        if (conv.guest_mask == 0) {
            unsigned used = 0;
            bool sp_pinned = false;
            for (unsigned k = 0; k < 32 && used < max_pins; ++k) {
                const auto r = rank[k];
                if (weight[r] < 1) break;
                if (r == 18) continue;   // platform register (TEB): rarely hot, keep in context
                plan.pinned |= 1u << r;
                ++used;
                if (r == 31) sp_pinned = true;
            }
            // SP takes rbx when pinned (most common base register).
            unsigned next = 0;
            if (sp_pinned) { plan.pin_host[31] = static_cast<std::int8_t>(kPinHosts[0]); next = 1; }
            for (unsigned k = 0; k < 32; ++k) {
                const auto r = rank[k];
                if (!(plan.pinned & (1u << r)) || r == 31) continue;
                plan.pin_host[r] = static_cast<std::int8_t>(kPinHosts[next++]);
            }
        } else {
            // Cross-region convention: its registers stay pinned in their
            // hosts (no entry loads, no exit stores). Other registers take
            // the remaining callee-saved hosts, then r11/r10, then evict the
            // convention register cheapest to give up: its uses plus a store
            // at every entry and a reload at every exit.
            std::uint32_t kept = conv.guest_mask, host_used = 0;
            for (unsigned g = 0; g < 32; ++g)
                if ((kept >> g) & 1u) {
                    plan.pin_host[g] = conv.host[g];
                    host_used |= 1u << static_cast<unsigned>(conv.host[g]);
                }
            // (Scaling this cost by 4 or 16 measured no different on pybench.)
            const std::int32_t evict_cost = -entry_sum + exit_sum;
            // Diagnostics: TRANSMUTE_FX_TRACE_PINS=<hex region start> prints the weights.
            static const std::uint64_t trace_pc = [] {
                const char* v = std::getenv("TRANSMUTE_FX_TRACE_PINS");
                return v ? std::strtoull(v, nullptr, 16) : ~std::uint64_t{0};
            }();
            if (trace_pc == guest_pc || trace_pc == 1) {
                std::fprintf(stderr, "pins %llx: entries %d exits %d evict %d;", static_cast<unsigned long long>(guest_pc),
                             entry_sum, exit_sum, evict_cost);
                for (unsigned k = 0; k < 32; ++k)
                    if (weight[rank[k]] > 0 || ((conv.guest_mask >> rank[k]) & 1u))
                        std::fprintf(stderr, " x%u=%d/%d", rank[k], weight[rank[k]], use_benefit[rank[k]]);
                std::fprintf(stderr, "\n");
            }
            for (unsigned k = 0; k < 32; ++k) {
                const auto r = rank[k];
                if (weight[r] < 1) break;
                if (r == 18 || ((conv.guest_mask >> r) & 1u)) continue;
                int host = -1;
                for (unsigned i = 0; i < max_pins && host < 0; ++i)
                    if (!((host_used >> kPinHosts[i]) & 1u)) host = kPinHosts[i];
                if (host < 0) {
                    int victim = -1;
                    std::int32_t loss = 0;
                    for (auto m = kept; m; m &= m - 1) {
                        const auto g = static_cast<unsigned>(std::countr_zero(m));
                        const auto l = std::max<std::int32_t>(0, use_benefit[g]) + evict_cost;
                        if (victim < 0 || l < loss) { victim = static_cast<int>(g); loss = l; }
                    }
                    if (victim < 0 || weight[r] <= loss) break;
                    host = plan.pin_host[victim];
                    plan.pin_host[victim] = -1;
                    kept &= ~(1u << victim);
                }
                plan.pin_host[r] = static_cast<std::int8_t>(host);
                host_used |= 1u << static_cast<unsigned>(host);
            }
            plan.globals = kept;
            plan.evicted = conv.guest_mask & ~kept;
            for (unsigned r = 0; r < 32; ++r)
                if (plan.pin_host[r] >= 0) plan.pinned |= 1u << r;
        }
        for (unsigned r = 0; r < 32; ++r)
            if (plan.pin_host[r] >= 0) plan.pin_hosts |= 1u << plan.pin_host[r];
        plan.extra_pins = (plan.pin_hosts & ((1u << 10) | (1u << 11))) != 0;

        // Vector pins in host callee-saved xmm6..xmm15 (they survive helper and host calls).
        unsigned vorder[32];
        for (unsigned r = 0; r < 32; ++r) {
            unsigned j = r;
            while (j > 0 && vbenefit[vorder[j - 1]] < vbenefit[r]) {
                vorder[j] = vorder[j - 1];
                --j;
            }
            vorder[j] = r;
        }
        const unsigned vlimit = std::min(overrides.vector_pin_limit, 10u);
        for (unsigned k = 0, used_v = 0; k < 32 && used_v < vlimit; ++k) {
            const auto r = vorder[k];
            if (vbenefit[r] < 1) break;
            plan.vpinned |= 1u << r;
            plan.vpin_host[r] = static_cast<std::int8_t>(15 - used_v);   // xmm15 downwards
            plan.vpin_hosts |= 1u << (15 - used_v);
            ++used_v;
        }
    }
}

} // namespace core
} // namespace transmute::fx
