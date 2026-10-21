#pragma once

// Metal resources on Vulkan (owner: A5): MTLBuffer, MTLTexture (and views),
// MTLSamplerState, MTLHeap. Pure host code over Device.
//
// Buffers: every buffer is created with SHADER_DEVICE_ADDRESS usage, because
// the MSL lowering reaches [[buffer(n)]] arguments through 64-bit device
// addresses in the argument table (include/alchemy/msl/compiler.hpp).
// Shared and managed buffers live in host-visible coherent memory that stays
// mapped: contents is the mapping (under alchemy-fx guest VA == host VA, so
// the guest uses the pointer directly) and didModifyRange: needs no flush.
// Private buffers are device-local; contents is null.
//
// Textures: VkImage with optimal tiling regardless of the Metal storage mode
// (macOS Metal has no shared-storage textures either); replaceRegion: /
// getBytes: stage through host memory synchronously, as Metal's CPU access
// is synchronous. Layouts: every texture the Metal layer creates lives in
// VK_IMAGE_LAYOUT_GENERAL from its first submission on (Device queues the
// initial transition into the next submission), so encoders never need a
// layout transition inside a dynamic-rendering instance. Only wrapped
// external images (QuartzCore's CAMetalDrawable images) change layout; their
// current layout is tracked per image (ImageState) and moved to GENERAL by
// the command buffer at their first use in it (CommandBuffer::prepare_texture).
//
// Destruction is deferred until the GPU finished all work submitted before
// the last reference went away (Device::defer).

#include "alchemy/darwin/metal/device.hpp"
#include "alchemy/darwin/metal/formats.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <tuple>

