#pragma once

// Structured control flow for the MSL compiler (owner: A6). Rewrites an
// entry wrapper's CFG (clang -O2 output: reducible, otherwise unstructured)
// into the form SPIR-V's structured control flow rules accept and records
// the merge instruction every header needs:
//   - switch terminators become chains of conditional branches;
//   - every natural loop gets a header holding only its phis, a single
//     continue block (the only back edge) and a single merge block that all
//     exits pass through (with a selector dispatching to the original exit
//     targets);
//   - blocks shared by two selection constructs that do not nest (clang
//     sinks common tails, e.g. two paths ending in the same store and
//     return) are duplicated per construct (tail duplication);
//   - every other conditional branch gets a selection merge: its immediate
//     post-dominator within the enclosing loop body, split off into a fresh
//     block when another construct already uses it; branches whose second
//     target is a break or continue need none.
// Irreducible control flow is rejected. Values whose definition no longer
// dominates a use are reported for demotion to Function variables.

#include "llvm_ir.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace alchemy::msl::detail {

struct Merge {
    enum class Kind { none, selection, loop } kind{Kind::none};
    ir::BasicBlock* merge{};
    ir::BasicBlock* continue_target{};
};

struct StructuredCfg {
    std::vector<ir::BasicBlock*> order;              // emission order (reverse post-order)
    std::map<ir::BasicBlock*, Merge> merges;         // by header block
    std::set<ir::Value*> demoted;                    // values that need a Function variable
    // Instructions of duplicated tail blocks -> the instruction they copy.
    // A copy of a demoted value stores into the original's variable.
    std::map<ir::Value*, ir::Value*> clone_of;
};

// Throws std::runtime_error naming the problem for unsupported control flow.
[[nodiscard]] StructuredCfg structurize(ir::Module& module, ir::Function& function);

} // namespace alchemy::msl::detail
