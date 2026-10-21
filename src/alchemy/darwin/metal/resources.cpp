#include "alchemy/darwin/metal/resources.hpp"
#include "alchemy/darwin/metal/interop.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>

// Metal buffers, textures, samplers and heaps on Vulkan. See resources.hpp.
namespace alchemy::metal {
namespace {

constexpr VkBufferUsageFlags all_buffer_usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;

MemoryKind buffer_memory(const ResourceOptions& options) {
    switch (options.storage) {
    case StorageMode::private_: case StorageMode::memoryless: return MemoryKind::device_local;
    default:
        // MTLCPUCacheModeWriteCombined (1) asks for uncached; default cache
        // mode reads back through cached memory when the device has it.
        return options.cpu_cache_mode == 1 ? MemoryKind::host_visible : MemoryKind::host_cached;
    }
}

std::uint32_t mip_extent(std::uint32_t value, std::uint32_t level) { return std::max(1U, value >> level); }

} // namespace

// ---- buffers ----------------------------------------------------------------------

std::shared_ptr<Buffer> Buffer::create(std::shared_ptr<Device> device, VkDeviceSize length, ResourceOptions options) {
    if (length > device->profile().max_buffer_length)
        throw VulkanError("Alchemy Metal: buffer length " + std::to_string(length) + " exceeds maxBufferLength " +
                          std::to_string(device->profile().max_buffer_length));
    if (options.storage == StorageMode::memoryless)
        throw VulkanError("Alchemy Metal: MTLStorageModeMemoryless is invalid for buffers");
    std::shared_ptr<Buffer> result(new Buffer(std::move(device)));
    auto& d = *result->device;
    result->length = length;
    result->options = options;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    // Vulkan buffers cannot be empty; fillBuffer works on 4-byte units.
    info.size = std::max<VkDeviceSize>(4, (length + 3) & ~VkDeviceSize{3});
    info.usage = all_buffer_usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(d.vk.vkCreateBuffer(d.device, &info, nullptr, &result->buffer), "vkCreateBuffer");
    VkMemoryRequirements requirements{};
    d.vk.vkGetBufferMemoryRequirements(d.device, result->buffer, &requirements);
    // Argument-table addresses and setBytes data are 16-byte aligned at least.
    requirements.alignment = std::max<VkDeviceSize>(requirements.alignment, 256);
    result->memory = d.allocate(requirements, buffer_memory(options), false);
    check(d.vk.vkBindBufferMemory(d.device, result->buffer, result->memory.memory, result->memory.offset),
          "vkBindBufferMemory");
    VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    address.buffer = result->buffer;
    result->address = d.vk.vkGetBufferDeviceAddress(d.device, &address);
    return result;
}

std::shared_ptr<Buffer> Buffer::import_host(std::shared_ptr<Device> device, std::uint8_t* pointer, VkDeviceSize length,
                                            ResourceOptions options, std::function<void()> released, std::string& why) {
    const auto refuse = [&](std::string reason) -> std::shared_ptr<Buffer> { why = std::move(reason); return nullptr; };
    if (!pointer || !length) return refuse("a null pointer or zero length");
    if (options.storage == StorageMode::private_ || options.storage == StorageMode::memoryless)
        return refuse("client memory needs shared or managed storage");
    if (length > device->profile().max_buffer_length) return refuse("the length exceeds maxBufferLength");
    // Metal asks for a page-aligned pointer; the import covers whole units of
    // the import alignment (guest pages are 16 KiB, so the units exist).
    const auto alignment = host_import_alignment(*device);
    if (!alignment) return refuse("the Vulkan device has no VK_EXT_external_memory_host");
    if (reinterpret_cast<std::uintptr_t>(pointer) % alignment)
        return refuse("the pointer is not aligned to " + std::to_string(alignment) + " bytes");
    const auto span = (length + alignment - 1) / alignment * alignment;

    std::shared_ptr<Buffer> result(new Buffer(device));
    auto& d = *result->device;
    result->length = length;
    result->options = options;
    VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.pNext = &external;
    info.size = span;
    info.usage = all_buffer_usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (d.vk.vkCreateBuffer(d.device, &info, nullptr, &result->buffer) != VK_SUCCESS) {
        result->buffer = VK_NULL_HANDLE;
        return refuse("vkCreateBuffer of an external host buffer failed");
    }
    const auto discard = [&](std::string reason) {
        d.vk.vkDestroyBuffer(d.device, result->buffer, nullptr);
        result->buffer = VK_NULL_HANDLE;
        return refuse(std::move(reason));
    };
    VkMemoryRequirements requirements{};
    d.vk.vkGetBufferMemoryRequirements(d.device, result->buffer, &requirements);
    // g3's helper (host-coherent memory); argument tables and shaders reach
    // buffers by device address.
    std::string reason;
    const auto imported = import_host_memory(d, pointer, span, requirements.memoryTypeBits, &reason,
                                             VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT);
    if (!imported) return discard(reason);
    if (d.vk.vkBindBufferMemory(d.device, result->buffer, imported->memory, 0) != VK_SUCCESS) {
        d.vk.vkFreeMemory(d.device, imported->memory, nullptr);
        return discard("vkBindBufferMemory of the imported pointer failed");
    }
    result->memory.memory = imported->memory;
    result->memory.size = span;
    result->memory.mapped = pointer;
    result->memory.type = imported->type;
    result->imported_ = true;
    result->released_ = std::move(released);
    VkBufferDeviceAddressInfo address_info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    address_info.buffer = result->buffer;
    result->address = d.vk.vkGetBufferDeviceAddress(d.device, &address_info);
    return result;
}

Buffer::~Buffer() {
    if (!buffer) return;
    Device* owner = device.get();
    auto handle = buffer;
    auto allocation = memory;
    if (imported_) {
        // The client's memory: no unmap and no pool, then the client's release.
        owner->defer([owner, handle, allocation, released = std::move(released_)] {
            owner->vk.vkDestroyBuffer(owner->device, handle, nullptr);
            owner->vk.vkFreeMemory(owner->device, allocation.memory, nullptr);
            if (released) released();
        });
        return;
    }
    owner->defer([owner, handle, allocation]() mutable {
        owner->vk.vkDestroyBuffer(owner->device, handle, nullptr);
        owner->free(allocation);
    });
}

void Buffer::write(VkDeviceSize offset, std::span<const std::uint8_t> bytes) {
    if (offset > length || bytes.size() > length - offset) throw std::out_of_range("Alchemy Metal: buffer write out of range");
    if (bytes.empty()) return;
    if (const auto host = contents()) { std::memcpy(host + offset, bytes.data(), bytes.size()); return; }
    auto staging = Buffer::create(device, bytes.size(), {StorageMode::shared, 1});
    std::memcpy(staging->contents(), bytes.data(), bytes.size());
    device->immediate([&](VkCommandBuffer command) {
        const VkBufferCopy copy{0, offset, bytes.size()};
        device->vk.vkCmdCopyBuffer(command, staging->buffer, buffer, 1, &copy);
    });
}

void Buffer::read(VkDeviceSize offset, std::span<std::uint8_t> bytes) {
    if (offset > length || bytes.size() > length - offset) throw std::out_of_range("Alchemy Metal: buffer read out of range");
    if (bytes.empty()) return;
    if (const auto host = contents()) { std::memcpy(bytes.data(), host + offset, bytes.size()); return; }
    auto staging = Buffer::create(device, bytes.size(), {StorageMode::shared, 0});
    device->immediate([&](VkCommandBuffer command) {
        const VkBufferCopy copy{offset, 0, bytes.size()};
        device->vk.vkCmdCopyBuffer(command, buffer, staging->buffer, 1, &copy);
    });
    std::memcpy(bytes.data(), staging->contents(), bytes.size());
}

// ---- textures ---------------------------------------------------------------------

VkImageViewType view_type(TextureType type) {
    switch (type) {
    case TextureType::type1d: return VK_IMAGE_VIEW_TYPE_1D;
    case TextureType::type1d_array: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case TextureType::type2d: case TextureType::type2d_ms: return VK_IMAGE_VIEW_TYPE_2D;
    case TextureType::type2d_array: case TextureType::type2d_ms_array: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case TextureType::cube: return VK_IMAGE_VIEW_TYPE_CUBE;
    case TextureType::cube_array: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    case TextureType::type3d: return VK_IMAGE_VIEW_TYPE_3D;
    case TextureType::texture_buffer: break;
    }
    throw VulkanError("Alchemy Metal: texture buffers have no image view");
}

VkComponentMapping component_mapping(const std::array<std::uint8_t, 4>& swizzle, const VkComponentMapping& format) {
    // MTLTextureSwizzle: Zero, One, Red, Green, Blue, Alpha.
    const auto channel = [&](std::uint8_t value) -> VkComponentSwizzle {
        const VkComponentSwizzle base[4] = {format.r == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_R : format.r,
                                            format.g == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_G : format.g,
                                            format.b == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_B : format.b,
                                            format.a == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_A : format.a};
        switch (value) {
        case 0: return VK_COMPONENT_SWIZZLE_ZERO;
        case 1: return VK_COMPONENT_SWIZZLE_ONE;
        case 2: case 3: case 4: case 5: return base[value - 2];
        default: throw VulkanError("Alchemy Metal: invalid MTLTextureSwizzle " + std::to_string(value));
        }
    };
    VkComponentMapping result{channel(swizzle[0]), channel(swizzle[1]), channel(swizzle[2]), channel(swizzle[3])};
    const auto identity = [](VkComponentSwizzle value, VkComponentSwizzle self) {
        return value == self ? VK_COMPONENT_SWIZZLE_IDENTITY : value;
    };
    result.r = identity(result.r, VK_COMPONENT_SWIZZLE_R);
    result.g = identity(result.g, VK_COMPONENT_SWIZZLE_G);
    result.b = identity(result.b, VK_COMPONENT_SWIZZLE_B);
    result.a = identity(result.a, VK_COMPONENT_SWIZZLE_A);
    return result;
}

std::uint32_t Texture::layer_count() const noexcept {
    switch (descriptor.type) {
    case TextureType::cube: return 6;
    case TextureType::cube_array: return 6 * descriptor.array_length;
    case TextureType::type1d_array: case TextureType::type2d_array: case TextureType::type2d_ms_array:
        return descriptor.array_length;
    default: return 1;
    }
}

VkImageAspectFlags Texture::view_aspects() const noexcept {
    const auto all = aspects();
    // Sampling a combined depth/stencil texture reads depth; a stencil view
    // format (X32_Stencil8) reads stencil.
    if (info->stencil_view) return VK_IMAGE_ASPECT_STENCIL_BIT;
    if ((all & VK_IMAGE_ASPECT_DEPTH_BIT) && (all & VK_IMAGE_ASPECT_STENCIL_BIT)) return VK_IMAGE_ASPECT_DEPTH_BIT;
    return all;
}

VkExtent3D Texture::level_extent(std::uint32_t level) const noexcept {
    return {mip_extent(descriptor.width, level), mip_extent(descriptor.height, level), mip_extent(descriptor.depth, level)};
}

namespace {

bool is_identity(const VkComponentMapping& components) {
    return components.r == VK_COMPONENT_SWIZZLE_IDENTITY && components.g == VK_COMPONENT_SWIZZLE_IDENTITY &&
           components.b == VK_COMPONENT_SWIZZLE_IDENTITY && components.a == VK_COMPONENT_SWIZZLE_IDENTITY;
}

// The image usages a view of `format` can carry: Vulkan requires the view
// format to support every usage the view inherits from its image
// (VUID-VkImageViewCreateInfo-usage-02274/02275/02276/02277), which a
// MUTABLE_FORMAT image's sRGB views (no storage) or a storage-capable image's
// attachment views would otherwise violate.
VkImageUsageFlags view_usage_for(VkFormatFeatureFlags features) {
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    return usage;
}

} // namespace

void Texture::create_shader_views(VkImageViewType type, VkFormat view_format, std::uint32_t levels, std::uint32_t layers,
                                  const VkComponentMapping& components) {
    const auto features = device->format_features(view_format);
    // Wrapped images whose owner did not report its usage: the view inherits
    // the image's usage unrestricted (the owner created it for these uses).
    const VkImageUsageFlags image = image_usage ? image_usage
                                                : VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    const bool sampled = (image & VK_IMAGE_USAGE_SAMPLED_BIT) && (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    // Storage descriptors need the identity swizzle
    // (VUID-VkWriteDescriptorSet-descriptorType-00336) and one aspect.
    const bool color = !info->has_depth() && !info->has_stencil();
    const bool storage = color && (image & VK_IMAGE_USAGE_STORAGE_BIT) && (features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    const auto make = [&](VkImageUsageFlags usage, const VkComponentMapping& mapping) {
        VkImageViewUsageCreateInfo restricted{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
        restricted.usage = usage;
        VkImageViewCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        if (image_usage) create.pNext = &restricted;
        create.image = this->image;
        create.viewType = type;
        create.format = view_format;
        create.components = mapping;
        create.subresourceRange = {view_aspects(), base_level, levels, base_layer, layers};
        VkImageView result{};
        check(device->vk.vkCreateImageView(device->device, &create, nullptr, &result), "vkCreateImageView");
        return result;
    };
    if (sampled && storage && is_identity(components)) {
        view = storage_view = make(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, components);
        return;
    }
    if (sampled) view = make(VK_IMAGE_USAGE_SAMPLED_BIT, components);
    if (storage) storage_view = make(VK_IMAGE_USAGE_STORAGE_BIT, VkComponentMapping{});
}

std::shared_ptr<Texture> Texture::create(std::shared_ptr<Device> owner, const TextureDescriptor& descriptor) {
    if (descriptor.type == TextureType::texture_buffer)
        throw VulkanError("Alchemy Metal: MTLTextureTypeTextureBuffer textures are created from a buffer");
    const auto* info = pixel_format_info(descriptor.pixel_format);
    if (!info) throw VulkanError("Alchemy Metal: unsupported MTLPixelFormat " + std::to_string(descriptor.pixel_format));
    std::shared_ptr<Texture> result(new Texture(std::move(owner)));
    auto& d = *result->device;
    result->descriptor = descriptor;
    result->info = info;
    result->format = d.texture_format(descriptor.pixel_format);
    result->state = std::make_shared<ImageState>();
    result->owns_image = true;
    auto& desc = result->descriptor;
    desc.width = std::max(1U, desc.width);
    desc.height = std::max(1U, desc.height);
    desc.depth = std::max(1U, desc.depth);
    desc.mipmap_levels = std::max(1U, desc.mipmap_levels);
    desc.array_length = std::max(1U, desc.array_length);
    desc.sample_count = std::max(1U, desc.sample_count);

    const auto features = d.format_features(result->format);
    if (!features) throw VulkanError("Alchemy Metal: MTLPixelFormat" + std::string(info->name) + " is not supported by " + d.profile().name);
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    switch (desc.type) {
    case TextureType::type1d: case TextureType::type1d_array: image.imageType = VK_IMAGE_TYPE_1D; break;
    case TextureType::type3d: image.imageType = VK_IMAGE_TYPE_3D; break;
    default: image.imageType = VK_IMAGE_TYPE_2D; break;
    }
    if (desc.type == TextureType::cube || desc.type == TextureType::cube_array)
        image.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    const bool color = !info->has_depth() && !info->has_stencil();
    // Views with another format of the same size (PixelFormatView, and the
    // sRGB/linear pairs Metal allows without it).
    if ((desc.usage & texture_usage::pixel_format_view) || srgb_partner(desc.pixel_format))
        if (color) image.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    // Render passes attach one slice of a 3D texture through a 2D view.
    if (desc.type == TextureType::type3d && color && (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
        image.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    // Metal views a square 2D array of six or more slices as a cube (array)
    // without declaring it at creation; Vulkan needs CUBE_COMPATIBLE.
    if (desc.type == TextureType::type2d_array && desc.width == desc.height && desc.array_length >= 6 &&
        desc.sample_count == 1)
        image.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    image.format = result->format;
    image.extent = {desc.width, image.imageType == VK_IMAGE_TYPE_1D ? 1 : desc.height,
                    image.imageType == VK_IMAGE_TYPE_3D ? desc.depth : 1};
    image.mipLevels = desc.mipmap_levels;
    image.arrayLayers = result->layer_count();
    image.samples = static_cast<VkSampleCountFlagBits>(desc.sample_count);
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // Metal's usage flags are hints: Metal lets shaders write and render into
    // textures created without ShaderWrite / RenderTarget (Blender binds
    // ShaderRead-only textures to access::read_write arguments and renders
    // into them), while Vulkan enforces the image usage
    // (VUID-VkWriteDescriptorSet-descriptorType-00339,
    // VUID-VkRenderingInfo-colorAttachmentCount-06087). Every usage the
    // format supports is therefore given; transfers are always allowed.
    image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) image.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    // Storage: single-sampled color formats with storage support (Metal's
    // read_write tiers never cover multisampled textures). sRGB formats have
    // no storage support in Vulkan; binding one to a writable argument fails
    // explicitly at the bind (CommandBuffer::write_set).
    if (color && !info->compressed() && (features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && desc.sample_count == 1)
        image.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    // Attachments: depth/stencil textures are always attachable (Blender's
    // depth upload draws into depth textures created for reading only), and
    // so are color textures whose format can be a color attachment.
    if (!color && (features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
        image.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (color && !info->compressed() && (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
        image.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        // Framebuffer fetch reads color attachments as input attachments of
        // the same render pass (dynamic rendering local read).
        if (d.dynamic_rendering_local_read) image.usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    }
    if (desc.usage & texture_usage::render_target) {
        if (!color && !(image.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
            throw VulkanError("Alchemy Metal: MTLPixelFormat" + std::string(info->name) + " cannot be a depth/stencil target here");
        if (color && !(image.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
            throw VulkanError("Alchemy Metal: MTLPixelFormat" + std::string(info->name) + " cannot be a render target here");
    }
    // Memoryless textures (tile memory on Apple GPUs) are ordinary
    // device-local images: Vulkan's TRANSIENT_ATTACHMENT usage would forbid
    // the sampled/storage/transfer usages above, and Metal's rules for
    // memoryless attachments (load Clear/DontCare, store DontCare) keep their
    // contents unobserved either way.
    if (desc.sample_count > 1 && !(d.profile().texture_sample_counts & desc.sample_count))
        throw VulkanError("Alchemy Metal: sample count " + std::to_string(desc.sample_count) + " is not supported");
    check(d.vk.vkCreateImage(d.device, &image, nullptr, &result->image), "vkCreateImage");
    result->image_usage = image.usage;
    result->image_flags = image.flags;
    VkMemoryRequirements requirements{};
    d.vk.vkGetImageMemoryRequirements(d.device, result->image, &requirements);
    result->memory = d.allocate(requirements, MemoryKind::device_local, true);
    check(d.vk.vkBindImageMemory(d.device, result->image, result->memory.memory, result->memory.offset), "vkBindImageMemory");
    if (image.usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT))
        result->create_shader_views(view_type(desc.type), result->format, desc.mipmap_levels, result->layer_count(),
                                    component_mapping(desc.swizzle, info->swizzle));
    *result->state->layout = VK_IMAGE_LAYOUT_GENERAL;
    d.queue_initial_layout(result->image, info->aspects());
    return result;
}

std::shared_ptr<Texture> Texture::create_view(const std::shared_ptr<Texture>& parent, std::uint32_t pixel_format,
    TextureType type, std::uint32_t first_level, std::uint32_t level_count, std::uint32_t first_slice,
    std::uint32_t slice_count, std::array<std::uint8_t, 4> swizzle) {
    if (parent->is_buffer()) throw VulkanError("Alchemy Metal: views of texture buffers are not translated");
    const auto* info = pixel_format_info(pixel_format);
    if (!info) throw VulkanError("Alchemy Metal: unsupported MTLPixelFormat " + std::to_string(pixel_format));
    std::shared_ptr<Texture> result(new Texture(parent->device));
    const auto& root = parent->parent ? parent->parent : parent;
    result->parent = root;
    result->info = info;
    result->format = parent->device->texture_format(pixel_format);
    result->image = root->image;
    result->image_usage = root->image_usage;
    result->image_flags = root->image_flags;
    result->state = root->state;
    // Another format needs a MUTABLE_FORMAT image (MTLTextureUsagePixelFormatView,
    // or an sRGB/linear pair); Metal's own validation requires the usage too.
    if (result->format != root->format && !(root->image_flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT))
        throw VulkanError("Alchemy Metal: MTLPixelFormat" + std::string(info->name) + " view of an MTLPixelFormat" +
                          std::string(root->info->name) + " texture" +
                          (root->label.empty() ? std::string() : " '" + root->label + "'") +
                          " created without MTLTextureUsagePixelFormatView");
    result->descriptor = parent->descriptor;
    result->descriptor.pixel_format = pixel_format;
    result->descriptor.type = type;
    result->descriptor.swizzle = swizzle;
    result->base_level = parent->base_level + first_level;
    result->base_layer = parent->base_layer + first_slice;
    // Sizes of a view are those of its first level.
    const auto extent = parent->level_extent(first_level);
    result->descriptor.width = extent.width;
    result->descriptor.height = extent.height;
    result->descriptor.depth = extent.depth;
    result->descriptor.mipmap_levels = level_count;
    std::uint32_t layers = slice_count;
    switch (type) {
    case TextureType::cube: layers = 6; result->descriptor.array_length = 1; break;
    case TextureType::cube_array:
        // Slices of a cube array view count faces.
        result->descriptor.array_length = std::max(1U, slice_count / 6);
        layers = result->descriptor.array_length * 6;
        break;
    default: result->descriptor.array_length = slice_count; break;
    }
    if (parent->descriptor.type == TextureType::cube || parent->descriptor.type == TextureType::cube_array) {
        // A cube's slices are faces already.
    }
    result->create_shader_views(view_type(type), result->format, level_count, layers,
                                component_mapping(swizzle, info->swizzle));
    return result;
}

std::shared_ptr<Texture> Texture::create_buffer_texture(const std::shared_ptr<Buffer>& buffer,
    const TextureDescriptor& descriptor, VkDeviceSize offset, VkDeviceSize bytes_per_row) {
    if (descriptor.type != TextureType::texture_buffer)
        throw VulkanError("Alchemy Metal: linear textures over buffers are only translated for MTLTextureTypeTextureBuffer");
    const auto* info = pixel_format_info(descriptor.pixel_format);
    if (!info || info->compressed() || info->has_depth() || info->has_stencil())
        throw VulkanError("Alchemy Metal: unsupported texture buffer format " + std::to_string(descriptor.pixel_format));
    std::shared_ptr<Texture> result(new Texture(buffer->device));
    auto& d = *result->device;
    result->descriptor = descriptor;
    result->descriptor.height = result->descriptor.depth = result->descriptor.mipmap_levels = 1;
    result->info = info;
    result->format = info->vulkan;
    result->buffer = buffer;
    result->buffer_offset = offset;
    result->bytes_per_row = bytes_per_row;
    result->state = std::make_shared<ImageState>();
    const auto size = static_cast<VkDeviceSize>(descriptor.width) * info->block_bytes;
    if (offset > buffer->length || size > buffer->length - offset)
        throw VulkanError("Alchemy Metal: texture buffer exceeds its buffer");
    if (offset % std::max<std::uint32_t>(1, d.profile().min_texel_buffer_alignment))
        throw VulkanError("Alchemy Metal: texture buffer offset is not aligned to minimumTextureBufferAlignment");
    VkBufferViewCreateInfo view{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
    view.buffer = buffer->buffer;
    view.format = result->format;
    view.offset = offset;
    view.range = size;
    check(d.vk.vkCreateBufferView(d.device, &view, nullptr, &result->buffer_view), "vkCreateBufferView");
    return result;
}

std::shared_ptr<Texture> Texture::wrap(std::shared_ptr<Device> owner, VkImage image, const TextureDescriptor& descriptor,
                                       VkImageLayout current_layout,
                                       std::shared_ptr<std::atomic<VkImageLayout>> shared_layout,
                                       VkImageUsageFlags usage) {
    const auto* info = pixel_format_info(descriptor.pixel_format);
    if (!info) throw VulkanError("Alchemy Metal: unsupported MTLPixelFormat " + std::to_string(descriptor.pixel_format));
    std::shared_ptr<Texture> result(new Texture(std::move(owner)));
    result->descriptor = descriptor;
    result->info = info;
    result->format = info->vulkan;
    result->image = image;
    result->state = std::make_shared<ImageState>();
    if (shared_layout) result->state->layout = std::move(shared_layout);
    else *result->state->layout = current_layout;
    result->state->external = true;
    result->image_usage = usage;
    if (descriptor.usage & (texture_usage::shader_read | texture_usage::shader_write))
        result->create_shader_views(VK_IMAGE_VIEW_TYPE_2D, result->format, 1, 1, component_mapping(descriptor.swizzle, info->swizzle));
    return result;
}

Texture::~Texture() {
    Device* owner = device.get();
    std::vector<VkImageView> views;
    if (view) views.push_back(view);
    if (storage_view && storage_view != view) views.push_back(storage_view);
    for (const auto& [key, value] : attachment_views_) views.push_back(value);
    const auto handle = owns_image ? image : VK_NULL_HANDLE;
    auto allocation = memory;
    auto buffer_view_handle = buffer_view;
    if (handle) owner->cancel_initial_layout(handle);
    if (views.empty() && !handle && !buffer_view_handle) return;
    owner->defer([owner, views, handle, allocation, buffer_view_handle]() mutable {
        for (const auto value : views) owner->vk.vkDestroyImageView(owner->device, value, nullptr);
        if (buffer_view_handle) owner->vk.vkDestroyBufferView(owner->device, buffer_view_handle, nullptr);
        if (handle) owner->vk.vkDestroyImage(owner->device, handle, nullptr);
        owner->free(allocation);
    });
}

VkImageView Texture::attachment_view(std::uint32_t level, std::uint32_t layer, std::uint32_t layers) {
    std::lock_guard lock(views_mutex_);
    const auto key = std::make_tuple(level, layer, layers);
    if (const auto found = attachment_views_.find(key); found != attachment_views_.end()) return found->second;
    const bool depth_stencil = info->has_depth() || info->has_stencil();
    const VkImageUsageFlags needed = depth_stencil ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    // Every attachable format is created attachable (Texture::create), so
    // this only refuses formats Vulkan cannot render to.
    if (image_usage && !(image_usage & needed))
        throw VulkanError("Alchemy Metal: MTLPixelFormat" + std::string(info->name) + " texture" +
                          (label.empty() ? std::string() : " '" + label + "'") +
                          " is a render pass attachment but its format cannot be one on " + device->profile().name);
    if (image_usage && !(view_usage_for(device->format_features(format)) & needed))
        throw VulkanError("Alchemy Metal: MTLPixelFormat" + std::string(info->name) +
                          " view cannot be a render pass attachment on " + device->profile().name);
    // The view carries only the attachment usage (the image's storage or
    // sampled usage may not be valid for this view's format).
    VkImageViewUsageCreateInfo restricted{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    restricted.usage = needed;
    // Color attachment views double as framebuffer-fetch input attachments.
    if (!depth_stencil && (image_usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) restricted.usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    VkImageViewCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    if (image_usage) create.pNext = &restricted;
    create.image = image;
    create.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    if (descriptor.type == TextureType::type1d || descriptor.type == TextureType::type1d_array)
        create.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
    create.format = format;
    // Attachments use every aspect of the format.
    create.subresourceRange = {info->aspects(), base_level + level, 1, base_layer + layer, layers};
    if (info->stencil_view) create.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    VkImageView result{};
    check(device->vk.vkCreateImageView(device->device, &create, nullptr, &result), "vkCreateImageView");
    attachment_views_.emplace(key, result);
    return result;
}

VkBufferImageCopy Texture::copy_region(VkDeviceSize offset, std::uint64_t bytes_per_row, std::uint64_t bytes_per_image,
    const Region& region, std::uint32_t level, std::uint32_t slice, VkImageAspectFlags aspect) const {
    VkBufferImageCopy copy{};
    copy.bufferOffset = offset;
    const std::uint32_t block_bytes = (aspect == VK_IMAGE_ASPECT_STENCIL_BIT) ? 1U : info->block_bytes;
    if (bytes_per_row) copy.bufferRowLength = static_cast<std::uint32_t>(bytes_per_row / block_bytes * info->block_width);
    if (bytes_per_image && bytes_per_row)
        copy.bufferImageHeight = static_cast<std::uint32_t>(bytes_per_image / bytes_per_row * info->block_height);
    const bool volume = descriptor.type == TextureType::type3d;
    copy.imageSubresource = {aspect, base_level + level, base_layer + (volume ? 0 : slice), volume ? 1U : region.depth};
    if (!volume && region.depth > 1) copy.imageSubresource.layerCount = region.depth;
    copy.imageOffset = {static_cast<std::int32_t>(region.x), static_cast<std::int32_t>(region.y),
                        static_cast<std::int32_t>(volume ? region.z : 0)};
    copy.imageExtent = {region.width, region.height, volume ? region.depth : 1};
    if (!volume) copy.imageSubresource.layerCount = 1;
    return copy;
}

std::uint64_t Texture::copy_size(std::uint64_t bytes_per_row, std::uint64_t bytes_per_image, const Region& region) const {
    const auto rows = (region.height + info->block_height - 1) / info->block_height;
    const auto row = bytes_per_row ? bytes_per_row
                                   : static_cast<std::uint64_t>((region.width + info->block_width - 1) / info->block_width) *
                                         info->block_bytes;
    const auto image = bytes_per_image ? bytes_per_image : row * rows;
    return image * (region.depth - 1) + row * (rows - 1) +
           static_cast<std::uint64_t>((region.width + info->block_width - 1) / info->block_width) * info->block_bytes;
}

void Texture::replace_region(const Region& region, std::uint32_t level, std::uint32_t slice,
    std::span<const std::uint8_t> bytes, std::uint64_t bytes_per_row, std::uint64_t bytes_per_image) {
    if (is_buffer()) {
        // Linear texture over a buffer: the bytes are the buffer's.
        const auto row = static_cast<VkDeviceSize>(region.width) * info->block_bytes;
        buffer->write(buffer_offset + static_cast<VkDeviceSize>(region.x) * info->block_bytes,
                      bytes.first(static_cast<std::size_t>(std::min<VkDeviceSize>(row, bytes.size()))));
        return;
    }
    const auto size = copy_size(bytes_per_row, bytes_per_image, region);
    if (bytes.size() < size) throw std::out_of_range("Alchemy Metal: replaceRegion source is too small");
    auto staging = Buffer::create(device, copy_size(0, 0, region), {StorageMode::shared, 1});
    repack(bytes.data(), bytes_per_row, bytes_per_image, staging->contents(), 0, 0, region);
    const auto aspect = view_aspects();
    const auto copy = copy_region(0, 0, 0, region, level, slice, aspect);
    device->immediate([&](VkCommandBuffer command) {
        device->vk.vkCmdCopyBufferToImage(command, staging->buffer, image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    });
}

void Texture::get_bytes(std::span<std::uint8_t> bytes, std::uint64_t bytes_per_row, std::uint64_t bytes_per_image,
                        const Region& region, std::uint32_t level, std::uint32_t slice) {
    if (is_buffer()) {
        const auto row = static_cast<VkDeviceSize>(region.width) * info->block_bytes;
        buffer->read(buffer_offset + static_cast<VkDeviceSize>(region.x) * info->block_bytes,
                     bytes.first(static_cast<std::size_t>(std::min<VkDeviceSize>(row, bytes.size()))));
        return;
    }
    const auto size = copy_size(bytes_per_row, bytes_per_image, region);
    if (bytes.size() < size) throw std::out_of_range("Alchemy Metal: getBytes destination is too small");
    const auto tight = copy_size(0, 0, region);
    auto staging = Buffer::create(device, tight, {StorageMode::shared, 0});
    const auto aspect = view_aspects();
    const auto copy = copy_region(0, 0, 0, region, level, slice, aspect);
    device->immediate([&](VkCommandBuffer command) {
        device->vk.vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_GENERAL, staging->buffer, 1, &copy);
    });
    repack(staging->contents(), 0, 0, bytes.data(), bytes_per_row, bytes_per_image, region);
}

// The GPU copies use tightly packed rows; Metal's bytesPerRow/bytesPerImage
// pitches are applied on the CPU (some drivers mishandle row lengths that
// are not a multiple of their copy granularity).
void Texture::repack(const std::uint8_t* source, std::uint64_t source_row, std::uint64_t source_image,
                     std::uint8_t* destination, std::uint64_t destination_row, std::uint64_t destination_image,
                     const Region& region) const {
    const std::uint64_t row_bytes = static_cast<std::uint64_t>((region.width + info->block_width - 1) / info->block_width) *
                                    info->block_bytes;
    const std::uint64_t rows = (region.height + info->block_height - 1) / info->block_height;
    if (!source_row) source_row = row_bytes;
    if (!destination_row) destination_row = row_bytes;
    if (!source_image) source_image = source_row * rows;
    if (!destination_image) destination_image = destination_row * rows;
    for (std::uint32_t z = 0; z < region.depth; ++z)
        for (std::uint64_t y = 0; y < rows; ++y)
            std::memcpy(destination + z * destination_image + y * destination_row, source + z * source_image + y * source_row,
                        static_cast<std::size_t>(row_bytes));
}

// ---- samplers ---------------------------------------------------------------------

VkSamplerCreateInfo sampler_info(const Device& device, const SamplerDescriptor& d) {
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = d.mag_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter = d.min_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.mipmapMode = d.mip_filter == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    bool border{};
    const auto address = [&](std::uint32_t mode) {
        switch (mode) {
        case 0: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 1:
            if (!device.features12.samplerMirrorClampToEdge)
                throw VulkanError("Alchemy Metal: MTLSamplerAddressModeMirrorClampToEdge needs samplerMirrorClampToEdge");
            return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        case 2: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 3: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 4: case 5: border = true; return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: throw VulkanError("Alchemy Metal: invalid MTLSamplerAddressMode " + std::to_string(mode));
        }
    };
    info.addressModeU = address(d.address[0]);
    info.addressModeV = address(d.address[1]);
    info.addressModeW = address(d.address[2]);
    // ClampToZero samples transparent black; ClampToBorderColor the chosen color.
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (border) {
        const bool zero = d.address[0] == 4 || d.address[1] == 4 || d.address[2] == 4;
        if (!zero && d.border_color == 1) info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        if (!zero && d.border_color == 2) info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    }
    info.minLod = d.lod_min;
    info.maxLod = d.mip_filter == 0 ? std::min(0.25F, d.lod_max) : d.lod_max;
    if (d.mip_filter == 0) info.minLod = 0;
    if (d.max_anisotropy > 1 && device.features10.samplerAnisotropy) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = std::min(static_cast<float>(d.max_anisotropy), device.properties.limits.maxSamplerAnisotropy);
    }
    if (d.compare_function != 0) {
        info.compareEnable = VK_TRUE;
        info.compareOp = static_cast<VkCompareOp>(d.compare_function);   // MTLCompareFunction == VkCompareOp order
    }
    if (!d.normalized_coordinates) {
        info.unnormalizedCoordinates = VK_TRUE;
        info.minFilter = info.magFilter;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.minLod = info.maxLod = 0;
        info.anisotropyEnable = VK_FALSE;
        info.compareEnable = VK_FALSE;
        const auto clamp = [](VkSamplerAddressMode mode) {
            return mode == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ? mode : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        };
        info.addressModeU = clamp(info.addressModeU);
        info.addressModeV = clamp(info.addressModeV);
        info.addressModeW = clamp(info.addressModeW);
    }
    return info;
}

namespace {
std::mutex sampler_tokens_mutex;
std::unordered_map<std::uint64_t, std::weak_ptr<Sampler>> sampler_tokens;
} // namespace

std::uint64_t sampler_token(const std::shared_ptr<Sampler>& sampler) {
    if (!sampler) return 0;
    const auto token = reinterpret_cast<std::uint64_t>(sampler.get());
    std::lock_guard lock(sampler_tokens_mutex);
    sampler_tokens[token] = sampler;
    return token;
}

std::shared_ptr<Sampler> sampler_from_token(std::uint64_t token) {
    if (!token) return nullptr;
    std::lock_guard lock(sampler_tokens_mutex);
    const auto found = sampler_tokens.find(token);
    auto sampler = found == sampler_tokens.end() ? nullptr : found->second.lock();
    if (!sampler)
        throw std::runtime_error("Alchemy Metal: an argument buffer holds a sampler that was never encoded or is released");
    return sampler;
}

std::shared_ptr<Sampler> Sampler::create(std::shared_ptr<Device> device, const SamplerDescriptor& descriptor) {
    std::shared_ptr<Sampler> result(new Sampler(std::move(device)));
    result->descriptor = descriptor;
    const auto info = sampler_info(*result->device, descriptor);
    check(result->device->vk.vkCreateSampler(result->device->device, &info, nullptr, &result->sampler), "vkCreateSampler");
    return result;
}

std::shared_ptr<Sampler> Sampler::with_lod_clamp(const std::shared_ptr<Sampler>& sampler, float lod_min, float lod_max) {
    if (!sampler) return nullptr;
    // Vulkan requires minLod <= maxLod (Metal leaves an inverted range
    // undefined): an inverted pair clamps to the minimum. NaN becomes 0.
    if (!(lod_min >= 0)) lod_min = 0;
    if (!(lod_max >= lod_min)) lod_max = lod_min;
    if (lod_min == sampler->descriptor.lod_min && lod_max == sampler->descriptor.lod_max) return sampler;
    std::lock_guard lock(sampler->clamped_mutex_);
    // A client varying the range per draw must not run the device out of
    // samplers (maxSamplerAllocationCount): past the bound the cache starts
    // over. Bindings and command buffers own their samplers, so dropped
    // entries live as long as something uses them.
    if (sampler->clamped_.size() >= max_clamped_samplers && !sampler->clamped_.contains({lod_min, lod_max}))
        sampler->clamped_.clear();
    auto& derived = sampler->clamped_[{lod_min, lod_max}];
    if (!derived) {
        auto descriptor = sampler->descriptor;
        descriptor.lod_min = lod_min;
        descriptor.lod_max = lod_max;
        derived = create(sampler->device, descriptor);
    }
    return derived;
}

Sampler::~Sampler() {
    {
        std::lock_guard lock(sampler_tokens_mutex);
        const auto found = sampler_tokens.find(reinterpret_cast<std::uint64_t>(this));
        if (found != sampler_tokens.end() && found->second.expired()) sampler_tokens.erase(found);
    }
    if (!sampler) return;
    Device* owner = device.get();
    const auto handle = sampler;
    owner->defer([owner, handle] { owner->vk.vkDestroySampler(owner->device, handle, nullptr); });
}

bool Heap::reserve(std::uint64_t bytes) {
    auto current = used.load();
    do {
        if (current + bytes > size) return false;
    } while (!used.compare_exchange_weak(current, current + bytes));
    return true;
}

} // namespace alchemy::metal
