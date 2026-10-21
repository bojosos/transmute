#pragma once

// Translator public API (owner: translator core; additive changes only).
// Used by the runtime (JIT on miss), the cache (persistence) and AOT.

#include "transmute/fx/context.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace transmute::fx {

// Source of guest instruction words. The JIT reads host memory directly; AOT
// reads a mapped image that is not executing.
class CodeSource {
public:
    virtual ~CodeSource() = default;
    // Reads the 32-bit word at `address`. False if it is not readable guest code.
    virtual bool read(std::uint64_t address, std::uint32_t& word) const = 0;
    // Bounds [begin, end) of the function containing `address` (e.g. .pdata),
    // if known. Regions do not cross function bounds when they are known.
    virtual bool function_bounds(std::uint64_t address, std::uint64_t& begin,
                                 std::uint64_t& end) const {
        (void)address; (void)begin; (void)end;
        return false;
    }
    // True if another region already provides an entry for `pc` (e.g. AOT
    // roots inside big functions). Region formation then stops there and
    // leaves through a linkable direct exit instead of translating the
    // already covered code again. Never consulted for the region start.
    virtual bool has_entry(std::uint64_t pc) const {
        (void)pc;
        return false;
    }
};

enum class FixupKind : std::uint8_t {
    // 8-byte little-endian immediate at `offset` holding guest absolute
    // address `value` (branch targets, ADR/ADRP results, literal addresses).
    guest_address_imm64 = 0,
    // 8-byte immediate at `offset` that is a guest *constant* (e.g. a fused
    // MOVZ/MOVK value), not an address: never relocated. Recorded so the
    // cache's unfixed-address audit can tell constants from missing fixups.
    literal_imm64 = 1,
};

struct CodeFixup {
    std::uint32_t offset;
    FixupKind kind;
    std::uint64_t value;
};

// Entry point that expects the flushed state (everything in GuestContext,
// except the registers of translator_register_convention(), which are in
// their host registers).
struct RegionEntry {
    std::uint64_t guest_pc;
    std::uint32_t offset;
};

// Unlinked direct branch: rel32 field at `rel32_offset` (4-byte aligned)
// initially jumps to the region's own unlinked tail. The runtime may patch it
// to the entry of the region translated for `target_pc`.
struct RegionExit {
    std::uint32_t rel32_offset;
    std::uint64_t target_pc;
};

enum class FlagsLocation : std::uint8_t {
    context = 0,        // ctx->nzcv
    host_sub = 1,       // EFLAGS with FlagsProducer::sub meaning
    host_add = 2,
    host_logic = 3,
    host_register = 4,  // ARM-layout NZCV in host GPR `flags_register`
};

// Host code range whose faults belong to guest instruction `guest_pc`. Guest
// registers are recovered from pins (host registers) and GuestContext.
struct FaultSite {
    std::uint32_t host_begin;
    std::uint32_t host_end;
    std::uint64_t guest_pc;
    FlagsLocation flags;
    std::uint8_t flags_register;
    // (additive) 1-based index into TranslatedRegion::fault_maps, 0 = none:
    // guest registers whose current value is in a host register at this site
    // (write-back register cache), applied after and overriding the pins.
    std::uint16_t map{};
};

// Guest register held in a host register throughout the region body
// (between entry loads and exit flushes).
struct PinnedRegister {
    std::uint8_t guest;     // 0..31 (31 = SP) or vector 0..31
    std::uint8_t host;      // x64 register number; kFaultMapContext in a fault map
    bool vector;
};

// (additive) Fault map entries: TranslatedRegion::fault_map_regs[begin, begin + count).
// An entry with host == kFaultMapContext says the guest register's value is
// in GuestContext at that site (a region pin of it does not apply).
struct FaultMap {
    std::uint32_t begin;
    std::uint32_t count;
};
inline constexpr std::uint8_t kFaultMapContext = 0xff;

