#pragma once

// Argument buffers holding textures and samplers (tier 2) on Vulkan: the
// device-wide bindless descriptor set (owner: A5; metal-parity M2).
//
// One descriptor set per device at msl::bindless_descriptor_set: an array of
// sampled images, one of storage images (the same slot index in both) and
// one of samplers, created UPDATE_AFTER_BIND | PARTIALLY_BOUND |
// UPDATE_UNUSED_WHILE_PENDING so slots can be written while command buffers
// that bind the set are recorded or in flight. A texture or sampler gets a
// slot the first time a client asks for its MTLResourceID (gpuResourceID, or
// an MTLArgumentEncoder write); the slot is that ID, and the shader indexes
// the arrays with it. Slot 0 is never handed out (a nil resource). Slots are
// freed once the GPU finished with the object (Device::defer). Images stay in
// VK_IMAGE_LAYOUT_GENERAL, so a written descriptor stays valid.
//
// Without descriptor indexing and the update-after-bind features there is no
// set (Bindless::of answers nullptr): textures in argument buffers then fail
// explicitly, and argumentBuffersSupport does not claim Tier2.
//
// Pure host code. Thread-safe.

#include "alchemy/darwin/metal/device.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace alchemy::metal {

class Bindless {
public:
    // The device's set, made on first use; nullptr without the features.
    [[nodiscard]] static Bindless* of(Device& device);
    // Whether the device has what the set needs (DeviceProfile tier 2).
    [[nodiscard]] static bool supported(const Device& device);

    VkDescriptorSetLayout layout{};
    VkDescriptorSet set{};
    // An empty set layout: compute pipeline layouts put it at set 1, between
    // the kernel's set 0 and the bindless set.
    VkDescriptorSetLayout empty_layout{};
    // Binds the set at msl::bindless_descriptor_set of `layout`.
    void bind(VkCommandBuffer command, VkPipelineBindPoint point, VkPipelineLayout layout) const;
    std::uint32_t texture_capacity{}, sampler_capacity{};

    // A slot holding `sampled` (and `storage` when not null); 0 when full.
    [[nodiscard]] std::uint32_t add_texture(VkImageView sampled, VkImageView storage);
    [[nodiscard]] std::uint32_t add_sampler(VkSampler sampler);
    // Frees a slot. The caller makes sure the GPU no longer reads it.
    void remove_texture(std::uint32_t slot);
    void remove_sampler(std::uint32_t slot);

    void cleanup();
    explicit Bindless(Device& owner) : device(&owner) {}

private:
    Device* device;
    VkDescriptorPool pool_{};
    std::mutex mutex_;
    std::vector<std::uint32_t> free_textures_, free_samplers_;
    std::uint32_t next_texture_{1}, next_sampler_{1};
    void create();
};

} // namespace alchemy::metal
