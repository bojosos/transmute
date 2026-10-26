#include "structurize.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

// Structurizer (owner: A6). See structurize.hpp.
namespace alchemy::msl::detail {
namespace {

using ir::BasicBlock;
using ir::Function;
using ir::Opcode;
using ir::Value;

Value* terminator(BasicBlock* block) { return block->instructions.back(); }

bool is_phi(const Value* v) { return v->kind == Value::Kind::instruction && v->op == Opcode::phi; }

class Structurizer {
public:
    Structurizer(ir::Module& module, Function& function) : m_(module), f_(function) {}

    StructuredCfg run() {
        lower_switches();
        normalise_branches();
        remove_unreachable();
        canonicalise_loops();
        duplicate_shared_tails();
        structure_selections();
        analyse();
        StructuredCfg result;
        result.order = rpo_;
        // Blocks no edge reaches (unreachable merges) follow their header.
        for (auto& [header, merge] : merges_) {
            for (auto* block : {merge.merge, merge.continue_target}) {
                if (!block || index_.count(block)) continue;
                auto at = std::find(result.order.begin(), result.order.end(), header);
                result.order.insert(at + 1, block);
                index_[block] = -1;
            }
        }
        result.merges = merges_;
        result.demoted = find_demotions();
        result.clone_of = clone_of_;
        return result;
    }

private:
    ir::Module& m_;
    Function& f_;
    std::map<BasicBlock*, Merge> merges_;
    std::vector<BasicBlock*> rpo_;
    std::unordered_map<BasicBlock*, int> index_;     // RPO index
    std::vector<int> idom_;                           // by RPO index
    std::uint32_t counter_{};
    std::uint64_t generation_{};                      // bumped on every CFG edit
    // ALCHEMY_MSL_STRUCTURIZE_TRACE: one stderr line per tail duplication / funnel.
    const bool trace_ = std::getenv("ALCHEMY_MSL_STRUCTURIZE_TRACE") != nullptr;
    void cfg_changed() {
        f_.compute_cfg();
        ++generation_;
    }

    // ---- IR editing ----------------------------------------------------------------------
    BasicBlock* new_block(const std::string& hint) {
        auto* block = m_.make<BasicBlock>();
        block->kind = Value::Kind::block;
        block->name = "alchemy." + hint + "." + std::to_string(counter_++);
        block->type = m_.primitive(ir::Type::Kind::label);
        block->function = &f_;
        f_.blocks.push_back(block);
        return block;
    }
    Value* new_instruction(BasicBlock* block, Opcode op, const ir::Type* type) {
        auto* v = m_.make<Value>();
        v->kind = Value::Kind::instruction;
        v->op = op;
        v->type = type;
        v->parent = block;
        return v;
    }
    void add_branch(BasicBlock* block, BasicBlock* target) {
        auto* br = new_instruction(block, Opcode::br, m_.primitive(ir::Type::Kind::void_));
        br->targets.push_back(target);
        block->instructions.push_back(br);
    }
    void add_conditional(BasicBlock* block, Value* condition, BasicBlock* yes, BasicBlock* no) {
        auto* br = new_instruction(block, Opcode::br, m_.primitive(ir::Type::Kind::void_));
        br->operands.push_back(condition);
        br->targets = {yes, no};
        block->instructions.push_back(br);
    }
    Value* add_compare(BasicBlock* block, Value* a, Value* b) {
        auto* compare = new_instruction(block, Opcode::icmp, m_.integer(1));
        compare->predicate = ir::Predicate::eq;
        compare->operands = {a, b};
        block->instructions.push_back(compare);
        return compare;
    }
    static void replace_target(BasicBlock* pred, BasicBlock* from, BasicBlock* to) {
        for (auto*& target : terminator(pred)->targets) if (target == from) target = to;
    }
    // Moves the phi entries of `block` coming from `from` to come from `to`.
    static void rename_incoming(BasicBlock* block, BasicBlock* from, BasicBlock* to) {
        for (auto* v : block->instructions) {
            if (!is_phi(v)) break;
            for (auto*& incoming : v->targets) if (incoming == from) incoming = to;
        }
    }

    // Routes the edges preds -> target through a new block; phi entries of
    // target from those preds merge into phis of the new block.
    BasicBlock* redirect(const std::vector<BasicBlock*>& preds, BasicBlock* target, const std::string& hint) {
        auto* block = new_block(hint);
        for (auto* pred : preds) replace_target(pred, target, block);
        std::vector<Value*> new_phis;
        for (auto* v : target->instructions) {
            if (!is_phi(v)) break;
            std::vector<Value*> values;
            std::vector<BasicBlock*> blocks;
            for (std::size_t i = 0; i < v->targets.size();) {
                if (std::find(preds.begin(), preds.end(), v->targets[i]) != preds.end()) {
                    values.push_back(v->operands[i]);
                    blocks.push_back(v->targets[i]);
                    v->operands.erase(v->operands.begin() + static_cast<std::ptrdiff_t>(i));
                    v->targets.erase(v->targets.begin() + static_cast<std::ptrdiff_t>(i));
                    continue;
                }
                ++i;
            }
            if (values.empty()) continue;
            Value* incoming = values[0];
            if (std::any_of(values.begin(), values.end(), [&](Value* x) { return x != values[0]; })) {
                auto* phi = new_instruction(block, Opcode::phi, v->type);
                phi->operands = values;
                phi->targets = blocks;
                new_phis.push_back(phi);
                incoming = phi;
            }
            v->operands.push_back(incoming);
            v->targets.push_back(block);
        }
        block->instructions = new_phis;
        add_branch(block, target);
        cfg_changed();
        return block;
    }

