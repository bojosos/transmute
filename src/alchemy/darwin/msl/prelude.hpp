#pragma once

// Precompiled preludes (owner: A6). Applications generate every library from
// one large common prelude followed by the shader's own code (Blender: about
// 50 KB of mtl_shader_defines/GLSL compatibility code ahead of each shader).
// Parsing that prelude is most of each library's clang time, so the IR
// compile precompiles it once:
//   - the rewriter reports split points (rewriter.hpp SplitPoint): line
//     starts of translation-unit-scope declarations;
//   - each library's prefixes up to its split points are hashed; the longest
//     prefix with a precompiled prelude on disk is used: clang compiles only
//     the rest, with -include-pch of the prelude (which includes metal_stdlib);
//   - otherwise the longest prefix (at least 16 KB) shared with a library
//     compiled earlier by this process is precompiled, once, and used from
//     then on by every library starting with exactly those bytes.
// PCHs live in <stdlib dir>/preludes/p-<configuration>-<length>-<hash>.pch
// (built from stdin, so they depend on no other file than the stdlib),
// persist across processes (like the metal_stdlib PCH) and are evicted
// oldest first beyond 32 per configuration. The IR is the same as compiling
// the whole source (tested on the corpus). ALCHEMY_MSL_PRELUDE=0 disables.

#include "alchemy/msl/compiler.hpp"
#include "frontend.hpp"
#include "rewriter.hpp"

#include <cstdint>

namespace alchemy::msl::detail {

// compile_to_ir for a rewritten library, through a precompiled prelude when
// one applies (falls back to the whole source when clang refuses it).
[[nodiscard]] ClangOutput compile_rewritten(const RewriteResult& rewritten, const CompileOptions& options);

struct PreludeStatistics {
    std::uint64_t hits{};       // compiles that used a precompiled prelude
    std::uint64_t misses{};     // compiles of the whole source
    std::uint64_t built{};      // preludes precompiled by this process
    std::uint64_t rejected{};   // preludes clang refused (then not used again)
};
[[nodiscard]] PreludeStatistics prelude_statistics();
// Forgets the preludes this process knows (tests): the next compile lists
// the directory again and has no earlier libraries to share a prefix with.
void reset_preludes();
// Tests: compile whole sources (false) or use preludes (true, the default
// unless ALCHEMY_MSL_PRELUDE=0).
void set_preludes_enabled(bool enabled);

} // namespace alchemy::msl::detail
