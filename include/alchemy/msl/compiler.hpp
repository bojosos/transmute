#pragma once

// Alchemy MSL compiler: Metal Shading Language source -> Vulkan SPIR-V plus
// the reflection Metal's runtime needs (alchemy-fx GUI).
//
// Implemented by src/alchemy/msl (A6, see its README.md): the pinned host
// clang (LLVM 19.1.5, run as a child process, see toolchain()) compiles MSL,
// rewritten to C++ for OpenCL, against the project-authored
// runtime/msl/metal_stdlib to LLVM IR, and an in-house LLVM IR -> SPIR-V
// lowering emits each entry point. Used by src/alchemy/metal (A5)
// for -[MTLDevice newLibraryWithSource:options:error:] and
// newLibraryWithSource:options:completionHandler:, and by pipeline creation
// (link, specialization).
//
// This header is the A5/A6 contract. Every lowering decision Metal-on-Vulkan
// depends on is fixed here, so both sides can be written in parallel:
//
// ---- Resource model (fixed) ------------------------------------------------------
// Each stage uses its own descriptor set: vertex = 0, fragment = 1,
// kernel = 0 (stage_descriptor_set). Within a stage's set:
//   binding 0                    argument table: a uniform buffer holding one
//                                64-bit buffer device address per MSL buffer
//                                index, at byte offset 8 * index
//                                (VK_KHR_buffer_device_address / Vulkan 1.2).
//   binding 16 + n               [[texture(n)]], n < 128 (arrays of textures
//                                take one binding with descriptorCount = N)
//   binding 160 + n              [[sampler(n)]], n < 16
//   binding 176 + k              constexpr sampler k (immutable sampler; the
//                                state is in reflection.constexpr_samplers)
//   binding 192 + k              the samplers of the k-th argument buffer of
//                                samplers (ArgumentBufferSamplers), one array
//   binding 224 + n              fragment [[color(n)]] input (framebuffer
//                                fetch, ColorInput): input attachment n
// Texture descriptors follow the declared access exactly
// (TextureBinding::access, descriptor.type): access::sample (the default)
// and access::read are sampled images (read() is OpImageFetch, so a
// read-only texture needs only VK_IMAGE_USAGE_SAMPLED_BIT and any format
// with sampled-image support); access::write and access::read_write are
// storage images (without a format: shaderStorageImageWriteWithoutFormat,
// and ReadWithoutFormat for read_write); a texture used with texture atomics
// is a storage image with an R32i/R32ui format whatever its access.
// texture_buffer is a uniform texel buffer (read) or a storage texel buffer
// (write, read_write) likewise. Aliases (`using rw_t = texture2d<float,
// access::read_write>;`) are resolved.
// Every [[buffer(n)]] argument (device or constant address space, pointer or
// reference, argument buffers included) is accessed through
// PhysicalStorageBuffer64 pointers loaded from the argument table, so MSL
// pointer arithmetic, casts and setVertexBuffer:offset: work unchanged: A5
// writes buffer.gpuAddress + offset into slot n before each draw/dispatch.
// setVertexBytes:/setFragmentBytes:/setBytes: data is copied by A5 to
// transient memory and its address written the same way. Buffers have no
// bounds: like Metal, out-of-range access is undefined.
// Stage inputs/outputs:
//   [[stage_in]] vertex attributes  Location = [[attribute(n)]] = n; A5 maps
//                                   MTLVertexDescriptor (formats, strides,
//                                   step functions) to Vulkan vertex input.
//   vertex -> fragment varyings     locations assigned by the compiler; a
//                                   render pipeline must call link() to make
//                                   the two stages agree (names match by
//                                   [[user(name)]] or member name, as Metal).
//   [[color(n)]] outputs            Location n, Index = [[index(i)]] (dual source)
//   Integer interface variables are signed (OpTypeInt 32 1) exactly when the
//   declared MSL type's scalar is signed (int/short/char, through aliases
//   such as Blender's `using ivec2 = int2;`), unsigned otherwise, so SINT/
//   UINT vertex formats and integer colour attachments match; 8/16-bit
//   declared types are 32-bit variables of the same signedness.
// Function constants: specialization constants with SpecId =
// [[function_constant(index)]] index (bool constants are 32-bit VkBool32 in
// VkSpecializationInfo data, other scalars their natural size).
// Runtime sizes Vulkan needs at pipeline creation are specialization
// constants with reserved ids (see reserved_spec_id): the kernel workgroup
// size (threadsPerThreadgroup of the dispatch) and each [[threadgroup(n)]]
// buffer's length (setThreadgroupMemoryLength:atIndex:, in bytes).
//
// ---- Threading and lifetime -----------------------------------------------------
// compile, link and dump_source are thread-safe and reentrant (Metal compiles
// from guest worker threads and completion handlers). They are pure host
// code: no guest memory, engine or Objective-C access. Results are values
// owned by the caller. The compiler may cache results in memory and on disk
// (keyed by compiler_version(), options and source hash); a cache hit is
// indistinguishable from a fresh compile.
//
// ---- Failure ------------------------------------------------------------------------
// Source errors and unsupported MSL features are diagnostics (severity error)
// and make CompileResult::success() false; compile() throws only for
// internal failures (toolchain missing, out of memory). Unsupported features
// are never silently dropped: they are errors naming the construct.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace alchemy::msl {

