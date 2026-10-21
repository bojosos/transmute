#pragma once

// QuartzCore <-> Metal-on-Vulkan contract (A7 QuartzCore <-> A5 Metal).
//
// CAMetalLayer presents through the Vulkan device behind the layer's
// MTLDevice. QuartzCore owns every presentation object (VkSurfaceKHR on the
// window server's content HWND, VkSwapchainKHR, the drawable images and the
// per-frame blit); Metal owns the instance, the device, its queue and the
// MTLTexture objects. This header is the whole interface between the two.
//
// ---- What Metal provides (A5) --------------------------------------------------
// One MetalVulkanDevice per MTLDevice object, registered with
// register_metal_device(mtl_device, bridge) when the device is created (and
// unregistered when it is destroyed). Requirements:
// - the VkInstance enables VK_KHR_surface and VK_KHR_win32_surface (when the
//   loader offers them; a device without them makes nextDrawable fail
//   explicitly), API version 1.1 or later;
// - the VkDevice enables VK_KHR_swapchain on queue_family(), which must
//   support graphics + transfer (presentation support is checked per surface);
// - with_queue runs its callback with exclusive use of the queue that Metal
//   command buffers are submitted to (the same mutex Metal holds around its
//   own vkQueueSubmit). QuartzCore never calls it while holding a lock that
//   Metal could wait for, and never calls back into Metal from inside it;
// - wrap_drawable_texture creates the MTLTexture (+1 retained guest object)
//   for a drawable image QuartzCore allocated. The texture keeps the
//   DrawableImage alive (shared ownership) but never destroys the VkImage
//   itself; it reports the image's MTLPixelFormat, width, height,
//   MTLTextureType2D, storage mode private and `texture_usage`, and resolves
//   to `image->image` when encoded (render target, blit source/destination,
//   sampled or storage use as `image->usage` allows).
//
// ---- Layout and ordering rules --------------------------------------------------
// - DrawableImage::layout is the VkImageLayout the image is in once every
//   command buffer Metal has submitted so far has executed. Metal reads it
//   when it records the first use of the image in a command buffer and
//   stores the final layout when it submits that command buffer. QuartzCore
//   does the same for its present blit (it leaves the image in
//   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL). The initial value is
//   VK_IMAGE_LAYOUT_UNDEFINED.
// - Presentation is ordered by queue submission: -[MTLCommandBuffer
//   presentDrawable:] is implemented by Metal as Apple documents it, by
//   sending -present to the drawable (objc.send, so guest drawables work)
//   once the command buffer has been submitted (vkQueueSubmit returned).
//   QuartzCore then submits its blit on the same queue; its first barrier
//   uses srcStageMask ALL_COMMANDS / srcAccessMask MEMORY_WRITE, which
//   orders it after every previously submitted Metal command. No semaphore
//   crosses the interface. -presentDrawable:atTime: sends -presentAtTime:,
//   -presentDrawable:afterMinimumDuration: sends -presentAfterMinimumDuration:.
// - A drawable image is reused for a later -nextDrawable only after the blit
//   that read it completed (QuartzCore waits on its own fence) or, when the
//   drawable was released without being presented, immediately: Metal work
//   that still references it is ordered before the next user's work by the
//   same queue-order rule.
//
// ---- Lifetime ---------------------------------------------------------------------
// The bridge object must keep the VkInstance and VkDevice alive while any
// shared_ptr to it exists (QuartzCore holds one per presenting layer and per
// drawable image). Destroying a DrawableImage waits for QuartzCore's last
// blit of it and then destroys the VkImage and its memory; it never calls
// guest code.
//
// ---- Threading ---------------------------------------------------------------------
// All functions here are callable from any thread. Bridge methods may be
// called concurrently from several guest threads.