    // ---- preparation ------------------------------------------------------------------------
    void lower_switches() {
        const auto blocks = f_.blocks;
        for (auto* block : blocks) {
            auto* sw = terminator(block);
            if (sw->op != Opcode::switch_) continue;
            block->instructions.pop_back();
            auto* value = sw->operands[0];
            auto* fallback = sw->targets[0];
            auto* current = block;
            const auto cases = sw->operands.size() - 1;
            if (cases == 0) {
                add_branch(block, fallback);
                continue;
            }
            // Every target's phis take one value from `block` (LLVM repeats
            // the entry once per case edge, all with the same value). Take
            // those entries out; they come back once per chain block that
            // branches to the target.
            std::map<Value*, Value*> incoming;   // phi -> value from block
            std::set<BasicBlock*> targets(sw->targets.begin(), sw->targets.end());
            for (auto* target : targets) {
                for (auto* v : target->instructions) {
                    if (!is_phi(v)) break;
                    for (std::size_t i = 0; i < v->targets.size();) {
                        if (v->targets[i] != block) { ++i; continue; }
                        incoming.emplace(v, v->operands[i]);
                        v->operands.erase(v->operands.begin() + static_cast<std::ptrdiff_t>(i));
                        v->targets.erase(v->targets.begin() + static_cast<std::ptrdiff_t>(i));
                    }
                }
            }
            // One chain block per distinct case target, testing all of its
            // values at once (cases sharing a target would otherwise enter it
            // from several chain blocks, which a loop after it cannot take);
            // cases that go to the default target need no test.
            std::vector<std::pair<BasicBlock*, std::vector<Value*>>> groups;
            for (std::size_t c = 0; c < cases; ++c) {
                auto* target = sw->targets[c + 1];
                if (target == fallback) continue;
                auto found = std::find_if(groups.begin(), groups.end(), [&](auto& g) { return g.first == target; });
                if (found == groups.end()) groups.push_back({target, {sw->operands[c + 1]}});
                else found->second.push_back(sw->operands[c + 1]);
            }
            // An unreachable default (an exhaustive switch): the last case
            // needs no test. Otherwise its edge into a dead end would keep the
            // chain's selections from merging where the cases join.
            if (!groups.empty() && fallback->instructions.size() == 1 &&
                fallback->instructions.front()->op == Opcode::unreachable) {
                fallback = groups.back().first;
                groups.pop_back();
            }
            std::set<BasicBlock*> chain = {block};
            if (groups.empty()) add_branch(block, fallback);
            for (std::size_t g = 0; g < groups.size(); ++g) {
                auto* target = groups[g].first;
                Value* compare = nullptr;
                for (auto* constant : groups[g].second) {
                    auto* equal = add_compare(current, value, constant);
                    if (!compare) {
                        compare = equal;
                        continue;
                    }
                    auto* either = new_instruction(current, Opcode::or_, m_.integer(1));
                    either->operands = {compare, equal};
                    current->instructions.push_back(either);
                    compare = either;
                }
                BasicBlock* next = g + 1 < groups.size() ? new_block("switch") : fallback;
                add_conditional(current, compare, target, next);
                current = next;
                if (g + 1 < groups.size()) chain.insert(next);
            }
            cfg_changed();
            for (auto* target : targets) {
                for (auto* v : target->instructions) {
                    if (!is_phi(v)) break;
                    const auto found = incoming.find(v);
                    if (found == incoming.end()) throw std::logic_error("switch target phi without an entry for the switch");
                    std::set<BasicBlock*> added;
                    for (auto* pred : target->predecessors) {
                        if (!chain.count(pred) || !added.insert(pred).second) continue;
                        v->operands.push_back(found->second);
                        v->targets.push_back(pred);
                    }
                }
            }
        }
        cfg_changed();
    }

    void normalise_branches() {
        for (auto* block : f_.blocks) {
            auto* t = terminator(block);
            if (t->op == Opcode::br && t->targets.size() == 2 && t->targets[0] == t->targets[1]) {
                t->operands.clear();
                t->targets.pop_back();
            }
        }
        cfg_changed();
        // One phi entry per predecessor.
        for (auto* block : f_.blocks) {
            for (auto* v : block->instructions) {
                if (!is_phi(v)) break;
                std::vector<Value*> values;
                std::vector<BasicBlock*> blocks;
                for (std::size_t i = 0; i < v->targets.size(); ++i) {
                    if (std::find(blocks.begin(), blocks.end(), v->targets[i]) != blocks.end()) continue;
                    values.push_back(v->operands[i]);
                    blocks.push_back(v->targets[i]);
                }
                v->operands = values;
                v->targets = blocks;
            }
        }
    }

    void remove_unreachable() {
        std::set<BasicBlock*> reached;
        std::vector<BasicBlock*> stack = {f_.blocks.front()};
        while (!stack.empty()) {
            auto* b = stack.back();
            stack.pop_back();
            if (!reached.insert(b).second) continue;
            for (auto* s : b->successors) stack.push_back(s);
        }
        std::vector<BasicBlock*> kept;
        for (auto* b : f_.blocks) if (reached.count(b)) kept.push_back(b);
        for (auto* b : kept) {
            for (auto* v : b->instructions) {
                if (!is_phi(v)) break;
                for (std::size_t i = 0; i < v->targets.size();) {
                    if (!reached.count(v->targets[i])) {
                        v->operands.erase(v->operands.begin() + static_cast<std::ptrdiff_t>(i));
                        v->targets.erase(v->targets.begin() + static_cast<std::ptrdiff_t>(i));
                        continue;
                    }
                    ++i;
                }
            }
        }
        f_.blocks = kept;
        cfg_changed();
    }