enum class Stage : std::uint32_t { vertex, fragment, kernel };

[[nodiscard]] constexpr std::uint32_t stage_descriptor_set(Stage stage) noexcept {
    return stage == Stage::fragment ? 1U : 0U;
}
inline constexpr std::uint32_t argument_table_binding = 0;
inline constexpr std::uint32_t texture_binding_base = 16;
inline constexpr std::uint32_t sampler_binding_base = 160;
inline constexpr std::uint32_t constexpr_sampler_binding_base = 176;
// Argument buffers holding samplers (addition by A6, wave 2; see
// ArgumentBufferSamplers): binding 192 + k for the k-th such [[buffer]]
// argument of an entry point, one array of samplers.
inline constexpr std::uint32_t argument_buffer_sampler_binding_base = 192;
// Framebuffer fetch (addition by A6, wave 2; see ColorInput): a fragment
// input [[color(n)]] reads input attachment n at binding 224 + n.
inline constexpr std::uint32_t color_input_binding_base = 224;
inline constexpr std::uint32_t maximum_buffers = 31;     // [[buffer(0..30)]]
inline constexpr std::uint32_t maximum_textures = 128;
inline constexpr std::uint32_t maximum_samplers = 16;

// Reserved specialization-constant ids (above any MSL function constant
// index, which is < 65536).
namespace reserved_spec_id {
inline constexpr std::uint32_t workgroup_size_x = 0x10000, workgroup_size_y = 0x10001, workgroup_size_z = 0x10002;
inline constexpr std::uint32_t threadgroup_memory_base = 0x10010;   // + [[threadgroup(n)]] index: length in bytes
// + [[function_constant(index)]] index: bool, true when the pipeline provides a
// value for that constant (is_function_constant_defined). Default false.
inline constexpr std::uint32_t function_constant_defined_base = 0x20000;
} // namespace reserved_spec_id

// Kernel grid (addition by A5, wave 1). Every kernel pipeline layout has one
// compute push-constant range at offset 0 of kernel_push_constant_size bytes:
//   struct { uint32_t threads_per_grid[3]; uint32_t non_uniform; }
// threads_per_grid is dispatchThreads:'s grid size, or threadgroups x
// threadsPerThreadgroup for dispatchThreadgroups:. non_uniform is 1 for
// dispatchThreads:, whose grid need not be a multiple of the threadgroup
// size: Vulkan dispatches whole workgroups, so the kernel must return at
// entry for threads with any thread_position_in_grid component >=
// threads_per_grid (Metal never runs them). [[threads_per_grid]] reads this
// block. Kernels that need neither may ignore it.
inline constexpr std::uint32_t kernel_push_constant_size = 16;

