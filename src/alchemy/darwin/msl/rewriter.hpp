#pragma once

// MSL -> C++ for OpenCL source rewriter (owner: A6). Runs on the output of
// `clang -E` (macros already expanded, line markers kept) and produces the
// source clang compiles to LLVM IR:
//   - MSL attributes ([[buffer(n)]], [[stage_in]], [[position]], ...) are
//     consumed and recorded as an interface description per entry point;
//   - address spaces become OpenCL ones (device/constant -> __global,
//     threadgroup -> __local, thread -> generic), OpenCL keywords used as MSL
//     identifiers are renamed;
//   - function constants become extern globals the lowering turns into
//     OpSpecConstant; function-scope threadgroup variables become references
//     to statically allocated workgroup storage;
//   - each entry point gets an extern "C" wrapper (__msl_entry_<i>) that
//     reads the stage inputs and resources through compiler intrinsics,
//     calls the user's function and writes its outputs.
// The user's code is wrapped in an anonymous namespace so only the wrappers
// survive optimisation.

#include "alchemy/msl/compiler.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::msl::detail {

// Builtin ids the wrappers pass to the intrinsics: alchemy::msl::Builtin
// values, plus the fragment outputs Reflection describes separately.
inline constexpr std::uint32_t builtin_frag_depth = 100;
inline constexpr std::uint32_t builtin_stencil = 101;
inline constexpr std::uint32_t builtin_sample_mask_out = 102;
inline constexpr std::uint32_t builtin_invariant_flag = 0x100;   // or'ed into position outputs

// A parsed MSL type spelling (scalar/vector/matrix/texture/sampler names only;
// anything else is an opaque name).
struct TypeSpelling {
    Type::Kind kind{Type::Kind::unknown};
    Scalar scalar{Scalar::none};
    std::uint32_t components{1}, columns{1};
    bool packed{};
    TextureType texture{TextureType::texture2d};
    Access access{Access::sample};
    std::uint32_t array_length{};          // array<T, N> of textures
    std::string text;                      // the spelling, cleaned of address spaces/cv/ref
};
[[nodiscard]] TypeSpelling parse_type_spelling(std::string_view text);

struct InterfaceVarying {
    std::string name;                      // [[user(name)]] or member name
    std::string member;                    // member name in the struct
    std::uint32_t location{};
    TypeSpelling type;
    Interpolation interpolation{Interpolation::perspective};
    std::optional<std::uint32_t> function_constant;
};

struct InterfaceBuffer {
    std::uint32_t index{};                 // literal part of the index
    std::optional<std::uint32_t> index_constant;   // [[buffer(fc + n)]]: function constant index
    std::string name;
    BufferBinding::Space space{BufferBinding::Space::device};
    bool is_const{};
    bool reference{};
    std::string pointee;                   // pointee type spelling
    std::optional<std::uint32_t> function_constant;
    // An argument buffer of buffer pointers and data (BufferBinding::arguments).
    struct Argument {
        std::string name;
        std::uint32_t id{}, count{1}, offset{}, stride{8};
        bool pointer{true};
        TypeSpelling type;                 // data members
    };
    bool argument_buffer{};
    std::vector<Argument> arguments;
    std::uint32_t argument_length{}, argument_alignment{};
};

struct InterfaceTexture {
    std::uint32_t index{};
    std::string name;
    TypeSpelling type;
    std::optional<std::uint32_t> function_constant;
};

struct InterfaceSampler {
    std::uint32_t index{};
    std::string name;
};

// A [[buffer]] argument of samplers only (ArgumentBufferSamplers): the
// wrapper passes a static struct whose sampler handles name the elements of
// the k-th argument-buffer binding (sampler handle bit 62 | k << 32 | element).
struct InterfaceArgumentBuffer {
    std::uint32_t index{};
    std::optional<std::uint32_t> index_constant;
    std::string name, type;
    std::vector<ArgumentBufferSamplers::Member> members;
    std::uint32_t count{};
};
inline constexpr std::uint64_t argument_buffer_sampler_flag = 1ULL << 62;

struct InterfaceBuiltin {
    Builtin builtin{};
    std::string name;
};

struct InterfaceThreadgroup {
    std::uint32_t index{};
    std::string name;
    std::string element;
};

struct EntryPoint {
    Stage stage{Stage::vertex};
    std::string name;                      // MSL function name (or [[host_name]])
    std::string wrapper;                   // __msl_entry_<i>
    std::uint32_t line{};
    std::vector<InterfaceBuffer> buffers;
    std::vector<InterfaceTexture> textures;
    std::vector<InterfaceSampler> samplers;
    std::vector<InterfaceArgumentBuffer> argument_buffers;   // k = position
    std::vector<InterfaceThreadgroup> threadgroups;
    std::vector<InterfaceBuiltin> builtin_inputs;
    std::vector<InterfaceBuiltin> builtin_outputs;
    std::vector<InterfaceVarying> attributes;     // vertex [[stage_in]] (location = attribute index)
    std::vector<InterfaceVarying> inputs;         // fragment varyings
    std::vector<InterfaceVarying> outputs;        // vertex varyings
    struct Color { std::uint32_t color{}, index{}; TypeSpelling type; std::string name; };
    std::vector<Color> colors;
    std::vector<Color> color_inputs;              // fragment [[color(n)]] inputs (framebuffer fetch)
    DepthOutput depth{DepthOutput::none};
    bool writes_stencil{}, writes_sample_mask{}, early_fragment_tests{}, invariant_position{};
    std::uint32_t max_total_threads_per_threadgroup{};
    std::vector<std::string> notes;
};

struct FunctionConstantDecl {
    std::string name;
    std::uint32_t index{};
    TypeSpelling type;
};

// The pragmas around the user's code in the rewritten source (internal
// linkage and always_inline for everything the application wrote).
inline constexpr const char* user_code_begin =
    "#pragma clang attribute push (__attribute__((internal_linkage)), apply_to = any(function, variable(is_global)))\n"
    "#pragma clang attribute push (__attribute__((always_inline)), apply_to = function)\n";
inline constexpr const char* user_code_end = "#pragma clang attribute pop\n#pragma clang attribute pop\n";

// A place the rewritten source can be cut in two, for a precompiled prelude
// (prelude.hpp): the start of a line beginning a translation-unit-scope
// declaration, inside the user_code_begin region (function constants are
// declared in place, so the text before a split point is normally the same
// in every library sharing that source prefix; the prelude cache compares
// the bytes). `line`/`file` restore the position: the rest compiles as
// user_code_begin + `# line "file"` + source.substr(offset).
struct SplitPoint {
    std::size_t offset{};
    std::uint32_t line{};
    std::string file;
};

struct RewriteResult {
    std::string source;
    std::vector<SplitPoint> split_points;   // ascending offsets
    std::vector<EntryPoint> entries;
    std::vector<FunctionConstantDecl> function_constants;
    std::uint32_t threadgroup_variables{};   // function-scope threadgroup declarations rewritten
    std::vector<Diagnostic> diagnostics;
};

// `preprocessed`: clang -E output of the library source.
[[nodiscard]] RewriteResult rewrite(std::string_view preprocessed);

} // namespace alchemy::msl::detail
