#pragma once

// The MSL compiler's clang front end (owner: A6): runs the pinned clang.exe
// (LLVM 19.1.5) as a child process to preprocess MSL and to compile the
// rewritten source as C++ for OpenCL 1.0 to textual LLVM IR for spir64.
// Sources and results travel through pipes; the only files are the
// embedded metal_stdlib, written once to the cache directory.

#include "alchemy/msl/compiler.hpp"
#include "preprocess.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::msl::detail {

struct ClangOutput {
    bool ok{};
    std::string output;                    // stdout: preprocessed source or LLVM IR
    std::vector<Diagnostic> diagnostics;   // parsed from stderr
    bool pch_rejected{};                   // compile_to_ir with `pch`: clang could not use it
};

// Preprocesses the application's source with its macros: the in-process
// preprocessor (preprocess.hpp) when it handles the source, else clang -E.
// __ALCHEMY_MSL_PREPROCESS hides everything in metal_stdlib but its macros.
// ALCHEMY_MSL_PP=clang always uses clang -E.
[[nodiscard]] ClangOutput preprocess(std::string_view source, const CompileOptions& options);
// The two paths separately (tests compare them).
[[nodiscard]] ClangOutput preprocess_with_clang(std::string_view source, const CompileOptions& options);
[[nodiscard]] InHouseOutput preprocess_in_house(std::string_view source, const CompileOptions& options);
// clang -O2 -S -emit-llvm of the rewritten source with metal_stdlib included.
// With `pch` (a prelude PCH from build_pch, which includes metal_stdlib), the
// source is compiled after it instead; when clang refuses that PCH the result
// has pch_rejected set and nothing else is tried.
[[nodiscard]] ClangOutput compile_to_ir(std::string_view source, const CompileOptions& options,
                                        const std::filesystem::path& pch = {});
// Precompiles `header` (text, read from stdin by clang, so no file has to stay
// unchanged for the PCH to remain valid) with the IR compile's arguments
// into `output` (written under a private name and renamed).
[[nodiscard]] ClangOutput build_pch(std::string_view header, const std::filesystem::path& output,
                                    const CompileOptions& options);
// Identifies the IR compile's language configuration (arguments without the
// application's macros, clang): PCHs are only valid within one.
[[nodiscard]] std::uint64_t ir_configuration_hash(const CompileOptions& options);

// Directory holding the materialised metal_stdlib (created on first use).
[[nodiscard]] const std::filesystem::path& stdlib_directory();
// 64-bit FNV-1a of the embedded stdlib (part of compiler_version()).
[[nodiscard]] std::uint64_t stdlib_fingerprint();
// The compiler's cache root: ALCHEMY_MSL_CACHE, else %LOCALAPPDATA%/Alchemy/msl;
// empty when caching on disk is disabled (ALCHEMY_MSL_CACHE=0).
[[nodiscard]] const std::filesystem::path& cache_root();

[[nodiscard]] std::string environment(const char* name);
[[nodiscard]] std::uint64_t fnv1a(std::string_view data, std::uint64_t hash = 0xcbf29ce484222325ULL);

} // namespace alchemy::msl::detail