// ---- options ------------------------------------------------------------------------
struct CompileOptions {
    // MTLCompileOptions.languageVersion as major.minor (MTLLanguageVersion2_4 = {2, 4}).
    std::uint32_t language_major{2}, language_minor{4};
    // MTLCompileOptions.preprocessorMacros, in dictionary order; NSNumber
    // values are passed as their decimal text.
    std::vector<std::pair<std::string, std::string>> macros;
    bool fast_math{true};                 // fastMathEnabled / mathMode != safe
    bool preserve_invariance{false};
    std::string label;                    // diagnostics and dump file naming only
    // Target environment chosen by Metal (A5) from the device.
    std::uint32_t vulkan_major{1}, vulkan_minor{3};   // SPIR-V 1.6 for Vulkan 1.3
    std::uint32_t subgroup_size{32};      // simdgroup width the device guarantees (threadExecutionWidth)
    bool debug_info{false};               // OpLine / OpName for RenderDoc
};

// ---- types -----------------------------------------------------------------------------
enum class Scalar : std::uint32_t { none, boolean, int8, uint8, int16, uint16, int32, uint32, int64, uint64, half, bfloat, float32 };

struct Type {
    enum class Kind : std::uint32_t { scalar, vector, matrix, array, structure, texture, sampler, pointer, atomic, unknown };
    Kind kind{Kind::unknown};
    Scalar scalar{Scalar::none};          // element scalar for scalar/vector/matrix/atomic
    std::uint32_t components{1};          // vector size, or rows of a matrix
    std::uint32_t columns{1};             // matrix columns
    std::uint32_t array_length{};         // array: element count (0 = runtime sized, buffer pointers)
    std::uint32_t size{}, alignment{};    // bytes under MSL layout rules (float3 is 16/16, packed_float3 12/4)
    bool packed{};                        // packed_floatN / packed_halfN
    std::string name;                     // struct name or MSL spelling ("float4", "texture2d<float>")
    struct Member { std::string name; std::uint32_t offset{}; std::uint32_t type{}; };   // type: index into Reflection::types
    std::vector<Member> members;          // structure
    std::uint32_t element{};              // array/pointer: index into Reflection::types
};

enum class Access : std::uint32_t { read, write, read_write, sample };

// ---- reflection -------------------------------------------------------------------------
enum class DescriptorType : std::uint32_t { uniform_buffer, storage_buffer, sampled_image, storage_image, sampler,
    uniform_texel_buffer, storage_texel_buffer, input_attachment /* addition by A6, wave 2 */ };

struct Descriptor {
    std::uint32_t set{}, binding{};
    DescriptorType type{DescriptorType::uniform_buffer};
    std::uint32_t count{1};
};

struct BufferBinding {
    std::uint32_t index{};                // [[buffer(n)]]
    std::string name;                     // parameter name
    enum class Space : std::uint32_t { device, constant } space{Space::device};
    Access access{Access::read};          // read for constant or const-qualified device data
    std::uint32_t type{};                 // pointee type (index into Reflection::types)
    bool reference{};                     // T& (one element) rather than T*
    bool argument_buffer{};               // pointee is an argument buffer struct ([[id(n)]] members)
    bool used{true};                      // statically referenced by the entry point
    std::optional<std::uint32_t> function_constant;   // [[function_constant(i)]]: present only when true
    // [[buffer(fc + n)]] (Blender's MTL_uniform_buffer_base_index + n): the
    // real index is the value of function constant `index_function_constant`
    // plus `index`; the shader reads its argument-table slot at that index.
    std::optional<std::uint32_t> index_function_constant;
    // An argument buffer of buffer pointers and constant data (addition by g2,
    // Chrome stage 4; argument_buffer is set): the pointee struct's [[id(n)]]
    // members in declaration order, at the byte offsets clang gives them (the
    // generated wrapper static_asserts each one). The shader reads the struct
    // from memory like any other buffer, so an MTLArgumentEncoder writes buffer
    // device addresses (8 bytes) and constant data at these offsets. Textures
    // and samplers in such a struct are unsupported (no bindless descriptors).
    struct Argument {
        std::string name;
        std::uint32_t id{};               // [[id(n)]]; an array takes ids id .. id + count - 1
        std::uint32_t count{1};           // elements of an array member
        std::uint32_t offset{};           // byte offset of element 0
        std::uint32_t stride{8};          // bytes between elements
        bool pointer{true};               // a buffer pointer, else constant data of `type`
        std::uint32_t type{};             // data element type (index into Reflection::types)
    };
    std::vector<Argument> arguments;
    std::uint32_t argument_length{}, argument_alignment{};   // the struct's size and alignment
    // Byte offset of this buffer's address in the argument table (= 8 * index).
    [[nodiscard]] std::uint32_t table_offset() const noexcept { return 8U * index; }
};