namespace alchemy::metal {

// MTLStorageMode / MTLCPUCacheMode / MTLHazardTrackingMode.
enum class StorageMode : std::uint32_t { shared = 0, managed = 1, private_ = 2, memoryless = 3 };
// MTLResourceOptions packing: cpu cache mode bits 0-3, storage mode bits 4-7,
// hazard tracking bits 8-9.
struct ResourceOptions {
    StorageMode storage{StorageMode::shared};
    std::uint32_t cpu_cache_mode{};
    std::uint32_t hazard_tracking{};
    static ResourceOptions decode(std::uint64_t options) {
        return {static_cast<StorageMode>((options >> 4) & 0xf), static_cast<std::uint32_t>(options & 0xf),
                static_cast<std::uint32_t>((options >> 8) & 0x3)};
    }
    [[nodiscard]] std::uint64_t encode() const {
        return (static_cast<std::uint64_t>(storage) << 4) | cpu_cache_mode | (static_cast<std::uint64_t>(hazard_tracking) << 8);
    }
};

class Buffer {
public:
    // Throws VulkanError for allocation failures and lengths beyond
    // DeviceProfile::max_buffer_length.
    static std::shared_ptr<Buffer> create(std::shared_ptr<Device> device, VkDeviceSize length, ResourceOptions options);
    // A buffer over client memory (-newBufferWithBytesNoCopy:length:options:
    // deallocator:): the pages holding [pointer, pointer + length) imported
    // with VK_EXT_external_memory_host (guest VA == host VA), so the CPU and
    // the GPU share the bytes and contents() is `pointer`. nullptr when the
    // device cannot import them (`why` says why). `released` runs once,
    // after the GPU can no longer use the memory (any thread).
    static std::shared_ptr<Buffer> import_host(std::shared_ptr<Device> device, std::uint8_t* pointer, VkDeviceSize length,
                                               ResourceOptions options, std::function<void()> released, std::string& why);
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    const std::shared_ptr<Device> device;
    VkBuffer buffer{};
    Allocation memory;
    VkDeviceSize length{};
    ResourceOptions options;
    VkDeviceAddress address{};
    std::string label;
    // Host pointer for shared/managed storage, nullptr for private.
    [[nodiscard]] std::uint8_t* contents() const noexcept {
        return options.storage == StorageMode::private_ ? nullptr : memory.mapped;
    }
    // Copies bytes into the buffer: directly for host-visible storage, through
    // a staging buffer and a synchronous submission for private storage.
    void write(VkDeviceSize offset, std::span<const std::uint8_t> bytes);
    void read(VkDeviceSize offset, std::span<std::uint8_t> bytes);

private:
    explicit Buffer(std::shared_ptr<Device> owner) : device(std::move(owner)) {}
    bool imported_{};                        // memory is the client's (import_host)
    std::function<void()> released_;
};

// MTLTextureType.
enum class TextureType : std::uint32_t { type1d = 0, type1d_array = 1, type2d = 2, type2d_array = 3, type2d_ms = 4,
    cube = 5, cube_array = 6, type3d = 7, type2d_ms_array = 8, texture_buffer = 9 };
// MTLTextureUsage bits.
namespace texture_usage {
inline constexpr std::uint64_t shader_read = 1, shader_write = 2, render_target = 4, pixel_format_view = 0x10,
    shader_atomic = 0x20;
} // namespace texture_usage

struct TextureDescriptor {
    TextureType type{TextureType::type2d};
    std::uint32_t pixel_format{pixel_format::rgba8_unorm};
    std::uint32_t width{1}, height{1}, depth{1};
    std::uint32_t mipmap_levels{1}, sample_count{1}, array_length{1};
    std::uint64_t usage{texture_usage::shader_read};
    ResourceOptions options{StorageMode::managed};
    // MTLTextureSwizzleChannels: MTLTextureSwizzle values (Zero=0, One=1,
    // Red=2, Green=3, Blue=4, Alpha=5) for red, green, blue, alpha.
    std::array<std::uint8_t, 4> swizzle{2, 3, 4, 5};
};

// Per-VkImage state shared by a texture and its views. `layout` is the
// image's own, or for wrapped images optionally one shared with the image's
// owner (QuartzCore's drawable image), which then sees every change.
struct ImageState {
    std::shared_ptr<std::atomic<VkImageLayout>> layout{
        std::make_shared<std::atomic<VkImageLayout>>(VK_IMAGE_LAYOUT_UNDEFINED)};
    bool external{};   // swapchain image: layout changes are tracked
};

struct Region { std::uint32_t x{}, y{}, z{}, width{1}, height{1}, depth{1}; };

// ---- g3: IOSurface-backed textures (surface_textures.cpp) --------------------------
// A texture from -[MTLDevice newTextureWithDescriptor:iosurface:plane:] keeps
// its own (optimal) VkImage and the IOSurface's pages coherent at Metal's
// synchronisation points. A command buffer that uses the texture (or a view
// of it) calls record_head at the start of each submission batch that uses
// it (after the batch's event waits), which brings the image up to date with
// the pages when they may have changed (`force`: always, as after an event
// wait another process may have written them), and - after uses that may
// have written the texture - record_tail before each encodeSignalEvent: and
// after its last command, which copies the image into the pages. The work
// either returns runs once the command buffer completed (or failed). Both
// record outside any rendering instance, with the image in
// VK_IMAGE_LAYOUT_GENERAL, and bring their own barriers. `writes_before` is
// writes_submitted() when the command buffer first used the texture in that
// batch: GPU writes to the surface the use must see.
class SurfaceBacking {
public:
    virtual ~SurfaceBacking() = default;
    [[nodiscard]] virtual std::uint64_t writes_submitted() const = 0;
    [[nodiscard]] virtual std::function<void()> record_head(VkCommandBuffer command, bool force, std::uint64_t writes_before) = 0;
    [[nodiscard]] virtual std::function<void()> record_tail(VkCommandBuffer command) = 0;
    [[nodiscard]] virtual std::uint64_t surface_object() const noexcept = 0;   // the IOSurfaceRef
    [[nodiscard]] virtual std::uint64_t plane() const noexcept = 0;
};

class Texture {
public:
    static std::shared_ptr<Texture> create(std::shared_ptr<Device> device, const TextureDescriptor& descriptor);
    // newTextureViewWithPixelFormat:textureType:levels:slices:swizzle:.
    // Levels and slices are relative to `parent`.
    static std::shared_ptr<Texture> create_view(const std::shared_ptr<Texture>& parent, std::uint32_t pixel_format,
        TextureType type, std::uint32_t first_level, std::uint32_t level_count, std::uint32_t first_slice,
        std::uint32_t slice_count, std::array<std::uint8_t, 4> swizzle);
    // -[MTLBuffer newTextureWithDescriptor:offset:bytesPerRow:] with
    // MTLTextureTypeTextureBuffer: a texel buffer view of the buffer.
    static std::shared_ptr<Texture> create_buffer_texture(const std::shared_ptr<Buffer>& buffer,
        const TextureDescriptor& descriptor, VkDeviceSize offset, VkDeviceSize bytes_per_row);
    // An image Metal does not own (swapchain image): never destroyed here.
    // With `shared_layout` the image's layout lives there (the pointer may
    // alias, and so keep alive, the owner's image object) and starts at
    // its current value; otherwise at `current_layout`.
    // `usage` is the Vulkan usage the owner created the image with (0 when
    // unknown: the views then inherit it unrestricted).
    static std::shared_ptr<Texture> wrap(std::shared_ptr<Device> device, VkImage image, const TextureDescriptor& descriptor,
        VkImageLayout current_layout, std::shared_ptr<std::atomic<VkImageLayout>> shared_layout = nullptr,
        VkImageUsageFlags usage = 0);
    ~Texture();
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    const std::shared_ptr<Device> device;
    TextureDescriptor descriptor;
    const PixelFormatInfo* info{};
    VkFormat format{};
    VkImage image{};
    Allocation memory;
    // Shader views of every level and slice of this texture: `view` for
    // sampled access (Metal swizzle applied, view usage SAMPLED), and
    // `storage_view` for read/write access (identity swizzle, usage STORAGE;
    // null when the image or the view format cannot be a storage image).
    // They are the same handle when both are possible and the swizzle is
    // the identity.
    VkImageView view{};
    VkImageView storage_view{};
    VkBufferView buffer_view{};            // texture buffers
    std::shared_ptr<Buffer> buffer;        // texture buffers: the backing buffer
    VkDeviceSize buffer_offset{}, bytes_per_row{};
    std::shared_ptr<Texture> parent;       // views: the texture owning the image
    std::uint32_t base_level{}, base_layer{};   // of this texture inside the VkImage
    std::shared_ptr<ImageState> state;
    std::string label;
    bool owns_image{};
    // The VkImage's usage when this provider created it (views copy the
    // root's); for wrapped images the owner's when it told, else 0.
    VkImageUsageFlags image_usage{};
    // The VkImage's create flags (MUTABLE_FORMAT decides which view formats
    // are possible); views copy the root's.
    VkImageCreateFlags image_flags{};
    // g3: the IOSurface behind this texture (set on the root texture; views
    // reach it through `parent`), see SurfaceBacking.
    std::shared_ptr<SurfaceBacking> surface;
    [[nodiscard]] const std::shared_ptr<SurfaceBacking>& surface_backing() const noexcept {
        return parent ? parent->surface : surface;
    }

