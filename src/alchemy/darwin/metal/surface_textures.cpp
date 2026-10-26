#include "alchemy/darwin/metal/bridge.hpp"

#include "alchemy/darwin/metal/formats.hpp"
#include "alchemy/darwin/metal/interop.hpp"
#include "alchemy/host/process.hpp"

// IOSurface-backed MTLTextures (owner: g3, Chrome stage 4):
// -[MTLDevice newTextureWithDescriptor:iosurface:plane:], -[MTLTexture
// iosurface] and -iosurfacePlane. Chrome's GPU process renders viz's output
// and ANGLE's IOSurface pbuffers (EGL_ANGLE_iosurface_client_buffer) into
// such textures and hands the surfaces to the browser's CALayer tree.
//
// Design. A Vulkan image cannot alias the surface's pages with the layout
// IOSurface defines (its bytesPerRow; Windows drivers offer no explicit
// linear layouts), so the texture keeps its own optimal VkImage and the pages
// are kept coherent with it at Metal's synchronisation points, as on macOS
// where the GPU's writes are in the surface once the command buffer completed:
//   - a command buffer using the texture first copies the pages into the
//     image when they may have changed since the image last matched them:
//     the seed moved (IOSurfaceUnlock after writing, or a completed GPU write,
//     in any process), another texture's GPU write was submitted, or an
//     event wait precedes the use (another process may have written them);
//   - one that may have written it (render target, blit destination, shader
//     write) copies the image back into the pages before each event signal
//     and after its last command; its completion ends the surface's GPU-write
//     ticket, which is what IOSurfaceLock and the compositor wait for
//     (quartzcore_vulkan.hpp).
// The copies go straight between the image and the pages when the device has
// VK_EXT_external_memory_host (the pages imported as a VkBuffer; guest VA ==
// host VA), else through a host-visible staging buffer and a memcpy at commit
// (upload) or completion (download); that fallback cannot see CPU writes made
// after the commit of a command buffer that waits for an event before using
// the texture. The pages stay mapped while GPU work on them may run
// (SurfacePages::keep), also after the IOSurface is freed.
//
// Supported: plane 0 of a chunky surface or any plane of a planar one (video
// frames: '420v' has an R8 luma and an RG8 chroma plane) whose elements are
// single texels, MTLTextureType2D, one level, one sample, one slice, a pixel
// format whose texel size is the plane's bytesPerElement, width and height
// within the plane. Anything else answers nil (Metal's failure value) and is
// reported once per reason.
#if __has_include("alchemy/darwin/quartzcore/quartzcore_vulkan.hpp")
#include "alchemy/darwin/quartzcore/quartzcore_vulkan.hpp"
#define ALCHEMY_METAL_SURFACE_TEXTURES 1
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <tuple>

namespace alchemy::metal::bridge {

#if defined(ALCHEMY_METAL_SURFACE_TEXTURES)

namespace {

namespace qc = gui::quartzcore;

void report_once(const std::string& reason) {
    static std::mutex mutex;
    static std::set<std::string> reported;
    {
        std::lock_guard lock(mutex);
        if (!reported.insert(reason).second) return;
    }
    std::fprintf(stderr, "[alchemy metal] newTextureWithDescriptor:iosurface:plane: answers nil: %s\n", reason.c_str());
}

// ---- the pages as a VkBuffer (VK_EXT_external_memory_host) ------------------------------

class PagesBuffer {
public:
    PagesBuffer(std::shared_ptr<Device> owner, qc::SurfacePages surface) : device(std::move(owner)), pages(std::move(surface)) {}
    ~PagesBuffer() {
        // After every submission that may still use the buffer; the pages
        // (pages.keep) are released after the memory.
        auto* owner = device.get();
        const auto handle = buffer;
        const auto allocation = memory;
        device->defer([owner, handle, allocation, keep = pages.keep] {
            if (handle) owner->vk.vkDestroyBuffer(owner->device, handle, nullptr);
            if (allocation) owner->vk.vkFreeMemory(owner->device, allocation, nullptr);
        });
    }
    PagesBuffer(const PagesBuffer&) = delete;
    PagesBuffer& operator=(const PagesBuffer&) = delete;