    // ---- analyses -----------------------------------------------------------------------
    void analyse() {
        cfg_changed();
        rpo_.clear();
        index_.clear();
        std::set<BasicBlock*> visited;
        std::vector<BasicBlock*> post;
        std::vector<std::pair<BasicBlock*, std::size_t>> stack = {{f_.blocks.front(), 0}};
        visited.insert(f_.blocks.front());
        while (!stack.empty()) {
            auto& [block, next] = stack.back();
            if (next < block->successors.size()) {
                auto* s = block->successors[next++];
                if (visited.insert(s).second) stack.push_back({s, 0});
                continue;
            }
            post.push_back(block);
            stack.pop_back();
        }
        rpo_.assign(post.rbegin(), post.rend());
        for (std::size_t i = 0; i < rpo_.size(); ++i) index_[rpo_[i]] = static_cast<int>(i);
        // Cooper-Harvey-Kennedy dominators.
        idom_.assign(rpo_.size(), -1);
        idom_[0] = 0;
        bool changed = true;
        while (changed) {
            changed = false;
            for (std::size_t i = 1; i < rpo_.size(); ++i) {
                int candidate = -1;
                for (auto* pred : rpo_[i]->predecessors) {
                    const auto found = index_.find(pred);
                    if (found == index_.end() || idom_[found->second] < 0) continue;
                    candidate = candidate < 0 ? found->second : intersect(found->second, candidate);
                }
                if (candidate >= 0 && idom_[i] != candidate) {
                    idom_[i] = candidate;
                    changed = true;
                }
            }
        }
    }
    int intersect(int a, int b) const {
        while (a != b) {
            while (a > b) a = idom_[a];
            while (b > a) b = idom_[b];
        }
        return a;
    }
    bool dominates(BasicBlock* a, BasicBlock* b) const {
        const auto ia = index_.find(a), ib = index_.find(b);
        if (ia == index_.end() || ib == index_.end()) return false;
        int x = ib->second;
        while (true) {
            if (x == ia->second) return true;
            if (x == 0) return false;
            x = idom_[x];
        }
    }

    struct Loop {
        BasicBlock* header{};
        std::set<BasicBlock*> body;
    };
    std::vector<Loop> find_loops() {
        // Back edges by DFS; reject irreducible retreating edges.
        std::vector<Loop> loops;
        std::map<BasicBlock*, int> state;   // 1 on stack, 2 done
        std::map<BasicBlock*, std::vector<BasicBlock*>> latches;
        std::vector<std::pair<BasicBlock*, std::size_t>> stack = {{f_.blocks.front(), 0}};
        state[f_.blocks.front()] = 1;
        while (!stack.empty()) {
            auto& [block, next] = stack.back();
            if (next < block->successors.size()) {
                auto* s = block->successors[next++];
                const auto st = state[s];
                if (st == 0) {
                    state[s] = 1;
                    stack.push_back({s, 0});
                } else if (st == 1) {
                    if (!dominates(s, block))
                        throw std::runtime_error("irreducible control flow (a loop with several entries) is unsupported");
                    latches[s].push_back(block);
                }
                continue;
            }
            state[block] = 2;
            stack.pop_back();
        }
        for (auto& [header, sources] : latches) {
            Loop loop;
            loop.header = header;
            loop.body.insert(header);
            std::vector<BasicBlock*> work = sources;
            while (!work.empty()) {
                auto* b = work.back();
                work.pop_back();
                if (!loop.body.insert(b).second) continue;
                for (auto* p : b->predecessors) if (index_.count(p)) work.push_back(p);
            }
            loops.push_back(std::move(loop));
        }
        return loops;
    }

    // ---- loops ----------------------------------------------------------------------------
    void canonicalise_loops() {
        std::set<BasicBlock*> done;
        while (true) {
            analyse();
            auto loops = find_loops();
            std::sort(loops.begin(), loops.end(), [](const Loop& a, const Loop& b) { return a.body.size() < b.body.size(); });
            const Loop* pick = nullptr;
            for (const auto& loop : loops) if (!done.count(loop.header)) { pick = &loop; break; }
            if (!pick) return;
            done.insert(pick->header);
            canonicalise(*pick);
        }
    }

    void canonicalise(const Loop& loop) {
        auto* header = loop.header;
        // 1. Single continue block holding the only back edge.
        std::vector<BasicBlock*> latches;
        for (auto* p : header->predecessors) if (loop.body.count(p)) latches.push_back(p);
        auto* continue_block = redirect(latches, header, "continue");
        // 2. Header holds only phis and an unconditional branch to the body.
        auto* body_entry = new_block("body");
        auto first = std::find_if(header->instructions.begin(), header->instructions.end(), [](Value* v) { return !is_phi(v); });
        body_entry->instructions.assign(first, header->instructions.end());
        header->instructions.erase(first, header->instructions.end());
        for (auto* v : body_entry->instructions) v->parent = body_entry;
        for (auto* s : header->successors) rename_incoming(s, header, body_entry);
        add_branch(header, body_entry);
        cfg_changed();
        // 3. Exits through one merge block.
        analyse();
        std::set<BasicBlock*> body = loop.body;
        body.insert(continue_block);
        body.insert(body_entry);
        // Blocks created inside the body by inner loops are dominated by the header.
        for (auto* b : rpo_) if (dominates(header, b) && reaches(b, continue_block, header)) body.insert(b);
        std::vector<std::pair<BasicBlock*, BasicBlock*>> exits;
        for (auto* b : body)
            for (auto* s : b->successors)
                if (!body.count(s)) exits.push_back({b, s});
        std::sort(exits.begin(), exits.end(), [&](auto& a, auto& b) {
            return std::pair(index_[a.first], index_[a.second]) < std::pair(index_[b.first], index_[b.second]); });
        std::vector<BasicBlock*> targets;
        for (auto& [from, to] : exits) if (std::find(targets.begin(), targets.end(), to) == targets.end()) targets.push_back(to);
        BasicBlock* merge = nullptr;
        if (targets.empty()) {
            merge = new_block("unreachable");
            auto* u = new_instruction(merge, Opcode::unreachable, m_.primitive(ir::Type::Kind::void_));
            merge->instructions.push_back(u);
        } else if (targets.size() == 1 && std::all_of(targets[0]->predecessors.begin(), targets[0]->predecessors.end(),
                                                       [&](BasicBlock* p) { return body.count(p) != 0; }) &&
                   !claimed(targets[0])) {
            merge = targets[0];
        } else if (targets.size() == 1) {
            std::vector<BasicBlock*> sources;
            for (auto& [from, to] : exits) sources.push_back(from);
            merge = redirect(sources, targets[0], "merge");
        } else {
            merge = funnel(exits, targets, "merge");
        }
        merges_[header] = {Merge::Kind::loop, merge, continue_block};
        cfg_changed();
    }

