// Tier-2 region-wide analyses and register assignment at block entries
// (owner: translator core). See RegionEmitter (region_emitter.hpp) and
// NOTES_CORE.md, "Tier 2".
#include "fx/translator/region_emitter.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace transmute::fx::core {

namespace {

constexpr std::uint64_t kAll = ~std::uint64_t{0};

std::uint64_t pack(std::uint32_t gpr, std::uint32_t vec) { return gpr | (static_cast<std::uint64_t>(vec) << 32); }

bool is_add_sub(Kind k) { return k == Kind::add_sub_imm || k == Kind::add_sub_shift || k == Kind::add_sub_ext; }

} // namespace

// Loop depth, interrupt checks covered by other headers, and liveness.
void RegionEmitter::compute_tier2() {
    const auto n = static_cast<std::int32_t>(plan_.insts.size());
    const auto& insts = plan_.insts;

    // Loop depth: layout ranges [header, last back-edge source] (s_.loop_end).
    s_.depth.assign(static_cast<std::size_t>(n), 0);
    {
        std::vector<std::int32_t> delta(static_cast<std::size_t>(n) + 1, 0);
        for (std::int32_t h = 0; h < n; ++h)
            if (const auto e = s_.loop_end[static_cast<std::size_t>(h)]; e >= 0) {
                ++delta[static_cast<std::size_t>(h)];
                --delta[static_cast<std::size_t>(e) + 1];
            }
        std::int32_t level = 0;
        for (std::int32_t i = 0; i < n; ++i) {
            level += delta[static_cast<std::size_t>(i)];
            s_.depth[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(std::clamp(level, 0, 255));
        }
    }

    // Interrupt checks (Plan::t2_checks): every cycle of the region passes a
    // loop header; a header H needs no check of its own when another header
    // H2 that keeps its check lies on every path from H to each back edge
    // into H (each cycle through H then contains H2). H2 is then locked: it
    // keeps its check. Exits leave the region (a region start checks).
    s_.no_check.assign(static_cast<std::size_t>(n), 0);
    if (plan_.t2_checks && options_.interrupt_checks && !plan_.count_loops) {
        std::vector<std::uint8_t> locked(static_cast<std::size_t>(n), 0);
        std::vector<std::uint32_t> seen(static_cast<std::size_t>(n), 0);
        std::vector<std::int32_t> stack;
        std::uint32_t stamp = 0;
        unsigned budget = 4u << 20;   // bounds the analysis on huge regions (successor visits)
        for (std::int32_t h = 0; h < n && budget != 0; ++h) {
            if (!insts[static_cast<std::size_t>(h)].loop_header) continue;
            const auto end = s_.loop_end[static_cast<std::size_t>(h)];
            if (locked[static_cast<std::size_t>(h)]) continue;
            for (std::int32_t h2 = h + 1; h2 <= end && budget != 0; ++h2) {
                const auto& p2 = insts[static_cast<std::size_t>(h2)];
                if (!p2.loop_header || s_.no_check[static_cast<std::size_t>(h2)]) continue;
                // Does a path from h reach a back-edge source of h without passing h2?
                ++stamp;
                bool escapes = false;
                stack.clear();
                stack.push_back(h);
                seen[static_cast<std::size_t>(h)] = stamp;
                while (!stack.empty() && !escapes && budget != 0) {
                    const auto i = stack.back();
                    stack.pop_back();
                    plan_.for_each_successor(i, [&](std::int32_t s) {
                        --budget;
                        if (s == h && i >= h) escapes = true;   // a back edge into h reached without h2
                        if (s == h2 || seen[static_cast<std::size_t>(s)] == stamp) return;
                        seen[static_cast<std::size_t>(s)] = stamp;
                        stack.push_back(s);
                    });
                }
                if (escapes || budget == 0) continue;
                s_.no_check[static_cast<std::size_t>(h)] = 1;
                locked[static_cast<std::size_t>(h2)] = 1;
                break;
            }
        }
        if (budget == 0) std::fill(s_.no_check.begin(), s_.no_check.end(), std::uint8_t{0});
    }

    // Pin lending (see "pin lending" below): innermost loops first, ranges
    // that do not overlap; no switch dispatch in the region (its multiway
    // edges have no stubs to lend or return pins).
    s_.lent.assign(static_cast<std::size_t>(n), 0);
    if (plan_.t2_lend && plan_.tables.empty() && (plan_.pinned | plan_.vpinned) != 0) {
        struct Loop {
            std::int32_t h, e;
        };
        std::vector<Loop> loops;
        for (std::int32_t h = 0; h < n; ++h)
            if (const auto e = s_.loop_end[static_cast<std::size_t>(h)]; e >= 0) loops.push_back({h, e});
        std::stable_sort(loops.begin(), loops.end(), [](const Loop& a, const Loop& b) { return a.e - a.h < b.e - b.h; });
        std::vector<std::uint8_t> chosen(static_cast<std::size_t>(n), 0);
        const auto free_g = static_cast<unsigned>(std::popcount(0x0f07u & ~plan_.pin_hosts));
        const auto free_v = static_cast<unsigned>(std::popcount(0xffffu & ~plan_.vpin_hosts));
        for (const auto& l : loops) {
            bool skip = false;
            std::uint32_t ref = 0, vref = 0;
            for (std::int32_t i = l.h; i <= l.e && !skip; ++i) {
                const auto& pi = insts[static_cast<std::size_t>(i)];
                skip = chosen[static_cast<std::size_t>(i)] || pi.flow == Flow::interpret || pi.conservative_writes;
                ref |= pi.reads | pi.writes;
                vref |= pi.vreads | pi.vwrites;
            }
            // No loop may cross the range's boundary (each iteration of it would
            // lend and return the pins): every other loop overlapping it lies
            // inside it or contains it.
            for (const auto& o : loops)
                if (o.h <= l.e && o.e >= l.h && !(o.h >= l.h && o.e <= l.e) && !(o.h <= l.h && o.e >= l.e)) skip = true;
            if (skip) continue;
            std::uint64_t lend = 0;
            if (static_cast<unsigned>(std::popcount(ref & ~plan_.pinned)) + 1 > free_g) lend |= plan_.pinned & ~ref;
            if (static_cast<unsigned>(std::popcount(vref & ~plan_.vpinned)) > free_v)
                lend |= static_cast<std::uint64_t>(plan_.vpinned & ~vref) << 32;
            // Diagnostics: TRANSMUTE_FX_TRACE_LEND=1 prints each loop's decision.
            static const bool trace = std::getenv("TRANSMUTE_FX_TRACE_LEND") != nullptr;
            if (trace)
                std::fprintf(stderr, "lend %llx..%llx: refs %08x/%08x pinned %08x/%08x free %u/%u -> %016llx\n",
                             static_cast<unsigned long long>(insts[static_cast<std::size_t>(l.h)].pc),
                             static_cast<unsigned long long>(insts[static_cast<std::size_t>(l.e)].pc), ref, vref, plan_.pinned,
                             plan_.vpinned, free_g, free_v, static_cast<unsigned long long>(lend));
            if (lend == 0) continue;
            for (std::int32_t i = l.h; i <= l.e; ++i) {
                chosen[static_cast<std::size_t>(i)] = 1;
                s_.lent[static_cast<std::size_t>(i)] = lend;
            }
        }
    }

    // Liveness (backward, to a fixed point).
    s_.olive_in.assign(static_cast<std::size_t>(n), 0);
    s_.olive_out.assign(static_cast<std::size_t>(n), 0);
    s_.rlive_in.assign(static_cast<std::size_t>(n), 0);
    if (!dead_on_ && !plan_.t2_alloc) return;
    for (bool changed = true; changed;) {
        changed = false;
        for (std::int32_t i = n - 1; i >= 0; --i) {
            const auto& pi = insts[static_cast<std::size_t>(i)];
            bool exits = false;
            switch (pi.flow) {
            case Flow::normal: exits = pi.fall < 0; break;
            case Flow::jump: exits = pi.taken < 0; break;
            case Flow::cond: exits = pi.taken < 0 || pi.fall < 0; break;
            default: exits = true; break;   // calls, BR/RET (a switch's fallback too), traps, SVC, interpret exits
            }
            std::uint64_t rout = 0, oout = 0;
            plan_.for_each_successor(i, [&](std::int32_t s) {
                rout |= s_.rlive_in[static_cast<std::size_t>(s)];
                oout |= s_.olive_in[static_cast<std::size_t>(s)];
            });
            if (exits) rout = oout = kAll;
            const auto reads = pack(pi.reads, pi.vreads);
            const auto writes = pack(pi.writes, pi.vwrites);
            const auto rin = reads | (rout & ~writes);
            // Observation points: a fault (the state before the instruction),
            // unknown family usage, an interrupt check at the block start, an
            // ADD/SUB whose result an adjacent access may fold (a fault there
            // reports the ADD's pc with the state before the ADD).
            bool obs = pi.may_fault || pi.conservative_writes || pi.flow == Flow::interpret ||
                       (pi.loop_header && options_.interrupt_checks && !s_.no_check[static_cast<std::size_t>(i)]);
            if (!obs && is_add_sub(pi.inst.kind) && i + 1 < n && insts[static_cast<std::size_t>(i) + 1].may_fault &&
                !insts[static_cast<std::size_t>(i) + 1].leader)
                obs = true;
            const auto oin = obs ? kAll : reads | (oout & ~writes);
            auto& ri = s_.rlive_in[static_cast<std::size_t>(i)];
            auto& oi = s_.olive_in[static_cast<std::size_t>(i)];
            auto& oo = s_.olive_out[static_cast<std::size_t>(i)];
            if (rin != ri || oin != oi || oout != oo) {
                ri = rin;
                oi = oin;
                oo = oout;
                changed = true;
            }
        }
    }
}

// ---- pin lending -------------------------------------------------------------------
// Pins are region-wide; inside a loop that never uses a pinned register
// (reads or writes it), that pin's host serves the register cache instead
// (Plan::t2_lend): the value goes to the context on every edge into the loop
// and comes back into its host on every edge out of it (and before exits:
// convention registers like evicted ones). Fault maps name lent registers as
// in the context. Loops are chosen innermost first, without overlapping
// ranges, where the loop references more unpinned registers of that class
// than the cache has hosts.

void RegionEmitter::set_lent(std::uint64_t lent) {
    lent_ = lent;
    lent_hosts_ = lent_vhosts_ = 0;
    for (auto m = lent; m; m &= m - 1) {
        const auto b = static_cast<unsigned>(std::countr_zero(m));
        if (b < 32) lent_hosts_ |= 1u << static_cast<unsigned>(plan_.pin_host[b]);
        else lent_vhosts_ |= 1u << static_cast<unsigned>(plan_.vpin_host[b - 32]);
    }
}

std::uint64_t RegionEmitter::cache_hosts_at(std::int32_t index) const {
    std::uint32_t gpr = 0x0f07u & ~plan_.pin_hosts;   // r8-r11, rcx, rdx, rax (kScratchMask)
    std::uint32_t vec = 0xffffu & ~plan_.vpin_hosts;
    for (auto m = lent_at(index); m; m &= m - 1) {
        const auto b = static_cast<unsigned>(std::countr_zero(m));
        if (b < 32) gpr |= 1u << static_cast<unsigned>(plan_.pin_host[b]);
        else vec |= 1u << static_cast<unsigned>(plan_.vpin_host[b - 32]);
    }
    return gpr | (static_cast<std::uint64_t>(vec) << 32);
}

void RegionEmitter::restrict_hosts(RegCache& c, std::uint32_t hosts) {
    for (auto m = c.mask & ~hosts; m; m &= m - 1) drop_entry(c, static_cast<unsigned>(std::countr_zero(m)));
}

void RegionEmitter::lend_enter(std::uint64_t from, std::uint64_t to) {
    for (auto m = to & ~from; m; m &= m - 1) {
        const auto b = static_cast<unsigned>(std::countr_zero(m));
        if (b < 32) a_.mov(64, gmem(b), x64::gp(static_cast<unsigned>(plan_.pin_host[b])));
        else vmove(x64::ptr(x64::Gp::r15, v_disp(b - 32)), x64::xmm(static_cast<unsigned>(plan_.vpin_host[b - 32])));
    }
}

void RegionEmitter::lend_leave(std::uint64_t from, std::uint64_t to) {
    for (auto m = from & ~to; m; m &= m - 1) {
        const auto b = static_cast<unsigned>(std::countr_zero(m));
        if (b < 32) a_.mov(64, x64::gp(static_cast<unsigned>(plan_.pin_host[b])), gmem(b));
        else vmove(x64::xmm(static_cast<unsigned>(plan_.vpin_host[b - 32])), x64::ptr(x64::Gp::r15, v_disp(b - 32)));
    }
}

bool RegionEmitter::observed(unsigned g, bool vec) const {
    if (!dead_on_ || !cur_) return true;
    const auto mask = in_instruction_ ? s_.olive_in[static_cast<std::size_t>(index_)]
                                      : s_.olive_out[static_cast<std::size_t>(after_last_)];
    return ((mask >> (g + (vec ? 32u : 0u))) & 1u) != 0;
}

std::uint64_t RegionEmitter::dead_at(std::int32_t target) const {
    if (!dead_on_ || target < 0) return 0;
    return ~s_.olive_in[static_cast<std::size_t>(target)];
}

// Pass 1: the cache as it reaches each internal successor of `from` that
// starts a block (only `only`, if given), before any store or reconcile.
void RegionEmitter::record_edges(std::int32_t from, std::int32_t only) {
    if (alloc_pass_ != 1 || alias_count_ != 0 || galias_count_ != 0) return;
    std::int32_t snap = -1;
    const auto add = [&](std::int32_t to) {
        if (to < 0 || !plan_.insts[static_cast<std::size_t>(to)].leader || (only >= 0 && to != only)) return;
        if (snap < 0) snap = push_snap();
        s_.edge_recs.push_back({from, to, snap});
    };
    if (only >= 0) add(only);
    else plan_.for_each_successor(from, add);
}

// Between the passes: the entry state of every block that pass 1 could not
// take from a single predecessor laid out before it (merge points, loop
// headers, blocks only reached by back edges). Each guest register goes to
// the host it occupies on the heaviest incoming edges (weight 8^loop depth of
// the edge's source), if those edges carry at least half the block's
// incoming weight and its value is still read there (or, dirty, still
// observed: keeping it avoids a store). Dirty if dirty on at least half of
// that weight. rax never (edge flag materialization clobbers it).
void RegionEmitter::build_presets() {
    const auto n = static_cast<std::int32_t>(plan_.insts.size());
    auto& recs = s_.edge_recs;
    s_.preset_of.assign(static_cast<std::size_t>(n), -1);
    s_.presets.clear();
    std::stable_sort(recs.begin(), recs.end(), [](const Scratch::EdgeRec& a, const Scratch::EdgeRec& b) { return a.to < b.to; });
    const auto weight = [&](std::int32_t from) -> std::uint32_t {
        return 1u << (3u * std::min<unsigned>(s_.depth[static_cast<std::size_t>(from)], 6u));
    };
    std::uint32_t score[32][16], dscore[32][16];
    for (std::size_t a = 0; a < recs.size();) {
        std::size_t b = a;
        while (b < recs.size() && recs[b].to == recs[a].to) ++b;
        const auto t = recs[a].to;
        const auto& pt = plan_.insts[static_cast<std::size_t>(t)];
        const bool need = pt.loop_header || pt.first_pred >= t;
        if (need) {
            std::uint32_t total = 0;
            for (auto k = a; k < b; ++k) total += weight(recs[k].from);
            CacheSnapshot e;
            reset_cache(e.gpr);
            reset_cache(e.vec);
            for (const bool vec : {false, true}) {
                std::memset(score, 0, sizeof score);
                std::memset(dscore, 0, sizeof dscore);
                for (auto k = a; k < b; ++k) {
                    const auto& snap = s_.cache_snaps[static_cast<std::size_t>(recs[k].snap)];
                    const auto& c = vec ? snap.vec : snap.gpr;
                    const auto w = weight(recs[k].from);
                    for (auto m = c.mask; m; m &= m - 1) {
                        const auto h = static_cast<unsigned>(std::countr_zero(m));
                        const auto g = static_cast<unsigned>(c.guest[h]);
                        score[g][h] += w;
                        if ((c.dirty >> h) & 1u) dscore[g][h] += w;
                    }
                }
                const unsigned shift = vec ? 32u : 0u;
                auto rlive = static_cast<std::uint32_t>(s_.rlive_in[static_cast<std::size_t>(t)] >> shift);
                // Like tier 1's loop-header rule: registers the header's block
                // reads before writing them (keeping everything live there, or
                // what the loop reads, added pressure and measured worse),
                // nearest first; at most all but two of the cache's hosts.
                std::int32_t first_read[32];
                std::fill(std::begin(first_read), std::end(first_read), 1 << 20);
                {
                    std::uint32_t wr = 0, seen = 0;
                    for (std::int32_t j = t; j < n && j < t + 32; ++j) {
                        const auto& pj = plan_.insts[static_cast<std::size_t>(j)];
                        if (pj.flow == Flow::interpret || pj.conservative_writes) break;
                        for (auto m = (vec ? pj.vreads : pj.reads) & ~wr & ~seen; m; m &= m - 1)
                            first_read[std::countr_zero(m)] = j - t;
                        seen |= (vec ? pj.vreads : pj.reads) & ~wr;
                        wr |= vec ? pj.vwrites : pj.writes;
                        if (pj.flow != Flow::normal || (j + 1 < n && plan_.insts[static_cast<std::size_t>(j) + 1].leader)) break;
                    }
                    rlive &= seen;
                }
                struct Cand {
                    std::uint32_t score;
                    std::uint8_t g, h;
                };
                Cand cands[64];
                unsigned nc = 0;
                for (unsigned g = 0; g < 32; ++g) {
                    unsigned best = 16;
                    for (unsigned h = 0; h < 16; ++h)
                        if (score[g][h] != 0 && (best == 16 || score[g][h] > score[g][best])) best = h;
                    if (best == 16 || (!vec && best == 0)) continue;
                    if (2u * score[g][best] < total) continue;
                    if (!((rlive >> g) & 1u) || nc == 64) continue;   // not read there before written
                    cands[nc++] = {score[g][best], static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(best)};
                }
                std::stable_sort(cands, cands + nc, [&](const Cand& x, const Cand& y) {
                    return x.score != y.score ? x.score > y.score : first_read[x.g] < first_read[y.g];
                });
                auto& out = vec ? e.vec : e.gpr;
                const auto hosts = static_cast<std::uint32_t>(cache_hosts_at(t) >> shift);   // pin lending
                const auto cap = std::max(1, std::popcount(hosts) - 2);
                for (unsigned k = 0; k < nc && std::popcount(out.mask) < cap; ++k) {
                    const auto g = cands[k].g, h = cands[k].h;
                    if (out.host[g] >= 0 || out.guest[h] >= 0 || !((hosts >> h) & 1u)) continue;
                    out.host[g] = static_cast<std::int8_t>(h);
                    out.guest[h] = static_cast<std::int8_t>(g);
                    out.used[h] = 0;
                    out.mask |= 1u << h;
                    if (2u * dscore[g][h] >= score[g][h]) out.dirty |= 1u << h;
                }
            }
            // Diagnostics: TRANSMUTE_FX_TRACE_PRESETS=1 prints every chosen entry state.
            static const bool trace = std::getenv("TRANSMUTE_FX_TRACE_PRESETS") != nullptr;
            if (trace) {
                std::fprintf(stderr, "preset %llx (%u edges, total %u):", static_cast<unsigned long long>(pt.pc),
                             static_cast<unsigned>(b - a), total);
                for (unsigned h = 0; h < 16; ++h)
                    if ((e.gpr.mask >> h) & 1u) std::fprintf(stderr, " x%d@%u%s", e.gpr.guest[h], h, ((e.gpr.dirty >> h) & 1u) ? "*" : "");
                for (unsigned h = 0; h < 16; ++h)
                    if ((e.vec.mask >> h) & 1u) std::fprintf(stderr, " v%d@%u%s", e.vec.guest[h], h, ((e.vec.dirty >> h) & 1u) ? "*" : "");
                for (auto k = a; k < b; ++k) {
                    const auto& snap = s_.cache_snaps[static_cast<std::size_t>(recs[k].snap)];
                    std::fprintf(stderr, " | from %llx w%u:", static_cast<unsigned long long>(plan_.insts[static_cast<std::size_t>(recs[k].from)].pc),
                                 weight(recs[k].from));
                    for (unsigned h = 0; h < 16; ++h)
                        if ((snap.gpr.mask >> h) & 1u) std::fprintf(stderr, " x%d@%u", snap.gpr.guest[h], h);
                }
                std::fputc('\n', stderr);
            }
            s_.preset_of[static_cast<std::size_t>(t)] = static_cast<std::int32_t>(s_.presets.size());
            s_.presets.push_back(e);
        }
        a = b;
    }
}

} // namespace transmute::fx::core

