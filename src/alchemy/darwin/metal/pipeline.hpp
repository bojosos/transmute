#pragma once

// Metal libraries, functions and pipeline states on Vulkan (owner: A5).
//
// Shader code comes from the MSL compiler contract
// (include/alchemy/msl/compiler.hpp): one msl::Function per entry point with
// its SPIR-V and Reflection. Pipelines are built from the contract's fixed
// resource model (per-stage descriptor set, argument table at binding 0,
// textures at 16 + n, samplers at 160 + n, constexpr samplers at 176 + k),
// so tests can create them from precompiled SPIR-V and hand-written
// Reflection exactly like the compiler's output.
//
// Specialization: MTLFunctionConstantValues become specialization constants
// (SpecId = function constant index, bool as VkBool32); compute pipelines
// additionally specialize the reserved ids (workgroup size per dispatch,
// threadgroup memory lengths), so a compute pipeline state owns one
// VkPipeline per (threadsPerThreadgroup, threadgroup memory) it was
// dispatched with. Render pipelines own one VkPipeline per primitive
// topology class used (or exactly one when inputPrimitiveTopology is set).
// Everything else Metal sets on the encoder is Vulkan dynamic state.

#include "alchemy/darwin/metal/device.hpp"
#include "alchemy/msl/compiler.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace alchemy::metal {

// MTLDataType scalars used for function constants.
[[nodiscard]] std::uint32_t data_type_size(std::uint32_t data_type);   // 0 = unsupported (vectors, structs)

// MTLFunctionConstantValues contents.
struct ConstantValue {
    std::uint32_t data_type{};
    std::vector<std::uint8_t> bytes;    // natural size of data_type
};
struct ConstantValues {
    std::map<std::uint32_t, ConstantValue> by_index;
    std::map<std::string, ConstantValue> by_name;
};

class ShaderLibrary;

// An MTLFunction: one entry point plus the constant values it was
// specialized with (newFunctionWithName:constantValues:error:).
class ShaderFunction {
public:
    ShaderFunction(std::shared_ptr<Device> device, std::shared_ptr<const msl::Function> function);
    ~ShaderFunction();
    ShaderFunction(const ShaderFunction&) = delete;
    ShaderFunction& operator=(const ShaderFunction&) = delete;
    const std::shared_ptr<Device> device;
    const std::shared_ptr<const msl::Function> function;
    // SpecId -> value bytes (bool already widened to VkBool32).
    std::map<std::uint32_t, std::vector<std::uint8_t>> constants;
    std::string label;
    [[nodiscard]] const std::string& name() const noexcept { return function->name; }
    [[nodiscard]] msl::Stage stage() const noexcept { return function->stage; }
    [[nodiscard]] const msl::Reflection& reflection() const noexcept { return function->reflection; }
    // Applies MTLFunctionConstantValues; returns an error message (Metal's
    // NSError text) for a required constant without a value or a type mismatch.
    [[nodiscard]] std::optional<std::string> specialize(const ConstantValues& values);
    [[nodiscard]] VkShaderModule module();
private:
    std::mutex mutex_;
    VkShaderModule module_{};
};

// An MTLLibrary: the compiled entry points of one source.
class ShaderLibrary {
public:
    ShaderLibrary(std::shared_ptr<Device> owner, std::vector<msl::Function> functions);
    const std::shared_ptr<Device> device;
    std::vector<std::shared_ptr<const msl::Function>> functions;
    std::string label;
    [[nodiscard]] std::shared_ptr<const msl::Function> find(std::string_view name) const;
};

// ---- render pipelines ---------------------------------------------------------------

struct ColorAttachmentState {
    std::uint32_t pixel_format{};
    bool blending{};
    std::uint32_t rgb_operation{}, alpha_operation{};           // MTLBlendOperation
    std::uint32_t source_rgb{1}, destination_rgb{0}, source_alpha{1}, destination_alpha{0};   // MTLBlendFactor
    std::uint32_t write_mask{0xf};                               // MTLColorWriteMask
};
struct VertexAttribute { std::uint32_t format{}, offset{}, buffer_index{}; };
struct VertexLayout { std::uint32_t stride{}, step_function{1}, step_rate{1}; };
struct RenderPipelineDescriptor {
    std::shared_ptr<ShaderFunction> vertex, fragment;
    std::array<ColorAttachmentState, 8> colors{};
    std::uint32_t depth_format{}, stencil_format{};             // MTLPixelFormat
    std::uint32_t sample_count{1};
    bool alpha_to_coverage{}, alpha_to_one{}, rasterization{true};
    std::uint32_t topology_class{};                              // MTLPrimitiveTopologyClass, 0 = unspecified
    std::array<std::optional<VertexAttribute>, 31> attributes{};
    std::array<std::optional<VertexLayout>, 31> layouts{};
    std::string label;
};

// MTLPrimitiveType -> topology and its class (1 point, 2 line, 3 triangle).
[[nodiscard]] VkPrimitiveTopology primitive_topology(std::uint32_t primitive_type);
[[nodiscard]] std::uint32_t topology_class(std::uint32_t primitive_type);

