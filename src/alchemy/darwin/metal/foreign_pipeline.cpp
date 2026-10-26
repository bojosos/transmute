// Vulkan objects for translated pipelines (foreign_pipeline.hpp).
#include "alchemy/darwin/metal/foreign_pipeline.hpp"
#include "alchemy/darwin/metal/pipeline.hpp"

#include "alchemy/perf.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace alchemy::metal::air {

namespace {

std::uint32_t u32(const json::Value& value, const char* what) {
    const auto v = value.as_u64();
    if (!v || *v > 0xffffffffULL) throw std::runtime_error(std::string("translated pipeline: ") + what + " is not a 32-bit number");
    return static_cast<std::uint32_t>(*v);
}

std::uint32_t u32_or(const json::Value& value, std::uint32_t fallback) {
    const auto v = value.as_u64();
    return v && *v <= 0xffffffffULL ? static_cast<std::uint32_t>(*v) : fallback;
}

float f32(const json::Value& value, float fallback) {
    const auto v = value.as_double();
    return v ? static_cast<float>(*v) : fallback;
}

// A SpecializationKey ({"data": [bytes], "entries": [{"constant_id", "offset", "size"}]})
// or a stage's specialization layout's defaults, as Vulkan specialization info
// whose storage `data` and `entries` hold.
struct Specialization {
    std::vector<std::uint8_t> data;
    std::vector<VkSpecializationMapEntry> entries;
    VkSpecializationInfo info{};

    void from_key(const json::Value& key) {
        for (const auto& byte : key["data"].items()) data.push_back(static_cast<std::uint8_t>(u32(byte, "specialization byte")));
        for (const auto& e : key["entries"].items())
            entries.push_back({u32(e["constant_id"], "constant_id"), u32(e["offset"], "offset"), u32(e["size"], "size")});
        finish();
    }
    void from_defaults(const json::Value& layout) {
        for (const auto& c : layout["constants"].items()) {
            const auto size = u32(c["size"], "constant size");
            const auto value = c["default"].as_u64().value_or(0);
            const auto offset = static_cast<std::uint32_t>(data.size());
            for (std::uint32_t i = 0; i < size; ++i) data.push_back(static_cast<std::uint8_t>(i < 8 ? value >> (8 * i) : 0));
            entries.push_back({u32(c["id"], "constant id"), offset, size});
        }
        finish();
    }
    void finish() {
        info.mapEntryCount = static_cast<std::uint32_t>(entries.size());
        info.pMapEntries = entries.data();
        info.dataSize = data.size();
        info.pData = data.data();
    }
};

void note_created(Device& d) { d.note_pipeline_cache_dirty(); }

// Fails for what this class does not create (none of it is in a pipeline
// whose functions are plain vertex, fragment and kernel functions).
void check_supported(const json::Value& info) {
    const auto& g = info["graphics"];
    if (g.is_null()) return;
    const auto present = [&](const char* name) {
        const auto& v = g[name];
        return !(v.is_null() || (v.is_array() && v.size() == 0));
    };
    for (const auto* name : {"tessellation", "fragment_imageblock", "mesh_records", "emulated_tessellation", "input_attachments",
                             "storage_attachments", "shadow_stencil", "logic_op", "shader_tests", "color_group"})
        if (present(name)) throw std::runtime_error(std::string("translated pipeline needs ") + name + ", which is not implemented");
    if (g["rasterization_order_attachment_access"].as_bool().value_or(false))
        throw std::runtime_error("translated pipeline needs rasterization-order attachment access, which is not implemented");
    if (u32_or(g["max_vertex_amplification_count"], 1) > 1)
        throw std::runtime_error("translated pipeline needs vertex amplification, which is not implemented");
    for (const auto& stage : info["stages"].items()) {
        const auto bit = u32(stage["stage"], "stage");
        if (bit != VK_SHADER_STAGE_VERTEX_BIT && bit != VK_SHADER_STAGE_FRAGMENT_BIT)
            throw std::runtime_error("translated pipeline has a stage " + std::to_string(bit) + " beyond vertex and fragment");
    }
}

} // namespace