    const std::shared_ptr<Device> device;
    const qc::SurfacePages pages;
    VkBuffer buffer{};
    VkDeviceMemory memory{};
};

// The surface's pages imported into `device`, shared by every texture of the
// surface on that device; nullptr when the device cannot import them (the
// reason is reported once).
// ALCHEMY_METAL_IOSURFACE_IMPORT=0 stages every texture through host memory
// (diagnostics and the fallback's test).
bool import_disabled() {
    static const bool value = host::process::environment("ALCHEMY_METAL_IOSURFACE_IMPORT").value_or("") == "0";
    return value;
}

std::shared_ptr<PagesBuffer> import_pages(const std::shared_ptr<Device>& device, const qc::SurfacePages& pages) {
    if (import_disabled() || !device->has_device_extension(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) return nullptr;
    static std::mutex mutex;
    static std::map<std::tuple<const Device*, std::uint64_t, std::uint32_t>, std::weak_ptr<PagesBuffer>> cache;
    const auto key = std::make_tuple(device.get(), pages.base, pages.id);
    std::lock_guard lock(mutex);
    if (auto found = cache.find(key); found != cache.end()) {
        if (auto existing = found->second.lock()) return existing;
        cache.erase(found);
    }
    const auto failed = [](const std::string& why) -> std::shared_ptr<PagesBuffer> {
        static std::once_flag once;
        std::call_once(once, [&] {
            std::fprintf(stderr, "[alchemy metal] IOSurface pages not imported (%s); IOSurface textures stage through host memory\n",
                         why.c_str());
        });
        return nullptr;
    };
    auto* pointer = reinterpret_cast<void*>(pages.base);   // guest VA == host VA
    auto result = std::make_shared<PagesBuffer>(device, pages);
    VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    create.pNext = &external;
    create.size = pages.alloc_size;
    create.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (device->vk.vkCreateBuffer(device->device, &create, nullptr, &result->buffer) != VK_SUCCESS)
        return failed("vkCreateBuffer of an external host buffer failed");
    VkMemoryRequirements requirements{};
    device->vk.vkGetBufferMemoryRequirements(device->device, result->buffer, &requirements);
    std::string why;
    const auto imported = import_host_memory(*device, pointer, pages.alloc_size, requirements.memoryTypeBits, &why);
    if (!imported) return failed(why);
    result->memory = imported->memory;
    if (device->vk.vkBindBufferMemory(device->device, result->buffer, result->memory, 0) != VK_SUCCESS)
        return failed("vkBindBufferMemory of the imported pages failed");
    cache[key] = result;
    return result;
}

// ---- the texture's side -------------------------------------------------------------------

void memory_barrier(const Device& device, VkCommandBuffer command, VkPipelineStageFlags2 source, VkAccessFlags2 source_access,
                    VkPipelineStageFlags2 destination, VkAccessFlags2 destination_access) {
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = source;
    barrier.srcAccessMask = source_access;
    barrier.dstStageMask = destination;
    barrier.dstAccessMask = destination_access;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    device.vk.vkCmdPipelineBarrier2(command, &dependency);
}

class SurfaceTexture final : public SurfaceBacking, public std::enable_shared_from_this<SurfaceTexture> {
public:
    // The texture's rows are the plane's: `offset` bytes into the pages, `row_bytes` apart.
    SurfaceTexture(std::shared_ptr<Device> owner, qc::SurfacePages surface, std::uint64_t surface_plane, VkImage target,
                   std::uint32_t texel_bytes, std::uint32_t texture_width, std::uint32_t texture_height, std::uint64_t offset,
                   std::uint64_t row_bytes)
        : device(std::move(owner)), pages(std::move(surface)), plane_(surface_plane), image(target), block_bytes(texel_bytes),
          width(texture_width), height(texture_height), plane_offset(offset), plane_row_bytes(row_bytes),
          import(import_pages(device, pages)) {}

    [[nodiscard]] std::uint64_t writes_submitted() const override { return qc::gpu_writes_submitted(pages); }

    std::function<void()> record_head(VkCommandBuffer command, bool force, std::uint64_t writes_before) override {
        const auto seed = pages.seed();
        const auto writes = qc::gpu_writes_submitted(pages);
        if (!force && synced_.load() && synced_seed_.load() == seed && synced_writes_.load() == writes) return {};
        synced_seed_ = seed;
        synced_writes_ = writes;
        synced_ = true;
        uploads.fetch_add(1);
        memory_barrier(*device, command, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        std::function<void()> work;
        if (import) {
            const auto region = copy_region(plane_row_bytes, plane_offset);
            device->vk.vkCmdCopyBufferToImage(command, import->buffer, image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        } else {
            // The pages through host memory, as they are once the GPU writes
            // this use must see (another texture's, queued before it) ended:
            // the copy is made now, not in queue order.
            if (!qc::wait_for_gpu_writes(pages, writes_before, 2000))
                std::fputs("[alchemy metal] IOSurface texture: earlier GPU writes still pending after 2 s; uploading the pages as they are\n",
                           stderr);
            const auto row = static_cast<std::uint64_t>(width) * block_bytes;
            auto staging = Buffer::create(device, row * height, {StorageMode::shared, 1});
            for (std::uint32_t y = 0; y < height; ++y)
                std::memcpy(staging->contents() + y * row,
                            reinterpret_cast<const void*>(pages.base + plane_offset + y * plane_row_bytes), row);
            const auto region = copy_region(row, 0);
            device->vk.vkCmdCopyBufferToImage(command, staging->buffer, image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
            work = [staging] {};
        }
        memory_barrier(*device, command, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        return work;
    }

    std::function<void()> record_tail(VkCommandBuffer command) override {
        const auto ticket = qc::begin_gpu_write(pages);
        // Our own write: the image holds it (unless another texture's write
        // came in between; then the next use uploads).
        auto expected = ticket - 1;
        synced_writes_.compare_exchange_strong(expected, ticket);
        downloads.fetch_add(1);
        memory_barrier(*device, command, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        std::shared_ptr<Buffer> staging;
        const auto row = static_cast<std::uint64_t>(width) * block_bytes;
        if (import) {
            const auto region = copy_region(plane_row_bytes, plane_offset);
            device->vk.vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_GENERAL, import->buffer, 1, &region);
        } else {
            staging = Buffer::create(device, row * height, {StorageMode::shared, 1});
            const auto region = copy_region(row, 0);
            device->vk.vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_GENERAL, staging->buffer, 1, &region);
        }
        // The caller's host barrier (ALL_COMMANDS -> HOST) follows.
        return [self = shared_from_this(), ticket, staging, row] {
            if (staging)
                for (std::uint32_t y = 0; y < self->height; ++y)
                    std::memcpy(reinterpret_cast<void*>(self->pages.base + self->plane_offset + y * self->plane_row_bytes),
                                staging->contents() + y * row, row);
            // The image equals the pages again unless the CPU wrote them
            // meanwhile (the seed then moved twice: the next use uploads).
            const auto before = self->pages.seed();
            const auto after = qc::end_gpu_write(self->pages, ticket);
            auto expected = before;
            if (after == before + 1) self->synced_seed_.compare_exchange_strong(expected, after);
        };
    }

    [[nodiscard]] std::uint64_t surface_object() const noexcept override { return pages.surface; }
    [[nodiscard]] std::uint64_t plane() const noexcept override { return plane_; }

    const std::shared_ptr<Device> device;
    const qc::SurfacePages pages;
    const std::uint64_t plane_;
    const VkImage image;
    const std::uint32_t block_bytes, width, height;
    const std::uint64_t plane_offset, plane_row_bytes;
    const std::shared_ptr<PagesBuffer> import;
    std::atomic<std::uint64_t> uploads{0}, downloads{0};

private:
    [[nodiscard]] VkBufferImageCopy copy_region(std::uint64_t row_bytes, std::uint64_t offset) const {
        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.bufferRowLength = static_cast<std::uint32_t>(row_bytes / block_bytes);
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        return region;
    }
    std::atomic<bool> synced_{false};
    std::atomic<std::uint32_t> synced_seed_{0};
    std::atomic<std::uint64_t> synced_writes_{0};
};

Address new_surface_texture(Address device_object, Address descriptor, Address surface, std::uint64_t plane) {
    auto& r = runtime();
    if (!descriptor) { report_once("nil descriptor"); return 0; }
    const auto pages = qc::surface_pages(surface);
    if (!pages) { report_once("the argument is not an IOSurfaceRef of this provider"); return 0; }
    // The plane's geometry: the whole surface when it is chunky.
    qc::SurfacePlane geometry{pages->width, pages->height, pages->bytes_per_element, 1, 1, 0, pages->bytes_per_row};
    if (pages->planes.empty()) {
        if (plane != 0) { report_once("plane " + std::to_string(plane) + " of a chunky surface"); return 0; }
    } else {
        if (plane >= pages->planes.size()) {
            report_once("plane " + std::to_string(plane) + " of a surface with " + std::to_string(pages->planes.size()) + " planes");
            return 0;
        }
        geometry = pages->planes[plane];
        if (geometry.element_width != 1 || geometry.element_height != 1) {
            report_once("plane " + std::to_string(plane) + " has " + std::to_string(geometry.element_width) + "x" +
                        std::to_string(geometry.element_height) + "-pixel elements");
            return 0;
        }
    }
    const auto type = descriptor_u64(descriptor, "textureType");
    const auto levels = descriptor_u64(descriptor, "mipmapLevelCount");
    const auto samples = descriptor_u64(descriptor, "sampleCount");
    const auto slices = descriptor_u64(descriptor, "arrayLength");
    const auto depth = descriptor_u64(descriptor, "depth");
    if (type != static_cast<std::uint64_t>(TextureType::type2d) || levels > 1 || samples > 1 || slices > 1 || depth > 1) {
        report_once("only 2D single-level, single-sample textures are backed by IOSurfaces (textureType " + std::to_string(type) + ")");
        return 0;
    }
    const auto format = static_cast<std::uint32_t>(descriptor_u64(descriptor, "pixelFormat"));
    const auto* info = pixel_format_info(format);
    if (!info || info->compressed() || info->has_depth() || info->has_stencil()) {
        report_once("MTLPixelFormat " + std::to_string(format) + " is not a color format an IOSurface can back");
        return 0;
    }
    if (info->block_bytes != geometry.bytes_per_element || geometry.bytes_per_row % info->block_bytes ||
        geometry.offset % info->block_bytes) {
        report_once("MTLPixelFormat" + std::string(info->name) + " has " + std::to_string(info->block_bytes) +
                    " bytes per texel; the surface's plane " + std::to_string(plane) + " has " +
                    std::to_string(geometry.bytes_per_element) + " per element");
        return 0;
    }
    const auto width = static_cast<std::uint32_t>(std::max<std::uint64_t>(1, descriptor_u64(descriptor, "width")));
    const auto height = static_cast<std::uint32_t>(std::max<std::uint64_t>(1, descriptor_u64(descriptor, "height")));
    if (width > geometry.width || height > geometry.height) {
        report_once("the texture (" + std::to_string(width) + "x" + std::to_string(height) + ") is larger than the surface's plane " +
                    std::to_string(plane) + " (" + std::to_string(geometry.width) + "x" + std::to_string(geometry.height) + ")");
        return 0;
    }
    // The texture itself is an ordinary one of the device (descriptor,
    // usage, storage mode); the backing keeps it coherent with the pages.
    CallRegs arguments;
    arguments.x[2] = descriptor;
    const auto object = r.objc.send(device_object, "newTextureWithDescriptor:", arguments).x0;
    const auto texture = texture_of(object);
    if (!texture) { r.release(object); return 0; }
    texture->surface = std::make_shared<SurfaceTexture>(texture->device, *pages, plane, texture->image, info->block_bytes, width, height,
                                                        geometry.offset, geometry.bytes_per_row);
    // The texture retains its IOSurface (released in -dealloc).
    if (const auto held = r.objc.find_host_state<Held>(object)) held->keep(r.retain(surface));
    return object;
}

} // namespace

void install_surface_textures(Runtime& r) {
    const auto device = r.objc.find_class("AlchemyMTLDevice");
    const auto texture = r.objc.find_class("AlchemyMTLTexture");
    if (!device || !texture) throw std::runtime_error("Alchemy Metal: install_surface_textures before the device and texture classes");
    r.method(device, "newTextureWithDescriptor:iosurface:plane:", [](CpuState& s, GuestMemory&) {
        s.x[0] = new_surface_texture(s.x[0], s.x[2], s.x[3], s.x[4]);
    }, false, "@@:@^{__IOSurface=}Q");
    // The surface of the texture or of the texture it is a view of (not
    // retained: the texture holds it).
    r.method(texture, "iosurface", [](CpuState& s, GuestMemory&) {
        const auto value = texture_of(s.x[0]);
        const std::shared_ptr<SurfaceBacking> backing = value ? value->surface_backing() : nullptr;
        s.x[0] = backing ? backing->surface_object() : 0;
    }, false, "^{__IOSurface=}@:");
    r.method(texture, "iosurfacePlane", [](CpuState& s, GuestMemory&) {
        const auto value = texture_of(s.x[0]);
        const std::shared_ptr<SurfaceBacking> backing = value ? value->surface_backing() : nullptr;
        s.x[0] = backing ? backing->plane() : 0;
    }, false, "Q@:");
}

std::optional<SurfaceTextureStats> surface_texture_stats(std::uint64_t texture_object) {
    const auto texture = texture_of(texture_object);
    const auto backing = texture ? std::dynamic_pointer_cast<SurfaceTexture>(texture->surface_backing()) : nullptr;
    if (!backing) return std::nullopt;
    return SurfaceTextureStats{backing->uploads.load(), backing->downloads.load(), backing->import != nullptr};
}

#else

void install_surface_textures(Runtime& r) {
    // Without QuartzCore's IOSurface provider there are no surfaces to back
    // textures: creation answers nil and no texture has a surface.
    const auto device = r.objc.find_class("AlchemyMTLDevice");
    const auto texture = r.objc.find_class("AlchemyMTLTexture");
    if (!device || !texture) return;
    r.method(device, "newTextureWithDescriptor:iosurface:plane:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false,
             "@@:@^{__IOSurface=}Q");
    r.method(texture, "iosurface", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "^{__IOSurface=}@:");
    r.method(texture, "iosurfacePlane", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "Q@:");
}

std::optional<SurfaceTextureStats> surface_texture_stats(std::uint64_t) { return std::nullopt; }

#endif

} // namespace alchemy::metal::bridge