    // Routes the edges `exits` (from -> to, every `to` in `targets`) through
    // one new block: one edge block per edge, a selector phi in the new block
    // and a dispatch chain from it to the original targets (whose phis take
    // their values through it). Returns the new block.
    BasicBlock* funnel(const std::vector<std::pair<BasicBlock*, BasicBlock*>>& exits,
                       const std::vector<BasicBlock*>& targets, const std::string& hint) {
        BasicBlock* merge = nullptr;
        {
            merge = new_block(hint);
            auto* selector = new_instruction(merge, Opcode::phi, m_.integer(32));
            merge->instructions.push_back(selector);
            std::map<BasicBlock*, std::vector<std::pair<BasicBlock*, BasicBlock*>>> edges_by_target;   // target -> (edge block, from)
            for (auto& [from, to] : exits) {
                auto* edge = redirect({from}, to, "exit");
                // edge -> merge instead of edge -> to.
                const auto position = std::find(targets.begin(), targets.end(), to) - targets.begin();
                terminator(edge)->targets[0] = merge;
                selector->operands.push_back(m_.constant_int(m_.integer(32), static_cast<std::uint64_t>(position)));
                selector->targets.push_back(edge);
                edges_by_target[to].push_back({edge, from});
            }
            cfg_changed();
            // Phis of each target: their entries from edge blocks become merge
            // phis, fed through the dispatch block that reaches the target.
            std::vector<BasicBlock*> dispatch(targets.size(), nullptr);
            BasicBlock* current = merge;
            for (std::size_t t = 0; t < targets.size(); ++t) {
                dispatch[t] = current;
                if (t + 1 == targets.size()) {
                    add_branch(current, targets[t]);
                    break;
                }
                auto* compare = add_compare(current, selector, m_.constant_int(m_.integer(32), t));
                auto* rest = new_block("dispatch");
                add_conditional(current, compare, targets[t], rest);
                current = rest;
            }
            for (std::size_t t = 0; t < targets.size(); ++t) {
                auto* target = targets[t];
                for (auto* v : target->instructions) {
                    if (!is_phi(v)) break;
                    auto* through = new_instruction(merge, Opcode::phi, v->type);
                    for (std::size_t i = 0; i < v->targets.size();) {
                        const auto& edges = edges_by_target[target];
                        const bool from_edge = std::any_of(edges.begin(), edges.end(),
                            [&](auto& e) { return e.first == v->targets[i]; });
                        if (!from_edge) { ++i; continue; }
                        through->operands.push_back(v->operands[i]);
                        through->targets.push_back(v->targets[i]);
                        v->operands.erase(v->operands.begin() + static_cast<std::ptrdiff_t>(i));
                        v->targets.erase(v->targets.begin() + static_cast<std::ptrdiff_t>(i));
                    }
                    // Edge blocks that go to other targets contribute undef.
                    for (auto* edge_block : selector->targets) {
                        if (std::find(through->targets.begin(), through->targets.end(), edge_block) != through->targets.end()) continue;
                        through->operands.push_back(m_.special(Value::Kind::constant_undef, v->type));
                        through->targets.push_back(edge_block);
                    }
                    merge->instructions.insert(merge->instructions.begin() + 1, through);
                    v->operands.push_back(through);
                    v->targets.push_back(dispatch[t]);
                }
            }
            cfg_changed();
        }
        return merge;
    }

