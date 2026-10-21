#pragma once

// In-process C preprocessor for MSL sources (owner: A6). It replaces the
// `clang -E` child process on the success path, so a library costs one clang
// launch (the IR compile) instead of two. Its output has the shape of
// `clang -E` output that the rewriter and the IR compile rely on: one output
// line per source line, `# N "file"` markers for #line, #pragma lines kept.
//
// It is deliberately strict: anything it does not implement exactly as clang
// does (an active #error or #warning, #include of anything but the metal
// headers, __has_include and friends, _Pragma, __COUNTER__, __VA_OPT__, named
// variadic parameters, digraphs, raw strings, invalid pastes, malformed
// directives or expressions) makes it decline with a reason, and the caller
// runs `clang -E`, which also produces the diagnostics for broken sources.
// alchemy_msl_tests preprocessor compares its output token by token (with
// line numbers) against `clang -E` on the fixtures and the corpus.

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace alchemy::msl::detail {

// Macro definitions parsed from `#define` lines (`clang -dM -E` output).
class MacroSet;
[[nodiscard]] std::shared_ptr<const MacroSet> parse_macro_set(std::string_view defines);
[[nodiscard]] std::size_t macro_count(const MacroSet& set);

struct InHouseOutput {
    bool handled{};          // false: run clang -E instead (`reason` says why)
    std::string output;
    std::string reason;
};

// `builtins`: clang's predefined and command-line macros; `stdlib`: what
// #include <metal_stdlib> (or any metal_* / simd header) adds; `user`:
// MTLCompileOptions.preprocessorMacros (NAME=VALUE, defined after builtins).
[[nodiscard]] InHouseOutput preprocess_in_house(std::string_view source, const MacroSet& builtins,
                                                const MacroSet& stdlib,
                                                const std::vector<std::pair<std::string, std::string>>& user);

} // namespace alchemy::msl::detail