    [[nodiscard]] bool is_buffer() const noexcept { return descriptor.type == TextureType::texture_buffer; }
    // Vulkan array layers of this texture (cubes count 6 per cube).
    [[nodiscard]] std::uint32_t layer_count() const noexcept;
    [[nodiscard]] VkImageAspectFlags aspects() const noexcept { return info->aspects(); }
    [[nodiscard]] VkImageAspectFlags view_aspects() const noexcept;
    [[nodiscard]] VkExtent3D level_extent(std::uint32_t level) const noexcept;
    // A single-level view for attachments (cached): `level` and `layer`
    // relative to this texture; `layers` > 1 for layered rendering.
    [[nodiscard]] VkImageView attachment_view(std::uint32_t level, std::uint32_t layer, std::uint32_t layers);
    // replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:.
    void replace_region(const Region& region, std::uint32_t level, std::uint32_t slice, std::span<const std::uint8_t> bytes,
                        std::uint64_t bytes_per_row, std::uint64_t bytes_per_image);
    // getBytes:bytesPerRow:bytesPerImage:fromRegion:mipmapLevel:slice:.
    void get_bytes(std::span<std::uint8_t> bytes, std::uint64_t bytes_per_row, std::uint64_t bytes_per_image,
                   const Region& region, std::uint32_t level, std::uint32_t slice);
    // The buffer<->image copy layout Metal's bytesPerRow/bytesPerImage describe.
    [[nodiscard]] VkBufferImageCopy copy_region(VkDeviceSize buffer_offset, std::uint64_t bytes_per_row,
        std::uint64_t bytes_per_image, const Region& region, std::uint32_t level, std::uint32_t slice,
        VkImageAspectFlags aspect) const;
    // Size in bytes of a copy of `region` with the given pitches.
    [[nodiscard]] std::uint64_t copy_size(std::uint64_t bytes_per_row, std::uint64_t bytes_per_image,
                                          const Region& region) const;
    [[nodiscard]] std::uint64_t allocated_size() const noexcept { return memory.size; }

private:
    explicit Texture(std::shared_ptr<Device> owner) : device(std::move(owner)) {}
    void repack(const std::uint8_t* source, std::uint64_t source_row, std::uint64_t source_image, std::uint8_t* destination,
                std::uint64_t destination_row, std::uint64_t destination_image, const Region& region) const;
    void create_shader_views(VkImageViewType type, VkFormat view_format, std::uint32_t levels, std::uint32_t layers,
                             const VkComponentMapping& components);
    std::mutex views_mutex_;
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, VkImageView> attachment_views_;
};

[[nodiscard]] VkImageViewType view_type(TextureType type);
[[nodiscard]] VkComponentMapping component_mapping(const std::array<std::uint8_t, 4>& swizzle,
                                                   const VkComponentMapping& format_swizzle);

struct SamplerDescriptor {
    std::uint32_t min_filter{}, mag_filter{}, mip_filter{};   // MTLSamplerMinMagFilter / MTLSamplerMipFilter
    std::array<std::uint32_t, 3> address{};                   // MTLSamplerAddressMode s, t, r
    std::uint32_t border_color{};                             // MTLSamplerBorderColor
    std::uint32_t max_anisotropy{1};
    std::uint32_t compare_function{};                         // MTLCompareFunction; Never = no comparison
    float lod_min{0}, lod_max{3.40282347e+38F};
    bool normalized_coordinates{true}, lod_average{}, support_argument_buffers{};
    std::string label;
};

class Sampler {
public:
    static std::shared_ptr<Sampler> create(std::shared_ptr<Device> device, const SamplerDescriptor& descriptor);
    ~Sampler();
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;
    // The encoders' setSamplerState:lodMinClamp:lodMaxClamp:atIndex: (ANGLE's
    // per-texture base/max level): this sampler with its LOD clamps replaced
    // by these. Made once per distinct pair and kept with this sampler (a
    // bounded cache, see max_clamped_samplers); a
    // pair equal to the descriptor's answers the sampler itself.
    [[nodiscard]] static std::shared_ptr<Sampler> with_lod_clamp(const std::shared_ptr<Sampler>& sampler, float lod_min,
                                                                 float lod_max);
    // At most this many derived samplers are kept per sampler.
    static constexpr std::size_t max_clamped_samplers = 64;
    const std::shared_ptr<Device> device;
    SamplerDescriptor descriptor;
    VkSampler sampler{};
private:
    explicit Sampler(std::shared_ptr<Device> owner) : device(std::move(owner)) {}
    std::mutex clamped_mutex_;
    std::map<std::pair<float, float>, std::shared_ptr<Sampler>> clamped_;
};
[[nodiscard]] VkSamplerCreateInfo sampler_info(const Device& device, const SamplerDescriptor& descriptor);
// Samplers inside argument buffers (MTLArgumentEncoder setSamplerState:):
// the buffer holds an opaque 64-bit token per sampler (0 = nil), resolved
// when a draw or dispatch binds the argument buffer. A token stays valid
// while its sampler lives; resolving a dead one throws.
[[nodiscard]] std::uint64_t sampler_token(const std::shared_ptr<Sampler>& sampler);
[[nodiscard]] std::shared_ptr<Sampler> sampler_from_token(std::uint64_t token);

// MTLHeap (MTLHeapTypeAutomatic): resources are sub-allocated from the
// device allocator and accounted against the heap's size, which gives the
// documented residency and accounting behaviour. Placement heaps and
// aliasing (makeAliasable) are not translated.
class Heap {
public:
    Heap(std::shared_ptr<Device> owner, std::uint64_t heap_size, ResourceOptions heap_options, std::uint32_t heap_type)
        : device(std::move(owner)), size(heap_size), options(heap_options), type(heap_type) {}
    const std::shared_ptr<Device> device;
    const std::uint64_t size;
    const ResourceOptions options;
    const std::uint32_t type;   // MTLHeapType
    std::string label;
    std::atomic<std::uint64_t> used{0};
    // Reserves `bytes`; false when the heap is full (Metal returns nil).
    bool reserve(std::uint64_t bytes);
    void release(std::uint64_t bytes) { used -= bytes; }
};

} // namespace alchemy::metal
