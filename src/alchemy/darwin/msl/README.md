# Alchemy MSL compiler

Metal Shading Language source -> Vulkan SPIR-V plus the reflection Metal needs,
for `-[MTLDevice newLibraryWithSource:...]` in alchemy-fx's Metal-on-Vulkan
provider (`src/alchemy/metal`, A5). The contract, including the fixed resource
model, is `include/alchemy/msl/compiler.hpp`. Everything here is
project-authored (MIT): the standard library in `runtime/msl` was written from
the public MSL specification, and no Apple header, SDK or binary is used.

## Pipeline

```
MSL source
  | dump_source (ALCHEMY_MSL_DUMP)        dump.cpp
  | cache lookup (memory, then disk)       cache.cpp
  v
preprocessor (in process; clang -E when   preprocess.cpp, frontend.cpp
  it declines): macros, #include <metal_stdlib> with only its macros visible
  v
rewriter: MSL attributes -> interface      rewriter.cpp
  description; address spaces -> OpenCL
  ones; entry wrappers __msl_entry_<i>
  v
clang -x clcpp -target spir64 -O2 -S -emit-llvm   frontend.cpp, prelude.cpp
  (+ -include-pch metal_stdlib, or of the library's precompiled prelude;
  a clang started ahead of time and waiting on stdin)
  v
LLVM IR text parser                        llvm_ir.cpp
  v  per entry point
structurizer (SPIR-V structured CF)        structurize.cpp
  v
lowering -> SPIR-V module + Reflection     lower.cpp, spirv_builder.cpp
```

* **Front end.** The pinned `clang.exe` 19.1.5 (CMake `ALCHEMY_CLANG`; the x64
  host binary is preferred, `ALCHEMY_MSL_CLANG` overrides at run time) runs as
  a child process once per library, for the IR compile. Sources travel
  through pipes; a child inherits only its own pipe handles, so concurrent
  compiles do not hold each other's pipes open. `toolchain()` reports the
  clang found (`clang --version`). The libclang C API is not used.
  Starting clang.exe costs tens of milliseconds before it reads its input,
  so the IR compile keeps one spare clang per argument list, started ahead
  of time and blocked on stdin (at most 4; `ALCHEMY_MSL_WARM=0` disables): a
  compile takes the spare, writes its source and starts the next spare while
  clang works. Unused spares end when the process does (their stdin closes).
* **Preprocessor.** `preprocess.cpp` is an in-process C preprocessor
  (Prosser's hide-set expansion, as clang implements the standard) seeded
  with clang's predefined macros and the macros `<metal_stdlib>` adds, both
  taken once from `clang -dM -E` and kept next to the materialised stdlib.
  Its output matches `clang -E` token for token and line for line (tested on
  synthetic cases, every fixture and the whole corpus). Anything it does not
  implement exactly (an active `#error`/`#warning`, other `#include`s,
  `__has_include` and friends, `_Pragma`, `__COUNTER__`, `__VA_OPT__`, named
  variadic parameters, digraphs, raw strings, invalid pastes, malformed
  directives) makes it decline and `clang -E` runs instead, also producing
  the diagnostics. `ALCHEMY_MSL_PP=clang` always uses `clang -E`.
* **Precompiled metal_stdlib.** The library is precompiled once per (language
  version, fast math, clang) into `<stdlib dir>/metal_stdlib-*.pch` and the
  IR compile uses `-include-pch` (text include if clang refuses the PCH;
  `ALCHEMY_MSL_PCH=0` disables).
* **Precompiled preludes** (`prelude.cpp`). Applications generate every
  library from one large common prelude and the shader's own code (Blender:
  ~50 KB of `mtl_shader_defines` and GLSL compatibility code, most of each
  library's clang time). The rewriter reports split points (line starts of
  translation-unit-scope declarations; function constants are declared in
  place, so the text before a split point does not depend on what follows).
  Each library's prefixes at its split points are hashed: the longest prefix
  with a precompiled prelude is not compiled again (clang compiles only the
  rest, with `-include-pch` of the prelude, which includes metal_stdlib);
  otherwise the longest prefix of at least 16 KB shared with a library this
  process compiled before is precompiled once, from stdin, into
  `<stdlib dir>/preludes/p-<configuration>-<length>-<hash>.pch` and used by
  every later library starting with exactly those bytes, in this process and
  the next ones (at most 32 per configuration, oldest evicted). The IR is
  the same as for the whole source (the corpus test compares a sample).
  `ALCHEMY_MSL_PRELUDE=0` disables; a prelude clang refuses is not used
  again.
