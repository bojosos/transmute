// translate_region(): plan, emit, retry on emitter limitations (owner: translator core).
#include "fx/translator/region_emitter.hpp"
#include "fx/translator/region_plan.hpp"
#include "fx/translator/translator.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace transmute::fx {

namespace core {
CoreCounters& core_counters() {
    static CoreCounters counters{};
    return counters;
}

namespace {
// Debugging knob (read once): TRANSMUTE_FX_CORE=nopins,novecpins,noextrapins,
// nocache (register cache), nofcmp (FCMP flags fusion), exactflags (NZCV live
// across BL/RET), familypins7 (no r10/r11 pins in regions with family
// instructions), norbit (no RBIT+CLZ -> TZCNT), nowriteback (register cache
// stores results immediately), noreusedest (do not reuse overwritten cached
// destinations), nofacts (no vector value facts for fusions), novzjoin (V
// registers' zero-above facts forgotten at every label), notier2 (no
// optimizing tier: on by default, region_alloc.cpp; tier2 is accepted), the
// tier-2 features t2nochecks, t2noalign, t2noalloc, t2nodead, t2nogalias,
// t2noderived, t2noalias (vector copy aliases), t2lend (opt-in pin lending),
// t2carry (dirty values across loop edges, prototype), t2entries (count
// region entries instead of loop iterations), t2loops (one per-thread loop
// counter, tier-up at the header), t2proto (the prototype's tier 2: every
// t2no<x>, t2entries), wbunsafe (MEASUREMENT ONLY: no write-back flush
// at fault sites, so faults see stale state), nofaultmap (flush dirty cached
// registers before fault sites instead of describing them in fault maps),
// nocarry (block-local register cache: no entries across merge points),
// carrydirty (dirty cache entries also cross block edges inside loops; the
// default carries clean entries only), lru (register-cache victims least
// recently used instead of by next use), oldpins (pin weights count every
// use instead of a block's first read / write). nocarry,nofaultmap,lru,oldpins
// give the code generation of the block-local write-back cache; notier (no
// first-tier regions: TranslateOptions::lazy is ignored, every JIT region is
// formed eagerly as before tiering); noinline (no call inlining), inlinehot
// (inline only in tier-1 translations: regions that would inline are tier 0
// first; TRANSMUTE_FX_INLINE=<n> sets the callee size limit); notables (every
// BR is an indirect exit: no in-region switch dispatch); oldlayout (the code
// layout before the hot-footprint work: exit tails, lookups and entry stubs
// in line, every loop aligned, rel32 branches only), oldcap (regions stop at
// max_instructions inside large .pdata functions), coldentries (secondary
// entry stubs stay in the cold section), loopalign (every loop header
// aligned to 16; codegen_layout_bits), hotalign (tier-2 regions align loop
// headers of any size, not only small loops), align32 (loop headers aligned
// to 32 bytes instead of 16); nocache..norbit
// the code generation before the register cache. Folded into
// translator_family_fingerprint(), so persistent
// caches never mix code generated with different knobs.
// The knobs are comma-separated words, matched exactly (notier vs notier2).
bool has_knob(const std::string& list, const char* name) {
    const std::size_t n = std::strlen(name);
    for (std::size_t at = 0; at <= list.size();) {
        auto end = list.find(',', at);
        if (end == std::string::npos) end = list.size();
        if (end - at == n && list.compare(at, n, name) == 0) return true;
        at = end + 1;
    }
    return false;
}

// The optimizing tier for hot loops (measurements: docs/FX_CLASSROOM_PROFILE.md).
constexpr bool kTier2ByDefault = true;

const PlanOverrides& default_overrides() {
    static const PlanOverrides o = [] {
        PlanOverrides d;
        d.tier2 = kTier2ByDefault;
        if (const char* v = std::getenv("TRANSMUTE_FX_CORE")) {
            const std::string s(v);
            const auto k = [&](const char* name) { return has_knob(s, name); };
            d.no_pins = k("nopins");
            d.no_extra_pins = k("noextrapins");
            if (k("novecpins")) d.vector_pin_limit = 0;
            d.no_reg_cache = k("nocache");
            d.no_fcmp_fusion = k("nofcmp");
            d.exact_flags = k("exactflags");
            d.family_pins7 = k("familypins7");
            d.no_rbit_fusion = k("norbit");
            d.no_write_back = k("nowriteback");
            d.no_reuse_dest = k("noreusedest");
            d.no_vec_facts = k("nofacts");
            d.no_vzero_join = k("novzjoin");
            // Tier 2 is on by default (docs/FX_ENGINE.md, "Tier 2"); notier2 turns it off.
            d.tier2 = kTier2ByDefault ? !k("notier2") : k("tier2");
            d.t2_no_alias = k("t2noalias");
            d.t2_carry = k("t2carry");
            d.t2_loops = k("t2loops");
            const bool proto = k("t2proto");   // the prototype's tier-2 code generation
            d.t2_no_checks = proto || k("t2nochecks");
            d.t2_no_align = proto || k("t2noalign");
            d.t2_no_alloc = proto || k("t2noalloc");
            d.t2_no_dead = proto || k("t2nodead");
            d.t2_lend = !proto && k("t2lend");
            d.t2_no_galias = proto || k("t2nogalias");
            d.t2_no_derived = proto || k("t2noderived");
            d.t2_entries = proto || k("t2entries");
            d.wb_unsafe = k("wbunsafe");
            d.no_fault_maps = k("nofaultmap");
            d.no_carry = k("nocarry");
            d.old_pins = k("oldpins");
            d.lru = k("lru");
            d.carry_dirty = k("carrydirty");
            d.eager = k("notier");
            d.no_inline = k("noinline");
            d.inline_hot = k("inlinehot");
            d.no_tables = k("notables");
            d.table_exits = k("tableexits");
            d.old_layout = k("oldlayout");
            d.old_cap = k("oldcap");
            d.cold_entries = k("coldentries");
            d.align_loops = k("loopalign");
            d.align_hot = k("hotalign");
            d.align32 = k("align32");
        }
        if (const char* v = std::getenv("TRANSMUTE_FX_INLINE")) d.inline_max = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
        return d;
    }();
    return o;
}
} // namespace

std::uint32_t codegen_knob_bits() {
    const auto& o = default_overrides();
    return (o.no_pins ? 1u : 0u) | (o.no_extra_pins ? 2u : 0u) | (o.vector_pin_limit != 10 ? 4u : 0u) |
           (o.no_reg_cache ? 8u : 0u) | (o.no_fcmp_fusion ? 16u : 0u) | (o.exact_flags ? 32u : 0u) |
           (o.family_pins7 ? 64u : 0u) | (o.no_rbit_fusion ? 128u : 0u) | (o.no_write_back ? 256u : 0u) |
           (o.wb_unsafe ? 512u : 0u) | (o.no_fault_maps ? 1024u : 0u) |
           (o.no_carry ? 2048u : 0u) | (o.old_pins ? 4096u : 0u) |
           (o.lru ? 8192u : 0u) | (o.carry_dirty ? 16384u : 0u) | (o.eager ? 32768u : 0u) |
           (isel_knob_bits() << 16) |   // nofold/nozext/nosrcreg/nomovk/oldisel (core_isel.hpp)
           (o.inline_hot ? 1u << 22 : 0u) | (o.no_inline ? 1u << 23 : 0u) | (o.no_tables ? 1u << 20 : 0u) |
           (o.table_exits ? 1u << 21 : 0u) | (o.no_reuse_dest ? 1u << 24 : 0u) |
           (o.no_vec_facts ? 1u << 25 : 0u) | (o.tier2 != kTier2ByDefault ? 1u << 26 : 0u) | (o.t2_no_alias ? 1u << 27 : 0u) |
           (o.t2_carry ? 1u << 28 : 0u) | (o.t2_loops ? 1u << 29 : 0u) | (o.old_layout ? 1u << 30 : 0u) |
           (o.old_cap ? 1u << 31 : 0u);
}

std::uint32_t codegen_layout_bits() {
    const auto& o = default_overrides();
    return (o.cold_entries ? 1u : 0u) | (o.align_loops ? 2u : 0u) | (o.align_hot ? 4u : 0u) | (o.align32 ? 8u : 0u) |
           (o.no_vzero_join ? 16u : 0u);
}

unsigned codegen_inline_max() { return default_overrides().inline_max; }

namespace {
thread_local const PlanOverrides* g_test_overrides = nullptr;
}

void set_overrides_for_testing(const PlanOverrides* overrides) { g_test_overrides = overrides; }

const PlanOverrides& default_plan_overrides() { return default_overrides(); }

std::uint32_t codegen_tier2_bits() {
    const auto& o = default_overrides();
    return (o.t2_no_checks ? 1u : 0u) | (o.t2_no_align ? 2u : 0u) | (o.t2_no_alloc ? 4u : 0u) |
           (o.t2_no_dead ? 8u : 0u) | (o.t2_lend ? 16u : 0u) | (o.t2_no_galias ? 32u : 0u) |
           (o.t2_entries ? 64u : 0u) | (o.t2_no_derived ? 128u : 0u);
}
} // namespace core