#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
// No platform define: the window surface is host::gpu's (create_vulkan_surface).
#include <vulkan/vulkan.h>
#endif

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace alchemy::gui::quartzcore {

// MTLPixelFormat values CAMetalLayer accepts (Metal's MTLPixelFormat enum).
namespace pixel_format {
inline constexpr std::uint64_t invalid = 0, bgra8_unorm = 80, bgra8_unorm_srgb = 81, rgb10a2_unorm = 90,
    bgr10a2_unorm = 94, rgba16_float = 115, bgr10_xr = 554, bgr10_xr_srgb = 555, bgra10_xr = 552,
    bgra10_xr_srgb = 553;
} // namespace pixel_format

// MTLTextureUsage bits.
namespace texture_usage {
inline constexpr std::uint64_t shader_read = 1, shader_write = 2, render_target = 4;
} // namespace texture_usage

#ifdef ALCHEMY_HAVE_VULKAN_HEADERS

// One drawable image allocated by QuartzCore (device-local, one mip level,
// one layer, single sample, optimal tiling).
struct DrawableImage {
    VkImage image{VK_NULL_HANDLE};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkExtent2D extent{};
    VkImageUsageFlags usage{};           // Vulkan usage the image was created with
    std::uint64_t pixel_format{};        // MTLPixelFormat
    std::uint64_t texture_usage{};       // MTLTextureUsage the texture reports
    std::atomic<VkImageLayout> layout{VK_IMAGE_LAYOUT_UNDEFINED};   // see "Layout and ordering rules"

    DrawableImage() = default;
    DrawableImage(const DrawableImage&) = delete;
    DrawableImage& operator=(const DrawableImage&) = delete;
    virtual ~DrawableImage() = default;
};

class MetalVulkanDevice {
public:
    virtual ~MetalVulkanDevice() = default;
    [[nodiscard]] virtual PFN_vkGetInstanceProcAddr get_instance_proc_addr() const = 0;
    [[nodiscard]] virtual VkInstance instance() const = 0;
    [[nodiscard]] virtual VkPhysicalDevice physical_device() const = 0;
    [[nodiscard]] virtual VkDevice device() const = 0;
    [[nodiscard]] virtual std::uint32_t queue_family() const = 0;
    // Exclusive use of Metal's submission queue for the duration of `work`.
    virtual void with_queue(const std::function<void(VkQueue)>& work) = 0;
    // A +1 MTLTexture guest object wrapping `image` (see above).
    [[nodiscard]] virtual std::uint64_t wrap_drawable_texture(const std::shared_ptr<DrawableImage>& image) = 0;
};

#endif // ALCHEMY_HAVE_VULKAN_HEADERS

// Registry: MTLDevice guest object -> its Vulkan bridge. Metal registers a
// device before handing it to the guest; registering an address again
// replaces the bridge. Any thread.
#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
void register_metal_device(std::uint64_t mtl_device, std::shared_ptr<MetalVulkanDevice> device);
[[nodiscard]] std::shared_ptr<MetalVulkanDevice> find_metal_device(std::uint64_t mtl_device);
#endif
void unregister_metal_device(std::uint64_t mtl_device);

// ---- IOSurface-backed textures (Metal's surface_textures.cpp) --------------------
// -[MTLDevice newTextureWithDescriptor:iosurface:plane:] renders into and
// samples from the surface's pixel pages (guest VA == host VA, so `base` is a
// host pointer too). SurfacePages describes them and keeps them mapped while
// it lives, also after the IOSurface object is freed, so GPU work queued on
// them stays valid; it may be dropped on any thread.
//
// GPU writes follow macOS's implicit IOSurface synchronization: a command
// buffer that may write the surface takes a ticket with begin_gpu_write when
// it is submitted and passes it to end_gpu_write when it completed (also when
// it failed). IOSurfaceLock (without kIOSurfaceLockAvoidSync) and the CALayer
// compositor wait until every ticket taken before they started has ended, in
// every process of the tree; end_gpu_write also advances the seed. The CPU
// side's writes are the seed: IOSurfaceUnlock without kIOSurfaceLockReadOnly
// advances it. So a texture whose image was last made equal to the pages at
// seed S with W GPU writes submitted (gpu_writes_submitted) needs no upload
// while both are unchanged; another texture's submitted write changes W at
// once, before its completion moves the seed.
struct SurfacePages {
    std::uint64_t surface{};   // the IOSurfaceRef (not retained here)
    std::uint32_t id{}, width{}, height{}, bytes_per_element{}, pixel_format{};
    std::uint64_t base{}, bytes_per_row{}, alloc_size{};
    bool shared_memory{};      // fx shared memory (vm_allocate'd), not private guest memory
    std::shared_ptr<void> keep;
    [[nodiscard]] std::uint32_t seed() const;
};
// nullopt when `surface_object` is not an IOSurface of this provider.
[[nodiscard]] std::optional<SurfacePages> surface_pages(std::uint64_t surface_object);
[[nodiscard]] std::uint64_t begin_gpu_write(const SurfacePages& pages);
[[nodiscard]] std::uint64_t gpu_writes_submitted(const SurfacePages& pages);
// Returns the seed after the write.
std::uint32_t end_gpu_write(const SurfacePages& pages, std::uint64_t ticket);
// Waits until the writes up to `ticket` have ended (in submission order);
// false after `timeout_ms`.
bool wait_for_gpu_writes(const SurfacePages& pages, std::uint64_t ticket, std::uint32_t timeout_ms);

} // namespace alchemy::gui::quartzcore