struct TranslationStats {
    std::uint32_t guest_instructions{};
    std::uint32_t interpreted_instructions{};
    std::uint32_t blocks{};
    // (additive) Code layout: bytes [0, hot_bytes) are the region body; the
    // cold section after it holds exit stubs, unlinked exit tails, rare
    // lookups and entry stubs, constant pools and switch tables (0 = unknown:
    // treat the whole code as body). Diagnostics only (hot-footprint audits).
    std::uint32_t hot_bytes{};
};

struct TranslatedRegion {
    std::uint64_t guest_start{};
    // Guest byte ranges [begin, end) whose contents this code depends on.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> guest_ranges;
    // Copy to 16-byte aligned code memory, then apply `fixups` (identity in
    // the JIT: values are already absolute).
    std::vector<std::uint8_t> code;
    std::vector<RegionEntry> entries;   // entries[0].guest_pc == guest_start
    std::vector<RegionExit> exits;
    std::vector<CodeFixup> fixups;
    std::vector<FaultSite> faults;      // sorted by host_begin
    std::vector<PinnedRegister> pins;
    TranslationStats stats;
    std::vector<FaultMap> fault_maps;            // (additive) FaultSite::map - 1 indexes these
    std::vector<PinnedRegister> fault_map_regs;
    // (additive) Tiering. tier 0: a provisional first-tier region (formation
    // stopped early, TranslateOptions::lazy) that counts its region-start
    // entries; tier 1: final (complete, or translated eagerly). The runtime
    // replaces a tier-0 region by a tier-1 translation once it gets hot.
    std::uint8_t tier{1};
    // Offset of the rel32 of the entry counter `dec dword [rip + rel32]`
    // (tier 0 only; 0 = none). It initially addresses `tier_counter_slot`, a
    // 4-byte slot at the end of the code; the runtime may repoint it at a
    // counter of its own (never adjacent to code) and choose the threshold.
    // When the counter reaches zero the region exits with ExitReason::tier_up.
    std::uint32_t tier_counter{};
    std::uint32_t tier_counter_slot{};
};

// (additive) Guest register state at fault site `site` of `region`: calls
// fn(guest, host, vector) for every guest register whose value is in a host
// register there (region pins, then the site's fault map, which overrides
// them). Registers not reported are in GuestContext.
template <class Fn>
void for_each_fault_register(const std::vector<PinnedRegister>& pins, const FaultSite& site,
                             const std::vector<FaultMap>& maps, const std::vector<PinnedRegister>& map_regs, Fn&& fn) {
    std::uint32_t over_g = 0, over_v = 0;
    const PinnedRegister* begin = nullptr;
    const PinnedRegister* end = nullptr;
    if (site.map != 0 && site.map <= maps.size()) {
        const FaultMap& m = maps[site.map - 1u];
        if (m.begin <= map_regs.size() && m.count <= map_regs.size() - m.begin) {
            begin = map_regs.data() + m.begin;
            end = begin + m.count;
        }
    }
    for (auto* p = begin; p != end; ++p) (p->vector ? over_v : over_g) |= 1u << (p->guest & 31);
    for (const auto& p : pins)
        if (!(((p.vector ? over_v : over_g) >> (p.guest & 31)) & 1u)) fn(p.guest, p.host, p.vector);
    for (auto* p = begin; p != end; ++p)
        if (p->host != kFaultMapContext) fn(p->guest, p->host, p->vector);
}