// Descriptor set layout of one stage built from its reflection, with the
// immutable samplers of its constexpr samplers.
struct StageLayout {
    VkDescriptorSetLayout set_layout{};
    std::vector<VkSampler> immutable_samplers;
    std::uint32_t argument_table_size{};
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    // Per Reflection::argument_buffers entry: the [[buffer]] index whose
    // encoded samplers fill its binding (function constant resolved).
    std::vector<std::uint32_t> argument_buffer_indices;
};

// Function-constant values (SpecId -> bytes) of a specialized function.
using SpecializationValues = std::map<std::uint32_t, std::vector<std::uint8_t>>;
// The [[buffer]] index of an argument buffer of samplers: its literal part
// plus the function constant's value (0 when the constant has no value).
[[nodiscard]] std::uint32_t argument_buffer_index(const msl::ArgumentBufferSamplers& buffer, const SpecializationValues* constants);

class RenderPipeline {
public:
    // Throws VulkanError (driver failures) or std::runtime_error with Metal's
    // NSError text (invalid descriptors, link failures).
    static std::shared_ptr<RenderPipeline> create(std::shared_ptr<Device> device, const RenderPipelineDescriptor& descriptor);
    ~RenderPipeline();
    RenderPipeline(const RenderPipeline&) = delete;
    RenderPipeline& operator=(const RenderPipeline&) = delete;
    const std::shared_ptr<Device> device;
    RenderPipelineDescriptor descriptor;
    std::array<StageLayout, 2> stages{};          // vertex (set 0), fragment (set 1)
    VkPipelineLayout layout{};
    // The VkPipeline for an MTLPrimitiveType (created on first use).
    [[nodiscard]] VkPipeline pipeline(std::uint32_t primitive_type);
    // Vertex buffer indices the vertex input state reads (binding = index).
    std::vector<std::uint32_t> vertex_bindings;
    // Reflection of the linked stages (varying locations agree).
    [[nodiscard]] const msl::Reflection& vertex_reflection() const noexcept { return linked_vertex_.reflection; }
    [[nodiscard]] const msl::Reflection* fragment_reflection() const noexcept {
        return descriptor.fragment ? &linked_fragment_.reflection : nullptr;
    }
private:
    explicit RenderPipeline(std::shared_ptr<Device> owner) : device(std::move(owner)) {}
    VkPipeline build(std::uint32_t topology_class);
    VkPipeline build_locked(std::uint32_t topology_class);
    std::mutex mutex_;
    std::array<VkPipeline, 4> pipelines_{};      // by topology class
    msl::Function linked_vertex_, linked_fragment_;
    VkShaderModule vertex_module_{}, fragment_module_{};
};

// ---- compute pipelines ----------------------------------------------------------------

class ComputePipeline {
public:
    static std::shared_ptr<ComputePipeline> create(std::shared_ptr<Device> device, std::shared_ptr<ShaderFunction> function,
                                                   std::uint32_t max_total_threads = 0);
    ~ComputePipeline();
    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;
    const std::shared_ptr<Device> device;
    const std::shared_ptr<ShaderFunction> function;
    StageLayout stage;
    VkPipelineLayout layout{};
    std::uint32_t max_total_threads{}, thread_execution_width{};
    std::uint32_t static_threadgroup_memory{};
    // The kernel declares the push-constant grid block (kernel_push_constant_size),
    // so it can mask the threads of a non-uniform dispatchThreads: grid.
    bool reads_grid{};
    std::string label;
    // The VkPipeline for one dispatch shape.
    [[nodiscard]] VkPipeline pipeline(const std::array<std::uint32_t, 3>& threads_per_threadgroup,
                                      const std::vector<std::uint32_t>& threadgroup_memory);
private:
    explicit ComputePipeline(std::shared_ptr<Device> owner, std::shared_ptr<ShaderFunction> shader)
        : device(std::move(owner)), function(std::move(shader)) {}
    std::mutex mutex_;
    std::map<std::vector<std::uint32_t>, VkPipeline> variants_;
};

// Kernel push constants: the grid of the dispatch (see the contract
// addition in include/alchemy/msl/compiler.hpp, kernel_push_constants).
struct KernelPushConstants { std::uint32_t threads_per_grid[3]; std::uint32_t non_uniform; };

// ---- depth/stencil ----------------------------------------------------------------------

struct StencilState {
    std::uint32_t compare{7}, stencil_failure{}, depth_failure{}, pass{};   // MTLCompareFunction / MTLStencilOperation
    std::uint32_t read_mask{0xffffffff}, write_mask{0xffffffff};
    [[nodiscard]] bool active() const noexcept { return compare != 7 || stencil_failure || depth_failure || pass; }
};
struct DepthStencilState {
    std::uint32_t depth_compare{7};   // MTLCompareFunctionAlways
    bool depth_write{};
    StencilState front, back;
    std::string label;
};

// Builds a stage's descriptor set layout from its reflection.
[[nodiscard]] StageLayout make_stage_layout(Device& device, const msl::Reflection* reflection, VkShaderStageFlags stages,
                                            const SpecializationValues* constants = nullptr);
void destroy_stage_layout(Device& device, StageLayout& layout);

} // namespace alchemy::metal
