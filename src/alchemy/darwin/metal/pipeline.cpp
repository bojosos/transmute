#include "alchemy/darwin/metal/pipeline.hpp"

#include "alchemy/darwin/metal/formats.hpp"
#include "alchemy/darwin/metal/resources.hpp"
#include "alchemy/perf.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <span>
#include <stdexcept>

// Metal pipelines on Vulkan. See pipeline.hpp.
namespace alchemy::metal {
namespace {

// Location -> signedness (true: signed) of every Input variable of integer
// scalar or vector type in a SPIR-V module (vertex attributes).
std::map<std::uint32_t, bool> integer_input_signedness(std::span<const std::uint32_t> spirv) {
    constexpr std::uint32_t op_decorate = 71, op_type_int = 21, op_type_vector = 23, op_type_pointer = 32,
        op_variable = 59, decoration_location = 30, storage_input = 1;
    std::map<std::uint32_t, std::uint32_t> locations;    // variable id -> location
    std::map<std::uint32_t, bool> integers;              // int type id -> signed
    std::map<std::uint32_t, std::uint32_t> vectors;      // vector type id -> component type id
    std::map<std::uint32_t, std::uint32_t> pointers;     // Input pointer type id -> pointee type id
    std::map<std::uint32_t, std::uint32_t> variables;    // Input variable id -> pointer type id
    for (std::size_t at = 5; at < spirv.size();) {
        const auto count = spirv[at] >> 16, opcode = spirv[at] & 0xffff;
        if (count == 0 || at + count > spirv.size()) break;
        const auto* word = spirv.data() + at;
        if (opcode == op_decorate && count >= 4 && word[2] == decoration_location) locations[word[1]] = word[3];
        else if (opcode == op_type_int && count >= 4) integers[word[1]] = word[3] != 0;
        else if (opcode == op_type_vector && count >= 4) vectors[word[1]] = word[2];
        else if (opcode == op_type_pointer && count >= 4 && word[2] == storage_input) pointers[word[1]] = word[3];
        else if (opcode == op_variable && count >= 4 && word[3] == storage_input) variables[word[2]] = word[1];
        at += count;
    }
    std::map<std::uint32_t, bool> result;
    for (const auto& [variable, pointer] : variables) {
        const auto location = locations.find(variable);
        const auto pointee = pointers.find(pointer);
        if (location == locations.end() || pointee == pointers.end()) continue;
        auto type = pointee->second;
        if (const auto vector = vectors.find(type); vector != vectors.end()) type = vector->second;
        if (const auto integer = integers.find(type); integer != integers.end()) result[location->second] = integer->second;
    }
    return result;
}

// MTLBlendFactor -> VkBlendFactor (the enumerations differ in order).
VkBlendFactor blend_factor(std::uint32_t value) {
    constexpr VkBlendFactor table[] = {VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_DST_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_CONSTANT_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR, VK_BLEND_FACTOR_SRC1_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA};
    if (value >= std::size(table)) throw std::runtime_error("invalid MTLBlendFactor " + std::to_string(value));
    return table[value];
}

VkColorComponentFlags write_mask(std::uint32_t metal) {
    // MTLColorWriteMask: Red 8, Green 4, Blue 2, Alpha 1.
    VkColorComponentFlags result{};
    if (metal & 8) result |= VK_COLOR_COMPONENT_R_BIT;
    if (metal & 4) result |= VK_COLOR_COMPONENT_G_BIT;
    if (metal & 2) result |= VK_COLOR_COMPONENT_B_BIT;
    if (metal & 1) result |= VK_COLOR_COMPONENT_A_BIT;
    return result;
}

VkDescriptorType descriptor_type(msl::DescriptorType type) {
    switch (type) {
    case msl::DescriptorType::uniform_buffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case msl::DescriptorType::storage_buffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case msl::DescriptorType::sampled_image: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case msl::DescriptorType::storage_image: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    case msl::DescriptorType::sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
    case msl::DescriptorType::uniform_texel_buffer: return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    case msl::DescriptorType::storage_texel_buffer: return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    }
    throw std::runtime_error("invalid MSL descriptor type");
}

VkShaderModule create_module(Device& device, const std::vector<std::uint32_t>& code, const std::string& name) {
    if (code.empty()) throw std::runtime_error("Metal function " + name + " has no SPIR-V");
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = code.size() * sizeof(std::uint32_t);
    info.pCode = code.data();
    VkShaderModule module{};
    check(device.vk.vkCreateShaderModule(device.device, &info, nullptr, &module), "vkCreateShaderModule");
    device.set_name(VK_OBJECT_TYPE_SHADER_MODULE, reinterpret_cast<std::uint64_t>(module), name);
    return module;
}

// Specialization data built from SpecId -> bytes.
struct Specialization {
    std::vector<VkSpecializationMapEntry> entries;
    std::vector<std::uint8_t> data;
    VkSpecializationInfo info{};
    void add(std::uint32_t id, const void* bytes, std::size_t size) {
        entries.push_back({id, static_cast<std::uint32_t>(data.size()), size});
        const auto* source = static_cast<const std::uint8_t*>(bytes);
        data.insert(data.end(), source, source + size);
    }
    const VkSpecializationInfo* finish() {
        if (entries.empty()) return nullptr;
        info.mapEntryCount = static_cast<std::uint32_t>(entries.size());
        info.pMapEntries = entries.data();
        info.dataSize = data.size();
        info.pData = data.data();
        return &info;
    }
};

// Saves the pipeline cache at most every few seconds while pipelines are
// being created (Blender compiles hundreds at startup).
void note_pipeline_created(Device& device) {
    device.note_pipeline_cache_dirty();
    static std::mutex mutex;
    static auto last = std::chrono::steady_clock::now();
    std::lock_guard lock(mutex);
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(5)) return;
    last = now;
    try { device.save_pipeline_cache(); } catch (const std::exception&) {}
}

void add_required_subgroup_size(const Device& device, VkPipelineShaderStageCreateInfo& stage,
                                VkPipelineShaderStageRequiredSubgroupSizeCreateInfo& required) {
    if (!device.features13.subgroupSizeControl) return;
    VkPhysicalDeviceSubgroupSizeControlProperties control{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &control;
    device.vk.vkGetPhysicalDeviceProperties2(device.physical, &properties);
    const auto width = device.profile().thread_execution_width;
    if (!(control.requiredSubgroupSizeStages & stage.stage) || width < control.minSubgroupSize ||
        width > control.maxSubgroupSize)
        return;
    required = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
    required.requiredSubgroupSize = width;
    stage.pNext = &required;
}

} // namespace

std::uint32_t data_type_size(std::uint32_t type) {
    switch (type) {
    case 3: case 29: case 33: return 4;    // Float, Int, UInt
    case 16: case 37: case 41: return 2;   // Half, Short, UShort
    case 45: case 49: case 53: return 1;   // Char, UChar, Bool
    case 81: case 85: return 8;            // Long, ULong
    case 121: return 2;                    // BFloat
    default: return 0;
    }
}

// ---- functions ------------------------------------------------------------------------

ShaderFunction::ShaderFunction(std::shared_ptr<Device> owner, std::shared_ptr<const msl::Function> value)
    : device(std::move(owner)), function(std::move(value)) {}

ShaderFunction::~ShaderFunction() {
    if (!module_) return;
    Device* owner = device.get();
    const auto handle = module_;
    owner->defer([owner, handle] { owner->vk.vkDestroyShaderModule(owner->device, handle, nullptr); });
}

std::optional<std::string> ShaderFunction::specialize(const ConstantValues& values) {
    for (const auto& constant : reflection().function_constants) {
        const ConstantValue* value = nullptr;
        if (const auto found = values.by_index.find(constant.index); found != values.by_index.end()) value = &found->second;
        else if (const auto named = values.by_name.find(constant.name); named != values.by_name.end()) value = &named->second;
        if (!value) {
            if (constant.required)
                return "Function " + name() + " has a function constant " + constant.name + " (index " +
                       std::to_string(constant.index) + ") without a value";
            continue;
        }
        const auto& type = reflection().types.at(constant.type);
        if (type.kind != msl::Type::Kind::scalar)
            return "function constant " + constant.name + ": vector function constants are not translated";
        auto bytes = value->bytes;
        if (type.scalar == msl::Scalar::boolean) {
            const std::uint32_t widened = std::any_of(bytes.begin(), bytes.end(), [](auto b) { return b != 0; }) ? 1U : 0U;
            bytes.assign(reinterpret_cast<const std::uint8_t*>(&widened), reinterpret_cast<const std::uint8_t*>(&widened) + 4);
        } else if (bytes.size() != type.size && type.size) {
            return "function constant " + constant.name + " has a value of the wrong size";
        }
        constants[constant.index] = std::move(bytes);
        // is_function_constant_defined(constant) is true from here on.
        const std::uint32_t defined = 1;
        constants[msl::reserved_spec_id::function_constant_defined_base + constant.index].assign(
            reinterpret_cast<const std::uint8_t*>(&defined), reinterpret_cast<const std::uint8_t*>(&defined) + 4);
    }
    // ALCHEMY_METAL_TRACE_CONSTANTS=1: the specialization of every function.
    static const bool trace = [] {
        char value[8]{};
        std::size_t length{};
        return getenv_s(&length, value, sizeof(value), "ALCHEMY_METAL_TRACE_CONSTANTS") == 0 && length > 1 && value[0] != '0';
    }();
    if (trace) {
        std::string line = "[metal constants] " + name();
        for (const auto& constant : reflection().function_constants) {
            line += " " + (constant.name.empty() ? std::to_string(constant.index) : constant.name) + "=";
            const auto found = constants.find(constant.index);
            if (found == constants.end()) { line += "unset"; continue; }
            std::uint64_t raw{};
            std::memcpy(&raw, found->second.data(), std::min<std::size_t>(8, found->second.size()));
            line += "0x" + [&] { char buffer[24]; std::snprintf(buffer, sizeof(buffer), "%llx", static_cast<unsigned long long>(raw)); return std::string(buffer); }();
        }
        std::fprintf(stderr, "%s\n", line.c_str());
    }
    return std::nullopt;
}

VkShaderModule ShaderFunction::module() {
    std::lock_guard lock(mutex_);
    if (!module_) module_ = create_module(*device, function->spirv, function->name);
    return module_;
}

ShaderLibrary::ShaderLibrary(std::shared_ptr<Device> owner, std::vector<msl::Function> list) : device(std::move(owner)) {
    for (auto& function : list) functions.push_back(std::make_shared<const msl::Function>(std::move(function)));
}

std::shared_ptr<const msl::Function> ShaderLibrary::find(std::string_view name) const {
    for (const auto& function : functions)
        if (function->name == name) return function;
    return nullptr;
}

// ---- layouts --------------------------------------------------------------------------

std::uint32_t argument_buffer_index(const msl::ArgumentBufferSamplers& buffer, const SpecializationValues* constants) {
    std::int64_t index = buffer.index;
    if (buffer.index_function_constant && constants) {
        if (const auto found = constants->find(*buffer.index_function_constant); found != constants->end()) {
            const auto& bytes = found->second;
            std::int64_t value{};
            if (bytes.size() >= 4) { std::int32_t word{}; std::memcpy(&word, bytes.data(), 4); value = word; }
            else if (bytes.size() == 2) { std::int16_t half{}; std::memcpy(&half, bytes.data(), 2); value = half; }
            else if (bytes.size() == 1) value = static_cast<std::int8_t>(bytes[0]);
            index += value;
        }
    }
    if (index < 0 || index >= 31)
        throw std::runtime_error("argument buffer '" + buffer.name + "' resolves to buffer index " + std::to_string(index));
    return static_cast<std::uint32_t>(index);
}

StageLayout make_stage_layout(Device& device, const msl::Reflection* reflection, VkShaderStageFlags stages,
                              const SpecializationValues* constants) {
    StageLayout result;
    if (reflection) {
        result.argument_table_size = reflection->argument_table_size;
        if (reflection->argument_table_size)
            result.bindings.push_back({msl::argument_table_binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr});
        for (const auto& texture : reflection->textures)
            result.bindings.push_back({texture.descriptor.binding, descriptor_type(texture.descriptor.type),
                                       std::max(1U, texture.descriptor.count), stages, nullptr});
        for (const auto& sampler : reflection->samplers)
            result.bindings.push_back({sampler.descriptor.binding, VK_DESCRIPTOR_TYPE_SAMPLER,
                                       std::max(1U, sampler.descriptor.count), stages, nullptr});
        // Argument buffers of samplers: one sampler array each, filled at
        // draw time from the MTLSamplerStates encoded into the bound buffer.
        for (const auto& buffer : reflection->argument_buffers) {
            result.bindings.push_back({buffer.descriptor.binding, VK_DESCRIPTOR_TYPE_SAMPLER,
                                       std::max(1U, buffer.descriptor.count), stages, nullptr});
            result.argument_buffer_indices.push_back(argument_buffer_index(buffer, constants));
        }
        // Framebuffer fetch: [[color(n)]] inputs are input attachments.
        if (!reflection->color_inputs.empty() && !device.dynamic_rendering_local_read)
            throw std::runtime_error("framebuffer fetch ([[color(n)]] fragment inputs) needs "
                                     "VK_KHR_dynamic_rendering_local_read, which " + device.profile().name + " lacks");
        for (const auto& input : reflection->color_inputs)
            result.bindings.push_back({input.descriptor.binding, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, stages, nullptr});
        result.immutable_samplers.reserve(reflection->constexpr_samplers.size());
        for (const auto& constant : reflection->constexpr_samplers) {
            SamplerDescriptor descriptor;
            descriptor.min_filter = constant.min_filter;
            descriptor.mag_filter = constant.mag_filter;
            descriptor.mip_filter = constant.mip_filter;
            descriptor.address = constant.address;
            descriptor.compare_function = constant.compare_function;
            descriptor.border_color = constant.border_color;
            descriptor.max_anisotropy = constant.max_anisotropy;
            descriptor.lod_min = constant.lod_min;
            descriptor.lod_max = constant.lod_max;
            descriptor.normalized_coordinates = constant.normalized_coordinates;
            const auto info = sampler_info(device, descriptor);
            VkSampler sampler{};
            check(device.vk.vkCreateSampler(device.device, &info, nullptr, &sampler), "vkCreateSampler");
            result.immutable_samplers.push_back(sampler);
        }
        for (std::size_t index = 0; index < reflection->constexpr_samplers.size(); ++index)
            result.bindings.push_back({reflection->constexpr_samplers[index].descriptor.binding, VK_DESCRIPTOR_TYPE_SAMPLER,
                                       1, stages, &result.immutable_samplers[index]});
    }
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = static_cast<std::uint32_t>(result.bindings.size());
    info.pBindings = result.bindings.data();
    check(device.vk.vkCreateDescriptorSetLayout(device.device, &info, nullptr, &result.set_layout),
          "vkCreateDescriptorSetLayout");
    return result;
}

void destroy_stage_layout(Device& device, StageLayout& layout) {
    if (layout.set_layout) device.vk.vkDestroyDescriptorSetLayout(device.device, layout.set_layout, nullptr);
    for (const auto sampler : layout.immutable_samplers) device.vk.vkDestroySampler(device.device, sampler, nullptr);
    layout = {};
}

// ---- render pipelines ------------------------------------------------------------------

VkPrimitiveTopology primitive_topology(std::uint32_t type) {
    switch (type) {
    case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 2: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 3: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case 4: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    default: throw std::runtime_error("invalid MTLPrimitiveType " + std::to_string(type));
    }
}

std::uint32_t topology_class(std::uint32_t type) {
    switch (type) {
    case 0: return 1;
    case 1: case 2: return 2;
    default: return 3;
    }
}

std::shared_ptr<RenderPipeline> RenderPipeline::create(std::shared_ptr<Device> device, const RenderPipelineDescriptor& descriptor) {
    if (!descriptor.vertex) throw std::runtime_error("No vertex function set");
    if (descriptor.vertex->stage() != msl::Stage::vertex)
        throw std::runtime_error("vertexFunction " + descriptor.vertex->name() + " is not a vertex function");
    if (descriptor.fragment && descriptor.fragment->stage() != msl::Stage::fragment)
        throw std::runtime_error("fragmentFunction " + descriptor.fragment->name() + " is not a fragment function");
    std::shared_ptr<RenderPipeline> result(new RenderPipeline(std::move(device)));
    auto& d = *result->device;
    result->descriptor = descriptor;
    result->linked_vertex_ = *descriptor.vertex->function;
    if (descriptor.fragment) {
        result->linked_fragment_ = *descriptor.fragment->function;
        // Varyings agree already when every fragment input has a vertex output
        // of the same name at the same location; otherwise the stage linker
        // renumbers them as Metal's pipeline creation would.
        const auto& outputs = descriptor.vertex->reflection().outputs;
        bool agree = true;
        for (const auto& input : descriptor.fragment->reflection().inputs) {
            const auto found = std::find_if(outputs.begin(), outputs.end(), [&](const auto& output) { return output.name == input.name; });
            if (found == outputs.end() || found->location != input.location) { agree = false; break; }
        }
        if (!agree) {
            auto linked = msl::link(*descriptor.vertex->function, *descriptor.fragment->function);
            if (!linked.success()) throw std::runtime_error(msl::format_diagnostics(linked.diagnostics));
            result->linked_vertex_ = std::move(linked.vertex);
            result->linked_fragment_ = std::move(linked.fragment);
        }
    }
    result->stages[0] = make_stage_layout(d, &result->linked_vertex_.reflection, VK_SHADER_STAGE_VERTEX_BIT,
                                          &descriptor.vertex->constants);
    result->stages[1] = make_stage_layout(d, descriptor.fragment ? &result->linked_fragment_.reflection : nullptr,
                                          VK_SHADER_STAGE_FRAGMENT_BIT,
                                          descriptor.fragment ? &descriptor.fragment->constants : nullptr);
    const VkDescriptorSetLayout sets[] = {result->stages[0].set_layout, result->stages[1].set_layout};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 2;
    layout.pSetLayouts = sets;
    check(d.vk.vkCreatePipelineLayout(d.device, &layout, nullptr, &result->layout), "vkCreatePipelineLayout");
    result->vertex_module_ = create_module(d, result->linked_vertex_.spirv, result->linked_vertex_.name);
    if (descriptor.fragment)
        result->fragment_module_ = create_module(d, result->linked_fragment_.spirv, result->linked_fragment_.name);
    for (std::uint32_t index = 0; index < descriptor.layouts.size(); ++index) {
        bool used{};
        for (const auto& attribute : descriptor.attributes) used |= attribute && attribute->format && attribute->buffer_index == index;
        if (used) result->vertex_bindings.push_back(index);
    }
    // Pipelines with a declared topology class compile now, like Metal.
    if (descriptor.topology_class) static_cast<void>(result->build(descriptor.topology_class));
    else static_cast<void>(result->build(3));
    return result;
}

RenderPipeline::~RenderPipeline() {
    Device* owner = device.get();
    auto pipelines = pipelines_;
    auto stage_layouts = stages;
    const auto pipeline_layout = layout;
    const auto modules = std::array<VkShaderModule, 2>{vertex_module_, fragment_module_};
    owner->defer([owner, pipelines, stage_layouts, pipeline_layout, modules]() mutable {
        for (const auto pipeline : pipelines) if (pipeline) owner->vk.vkDestroyPipeline(owner->device, pipeline, nullptr);
        if (pipeline_layout) owner->vk.vkDestroyPipelineLayout(owner->device, pipeline_layout, nullptr);
        for (auto& stage : stage_layouts) destroy_stage_layout(*owner, stage);
        for (const auto module : modules) if (module) owner->vk.vkDestroyShaderModule(owner->device, module, nullptr);
    });
}

VkPipeline RenderPipeline::pipeline(std::uint32_t primitive_type) {
    const auto wanted = descriptor.topology_class ? descriptor.topology_class : topology_class(primitive_type);
    if (descriptor.topology_class && descriptor.topology_class != topology_class(primitive_type))
        throw std::runtime_error("draw primitive type does not match the pipeline's inputPrimitiveTopology");
    std::lock_guard lock(mutex_);
    if (pipelines_[wanted]) return pipelines_[wanted];
    return build_locked(wanted);
}

VkPipeline RenderPipeline::build(std::uint32_t wanted) {
    std::lock_guard lock(mutex_);
    if (pipelines_[wanted]) return pipelines_[wanted];
    return build_locked(wanted);
}

VkPipeline RenderPipeline::build_locked(std::uint32_t wanted) {
    auto& d = *device;
    Specialization vertex_constants, fragment_constants;
    for (const auto& [id, bytes] : descriptor.vertex->constants) vertex_constants.add(id, bytes.data(), bytes.size());
    if (descriptor.fragment)
        for (const auto& [id, bytes] : descriptor.fragment->constants) fragment_constants.add(id, bytes.data(), bytes.size());
    // metal::get_num_samples() in either stage.
    const std::uint32_t samples = std::max(1U, descriptor.sample_count);
    vertex_constants.add(msl::reserved_spec_id::raster_sample_count, &samples, 4);
    fragment_constants.add(msl::reserved_spec_id::raster_sample_count, &samples, 4);
    VkPipelineShaderStageCreateInfo shader_stages[2]{};
    shader_stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shader_stages[0].module = vertex_module_;
    shader_stages[0].pName = linked_vertex_.name.c_str();
    shader_stages[0].pSpecializationInfo = vertex_constants.finish();
    std::uint32_t stage_count = 1;
    if (descriptor.fragment && descriptor.rasterization) {
        shader_stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        shader_stages[1].module = fragment_module_;
        shader_stages[1].pName = linked_fragment_.name.c_str();
        shader_stages[1].pSpecializationInfo = fragment_constants.finish();
        stage_count = 2;
    }

    // Vertex input: binding = Metal buffer index.
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (const auto index : vertex_bindings) {
        const auto layout = descriptor.layouts[index].value_or(VertexLayout{});
        VkVertexInputBindingDescription binding{index, layout.stride, VK_VERTEX_INPUT_RATE_VERTEX};
        switch (layout.step_function) {
        case 0: binding.stride = 0; break;   // constant: every vertex reads element 0
        case 1: break;
        case 2:
            binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
            if (layout.step_rate != 1)
                throw std::runtime_error("MTLVertexStepFunctionPerInstance with stepRate " + std::to_string(layout.step_rate) +
                                         " is not translated");
            break;
        default: throw std::runtime_error("tessellation vertex step functions are not translated");
        }
        bindings.push_back(binding);
    }
    const auto integer_inputs = integer_input_signedness(linked_vertex_.spirv);
    for (std::uint32_t location = 0; location < descriptor.attributes.size(); ++location) {
        const auto& attribute = descriptor.attributes[location];
        if (!attribute || !attribute->format) continue;
        const auto* info = vertex_format_info(attribute->format);
        if (!info) throw std::runtime_error("unsupported MTLVertexFormat " + std::to_string(attribute->format));
        auto format = info->vulkan;
        VkFormatProperties properties{};
        d.vk.vkGetPhysicalDeviceFormatProperties(d.physical, format, &properties);
        if (!(properties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
            // Three-component 8/16-bit formats read as four components (the
            // shader ignores the extra one); others are untranslatable here.
            if (!info->four_component)
                throw std::runtime_error("MTLVertexFormat " + std::to_string(attribute->format) +
                                         " is not supported as a vertex format by " + d.profile().name);
            format = info->four_component;
        }
        // The SPIR-V input's integer signedness decides an integer format's
        // (the module, not the MSL spelling: the lowering may type an int2
        // attribute as a vector of unsigned 32-bit integers).
        if (const auto found = integer_inputs.find(location); found != integer_inputs.end())
            format = vertex_format_with_signedness(format, found->second);
        attributes.push_back({location, attribute->buffer_index, format, attribute->offset});
    }
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = static_cast<std::uint32_t>(bindings.size());
    vertex_input.pVertexBindingDescriptions = bindings.data();
    vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = wanted == 1 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                      : wanted == 2 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.rasterizerDiscardEnable = descriptor.rasterization ? VK_FALSE : VK_TRUE;
    raster.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = static_cast<VkSampleCountFlagBits>(std::max(1U, descriptor.sample_count));
    multisample.alphaToCoverageEnable = descriptor.alpha_to_coverage;
    multisample.alphaToOneEnable = descriptor.alpha_to_one && d.features10.alphaToOne;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};

    std::vector<VkFormat> color_formats;
    std::vector<VkPipelineColorBlendAttachmentState> blends;
    std::uint32_t color_count = 0;
    for (std::uint32_t index = 0; index < descriptor.colors.size(); ++index)
        if (descriptor.colors[index].pixel_format) color_count = index + 1;
    for (std::uint32_t index = 0; index < color_count; ++index) {
        const auto& color = descriptor.colors[index];
        const auto format = color.pixel_format ? d.texture_format(color.pixel_format) : VK_FORMAT_UNDEFINED;
        if (color.pixel_format && format == VK_FORMAT_UNDEFINED)
            throw std::runtime_error("unsupported color attachment MTLPixelFormat " + std::to_string(color.pixel_format));
        color_formats.push_back(format);
        VkPipelineColorBlendAttachmentState blend{};
        const auto* info = pixel_format_info(color.pixel_format);
        blend.blendEnable = color.blending && info && !info->integer() ? VK_TRUE : VK_FALSE;
        blend.srcColorBlendFactor = blend_factor(color.source_rgb);
        blend.dstColorBlendFactor = blend_factor(color.destination_rgb);
        blend.colorBlendOp = static_cast<VkBlendOp>(color.rgb_operation);   // MTLBlendOperation == VkBlendOp order
        blend.srcAlphaBlendFactor = blend_factor(color.source_alpha);
        blend.dstAlphaBlendFactor = blend_factor(color.destination_alpha);
        blend.alphaBlendOp = static_cast<VkBlendOp>(color.alpha_operation);
        blend.colorWriteMask = color.pixel_format ? write_mask(color.write_mask) : 0;
        blends.push_back(blend);
    }
    VkPipelineColorBlendStateCreateInfo blend_state{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend_state.attachmentCount = static_cast<std::uint32_t>(blends.size());
    blend_state.pAttachments = blends.data();

    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
        VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
        VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(std::size(dynamic_states));
    dynamic.pDynamicStates = dynamic_states;

    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = static_cast<std::uint32_t>(color_formats.size());
    rendering.pColorAttachmentFormats = color_formats.data();
    if (descriptor.depth_format) {
        rendering.depthAttachmentFormat = d.texture_format(descriptor.depth_format);
        if (!pixel_format_info(descriptor.depth_format) || !pixel_format_info(descriptor.depth_format)->has_depth())
            throw std::runtime_error("depthAttachmentPixelFormat is not a depth format");
    }
    if (descriptor.stencil_format) {
        rendering.stencilAttachmentFormat = d.texture_format(descriptor.stencil_format);
        if (!pixel_format_info(descriptor.stencil_format) || !pixel_format_info(descriptor.stencil_format)->has_stencil())
            throw std::runtime_error("stencilAttachmentPixelFormat is not a stencil format");
    }

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = stage_count;
    info.pStages = shader_stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend_state;
    info.pDynamicState = &dynamic;
    info.layout = layout;
    VkPipeline result{};
    {
        perf::Scope timed(perf::mtl_pipeline_create_ticks, perf::mtl_pipelines_created);
        check(d.vk.vkCreateGraphicsPipelines(d.device, d.pipeline_cache, 1, &info, nullptr, &result), "vkCreateGraphicsPipelines");
    }
    perf::phase("first VkPipeline created");
    d.set_name(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<std::uint64_t>(result), descriptor.label);
    pipelines_[wanted] = result;
    note_pipeline_created(d);
    return result;
}

// ---- compute pipelines ---------------------------------------------------------------------

std::shared_ptr<ComputePipeline> ComputePipeline::create(std::shared_ptr<Device> device,
    std::shared_ptr<ShaderFunction> function, std::uint32_t max_total_threads) {
    if (!function) throw std::runtime_error("No compute function set");
    if (function->stage() != msl::Stage::kernel) throw std::runtime_error("function " + function->name() + " is not a kernel");
    std::shared_ptr<ComputePipeline> result(new ComputePipeline(std::move(device), std::move(function)));
    auto& d = *result->device;
    const auto& reflection = result->function->reflection();
    result->stage = make_stage_layout(d, &reflection, VK_SHADER_STAGE_COMPUTE_BIT, &result->function->constants);
    // OpVariable (59) in StorageClass PushConstant (9).
    const auto& code = result->function->function->spirv;
    for (std::size_t index = 5; index < code.size() && !result->reads_grid;) {
        const auto count = code[index] >> 16;
        if (!count) break;
        if ((code[index] & 0xffff) == 59 && count >= 4 && code[index + 3] == 9) result->reads_grid = true;
        index += count;
    }
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, msl::kernel_push_constant_size};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &result->stage.set_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    check(d.vk.vkCreatePipelineLayout(d.device, &layout, nullptr, &result->layout), "vkCreatePipelineLayout");
    auto maximum = d.profile().max_total_threads_per_threadgroup;
    if (reflection.max_total_threads_per_threadgroup) maximum = std::min(maximum, reflection.max_total_threads_per_threadgroup);
    if (max_total_threads) maximum = std::min(maximum, max_total_threads);
    result->max_total_threads = maximum;
    result->thread_execution_width = d.profile().thread_execution_width;
    // Metal compiles the pipeline at creation; the first variant (one
    // SIMD-group wide) validates the SPIR-V now.
    static_cast<void>(result->pipeline({std::min(maximum, result->thread_execution_width), 1, 1}, {}));
    return result;
}

ComputePipeline::~ComputePipeline() {
    Device* owner = device.get();
    std::vector<VkPipeline> pipelines;
    for (const auto& [key, value] : variants_) pipelines.push_back(value);
    auto stage_layout = stage;
    const auto pipeline_layout = layout;
    owner->defer([owner, pipelines, stage_layout, pipeline_layout]() mutable {
        for (const auto pipeline : pipelines) owner->vk.vkDestroyPipeline(owner->device, pipeline, nullptr);
        if (pipeline_layout) owner->vk.vkDestroyPipelineLayout(owner->device, pipeline_layout, nullptr);
        destroy_stage_layout(*owner, stage_layout);
    });
}

VkPipeline ComputePipeline::pipeline(const std::array<std::uint32_t, 3>& threads, const std::vector<std::uint32_t>& memory) {
    std::vector<std::uint32_t> key{threads[0], threads[1], threads[2]};
    key.insert(key.end(), memory.begin(), memory.end());
    std::lock_guard lock(mutex_);
    if (const auto found = variants_.find(key); found != variants_.end()) return found->second;
    if (static_cast<std::uint64_t>(threads[0]) * threads[1] * threads[2] > max_total_threads)
        throw std::runtime_error("threadsPerThreadgroup exceeds maxTotalThreadsPerThreadgroup (" +
                                 std::to_string(max_total_threads) + ")");
    auto& d = *device;
    Specialization constants;
    for (const auto& [id, bytes] : function->constants) constants.add(id, bytes.data(), bytes.size());
    constants.add(msl::reserved_spec_id::workgroup_size_x, &threads[0], 4);
    constants.add(msl::reserved_spec_id::workgroup_size_y, &threads[1], 4);
    constants.add(msl::reserved_spec_id::workgroup_size_z, &threads[2], 4);
    for (const auto& group : function->reflection().threadgroup_memory) {
        const std::uint32_t length = group.index < memory.size() ? memory[group.index] : 0;
        constants.add(group.spec_id ? group.spec_id : msl::reserved_spec_id::threadgroup_memory_base + group.index, &length, 4);
    }
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = function->module();
    info.stage.pName = function->name().c_str();
    info.stage.pSpecializationInfo = constants.finish();
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo required{};
    add_required_subgroup_size(d, info.stage, required);
    info.layout = layout;
    VkPipeline result{};
    {
        perf::Scope timed(perf::mtl_pipeline_create_ticks, perf::mtl_pipelines_created);
        check(d.vk.vkCreateComputePipelines(d.device, d.pipeline_cache, 1, &info, nullptr, &result), "vkCreateComputePipelines");
    }
    d.set_name(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<std::uint64_t>(result), label.empty() ? function->name() : label);
    variants_.emplace(std::move(key), result);
    note_pipeline_created(d);
    return result;
}

} // namespace alchemy::metal
