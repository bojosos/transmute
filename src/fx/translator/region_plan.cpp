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
        mix(0xfc);   // code layout knobs (all 32 bits)
        for (unsigned b = 0; b < 32; b += 8) mix(static_cast<unsigned char>(layout >> b));
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
constexpr unsigned kTableScan = 16;        // instructions scanned back from the BR
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
    bool straight_bound; // immutable image code + ADR/ADRP table/base + straight-line bound
    bool need_zext32;    // 64-bit table index load guarded by 32-bit CMP: require ext_zero32
    std::uint64_t bound_pc;
    const char* fail;    // diagnostics: the step that did not match
};

// Decodes the straight-line instruction at `pc` (false: unreadable or not core).
// Treats NOP and BTI hint words as Kind::nop so Apple ld64-relaxed ADR+NOP
// jump tables and BTI landing pads do not break table/bound/flag scans.
bool decode_at(const CodeSource& code, std::uint64_t pc, Inst& out) {
    std::uint32_t word = 0;
    if (!code.read(pc, word)) return false;
    decode(word, pc, out);
    if (out.kind == Kind::unknown && (word == 0xd503201fu || (word & 0xffffff3fu) == 0xd503241fu))
        out.kind = Kind::nop;
    return out.kind != Kind::unknown;
}

// Straight-line instruction at `pc` for table/bound/shadow scans: core
// instructions (including NOP/BTI), or a family-described non-branch
// instruction (FP/SIMD/atomic) with its GPR write mask and flag effect.
bool decode_scan_at(const CodeSource& code, std::uint64_t pc, Inst& out, std::uint32_t& fam_write,
                    bool& fam_writes_flags) {
    fam_write = 0;
    fam_writes_flags = false;
    std::uint32_t word = 0;
    if (!code.read(pc, word)) return false;
    decode(word, pc, out);
    if (out.kind == Kind::unknown && (word == 0xd503201fu || (word & 0xffffff3fu) == 0xd503241fu))
        out.kind = Kind::nop;
    if (out.kind != Kind::unknown) return true;
    const auto families = registered_families();
    for (unsigned f = 0; f < families.count; ++f) {
        if (!families.families[f].describe) continue;
        InstructionUse u{};
        if (families.families[f].describe(word, u)) {
            fam_write = u.gpr_write;
            fam_writes_flags = u.writes_flags;
            return true;
        }
    }
    return false;
}

bool writes_reg(const Inst& d, unsigned r) { return ((usage(d).write >> r) & 1u) != 0; }
bool writes_scan(const Inst& d, std::uint32_t fam_write, unsigned r) {
    return d.kind == Kind::unknown ? (((fam_write >> r) & 1u) != 0) : writes_reg(d, r);
}
bool is_branch(const Inst& d) { return d.kind != Kind::unknown && flow_of(d) != Flow::normal; }