VkSamplerCreateInfo sampler_create_info(const json::Value& desc, VkSamplerReductionModeCreateInfo& reduction) {
    if (!desc["border_components"].is_null()) throw std::runtime_error("sampler border color swizzle is not enabled");
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = static_cast<VkFilter>(u32(desc["mag_filter"], "mag_filter"));
    info.minFilter = static_cast<VkFilter>(u32(desc["min_filter"], "min_filter"));
    info.mipmapMode = static_cast<VkSamplerMipmapMode>(u32(desc["mipmap_mode"], "mipmap_mode"));
    info.addressModeU = static_cast<VkSamplerAddressMode>(u32(desc["address_mode_u"], "address_mode_u"));
    info.addressModeV = static_cast<VkSamplerAddressMode>(u32(desc["address_mode_v"], "address_mode_v"));
    info.addressModeW = static_cast<VkSamplerAddressMode>(u32(desc["address_mode_w"], "address_mode_w"));
    info.mipLodBias = f32(desc["mip_lod_bias"], 0);
    if (!desc["max_anisotropy"].is_null()) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = f32(desc["max_anisotropy"], 1);
    }
    if (!desc["compare"].is_null()) {
        info.compareEnable = VK_TRUE;
        info.compareOp = static_cast<VkCompareOp>(u32(desc["compare"], "compare"));
    }
    info.minLod = f32(desc["min_lod"], 0);
    info.maxLod = f32(desc["max_lod"], VK_LOD_CLAMP_NONE);
    info.borderColor = static_cast<VkBorderColor>(u32(desc["border_color"], "border_color"));
    const auto mode = u32_or(desc["reduction"], 0);
    if (mode != 0) {
        reduction = {VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
        reduction.reductionMode = static_cast<VkSamplerReductionMode>(mode);
        info.pNext = &reduction;
    }
    return info;
}