enum class TextureType : std::uint32_t { texture1d, texture1d_array, texture2d, texture2d_array, texture2d_ms,
    texture2d_ms_array, texture3d, texture_cube, texture_cube_array, texture_buffer, depth2d, depth2d_array,
    depth2d_ms, depth2d_ms_array, depth_cube, depth_cube_array };

struct TextureBinding {
    std::uint32_t index{};                // [[texture(n)]]
    std::string name;
    TextureType texture_type{TextureType::texture2d};
    Scalar sample_scalar{Scalar::float32};   // texture2d<T>: T
    Access access{Access::sample};
    std::uint32_t array_length{1};        // array<texture2d<float>, N>
    Descriptor descriptor;
    bool used{true};
    std::optional<std::uint32_t> function_constant;
};

struct SamplerBinding {
    std::uint32_t index{};                // [[sampler(n)]]
    std::string name;
    Descriptor descriptor;
    bool used{true};
};

// An argument buffer of samplers (addition by A6, wave 2): a [[buffer(n)]]
// argument whose pointee struct has only [[id(i)]] `sampler` /
// `array<sampler, N>` / `sampler[N]` members (Blender's SStruct when a stage
// uses more than 16 samplers). The shader never reads that buffer: its
// samplers are the elements of one descriptor binding (`descriptor`: type
// sampler, binding argument_buffer_sampler_binding_base + k for the k-th such
// argument, descriptorCount = `count`), element e holding the sampler the
// application encoded at [[id(e)]] (array elements take consecutive ids, as
// with MTLArgumentEncoder setSamplerState:atIndex:). The argument is not in
// Reflection::buffers (it takes no argument-table slot). A5 writes the
// MTLSamplerStates encoded into the buffer bound at `index` (+ the function
// constant's value) into that binding.
struct ArgumentBufferSamplers {
    std::uint32_t index{};                // [[buffer(n)]] literal part
    std::optional<std::uint32_t> index_function_constant;   // [[buffer(fc + n)]]
    std::string name;                     // parameter name
    std::string type;                     // pointee struct name
    struct Member { std::string name; std::uint32_t id{}, count{1}; };
    std::vector<Member> members;          // [[id]] order
    std::uint32_t count{};                // highest id + 1
    Descriptor descriptor;
    bool used{true};                      // a sampler of it is used
};

// A constexpr sampler declared in the source; A5 creates it as an immutable
// sampler at `descriptor`. Enumerations use MTLSamplerDescriptor's values
// (MTLSamplerMinMagFilter, MTLSamplerMipFilter, MTLSamplerAddressMode,
// MTLCompareFunction, MTLSamplerBorderColor) so A5 shares its translation.
struct ConstexprSampler {
    Descriptor descriptor;
    std::uint32_t min_filter{}, mag_filter{}, mip_filter{};
    std::array<std::uint32_t, 3> address{};   // s, t, r
    std::uint32_t compare_function{};         // 0 = never compare
    std::uint32_t border_color{};
    std::uint32_t max_anisotropy{1};
    float lod_min{0}, lod_max{1000};
    bool normalized_coordinates{true};
};

struct VertexAttribute {
    std::uint32_t index{};                // [[attribute(n)]] == Vulkan location
    std::string name;
    std::uint32_t type{};
    bool used{true};
};

enum class Interpolation : std::uint32_t { perspective, no_perspective, flat, centroid_perspective,
    centroid_no_perspective, sample_perspective, sample_no_perspective };

struct Varying {
    std::string name;                     // [[user(name)]] or member name; the link key
    std::uint32_t type{};
    std::uint32_t location{};
    Interpolation interpolation{Interpolation::perspective};
};

struct FragmentOutput {
    std::uint32_t color{};                // [[color(n)]] == Vulkan location
    std::uint32_t index{};                // [[index(i)]] dual-source blending
    std::uint32_t type{};
};