    // A loop header `header` entered from inside a selection construct and
    // from outside it (clang threads a branch so that two arms of nested
    // selections reach the same loop, the others bypassing it): a loop
    // cannot be duplicated, so every entry into the loop and every edge that
    // bypasses it to the block where its paths rejoin (`rejoin`, the first
    // block after the loop entered from elsewhere) go through one funnel
    // block that dispatches to the loop or to `rejoin`. The selections then
    // merge at the funnel, and the funnel's own selection contains the loop.
    std::uint32_t funnels_{};
    bool funnel_loop_entry(BasicBlock* header) {
        // Funnels can feed further duplication; a bounded number keeps a CFG
        // this does not structure from looping.
        if (++funnels_ > 64) { if (trace_) std::fprintf(stderr, "alchemy-msl structurize: no funnel for %s (line %d)\n", header->name.c_str(), __LINE__); return false; }
        analyse();
        const auto found_loop = merges_.find(header);
        BasicBlock* continue_target = found_loop != merges_.end() ? found_loop->second.continue_target : nullptr;
        const auto bodies = loop_bodies();
        std::set<BasicBlock*> region;
        BasicBlock* outer = nullptr;
        std::size_t size = ~std::size_t{0};
        for (auto& [h, body] : bodies)
            if (h != header && body.count(header) && body.size() < size) { outer = h; size = body.size(); }
        if (outer) {
            region = bodies.at(outer);
            region.erase(outer);
            region.erase(merges_.at(outer).continue_target);
        } else {
            region.insert(rpo_.begin(), rpo_.end());
        }
        // The selection that contains every entry: the nearest common
        // dominator of the entries that heads a two-way branch in the region.
        std::vector<BasicBlock*> entries;
        for (auto* p : header->predecessors)
            if (p != continue_target) entries.push_back(p);
        if (entries.empty()) { if (trace_) std::fprintf(stderr, "alchemy-msl structurize: no funnel for %s (line %d)\n", header->name.c_str(), __LINE__); return false; }
        int common = index_.at(entries.front());
        for (auto* p : entries) common = intersect(common, index_.at(p));
        BasicBlock* selection = rpo_[static_cast<std::size_t>(common)];
        while (true) {
            auto* t = terminator(selection);
            if (region.count(selection) && t->op == Opcode::br && t->targets.size() == 2 && !merges_.count(selection)) break;
            if (common == 0 || !region.count(selection))
                { if (trace_) std::fprintf(stderr, "alchemy-msl structurize: no funnel for %s (line %d)\n", header->name.c_str(), __LINE__); return false; }
            common = idom_[static_cast<std::size_t>(common)];
            selection = rpo_[static_cast<std::size_t>(common)];
        }
        // Its merge: the paths that do not enter the loop meet the loop's
        // paths there.
        BasicBlock* rejoin = selection_merge(selection, region);
        if (!rejoin || rejoin == header) { if (trace_) std::fprintf(stderr, "alchemy-msl structurize: no funnel for %s (line %d)\n", header->name.c_str(), __LINE__); return false; }
        // The loop's side: blocks reached from the header before the rejoin.
        std::set<BasicBlock*> loop_side;
        {
            std::vector<BasicBlock*> work = {header};
            while (!work.empty()) {
                auto* b = work.back();
                work.pop_back();
                if (b == rejoin || !region.count(b) || !loop_side.insert(b).second) continue;
                for (auto* s : b->successors) work.push_back(s);
            }
        }
        // The selection's other side: blocks reached from it before the loop
        // and the rejoin; their edges into the rejoin are funnelled too.
        std::set<BasicBlock*> other_side;
        {
            std::vector<BasicBlock*> work = {selection};
            while (!work.empty()) {
                auto* b = work.back();
                work.pop_back();
                if (b == rejoin || b == header || !region.count(b) || loop_side.count(b) || !other_side.insert(b).second)
                    continue;
                for (auto* s : b->successors) work.push_back(s);
            }
        }
        // Every edge from the other side into the loop's side (the loop
        // header, and blocks after the loop that the other side also
        // reaches) or into the rejoin goes through the funnel.
        std::vector<std::pair<BasicBlock*, BasicBlock*>> edges;
        std::vector<BasicBlock*> targets = {header};
        for (auto* p : entries)
            if (!other_side.count(p)) edges.push_back({p, header});
        for (auto* u : other_side) {
            for (auto* s : u->successors) {
                if (s != rejoin && !loop_side.count(s)) continue;
                edges.push_back({u, s});
                if (std::find(targets.begin(), targets.end(), s) == targets.end()) targets.push_back(s);
            }
        }
        std::sort(edges.begin(), edges.end(), [&](auto& a, auto& b) {
            return std::pair(index_[a.first], index_[a.second]) < std::pair(index_[b.first], index_[b.second]); });
        funnel(edges, targets, "funnel");
        return true;
    }

    // True when `from` reaches `to` without passing `stop`.
    bool reaches(BasicBlock* from, BasicBlock* to, BasicBlock* stop) const {
        std::set<BasicBlock*> seen;
        std::vector<BasicBlock*> work = {from};
        while (!work.empty()) {
            auto* b = work.back();
            work.pop_back();
            if (b == to) return true;
            if (b == stop || !seen.insert(b).second) continue;
            for (auto* s : b->successors) work.push_back(s);
        }
        return false;
    }

    bool claimed(BasicBlock* block) const {
        for (auto& [header, merge] : merges_)
            if (merge.merge == block || merge.continue_target == block) return true;
        return false;
    }

    // ---- selections -------------------------------------------------------------------------
    // The loop (header) whose body most tightly contains `block`.
    BasicBlock* innermost_loop(BasicBlock* block, const std::map<BasicBlock*, std::set<BasicBlock*>>& bodies) const {
        BasicBlock* best = nullptr;
        std::size_t size = ~std::size_t{0};
        for (auto& [header, body] : bodies) {
            if (body.count(block) && body.size() < size) {
                best = header;
                size = body.size();
            }
        }
        return best;
    }

    std::map<BasicBlock*, std::set<BasicBlock*>> loop_bodies() {
        std::map<BasicBlock*, std::set<BasicBlock*>> bodies;
        for (auto& [header, merge] : merges_) {
            if (merge.kind != Merge::Kind::loop) continue;
            std::set<BasicBlock*> body;
            for (auto* b : rpo_) if (dominates(header, b) && !dominates(merge.merge, b) && b != merge.merge) body.insert(b);
            bodies[header] = std::move(body);
        }
        return bodies;
    }