ForeignPipeline::ForeignPipeline(std::shared_ptr<Device> owner, std::shared_ptr<const Pipeline> translated,
                                 std::shared_ptr<const Library> library, json::Value descriptor)
    : device(std::move(owner)), pipeline(std::move(translated)), library_(std::move(library)), descriptor_(std::move(descriptor)) {
    auto& d = *device;
    const auto& info = pipeline->info();
    check_supported(info);
    try {
        // Set layouts, by set number.
        const auto& sets = info["layout"]["sets"];
        std::uint32_t set_count = 0;
        for (const auto& set : sets.items()) set_count = std::max(set_count, u32(set["set"], "set") + 1);
        set_layouts_.assign(set_count, VK_NULL_HANDLE);
        for (const auto& set : sets.items()) {
            std::vector<VkDescriptorSetLayoutBinding> bindings;
            std::vector<VkDescriptorBindingFlags> flags;
            std::vector<std::vector<VkSampler>> samplers;   // per binding with immutable samplers
            bool any_flags = false;
            for (const auto& b : set["bindings"].items()) {
                VkDescriptorSetLayoutBinding binding{};
                binding.binding = u32(b["binding"], "binding");
                binding.descriptorType = static_cast<VkDescriptorType>(u32(b["ty"], "descriptor type"));
                binding.descriptorCount = u32(b["count"], "descriptor count");
                binding.stageFlags = u32(b["stages"], "binding stages");
                if (!binding.stageFlags) binding.stageFlags = VK_SHADER_STAGE_ALL;   // used only by an added stage
                if (const auto& desc = b["immutable_sampler"]; !desc.is_null()) {
                    VkSamplerReductionModeCreateInfo reduction{};
                    auto sampler_info = sampler_create_info(desc, reduction);
                    VkSampler sampler{};
                    check(d.vk.vkCreateSampler(d.device, &sampler_info, nullptr, &sampler), "vkCreateSampler");
                    immutable_samplers_.push_back(sampler);
                    samplers.emplace_back(binding.descriptorCount, sampler);
                }
                const auto f = u32_or(b["flags"], 0);
                any_flags = any_flags || f;
                flags.push_back(f);
                bindings.push_back(binding);
            }
            // Point each immutable-sampler binding at its own array (stable now).
            std::size_t next = 0;
            for (std::size_t i = 0; i < bindings.size(); ++i)
                if (!set["bindings"][i]["immutable_sampler"].is_null()) bindings[i].pImmutableSamplers = samplers[next++].data();
            VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
            binding_flags.bindingCount = static_cast<std::uint32_t>(flags.size());
            binding_flags.pBindingFlags = flags.data();
            VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            layout_info.pNext = any_flags ? &binding_flags : nullptr;
            layout_info.flags = u32_or(set["flags"], 0);
            layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
            layout_info.pBindings = bindings.data();
            check(d.vk.vkCreateDescriptorSetLayout(d.device, &layout_info, nullptr, &set_layouts_[u32(set["set"], "set")]),
                  "vkCreateDescriptorSetLayout");
        }
        for (auto& layout : set_layouts_) {
            if (layout) continue;
            VkDescriptorSetLayoutCreateInfo empty{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            check(d.vk.vkCreateDescriptorSetLayout(d.device, &empty, nullptr, &layout), "vkCreateDescriptorSetLayout");
        }
        std::vector<VkPushConstantRange> ranges;
        for (const auto& r : info["layout"]["push_constants"].items())
            ranges.push_back({u32(r["stages"], "push stages"), u32(r["offset"], "push offset"), u32(r["size"], "push size")});
        VkPipelineLayoutCreateInfo pipeline_layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipeline_layout.setLayoutCount = static_cast<std::uint32_t>(set_layouts_.size());
        pipeline_layout.pSetLayouts = set_layouts_.data();
        pipeline_layout.pushConstantRangeCount = static_cast<std::uint32_t>(ranges.size());
        pipeline_layout.pPushConstantRanges = ranges.data();
        check(d.vk.vkCreatePipelineLayout(d.device, &pipeline_layout, nullptr, &layout_), "vkCreatePipelineLayout");

        for (std::size_t stage = 0; stage < info["stages"].size(); ++stage) {
            const auto code = pipeline->spirv(stage);
            VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            module_info.codeSize = code.size_bytes();
            module_info.pCode = code.data();
            VkShaderModule module{};
            check(d.vk.vkCreateShaderModule(d.device, &module_info, nullptr, &module), "vkCreateShaderModule");
            modules_.push_back(module);
        }
    } catch (...) {
        release();
        throw;
    }
}

ForeignPipeline::~ForeignPipeline() { release(); }

void ForeignPipeline::release() {
    auto& d = *device;
    for (auto& [key, p] : pipelines_) d.vk.vkDestroyPipeline(d.device, p, nullptr);
    pipelines_.clear();
    for (auto module : modules_) d.vk.vkDestroyShaderModule(d.device, module, nullptr);
    modules_.clear();
    if (layout_) d.vk.vkDestroyPipelineLayout(d.device, layout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    for (auto set : set_layouts_) if (set) d.vk.vkDestroyDescriptorSetLayout(d.device, set, nullptr);
    set_layouts_.clear();
    for (auto sampler : immutable_samplers_) d.vk.vkDestroySampler(d.device, sampler, nullptr);
    immutable_samplers_.clear();
}

ForeignPipeline::Planned ForeignPipeline::plan(const json::Value& state) {
    const auto shape = bind_shape(state);
    std::lock_guard lock(plan_mutex_);
    auto& slot = plans_[shape];
    if (!slot) slot = std::make_unique<Plan>(*pipeline, TranslatorDevice::of(*device), state);
    static const auto no_variant = template_of("VariantKey");
    const auto& variant = slot->info()["variant"];
    if (variant == no_variant || descriptor_["variant"] == variant) return {this, slot.get()};
    // The plan asks for another variant of this pipeline: compiled once,
    // planned for the same shape.
    if (!library_) throw TranslatorError("bind", "a plan asks for a pipeline variant, but the pipeline keeps no library");
    auto& compiled = variants_[variant.dump()];
    if (!compiled) {
        auto descriptor = descriptor_;
        descriptor.set("variant", variant);
        auto translated = std::make_shared<const Pipeline>(TranslatorDevice::of(*device), *library_, descriptor);
        compiled = std::make_shared<ForeignPipeline>(device, std::move(translated), library_, std::move(descriptor));
    }
    return compiled->plan(state);
}

VkPipeline ForeignPipeline::compute(const json::Value& region) {
    if (!pipeline->compute()) throw std::runtime_error("translated pipeline is not a compute pipeline");
    std::string key = region["specialization"].dump() + '|' + region["stage_flags"].dump() + '|' + region["required_subgroup_size"].dump();
    std::lock_guard lock(mutex_);
    if (const auto found = pipelines_.find(key); found != pipelines_.end()) return found->second;
    auto created = create_compute(region);
    pipelines_.emplace(std::move(key), created);
    return created;
}

VkPipeline ForeignPipeline::create_compute(const json::Value& region) {
    auto& d = *device;
    const auto& stage_info = pipeline->info()["stages"][0];
    Specialization specialization;
    specialization.from_key(region["specialization"]);
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage.flags = u32_or(region["stage_flags"], 0);
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = modules_.at(0);
    const auto* entry = stage_info["entry_point"].as_string();
    info.stage.pName = entry ? entry->c_str() : "main";
    info.stage.pSpecializationInfo = &specialization.info;
    if (const auto size = region["required_subgroup_size"].as_u64()) {
        subgroup.requiredSubgroupSize = static_cast<std::uint32_t>(*size);
        info.stage.pNext = &subgroup;
    }
    info.layout = layout_;
    VkPipeline result{};
    {
        perf::Scope timed(perf::mtl_pipeline_create_ticks, perf::mtl_pipelines_created);
        check(d.vk.vkCreateComputePipelines(d.device, d.pipeline_cache, 1, &info, nullptr, &result), "vkCreateComputePipelines");
    }
    note_created(d);
    return result;
}

VkPipeline ForeignPipeline::render(const json::Value& specialization, VkPrimitiveTopology topology, std::uint32_t raster) {
    if (pipeline->compute()) throw std::runtime_error("translated pipeline is not a render pipeline");
    // One VkPipeline per topology class and raster variant: the topology itself is dynamic state.
    const int topology_class = topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST ? 0
                               : topology <= VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ? 1
                                                                              : 2;
    if (topology_class != 2) raster &= ~raster_variant::fill_lines;
    std::string key = specialization.dump() + '|' + std::to_string(topology_class) + '|' + std::to_string(raster);
    std::lock_guard lock(mutex_);
    if (const auto found = pipelines_.find(key); found != pipelines_.end()) return found->second;
    auto created = create_render(specialization, topology, raster);
    pipelines_.emplace(std::move(key), created);
    return created;
}

VkPipeline ForeignPipeline::create_render(const json::Value& specialization_key, VkPrimitiveTopology topology, std::uint32_t raster_bits) {
    auto& d = *device;
    const auto& info = pipeline->info();
    const auto& g = info["graphics"];
    const auto& stages = info["stages"];

    // One specialization for every stage (each constant id belongs to one
    // stage's module; a module ignores the ids it does not declare).
    std::vector<Specialization> specializations(stages.size());
    std::vector<VkPipelineShaderStageCreateInfo> shader_stages;
    for (std::size_t i = 0; i < stages.size(); ++i) {
        if (specialization_key.is_null()) specializations[i].from_defaults(stages[i]["specialization"]);
        else specializations[i].from_key(specialization_key);
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.flags = u32_or(stages[i]["stage_flags"], 0);
        stage.stage = static_cast<VkShaderStageFlagBits>(u32(stages[i]["stage"], "stage"));
        stage.module = modules_.at(i);
        const auto* entry = stages[i]["entry_point"].as_string();
        stage.pName = entry ? entry->c_str() : "main";
        stage.pSpecializationInfo = &specializations[i].info;
        shader_stages.push_back(stage);
    }

    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputBindingDivisorDescriptionKHR> divisors;
    for (const auto& b : info["vertex_input"]["bindings"].items()) {
        VkVertexInputBindingDescription binding{};
        binding.binding = u32(b["binding"], "vertex binding");
        binding.inputRate = static_cast<VkVertexInputRate>(u32_or(b["input_rate"], 0));
        const auto& stride = b["stride"];
        binding.stride = stride.is_object() ? u32_or(stride["Static"], 0) : 0;   // "PerBind": dynamic stride
        const auto divisor = u32_or(b["divisor"], 1);
        if (binding.inputRate == VK_VERTEX_INPUT_RATE_INSTANCE && divisor != 1)
            throw std::runtime_error("translated pipeline needs a vertex attribute divisor, which is not enabled");
        bindings.push_back(binding);
    }
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (const auto& a : info["vertex_input"]["attributes"].items())
        attributes.push_back({u32(a["location"], "location"), u32(a["binding"], "attribute binding"),
                              static_cast<VkFormat>(u32(a["format"], "attribute format")), u32(a["offset"], "attribute offset")});
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = static_cast<std::uint32_t>(bindings.size());
    vertex_input.pVertexBindingDescriptions = bindings.data();
    vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.data();

    std::vector<VkDynamicState> dynamic_states;
    for (const auto& s : g["dynamic_states"].items()) dynamic_states.push_back(static_cast<VkDynamicState>(u32(s, "dynamic state")));
    const auto dynamic = [&](VkDynamicState s) { return std::find(dynamic_states.begin(), dynamic_states.end(), s) != dynamic_states.end(); };

    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = topology;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = dynamic(VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT) ? 0 : 1;
    viewport.scissorCount = dynamic(VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT) ? 0 : 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.rasterizerDiscardEnable = g["rasterizer_discard"].as_bool().value_or(false);
    raster.polygonMode = raster_bits & raster_variant::fill_lines ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    raster.depthClampEnable = (raster_bits & raster_variant::depth_clamp) != 0;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    // Metal's depth bias applies to every pipeline: enabled, its values dynamic.
    raster.depthBiasEnable = dynamic(VK_DYNAMIC_STATE_DEPTH_BIAS) && !dynamic(VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE);
    raster.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = static_cast<VkSampleCountFlagBits>(std::max(1U, u32_or(g["rasterization_samples"], 1)));
    multisample.alphaToCoverageEnable = g["alpha_to_coverage"].as_bool().value_or(false);
    multisample.alphaToOneEnable = g["alpha_to_one"].as_bool().value_or(false);
    multisample.sampleShadingEnable = g["sample_shading"].as_bool().value_or(false);
    multisample.minSampleShading = multisample.sampleShadingEnable ? 1.0F : 0.0F;
    std::uint32_t sample_mask = 0xffffffffU;
    if (const auto mask = g["sample_mask"].as_u64()) {
        sample_mask = static_cast<std::uint32_t>(*mask);
        multisample.pSampleMask = &sample_mask;
    }
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    depth.front.compareOp = depth.back.compareOp = VK_COMPARE_OP_ALWAYS;
    depth.maxDepthBounds = 1.0F;

    std::vector<VkFormat> color_formats;
    std::vector<VkPipelineColorBlendAttachmentState> blends;
    for (const auto& c : g["color_attachments"].items()) {
        VkPipelineColorBlendAttachmentState blend{};
        if (c.is_null()) {
            color_formats.push_back(VK_FORMAT_UNDEFINED);
            blends.push_back(blend);
            continue;
        }
        color_formats.push_back(static_cast<VkFormat>(u32(c["format"], "color format")));
        blend.colorWriteMask = u32_or(c["write_mask"], 0xf);
        if (const auto& b = c["blend"]; !b.is_null()) {
            blend.blendEnable = VK_TRUE;
            blend.srcColorBlendFactor = static_cast<VkBlendFactor>(u32(b["src_color"], "src_color"));
            blend.dstColorBlendFactor = static_cast<VkBlendFactor>(u32(b["dst_color"], "dst_color"));
            blend.colorBlendOp = static_cast<VkBlendOp>(u32(b["color_op"], "color_op"));
            blend.srcAlphaBlendFactor = static_cast<VkBlendFactor>(u32(b["src_alpha"], "src_alpha"));
            blend.dstAlphaBlendFactor = static_cast<VkBlendFactor>(u32(b["dst_alpha"], "dst_alpha"));
            blend.alphaBlendOp = static_cast<VkBlendOp>(u32(b["alpha_op"], "alpha_op"));
        }
        blends.push_back(blend);
    }
    VkPipelineColorBlendStateCreateInfo blend_state{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend_state.attachmentCount = static_cast<std::uint32_t>(blends.size());
    blend_state.pAttachments = blends.data();
    VkPipelineDynamicStateCreateInfo dynamic_info{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic_info.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic_info.pDynamicStates = dynamic_states.data();

    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = static_cast<std::uint32_t>(color_formats.size());
    rendering.pColorAttachmentFormats = color_formats.data();
    rendering.depthAttachmentFormat = static_cast<VkFormat>(u32_or(g["depth_format"], VK_FORMAT_UNDEFINED));
    rendering.stencilAttachmentFormat = static_cast<VkFormat>(u32_or(g["stencil_format"], VK_FORMAT_UNDEFINED));

    VkGraphicsPipelineCreateInfo create{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    create.pNext = &rendering;
    create.stageCount = static_cast<std::uint32_t>(shader_stages.size());
    create.pStages = shader_stages.data();
    create.pVertexInputState = &vertex_input;
    create.pInputAssemblyState = &assembly;
    create.pViewportState = &viewport;
    create.pRasterizationState = &raster;
    create.pMultisampleState = &multisample;
    create.pDepthStencilState = &depth;
    create.pColorBlendState = &blend_state;
    create.pDynamicState = &dynamic_info;
    create.layout = layout_;
    VkPipeline result{};
    {
        perf::Scope timed(perf::mtl_pipeline_create_ticks, perf::mtl_pipelines_created);
        check(d.vk.vkCreateGraphicsPipelines(d.device, d.pipeline_cache, 1, &create, nullptr, &result), "vkCreateGraphicsPipelines");
    }
    note_created(d);
    return result;
}

} // namespace alchemy::metal::air