// A fragment input [[color(n)]] (framebuffer fetch; addition by A6, wave 2):
// the current value of colour attachment n, read from a SubpassData input
// attachment with InputAttachmentIndex n at `descriptor` (set 1, binding
// color_input_binding_base + n, DescriptorType::input_attachment).
// Under dynamic rendering this needs VK_KHR_dynamic_rendering_local_read
// (input attachment index n = colour attachment n, the default mapping).
// [[raster_order_group(k)]] is ignored (no fragment shader interlock).
struct ColorInput {
    std::uint32_t color{};
    std::uint32_t type{};                 // index into Reflection::types
    std::string name;
    Descriptor descriptor;
};

enum class DepthOutput : std::uint32_t { none, any, greater, less };

struct FunctionConstant {
    std::uint32_t index{};                // [[function_constant(index)]] == SpecId
    std::string name;
    std::uint32_t type{};                 // scalar or vector type
    bool required{};                      // used without is_function_constant_defined
};

struct ThreadgroupMemory {
    std::uint32_t index{};                // [[threadgroup(n)]]
    std::string name;
    std::uint32_t type{};                 // element type
    std::uint32_t spec_id{};              // reserved_spec_id::threadgroup_memory_base + index
};

enum class Builtin : std::uint32_t { vertex_id, instance_id, base_vertex, base_instance, position, point_size,
    clip_distance, front_facing, point_coord, sample_id, sample_mask, primitive_id, render_target_array_index,
    viewport_array_index, thread_position_in_grid, thread_position_in_threadgroup, threadgroup_position_in_grid,
    threads_per_threadgroup, threads_per_grid, threadgroups_per_grid, thread_index_in_threadgroup,
    thread_index_in_simdgroup, simdgroup_index_in_threadgroup, threads_per_simdgroup, simdgroups_per_threadgroup };

struct Reflection {
    Stage stage{Stage::vertex};
    std::vector<Type> types;              // every type referenced below, by index
    Descriptor argument_table;            // set/binding of the address table (binding 0)
    std::uint32_t argument_table_size{};  // bytes: 8 * (highest buffer index + 1); 0 when no buffers
    std::vector<BufferBinding> buffers;
    std::vector<TextureBinding> textures;
    std::vector<SamplerBinding> samplers;
    std::vector<ConstexprSampler> constexpr_samplers;
    std::vector<ArgumentBufferSamplers> argument_buffers;   // addition by A6, wave 2
    std::vector<FunctionConstant> function_constants;
    std::vector<Builtin> builtins;        // builtins the entry point reads or writes
    // Vertex.
    std::vector<VertexAttribute> stage_in;
    std::vector<Varying> outputs;         // vertex outputs besides [[position]]/[[point_size]]/[[clip_distance]]
    // Fragment.
    std::vector<Varying> inputs;
    std::vector<FragmentOutput> color_outputs;
    std::vector<ColorInput> color_inputs;   // framebuffer fetch (addition by A6, wave 2)
    DepthOutput depth{DepthOutput::none};
    bool writes_sample_mask{}, writes_stencil{}, early_fragment_tests{};   // [[early_fragment_tests]]
    bool discards{};
    // Kernel.
    std::uint32_t max_total_threads_per_threadgroup{};   // [[max_total_threads_per_threadgroup(n)]], 0 = none
    std::vector<ThreadgroupMemory> threadgroup_memory;
    bool uses_simdgroup_operations{};
    // Lowering notes for diagnostics and the Metal trace (e.g. "argument
    // buffer buffer(2): textures inside argument buffers are unsupported").
    std::vector<std::string> notes;
};

// ---- results ---------------------------------------------------------------------------
struct Diagnostic {
    enum class Severity : std::uint32_t { note, warning, error } severity{Severity::error};
    std::string message;
    std::uint32_t line{}, column{};       // 1-based in the MSL source; 0 when not tied to a location
};

// One entry point ([[vertex]], [[fragment]] or [[kernel]] function).
struct Function {
    std::string name;                     // MSL function name == SPIR-V OpEntryPoint name
    Stage stage{Stage::vertex};
    std::vector<std::uint32_t> spirv;     // complete SPIR-V module with this single entry point; spirv-val clean
    Reflection reflection;
};