// The cross-region register convention (translator.hpp). Default: SP, x19,
// x20, x21, x22, FP and LR in the seven host callee-saved registers; SP in
// rbx (the most common base register), LR in r13 (never a base), FP in rbp.
const RegisterConvention& translator_register_convention() {
    static const RegisterConvention conv = [] {
        RegisterConvention c;
        std::fill(std::begin(c.host), std::end(c.host), static_cast<std::int8_t>(-1));
        // Preferred host per guest register; others take the next free one.
        struct Pref { unsigned guest; std::uint8_t host; };
        static constexpr Pref kPreferred[] = {{31, 3 /*rbx*/}, {19, 6 /*rsi*/}, {20, 7 /*rdi*/}, {21, 14 /*r14*/},
                                              {22, 12 /*r12*/}, {29, 5 /*rbp*/}, {30, 13 /*r13*/}};
        static constexpr std::uint8_t kHosts[] = {3, 6, 7, 14, 12, 5, 13};
        std::uint32_t want = (1u << 31) | (1u << 19) | (1u << 20) | (1u << 21) | (1u << 22) | (1u << 29) | (1u << 30);
        if (const char* v = std::getenv("TRANSMUTE_FX_CORE"); v && std::string(v).find("noglobals") != std::string::npos)
            want = 0;
        if (const char* v = std::getenv("TRANSMUTE_FX_GLOBALS")) {
            want = 0;
            std::string s(v);
            for (std::size_t at = 0; at < s.size();) {
                auto end = s.find(',', at);
                if (end == std::string::npos) end = s.size();
                const auto name = s.substr(at, end - at);
                at = end + 1;
                if (name == "sp") want |= 1u << 31;
                else if (name == "fp") want |= 1u << 29;
                else if (name == "lr") want |= 1u << 30;
                else if (name.size() >= 2 && name[0] == 'x') {
                    const auto r = std::strtoul(name.c_str() + 1, nullptr, 10);
                    if (r < 31 && r != 18) want |= 1u << r;   // x18: platform register (never pinned)
                }
            }
        }
        std::uint32_t used = 0;
        for (const auto& p : kPreferred) {
            if (!((want >> p.guest) & 1u)) continue;
            c.host[p.guest] = static_cast<std::int8_t>(p.host);
            used |= 1u << p.host;
        }
        for (unsigned g = 0; g < 32; ++g) {
            if (!((want >> g) & 1u) || c.host[g] >= 0) continue;
            for (const auto h : kHosts) {
                if ((used >> h) & 1u) continue;
                c.host[g] = static_cast<std::int8_t>(h);
                used |= 1u << h;
                break;
            }
        }
        for (unsigned g = 0; g < 32; ++g)
            if (c.host[g] >= 0) c.guest_mask |= 1u << g;
        return c;
    }();
    return conv;
}