* **Language mapping.** MSL is compiled as C++ for OpenCL 1.0 to `spir64`:
  `device`/`constant` -> `__global` (addrspace 1), `threadgroup` -> `__local`
  (3), `thread` -> generic/private. OpenCL keywords and builtin typedefs used
  as MSL names (`read_write`, `kernel`, `global`, `atomic_uint`, ...) are
  renamed. User code stays in the global namespace, as in Metal (a namespace
  would let user overloads such as Blender's `mix(vec, vec, bool)` hide
  `metal::mix` from unqualified lookup); `#pragma clang attribute` gives it
  internal linkage and `always_inline`, so only the wrappers stay external
  and everything is inlined into them.
* **OpenCL C++ adaptations.** Each operand of a conditional operator becomes
  `(metal::__msl_rv{}, (operand))`, an overloaded comma returning a prvalue:
  clang's OpenCL C++ rejects a class prvalue against a `__private` lvalue of
  the same class (`c ? float2(0) : local[i]`). Array parameters (`float4
  offset[3]`) become pointers, which clang would otherwise decay to
  `__private` pointers that an array member reached through `this` cannot
  bind to.
* **metal_stdlib.** `runtime/msl/metal_stdlib` + `runtime/msl/__msl/*.h` are
  embedded in the compiler (`embed_stdlib.cmake`) and written once per
  fingerprint to `<cache>/stdlib-<hash>/` (also `metal_*` and `simd/simd.h`
  forwarding headers). Vectors are class templates over clang
  `ext_vector_type` (swizzles, including writable ones, are generated by
  `gen_swizzles.py`); matrices are column arrays; textures and samplers are
  integer handles; every operation that is not plain C++ is an `extern`
  intrinsic (`__msl_glsl*`, `__msl_tex_*`, `__msl_group*`, `__msl_barrier`,
  ...) the lowering recognises by name.
* **Rewriter.** A shallow token-level pass over the preprocessed source:
  records each `[[vertex]]`/`[[fragment]]`/`[[kernel]]` function's interface
  (buffers, textures, samplers, threadgroup parameters, builtins, stage_in /
  varyings / colour outputs, function constants), removes the attributes, and
  emits an `extern "C"` wrapper per entry that reads inputs/resources through
  intrinsics, calls the function and writes the outputs. Function constants
  become `extern` globals (`msl.fc.<index>`); `is_function_constant_defined`
  becomes an intrinsic; function-scope `threadgroup` variables become static
  workgroup storage.
* **IR parser.** Models only what clang emits for this subset; anything else
  is an error naming the construct.