    // Post-dominator sets of a region (exits are virtual), computed once per
    // (region, CFG generation): the tail duplication and selection passes ask
    // for the immediate post-dominator of every header of a region.
    struct PostDominators {
        std::uint64_t generation{~std::uint64_t{0}};
        std::set<BasicBlock*> region;
        std::vector<BasicBlock*> nodes;
        std::unordered_map<BasicBlock*, int> id;
        std::size_t words{};
        std::vector<std::vector<std::uint64_t>> pdom;   // bit k: nodes[k] (bit n: the virtual exit)
        std::vector<int> count;                         // set bits per node
    };
    std::vector<PostDominators> pdom_cache_;

    const PostDominators& post_dominators(const std::set<BasicBlock*>& region) {
        for (auto& cached : pdom_cache_)
            if (cached.generation == generation_ && cached.region == region) return cached;
        if (pdom_cache_.size() >= 8) pdom_cache_.erase(pdom_cache_.begin());
        PostDominators p;
        p.generation = generation_;
        p.region = region;
        // Successors before predecessors (descending RPO index) for a fast
        // backward fixed point; blocks without an index last.
        p.nodes.assign(region.begin(), region.end());
        std::sort(p.nodes.begin(), p.nodes.end(), [&](BasicBlock* a, BasicBlock* b) {
            const auto ia = index_.find(a), ib = index_.find(b);
            const int x = ia == index_.end() ? -1 : ia->second, y = ib == index_.end() ? -1 : ib->second;
            return x != y ? x > y : a < b;
        });
        const int n = static_cast<int>(p.nodes.size());
        for (int i = 0; i < n; ++i) p.id[p.nodes[static_cast<std::size_t>(i)]] = i;
        p.words = static_cast<std::size_t>(n + 1 + 63) / 64;
        p.pdom.assign(static_cast<std::size_t>(n), std::vector<std::uint64_t>(p.words, ~std::uint64_t{0}));
        // Early returns do not decide merges: a conditional edge into a block
        // that ends the function (`if (c) return;`, discard) is left out when
        // the block has another way on, so the selection merges where the
        // paths that keep going rejoin. Otherwise every header would merge at
        // the shared return block and the rest of the function would be
        // duplicated per early return (the return block itself is duplicated
        // instead, which is cheap).
        std::vector<std::vector<int>> successors(static_cast<std::size_t>(n));
        std::vector<bool> exits(static_cast<std::size_t>(n), false);
        for (int i = 0; i < n; ++i) {
            auto* b = p.nodes[static_cast<std::size_t>(i)];
            std::vector<BasicBlock*> list;
            for (auto* s : b->successors) if (!s->successors.empty()) list.push_back(s);
            if (list.empty()) list = b->successors;
            bool leaves = b->successors.empty();
            for (auto* s : list) {
                const auto found = p.id.find(s);
                if (found == p.id.end()) leaves = true;
                else successors[static_cast<std::size_t>(i)].push_back(found->second);
            }
            exits[static_cast<std::size_t>(i)] = leaves;
        }
        std::vector<std::uint64_t> next(p.words);
        bool changed = true;
        while (changed) {
            changed = false;
            for (int i = 0; i < n; ++i) {
                std::fill(next.begin(), next.end(), ~std::uint64_t{0});
                bool any = false;
                if (exits[static_cast<std::size_t>(i)]) {
                    std::fill(next.begin(), next.end(), 0);
                    next[static_cast<std::size_t>(n) / 64] |= std::uint64_t{1} << (n % 64);
                    any = true;
                }
                for (const int s : successors[static_cast<std::size_t>(i)]) {
                    const auto& ps = p.pdom[static_cast<std::size_t>(s)];
                    for (std::size_t w = 0; w < p.words; ++w) next[w] &= ps[w];
                    any = true;
                }
                if (!any) std::fill(next.begin(), next.end(), 0);
                next[static_cast<std::size_t>(i) / 64] |= std::uint64_t{1} << (i % 64);
                if (next != p.pdom[static_cast<std::size_t>(i)]) {
                    p.pdom[static_cast<std::size_t>(i)] = next;
                    changed = true;
                }
            }
        }
        p.count.resize(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            int bits = 0;
            for (int k = 0; k < n; ++k)
                bits += static_cast<int>((p.pdom[static_cast<std::size_t>(i)][static_cast<std::size_t>(k) / 64] >> (k % 64)) & 1);
            p.count[static_cast<std::size_t>(i)] = bits;
        }
        pdom_cache_.push_back(std::move(p));
        return pdom_cache_.back();
    }

    // Immediate post-dominator of `block` in `region` (exits are virtual):
    // the strict post-dominator post-dominated by all the others, i.e. the
    // one with the most post-dominators (they form a chain).
    BasicBlock* region_ipdom(BasicBlock* block, const std::set<BasicBlock*>& region) {
        const auto& p = post_dominators(region);
        const auto found = p.id.find(block);
        if (found == p.id.end()) return nullptr;
        const int me = found->second;
        const auto& mine = p.pdom[static_cast<std::size_t>(me)];
        const int n = static_cast<int>(p.nodes.size());
        int best = -1;
        for (int k = 0; k < n; ++k) {
            if (k == me || !((mine[static_cast<std::size_t>(k) / 64] >> (k % 64)) & 1)) continue;
            if (best < 0 || p.count[static_cast<std::size_t>(k)] > p.count[static_cast<std::size_t>(best)]) best = k;
        }
        return best < 0 ? nullptr : p.nodes[static_cast<std::size_t>(best)];
    }