struct TranslateOptions {
    unsigned max_instructions = 4096;  // per region
    bool interrupt_checks = true;      // check ctx->interrupt_pending at back-edges
    bool profile = false;              // emit execution counters
    int tier = 0;
    // NZCV is treated as dead at BL/BLR and RET (AAPCS64: undefined on entry
    // to and return from a public interface), so it is not materialized for
    // those exits. false = strict model (flags live at every exit), e.g. for
    // differential tests of random, non-ABI-conforming programs.
    bool abi_flags = true;
    // (additive) First-tier (JIT) region formation: at most
    // `lazy_max_instructions` are translated and, with `lazy_split_calls`,
    // BL/BLR continuations are not followed (a continuation becomes its own
    // region when a call returns to it; the RSB entry is a linkable direct
    // exit). A region cut short this way is tier 0 (TranslatedRegion::tier)
    // with an entry counter; the runtime retranslates it without `lazy` once
    // it is hot. None of the formation options below are part of the
    // persistent-cache key: regions formed either way are interchangeable.
    bool lazy = false;
    unsigned lazy_max_instructions = 4096;
    bool lazy_split_calls = false;
    // (additive) Initial value of a tier-0 region's own counter slot.
    unsigned tier_threshold = 4096;
    // (additive) Synthetic function bounds [pc - window_backward, pc +
    // window_forward) for code whose CodeSource has no function bounds (e.g.
    // Mach-O images without .pdata). Without bounds a region follows every
    // direct branch, tail call and call continuation up to
    // max_instructions, so regions started at different functions cover the
    // same code again and again. 0 = unbounded.
    std::uint32_t window_forward = 0;
    std::uint32_t window_backward = 0;
    // (additive) Tier-2 (optimizing) code generation for hot regions. With
    // the tier on, first translations (`lazy`) that contain loops are tier 0
    // and count their loops' iterations at the loop headers (one counter per
    // region; 16 x tier_threshold iterations, then ExitReason::tier_up_loop);
    // regions without loops are final. The eager translation they tier up to
    // is optimized (region_alloc.cpp, docs/FX_ENGINE.md). -1 =
    // TRANSMUTE_FX_CORE (tier2 / notier2) decides, 0 = off, 1 = on, 2 = on
    // with one per-thread loop counter and tier-up at the header (t2loops),
    // 3 = on, counting region entries instead (t2entries, the prototype's
    // policy). Optimized and plain regions are interchangeable.
    std::int8_t optimize = -1;
};

// Translates the region starting at `guest_pc`. Undecodable or unsupported
// instructions become interpret exits; never throws for them. Throws
// std::runtime_error only if `guest_pc` itself cannot be read.
TranslatedRegion translate_region(std::uint64_t guest_pc, const CodeSource& code,
                                  const TranslateOptions& options = {});
// (additive) Same, into `out`, reusing its vectors' capacity (the JIT keeps one
// per thread: fewer allocations per region).
void translate_region(std::uint64_t guest_pc, const CodeSource& code, const TranslateOptions& options,
                      TranslatedRegion& out);

// Identity of the registered instruction families (FNV-1a over their names in
// registration order). Translations depend on it: include it in persistent
// cache keys so regions translated with other families are not reused.
std::uint64_t translator_family_fingerprint();

// (additive) Cross-region register convention. Guest GPRs in `guest_mask`
// live in host register host[g] (host callee-saved) at every boundary
// between pieces of translated code: region entries, exits to translated
// code (links, RSB returns, inline lookups), and the runtime stubs translated
// code jumps to. There, GuestContext::x[g] is stale: stubs that leave
// translated code for C++ (dispatch exit, link, lookup miss, return to host)
// store them first, `enter` and the stubs that enter translated code from
// the flushed state (stub_indirect hits, stub_host_return) load them, and
// host entries (thunks, callbacks) are reached through an adapter that
// stores them. Inside a region a convention register is a pin in host[g]
// (listed in TranslatedRegion::pins), or evicted: stored at the region's
// entries and reloaded at its exits while host[g] holds another pin.
// Fixed for the process (read once; folded into the translator fingerprint):
// TRANSMUTE_FX_GLOBALS=<list> (e.g. sp,x19,x20,x29,x30; "none") or
// TRANSMUTE_FX_CORE=noglobals (none: every boundary is the flushed state).
struct RegisterConvention {
    std::uint32_t guest_mask{};   // bit g = Xg (31 = SP)
    std::int8_t host[32]{};       // host GPR id of each convention register, else -1
};
const RegisterConvention& translator_register_convention();

// Family registration entry points, called once by executables/tests that
// want the family (each owned by its family component).
void register_fp_emitters();
void register_neon_emitters();
void register_system_emitters();

} // namespace transmute::fx