* **Structurizer.** Switches -> branch chains (one test per distinct case
  target, the last case of an exhaustive switch untested); natural loops get a single
  continue block and a single merge (with an exit selector); every other
  conditional branch gets a selection merge at its region's immediate
  post-dominator. Blocks shared by two non-nesting constructs (clang sinks
  common tails, e.g. two paths ending in the same store + return) are
  duplicated per construct; a loop header, or a shared tail longer than six
  blocks, is not copied: every edge from the other side of the nearest
  enclosing selection into it (and into the blocks after it that side
  reaches, and into that selection's merge) goes through one funnel block
  with a selector phi and a dispatch chain. Post-dominators are computed once
  per region and CFG edit, and the duplication scan resumes at the header
  that changed (`ALCHEMY_MSL_STRUCTURIZE_TRACE` reports each copy/funnel).
  Values whose definition no longer dominates a use are demoted to Function
  variables. Irreducible flow is rejected.
* **Lowering.** One SPIR-V 1.6 module (1.5 for Vulkan 1.2) per entry point,
  `PhysicalStorageBuffer64` memory model when buffers are used:
  * `[[buffer(n)]]`: a 64-bit address loaded from the stage's argument table
    (uniform buffer at set `stage_descriptor_set`, binding 0, byte `8 * n`;
    declared as 256 bytes). `[[buffer(fc + n)]]` indexes the table at run
    time with the function constant's value. Loads/stores use
    `OpConvertUToPtr` + `Aligned`, so pointer arithmetic, casts and offsets
    behave as in Metal.
  * `thread` memory (allocas) and `threadgroup` memory are word arrays in
    Function / Workgroup storage addressed by byte offset (type punning works);
    `[[threadgroup(n)]]` arrays are sized by spec constant `0x10010 + n`.
    Constant data globals are a Private word array.
  * i8/i16 values live widened in 32-bit registers, `half` in 32-bit floats;
    8/16-bit types appear only in memory. `double` is rejected.
  * Textures: `OpTypeImage` at binding `16 + n` (arrays: one binding,
    `descriptorCount = N`), samplers at `160 + n`, constexpr samplers at
    `176 + k` (state in `Reflection::constexpr_samplers`, MTLSampler* enums).
    `sample` (implicit lod only in fragments; bias/level/gradient/min_lod_clamp,
    offsets), `sample_compare`, `gather`, `gather_compare`, `read`, `write`,
    `get_width/height/depth/array_size/num_mip_levels/num_samples`,
    `fence`, texture atomics (`atomic_fetch_*` on int/uint storage images).
    Sampling through a constexpr sampler with `coord::pixel` + nearest
    filtering (GLSL `texelFetch` on Metal, as Blender writes it) becomes
    `OpImageFetch` with the address mode (clamp_to_edge / clamp_to_zero)
    applied in the shader, because Vulkan forbids lod, offsets and arrays with
    unnormalized samplers.
  * Kernel grid (`kernel_push_constant_size`, compiler.hpp): every kernel
    wrapper starts with `if (__msl_kernel_outside_grid()) return;`, which
    reads the 16-byte push-constant block (`threads_per_grid`, `non_uniform`)
    so the threads Vulkan runs past a non-uniform `dispatchThreads:` grid
    return at entry. `[[threads_per_grid]]` is the pushed grid, or
    threadgroups x threads per threadgroup when none is pushed (indirect).
  * Interface signedness: an integer stage_in attribute, varying or colour
    output is a signed `OpTypeInt 32 1` variable exactly when its declared
    MSL type's scalar is signed (`__io_sign` in `entry.h` asks clang, so
    aliases such as Blender's `using ivec2 = int2;` resolve), so SINT/UINT
    vertex formats, flat varyings and integer attachments match. The
    rewriter also resolves `using`/`typedef` aliases for the reflection.
  * Argument buffers of samplers: a `[[buffer(n)]]` argument whose struct has
    only `[[id(i)]]` `sampler` / `array<sampler, N>` / `sampler[N]` members
    (Blender's `SStruct` when a stage uses more than 16 samplers) is not read
    from memory: the wrapper passes a static struct whose sampler handles
    name element `id` of one sampler array at binding `192 + k`
    (`Reflection::argument_buffers`). Array indexing keeps the address space
    (`constant sampler* s = &args.sampler_args[3];`).
  * Argument buffers of buffer pointers and data (ANGLE's uniform-block
    argument buffer, design): a `[[buffer(n)]]` struct whose members all
    carry `[[id(i)]]` and are `constant`/`device` pointers (arrays `[N]` or
    `array<T*, N>` take ids `i .. i + N - 1`) or scalars/vectors/matrices is
    read from memory like any buffer; `BufferBinding::arguments` gives
    MTLArgumentEncoder each id's byte offset (MSL size rules, which the
    wrappers `static_assert` against clang's layout) and the struct's length.
    Textures or samplers next to them are an explicit error.
  * Framebuffer fetch: a fragment `[[color(n)]]` input (a parameter, or the
    members of a struct parameter such as Blender's `FragmentTileIn`) is an
    `OpImageRead` of a `SubpassData` input attachment n at binding `224 + n`
    (`Reflection::color_inputs`); `[[raster_order_group]]` is ignored.
  * 64-bit buffer accesses with less than 8-byte alignment (clang merging two
    32-bit loads) are split into 32-bit accesses (Vulkan's `Aligned` rule).
  * Function constants: `OpSpecConstant*` with `SpecId = index`;
    `is_function_constant_defined(c)` reads spec id `0x20000 + index`
    (bool, default false); `[[function_constant(c)]]` on parameters/outputs
    is honoured (an absent `[[depth]]` output writes the fragment's own depth,
    an absent `[[sample_mask]]` all ones). A program-scope constant computed
    from function constants (`constant bool c = a || b;`, ANGLE's
    `ANGLESampleMaskWriteEnabled`) becomes an inline function called at each
    use, so it is evaluated after specialization (as a global it would need a
    constructor no entry point runs); it may name `[[function_constant(c)]]`
    conditions. Any other program-scope variable with a dynamic initializer is
    an explicit error. Kernel workgroup size: `WorkgroupSize` built from spec
    ids `0x10000..0x10002`.
  * `constant` and `device` both become `__global`, so overloads differing
    only in them (Tint's `tint_array::operator[]` set) would be
    redefinitions: a later one with the same head after renaming is dropped
    when its body is the same tokens, otherwise an explicit error.
  * Builtins: positions, ids, `front_facing`, `point_coord`, `sample_id`,
    `sample_mask` (in/out), `[[depth(any|less|greater)]]`, `point_size`,
    `clip_distance`, `render_target_array_index`, `viewport_array_index`,
    all kernel grid/threadgroup ids and sizes, SIMD-group lane/index/width
    (kernels and fragments).
  * SIMD-group functions -> `GroupNonUniform*` (`simd_sum/product/min/max/
    and/or/xor`, prefix sums/products, `simd_shuffle*`, `simd_broadcast*`,
    `simd_ballot/any/all/vote`, `simd_is_first`, `quad_*`); barriers ->
    `OpControlBarrier`/`OpMemoryBarrier`; atomics on device and threadgroup
    memory -> `OpAtomic*`; `discard_fragment` -> `OpTerminateInvocation`
    (SPIR-V 1.6) or `OpKill`; derivatives -> `OpDPdx/DPdy/Fwidth` (+ fine/coarse).
* **Linking.** `link(vertex, fragment)` matches fragment inputs to vertex
  outputs by `[[user(name)]]` / member name, checks shapes, and rewrites the
  fragment's input `Location` decorations. Unwritten inputs and shape
  mismatches are errors.

## What the Metal provider (A5) must enable

The modules declare only the capabilities they use. The Vulkan device needs:
Vulkan 1.3 (SPIR-V 1.6; `vulkan_minor = 2` selects SPIR-V 1.5 and `OpKill`),
`bufferDeviceAddress` and `shaderInt64` (every buffer access), and when a
shader uses them: `storageBuffer16BitAccess` / `storageBuffer8BitAccess`
(half/short/char data in buffers; `Float16`/`Int16`/`Int8` follow),
`shaderInt16`/`shaderFloat16` as reported, `sampleRateShading`
(`[[sample_id]]`, sample interpolation), `shaderClipDistance`,
`shaderStorageImageReadWithoutFormat` / `WriteWithoutFormat` (read/write
textures carry no format), `shaderStorageImageMultisample`,
`imageCubeArray`, `shaderImageGatherExtended` (non-constant offsets),
`fragmentStoresAndAtomics` / `vertexPipelineStoresAndAtomics` (buffer
writes outside kernels), `shaderOutputLayer` / `shaderOutputViewportIndex`,
`shaderInt64Atomics`, `VK_EXT_shader_stencil_export`,
`VK_EXT_shader_atomic_float` (float atomic add), and subgroup support
(`subgroupSupportedStages`/`Operations`: basic, vote, arithmetic, ballot,
shuffle, shuffle_relative, quad) in the stages that use SIMD-group
functions. The argument table must be bound with at least
`Reflection::argument_table_size` bytes (the shader declares 256).

## Caching and diagnostics

* Key: `compiler_version()` (format number, stdlib fingerprint, clang
  version) + every `CompileOptions` field + source hash and length. Memory
  map, then `ALCHEMY_MSL_CACHE` (default `%LOCALAPPDATA%/Alchemy/msl`,
  `0`/`off` disables) `results/<hash>.bin` (full key stored inside and
  compared; written to a private file and renamed). Only successful results
  are written to disk.
* Diagnostics are `program_source:LINE:COL: error: ...`; clang's line markers
  keep source lines exact; problems inside metal_stdlib or the generated
  wrappers name the file instead. Lowering failures name the entry point and
  the construct; internal errors (a compiler bug) are reported as
  `internal compiler error` diagnostics, never crashes.
* `ALCHEMY_MSL_KEEP=<dir>` keeps each compile's `.pre`, `.rewritten.cpp` and
  `.ll` for debugging; `alchemy_msl_tests compile <file.metal> [dir]` prints
  diagnostics, reflection and timing and writes the `.spv` files.

## Performance

A library costs one clang run (the IR compile, of only the part after its
precompiled prelude when it has one, by a clang started ahead of time);
preprocessing (about 10 ms), rewriting (5-20 ms), IR parsing and lowering (a
few ms) run in process. The first library of a prelude pays for building its
PCH (about 0.4 s, once per machine), and the first compile per (language
version, fast math) builds the metal_stdlib PCH and reads clang's predefined
macros (about 0.3 s, once per machine). Measured on the shared development
machine (other builds running, so absolute numbers vary by +-30%), serial
cold compiles of 40 corpus libraries: 216-250 ms per library without
preludes and warm processes (`ALCHEMY_MSL_PRELUDE=0 ALCHEMY_MSL_WARM=0`),
137-150 ms with them; clang's share drops from 190-220 ms to 112-124 ms,
the rest is the clang process itself (an empty source with the PCH takes
about 50 ms here). Blender compiles about 200 libraries at startup, many in
parallel: the second presented frame (the UI after the driver dialog) of a
cold start (empty result cache) came at 30.4-30.7 s against 37.4-39.0 s
before these changes (interleaved runs), and at 12-16 s warm. The corpus
(257 libraries) compiles in about 45 s wall on 8 threads including
spirv-val; the largest EEVEE shaders (eevee_deferred_light, film) take 1.5-3
s, mostly clang. `ALCHEMY_MSL_TIMING=1` prints each uncached compile's phase
times and its prelude use. `compile` is thread-safe, so Metal can compile
libraries in parallel (`newLibraryWithSource:completionHandler:` from worker
threads); a disk-cache hit costs a file read.

## Supported subset and known gaps

Supported: the MSL used by the fixtures in `tests/fixtures` (Blender-style
generated shaders: GLSL type aliases via `#define`/`using`, shader classes
with members and methods, `constant`/`device` struct buffers including
`[[buffer(MTL_uniform_buffer_base_index + n)]]`, `[[stage_in]]` vertex
attributes, flat/centroid/sample/no-perspective varyings, function constants,
threadgroup memory and barriers, atomics, SIMD-group reductions/shuffles,
texture wrappers holding `thread texture2d<...>*` and `thread sampler*`,
dual-source blending, depth/sample-mask outputs, `[[early_fragment_tests]]`),
and the whole Blender 4.5 corpus (257 libraries: the startup UI, Material\nPreview and Rendered EEVEE shading, the node and image editors). User overloads of standard library names
(`mix`, `clamp`, `min`, ...), early returns and discards from inlined helper
functions, switches with shared case targets, struct copies that clang turns
into wide integer loads (split into narrow loads), scalar-edge
`step`/`smoothstep`. Chrome's generators, from hand-written fixtures in
their styles (design: Tint's and ANGLE's public MSL writers; `tint_*.metal`,
`angle_style.metal`): Tint's `tint_array` with address-space-qualified
`operator[]`, module-variable structs, designated initializers, packed vec3
members, storage-buffer sizes at `buffer(30)`, a workgroup struct at
`[[threadgroup(0)]]`, explicit-order atomics and compare-exchange, volatile
loop guards, every WGSL texture kind; ANGLE's function constants (derived
ones included), guarded depth/sample-mask outputs and parameters, macros
from `MTLCompileOptions`, and texture environments of thread pointers.

Unsupported (explicit errors): `double`, `bfloat`, run-time selected
textures/samplers (a handle not constant after inlining), argument buffers
holding textures, or samplers together with other members, generic pointers stored in memory, array
varyings and array vertex attributes, kernel `[[stage_in]]`,
`[[barycentric_coord]]`, mesh/object/tile/
visible/intersection functions, ray tracing, imageblocks, `memcpy`/`memset`
with run-time length, recursion and non-inlinable calls, irreducible control
flow, vector function constants, pixel-coordinate samplers with bias/gradient
or repeat addressing, `read()` on sampled cube textures, atomics on texture
arrays and 64-bit threadgroup atomics, program-scope variables with dynamic
initializers not computed from function constants, `[[stencil]]` outputs
with `[[function_constant]]`, `constant`/`device` overloads with different
bodies. `Reflection::types` describes scalars,
vectors, matrices and textures; buffer struct pointees are named but carry no
member layout.

## Tests

`alchemy_msl_tests <group>` (ctest `alchemy_msl_*`): `rewriter`, `fixtures`
(every fixture compiles and passes `spirv-val --target-env vulkan1.3`, with
per-library timings and a parallel compile), `reflection` (per fixture),
`link`, `cache`, `preprocessor` (the in-process preprocessor against
`clang -E`, token by token with lines, on synthetic cases, the fixtures and
the corpus), `corpus` (below), `vulkan` (fixtures/vulkan_compute.metal run end
to end through `vulkan-1.dll` on a Vulkan 1.3 device with bufferDeviceAddress
and shaderInt64, checked against a host reference, plus the kernel grid
guard on a non-uniform and an indirect-style dispatch; skipped with a message
without one). `alchemy_msl_smoke` links `alchemy_core_fx` like Metal does.

**Corpus.** `alchemy_msl_corpus` compiles every `<name>.metal` (with its
`<name>.options`: language, fast math, preprocessor macros) that
`ALCHEMY_MSL_DUMP` wrote, on all hardware threads
(`ALCHEMY_MSL_CORPUS_THREADS` overrides), requires every function to pass
spirv-val and prints per-library cold compile times. Blender's generated MSL
is GPL, so the corpus is not in the repository: it lives in
`artifacts/alchemy-blender/msl-corpus` (the CMake cache path
`ALCHEMY_MSL_CORPUS` defaults to it when it exists; the environment variable
of the same name overrides at run time) and the test is skipped with a
message without it. To refresh it, run Blender windowed with
`ALCHEMY_MSL_DUMP=<dir>` and copy the new files in. `tests/corpus_tour.py`
(`--python /Volumes/C/<path>/corpus_tour.py`) makes Blender compile more than
the startup UI: it switches the 3D views to Material Preview and Rendered
(EEVEE) shading and opens the shader, geometry node, image and compositor
editors, waiting at each step until no new library is dumped for 15 s, then
quits. The corpus test also reports prelude use and checks that a sample of
libraries compiles to identical SPIR-V with and without its prelude.
`alchemy_msl_tests compile <file.metal> [out-dir]` compiles one dumped
library with its options.

**Chrome corpus.** `alchemy_msl_chrome_corpus` does the same for the MSL
Chrome's GPU process hands Metal (Dawn/Tint for WebGPU and Skia Graphite,
ANGLE for WebGL): the CMake cache path `ALCHEMY_MSL_CHROME_CORPUS` (default
`artifacts/chrome-msl-corpus` when it exists; the environment variable
overrides) is a root with one `ALCHEMY_MSL_DUMP` directory per Chrome run
below it, read recursively and deduplicated by dump name (the source's
fingerprint). Skipped with a message when absent or empty; the shaders
never enter the repository. With `ALCHEMY_MSL_DUMP` set, a precompiled
library handed to `newLibraryWithData:` (ANGLE's built-in shaders) is
written there too, as `metallib-<hash>.metallib`, byte for byte (input for
evaluating metallib translators; it still fails explicitly in Metal).