// Value of an ADR / ADRP+ADD writing xR at `j` (insts[lo..j]), else false.
bool adr_value(const Inst* insts, const std::uint32_t* fam_writes, int lo, int j, unsigned r, std::uint64_t& value) {
    const auto& d = insts[j];
    if (d.kind == Kind::adr && d.rd == r) {
        value = static_cast<std::uint64_t>(d.imm);
        return true;
    }
    if (d.kind == Kind::add_sub_imm && d.op == 0 && !d.s && d.sf && d.rd == r && d.rn != 31) {
        for (int k = j - 1; k >= lo; --k) {
            if (!writes_scan(insts[k], fam_writes ? fam_writes[k] : 0u, d.rn)) continue;
            if (insts[k].kind == Kind::adr && insts[k].rd == d.rn) {
                value = static_cast<std::uint64_t>(insts[k].imm + d.imm);
                return true;
            }
            return false;
        }
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
unsigned scan_bound(const CodeSource& code, std::uint64_t ld_pc, unsigned limit, unsigned ri,
                    std::uint64_t* bound_pc = nullptr, bool* bound_64 = nullptr) {
    std::uint64_t pc = ld_pc;
    unsigned count = 0;
    for (unsigned k = 0; k < limit && count == 0; ++k) {
        pc -= 4;
        Inst d;
        std::uint32_t fw = 0;
        bool fwf = false;
        if (!decode_scan_at(code, pc, d, fw, fwf)) break;
        if (d.kind == Kind::unknown) {
            if ((fw >> ri) & 1u) break;
            continue;
        }
        if (d.kind == Kind::branch_cond && (d.cond == 8 || d.cond == 2)) {   // b.hi / b.hs past the default
            for (unsigned c = 1; c <= 6; ++c) {
                Inst f;
                std::uint32_t ffw = 0;
                bool ffwf = false;
                if (!decode_scan_at(code, pc - 4u * c, f, ffw, ffwf)) break;
                if (f.kind == Kind::unknown) {
                    if (ffwf || ((ffw >> ri) & 1u)) break;
                    continue;
                }
                if (is_branch(f)) break;
                const auto u = usage(f);
                if (f.kind == Kind::add_sub_imm && f.op == 1 && f.s && f.rd == 31 && f.rn == ri) {   // cmp wI/xI, #imm
                    const auto imm = static_cast<std::uint64_t>(f.imm);
                    count = static_cast<unsigned>(std::min<std::uint64_t>(d.cond == 8 ? imm + 1 : imm, kMaxTableEntries + 1));
                    if (bound_pc) *bound_pc = pc - 4u * c;
                    if (bound_64) *bound_64 = f.sf;
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
            if (count != 0) {
                if (bound_pc) *bound_pc = pc;
                if (bound_64) *bound_64 = true;   // UBFX/AND/LDRB all clear bits 63..32
            }
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
bool match_label_table(const Inst* insts, const std::uint32_t* fam_writes, int lo, int ld, std::uint64_t br_pc,
                       const CodeSource& code, TableMatch& m) {
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
        if (writes_scan(insts[j], fam_writes ? fam_writes[j] : 0u, rt)) tdef = j;
    const auto window_pc = br_pc - 4u * (kTableScan - static_cast<unsigned>(lo));
    if (tdef >= 0 ? !adr_value(insts, fam_writes, lo, tdef, rt, m.table) : !far_adr_value(code, window_pc, rt, m.table)) {
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
// BR is either:
//  (a) `ldr xN, [xB, xI, lsl/uxtw #3]` with xB and xI unchanged after the load, or
//  (b) stride-2^s pointer-computed dispatch (V8 15 JSDispatchTable):
//      `[lsr xI, xH, #r]; [lsl xO, xI, #s]; add xP, xB, xI/xO{, lsl #s}; ldr xN, [xP]`
//      possibly through register copies (`mov x17, xN; br x17`), including
//      in-place `xP == xB` (base recovered via `xP - (index << s)`) and
//      overwritten `xI` (index recovered via `xH >> r`).
// Any table: a label table takes the BR first (match_table), and the target is
// compared either way. Returns nullptr, or why not (TRANSMUTE_FX_TRACE_TABLES).
const char* match_shadow_load(std::uint64_t br_pc, unsigned rn, const CodeSource& code, std::int8_t& base,
                              std::int8_t& index, bool& index32, std::uint8_t& rshift, std::uint8_t& sub_stride) {
    rshift = 0;
    sub_stride = 0;
    if (rn >= 31 || br_pc < 4 * kTableScan) return "no register";
    constexpr unsigned kMaxCopies = 2;
    unsigned want = rn, copies = 0;
    std::uint32_t written = 0;   // registers written between the load and the BR
    for (unsigned back = 1; back <= kTableScan; ++back) {
        Inst d;
        std::uint32_t fw = 0;
        bool fwf = false;
        if (!decode_scan_at(code, br_pc - 4u * back, d, fw, fwf) || is_branch(d)) return "no writer of the target in the block";
        if (d.kind == Kind::unknown) {
            if ((fw >> want) & 1u) return "target written by non-core instruction";
            written |= fw;
            continue;
        }
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
        if (d.kind == Kind::load_store && d.mode == AddrMode::reg && d.mem == MemOp::load && d.rd == want) {
            if (d.size != 3 || d.amount != 3 || (d.shift != 2 && d.shift != 3)) return "not an 8-byte load scaled by lsl/uxtw #3";
            if (d.rn == 31 || d.rm == 31 || d.rn == d.rd || d.rm == d.rd) return "base or index is sp, zr or the load's target";
            if (((written >> d.rn) & 1u) != 0 || ((written >> d.rm) & 1u) != 0) return "base or index written after the load";
            base = static_cast<std::int8_t>(d.rn);
            index = static_cast<std::int8_t>(d.rm);
            index32 = d.shift == 2;
            return nullptr;
        }
        // Stride-2^s pointer-computed load: `ldr xT, [xP]` where xP = xB + (xI << s).
        if (d.kind != Kind::load_store || d.mode != AddrMode::offset || d.mem != MemOp::load || d.size != 3 ||
            d.imm != 0 || d.rd != want || d.rn >= 31 || d.rn == want)
            return "the target's writer is not a register-offset load";
        const unsigned ptr_reg = d.rn;
        if (((written >> ptr_reg) & 1u) != 0) return "pointer written after the load";
        std::uint32_t written_before_add = written;
        for (unsigned back2 = back + 1; back2 <= kTableScan; ++back2) {
            Inst a;
            std::uint32_t afw = 0;
            bool afwf = false;
            if (!decode_scan_at(code, br_pc - 4u * back2, a, afw, afwf) || is_branch(a)) return "no pointer add for stride table";
            if (a.kind == Kind::unknown) {
                if ((afw >> ptr_reg) & 1u) return "pointer written by non-core instruction";
                written_before_add |= afw;
                continue;
            }
            if (!writes_reg(a, ptr_reg)) {
                written_before_add |= usage(a).write;
                continue;
            }
            if (a.kind != Kind::add_sub_shift || a.op != 0 || a.s || !a.sf || a.shift != 0 || a.rd != ptr_reg ||
                a.rn >= 31 || a.rm >= 31)
                return "pointer writer is not ADD";
            unsigned breg = a.rn, ireg = a.rm, stride = a.amount;
            unsigned scan_from = back2 + 1;
            std::uint32_t written_idx = written_before_add | usage(a).write;
            if (stride == 0) {
                // Separate `lsl xO, xI, #s` (UBFM) before `add xP, xB, xO`: check a.rm then a.rn.
                bool found_lsl = false;
                for (unsigned try_rm = 0; try_rm < 2 && !found_lsl; ++try_rm) {
                    const unsigned cand_o = try_rm == 0 ? a.rm : a.rn;
                    const unsigned cand_b = try_rm == 0 ? a.rn : a.rm;
                    std::uint32_t wtmp = written_idx;
                    for (unsigned b3 = back2 + 1; b3 <= kTableScan; ++b3) {
                        Inst sh;
                        std::uint32_t sfw = 0;
                        bool sfwf = false;
                        if (!decode_scan_at(code, br_pc - 4u * b3, sh, sfw, sfwf) || is_branch(sh)) break;
                        if (sh.kind == Kind::unknown) {
                            if ((sfw >> cand_o) & 1u) break;
                            wtmp |= sfw;
                            continue;
                        }
                        if (!writes_reg(sh, cand_o)) {
                            wtmp |= usage(sh).write;
                            continue;
                        }
                        if (sh.kind == Kind::bitfield && sh.op == 2 && sh.sf && sh.rd == cand_o && sh.rn < 31 &&
                            sh.imms + 1 == sh.amount && sh.amount >= 59 && sh.amount <= 61) {
                            breg = cand_b;
                            ireg = sh.rn;
                            stride = 64u - sh.amount;
                            scan_from = b3 + 1;
                            written_idx = wtmp | usage(sh).write;
                            found_lsl = true;
                        }
                        break;
                    }
                }
                if (!found_lsl) return "pointer ADD has no stride shift";
            } else if (stride < 3 || stride > 5) {
                return "pointer ADD stride outside 8..32";
            }
            if (breg == ptr_reg) {
                base = static_cast<std::int8_t>(ptr_reg);
                sub_stride = static_cast<std::uint8_t>(stride);
            } else if (((written_before_add >> breg) & 1u) == 0) {
                base = static_cast<std::int8_t>(breg);
                sub_stride = 0;
            } else {
                return "stride table base written after ADD";
            }
            if (ireg != ptr_reg && ((written_before_add >> ireg) & 1u) == 0) {
                index = static_cast<std::int8_t>(ireg);
                index32 = false;
                rshift = 0;
                return nullptr;
            }
            // Index register was overwritten after the ADD (e.g. reused by the load):
            // recover it from an immediately preceding `lsr xI/wI, xH/wH, #k`.
            std::uint32_t wh = written_idx;
            for (unsigned b4 = scan_from; b4 <= kTableScan; ++b4) {
                Inst ls;
                std::uint32_t lfw = 0;
                bool lfwf = false;
                if (!decode_scan_at(code, br_pc - 4u * b4, ls, lfw, lfwf) || is_branch(ls)) break;
                if (ls.kind == Kind::unknown) {
                    if ((lfw >> ireg) & 1u) break;
                    wh |= lfw;
                    continue;
                }
                if (!writes_reg(ls, ireg)) {
                    wh |= usage(ls).write;
                    continue;
                }
                const bool lsr64 = ls.kind == Kind::bitfield && ls.op == 2 && ls.sf && ls.imms == 63 &&
                                   ls.amount > 0 && ls.amount < 32 && ls.rd == ireg && ls.rn < 31;
                const bool lsr32 = ls.kind == Kind::bitfield && ls.op == 2 && !ls.sf && ls.imms == 31 &&
                                   ls.amount > 0 && ls.amount < 32 && ls.rd == ireg && ls.rn < 31;
                if ((lsr64 || lsr32) && ((wh >> ls.rn) & 1u) == 0) {
                    index = static_cast<std::int8_t>(ls.rn);
                    index32 = lsr32;
                    rshift = static_cast<std::uint8_t>(ls.amount);
                    return nullptr;
                }
                break;
            }
            return "stride table index not live at BR";
        }
        return "no pointer add in scan";
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
    std::uint32_t fam_writes[kTableScan]{};
    int lo = static_cast<int>(kTableScan);
    for (int j = static_cast<int>(kTableScan) - 1; j >= 0; --j) {
        const auto pc = br_pc - 4u * (kTableScan - static_cast<unsigned>(j));
        bool fwf = false;
        if (!decode_scan_at(code, pc, insts[j], fam_writes[j], fwf) || is_branch(insts[j])) break;
        lo = j;
    }
    // The writer of xN: add xN, xB, xO, lsl #s.
    int add = -1;
    for (int j = static_cast<int>(kTableScan) - 1; j >= lo && add < 0; --j)
        if (writes_scan(insts[j], fam_writes[j], rn)) add = j;
    if (add < 0) return no("no writer of xN");
    const auto& a = insts[add];
    if (a.kind != Kind::add_sub_shift || a.op != 0 || a.s || !a.sf || a.shift != 0 || a.amount > 4 || a.rn == 31 ||
        a.rm == 31 || a.rd != rn) {
        if (a.kind == Kind::load_store && a.mode == AddrMode::reg && a.size == 3 && a.rd == rn)
            return match_label_table(insts, fam_writes, lo, add, br_pc, code, m);
        return no("xN not ADD xB, xO, LSL");
    }
    unsigned rb = a.rn, ro = a.rm;
    // The writer of xO: the table load. With unshifted ADD (`amount == 0`),
    // base and offset operands are commutative, so check both orders.
    const auto find_table_load = [&](unsigned reg) {
        for (int j = add - 1; j >= lo; --j) {
            if (!writes_scan(insts[j], fam_writes[j], reg)) continue;
            const auto& cand = insts[j];
            const bool ext_ok = cand.shift == 2 || cand.shift == 3 || cand.shift == 6 || cand.shift == 7;
            if (cand.kind == Kind::load_store && cand.mode == AddrMode::reg && cand.rd == reg &&
                cand.rn != 31 && cand.rm != 31 && ext_ok && cand.size <= 2 &&
                (cand.amount == cand.size || (cand.amount == 0 && cand.size == 0)) &&
                (cand.mem == MemOp::load || cand.mem == MemOp::load_signed64 || cand.mem == MemOp::load_signed32))
                return j;
            return -1;
        }
        return -1;
    };
    int ld = find_table_load(ro);
    if (ld < 0 && a.amount == 0) {
        if (const int alt = find_table_load(rb); alt >= 0) {
            std::swap(rb, ro);
            ld = alt;
        }
    }
    if (ld < 0) return no("xO not a register-offset load");
    const auto& l = insts[ld];
    const unsigned rt = l.rn, ri = l.rm;
    // Indexed dispatch works for both UXTW (2) and LSL (3) when the index
    // register is unmodified until the BR: e_table_branch checks wI < count AND
    // xN == target_k, so any non-zero upper 32 bits in xI safely take the exact
    // fallback. This keeps wide-span Clang/LLVM switches compact and in-region.
    bool by_target = l.shift != 2 && l.shift != 3;
    for (int j = ld + 1; j < static_cast<int>(kTableScan); ++j)
        if (writes_scan(insts[j], fam_writes[j], ri)) by_target = true;
    // The table address: ADR xT (or ADRP+ADD) before the load, xT unchanged in
    // between; or, with no writer in the window, set up further back.
    int tdef = -1;
    for (int j = ld - 1; j >= lo && tdef < 0; --j)
        if (writes_scan(insts[j], fam_writes[j], rt)) tdef = j;
    const auto window_pc = br_pc - 4u * (kTableScan - static_cast<unsigned>(lo));   // insts[lo]
    if (tdef >= 0) {
        if (!adr_value(insts, fam_writes, lo, tdef, rt, m.table)) return no("table: not ADR/ADRP+ADD");
    } else {
        if (!far_adr_value(code, window_pc, rt, m.table)) return no("table: no ADR/ADRP+ADD found");
        by_target = true;
    }
    // The base: xB's writer before the add (likewise).
    int bdef = -1;
    for (int j = add - 1; j >= lo && bdef < 0; --j)
        if (writes_scan(insts[j], fam_writes[j], rb)) bdef = j;
    if (bdef >= 0) {
        if (!adr_value(insts, fam_writes, lo, bdef, rb, m.base)) return no("base: not ADR/ADRP+ADD");
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
    std::uint64_t bpc = 0;
    bool b64 = false;
    m.count = scan_bound(code, br_pc - 4u * (kTableScan - static_cast<unsigned>(ld)),
                         kTableBoundScan + static_cast<unsigned>(ld - first), ri, &bpc, &b64);
    // Straight-line bound proof: immutable image code, table and base loaded in
    // this block, bound check found with no intervening branch/write to index/table/base.
    if (m.count > 0 && m.count <= kMaxTableEntries && bpc != 0 && tdef >= 0 && bdef >= 0 && code.image_code(br_pc)) {
        const auto first_pc = br_pc - 4u * (kTableScan - static_cast<unsigned>(first));
        m.bound_pc = std::min(bpc, first_pc);
        m.straight_bound = true;
        m.need_zext32 = !b64 && (l.shift == 3 || l.shift == 7);
    }
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

// A bounded DAG scan of the target's first kFlagScan instructions (in image
// code only, CodeSource::image_code), following unconditional B veneers and
// flag-free CBZ/CBNZ/TBZ/TBNZ splits: returns true only if EVERY reachable
// path writes NZCV before reading it, leaving image_code, hitting any other
// branch/call/ret/trap/undecodable word, cycling, or exhausting the budget.
// Non-core instructions accepted by a registered family (e.g. prologue
// STP D8, D9 or FCMP) are inspected via FamilyEmitter::describe. Every scanned
// slice becomes a dependency of the plan (Plan::dependency_ranges), so a write
// there kills the region. A fault or interrupt before the write sees ctx->nzcv
// stale (docs/FX_ENGINE.md).
bool target_flags_dead(const CodeSource& code, std::uint64_t target_pc, Plan& plan) {
    constexpr unsigned kFlagScan = 32;
    constexpr unsigned kMaxWork = 8;
    constexpr unsigned kMaxSegs = 16;
    if ((target_pc & 3) != 0) return false;
    const auto families = registered_families();
    thread_local DescribeMemo memo[kDescribeMemoSlots];
    const auto memo_tag = static_cast<std::uint16_t>(families.count + 1);

    std::pair<std::uint64_t, std::uint64_t> segs[kMaxSegs];
    unsigned nsegs = 0;
    const auto add_seg = [&](std::uint64_t s, std::uint64_t e) {
        if (s >= e) return true;
        if (nsegs >= kMaxSegs) return false;
        segs[nsegs++] = {s, e};
        return true;
    };

    // Active DFS stack with small verified-dead / on-stack sets (<= 32 PCs).
    std::uint64_t seen[kFlagScan];
    std::uint8_t state[kFlagScan];   // 1 = on active path (cycle if revisited), 2 = proven dead
    unsigned nseen = 0;
    const auto find_seen = [&](std::uint64_t p) -> int {
        for (unsigned i = 0; i < nseen; ++i)
            if (seen[i] == p) return static_cast<int>(i);
        return -1;
    };

    unsigned budget = kFlagScan;
    unsigned splits = 0;
    unsigned hops = 0;
    auto dfs = [&](auto&& self, std::uint64_t start_pc, unsigned depth) -> bool {
        if (depth > kMaxWork) return false;
        std::uint64_t seg_start = start_pc;
        std::uint64_t pc = start_pc;
        unsigned marked[kFlagScan];
        unsigned nmarked = 0;
        const auto finish = [&](bool ok) {
            for (unsigned i = 0; i < nmarked; ++i) state[marked[i]] = ok ? 2 : 0;
            return ok;
        };
        while (true) {
            if ((pc & 3) != 0 || !code.image_code(pc)) return finish(false);
            if (const int idx = find_seen(pc); idx >= 0) {
                if (state[idx] == 2) {
                    return finish(add_seg(seg_start, pc));
                }
                return finish(false);   // cycle or previously failed
            }
            if (budget == 0 || nseen >= kFlagScan) return finish(false);
            --budget;
            const unsigned slot = nseen++;
            seen[slot] = pc;
            state[slot] = 1;
            marked[nmarked++] = slot;

            std::uint32_t word = 0;
            if (!code.read(pc, word)) return finish(false);
            Inst d;
            decode(word, pc, d);
            if (d.kind == Kind::unknown && (word == 0xd503201fu || (word & 0xffffff3fu) == 0xd503241fu))
                d.kind = Kind::nop;
            if (d.kind == Kind::unknown) {
                if (families.count == 0) return finish(false);
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
                if (m.family < 0 || m.use.reads_flags) return finish(false);
                if (m.use.writes_flags) return finish(add_seg(seg_start, pc + 4));
                pc += 4;
                continue;
            }
            if (d.kind == Kind::branch) {
                if (hops >= 2) return finish(false);
                ++hops;
                if (!add_seg(seg_start, pc + 4)) return finish(false);
                const auto next = static_cast<std::uint64_t>(d.imm);
                seg_start = pc = next;
                continue;
            }
            if (d.kind == Kind::compare_branch || d.kind == Kind::test_branch) {
                if (splits >= 3) return finish(false);
                ++splits;
                if (!add_seg(seg_start, pc + 4)) return finish(false);
                const auto taken = static_cast<std::uint64_t>(d.imm);
                const bool ok = self(self, pc + 4, depth + 1) && self(self, taken, depth + 1);
                return finish(ok);
            }
            if (is_branch(d)) return finish(false);
            const auto u = usage(d);
            if (u.reads_flags) return finish(false);
            if (u.writes_flags) return finish(add_seg(seg_start, pc + 4));
            pc += 4;
        }
    };
    if (!dfs(dfs, target_pc, 0)) return false;
    for (unsigned s = 0; s < nsegs; ++s) plan.dependency_ranges.push_back(segs[s]);
    return true;
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
    plan.dependency_ranges.clear();
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
    plan.t2_dead = !overrides.t2_no_dead;
    plan.t2_lend = plan.optimize && overrides.t2_lend && !overrides.no_pins;
    plan.t2_galias = !overrides.t2_no_galias && !overrides.no_galias;
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
    plan.flag_skip = !overrides.no_flag_skip;
    plan.inline_cache_entries = overrides.no_inline_cache ? 0u : overrides.inline_cache_one ? 1u : 2u;
    plan.rsb_push = overrides.old_rsb ? Plan::RsbPush::old_masked : overrides.rsb_call ? Plan::RsbPush::call : Plan::RsbPush::inline_flag_free;
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
    bool any_undescribed_family = false;
    for (unsigned f = 0; f < families.count; ++f) any_undescribed_family |= !families.families[f].describe;

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
        // Optimized translations (hot code): larger leaves, and local subroutines
        // inside a function (PlanOverrides::kOptimizedInlineMax).
        const bool wide = plan.optimize && !overrides.no_loop_inline;
        const unsigned limit = wide ? std::max(overrides.inline_max, PlanOverrides::kOptimizedInlineMax) : overrides.inline_max;
        std::uint64_t lo = target, hi = target + 8ull * limit;
        if ((target & 3) != 0) return reject();
        {
            std::uint64_t b = 0, e = 0;
            if (code.function_bounds(target, b, e)) {
                if (e <= b || target >= e) return reject();
                if (b != target && !wide) return reject();                     // a local subroutine: optimized only
                if (b == target && e - b > 64ull * limit) return reject();     // a function far too big
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
        bool straight_bound;
        bool need_zext32;
        std::uint64_t bound_pc;
    };
    thread_local std::vector<FoundTable> found_tables;
    thread_local std::vector<std::uint64_t> found_pcs;
    // BRs on table shadows (PlanInst::shadow_base), unless a table takes them.
    struct FoundShadow {
        std::uint64_t br_pc;
        std::int8_t base, index;
        bool index32;
        std::uint8_t rshift, sub_stride;
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
            if (pc + 4 >= bound_begin && pc + 4 < bound_end) {
                continuations.push_back(pc + 4);
                stack.push_back((pc + 4) | kCheckEntry);
            }
            break;
        case Flow::interpret:
            // Stop walking past consecutive undecodable words in unbounded JIT code
            // (e.g. V8 constant pools / safepoint metadata after deopt BL tables).
            if (!function_bounded && !any_undescribed_family && plan.interpret_count >= 2 &&
                plan.insts.size() >= 2 && plan.insts[plan.insts.size() - 2].pc + 4 == pc &&
                plan.insts[plan.insts.size() - 2].flow == Flow::interpret)
                break;
            if (pc + 4 >= bound_begin && pc + 4 < bound_end) {
                continuations.push_back(pc + 4);
                stack.push_back((pc + 4) | kCheckEntry);
            }
            break;
        case Flow::indirect:
            if (pi.inst.kind == Kind::branch_reg && !overrides.no_shadows) {
                FoundShadow s{pc, -1, -1, false, 0, 0};
                const char* why =
                    match_shadow_load(pc, pi.inst.rn, code, s.base, s.index, s.index32, s.rshift, s.sub_stride);
                // Restrict table_shadow to direct stride-8 dispatch tables
                // (e.g. interpreter bytecode tables, sub_stride == 0); strided
                // JIT dispatch tables (V8 JSDispatchTable) run faster via the
                // 1-load direct-mapped lookup table without base reconstruction.
                if (!why && s.sub_stride == 0) found_shadows.push_back(s);
                static const bool trace = std::getenv("TRANSMUTE_FX_TRACE_TABLES") != nullptr;
                if (trace && !why)
                    std::fprintf(stderr, "table: br %llx: table shadow candidate (x%d, x%d%s rsh=%u sub=%u)\n",
                                 static_cast<unsigned long long>(pc), s.base, s.index, s.index32 ? " uxtw" : "",
                                 s.rshift, s.sub_stride);
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
                    std::uint32_t tbl_word = 0;
                    for (std::uint64_t t = 0; k < limit; ++k)
                        if (!table_target(code, m, k, t) || (t & 3) != 0 || !code.read(t, tbl_word)) break;
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
                    const bool image_unbounded = !function_bounded && code.image_code(pc);
                    for (unsigned k = 0; k < m.count && ok; ++k) {
                        std::uint64_t t = 0;
                        ok = table_target(code, m, k, t);
                        found_pcs.push_back(t);
                        if (!ok || (t & 3) != 0) continue;
                        const bool in_bounds = t >= bound_begin && t < bound_end;
                        const bool in_image_near =
                            image_unbounded && code.image_code(t) && (t >= pc ? t - pc : pc - t) < (256u << 10);
                        if (in_bounds || in_image_near) ++inside;
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
                    found_tables.push_back(
                        {pc, first, m.count, m.index_reg, m.by_target, m.straight_bound, m.need_zext32, m.bound_pc});
                    if (overrides.table_exits) break;   // every entry a linkable direct exit (cases are own regions)
                    std::uint64_t lo_case = ~0ULL, hi_case = 0;
                    std::vector<std::uint64_t> uniq_cases;
                    uniq_cases.reserve(m.count);
                    for (unsigned k = 0; k < m.count; ++k) {
                        const auto t = found_pcs[first + k];
                        if ((t & 3) != 0) continue;
                        lo_case = std::min(lo_case, t);
                        hi_case = std::max(hi_case, t);
                        uniq_cases.push_back(t);
                    }
                    std::sort(uniq_cases.begin(), uniq_cases.end());
                    uniq_cases.erase(std::unique(uniq_cases.begin(), uniq_cases.end()), uniq_cases.end());
                    std::uint64_t max_case_gap = 0;
                    for (std::size_t k = 1; k < uniq_cases.size(); ++k)
                        max_case_gap = std::max(max_case_gap, uniq_cases[k] - uniq_cases[k - 1]);
                    // Inline compact switches (e.g. tight dispatch loops) directly into
                    // the region; keep wide/deep C++ switches or switches with large arms
                    // as linkable direct exits so cold arms do not bloat L1i or dilute
                    // loop register-pin weights.
                    const bool deep_switch = pc > guest_pc && (pc - guest_pc) > 256;
                    if (plan.insts.size() >= 64 || deep_switch || max_case_gap > 256 ||
                        (lo_case <= hi_case && hi_case - lo_case > 1024))
                        break;
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
    // Complete first-tier regions (including loop-free final handlers such as
    // V8 LoadIC/Call): enable fast single-pass dead-store elimination and
    // zero-cost GPR copy aliases without 2-pass re-JIT overhead.
    if (tier2 && !plan.optimize && !plan.truncated) {
        if (!overrides.t2_no_dead && plan.insts.size() <= 2048) plan.t2_dead = true;
        if (!overrides.t2_no_galias && !overrides.no_galias) plan.t2_galias = true;
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
        plan.tables.push_back(
            {static_cast<std::uint32_t>(plan.table_pcs.size()), ft.count, ft.index_reg, ft.by_target, ft.straight_bound, false});
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
        pi.shadow_rshift = fs.rshift;
        pi.shadow_sub_stride = fs.sub_stride;
    }

    bool back_edges = false;   // dataflow below needs iteration only with back edges
    const auto add_edge = [&](std::int32_t from, std::int32_t to) {
        auto& t = plan.insts[to];
        if (t.first_pred < 0) { t.first_pred = from; t.preds = 1; }
        else if (t.first_pred != from && t.preds < 2) t.preds = 2;
        if (from >= to) back_edges = true;
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
    // True CFG cycle detection for loop headers: a layout back-edge from >= to
    // only forms a loop if `to` can actually reach `from` in the region CFG
    // (cold out-of-line slow paths jumping back to a hot rejoin are not loops).
    if (back_edges) {
        std::vector<std::int32_t> cand_max(static_cast<std::size_t>(n), -1);
        for (std::int32_t i = 0; i < n; ++i)
            plan.for_each_successor(i, [&](std::int32_t s) {
                if (s <= i) cand_max[static_cast<std::size_t>(s)] = std::max(cand_max[static_cast<std::size_t>(s)], i);
            });
        std::vector<std::uint32_t> seen(static_cast<std::size_t>(n), 0);
        std::vector<std::int32_t> dfs_stack;
        std::uint32_t stamp = 0;
        for (std::int32_t h = 0; h < n; ++h) {
            if (cand_max[static_cast<std::size_t>(h)] < 0) continue;
            ++stamp;
            dfs_stack.clear();
            dfs_stack.push_back(h);
            seen[static_cast<std::size_t>(h)] = stamp;
            std::int32_t max_back = -1;
            while (!dfs_stack.empty()) {
                const auto u = dfs_stack.back();
                dfs_stack.pop_back();
                plan.for_each_successor(u, [&](std::int32_t s) {
                    if (s == h && u >= h) {
                        max_back = std::max(max_back, u);
                        return;
                    }
                    if (seen[static_cast<std::size_t>(s)] != stamp) {
                        seen[static_cast<std::size_t>(s)] = stamp;
                        dfs_stack.push_back(s);
                    }
                });
            }
            if (max_back >= 0) {
                plan.insts[static_cast<std::size_t>(h)].loop_header = true;
                plan.insts[static_cast<std::size_t>(h)].loop_end = max_back;
            }
        }
    }
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

    // Verify JumpTable::proven_bounded after leaders/entries are finalized:
    // no entry or branch target may land between the bound check and the BR,
    // and every table entry must be representable in the host jump table.
    for (std::size_t ti = 0; ti < found_tables.size(); ++ti) {
        const auto& ft = found_tables[ti];
        const auto br = find_index(ft.br_pc);
        if (br < 0) continue;
        const auto tbl_idx = plan.insts[static_cast<std::size_t>(br)].table;
        if (tbl_idx < 0) continue;
        auto& tbl = plan.tables[static_cast<std::size_t>(tbl_idx)];
        if (!tbl.proven_bounded) continue;
        const auto bidx = find_index(ft.bound_pc);
        bool ok = bidx >= 0 && bidx < br;
        if (ok && ft.need_zext32 && !(ext_facts(plan, br, ft.index_reg) & ext_zero32)) ok = false;
        for (std::int32_t j = bidx; ok && j < br; ++j) {
            const auto& pj = plan.insts[static_cast<std::size_t>(j)];
            const auto& nxt = plan.insts[static_cast<std::size_t>(j + 1)];
            if (pj.fall != j + 1 || nxt.entry || nxt.leader) ok = false;
            if (!code.image_code(pj.pc)) ok = false;
        }
        for (std::uint32_t k = 0; ok && k < tbl.count; ++k) {
            const auto tpc = plan.table_pcs[tbl.first + k];
            if ((tpc & 3) != 0) ok = false;
            if (!tbl.by_target) {
                const auto rel = static_cast<std::int64_t>(tpc - plan.start);
                if (rel != static_cast<std::int32_t>(rel)) ok = false;
            }
        }
        tbl.proven_bounded = ok;
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
            case Flow::indirect:
                if (pi.table >= 0) {
                    auto& tbl = plan.tables[static_cast<std::size_t>(pi.table)];
                    bool cases_live = false;
                    for (std::uint32_t k = 0; k < tbl.count && !cases_live; ++k) {
                        const auto s = plan.table_targets[tbl.first + k];
                        cases_live = s >= 0 ? plan.insts[static_cast<std::size_t>(s)].flags_live_in
                                            : exit_live(plan.table_pcs[tbl.first + k]);
                    }
                    tbl.cases_flags_dead = !cases_live;
                    out = tbl.proven_bounded ? cases_live : (strict_flags || cases_live || br_live(i));
                } else {
                    out = strict_flags || (pi.inst.kind != Kind::ret && br_live(i));
                }
                break;
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
        // In counted first-tier regions (plan.count_loop_headers), check whether
        // entry 0 can reach an unconditional region exit without entering any
        // loop. If so, bypassable cold loops (e.g. slow-path arg-copy or proxy
        // loops in dispatch stubs) keep weight 1 until their loop counter proves
        // them hot and tiers the region up to tier 2, preventing cold loops from
        // evicting cross-region convention registers on every fast-path call.
        bool bypass_cold_loops = false;
        if (plan.count_loop_headers && !plan.insts[0].loop_header) {
            std::vector<std::uint8_t> vis(static_cast<std::size_t>(n), 0);
            std::vector<std::int32_t> q;
            q.push_back(0);
            vis[0] = 1;
            while (!q.empty() && !bypass_cold_loops) {
                const auto u = q.back();
                q.pop_back();
                const auto& pu = plan.insts[static_cast<std::size_t>(u)];
                if ((pu.flow == Flow::indirect && pu.table < 0) || (pu.flow == Flow::jump && pu.taken < 0) ||
                    (pu.flow == Flow::normal && pu.fall < 0)) {
                    bypass_cold_loops = true;
                    break;
                }
                plan.for_each_successor(u, [&](std::int32_t s) {
                    if (s >= 0 && !vis[static_cast<std::size_t>(s)] && !plan.insts[static_cast<std::size_t>(s)].loop_header) {
                        vis[static_cast<std::size_t>(s)] = 1;
                        q.push_back(s);
                    }
                });
            }
        }
        depth.assign(static_cast<std::size_t>(n) + 1, 0);
        bool has_loops = false;
        if (!bypass_cold_loops) {
            for (std::int32_t h = 0; h < n; ++h) {
                if (const auto e = plan.insts[static_cast<std::size_t>(h)].loop_end; e >= 0) {
                    ++depth[static_cast<std::size_t>(h)];
                    --depth[static_cast<std::size_t>(e) + 1];
                    has_loops = true;
                }
            }
        }
        // Fast-path reachability from entry 0 without crossing a non-inlined
        // call/SVC/interpret exit. In non-loop (or cold-loop-bypassed) regions
        // with cold post-call continuations (e.g. V8 LoadIC with 100+ miss/stub
        // BL continuations), weighting the call-free fast path 2x prioritizes
        // hot fast-path registers (like x28) over cold slow-path temps.
        std::vector<std::uint8_t> fast_path;
        bool fast_path_boost = false;
        if (!has_loops && n > 1) {
            fast_path.assign(static_cast<std::size_t>(n), 0);
            std::vector<std::int32_t> fq;
            fq.push_back(0);
            fast_path[0] = 1;
            std::int32_t fast_count = 1;
            while (!fq.empty()) {
                const auto u = fq.back();
                fq.pop_back();
                plan.for_each_successor(u, [&](std::int32_t s) {
                    if (s >= 0 && !fast_path[static_cast<std::size_t>(s)]) {
                        fast_path[static_cast<std::size_t>(s)] = 1;
                        ++fast_count;
                        fq.push_back(s);
                    }
                });
            }
            fast_path_boost = fast_count < n;
        }
        // benefit: loop-weighted uses. cost: a load at every entry and a store
        // at every exit where the register may be dirty. Unconditional exits
        // inside loops (calls, returns) are weighted like their instruction;
        // conditional exits are assumed rare.
        // All weights are scaled by kPinScale = 4 so repeated intra-block uses
        // truly count as a quarter (1 vs 4) even at level 0.
        constexpr std::int32_t kPinScale = 4;
        std::int32_t benefit[32]{};
        std::int32_t vbenefit[32]{};
        std::int32_t level = 0;
        // The register cache serves repeated uses within a block, so only a
        // block's first read and first write of a register count fully;
        // other uses count a quarter (evictions under pressure).
        // PlanOverrides::old_pins: every use counts fully.
        std::uint32_t seen = 0, wseen = 0, vseen = 0, vwseen = 0;
        std::uint32_t vexposed = 0, vused = 0;
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
        // classes 0..4: lw[level]; class 5: conditional exit (kPinScale);
        // class 6: fast-path level-0 exit (2*kPinScale); class 7: off-fast-path or loop-region level-0 exit (1)
        BitCounter exits[8], vexits[8];
        // Loop weights (scaled by kPinScale = 4): 1 + 8 * level; tier 2: 8^level.
        static constexpr std::int32_t kLinear[5] = {4, 36, 68, 100, 132}, kExp[5] = {4, 32, 256, 2048, 16384};
        const std::int32_t* lw = plan.optimize ? kExp : kLinear;
        const auto exit_weight = [&](int c) -> std::int32_t {
            if (c == 5) return kPinScale;
            if (c == 6) return lw[0] * 2;
            if (c == 7) return 1;
            return lw[c];
        };
        std::int32_t entry_sum = 0;
        std::int32_t exit_sum = 0;        // all exits, weighted (reloads of evicted convention registers)
        // Identify cold deopt-table BL stubs / interpret metadata outside loops so
        // they do not inflate entry_sum / exit_sum and block pinning hot registers
        // like V8's x28/x26. A back-to-back BL chain (`BL; BL; ...`) is V8's
        // noreturn deopt exit table.
        const auto is_cold_deopt_call = [&](std::int32_t idx) -> bool {
            const auto& p = plan.insts[static_cast<std::size_t>(idx)];
            if (p.flow != Flow::call || !p.leader) return false;
            const bool next_call_or_interp =
                p.continuation == idx + 1 && idx + 1 < n &&
                (plan.insts[static_cast<std::size_t>(idx + 1)].flow == Flow::call ||
                 plan.insts[static_cast<std::size_t>(idx + 1)].flow == Flow::interpret);
            const bool prev_call = idx > 0 && plan.insts[static_cast<std::size_t>(idx - 1)].flow == Flow::call &&
                                   plan.insts[static_cast<std::size_t>(idx - 1)].continuation == idx;
            return next_call_or_interp || prev_call;
        };
        // Pre-scan Tier-2 FP by-element (.4S/.2D FMUL/FMULX/FMLA/FMLS) reads
        // whose broadcast will be hoisted by vec_derived (outer-depth def or
        // >= 3 uses of the same (rm, lane)). Those consumers read the derived
        // context slot directly rather than Vm, so exclude Vm from their
        // vbenefit to keep hoisted vectors from stealing pinned XMMs.
        std::uint8_t fp_elem_hoisted[32][6]{};
        const auto decode_fp_elem_q = [](std::uint32_t w, unsigned& rm, unsigned& slot, unsigned& rn, unsigned& rd,
                                         bool& acc) -> bool {
            if ((w & 0xdf000400u) != 0x4f000000u) return false;
            const bool u = ((w >> 29) & 1u) != 0;
            const unsigned size = (w >> 22) & 3u;
            const unsigned opc = (w >> 12) & 15u;
            if (!((!u && (opc == 1 || opc == 5 || opc == 9)) || (u && opc == 9))) return false;
            const unsigned l = (w >> 21) & 1u, h = (w >> 11) & 1u;
            if (size == 2) {
                slot = (h << 1) | l;
            } else if (size == 3 && l == 0) {
                slot = 4u + h;
            } else {
                return false;
            }
            rm = (w >> 16) & 31u;
            rn = (w >> 5) & 31u;
            rd = w & 31u;
            acc = !u && (opc == 1 || opc == 5);
            return true;
        };
        if (plan.t2_derived) {
            std::int32_t max_vwrite_depth[32];
            std::fill(std::begin(max_vwrite_depth), std::end(max_vwrite_depth), -1);
            std::uint16_t elem_count[32][6]{};
            std::uint8_t elem_max_depth[32][6]{};
            std::int32_t scan_level = 0;
            for (std::int32_t i = 0; i < n; ++i) {
                scan_level += depth[i];
                const auto& pi = plan.insts[static_cast<std::size_t>(i)];
                if (pi.flow == Flow::interpret || pi.conservative_writes) continue;
                for (auto vm = pi.vwrites; vm; vm &= vm - 1) {
                    const auto r = static_cast<unsigned>(std::countr_zero(vm));
                    if (scan_level > max_vwrite_depth[r]) max_vwrite_depth[r] = scan_level;
                }
                unsigned rm, slot, rn, rd;
                bool acc;
                if (decode_fp_elem_q(pi.inst.word, rm, slot, rn, rd, acc)) {
                    if (elem_count[rm][slot] < 255) ++elem_count[rm][slot];
                    if (static_cast<unsigned>(scan_level) > elem_max_depth[rm][slot])
                        elem_max_depth[rm][slot] = static_cast<std::uint8_t>(std::min(scan_level, 255));
                }
            }
            for (unsigned r = 0; r < 32; ++r) {
                for (unsigned s = 0; s < 6; ++s) {
                    if (elem_count[r][s] >= 3 ||
                        (elem_count[r][s] > 0 && max_vwrite_depth[r] >= 0 &&
                         max_vwrite_depth[r] < static_cast<std::int32_t>(elem_max_depth[r][s]))) {
                        fp_elem_hoisted[r][s] = 1;
                    }
                }
            }
        }
        for (std::int32_t i = 0; i < n; ++i) {
            level += depth[i];
            const auto& pi = plan.insts[i];
            const auto clamped = std::min(level, 4);
            const bool on_fast = fast_path_boost && level == 0 && fast_path[static_cast<std::size_t>(i)] != 0;
            const bool off_fast = (fast_path_boost && level == 0 && !on_fast) || (has_loops && level == 0);
            const auto w = on_fast ? (lw[0] * 2) : off_fast ? 1 : lw[clamped];
            const bool cold_cont =
                level == 0 && pi.entry && pi.entry_slot > 0 && i > 0 &&
                (plan.insts[static_cast<std::size_t>(i - 1)].flow == Flow::interpret ||
                 is_cold_deopt_call(i - 1));
            if (pi.entry && !cold_cont)
                entry_sum += pi.entry_slot == 0 ? (on_fast ? -lw[0] * 2 : (has_loops ? -1 : -kPinScale)) : -w;   // the region start runs once per entry
            const auto dirty_out = pi.dirty_in | pi.writes;
            const auto vdirty_out = pi.vdirty_in | pi.vwrites;
            int exit_class = -1;   // exits[] index
            const int normal_class = on_fast ? 6 : off_fast ? 7 : clamped;
            switch (pi.flow) {
            case Flow::indirect:
                if (pi.table >= 0) break;   // switch dispatch: stays in the region (the fallback is rare)
                exit_class = normal_class;
                break;
            case Flow::call:
                if (level == 0 && is_cold_deopt_call(i)) break;
                exit_class = normal_class;
                break;
            case Flow::trap:
            case Flow::interpret:
                if (level == 0) break;   // cold trap/metadata exit outside loops
                exit_class = normal_class;
                break;
            case Flow::svc: exit_class = normal_class; break;
            case Flow::jump: if (pi.taken < 0) exit_class = normal_class; break;
            case Flow::normal: if (pi.fall < 0) exit_class = normal_class; break;
            case Flow::cond: if (pi.taken < 0 || pi.fall < 0) exit_class = off_fast ? 7 : 5; break;
            }
            if (exit_class >= 0) {
                exits[exit_class].add(dirty_out);
                vexits[exit_class].add(vdirty_out);
                exit_sum += exit_weight(exit_class);
            }
            if (pi.leader) seen = wseen = vseen = vwseen = 0;
            if (pi.flow == Flow::interpret || pi.conservative_writes) continue;
            std::uint32_t eff_vreads = pi.vreads;
            if (plan.t2_derived && eff_vreads) {
                unsigned rm, slot, rn, rd;
                bool acc;
                if (decode_fp_elem_q(pi.inst.word, rm, slot, rn, rd, acc) && fp_elem_hoisted[rm][slot] &&
                    rm != rn && (!acc || rm != rd)) {
                    eff_vreads &= ~(1u << rm);
                }
            }
            vexposed |= eff_vreads & ~vwseen;
            vused |= eff_vreads | pi.vwrites;
            if (!overrides.old_pins) {
                const auto first = (pi.reads & ~seen) | (pi.writes & ~wseen);
                const auto vfirst = (eff_vreads & ~vseen) | (pi.vwrites & ~vwseen);
                const auto rest_w = std::max<std::int32_t>(1, w / 4);
                add(first, w);
                add((pi.reads | pi.writes) & ~first, rest_w);
                vadd(vfirst, w);
                vadd((eff_vreads | pi.vwrites) & ~vfirst, rest_w);
                seen |= pi.reads | pi.writes;
                wseen |= pi.writes;
                vseen |= eff_vreads | pi.vwrites;
                vwseen |= pi.vwrites;
            } else {
                add(pi.reads | pi.writes, w);
                vadd(eff_vreads | pi.vwrites, w);
            }
        }
        std::int32_t use_benefit[32];   // uses only (convention registers pay nothing at entries and exits)
        std::copy(std::begin(benefit), std::end(benefit), std::begin(use_benefit));
        for (int c = 0; c < 8; ++c) {
            const std::int32_t cw = -exit_weight(c);
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
                if (weight[r] < kPinScale) break;
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
                if (weight[r] < kPinScale) break;
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
        const unsigned vlimit = std::min(overrides.vector_pin_limit, 12u);
        const bool vpressure = std::popcount(vused) > 12;
        for (unsigned k = 0, used_v = 0; k < 32 && used_v < vlimit; ++k) {
            const auto r = vorder[k];
            if (vbenefit[r] < kPinScale) break;
            // Pure block-local vector temps (never read before written in any
            // basic block) waste entry loads and exit stores when the block-local
            // vector cache already has ample free XMMs.
            if (!vpressure && !((vexposed >> r) & 1u)) continue;
            plan.vpinned |= 1u << r;
            plan.vpin_host[r] = static_cast<std::int8_t>(15 - used_v);   // xmm15 downwards
            plan.vpin_hosts |= 1u << (15 - used_v);
            ++used_v;
        }
    }
}

} // namespace core
} // namespace transmute::fx
