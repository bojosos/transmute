#include "alchemy/darwin/metal/bindless.hpp"

#include "alchemy/msl/compiler.hpp"

#include <algorithm>
#include <array>

// The bindless descriptor set. See bindless.hpp.
namespace alchemy::metal {

bool Bindless::supported(const Device& device) {
    // Every feature is enabled when the device has it (Device::initialize).
    const auto& f = device.features12;
    return f.bufferDeviceAddress && f.descriptorIndexing && f.runtimeDescriptorArray && f.descriptorBindingPartiallyBound &&
           f.shaderSampledImageArrayNonUniformIndexing && f.shaderStorageImageArrayNonUniformIndexing &&
           f.descriptorBindingSampledImageUpdateAfterBind && f.descriptorBindingStorageImageUpdateAfterBind &&
           f.descriptorBindingUpdateUnusedWhilePending;
}

Bindless* Bindless::of(Device& device) {
    if (!supported(device)) return nullptr;
    auto& value = device.extension<Bindless>("metal.bindless", [&device] {
        auto made = std::make_shared<Bindless>(device);
        made->create();
        return made;
    });
    return value.set ? &value : nullptr;
}

void Bindless::create() {
    auto& d = *device;
    VkPhysicalDeviceVulkan12Properties properties12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &properties12;
    d.vk.vkGetPhysicalDeviceProperties2(d.physical, &properties);
    const auto& p = properties12;
    // Room is left in the per-stage limits for a stage's own bindings
    // (msl::maximum_textures textures, a few dozen samplers).
    const auto room = [](std::uint32_t limit, std::uint32_t reserve) { return limit > reserve ? limit - reserve : 0U; };
    texture_capacity = std::min({65536U, p.maxDescriptorSetUpdateAfterBindSampledImages,
                                 p.maxDescriptorSetUpdateAfterBindStorageImages,
                                 room(p.maxPerStageDescriptorUpdateAfterBindSampledImages, 256),
                                 room(p.maxPerStageDescriptorUpdateAfterBindStorageImages, 256),
                                 room(p.maxPerStageUpdateAfterBindResources, 1024) / 2});
    sampler_capacity = std::min({2048U, p.maxDescriptorSetUpdateAfterBindSamplers,
                                 room(p.maxPerStageDescriptorUpdateAfterBindSamplers, 256)});
    if (texture_capacity < 16 || sampler_capacity < 16) return;   // no usable set: Bindless::of answers nullptr

    const VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                           VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    const std::array<VkDescriptorSetLayoutBinding, 3> bindings{{
        {msl::bindless_sampled_image_binding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, texture_capacity, VK_SHADER_STAGE_ALL, nullptr},
        {msl::bindless_storage_image_binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, texture_capacity, VK_SHADER_STAGE_ALL, nullptr},
        {msl::bindless_sampler_binding, VK_DESCRIPTOR_TYPE_SAMPLER, sampler_capacity, VK_SHADER_STAGE_ALL, nullptr}}};
    const std::array<VkDescriptorBindingFlags, 3> binding_flags{flags, flags, flags};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flag_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flag_info.bindingCount = static_cast<std::uint32_t>(binding_flags.size());
    flag_info.pBindingFlags = binding_flags.data();
    VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.pNext = &flag_info;
    layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layout_info.pBindings = bindings.data();
    check(d.vk.vkCreateDescriptorSetLayout(d.device, &layout_info, nullptr, &layout), "vkCreateDescriptorSetLayout (bindless)");

    const std::array<VkDescriptorPoolSize, 3> sizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, texture_capacity},
                                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, texture_capacity},
                                                     {VK_DESCRIPTOR_TYPE_SAMPLER, sampler_capacity}}};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    pool_info.pPoolSizes = sizes.data();
    check(d.vk.vkCreateDescriptorPool(d.device, &pool_info, nullptr, &pool_), "vkCreateDescriptorPool (bindless)");
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &layout;
    check(d.vk.vkAllocateDescriptorSets(d.device, &allocate, &set), "vkAllocateDescriptorSets (bindless)");
    VkDescriptorSetLayoutCreateInfo empty{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    check(d.vk.vkCreateDescriptorSetLayout(d.device, &empty, nullptr, &empty_layout), "vkCreateDescriptorSetLayout (empty)");
}

void Bindless::bind(VkCommandBuffer command, VkPipelineBindPoint point, VkPipelineLayout pipeline_layout) const {
    device->vk.vkCmdBindDescriptorSets(command, point, pipeline_layout, msl::bindless_descriptor_set, 1, &set, 0, nullptr);
}

void Bindless::cleanup() {
    auto& d = *device;
    if (pool_) d.vk.vkDestroyDescriptorPool(d.device, pool_, nullptr);
    if (layout) d.vk.vkDestroyDescriptorSetLayout(d.device, layout, nullptr);
    if (empty_layout) d.vk.vkDestroyDescriptorSetLayout(d.device, empty_layout, nullptr);
    empty_layout = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    layout = VK_NULL_HANDLE;
    set = VK_NULL_HANDLE;
}

std::uint32_t Bindless::add_texture(VkImageView sampled, VkImageView storage) {
    std::lock_guard lock(mutex_);
    std::uint32_t slot;
    if (!free_textures_.empty()) {
        slot = free_textures_.back();
        free_textures_.pop_back();
    } else if (next_texture_ < texture_capacity) {
        slot = next_texture_++;
    } else {
        return 0;
    }
    std::array<VkDescriptorImageInfo, 2> images{{{VK_NULL_HANDLE, sampled, VK_IMAGE_LAYOUT_GENERAL},
                                                 {VK_NULL_HANDLE, storage, VK_IMAGE_LAYOUT_GENERAL}}};
    std::array<VkWriteDescriptorSet, 2> writes{};
    std::uint32_t count = 0;
    for (std::uint32_t which = 0; which < 2; ++which) {
        if (!images[which].imageView) continue;
        auto& write = writes[count++];
        write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = which == 0 ? msl::bindless_sampled_image_binding : msl::bindless_storage_image_binding;
        write.dstArrayElement = slot;
        write.descriptorCount = 1;
        write.descriptorType = which == 0 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.pImageInfo = &images[which];
    }
    device->vk.vkUpdateDescriptorSets(device->device, count, writes.data(), 0, nullptr);
    return slot;
}

std::uint32_t Bindless::add_sampler(VkSampler sampler) {
    std::lock_guard lock(mutex_);
    std::uint32_t slot;
    if (!free_samplers_.empty()) {
        slot = free_samplers_.back();
        free_samplers_.pop_back();
    } else if (next_sampler_ < sampler_capacity) {
        slot = next_sampler_++;
    } else {
        return 0;
    }
    VkDescriptorImageInfo info{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = msl::bindless_sampler_binding;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &info;
    device->vk.vkUpdateDescriptorSets(device->device, 1, &write, 0, nullptr);
    return slot;
}

void Bindless::remove_texture(std::uint32_t slot) {
    if (!slot) return;
    std::lock_guard lock(mutex_);
    free_textures_.push_back(slot);
}

void Bindless::remove_sampler(std::uint32_t slot) {
    if (!slot) return;
    std::lock_guard lock(mutex_);
    free_samplers_.push_back(slot);
}

} // namespace alchemy::metal
