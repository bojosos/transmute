#pragma once

// Vulkan objects for a pipeline the AIR translator compiled (owner: A5).
//
// A translated pipeline (air::Pipeline) brings its own pipeline layout and
// binding model, not the MSL contract's: descriptor set layouts, push
// constant ranges, specialization constants keyed to run-time facts, and,
// for a render pipeline, its fixed-function and dynamic state. This class
// creates those objects: the set layouts and pipeline layout once, the
// shader modules once, and a VkPipeline per specialization (a compute
// pipeline per dispatch region; a render pipeline per draw specialization
// and topology class), cached.
//
// The binder's plans and commands (air::Plan) say what to write into the
// sets and push constants per dispatch or draw; commands.cpp records them.

#include "alchemy/darwin/metal/air_translator.hpp"
#include "alchemy/darwin/metal/device.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace alchemy::metal::air {

class ForeignPipeline {
public:
    // Throws VulkanError for driver failures and std::runtime_error for a
    // translated pipeline this class cannot create (a feature named in the
    // message: tessellation, mesh stages, input attachments, ...).
    // `library` and `descriptor` (the compile's) let plans ask for variants.
    ForeignPipeline(std::shared_ptr<Device> device, std::shared_ptr<const Pipeline> pipeline,
                    std::shared_ptr<const Library> library = nullptr, json::Value descriptor = {});
    ~ForeignPipeline();
    ForeignPipeline(const ForeignPipeline&) = delete;
    ForeignPipeline& operator=(const ForeignPipeline&) = delete;

    const std::shared_ptr<Device> device;
    const std::shared_ptr<const Pipeline> pipeline;

    [[nodiscard]] VkPipelineLayout layout() const noexcept { return layout_; }
    // By layout set number (sets the layout leaves out are empty layouts).
    [[nodiscard]] const std::vector<VkDescriptorSetLayout>& set_layouts() const noexcept { return set_layouts_; }
    [[nodiscard]] const json::Value& layout_info() const { return pipeline->info()["layout"]; }

    // The plan of a BindState, memoized by its shape: the pipeline that runs
    // it (this one, or the variant the plan asked for, compiled once) and
    // its plan. Throws TranslatorError for a state the translator refuses.
    struct Planned {
        ForeignPipeline* pipeline;
        const Plan* plan;
    };
    [[nodiscard]] Planned plan(const json::Value& state);

    // The compute VkPipeline of a DispatchCommands region ({"specialization",
    // "stage_flags", "required_subgroup_size", ...}).
    [[nodiscard]] VkPipeline compute(const json::Value& region);
    // The render VkPipeline for a draw's specialization (DrawCommands'
    // "specialization", null for the stages' defaults) and topology.
    // `raster`: raster_variant bits (pipeline.hpp) of the encoder's depth clip and fill modes.
    [[nodiscard]] VkPipeline render(const json::Value& specialization, VkPrimitiveTopology topology, std::uint32_t raster = 0);

private:
    void release();
    VkPipeline create_compute(const json::Value& region);
    VkPipeline create_render(const json::Value& specialization, VkPrimitiveTopology topology, std::uint32_t raster);

    std::vector<VkDescriptorSetLayout> set_layouts_;
    std::vector<VkSampler> immutable_samplers_;
    VkPipelineLayout layout_{};
    std::vector<VkShaderModule> modules_;   // per stage of the pipeline
    std::mutex mutex_;
    std::map<std::string, VkPipeline> pipelines_;   // by specialization (and topology class)
    std::shared_ptr<const Library> library_;
    json::Value descriptor_;
    std::mutex plan_mutex_;
    std::map<std::string, std::unique_ptr<Plan>> plans_;                       // by bind shape
    std::map<std::string, std::shared_ptr<ForeignPipeline>> variants_;         // by variant key
};

// A Vulkan sampler create info for a serialized metal2vulkan vk::SamplerDesc
// (a layout's immutable sampler, or a plan's sampler write); `reduction` is
// chained when the desc asks for min/max reduction. Throws for a border
// color swizzle (VK_EXT_border_color_swizzle is never enabled).
[[nodiscard]] VkSamplerCreateInfo sampler_create_info(const json::Value& desc, VkSamplerReductionModeCreateInfo& reduction);

} // namespace alchemy::metal::air
