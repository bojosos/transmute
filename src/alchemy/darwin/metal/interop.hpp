#pragma once

// Metal <-> QuartzCore interop (owner: A5; implemented against by A7,
// src/alchemy/quartzcore). Internal to alchemy-fx's GUI providers: not a
// guest ABI and not a wave-0 contract header.
//
// Presentation is QuartzCore's (quartzcore_vulkan.hpp, wired in
// quartzcore_link.cpp): Metal registers each MTLDevice object as a
// MetalVulkanDevice; QuartzCore wraps its drawable images through it (layout
// shared with DrawableImage::layout) and -[MTLCommandBuffer presentDrawable:]
// sends -present / -presentAtTime: / -presentAfterMinimumDuration: to the
// drawable right after the command buffer's vkQueueSubmit2. No semaphore
// crosses the interface; Metal leaves the drawable's layout GENERAL.
//
// ---- Device ------------------------------------------------------------------------
// CAMetalLayer.device is an MTLDevice from this provider. device_of() maps the
// guest object to the host Device: its VkInstance (created with
// VK_KHR_surface + VK_KHR_win32_surface when the loader has them),
// VkPhysicalDevice, VkDevice (VK_KHR_swapchain enabled when available),
// queue and queue family, and the `vk` dispatch table (load swapchain
// entry points yourself with vk.vkGetDeviceProcAddr / vkGetInstanceProcAddr).
// The queue is shared with every Metal command buffer: hold
// Device::queue_mutex() around vkQueuePresentKHR.
//
// ---- Drawable textures ----------------------------------------------------------
// A CAMetalDrawable's texture is a host Texture wrapping QuartzCore's image:
//   auto texture = metal::Texture::wrap(device, image, descriptor, layout, shared_layout);
//   std::uint64_t object = metal::new_texture_object(texture);   // +1 guest id<MTLTexture>
// The shared layout is the atomic QuartzCore also updates. Metal transitions
// from it to GENERAL at the image's first use in a command buffer: before a
// blit or compute command, before the render pass that uses it as an
// attachment, and - for a texture bound to a render encoder, where no barrier
// may be recorded inside the pass - at the start of the command buffer.
//
// Threading: these functions are callable from any guest thread; they never
// call guest code.

#include "alchemy/darwin/metal/device.hpp"
#include "alchemy/darwin/metal/resources.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace alchemy::metal {

// The host Device of a guest id<MTLDevice> from this provider, nullptr for
// any other object. Valid after install_metal_gui.
[[nodiscard]] std::shared_ptr<Device> device_of(std::uint64_t device_object);
// A +1 guest id<MTLTexture> for a host texture, and the reverse (nullptr for
// objects that are not Metal textures of this provider).
[[nodiscard]] std::uint64_t new_texture_object(std::shared_ptr<Texture> texture);
[[nodiscard]] std::shared_ptr<Texture> texture_of(std::uint64_t texture_object);

// ---- Host memory imported into the device (g3, external_memory.cpp) -----------------
// VK_EXT_external_memory_host. host_import_alignment: the device's
// minImportedHostPointerAlignment, 0 without the extension (Intel Iris Xe:
// 4 KiB; IOSurface pages are 16 KiB aligned). import_host_memory imports
// [pointer, pointer + size) (both aligned) as host-coherent memory of a type
// in `memory_type_bits` (the requirements of a resource created with
// VkExternalMemoryBufferCreateInfo{VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT});
// nullopt, with the reason in *why, when the device cannot. `flags` are
// chained as VkMemoryAllocateFlagsInfo (VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
// for buffers with SHADER_DEVICE_ADDRESS usage, as every MTLBuffer has). The
// caller binds and frees the memory, and keeps the range mapped until every
// submission using it has completed (free it through Device::defer).
struct HostImport { VkDeviceMemory memory{}; std::uint32_t type{}; };
[[nodiscard]] VkDeviceSize host_import_alignment(const Device& device);
[[nodiscard]] std::optional<HostImport> import_host_memory(const Device& device, void* pointer, VkDeviceSize size,
                                                           std::uint32_t memory_type_bits, std::string* why = nullptr,
                                                           VkMemoryAllocateFlags flags = 0);

} // namespace alchemy::metal