namespace transmute::fx::core {

// ---- GPR copy aliases --------------------------------------------------------------------
// MOV Xd, Xn into an unpinned register, Xn in a cache register: no code
// (region_emitter.hpp, "GPR copy aliases").
bool RegionEmitter::emit_gpr_copy(const PlanInst& pi) {
    const auto& d = pi.inst;
    if (d.kind != Kind::logic_shift || d.op != 1 || d.s || d.invert || !d.sf || d.rn != 31 || d.amount != 0)
        return false;
    const unsigned dst = d.rd, src = d.rm;
    if (dst == 31 || src == 31 || dst == src || pinned(dst) || pinned(src) || !may_cache() || pi.conservative_writes)
        return false;
    const int h = gc_.host[src] >= 0 ? gc_.host[src] : galias_host_[src];
    if (h < 0) return false;   // Xn only in its slot: an ordinary copy
    gc_.used[h] = ++cache_clock_;
    if (galias_host_[dst] == h || gc_.host[dst] == h) return true;   // Xd already names that value
    const auto bit = 1u << dst;
    if (const auto old = galias_host_[dst]; old >= 0) {   // Xd's previous value is overwritten
        galias_of_[old] &= ~bit;
        galias_host_[dst] = -1;
        --galias_count_;
        galias_dirty_ &= ~bit;
    }
    if (const auto hd = gc_.host[dst]; hd >= 0) {
        if (const auto heirs = galias_of_[hd]) {
            // An alias of Xd's host takes the entry over (dirty iff it was).
            const auto heir = static_cast<unsigned>(std::countr_zero(heirs));
            const bool dirty = ((galias_dirty_ >> heir) & 1u) != 0;
            galias_of_[hd] &= ~(1u << heir);
            galias_host_[heir] = -1;
            galias_dirty_ &= ~(1u << heir);
            --galias_count_;
            gc_.host[dst] = -1;
            gc_.host[heir] = hd;
            gc_.guest[hd] = static_cast<std::int8_t>(heir);
            if (dirty) gc_.dirty |= 1u << hd;
            else gc_.dirty &= ~(1u << hd);
        } else {
            drop_entry(gc_, static_cast<unsigned>(hd));   // a dirty old value is dead (MOV does not fault)
        }
    }
    galias_host_[dst] = static_cast<std::int8_t>(h);
    galias_of_[h] |= bit;
    galias_dirty_ |= bit;
    ++galias_count_;
    fault_reopen();
    return true;
}

std::uint32_t RegionEmitter::galias_touched() const {
    std::uint32_t m = 0;
    for (unsigned h = 0; h < 16; ++h)
        if (galias_of_[h]) m |= galias_of_[h] | (gc_.guest[h] >= 0 ? 1u << static_cast<unsigned>(gc_.guest[h]) : 0u);
    return m;
}

void RegionEmitter::galias_drop_host(unsigned host) {
    for (auto m = galias_of_[host]; m; m &= m - 1) {
        const auto g = static_cast<unsigned>(std::countr_zero(m));
        if (((galias_dirty_ >> g) & 1u) && observed(g, false)) {
            if (a_.control_flow_count() != cf_mark_)   // the store would run on one path only
                throw RetryTranslation{pc_, false, RetryTranslation::Resource::aliases};
            a_.mov(64, gmem(g), x64::gp(host));
        }
        galias_host_[g] = -1;
        galias_dirty_ &= ~(1u << g);
        --galias_count_;
    }
    galias_of_[host] = 0;
    fault_reopen();
}

// Every alias becomes an entry of its own (a copy into a free cache register,
// as the MOV would have made) or, with no register free, is stored if dirty.
void RegionEmitter::galias_resolve() {
    if (galias_count_ == 0) return;
    if (a_.control_flow_count() != cf_mark_) throw RetryTranslation{pc_, false, RetryTranslation::Resource::aliases};
    for (unsigned h = 0; h < 16; ++h) {
        for (auto m = galias_of_[h]; m; m &= m - 1) {
            const auto g = static_cast<unsigned>(std::countr_zero(m));
            const bool dirty = ((galias_dirty_ >> g) & 1u) != 0;
            galias_host_[g] = -1;
            galias_dirty_ &= ~(1u << g);
            --galias_count_;
            const auto spare = static_cast<std::uint32_t>(cache_hosts_at(index_)) & free_gpr_ & ~gc_.mask & ~1u;   // not rax
            if (spare != 0) {
                const auto nh = static_cast<unsigned>(std::countr_zero(spare));
                a_.mov(64, x64::gp(nh), x64::gp(h));
                gc_.host[g] = static_cast<std::int8_t>(nh);
                gc_.guest[nh] = static_cast<std::int8_t>(g);
                gc_.used[nh] = ++cache_clock_;
                gc_.mask |= 1u << nh;
                if (dirty) gc_.dirty |= 1u << nh;
            } else if (dirty && observed(g, false)) {
                a_.mov(64, gmem(g), x64::gp(h));
            }
        }
        galias_of_[h] = 0;
    }
    fault_reopen();
}

} // namespace transmute::fx::core
