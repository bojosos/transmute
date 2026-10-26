// Draws and dispatches of pipelines the AIR translator compiled (owner: A5;
// commands.hpp, foreign_pipeline.hpp).
//
// The translator decides the binding: per draw or dispatch, what the encoder
// has bound becomes a BindState (its Metal-shaped description), the
// pipeline's plan for that state's shape (memoized) says what each layout
// binding takes, and the plan's commands for this call give the buffer
// ranges, push constants, specialization and the Vulkan draw or dispatch.
// Covered: buffers, setBytes data, textures (through the views the plan
// names), samplers, constant blobs, vertex buffers, direct and indexed draws,
// and direct dispatches. Anything else the plan asks for (argument tables,
// heaps, neutral stand-ins, indirect forms, ...) fails explicitly, naming it.
#include "alchemy/darwin/metal/commands.hpp"

#include "alchemy/darwin/metal/air_translator.hpp"
#include "alchemy/darwin/metal/foreign_pipeline.hpp"
#include "alchemy/perf.hpp"

#include <cstring>
#include <stdexcept>

namespace alchemy::metal {

namespace {

constexpr std::uint32_t texture_types = 10;

std::uint32_t u32(const json::Value& value, const char* what) {
    const auto v = value.as_u64();
    if (!v || *v > 0xffffffffULL) throw std::runtime_error(std::string("translated binding: ") + what + " is not a 32-bit number");
    return static_cast<std::uint32_t>(*v);
}

// The serialized form of an enumeration member (air::metal_enum) by name.
json::Value name(const char* type, std::uint32_t raw) { return air::metal_enum(type, raw); }

json::Value sampler_descriptor(const SamplerDescriptor& d) {
    auto out = air::template_of("SamplerDescriptor");
    out.set("min_filter", name("SamplerMinMagFilter", d.min_filter));
    out.set("mag_filter", name("SamplerMinMagFilter", d.mag_filter));
    out.set("mip_filter", name("SamplerMipFilter", d.mip_filter));
    out.set("s_address_mode", name("SamplerAddressMode", d.address[0]));
    out.set("t_address_mode", name("SamplerAddressMode", d.address[1]));
    out.set("r_address_mode", name("SamplerAddressMode", d.address[2]));
    out.set("border_color", name("SamplerBorderColor", d.border_color));
    out.set("compare_function", name("CompareFunction", d.compare_function));
    out.set("max_anisotropy", d.max_anisotropy);
    out.set("lod_min_clamp", static_cast<double>(d.lod_min));
    out.set("lod_max_clamp", static_cast<double>(d.lod_max));
    out.set("normalized_coordinates", d.normalized_coordinates);
    out.set("lod_average", d.lod_average);
    return out;
}

json::Value texture_facts(const Texture& t) {
    const auto& d = t.descriptor;
    if (static_cast<std::uint32_t>(d.type) >= texture_types || t.is_buffer())
        throw std::runtime_error("Alchemy Metal: texture buffers bound to translated pipelines are not implemented");
    auto facts = air::template_of("TextureFacts");
    facts.set("texture_type", name("TextureType", static_cast<std::uint32_t>(d.type)));
    facts.set("pixel_format", name("PixelFormat", d.pixel_format));
    facts.set("sample_count", d.sample_count);
    facts.set("usage", static_cast<std::uint64_t>(d.usage));
    facts.set("array_length", d.array_length);
    facts.set("mipmap_level_count", d.mipmap_levels);
    auto swizzle = json::Value::object();
    const char* channels[] = {"red", "green", "blue", "alpha"};
    for (int i = 0; i < 4; ++i) swizzle.set(channels[i], name("TextureSwizzle", d.swizzle[i]));
    facts.set("swizzle", std::move(swizzle));
    return facts;
}

// A plan's slot reference: its stage table and index.
struct Slot {
    std::string stage;
    std::uint32_t index{};
};
Slot slot_of(const json::Value& slot) {
    const auto* stage = slot["stage"].as_string();
    if (!stage) throw std::runtime_error("translated binding: a slot without a stage: " + slot.dump());
    return {*stage, u32(slot["index"], "slot index")};
}

VkShaderStageFlagBits stage_bit(const std::string& stage) {
    if (stage == "Vertex") return VK_SHADER_STAGE_VERTEX_BIT;
    if (stage == "Fragment") return VK_SHADER_STAGE_FRAGMENT_BIT;
    if (stage == "Kernel") return VK_SHADER_STAGE_COMPUTE_BIT;
    throw std::runtime_error("translated binding: stage " + stage + " is not implemented");
}

// Samplers made for plans' sampler writes, per device, by their serialized
// vk::SamplerDesc (few: one per distinct sampler state and use).
struct ForeignSamplers {
    Device* device{};
    std::mutex mutex;
    std::map<std::string, VkSampler> samplers;
    VkSampler get(const json::Value& desc) {
        std::lock_guard lock(mutex);
        auto& slot = samplers[desc.dump()];
        if (!slot) {
            VkSamplerReductionModeCreateInfo reduction{};
            const auto info = air::sampler_create_info(desc, reduction);
            check(device->vk.vkCreateSampler(device->device, &info, nullptr, &slot), "vkCreateSampler");
        }
        return slot;
    }
    void cleanup() {
        for (auto& [key, sampler] : samplers) device->vk.vkDestroySampler(device->device, sampler, nullptr);
        samplers.clear();
    }
};

ForeignSamplers& foreign_samplers(Device& device) {
    return device.extension<ForeignSamplers>("air-samplers", [&device] {
        auto made = std::make_shared<ForeignSamplers>();
        made->device = &device;
        return made;
    });
}

void unsupported_if(const json::Value& value, const char* what) {
    const bool present = !(value.is_null() || (value.is_array() && value.size() == 0) ||
                           (value.as_bool() && !*value.as_bool()));
    if (present) throw std::runtime_error(std::string("Alchemy Metal: a translated pipeline's ") + what + " is not implemented");
}

} // namespace

void CommandBuffer::set_render_pipeline(std::shared_ptr<air::ForeignPipeline> pipeline) {
    require(EncoderKind::render, "setRenderPipelineState:");
    perf::add(perf::mtl_set_pipeline);
    retain(pipeline);
    foreign_render_ = std::move(pipeline);
    render_pipeline_.reset();
}

void CommandBuffer::set_compute_pipeline(std::shared_ptr<air::ForeignPipeline> pipeline) {
    require(EncoderKind::compute, "setComputePipelineState:");
    perf::add(perf::mtl_set_pipeline);
    retain(pipeline);
    foreign_compute_ = std::move(pipeline);
    compute_pipeline_.reset();
}

json::Value CommandBuffer::foreign_state(bool render) {
    auto state = air::template_of("BindState");
    auto stages = json::Value::object();
    const auto table = [&](StageState& st, const char* stage, bool kernel) {
        auto t = air::template_of("StageTable");
        auto& buffers = *t.find("buffers");
        for (std::uint32_t i = 0; i < st.buffers.size(); ++i) {
            const auto& b = st.buffers[i];
            auto bound = json::Value::object(), wrapped = json::Value::object();
            if (b.buffer) {
                bound.set("length", static_cast<std::uint64_t>(b.buffer->length));
                bound.set("offset", b.offset);
                bound.set("gpu_address", static_cast<std::uint64_t>(b.buffer->address));
                bound.set("device_address", static_cast<std::uint64_t>(b.buffer->address));
                bound.set("attribute_stride", nullptr);
                wrapped.set("Buffer", std::move(bound));
            } else if (b.vk_buffer) {
                bound.set("len", b.bytes_length);
                bound.set("attribute_stride", nullptr);
                wrapped.set("Bytes", std::move(bound));
            } else {
                continue;
            }
            buffers.set(std::to_string(i), std::move(wrapped));
        }
        auto& textures = *t.find("textures");
        for (std::uint32_t i = 0; i < st.textures.size(); ++i) {
            if (!st.textures[i]) continue;
            auto bound = json::Value::object(), wrapped = json::Value::object();
            bound.set("facts", texture_facts(*st.textures[i]));
            bound.set("resource_id", nullptr);
            wrapped.set("Texture", std::move(bound));
            textures.set(std::to_string(i), std::move(wrapped));
        }
        auto& samplers = *t.find("samplers");
        for (std::uint32_t i = 0; i < st.samplers.size(); ++i) {
            if (!st.samplers[i]) continue;
            auto bound = json::Value::object();
            bound.set("desc", sampler_descriptor(st.samplers[i]->descriptor));
            bound.set("resource_id", nullptr);
            samplers.set(std::to_string(i), std::move(bound));
        }
        if (kernel) {
            auto& lengths = *t.find("threadgroup_lengths");
            for (std::uint32_t i = 0; i < threadgroup_memory_.size(); ++i)
                if (threadgroup_memory_[i]) lengths.set(std::to_string(i), threadgroup_memory_[i]);
        }
        stages.set(stage, std::move(t));
    };
    if (render) {
        table(render_stages_[0], "Vertex", false);
        table(render_stages_[1], "Fragment", false);
        auto area = json::Value::array();
        area.push(extent_.width);
        area.push(extent_.height);
        state.set("render_area", std::move(area));
        state.set("viewport_count", viewport_count_);
        state.set("render_target_array_length", pass_layers_);
    } else {
        table(compute_stage_, "Kernel", true);
    }
    state.set("stages", std::move(stages));
    return state;
}

VkImageView CommandBuffer::foreign_view(const std::shared_ptr<Texture>& texture, const json::Value& view) {
    const auto key = std::to_string(reinterpret_cast<std::uintptr_t>(texture->image)) + ':' + std::to_string(texture->base_level) + ':' +
                     std::to_string(texture->base_layer) + ':' + view.dump();
    if (const auto found = foreign_view_cache_.find(key); found != foreign_view_cache_.end()) return found->second;
    const auto& d = texture->descriptor;
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = texture->image;
    info.viewType = static_cast<VkImageViewType>(u32(view["view_type"], "view type"));
    info.format = static_cast<VkFormat>(u32(view["format"], "view format"));
    const auto& c = view["components"];
    info.components = {static_cast<VkComponentSwizzle>(c["r"].as_u64().value_or(0)), static_cast<VkComponentSwizzle>(c["g"].as_u64().value_or(0)),
                       static_cast<VkComponentSwizzle>(c["b"].as_u64().value_or(0)), static_cast<VkComponentSwizzle>(c["a"].as_u64().value_or(0))};
    info.subresourceRange.aspectMask = u32(view["aspect"], "view aspect");
    if (const auto level = view["level"].as_u64()) {
        info.subresourceRange.baseMipLevel = texture->base_level + static_cast<std::uint32_t>(*level);
        info.subresourceRange.levelCount = 1;
    } else {
        info.subresourceRange.baseMipLevel = texture->base_level;
        info.subresourceRange.levelCount = d.mipmap_levels;
    }
    const bool cube = d.type == TextureType::cube || d.type == TextureType::cube_array;
    info.subresourceRange.baseArrayLayer = texture->base_layer;
    info.subresourceRange.layerCount = d.type == TextureType::type3d ? 1 : d.array_length * (cube ? 6 : 1);
    // Only the usage this view is for: an image's other usages may not
    // suit the view's format (storage of an sRGB view).
    VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    usage.usage = u32(view["layout"], "view layout") == VK_IMAGE_LAYOUT_GENERAL ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT;
    info.pNext = &usage;
    VkImageView created{};
    check(device->vk.vkCreateImageView(device->device, &info, nullptr, &created), "vkCreateImageView");
    foreign_views_.push_back(created);
    foreign_view_cache_.emplace(key, created);
    return created;
}

void CommandBuffer::foreign_bind(air::ForeignPipeline& pipeline, const json::Value& plan, const json::Value& state,
                                 const json::Value& ranges, VkPipelineBindPoint point) {
    static_cast<void>(state);
    const bool render = point == VK_PIPELINE_BIND_POINT_GRAPHICS;
    const auto stage_table = [&](const std::string& stage) -> StageState& {
        if (stage == "Vertex" && render) return render_stages_[0];
        if (stage == "Fragment" && render) return render_stages_[1];
        if (stage == "Kernel" && !render) return compute_stage_;
        throw std::runtime_error("translated binding: stage " + stage + " in a " + (render ? "render" : "compute") + " encoder");
    };
    const auto texture_at = [&](const json::Value& slot_ref) -> const std::shared_ptr<Texture>& {
        const auto slot = slot_of(slot_ref);
        auto& st = stage_table(slot.stage);
        if (slot.index >= st.textures.size() || !st.textures[slot.index])
            throw std::runtime_error("translated binding: no texture at " + slot.stage + " texture " + std::to_string(slot.index));
        const auto& texture = st.textures[slot.index];
        if (render) prepare_in_pass(texture, (texture->descriptor.usage & texture_usage::shader_write) != 0);
        else prepare_texture(texture, (texture->descriptor.usage & texture_usage::shader_write) != 0);
        return texture;
    };
    const auto range_of = [&](std::uint32_t set, std::uint32_t binding) -> std::pair<VkDeviceSize, VkDeviceSize> {
        for (const auto& r : ranges.items())
            if (u32(r["set"], "range set") == set && u32(r["binding"], "range binding") == binding)
                return {r["range"]["offset"].as_u64().value_or(0), r["range"]["range"].as_u64().value_or(VK_WHOLE_SIZE)};
        throw std::runtime_error("translated binding: no descriptor range for set " + std::to_string(set) + " binding " +
                                 std::to_string(binding));
    };
    auto& samplers = foreign_samplers(*device);
    const auto& translator = air::TranslatorDevice::of(*device);
    const auto sampler_for = [&](const json::Value& use, std::int64_t format) {
        const auto slot = slot_of(use["slot"]);
        auto& st = stage_table(slot.stage);
        if (slot.index >= st.samplers.size() || !st.samplers[slot.index])
            throw std::runtime_error("translated binding: no sampler at " + slot.stage + " sampler " + std::to_string(slot.index));
        auto desc = air::sampler_desc(translator, sampler_descriptor(st.samplers[slot.index]->descriptor), use, format);
        return samplers.get(desc);
    };

    const auto& set_layouts = pipeline.set_layouts();
    std::vector<VkDescriptorSet> sets;
    for (const auto layout : set_layouts) sets.push_back(allocate_set(layout));
    const auto& writes = plan["descriptors"];
    std::vector<VkDescriptorBufferInfo> buffer_infos;
    std::vector<VkDescriptorImageInfo> image_infos;
    buffer_infos.reserve(writes.size());
    image_infos.reserve(writes.size());
    std::vector<VkWriteDescriptorSet> updates;
    for (const auto& w : writes.items()) {
        VkWriteDescriptorSet update{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        const auto set = u32(w["set"], "write set");
        if (set >= sets.size()) throw std::runtime_error("translated binding: a write to set " + std::to_string(set));
        update.dstSet = sets[set];
        update.dstBinding = u32(w["binding"], "write binding");
        update.dstArrayElement = u32(w["array_element"], "array element");
        update.descriptorCount = 1;
        update.descriptorType = static_cast<VkDescriptorType>(u32(w["ty"], "descriptor type"));
        const auto& source = w["source"];
        const std::string kind = source.is_object() && source.size() ? source.names()[0] : source.as_string() ? *source.as_string() : "";
        const auto& body = source.is_object() && source.size() ? source.items()[0] : source;
        if (kind == "Buffer") {
            const auto slot = slot_of(body["slot"]);
            auto& b = stage_table(slot.stage).buffers.at(slot.index);
            if (!b.buffer) throw std::runtime_error("translated binding: no buffer at " + slot.stage + " buffer " + std::to_string(slot.index));
            const auto [offset, range] = range_of(set, update.dstBinding);
            buffer_infos.push_back({b.buffer->buffer, offset, range});
            update.pBufferInfo = &buffer_infos.back();
        } else if (kind == "InlineBytes") {
            const auto slot = slot_of(body["slot"]);
            auto& b = stage_table(slot.stage).buffers.at(slot.index);
            if (!b.vk_buffer || b.buffer) throw std::runtime_error("translated binding: no setBytes data at " + slot.stage + " buffer " + std::to_string(slot.index));
            const auto [offset, range] = range_of(set, update.dstBinding);
            buffer_infos.push_back({b.vk_buffer, b.vk_offset + offset, range});
            update.pBufferInfo = &buffer_infos.back();
        } else if (kind == "Texture") {
            const auto& texture = texture_at(body["slot"]);
            image_infos.push_back({VK_NULL_HANDLE, foreign_view(texture, body["view"]), VK_IMAGE_LAYOUT_GENERAL});
            update.pImageInfo = &image_infos.back();
        } else if (kind == "CombinedImageSampler") {
            const auto& image = body["image"];
            const auto* texture_source = image.find("Texture");
            if (!texture_source) throw std::runtime_error("translated binding: combined image source " + image.dump() + " is not implemented");
            const auto& texture = texture_at((*texture_source)["slot"]);
            VkDescriptorImageInfo info{VK_NULL_HANDLE, foreign_view(texture, (*texture_source)["view"]), VK_IMAGE_LAYOUT_GENERAL};
            const auto& sampler = body["sampler"];
            if (const auto* slot_use = sampler.find("Slot")) {
                info.sampler = sampler_for(*slot_use, static_cast<std::int64_t>(u32((*texture_source)["view"]["format"], "view format")));
            } else if (!(sampler.as_string() && *sampler.as_string() == "Immutable")) {
                throw std::runtime_error("translated binding: sampler source " + sampler.dump() + " is not implemented");
            }
            image_infos.push_back(info);
            update.pImageInfo = &image_infos.back();
        } else if (kind == "Sampler") {
            image_infos.push_back({sampler_for(body, -1), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
            update.pImageInfo = &image_infos.back();
        } else if (kind == "ConstantBlob") {
            const auto* stage = body["stage"].as_string();
            const auto bit = stage_bit(stage ? *stage : "");
            std::span<const std::uint8_t> blob;
            const auto& stages = pipeline.pipeline->info()["stages"];
            for (std::size_t i = 0; i < stages.size(); ++i)
                if (u32(stages[i]["stage"], "stage") == static_cast<std::uint32_t>(bit)) blob = pipeline.pipeline->constant_blob(i);
            const auto size = (blob.size() + 15) & ~std::size_t{15};
            const auto memory = upload(std::max<std::size_t>(size, 16), 64);
            std::memset(memory.pointer, 0, std::max<std::size_t>(size, 16));
            if (!blob.empty()) std::memcpy(memory.pointer, blob.data(), blob.size());
            buffer_infos.push_back({memory.buffer, memory.offset, std::max<std::size_t>(size, 16)});
            update.pBufferInfo = &buffer_infos.back();
        } else {
            throw std::runtime_error("Alchemy Metal: a translated pipeline's binding source " + (kind.empty() ? source.dump() : kind) +
                                     " is not implemented");
        }
        updates.push_back(update);
    }
    if (!updates.empty())
        device->vk.vkUpdateDescriptorSets(device->device, static_cast<std::uint32_t>(updates.size()), updates.data(), 0, nullptr);
    if (!sets.empty())
        device->vk.vkCmdBindDescriptorSets(current_, point, pipeline.layout(), 0, static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
    perf::add(perf::mtl_vk_binds);
}

void CommandBuffer::foreign_push(air::ForeignPipeline& pipeline, const json::Value& writes) {
    for (const auto& w : writes.items()) {
        std::vector<std::uint8_t> bytes;
        for (const auto& b : w["bytes"].items()) bytes.push_back(static_cast<std::uint8_t>(b.as_u64().value_or(0)));
        if (bytes.empty()) continue;
        device->vk.vkCmdPushConstants(current_, pipeline.layout(), u32(w["stages"], "push stages"), u32(w["offset"], "push offset"),
                                      static_cast<std::uint32_t>(bytes.size()), bytes.data());
    }
}

void CommandBuffer::restore_native_state() {
    native_state_lost_ = false;
    if (!viewports_.empty())
        device->vk.vkCmdSetViewportWithCount(current_, static_cast<std::uint32_t>(viewports_.size()), viewports_.data());
    emit_scissors();
    device->vk.vkCmdSetDepthBiasEnable(current_, depth_bias_enabled_ ? VK_TRUE : VK_FALSE);
    device->vk.vkCmdSetDepthBoundsTestEnable(current_, VK_FALSE);
}

void CommandBuffer::foreign_draw(std::uint32_t primitive, const json::Value& draw, const std::shared_ptr<Buffer>& index_buffer) {
    require(EncoderKind::render, "draw");
    auto& foreign = *foreign_render_;
    auto state = foreign_state(true);
    state.set("primitive_type", name("PrimitiveType", primitive));
    const auto planned = foreign.plan(state);
    const auto commands = planned.plan->draw(state, draw);
    unsupported_if(commands["descriptor_bytes"], "argument and handle tables");
    unsupported_if(commands["push_holes"], "device addresses in push constants");
    unsupported_if(commands["root_tables"], "resident root tables");
    unsupported_if(commands["amplifications"], "vertex amplification");
    unsupported_if(commands["index_bounds_pass"], "index bounds pass");
    unsupported_if(commands["mesh_dispatch"], "mesh dispatch");
    unsupported_if(commands["tessellation_dispatch"], "tessellation dispatch");
    perf::add(perf::mtl_draws);
    auto& pipeline = *planned.pipeline;
    const auto topology = primitive_topology(primitive);
    const auto vk_pipeline = pipeline.render(commands["specialization"], topology, raster_variant_);
    const auto command = current_;
    device->vk.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline);
    bound_pipeline_ = vk_pipeline;
    bound_layout_ = pipeline.layout();
    bound_topology_ = UINT32_MAX;
    for (auto& stage : render_stages_) stage.dirty = true;
    native_state_lost_ = true;
    // The translated pipeline's viewport and scissor are dynamic without a count.
    if (!viewports_.empty()) device->vk.vkCmdSetViewport(command, 0, 1, viewports_.data());
    const VkRect2D scissor = scissors_.empty() ? VkRect2D{{0, 0}, extent_} : scissors_.front();
    device->vk.vkCmdSetScissor(command, 0, 1, &scissor);
    device->vk.vkCmdSetPrimitiveTopology(command, topology);
    device->vk.vkCmdSetPrimitiveRestartEnable(command, primitive == 2 || primitive == 4 ? VK_TRUE : VK_FALSE);
    foreign_bind(pipeline, planned.plan->info(), state, commands["descriptor_ranges"], VK_PIPELINE_BIND_POINT_GRAPHICS);
    foreign_push(pipeline, commands["push_constants"]);

    // Vertex buffers the pipeline fetches fixed-function.
    const auto& offsets = commands["buffer_offsets"];
    const auto& strides = commands["vertex_strides"];
    for (const auto& vb : planned.plan->info()["vertex_buffers"].items()) {
        const auto binding = u32(vb["binding"], "vertex binding");
        const auto index = u32(vb["buffer_index"], "vertex buffer index");
        const auto& b = render_stages_[0].buffers.at(index);
        if (!b.vk_buffer) throw std::runtime_error("Metal draw: vertex buffer " + std::to_string(index) + " is not bound");
        VkDeviceSize offset = b.vk_offset;
        for (const auto& pair : offsets.items())
            if (u32(pair[0], "offset binding") == binding) offset = pair[1].as_u64().value_or(offset);
        VkDeviceSize stride = 0;
        bool dynamic_stride = vb["per_bind_stride"].as_bool().value_or(false);
        for (const auto& pair : strides.items())
            if (u32(pair[0], "stride binding") == binding) stride = pair[1].as_u64().value_or(0);
        device->vk.vkCmdBindVertexBuffers2(command, binding, 1, &b.vk_buffer, &offset, nullptr, dynamic_stride ? &stride : nullptr);
        bound_vertex_buffers_[binding < 31 ? binding : 0] = VK_NULL_HANDLE;   // the native path rebinds
    }

    const auto& call = draw["Draw"].is_null() ? commands["draw"] : commands["draw"];
    if (const auto* d = call.find("Draw")) {
        device->vk.vkCmdDraw(command, u32((*d)["vertex_count"], "vertex_count"), u32((*d)["instance_count"], "instance_count"),
                             u32((*d)["first_vertex"], "first_vertex"), u32((*d)["first_instance"], "first_instance"));
    } else if (const auto* d = call.find("DrawIndexed")) {
        if (!index_buffer) throw std::runtime_error("translated draw: an indexed draw without an index buffer");
        retain(index_buffer);
        device->vk.vkCmdBindIndexBuffer(command, index_buffer->buffer, (*d)["index_buffer_offset"].as_u64().value_or(0),
                                        static_cast<VkIndexType>(u32((*d)["index_type"], "index_type")));
        device->vk.vkCmdDrawIndexed(command, u32((*d)["index_count"], "index_count"), u32((*d)["instance_count"], "instance_count"),
                                    u32((*d)["first_index"], "first_index"),
                                    static_cast<std::int32_t>((*d)["vertex_offset"].as_i64().value_or(0)),
                                    u32((*d)["first_instance"], "first_instance"));
    } else {
        throw std::runtime_error("Alchemy Metal: a translated pipeline's draw " + call.dump() + " is not implemented");
    }
}

void CommandBuffer::foreign_dispatch(const json::Value& dispatch) {
    require(EncoderKind::compute, "dispatch");
    auto& foreign = *foreign_compute_;
    const auto state = foreign_state(false);
    const auto planned = foreign.plan(state);
    const auto commands = planned.plan->dispatch(state, dispatch);
    unsupported_if(commands["descriptor_bytes"], "argument and handle tables");
    unsupported_if(commands["push_holes"], "device addresses in push constants");
    unsupported_if(commands["root_table"], "resident root table");
    unsupported_if(commands["indirect_clamp"], "indirect clamp");
    unsupported_if(commands["region_record"], "region record");
    perf::add(perf::mtl_dispatches);
    if (!concurrent_ && dispatched_)
        barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    auto& pipeline = *planned.pipeline;
    const auto command = current_;
    bool bound = false;
    for (const auto& region : commands["regions"].items()) {
        unsupported_if(region["indirect"], "indirect dispatch region");
        const auto vk_pipeline = pipeline.compute(region);
        device->vk.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, vk_pipeline);
        if (!bound) {
            foreign_bind(pipeline, planned.plan->info(), state, commands["descriptor_ranges"], VK_PIPELINE_BIND_POINT_COMPUTE);
            bound = true;
        }
        foreign_push(pipeline, region["push_constants"]);
        const auto& groups = region["group_count"];
        device->vk.vkCmdDispatch(command, u32(groups[0], "group x"), u32(groups[1], "group y"), u32(groups[2], "group z"));
        bound_pipeline_ = vk_pipeline;
    }
    bound_layout_ = pipeline.layout();
    compute_stage_.dirty = true;
    dispatched_ = true;
}

} // namespace alchemy::metal