struct CompileResult {
    std::vector<Function> functions;      // source order
    std::vector<Diagnostic> diagnostics;
    [[nodiscard]] bool success() const noexcept {
        for (const auto& diagnostic : diagnostics) if (diagnostic.severity == Diagnostic::Severity::error) return false;
        return true;
    }
};

// Compiles one MTLLibrary's source. See the header comment for the rules.
[[nodiscard]] CompileResult compile(std::string_view source, const CompileOptions& options);

// Makes a vertex and a fragment function of one render pipeline agree on
// varying locations: returns copies whose SPIR-V Location decorations match
// by varying name. Fragment inputs the vertex stage does not write are an
// error diagnostic (as Metal's pipeline creation fails).
struct LinkResult {
    Function vertex, fragment;
    std::vector<Diagnostic> diagnostics;
    [[nodiscard]] bool success() const noexcept {
        for (const auto& diagnostic : diagnostics) if (diagnostic.severity == Diagnostic::Severity::error) return false;
        return true;
    }
};
[[nodiscard]] LinkResult link(const Function& vertex, const Function& fragment);

// "program_source:LINE:COL: error: message" lines, as Metal's NSError
// localizedDescription shows compiler output.
[[nodiscard]] std::string format_diagnostics(const std::vector<Diagnostic>& diagnostics);

// Identifies the compiler build for cache keys (bumped on any lowering change).
[[nodiscard]] std::string compiler_version();

// Reads every on-disk cache result of this compiler version into the
// in-process cache, so later hits open no file. Thread-safe; meant for a
// background thread at process start (gui::prefetch_metal_device). Returns
// the number of results loaded.
std::size_t preload_cache();

// ---- seed results ----------------------------------------------------------------------
// A read-only set of compile results (<directory>/results/*.bin, the cache's
// own file format) consulted after a cache miss; a hit is copied into the
// cache. Metal points it at <ALCHEMY_MSL_SEED>/<LC_UUID of the executable>.
// Seeds hold compiled application shaders: they are generated locally
// (seed_corpus) and never committed.
void set_seed_directory(const std::filesystem::path& directory);
// Compiles every <name>.metal of an ALCHEMY_MSL_DUMP corpus (with its
// .options) into the cache rooted at ALCHEMY_MSL_CACHE, on `threads`
// threads; `subgroup_size` when the .options file has none. Returns the
// number of libraries compiled without errors; `report` gets one line per
// failure.
std::size_t seed_corpus(const std::filesystem::path& corpus, unsigned threads, std::uint32_t subgroup_size,
                        std::string& report);
// The options of a dumped library (<name>.options beside <name>.metal).
[[nodiscard]] CompileOptions read_dump_options(const std::filesystem::path& source);

// ---- toolchain -----------------------------------------------------------------------
// The host clang the compiler front end runs: ALCHEMY_MSL_CLANG
// (environment) or the configured default (CMake cache ALCHEMY_CLANG),
// probed once with --version. Thread-safe.
struct Toolchain {
    bool available{};
    std::string path;                     // the clang executable used or tried
    std::string version;                  // first line of --version, e.g. "clang version 19.1.5"
    std::string error;                    // why it is unavailable
};
[[nodiscard]] const Toolchain& toolchain();

// ---- shader corpus -------------------------------------------------------------------
// ALCHEMY_MSL_DUMP=<directory>: Metal calls dump_source for every
// newLibraryWithSource: before compiling. Each distinct source is written once
// as <directory>/<label-or-"library">-<16 hex digit hash>.metal, followed by a
// .options file listing language version and macros. Returns the path written
// or found, or nullopt when dumping is off. Never throws: a dump failure is
// reported once to stderr and dumping continues for later sources.
std::optional<std::string> dump_source(std::string_view source, const CompileOptions& options);
// The same for a precompiled library handed to newLibraryWithData: (a
// metallib): <directory>/metallib-<16 hex digit hash>.metallib, written once.
// The dump directory is outside any repository (artifacts/); it is input for
// evaluating translators, never for git.
std::optional<std::string> dump_library_data(std::string_view bytes);

} // namespace alchemy::msl