    // The merge of the selection headed by `block`: its immediate
    // post-dominator in the region, unless that is a block ending the
    // function (or none), as when one arm leaves through a chain of blocks
    // into the shared return block: then the first block reachable from the
    // header that the header does not dominate (where the arm that keeps
    // going rejoins code from outside) becomes the merge, through a join
    // block, instead of duplicating everything after it per early exit.
    // True when `merge` is the merge of a construct that contains `block`.
    bool encloses_with_merge(BasicBlock* merge, BasicBlock* block) {
        for (auto& [header, m] : merges_)
            if (m.merge == merge && header != block && dominates(header, block) && !dominates(merge, block))
                return true;
        return false;
    }

    BasicBlock* selection_merge(BasicBlock* block, const std::set<BasicBlock*>& region) {
        auto* merge = region_ipdom(block, region);
        // Inside a loop, a missing merge comes from breaks, which the loop handles.
        const bool function_level = !rpo_.empty() && region.count(rpo_.front()) != 0;
        if (!function_level || (merge && !merge->successors.empty())) return merge;
        BasicBlock* best = nullptr;
        std::set<BasicBlock*> seen;
        std::vector<BasicBlock*> work(block->successors.begin(), block->successors.end());
        while (!work.empty()) {
            auto* b = work.back();
            work.pop_back();
            if (b == merge || !region.count(b) || !seen.insert(b).second) continue;
            // An enclosing construct's merge bounds this one (keeps nesting).
            if (!dominates(block, b) || encloses_with_merge(b, block)) {
                if (!b->successors.empty() && index_.count(b) &&
                    (!best || index_.at(b) < index_.at(best)))
                    best = b;
                continue;
            }
            for (auto* s : b->successors) work.push_back(s);
        }
        return best ? best : merge;
    }

    void structure_selections() {
        analyse();
        const auto order = rpo_;
        for (auto* block : order) {
            if (merges_.count(block)) continue;
            auto* t = terminator(block);
            if (t->op != Opcode::br || t->targets.size() != 2) continue;
            analyse();
            const auto bodies = loop_bodies();
            auto* loop = innermost_loop(block, bodies);
            auto special = [&](BasicBlock* target) {
                if (!loop) return false;
                const auto& merge = merges_.at(loop);
                return target == merge.merge || target == merge.continue_target;
            };
            if (special(t->targets[0]) || special(t->targets[1])) continue;
            std::set<BasicBlock*> region;
            if (loop) {
                region = bodies.at(loop);
                region.erase(loop);
                region.erase(merges_.at(loop).continue_target);
            } else {
                region.insert(rpo_.begin(), rpo_.end());
            }
            auto* merge = selection_merge(block, region);
            if (!merge) {
                merge = new_block("unreachable");
                merge->instructions.push_back(new_instruction(merge, Opcode::unreachable, m_.primitive(ir::Type::Kind::void_)));
                cfg_changed();
            } else if (claimed(merge) || !dominates(block, merge) || merges_.count(merge)) {
                std::vector<BasicBlock*> preds;
                // Not a back edge into the merge (a loop header right after
                // the selection: its continue block keeps branching to it).
                for (auto* p : merge->predecessors)
                    if (dominates(block, p) && !dominates(merge, p)) preds.push_back(p);
                merge = redirect(preds, merge, "join");
            }
            merges_[block] = {Merge::Kind::selection, merge, nullptr};
        }
    }

    // ---- tail duplication ----------------------------------------------------------------
    std::map<Value*, Value*> clone_of_;
    Value* original(Value* v) const {
        const auto found = clone_of_.find(v);
        return found == clone_of_.end() ? v : found->second;
    }

    // The selection region of `block` (its innermost loop body, or the whole
    // function); false when the block heads no selection.
    bool selection_region(BasicBlock* block, const std::map<BasicBlock*, std::set<BasicBlock*>>& bodies,
                          std::set<BasicBlock*>& region) {
        if (merges_.count(block)) return false;
        auto* t = terminator(block);
        if (t->op != Opcode::br || t->targets.size() != 2) return false;
        auto* loop = innermost_loop(block, bodies);
        if (loop) {
            const auto& merge = merges_.at(loop);
            for (auto* target : t->targets)
                if (target == merge.merge || target == merge.continue_target) return false;
            region = bodies.at(loop);
            region.erase(loop);
            region.erase(merge.continue_target);
        } else {
            region.clear();
            region.insert(rpo_.begin(), rpo_.end());
        }
        return true;
    }