namespace {
// Empties `r` for a new translation, keeping the vectors' capacity.
void reset_region(TranslatedRegion& r) {
    r.guest_start = 0;
    r.guest_ranges.clear();
    r.code.clear();
    r.entries.clear();
    r.exits.clear();
    r.fixups.clear();
    r.faults.clear();
    r.pins.clear();
    r.stats = {};
    r.fault_maps.clear();
    r.fault_map_regs.clear();
    r.tier = 1;
    r.tier_counter = 0;
    r.tier_counter_slot = 0;
}
} // namespace

TranslatedRegion translate_region(std::uint64_t guest_pc, const CodeSource& code, const TranslateOptions& options) {
    TranslatedRegion region;
    translate_region(guest_pc, code, options, region);
    return region;
}

void translate_region(std::uint64_t guest_pc, const CodeSource& code, const TranslateOptions& options,
                      TranslatedRegion& region) {
    thread_local core::Plan plan;
    core::PlanOverrides overrides = core::g_test_overrides ? *core::g_test_overrides : core::default_overrides();
    for (unsigned attempt = 0;; ++attempt) {
        if (attempt == 48) overrides.no_pins = true;
        core::build_plan(guest_pc, code, options, overrides, plan);
        reset_region(region);
        try {
            if (plan.optimize && plan.t2_alloc) {
                // Tier 2: pass 1 records the register cache at every edge,
                // pass 2 emits with entry states chosen from all of them.
                {
                    core::RegionEmitter first(plan, options, region);
                    first.set_alloc_pass(1);
                    first.run();
                    first.build_presets();
                }
                reset_region(region);
                core::RegionEmitter emitter(plan, options, region);
                emitter.set_alloc_pass(2);
                emitter.run();
            } else {
                core::RegionEmitter emitter(plan, options, region);
                emitter.run();
            }
            core::core_counters().regions.fetch_add(1, std::memory_order_relaxed);
            // Debugging knob: TRANSMUTE_FX_DUMP_PLAN=<hex guest pc> prints that region's plan.
            // (Read once: getenv per translation cost ~10% of the JIT.)
            static const std::uint64_t dump_pc = [] {
                const char* dump = std::getenv("TRANSMUTE_FX_DUMP_PLAN");
                return dump ? std::strtoull(dump, nullptr, 16) : ~std::uint64_t{0};
            }();
            // Diagnostics: TRANSMUTE_FX_TRACE_INLINE=1 prints every region with inlined calls.
            static const bool trace_inline = std::getenv("TRANSMUTE_FX_TRACE_INLINE") != nullptr;
            if (trace_inline && plan.inlined_calls != 0)
                std::fprintf(stderr, "inline: region %llx, %u calls, %u callee instructions, %u instructions\n",
                             static_cast<unsigned long long>(guest_pc), plan.inlined_calls, plan.inlined_insts,
                             static_cast<unsigned>(plan.insts.size()));
            if (dump_pc == guest_pc) {
                for (const auto& pi : plan.insts)
                    std::fprintf(stderr, "%llx %08x %s%s%s%s%s fl_in %d fl_out %d rf %d wf %d flow %d taken %d fall %d seg %u\n",
                                 static_cast<unsigned long long>(pi.pc), pi.inst.word, pi.leader ? "L" : "-",
                                 pi.entry ? "E" : "-", pi.loop_header ? "H" : "-", pi.inherit_flags ? "I" : "-",
                                 pi.inline_call ? "C" : pi.inline_ret ? "R" : "-",
                                 pi.flags_live_in, pi.flags_live_out, pi.reads_flags, pi.writes_flags,
                                 static_cast<int>(pi.flow), pi.taken, pi.fall, static_cast<unsigned>(pi.segment));
            }
            return;
        } catch (const core::RetryTranslation& retry) {
            core::core_counters().retries.fetch_add(1, std::memory_order_relaxed);
            static const bool trace_retry = std::getenv("TRANSMUTE_FX_TRACE_RETRY") != nullptr;
            if (trace_retry) {
                std::uint32_t word = 0;
                code.read(retry.pc, word);
                std::fprintf(stderr, "retry at %llx word %08x extra_pins %d resource %d\n",
                             static_cast<unsigned long long>(retry.pc), word, plan.extra_pins ? 1 : 0,
                             static_cast<int>(retry.resource));
            }
            if (retry.resource == core::RetryTranslation::Resource::aliases &&
                (!overrides.no_vec_alias || !overrides.no_galias)) {
                overrides.no_vec_alias = true;   // tier 2: an alias reached a point that needs none
                overrides.no_galias = true;
                continue;
            }
            if (retry.resource == core::RetryTranslation::Resource::facts && !overrides.no_vec_facts) {
                overrides.no_vec_facts = true;   // a checked fact the region cannot keep (region_emitter.cpp)
                continue;
            }
            if (retry.resource == core::RetryTranslation::Resource::writeback && !overrides.no_write_back) {
                overrides.no_write_back = true;   // a block the write-back cache cannot keep exact
                continue;
            }
            if (retry.resource == core::RetryTranslation::Resource::vec && plan.vpinned != 0 &&
                overrides.vector_pin_limit > 0) {   // give xmm scratch back first
                overrides.vector_pin_limit = overrides.vector_pin_limit > 4 ? 4 : 0;
                continue;
            }
            if (plan.extra_pins && !overrides.no_extra_pins) {   // first give the scratch registers back
                overrides.no_extra_pins = true;
                continue;
            }
            auto& list = retry.conservative ? overrides.conservative : overrides.interpret;
            if (std::find(list.begin(), list.end(), retry.pc) != list.end() || attempt > 64) {
                // The same instruction failed twice: interpret it.
                if (std::find(overrides.interpret.begin(), overrides.interpret.end(), retry.pc) != overrides.interpret.end()) {
                    if (attempt > 96) throw;
                    overrides.no_pins = true;
                } else {
                    overrides.interpret.push_back(retry.pc);
                }
            } else {
                list.push_back(retry.pc);
            }
        }
    }
}

} // namespace transmute::fx