    // A selection's construct is every block reachable from its header
    // before its merge; structured control flow needs the header to
    // dominate all of them. A block entered from outside as well is copied
    // for the edges inside the construct, until no such block remains.
    // Headers before the one that changed last stay as they were (a copy is
    // made for edges inside a construct, so it stays inside every enclosing
    // construct): each round resumes there, and a final full pass checks.
    void duplicate_shared_tails() {
        BasicBlock* resume = nullptr;
        for (int round = 0; round < 8192; ++round) {
            analyse();
            const auto bodies = loop_bodies();
            bool changed = false;
            bool skipping = resume != nullptr && index_.count(resume);
            for (auto* block : rpo_) {
                if (skipping && block != resume) continue;
                skipping = false;
                std::set<BasicBlock*> region;
                if (!selection_region(block, bodies, region)) continue;
                auto* merge = selection_merge(block, region);
                // The construct: blocks reachable from the header before the
                // merge. A block in it entered from outside (not dominated by
                // the header, or with a predecessor outside the construct,
                // such as a return block the merge's side reaches too) is
                // shared.
                std::set<BasicBlock*> visited;
                std::vector<BasicBlock*> stack(block->successors.begin(), block->successors.end());
                std::vector<BasicBlock*> construct;
                while (!stack.empty()) {
                    auto* b = stack.back();
                    stack.pop_back();
                    if (b == merge || !region.count(b) || !visited.insert(b).second) continue;
                    construct.push_back(b);
                    for (auto* s : b->successors) stack.push_back(s);
                }
                BasicBlock* shared = nullptr;
                for (auto* b : construct) {
                    bool outside = !dominates(block, b);
                    for (auto* p : b->predecessors) outside = outside || (p != block && !visited.count(p));
                    if (outside && (!shared || index_.at(b) < index_.at(shared))) shared = b;
                }
                if (!shared) continue;
                std::vector<BasicBlock*> preds;
                for (auto* p : shared->predecessors)
                    if (p == block || (visited.count(p) && p != shared)) preds.push_back(p);
                const auto found = merges_.find(shared);
                if (trace_) std::fprintf(stderr, "alchemy-msl structurize: round %d header %s shared %s%s\n", round, block->name.c_str(), shared->name.c_str(), found != merges_.end() ? " (loop)" : "");
                // A loop cannot be copied, and copying a long tail per
                // construct multiplies code (switch chains whose cases share
                // code): those are funnelled instead.
                std::size_t tail = 0;
                {
                    std::set<BasicBlock*> seen;
                    std::vector<BasicBlock*> work = {shared};
                    while (!work.empty() && tail <= 6) {
                        auto* b = work.back();
                        work.pop_back();
                        if (!visited.count(b) || !seen.insert(b).second) continue;
                        ++tail;
                        for (auto* s : b->successors) work.push_back(s);
                    }
                }
                const bool loop = found != merges_.end() && found->second.kind == Merge::Kind::loop;
                if (!(loop || tail > 6) || !funnel_loop_entry(shared)) {
                    if (loop) throw std::runtime_error("unstructured entry into a loop (" + shared->name + ")");
                    duplicate(shared, preds);
                }
                changed = true;
                resume = block;
                break;
            }
            if (!changed) {
                if (!resume) return;
                resume = nullptr;   // the final full pass
            }
        }
        throw std::runtime_error("control flow needs too much tail duplication to be structured");
    }

    void duplicate(BasicBlock* block, const std::vector<BasicBlock*>& preds) {
        if (merges_.count(block)) throw std::runtime_error("unstructured entry into a loop (" + block->name + ")");
        auto* copy = new_block("tail");
        std::unordered_map<Value*, Value*> map;
        for (auto* v : block->instructions) {
            if (v->op == Opcode::alloca) throw std::runtime_error("cannot duplicate a block holding an alloca");
            auto* c = m_.make<Value>();
            *c = *v;
            c->parent = copy;
            if (!c->name.empty()) c->name += ".tail" + std::to_string(counter_);
            map[v] = c;
            clone_of_[c] = original(v);
            copy->instructions.push_back(c);
        }
        auto from_preds = [&](BasicBlock* b) { return std::find(preds.begin(), preds.end(), b) != preds.end(); };
        auto remap = [&](std::vector<Value*>& operands) {
            for (auto*& operand : operands) {
                const auto found = map.find(operand);
                if (found != map.end()) operand = found->second;
            }
        };
        for (std::size_t i = 0; i < block->instructions.size(); ++i) {
            auto* v = block->instructions[i];
            auto* c = copy->instructions[i];
            if (is_phi(v)) {
                // The copy keeps the entries from `preds`, the original the rest.
                std::vector<Value*> keep_ops, copy_ops;
                std::vector<BasicBlock*> keep_targets, copy_targets;
                for (std::size_t k = 0; k < v->targets.size(); ++k) {
                    const bool mine = from_preds(v->targets[k]);
                    (mine ? copy_ops : keep_ops).push_back(v->operands[k]);
                    (mine ? copy_targets : keep_targets).push_back(v->targets[k]);
                }
                v->operands = keep_ops;
                v->targets = keep_targets;
                c->operands = copy_ops;
                c->targets = copy_targets;
            }
            remap(c->operands);
        }
        for (auto* pred : preds) replace_target(pred, block, copy);
        // Successor phis get an entry for the copy.
        const std::set<BasicBlock*> successors(block->successors.begin(), block->successors.end());
        for (auto* successor : successors) {
            for (auto* phi : successor->instructions) {
                if (!is_phi(phi)) break;
                const auto count = phi->targets.size();
                for (std::size_t k = 0; k < count; ++k) {
                    if (phi->targets[k] != block) continue;
                    auto* operand = phi->operands[k];
                    const auto found = map.find(operand);
                    phi->operands.push_back(found != map.end() ? found->second : operand);
                    phi->targets.push_back(copy);
                }
            }
        }
        cfg_changed();
    }

    // ---- dominance repair ----------------------------------------------------------------
    // A tail-duplicated copy is demoted as its original (clone_of): every copy
    // stores into the one Function variable the lowering keys by the original.
    std::set<Value*> find_demotions() {
        analyse();
        std::set<Value*> demoted;
        auto check = [&](Value* def, BasicBlock* use_block) {
            if (!def || def->kind != Value::Kind::instruction || !def->parent) return;
            if (def->parent == use_block) return;
            if (!dominates(def->parent, use_block)) demoted.insert(original(def));
        };
        for (auto* block : rpo_) {
            for (auto* v : block->instructions) {
                if (is_phi(v)) {
                    for (std::size_t i = 0; i < v->operands.size(); ++i) check(v->operands[i], v->targets[i]);
                    continue;
                }
                for (auto* operand : v->operands) check(operand, block);
            }
        }
        return demoted;
    }
};

} // namespace

StructuredCfg structurize(ir::Module& module, ir::Function& function) {
    return Structurizer(module, function).run();
}

} // namespace alchemy::msl::detail
