#include "alchemy/darwin/metal/commands.hpp"

#include "alchemy/perf.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <iostream>
#include <stdexcept>

// Metal command buffers and encoders on Vulkan. See commands.hpp.
namespace alchemy::metal {
namespace {

constexpr VkDeviceSize chunk_size = 1ULL << 20;
constexpr VkDeviceSize bytes_alignment = 64;

// Upload memory recycled between command buffers of one device.
struct UploadPool {
    Device* device{};
    std::mutex mutex;
    struct Entry { VkBuffer buffer{}; Allocation memory; VkDeviceAddress address{}; VkDeviceSize size{}; };
    std::vector<Entry> free;
    Entry acquire(VkDeviceSize minimum) {
        {
            std::lock_guard lock(mutex);
            for (auto it = free.begin(); it != free.end(); ++it)
                if (it->size >= minimum) { auto entry = *it; free.erase(it); return entry; }
        }
        Entry entry;
        entry.size = std::max(chunk_size, (minimum + chunk_size - 1) / chunk_size * chunk_size);
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = entry.size;
        info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        check(device->vk.vkCreateBuffer(device->device, &info, nullptr, &entry.buffer), "vkCreateBuffer");
        VkMemoryRequirements requirements{};
        device->vk.vkGetBufferMemoryRequirements(device->device, entry.buffer, &requirements);
        entry.memory = device->allocate(requirements, MemoryKind::host_visible, false);
        check(device->vk.vkBindBufferMemory(device->device, entry.buffer, entry.memory.memory, entry.memory.offset),
              "vkBindBufferMemory");
        VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        address.buffer = entry.buffer;
        entry.address = device->vk.vkGetBufferDeviceAddress(device->device, &address);
        return entry;
    }
    void release(Entry entry) {
        std::lock_guard lock(mutex);
        // Keep a bounded pool; give the rest back.
        if (free.size() < 64) { free.push_back(entry); return; }
        device->vk.vkDestroyBuffer(device->device, entry.buffer, nullptr);
        device->free(entry.memory);
    }
    void cleanup() {
        for (auto& entry : free) {
            device->vk.vkDestroyBuffer(device->device, entry.buffer, nullptr);
            device->free(entry.memory);
        }
        free.clear();
    }
};

UploadPool& upload_pool(Device& device) {
    return device.extension<UploadPool>("metal.upload", [&] {
        auto pool = std::make_shared<UploadPool>();
        pool->device = &device;
        return pool;
    });
}

// Descriptors written for bindings the guest left empty (Metal leaves them
// undefined; Vulkan requires valid descriptors): a 1x1 transparent black
// texture of every view type, a nearest sampler and a texel buffer.
struct Fallbacks {
    Device* device{};
    std::mutex mutex;
    bool created{};
    VkImage image_1d{}, image_2d{}, image_3d{};
    Allocation memory_1d, memory_2d, memory_3d;
    std::map<VkImageViewType, VkImageView> views;
    VkSampler sampler{};
    VkBuffer buffer{};
    Allocation buffer_memory;
    VkBufferView buffer_view{};
    void create() {
        auto& d = *device;
        const auto make_image = [&](VkImageType type, std::uint32_t layers, VkImageCreateFlags flags, VkImage& image,
                                    Allocation& memory) {
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.flags = flags;
            info.imageType = type;
            info.format = VK_FORMAT_R8G8B8A8_UNORM;
            info.extent = {1, 1, 1};
            info.mipLevels = 1;
            info.arrayLayers = layers;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            check(d.vk.vkCreateImage(d.device, &info, nullptr, &image), "vkCreateImage");
            VkMemoryRequirements requirements{};
            d.vk.vkGetImageMemoryRequirements(d.device, image, &requirements);
            memory = d.allocate(requirements, MemoryKind::device_local, true);
            check(d.vk.vkBindImageMemory(d.device, image, memory.memory, memory.offset), "vkBindImageMemory");
            d.queue_initial_layout(image, VK_IMAGE_ASPECT_COLOR_BIT);
        };
        make_image(VK_IMAGE_TYPE_1D, 1, 0, image_1d, memory_1d);
        make_image(VK_IMAGE_TYPE_2D, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, image_2d, memory_2d);
        make_image(VK_IMAGE_TYPE_3D, 1, 0, image_3d, memory_3d);
        const auto make_view = [&](VkImageViewType type, VkImage image, std::uint32_t layers) {
            VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            info.image = image;
            info.viewType = type;
            info.format = VK_FORMAT_R8G8B8A8_UNORM;
            info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
            VkImageView view{};
            check(d.vk.vkCreateImageView(d.device, &info, nullptr, &view), "vkCreateImageView");
            views[type] = view;
        };
        make_view(VK_IMAGE_VIEW_TYPE_1D, image_1d, 1);
        make_view(VK_IMAGE_VIEW_TYPE_1D_ARRAY, image_1d, 1);
        make_view(VK_IMAGE_VIEW_TYPE_2D, image_2d, 1);
        make_view(VK_IMAGE_VIEW_TYPE_2D_ARRAY, image_2d, 1);
        make_view(VK_IMAGE_VIEW_TYPE_CUBE, image_2d, 6);
        if (d.features10.imageCubeArray) make_view(VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, image_2d, 6);
        make_view(VK_IMAGE_VIEW_TYPE_3D, image_3d, 1);
        VkSamplerCreateInfo sampler_create{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        check(d.vk.vkCreateSampler(d.device, &sampler_create, nullptr, &sampler), "vkCreateSampler");
        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = 256;
        buffer_info.usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
        check(d.vk.vkCreateBuffer(d.device, &buffer_info, nullptr, &buffer), "vkCreateBuffer");
        VkMemoryRequirements requirements{};
        d.vk.vkGetBufferMemoryRequirements(d.device, buffer, &requirements);
        buffer_memory = d.allocate(requirements, MemoryKind::device_local, false);
        check(d.vk.vkBindBufferMemory(d.device, buffer, buffer_memory.memory, buffer_memory.offset), "vkBindBufferMemory");
        VkBufferViewCreateInfo view_info{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
        view_info.buffer = buffer;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.range = VK_WHOLE_SIZE;
        check(d.vk.vkCreateBufferView(d.device, &view_info, nullptr, &buffer_view), "vkCreateBufferView");
        created = true;
    }
    VkImageView view(VkImageViewType type) {
        std::lock_guard lock(mutex);
        if (!created) create();
        const auto found = views.find(type);
        return found == views.end() ? views.at(VK_IMAGE_VIEW_TYPE_2D) : found->second;
    }
    // The fallback images are storage-capable RGBA8 with identity views.
    VkImageView storage_view(VkImageViewType type) { return view(type); }
    VkSampler default_sampler() { std::lock_guard lock(mutex); if (!created) create(); return sampler; }
    VkBufferView texel_buffer() { std::lock_guard lock(mutex); if (!created) create(); return buffer_view; }
    void cleanup() {
        if (!created) return;
        auto& d = *device;
        for (const auto& [type, view] : views) d.vk.vkDestroyImageView(d.device, view, nullptr);
        d.vk.vkDestroySampler(d.device, sampler, nullptr);
        d.vk.vkDestroyBufferView(d.device, buffer_view, nullptr);
        d.vk.vkDestroyBuffer(d.device, buffer, nullptr);
        d.free(buffer_memory);
        for (auto [image, memory] : {std::pair{image_1d, &memory_1d}, std::pair{image_2d, &memory_2d}, std::pair{image_3d, &memory_3d}}) {
            d.cancel_initial_layout(image);
            d.vk.vkDestroyImage(d.device, image, nullptr);
            d.free(*memory);
        }
        created = false;
    }
};

Fallbacks& fallbacks(Device& device) {
    return device.extension<Fallbacks>("metal.fallbacks", [&] {
        auto value = std::make_shared<Fallbacks>();
        value->device = &device;
        return value;
    });
}

bool trace_indirect() {
    static const bool enabled = [] {
#ifdef _MSC_VER
        char* buffer = nullptr;
        std::size_t length = 0;
        bool value = false;
        if (_dupenv_s(&buffer, &length, "ALCHEMY_METAL_TRACE_INDIRECT") == 0 && buffer) {
            value = buffer[0] && std::strcmp(buffer, "0") != 0;
            std::free(buffer);
        }
        return value;
#else
        const char* value = std::getenv("ALCHEMY_METAL_TRACE_INDIRECT");
        return value && value[0] && std::strcmp(value, "0") != 0;
#endif
    }();
    return enabled;
}

// ALCHEMY_METAL_TRACE_PASSES=1: one line per render pass (attachments) and
// per draw (pipeline, fragment function, depth/stencil state) on stderr.
bool trace_passes() {
    static const bool enabled = [] {
#ifdef _MSC_VER
        char* buffer = nullptr;
        std::size_t length = 0;
        bool value = false;
        if (_dupenv_s(&buffer, &length, "ALCHEMY_METAL_TRACE_PASSES") == 0 && buffer) {
            value = buffer[0] && std::strcmp(buffer, "0") != 0;
            std::free(buffer);
        }
        return value;
#else
        const char* value = std::getenv("ALCHEMY_METAL_TRACE_PASSES");
        return value && value[0] && std::strcmp(value, "0") != 0;
#endif
    }();
    return enabled;
}

std::string pipeline_name(const RenderPipeline& pipeline) {
    if (!pipeline.descriptor.label.empty()) return pipeline.descriptor.label;
    return pipeline.descriptor.vertex ? pipeline.descriptor.vertex->name() : std::string("render pipeline");
}

VkImageViewType fallback_view_type(msl::TextureType type) {
    switch (type) {
    case msl::TextureType::texture1d: return VK_IMAGE_VIEW_TYPE_1D;
    case msl::TextureType::texture1d_array: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case msl::TextureType::texture2d_array: case msl::TextureType::depth2d_array: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case msl::TextureType::texture3d: return VK_IMAGE_VIEW_TYPE_3D;
    case msl::TextureType::texture_cube: case msl::TextureType::depth_cube: return VK_IMAGE_VIEW_TYPE_CUBE;
    case msl::TextureType::texture_cube_array: case msl::TextureType::depth_cube_array: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}

VkAttachmentLoadOp load_op(std::uint32_t action) {
    switch (action) {
    case 0: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    case 1: return VK_ATTACHMENT_LOAD_OP_LOAD;
    case 2: return VK_ATTACHMENT_LOAD_OP_CLEAR;
    default: throw std::runtime_error("invalid MTLLoadAction " + std::to_string(action));
    }
}

VkAttachmentStoreOp store_op(std::uint32_t action) {
    // DontCare, Store, MultisampleResolve, StoreAndMultisampleResolve,
    // Unknown, CustomSampleDepthStore.
    return action == 0 || action == 2 ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
}

bool resolves(std::uint32_t action) { return action == 2 || action == 3; }

std::uint32_t div_up(std::uint32_t value, std::uint32_t divisor) { return divisor ? (value + divisor - 1) / divisor : 0; }

} // namespace

// ---- events ---------------------------------------------------------------------------

std::shared_ptr<Event> Event::create(std::shared_ptr<Device> device, bool shared) {
    std::shared_ptr<Event> result(new Event(std::move(device), shared));
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    info.pNext = &type;
    check(result->device->vk.vkCreateSemaphore(result->device->device, &info, nullptr, &result->semaphore), "vkCreateSemaphore");
    return result;
}

Event::~Event() {
    if (!semaphore) return;
    Device* owner = device.get();
    const auto handle = semaphore;
    owner->defer([owner, handle] { owner->vk.vkDestroySemaphore(owner->device, handle, nullptr); });
}

std::uint64_t Event::signaled_value() const {
    std::uint64_t value{};
    check(device->vk.vkGetSemaphoreCounterValue(device->device, semaphore, &value), "vkGetSemaphoreCounterValue");
    return value;
}

void Event::signal(std::uint64_t value) {
    if (value <= signaled_value()) return;
    VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    info.semaphore = semaphore;
    info.value = value;
    check(device->vk.vkSignalSemaphore(device->device, &info), "vkSignalSemaphore");
}

bool Event::wait(std::uint64_t value, std::uint64_t timeout_ns) const {
    VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    info.semaphoreCount = 1;
    info.pSemaphores = &semaphore;
    info.pValues = &value;
    const auto result = device->vk.vkWaitSemaphores(device->device, &info, timeout_ns);
    if (result == VK_TIMEOUT) return false;
    check(result, "vkWaitSemaphores");
    return true;
}

// ---- counter sample buffers ------------------------------------------------------------

std::shared_ptr<CounterSampleBuffer> CounterSampleBuffer::create(std::shared_ptr<Device> device, std::uint32_t count) {
    if (!device->profile().timestamps) throw std::runtime_error("Metal counter sample buffer on a device without timestamps");
    std::shared_ptr<CounterSampleBuffer> result(new CounterSampleBuffer(std::move(device), count));
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = count;
    const auto& vk = result->device->vk;
    check(vk.vkCreateQueryPool(result->device->device, &info, nullptr, &result->pool), "vkCreateQueryPool");
    vk.vkResetQueryPool(result->device->device, result->pool, 0, count);   // every sample starts unwritten
    return result;
}

CounterSampleBuffer::~CounterSampleBuffer() {
    if (!pool) return;
    Device* owner = device.get();
    const auto handle = pool;
    owner->defer([owner, handle] { owner->vk.vkDestroyQueryPool(owner->device, handle, nullptr); });
}

std::vector<std::uint64_t> CounterSampleBuffer::resolve(std::uint32_t first, std::uint32_t length) const {
    if (first > count || length > count - first) throw std::runtime_error("Metal resolveCounterRange: outside the sample buffer");
    std::vector<std::uint64_t> pairs(2 * static_cast<std::size_t>(length));   // value, availability
    std::vector<std::uint64_t> values(length, error_value);
    if (!length) return values;
    const auto result = device->vk.vkGetQueryPoolResults(device->device, pool, first, length, pairs.size() * 8, pairs.data(), 16,
                                                         VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (result != VK_NOT_READY) check(result, "vkGetQueryPoolResults");
    const auto bits = device->timestamp_valid_bits;
    const auto mask = bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
    for (std::uint32_t index = 0; index < length; ++index)
        if (pairs[2 * index + 1]) values[index] = pairs[2 * index] & mask;
    return values;
}

void CounterSampleBuffer::mark_recorded(std::uint32_t index) {
    std::lock_guard lock(mutex_);
    recorded_.at(index) = true;
}

std::vector<bool> CounterSampleBuffer::recorded(std::uint32_t first, std::uint32_t length) const {
    std::lock_guard lock(mutex_);
    return {recorded_.begin() + first, recorded_.begin() + first + length};
}

void CommandBuffer::write_samples(const std::vector<CounterSample>& samples, VkPipelineStageFlags2 stage) {
    for (const auto& sample : samples) {
        if (!sample.buffer || sample.index >= sample.buffer->count) throw std::runtime_error("Metal sample index outside its counter sample buffer");
        retain(sample.buffer);
        sample.buffer->mark_recorded(sample.index);
        device->vk.vkCmdResetQueryPool(current_, sample.buffer->pool, sample.index, 1);
        device->vk.vkCmdWriteTimestamp2(current_, stage, sample.buffer->pool, sample.index);
    }
}

// ---- command buffer lifecycle -----------------------------------------------------------

CommandBuffer::CommandBuffer(std::shared_ptr<Device> owner, bool retained)
    : device(std::move(owner)), retained_references(retained) {}

std::shared_ptr<CommandBuffer> CommandBuffer::create(std::shared_ptr<Device> device, bool retained_references) {
    std::shared_ptr<CommandBuffer> result(new CommandBuffer(std::move(device), retained_references));
    perf::add(perf::mtl_command_buffers);
    result->commands_ = result->device->acquire_commands();
    result->batches_.emplace_back();
    return result;
}

CommandBuffer::~CommandBuffer() {
    if (value_ && !finalized_) {
        // Dropped while the GPU may still run it: the resources (and the
        // completion work, such as visibility results) move to a deferred step.
        struct Retired {
            CommandContext commands;
            std::vector<VkDescriptorPool> pools;
            std::vector<Chunk> chunks;
            std::vector<std::function<void()>> completions;
            std::vector<std::shared_ptr<void>> retained;
        };
        auto retired = std::make_shared<Retired>(Retired{std::move(commands_), std::move(descriptor_pools_),
            std::move(chunks_), std::move(completions_), std::move(retained_)});
        Device* owner = device.get();
        // Keep the device's other objects alive through the retained list.
        owner->defer([owner, retired] {
            for (auto& work : retired->completions) work();
            owner->recycle_commands(std::move(retired->commands));
            for (const auto pool : retired->pools) owner->recycle_descriptor_pool(pool);
            auto& uploads = upload_pool(*owner);
            for (auto& chunk : retired->chunks) uploads.release({chunk.buffer, chunk.memory, chunk.address, chunk.size});
            retired->retained.clear();
        });
        return;
    }
    // Never committed: give the recycled state back (nothing was submitted).
    if (!value_) {
        try {
            if (current_) device->vk.vkEndCommandBuffer(current_);
            device->recycle_commands(std::move(commands_));
            for (const auto pool : descriptor_pools_) device->recycle_descriptor_pool(pool);
            auto& pool = upload_pool(*device);
            for (auto& chunk : chunks_) pool.release({chunk.buffer, chunk.memory, chunk.address, chunk.size});
            if (queries_) device->vk.vkDestroyQueryPool(device->device, queries_, nullptr);
        } catch (const std::exception&) {
        }
    }
}

VkCommandBuffer CommandBuffer::recording() {
    if (current_) return current_;
    if (value_) throw std::runtime_error("Metal command buffer was already committed");
    if (commands_.used == commands_.buffers.size()) {
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = commands_.pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkCommandBuffer buffer{};
        check(device->vk.vkAllocateCommandBuffers(device->device, &allocate, &buffer), "vkAllocateCommandBuffers");
        commands_.buffers.push_back(buffer);
    }
    current_ = commands_.buffers[commands_.used++];
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(device->vk.vkBeginCommandBuffer(current_, &begin), "vkBeginCommandBuffer");
    full_barrier();
    return current_;
}

void CommandBuffer::close_recording() {
    if (!current_) return;
    check(device->vk.vkEndCommandBuffer(current_), "vkEndCommandBuffer");
    batches_.back().command_buffers.push_back(current_);
    current_ = VK_NULL_HANDLE;
}

void CommandBuffer::barrier(VkPipelineStageFlags2 source, VkAccessFlags2 source_access, VkPipelineStageFlags2 destination,
                            VkAccessFlags2 destination_access) {
    VkMemoryBarrier2 value{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    value.srcStageMask = source;
    value.srcAccessMask = source_access;
    value.dstStageMask = destination;
    value.dstAccessMask = destination_access;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &value;
    device->vk.vkCmdPipelineBarrier2(current_, &dependency);
    perf::add(perf::mtl_vk_barriers);
}

void CommandBuffer::full_barrier() {
    barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}

namespace {

// From the owner's layout to GENERAL. The tracked layout keeps the contents
// (UNDEFINED, as after QuartzCore hands out a fresh image, discards them: a
// drawable's initial contents are undefined). The first scope covers every
// earlier submission, whose writes are made available before the transition.
VkImageMemoryBarrier2 to_general(const Texture& texture, VkImageLayout layout) {
    VkImageMemoryBarrier2 value{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    value.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    value.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    value.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    value.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    value.oldLayout = layout;
    value.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    value.srcQueueFamilyIndex = value.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    value.image = texture.image;
    value.subresourceRange = {texture.aspects(), 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    return value;
}

} // namespace

bool CommandBuffer::claim_external(const Texture& texture, VkImageLayout& layout) {
    if (std::find(external_images_.begin(), external_images_.end(), texture.image) != external_images_.end()) return false;
    external_images_.push_back(texture.image);
    // Metal publishes GENERAL for the owner's next use (quartzcore_vulkan.hpp).
    layout = texture.state->layout->exchange(VK_IMAGE_LAYOUT_GENERAL);
    return layout != VK_IMAGE_LAYOUT_GENERAL;
}

void CommandBuffer::note_surface(const Texture& texture, bool write) {
    const auto& backing = texture.surface_backing();
    if (!backing) return;
    const auto batch = batches_.size() - 1;
    if (std::none_of(surface_uses_.begin(), surface_uses_.end(),
                     [&](const SurfaceUse& use) { return use.backing == backing && use.batch == batch; }))
        surface_uses_.push_back({backing, batch, backing->writes_submitted()});
    if (write && std::find(surface_writes_.begin(), surface_writes_.end(), backing) == surface_writes_.end())
        surface_writes_.push_back(backing);
}

void CommandBuffer::flush_surface_tails(VkCommandBuffer command) {
    for (const auto& backing : surface_writes_)
        if (auto work = backing->record_tail(command)) on_complete(std::move(work));
    surface_writes_.clear();
}

void CommandBuffer::prepare_texture(const std::shared_ptr<Texture>& texture, bool write) {
    if (!texture) return;
    retain(texture);
    note_surface(*texture, write);
    if (!texture->state->external) return;
    VkImageLayout layout{};
    if (!claim_external(*texture, layout)) return;
    const auto value = to_general(*texture, layout);
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &value;
    device->vk.vkCmdPipelineBarrier2(recording(), &dependency);
}

void CommandBuffer::prepare_in_pass(const std::shared_ptr<Texture>& texture, bool write) {
    if (!texture) return;
    retain(texture);
    note_surface(*texture, write);
    if (!texture->state->external) return;
    // Not used earlier in this command buffer (else it was claimed), so the
    // owner's layout holds from the start of the command buffer up to here.
    VkImageLayout layout{};
    if (claim_external(*texture, layout)) head_barriers_.push_back(to_general(*texture, layout));
}

void CommandBuffer::retain(std::shared_ptr<void> object) {
    if (!object) return;
    if (!retained_.empty() && retained_.back() == object) return;
    retained_.push_back(std::move(object));
}

void CommandBuffer::on_complete(std::function<void()> work) { completions_.push_back(std::move(work)); }

CommandBuffer::Upload CommandBuffer::upload(VkDeviceSize size, VkDeviceSize alignment) {
    alignment = std::max<VkDeviceSize>(alignment, 16);
    auto offset = (chunk_offset_ + alignment - 1) / alignment * alignment;
    if (chunks_.empty() || offset + size > chunks_.back().size) {
        const auto entry = upload_pool(*device).acquire(size);
        chunks_.push_back({entry.buffer, entry.memory, entry.address, entry.size});
        offset = 0;
    }
    chunk_offset_ = offset + std::max<VkDeviceSize>(size, 1);
    perf::add(perf::mtl_upload_bytes, size);
    const auto& chunk = chunks_.back();
    return {chunk.buffer, chunk.address + offset, offset, chunk.memory.mapped + offset};
}

VkDescriptorSet CommandBuffer::allocate_set(VkDescriptorSetLayout layout) {
    perf::add(perf::mtl_descriptor_sets);
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (descriptor_pools_.empty() || attempt == 1) {
            descriptor_pools_.push_back(device->acquire_descriptor_pool());
            perf::add(perf::mtl_descriptor_pools);
        }
        VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        info.descriptorPool = descriptor_pools_.back();
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;
        VkDescriptorSet set{};
        const auto result = device->vk.vkAllocateDescriptorSets(device->device, &info, &set);
        if (result == VK_SUCCESS) return set;
        if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL) check(result, "vkAllocateDescriptorSets");
    }
    throw VulkanError("Alchemy Metal: a descriptor set does not fit a fresh descriptor pool");
}

VkDescriptorSet CommandBuffer::write_set(StageState& state, const StageLayout& layout, const msl::Reflection* reflection) {
    if (!reflection || layout.bindings.empty()) return VK_NULL_HANDLE;
    perf::Scope timed(perf::mtl_write_set_ticks);
    const auto set = allocate_set(layout.set_layout);
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> buffers;
    std::vector<VkDescriptorImageInfo> images;
    std::vector<VkBufferView> texel_buffers;
    std::size_t image_count = 0, texel_count = 0;
    for (const auto& texture : reflection->textures) {
        const auto count = std::max(1U, texture.descriptor.count);
        if (texture.descriptor.type == msl::DescriptorType::uniform_texel_buffer ||
            texture.descriptor.type == msl::DescriptorType::storage_texel_buffer)
            texel_count += count;
        else image_count += count;
    }
    for (const auto& sampler : reflection->samplers) image_count += std::max(1U, sampler.descriptor.count);
    for (const auto& buffer : reflection->argument_buffers) image_count += std::max(buffer.count, buffer.descriptor.count);
    image_count += reflection->color_inputs.size();
    images.reserve(image_count);
    texel_buffers.reserve(texel_count);
    buffers.reserve(1);
    if (layout.argument_table_size) {
        // Every slot is written, whatever the reflected table size: a
        // [[buffer(fc + n)]] binding (BufferBinding::index_function_constant)
        // picks its slot from a function constant at pipeline creation.
        const auto size = std::max<std::uint32_t>(layout.argument_table_size,
                                                  8U * static_cast<std::uint32_t>(state.buffers.size()));
        const auto table = upload(size, device->profile().min_uniform_offset_alignment);
        perf::add(perf::mtl_argument_tables);
        for (std::uint32_t index = 0; index < state.buffers.size(); ++index)
            std::memcpy(table.pointer + 8 * index, &state.buffers[index].address, 8);
        if (size > 8 * state.buffers.size()) std::memset(table.pointer + 8 * state.buffers.size(), 0, size - 8 * state.buffers.size());
        buffers.push_back({table.buffer, table.offset, size});
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = msl::argument_table_binding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &buffers.back();
        writes.push_back(write);
    }
    auto& fallback = fallbacks(*device);
    for (const auto& texture : reflection->textures) {
        const auto count = std::max(1U, texture.descriptor.count);
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = texture.descriptor.binding;
        write.descriptorCount = count;
        const bool texel = texture.descriptor.type == msl::DescriptorType::uniform_texel_buffer ||
                           texture.descriptor.type == msl::DescriptorType::storage_texel_buffer;
        const bool storage = texture.descriptor.type == msl::DescriptorType::storage_image;
        write.descriptorType = texel ? (texture.descriptor.type == msl::DescriptorType::uniform_texel_buffer
                                            ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
                                     : storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        if (texel) {
            const auto first = texel_buffers.size();
            for (std::uint32_t element = 0; element < count; ++element) {
                const auto index = texture.index + element;
                const auto& bound = index < state.textures.size() ? state.textures[index] : nullptr;
                texel_buffers.push_back(bound && bound->buffer_view ? bound->buffer_view : fallback.texel_buffer());
            }
            write.pTexelBufferView = texel_buffers.data() + first;
        } else {
            const auto first = images.size();
            for (std::uint32_t element = 0; element < count; ++element) {
                const auto index = texture.index + element;
                const auto& bound = index < state.textures.size() ? state.textures[index] : nullptr;
                VkImageView view{};
                if (!bound) {
                    view = storage ? fallback.storage_view(fallback_view_type(texture.texture_type))
                                   : fallback.view(fallback_view_type(texture.texture_type));
                } else if (storage) {
                    // access::write / read_write / atomic textures. Every
                    // single-sampled color texture whose format Vulkan can
                    // store to has a storage view (Texture::create); the rest
                    // (sRGB, compressed, depth, multisampled) cannot be
                    // written by a Vulkan shader at all.
                    if (!bound->storage_view)
                        throw std::runtime_error("Alchemy Metal: MTLPixelFormat" + std::string(bound->info->name) + " texture" +
                            (bound->label.empty() ? std::string() : " '" + bound->label + "'") + " bound to the writable " +
                            "texture(" + std::to_string(index) + ") '" + texture.name +
                            "' is not a storage image on " + device->profile().name);
                    view = bound->storage_view;
                } else {
                    view = bound->view ? bound->view : fallback.view(fallback_view_type(texture.texture_type));
                }
                images.push_back({VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL});
            }
            write.pImageInfo = images.data() + first;
        }
        writes.push_back(write);
    }
    for (const auto& sampler : reflection->samplers) {
        const auto count = std::max(1U, sampler.descriptor.count);
        const auto first = images.size();
        for (std::uint32_t element = 0; element < count; ++element) {
            const auto index = sampler.index + element;
            const auto& bound = index < state.samplers.size() ? state.samplers[index] : nullptr;
            images.push_back({bound ? bound->sampler : fallback.default_sampler(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
        }
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = sampler.descriptor.binding;
        write.descriptorCount = count;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        write.pImageInfo = images.data() + first;
        writes.push_back(write);
    }
    // Argument buffers of samplers: the buffer bound at the argument's index
    // holds one sampler token per id (MTLArgumentEncoder setSamplerState:).
    for (std::size_t entry = 0; entry < reflection->argument_buffers.size(); ++entry) {
        const auto& argument = reflection->argument_buffers[entry];
        const auto count = std::max(argument.count, argument.descriptor.count);
        const auto index = entry < layout.argument_buffer_indices.size() ? layout.argument_buffer_indices[entry] : argument.index;
        const auto& bound = state.buffers.at(index);
        const std::uint8_t* tokens = nullptr;
        if (bound.buffer) {
            if (!bound.buffer->contents())
                throw std::runtime_error("Alchemy Metal: argument buffer of samplers '" + argument.name + "' (buffer " +
                                         std::to_string(index) + ") must be CPU-visible");
            if (bound.offset + 8ULL * count > bound.buffer->length)
                throw std::runtime_error("Alchemy Metal: argument buffer of samplers '" + argument.name + "' (buffer " +
                                         std::to_string(index) + ") is smaller than its " + std::to_string(count) + " samplers");
            tokens = bound.buffer->contents() + bound.offset;
        } else if (argument.used) {
            throw std::runtime_error("Alchemy Metal: no argument buffer bound at buffer(" + std::to_string(index) + ") for '" +
                                     argument.name + "'");
        }
        const auto first = images.size();
        for (std::uint32_t element = 0; element < count; ++element) {
            std::uint64_t token{};
            if (tokens) std::memcpy(&token, tokens + 8ULL * element, 8);
            const auto sampler = sampler_from_token(token);
            if (sampler) retain(sampler);
            images.push_back({sampler ? sampler->sampler : fallback.default_sampler(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
        }
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = argument.descriptor.binding;
        write.descriptorCount = count;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        write.pImageInfo = images.data() + first;
        writes.push_back(write);
    }
    // Framebuffer fetch: [[color(n)]] reads color attachment n of the
    // current pass as an input attachment (dynamic rendering local read;
    // attachments stay in GENERAL).
    for (const auto& input : reflection->color_inputs) {
        if (encoder_ != EncoderKind::render || input.color >= color_infos_.size() || !color_infos_[input.color].imageView)
            throw std::runtime_error("Alchemy Metal: fragment input [[color(" + std::to_string(input.color) + ")]] '" + input.name +
                                     "' reads a color attachment the render pass does not have");
        images.push_back({VK_NULL_HANDLE, color_infos_[input.color].imageView, VK_IMAGE_LAYOUT_GENERAL});
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = input.descriptor.binding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        write.pImageInfo = &images.back();
        writes.push_back(write);
    }
    device->vk.vkUpdateDescriptorSets(device->device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    perf::add(perf::mtl_descriptor_writes, writes.size());
    return set;
}

void CommandBuffer::require(EncoderKind kind, const char* operation) const {
    if (encoder_ != kind) throw std::runtime_error(std::string("Metal ") + operation + " outside a matching command encoder");
}

CommandBuffer::StageState& CommandBuffer::stage_state(Stage stage) {
    switch (stage) {
    case Stage::vertex: require(EncoderKind::render, "vertex binding"); return render_stages_[0];
    case Stage::fragment: require(EncoderKind::render, "fragment binding"); return render_stages_[1];
    case Stage::kernel: require(EncoderKind::compute, "compute binding"); return compute_stage_;
    }
    throw std::runtime_error("invalid Metal stage");
}

// ---- encoders ---------------------------------------------------------------------------

void CommandBuffer::begin_render(const RenderPass& pass) {
    perf::add(perf::mtl_render_encoders);
    if (encoder_ != EncoderKind::none) end_encoding();
    const auto command = recording();
    full_barrier();
    write_samples(pass.samples.start, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
    end_samples_ = pass.samples.end;
    pass_ = pass;
    raster_variant_ = 0;
    for (const auto& color : pass.colors) { prepare_texture(color.texture); prepare_texture(color.resolve_texture); }
    prepare_texture(pass.depth.texture);
    prepare_texture(pass.depth.resolve_texture);
    prepare_texture(pass.stencil.texture);
    if (pass.visibility) {
        retain(pass.visibility);
        query_capacity_ = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(pass.visibility->length / 8 * 2, 64, 65536));
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_OCCLUSION;
        info.queryCount = query_capacity_;
        if (queries_) {
            const auto previous = queries_;
            Device* owner = device.get();
            on_complete([owner, previous] { owner->vk.vkDestroyQueryPool(owner->device, previous, nullptr); });
        }
        check(device->vk.vkCreateQueryPool(device->device, &info, nullptr, &queries_), "vkCreateQueryPool");
        device->vk.vkCmdResetQueryPool(command, queries_, 0, query_capacity_);
        query_count_ = 0;
        query_offsets_.clear();
        query_active_ = false;
    }

    // Attachments. Sizes come from the first attachment's level.
    extent_ = {pass.width, pass.height};
    bool sized{};
    const auto size_from = [&](const Attachment& attachment) {
        if (!attachment.texture) return;
        const auto level = attachment.texture->level_extent(attachment.level);
        extent_ = sized ? VkExtent2D{std::min(extent_.width, level.width), std::min(extent_.height, level.height)}
                        : VkExtent2D{level.width, level.height};
        sized = true;
    };
    for (const auto& color : pass.colors) size_from(color);
    size_from(pass.depth);
    size_from(pass.stencil);
    if (!sized && (!pass.width || !pass.height))
        throw std::runtime_error("Metal render pass without attachments needs renderTargetWidth and renderTargetHeight");
    pass_layers_ = std::max(1U, pass.layers);
    const auto attachment_info = [&](const Attachment& attachment, bool color) {
        VkRenderingAttachmentInfo info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (!attachment.texture) return info;
        const auto& texture = *attachment.texture;
        const auto layer = texture.descriptor.type == TextureType::type3d ? attachment.depth_plane : attachment.slice;
        info.imageView = attachment.texture->attachment_view(attachment.level, layer, pass_layers_);
        info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        info.loadOp = load_op(attachment.load_action);
        info.storeOp = store_op(attachment.store_action);
        if (resolves(attachment.store_action) && attachment.resolve_texture) {
            const auto& resolve = *attachment.resolve_texture;
            const auto resolve_layer = resolve.descriptor.type == TextureType::type3d ? attachment.resolve_depth_plane
                                                                                       : attachment.resolve_slice;
            info.resolveMode = color && !texture.info->integer() ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            info.resolveImageView = attachment.resolve_texture->attachment_view(attachment.resolve_level, resolve_layer, pass_layers_);
            info.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        if (color) {
            if (texture.info->format_class == FormatClass::color_uint) {
                for (int c = 0; c < 4; ++c) info.clearValue.color.uint32[c] = static_cast<std::uint32_t>(attachment.clear_color[c]);
            } else if (texture.info->format_class == FormatClass::color_sint) {
                for (int c = 0; c < 4; ++c) info.clearValue.color.int32[c] = static_cast<std::int32_t>(attachment.clear_color[c]);
            } else {
                for (int c = 0; c < 4; ++c) info.clearValue.color.float32[c] = static_cast<float>(attachment.clear_color[c]);
            }
        } else {
            info.clearValue.depthStencil = {static_cast<float>(attachment.clear_depth), attachment.clear_stencil};
        }
        return info;
    };
    color_infos_.clear();
    std::uint32_t color_count = 0;
    for (std::uint32_t index = 0; index < pass.colors.size(); ++index)
        if (pass.colors[index].texture) color_count = index + 1;
    for (std::uint32_t index = 0; index < color_count; ++index) color_infos_.push_back(attachment_info(pass.colors[index], true));
    depth_info_ = attachment_info(pass.depth, false);
    stencil_info_ = attachment_info(pass.stencil, false);
    if (pass.stencil.texture) stencil_info_.clearValue.depthStencil.stencil = pass.stencil.clear_stencil;
    rendering_ = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering_.renderArea = {{0, 0}, extent_};
    rendering_.layerCount = pass_layers_;
    rendering_.colorAttachmentCount = color_count;
    rendering_.pColorAttachments = color_infos_.data();
    rendering_.pDepthAttachment = pass.depth.texture ? &depth_info_ : nullptr;
    rendering_.pStencilAttachment = pass.stencil.texture ? &stencil_info_ : nullptr;
    device->vk.vkCmdBeginRendering(command, &rendering_);
    encoder_ = EncoderKind::render;
    if (trace_passes()) {
        std::ostringstream line;
        line << "[metal pass] " << extent_.width << 'x' << extent_.height << " layers " << pass_layers_;
        const auto describe = [&](const char* what, const Attachment& a) {
            if (!a.texture) return;
            line << ' ' << what << '=' << a.texture->info->name << (a.texture->label.empty() ? "" : " '" + a.texture->label + "'")
                 << " load " << a.load_action << " store " << a.store_action;
            if (a.load_action == 2 && !std::strcmp(what, "depth")) line << " clear " << a.clear_depth;
        };
        for (std::size_t index = 0; index < pass.colors.size(); ++index) {
            const std::string what = "c" + std::to_string(index);
            describe(what.c_str(), pass.colors[index]);
        }
        describe("depth", pass.depth);
        describe("stencil", pass.stencil);
        std::cerr << line.str() << '\n';
    }

    // Metal's encoder defaults.
    render_pipeline_.reset();
    foreign_render_.reset();
    native_state_lost_ = false;
    depth_stencil_.reset();
    bound_pipeline_ = VK_NULL_HANDLE;
    bound_layout_ = VK_NULL_HANDLE;
    bound_topology_ = UINT32_MAX;
    bound_vertex_buffers_.fill(VK_NULL_HANDLE);
    bound_vertex_offsets_.fill(0);
    for (auto& stage : render_stages_) stage = StageState{};
    viewport_count_ = 1;
    scissors_.clear();
    const Viewport viewport{0, 0, static_cast<double>(extent_.width), static_cast<double>(extent_.height), 0, 1};
    set_viewports({&viewport, 1});
    const Scissor scissor{0, 0, extent_.width, extent_.height};
    set_scissors({&scissor, 1});
    set_cull_mode(0);
    set_front_facing(0);
    set_depth_bias(0, 0, 0);
    set_stencil_reference(0, 0);
    set_blend_color(0, 0, 0, 0);
    set_depth_stencil(nullptr);
    device->vk.vkCmdSetPrimitiveTopology(command, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    device->vk.vkCmdSetPrimitiveRestartEnable(command, VK_FALSE);
    device->vk.vkCmdSetDepthBoundsTestEnable(command, VK_FALSE);
}

void CommandBuffer::end_render() {
    const auto command = current_;
    if (query_active_) { device->vk.vkCmdEndQuery(command, queries_, query_count_ - 1); query_active_ = false; }
    device->vk.vkCmdEndRendering(command);
    write_samples(end_samples_, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    end_samples_.clear();
    if (!indirect_traces_.empty()) {
        barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
        for (const auto& trace : indirect_traces_) {
            const auto copy = upload(trace.size, 4);
            const VkBufferCopy region{trace.offset, copy.offset, trace.size};
            device->vk.vkCmdCopyBuffer(command, trace.buffer, copy.buffer, 1, &region);
            on_complete([trace, pointer = copy.pointer] {
                std::uint32_t words[5]{};
                std::memcpy(words, pointer, static_cast<std::size_t>(trace.size));
                std::cerr << "[metal indirect] " << trace.pipeline << " offset " << trace.offset << ": " << words[0] << ' '
                          << words[1] << ' ' << words[2] << ' ' << static_cast<std::int32_t>(words[3]);
                if (trace.size > 16) std::cerr << ' ' << words[4];
                std::cerr << '\n';
            });
        }
        indirect_traces_.clear();
    }
    if (queries_ && query_count_) {
        // Metal writes an offset's result when the pass ends, so later commands
        // of the same command buffer (a blit that resolves the queries, as
        // Dawn's) read it: an offset that one query used gets its result copied
        // on the GPU, in private and shared storage alike. Metal accumulates
        // every draw of a pass that used the same offset; those sums are made
        // at completion, which only shared storage allows.
        const auto& visibility = pass_.visibility;
        std::map<std::uint64_t, std::vector<std::uint32_t>> by_offset;
        for (std::uint32_t index = 0; index < query_count_; ++index) by_offset[query_offsets_[index]].push_back(index);
        barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        std::vector<std::pair<std::uint64_t, std::vector<std::uint32_t>>> summed;
        for (auto& [offset, indices] : by_offset) {
            if (offset % 8 || offset + 8 > visibility->length)
                throw std::runtime_error("Metal visibility result offset " + std::to_string(offset) +
                                         " not 8-byte aligned or past the visibilityResultBuffer");
            if (indices.size() == 1) {
                device->vk.vkCmdCopyQueryPoolResults(command, queries_, indices.front(), 1, visibility->buffer, offset, 8,
                                                     VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
                continue;
            }
            if (!visibility->contents())
                throw std::runtime_error("Metal visibility results summed over several queries at one offset need a "
                                         "shared visibilityResultBuffer (private storage is not translated)");
            summed.emplace_back(offset, std::move(indices));
        }
        if (!summed.empty()) {
            const auto results = upload(8ULL * query_count_, 8);
            device->vk.vkCmdCopyQueryPoolResults(command, queries_, 0, query_count_, results.buffer, results.offset, 8,
                                                 VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            const auto pointer = results.pointer;
            on_complete([visibility, summed = std::move(summed), pointer] {
                for (const auto& [offset, indices] : summed) {
                    std::uint64_t sum{};
                    for (const auto index : indices) {
                        std::uint64_t value{};
                        std::memcpy(&value, pointer + 8ULL * index, 8);
                        sum += value;
                    }
                    std::memcpy(visibility->contents() + offset, &sum, 8);
                }
            });
        }
        barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    }
    if (queries_) {
        const auto pool = queries_;
        Device* owner = device.get();
        on_complete([owner, pool] { owner->vk.vkDestroyQueryPool(owner->device, pool, nullptr); });
        queries_ = VK_NULL_HANDLE;
    }
    render_pipeline_.reset();
    pass_ = RenderPass{};
}

void CommandBuffer::begin_compute(bool concurrent, EncoderSamples samples) {
    perf::add(perf::mtl_compute_encoders);
    if (encoder_ != EncoderKind::none) end_encoding();
    recording();
    full_barrier();
    write_samples(samples.start, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
    end_samples_ = std::move(samples.end);
    encoder_ = EncoderKind::compute;
    concurrent_ = concurrent;
    dispatched_ = false;
    compute_pipeline_.reset();
    foreign_compute_.reset();
    compute_stage_ = StageState{};
    threadgroup_memory_.clear();
    bound_pipeline_ = VK_NULL_HANDLE;
    bound_layout_ = VK_NULL_HANDLE;
}

void CommandBuffer::begin_blit(EncoderSamples samples) {
    perf::add(perf::mtl_blit_encoders);
    if (encoder_ != EncoderKind::none) end_encoding();
    recording();
    full_barrier();
    write_samples(samples.start, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
    end_samples_ = std::move(samples.end);
    encoder_ = EncoderKind::blit;
    blit_started_ = false;
}

void CommandBuffer::end_encoding() {
    if (encoder_ == EncoderKind::render) end_render();
    if (encoder_ == EncoderKind::compute || encoder_ == EncoderKind::blit) {
        write_samples(end_samples_, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        end_samples_.clear();
    }
    if (encoder_ == EncoderKind::compute) compute_pipeline_.reset();
    encoder_ = EncoderKind::none;
}

void CommandBuffer::push_debug_group(const std::string& name) {
    if (!device->vk.vkCmdBeginDebugUtilsLabelEXT) return;
    VkDebugUtilsLabelEXT label_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    label_info.pLabelName = name.c_str();
    device->vk.vkCmdBeginDebugUtilsLabelEXT(recording(), &label_info);
}

void CommandBuffer::pop_debug_group() {
    if (device->vk.vkCmdEndDebugUtilsLabelEXT && current_) device->vk.vkCmdEndDebugUtilsLabelEXT(current_);
}

void CommandBuffer::insert_debug_signpost(const std::string& name) {
    if (!device->vk.vkCmdInsertDebugUtilsLabelEXT) return;
    VkDebugUtilsLabelEXT label_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    label_info.pLabelName = name.c_str();
    device->vk.vkCmdInsertDebugUtilsLabelEXT(recording(), &label_info);
}

// ---- bindings ---------------------------------------------------------------------------

void CommandBuffer::set_buffer(Stage stage, std::uint32_t index, std::shared_ptr<Buffer> buffer, std::uint64_t offset) {
    auto& state = stage_state(stage);
    if (index >= state.buffers.size()) throw std::out_of_range("Metal buffer index " + std::to_string(index) + " exceeds 30");
    perf::add(perf::mtl_set_buffer);
    auto& binding = state.buffers[index];
    binding = {};
    if (buffer) {
        binding.address = buffer->address + offset;
        binding.offset = offset;
        binding.vk_buffer = buffer->buffer;
        binding.vk_offset = offset;
        retain(buffer);
        binding.buffer = std::move(buffer);
    }
    state.dirty = true;
}

void CommandBuffer::set_buffer_offset(Stage stage, std::uint32_t index, std::uint64_t offset) {
    auto& state = stage_state(stage);
    if (index >= state.buffers.size()) throw std::out_of_range("Metal buffer index " + std::to_string(index) + " exceeds 30");
    auto& binding = state.buffers[index];
    if (!binding.buffer) throw std::runtime_error("Metal setBufferOffset: on an index without a buffer");
    binding.address = binding.buffer->address + offset;
    binding.offset = binding.vk_offset = offset;
    state.dirty = true;
}

void CommandBuffer::set_bytes(Stage stage, std::uint32_t index, std::span<const std::uint8_t> bytes) {
    auto& state = stage_state(stage);
    if (index >= state.buffers.size()) throw std::out_of_range("Metal buffer index " + std::to_string(index) + " exceeds 30");
    perf::add(perf::mtl_set_bytes);
    perf::add(perf::mtl_set_bytes_bytes, bytes.size());
    const auto memory = upload(bytes.size(), bytes_alignment);
    if (!bytes.empty()) std::memcpy(memory.pointer, bytes.data(), bytes.size());
    auto& binding = state.buffers[index];
    binding = {};
    binding.address = memory.address;
    binding.vk_buffer = memory.buffer;
    binding.vk_offset = memory.offset;
    binding.bytes_length = static_cast<std::uint32_t>(bytes.size());
    state.dirty = true;
}

void CommandBuffer::set_texture(Stage stage, std::uint32_t index, std::shared_ptr<Texture> texture) {
    auto& state = stage_state(stage);
    if (index >= state.textures.size()) throw std::out_of_range("Metal texture index " + std::to_string(index) + " exceeds 127");
    perf::add(perf::mtl_set_texture);
    if (texture) {
        // Shaders write only textures created with MTLTextureUsageShaderWrite.
        const bool write = (texture->descriptor.usage & texture_usage::shader_write) != 0;
        if (encoder_ != EncoderKind::render) prepare_texture(texture, write);
        else prepare_in_pass(texture, write);
    }
    state.textures[index] = std::move(texture);
    state.dirty = true;
}

void CommandBuffer::set_sampler(Stage stage, std::uint32_t index, std::shared_ptr<Sampler> sampler) {
    auto& state = stage_state(stage);
    if (index >= state.samplers.size()) throw std::out_of_range("Metal sampler index " + std::to_string(index) + " exceeds 15");
    perf::add(perf::mtl_set_sampler);
    retain(sampler);
    state.samplers[index] = std::move(sampler);
    state.dirty = true;
}

// ---- render state -------------------------------------------------------------------------

void CommandBuffer::set_render_pipeline(std::shared_ptr<RenderPipeline> pipeline) {
    require(EncoderKind::render, "setRenderPipelineState:");
    perf::add(perf::mtl_set_pipeline);
    retain(pipeline);
    render_pipeline_ = std::move(pipeline);
    foreign_render_.reset();
}

void CommandBuffer::set_viewports(std::span<const Viewport> viewports) {
    require(EncoderKind::render, "setViewport:");
    std::vector<VkViewport> values;
    for (const auto& viewport : viewports) {
        // Metal's NDC has +Y up; a negative-height viewport gives Vulkan the
        // same mapping (VK_KHR_maintenance1, core).
        VkViewport value{};
        value.x = static_cast<float>(viewport.x);
        value.y = static_cast<float>(viewport.y + viewport.height);
        value.width = static_cast<float>(viewport.width);
        value.height = -static_cast<float>(viewport.height);
        value.minDepth = static_cast<float>(std::clamp(viewport.znear, 0.0, 1.0));
        value.maxDepth = static_cast<float>(std::clamp(viewport.zfar, 0.0, 1.0));
        values.push_back(value);
    }
    device->vk.vkCmdSetViewportWithCount(current_, static_cast<std::uint32_t>(values.size()), values.data());
    perf::add(perf::mtl_vk_state);
    viewports_ = values;
    const auto count = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(values.size()));
    if (count != viewport_count_) {
        viewport_count_ = count;
        emit_scissors();
    }
}

void CommandBuffer::emit_scissors() {
    std::vector<VkRect2D> values(scissors_.begin(), scissors_.begin() + std::min<std::size_t>(scissors_.size(), viewport_count_));
    while (values.size() < viewport_count_) values.push_back({{0, 0}, extent_});
    device->vk.vkCmdSetScissorWithCount(current_, static_cast<std::uint32_t>(values.size()), values.data());
    perf::add(perf::mtl_vk_state);
}

void CommandBuffer::set_scissors(std::span<const Scissor> scissors) {
    require(EncoderKind::render, "setScissorRect:");
    std::vector<VkRect2D> values;
    for (const auto& scissor : scissors) {
        const auto x = std::min<std::uint64_t>(scissor.x, extent_.width);
        const auto y = std::min<std::uint64_t>(scissor.y, extent_.height);
        const auto width = std::min<std::uint64_t>(scissor.width, extent_.width - x);
        const auto height = std::min<std::uint64_t>(scissor.height, extent_.height - y);
        values.push_back({{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)},
                          {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)}});
    }
    scissors_ = std::move(values);
    emit_scissors();
}

void CommandBuffer::set_cull_mode(std::uint32_t mode) {
    require(EncoderKind::render, "setCullMode:");
    const VkCullModeFlags modes[] = {VK_CULL_MODE_NONE, VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_BACK_BIT};
    if (mode > 2) throw std::runtime_error("invalid MTLCullMode " + std::to_string(mode));
    device->vk.vkCmdSetCullMode(current_, modes[mode]);
    perf::add(perf::mtl_vk_state);
}

void CommandBuffer::set_depth_clip_mode(std::uint32_t mode) {
    require(EncoderKind::render, "setDepthClipMode:");
    if (mode > 1) throw std::runtime_error("invalid MTLDepthClipMode " + std::to_string(mode));
    if (mode && !device->features10.depthClamp)
        throw std::runtime_error("MTLDepthClipModeClamp needs Vulkan's depthClamp, which " + device->profile().name + " lacks");
    raster_variant_ = mode ? raster_variant_ | raster_variant::depth_clamp : raster_variant_ & ~raster_variant::depth_clamp;
}

void CommandBuffer::set_triangle_fill_mode(std::uint32_t mode) {
    require(EncoderKind::render, "setTriangleFillMode:");
    if (mode > 1) throw std::runtime_error("invalid MTLTriangleFillMode " + std::to_string(mode));
    if (mode && !device->features10.fillModeNonSolid)
        throw std::runtime_error("MTLTriangleFillModeLines needs Vulkan's fillModeNonSolid, which " + device->profile().name + " lacks");
    raster_variant_ = mode ? raster_variant_ | raster_variant::fill_lines : raster_variant_ & ~raster_variant::fill_lines;
}

void CommandBuffer::set_front_facing(std::uint32_t winding) {
    require(EncoderKind::render, "setFrontFacingWinding:");
    // Both APIs define winding in window coordinates as seen on screen; the
    // flipped viewport keeps the image, so the winding maps directly.
    device->vk.vkCmdSetFrontFace(current_, winding == 0 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
    perf::add(perf::mtl_vk_state);
}

void CommandBuffer::set_depth_bias(float bias, float slope, float clamp) {
    require(EncoderKind::render, "setDepthBias:slopeScale:clamp:");
    const bool enabled = bias != 0 || slope != 0;
    depth_bias_enabled_ = enabled;
    device->vk.vkCmdSetDepthBiasEnable(current_, enabled ? VK_TRUE : VK_FALSE);
    device->vk.vkCmdSetDepthBias(current_, bias, device->features10.depthBiasClamp ? clamp : 0.0F, slope);
    perf::add(perf::mtl_vk_state, 2);
}

void CommandBuffer::set_stencil_reference(std::uint32_t front, std::uint32_t back) {
    require(EncoderKind::render, "setStencilReferenceValue:");
    device->vk.vkCmdSetStencilReference(current_, VK_STENCIL_FACE_FRONT_BIT, front);
    device->vk.vkCmdSetStencilReference(current_, VK_STENCIL_FACE_BACK_BIT, back);
    perf::add(perf::mtl_vk_state, 2);
}

void CommandBuffer::set_blend_color(float red, float green, float blue, float alpha) {
    require(EncoderKind::render, "setBlendColorRed:green:blue:alpha:");
    const float constants[4] = {red, green, blue, alpha};
    device->vk.vkCmdSetBlendConstants(current_, constants);
    perf::add(perf::mtl_vk_state);
}

void CommandBuffer::set_depth_stencil(std::shared_ptr<DepthStencilState> state) {
    require(EncoderKind::render, "setDepthStencilState:");
    const DepthStencilState defaults{};
    const auto& value = state ? *state : defaults;
    const auto command = current_;
    const bool depth = pass_.depth.texture != nullptr;
    const bool stencil = pass_.stencil.texture != nullptr;
    device->vk.vkCmdSetDepthTestEnable(command, depth && (value.depth_compare != 7 || value.depth_write) ? VK_TRUE : VK_FALSE);
    device->vk.vkCmdSetDepthWriteEnable(command, depth && value.depth_write ? VK_TRUE : VK_FALSE);
    device->vk.vkCmdSetDepthCompareOp(command, static_cast<VkCompareOp>(value.depth_compare));
    device->vk.vkCmdSetStencilTestEnable(command, stencil && (value.front.active() || value.back.active()) ? VK_TRUE : VK_FALSE);
    const auto face = [&](VkStencilFaceFlags flags, const StencilState& s) {
        device->vk.vkCmdSetStencilOp(command, flags, static_cast<VkStencilOp>(s.stencil_failure), static_cast<VkStencilOp>(s.pass),
                                     static_cast<VkStencilOp>(s.depth_failure), static_cast<VkCompareOp>(s.compare));
        device->vk.vkCmdSetStencilCompareMask(command, flags, s.read_mask);
        device->vk.vkCmdSetStencilWriteMask(command, flags, s.write_mask);
    };
    face(VK_STENCIL_FACE_FRONT_BIT, value.front);
    face(VK_STENCIL_FACE_BACK_BIT, value.back);
    perf::add(perf::mtl_vk_state, 10);
    if (state) retain(state);
    depth_stencil_ = std::move(state);
}

void CommandBuffer::set_visibility_mode(std::uint32_t mode, std::uint64_t offset) {
    require(EncoderKind::render, "setVisibilityResultMode:offset:");
    if (!queries_) {
        if (mode == 0) return;
        throw std::runtime_error("Metal visibility result mode without a visibilityResultBuffer");
    }
    if (query_active_) { device->vk.vkCmdEndQuery(current_, queries_, query_count_ - 1); query_active_ = false; }
    if (mode == 0) return;   // MTLVisibilityResultModeDisabled
    if (query_count_ == query_capacity_)
        throw std::runtime_error("Metal render pass exceeds " + std::to_string(query_capacity_) + " visibility result queries");
    // Boolean (1) needs only non-zero; Counting (2) needs exact sample counts.
    device->vk.vkCmdBeginQuery(current_, queries_, query_count_,
                               mode == 2 && device->features10.occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    query_offsets_.push_back(offset);
    ++query_count_;
    query_active_ = true;
}

void CommandBuffer::flush_render(std::uint32_t primitive) {
    require(EncoderKind::render, "draw");
    if (!render_pipeline_) throw std::runtime_error("Metal draw without a render pipeline state");
    if (native_state_lost_) restore_native_state();
    const auto command = current_;
    const auto pipeline = render_pipeline_->pipeline(primitive, raster_variant_);
    if (trace_passes()) {
        const DepthStencilState defaults{};
        const auto& ds = depth_stencil_ ? *depth_stencil_ : defaults;
        const auto* fragment = render_pipeline_->fragment_reflection();
        std::cerr << "[metal draw] " << pipeline_name(*render_pipeline_) << " / "
                  << (render_pipeline_->descriptor.fragment ? render_pipeline_->descriptor.fragment->name() : std::string("-"))
                  << " depth cmp " << ds.depth_compare << " write " << ds.depth_write
                  << " stencil " << (ds.front.active() || ds.back.active())
                  << " fragdepth " << (fragment ? static_cast<int>(fragment->depth) : -1)
                  << " fetch " << (fragment ? fragment->color_inputs.size() : 0) << '\n';
    }
    perf::add(perf::mtl_draws);
    if (pipeline != bound_pipeline_) {
        device->vk.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        bound_pipeline_ = pipeline;
        perf::add(perf::mtl_pipeline_binds);
    }
    if (render_pipeline_->layout != bound_layout_) {
        bound_layout_ = render_pipeline_->layout;
        for (auto& stage : render_stages_) stage.dirty = true;
    }
    if (primitive != bound_topology_) {
        device->vk.vkCmdSetPrimitiveTopology(command, primitive_topology(primitive));
        // Metal restarts strips at the all-ones index.
        device->vk.vkCmdSetPrimitiveRestartEnable(command, primitive == 2 || primitive == 4 ? VK_TRUE : VK_FALSE);
        bound_topology_ = primitive;
    }
    const msl::Reflection* reflections[2] = {&render_pipeline_->vertex_reflection(), render_pipeline_->fragment_reflection()};
    if (reflections[1] && !reflections[1]->color_inputs.empty()) {
        // Framebuffer fetch sees the color written by earlier draws of the
        // pass: a by-region attachment-write -> input-attachment-read
        // dependency inside the rendering (dynamic rendering local read).
        VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        memory.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        memory.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        memory.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        memory.dstAccessMask = VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &memory;
        device->vk.vkCmdPipelineBarrier2(command, &dependency);
    }
    for (std::uint32_t index = 0; index < 2; ++index) {
        auto& stage = render_stages_[index];
        if (!stage.dirty) continue;
        const auto set = write_set(stage, render_pipeline_->stages[index], reflections[index]);
        if (set) {
            device->vk.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_layout_, index, 1, &set, 0, nullptr);
            perf::add(perf::mtl_vk_binds);
        }
        stage.dirty = false;
    }
    for (const auto index : render_pipeline_->vertex_bindings) {
        const auto& binding = render_stages_[0].buffers[index];
        if (!binding.vk_buffer)
            throw std::runtime_error("Metal draw: vertex descriptor buffer " + std::to_string(index) + " is not bound");
        if (bound_vertex_buffers_[index] == binding.vk_buffer && bound_vertex_offsets_[index] == binding.vk_offset) continue;
        device->vk.vkCmdBindVertexBuffers(command, index, 1, &binding.vk_buffer, &binding.vk_offset);
        perf::add(perf::mtl_vk_binds);
        bound_vertex_buffers_[index] = binding.vk_buffer;
        bound_vertex_offsets_[index] = binding.vk_offset;
    }
}

void CommandBuffer::draw(std::uint32_t primitive, std::uint32_t vertex_start, std::uint32_t vertex_count,
                         std::uint32_t instance_count, std::uint32_t base_instance) {
    if (!vertex_count || !instance_count) return;
    if (foreign_render_) {
        auto call = json::Value::object(), primitives = json::Value::object();
        primitives.set("vertex_start", vertex_start);
        primitives.set("vertex_count", vertex_count);
        primitives.set("instance_count", instance_count);
        primitives.set("base_instance", base_instance);
        call.set("Primitives", std::move(primitives));
        foreign_draw(primitive, call, nullptr);
        return;
    }
    flush_render(primitive);
    device->vk.vkCmdDraw(current_, vertex_count, instance_count, vertex_start, base_instance);
}

void CommandBuffer::draw_indexed(std::uint32_t primitive, std::uint32_t index_count, std::uint32_t index_type,
    const std::shared_ptr<Buffer>& index_buffer, std::uint64_t index_offset, std::uint32_t instance_count,
    std::int32_t base_vertex, std::uint32_t base_instance) {
    if (!index_count || !instance_count) return;
    if (!index_buffer) throw std::runtime_error("Metal indexed draw without an index buffer");
    if (foreign_render_) {
        auto call = json::Value::object(), indexed = json::Value::object(), reference = json::Value::object();
        indexed.set("index_type", index_type == 0 ? 0 : 1);   // VK_INDEX_TYPE_UINT16 / UINT32
        reference.set("token", 1);
        reference.set("length", static_cast<std::uint64_t>(index_buffer->length));
        indexed.set("index_buffer", std::move(reference));
        indexed.set("index_count", index_count);
        indexed.set("index_offset", static_cast<std::uint64_t>(index_offset));
        indexed.set("instance_count", instance_count);
        indexed.set("base_vertex", base_vertex);
        indexed.set("base_instance", base_instance);
        call.set("Indexed", std::move(indexed));
        foreign_draw(primitive, call, index_buffer);
        return;
    }
    flush_render(primitive);
    retain(index_buffer);
    device->vk.vkCmdBindIndexBuffer(current_, index_buffer->buffer, index_offset,
                                    index_type == 0 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    device->vk.vkCmdDrawIndexed(current_, index_count, instance_count, 0, base_vertex, base_instance);
}

void CommandBuffer::draw_indirect(std::uint32_t primitive, const std::shared_ptr<Buffer>& indirect, std::uint64_t offset) {
    if (!indirect) throw std::runtime_error("Metal indirect draw without an indirect buffer");
    if (foreign_render_) throw std::runtime_error("Alchemy Metal: indirect draws of translated pipelines are not implemented");
    flush_render(primitive);
    retain(indirect);
    // MTLDrawPrimitivesIndirectArguments == VkDrawIndirectCommand.
    device->vk.vkCmdDrawIndirect(current_, indirect->buffer, offset, 1, 16);
    if (trace_indirect()) indirect_traces_.push_back({indirect->buffer, offset, 16, pipeline_name(*render_pipeline_)});
}

void CommandBuffer::draw_indexed_indirect(std::uint32_t primitive, std::uint32_t index_type,
    const std::shared_ptr<Buffer>& index_buffer, std::uint64_t index_offset, const std::shared_ptr<Buffer>& indirect,
    std::uint64_t offset) {
    if (!indirect || !index_buffer) throw std::runtime_error("Metal indexed indirect draw without its buffers");
    if (foreign_render_) throw std::runtime_error("Alchemy Metal: indirect draws of translated pipelines are not implemented");
    flush_render(primitive);
    retain(index_buffer);
    retain(indirect);
    device->vk.vkCmdBindIndexBuffer(current_, index_buffer->buffer, index_offset,
                                    index_type == 0 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    // MTLDrawIndexedPrimitivesIndirectArguments == VkDrawIndexedIndirectCommand.
    device->vk.vkCmdDrawIndexedIndirect(current_, indirect->buffer, offset, 1, 20);
    if (trace_indirect()) indirect_traces_.push_back({indirect->buffer, offset, 20, pipeline_name(*render_pipeline_)});
}

void CommandBuffer::memory_barrier(std::uint64_t) {
    if (encoder_ == EncoderKind::compute) {
        barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
        return;
    }
    require(EncoderKind::render, "memoryBarrierWithScope:");
    // A barrier cannot sit inside a dynamic-rendering instance: suspend the
    // pass, barrier, and resume it with the attachments' contents loaded.
    if (query_active_) { device->vk.vkCmdEndQuery(current_, queries_, query_count_ - 1); query_active_ = false; }
    device->vk.vkCmdEndRendering(current_);
    full_barrier();
    for (auto& color : color_infos_) { color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE; }
    depth_info_.loadOp = stencil_info_.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_info_.storeOp = stencil_info_.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    device->vk.vkCmdBeginRendering(current_, &rendering_);
    if (!query_offsets_.empty() && queries_) {
        // The visibility mode continues with the same offset.
        const auto offset = query_offsets_.back();
        set_visibility_mode(1, offset);
    }
}

// ---- compute ------------------------------------------------------------------------------

void CommandBuffer::set_compute_pipeline(std::shared_ptr<ComputePipeline> pipeline) {
    require(EncoderKind::compute, "setComputePipelineState:");
    perf::add(perf::mtl_set_pipeline);
    retain(pipeline);
    compute_pipeline_ = std::move(pipeline);
    foreign_compute_.reset();
}

void CommandBuffer::set_threadgroup_memory(std::uint32_t index, std::uint32_t length) {
    require(EncoderKind::compute, "setThreadgroupMemoryLength:atIndex:");
    if (index >= 31) throw std::out_of_range("Metal threadgroup memory index exceeds 30");
    if (threadgroup_memory_.size() <= index) threadgroup_memory_.resize(index + 1);
    threadgroup_memory_[index] = length;
}

void CommandBuffer::flush_compute(const std::array<std::uint32_t, 3>& threads, const std::array<std::uint32_t, 3>& grid,
                                  bool non_uniform) {
    require(EncoderKind::compute, "dispatch");
    if (!compute_pipeline_) throw std::runtime_error("Metal dispatch without a compute pipeline state");
    perf::add(perf::mtl_dispatches);
    const auto command = current_;
    if (!concurrent_ && dispatched_)
        barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    const auto pipeline = compute_pipeline_->pipeline(threads, threadgroup_memory_);
    if (trace_passes())
        std::cerr << "[metal dispatch] " << (compute_pipeline_->label.empty() ? compute_pipeline_->function->name()
                                                                                  : compute_pipeline_->label)
                  << " grid " << grid[0] << 'x' << grid[1] << 'x' << grid[2] << " threads " << threads[0] << 'x' << threads[1]
                  << 'x' << threads[2] << (concurrent_ ? " concurrent" : "") << '\n';
    if (pipeline != bound_pipeline_) {
        device->vk.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        bound_pipeline_ = pipeline;
        perf::add(perf::mtl_pipeline_binds);
    }
    if (compute_pipeline_->layout != bound_layout_) { bound_layout_ = compute_pipeline_->layout; compute_stage_.dirty = true; }
    if (compute_stage_.dirty) {
        const auto set = write_set(compute_stage_, compute_pipeline_->stage, &compute_pipeline_->function->reflection());
        if (set) device->vk.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, bound_layout_, 0, 1, &set, 0, nullptr);
        compute_stage_.dirty = false;
    }
    const KernelPushConstants push{{grid[0], grid[1], grid[2]}, non_uniform ? 1U : 0U};
    device->vk.vkCmdPushConstants(command, bound_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    dispatched_ = true;
}

void CommandBuffer::dispatch_threadgroups(const std::array<std::uint32_t, 3>& groups, const std::array<std::uint32_t, 3>& threads) {
    if (!groups[0] || !groups[1] || !groups[2]) return;
    if (foreign_compute_) {
        auto dispatch = json::Value::object(), body = json::Value::object();
        auto g = json::Value::array(), t = json::Value::array();
        for (int i = 0; i < 3; ++i) { g.push(groups[i]); t.push(threads[i]); }
        body.set("groups", std::move(g));
        body.set("threads_per_group", std::move(t));
        dispatch.set("Threadgroups", std::move(body));
        foreign_dispatch(dispatch);
        return;
    }
    flush_compute(threads, {groups[0] * threads[0], groups[1] * threads[1], groups[2] * threads[2]}, false);
    device->vk.vkCmdDispatch(current_, groups[0], groups[1], groups[2]);
}

void CommandBuffer::dispatch_threads(const std::array<std::uint32_t, 3>& grid, const std::array<std::uint32_t, 3>& threads) {
    if (!grid[0] || !grid[1] || !grid[2]) return;
    if (foreign_compute_) {
        auto dispatch = json::Value::object(), body = json::Value::object();
        auto g = json::Value::array(), t = json::Value::array();
        for (int i = 0; i < 3; ++i) { g.push(grid[i]); t.push(threads[i]); }
        body.set("threads", std::move(g));
        body.set("threads_per_group", std::move(t));
        dispatch.set("Threads", std::move(body));
        foreign_dispatch(dispatch);
        return;
    }
    const std::array<std::uint32_t, 3> groups{div_up(grid[0], threads[0]), div_up(grid[1], threads[1]), div_up(grid[2], threads[2])};
    const bool uniform = groups[0] * threads[0] == grid[0] && groups[1] * threads[1] == grid[1] && groups[2] * threads[2] == grid[2];
    if (!uniform && compute_pipeline_ && !compute_pipeline_->reads_grid) {
        // The kernel cannot see the grid: threads past it run (Metal never
        // runs them). Kernels that bound-check their own inputs are fine.
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true))
            std::cerr << "Alchemy Metal: dispatchThreads: with a non-uniform grid on kernel "
                      << compute_pipeline_->function->name()
                      << ", which does not read the kernel grid push constants; threads beyond the grid run\n";
    }
    flush_compute(threads, grid, !uniform);
    device->vk.vkCmdDispatch(current_, groups[0], groups[1], groups[2]);
}

void CommandBuffer::dispatch_indirect(const std::shared_ptr<Buffer>& indirect, std::uint64_t offset,
                                      const std::array<std::uint32_t, 3>& threads) {
    if (!indirect) throw std::runtime_error("Metal indirect dispatch without an indirect buffer");
    if (foreign_compute_) throw std::runtime_error("Alchemy Metal: indirect dispatches of translated pipelines are not implemented");
    // The grid lives in GPU memory: threads_per_grid is unknown here (0).
    flush_compute(threads, {0, 0, 0}, false);
    retain(indirect);
    // MTLDispatchThreadgroupsIndirectArguments == VkDispatchIndirectCommand.
    device->vk.vkCmdDispatchIndirect(current_, indirect->buffer, offset);
}

// ---- blit -----------------------------------------------------------------------------------

namespace {
VkImageAspectFlags blit_aspect(const Texture& texture, std::uint64_t options) {
    // MTLBlitOptionDepthFromDepthStencil = 1, StencilFromDepthStencil = 2.
    if (options & 1) return VK_IMAGE_ASPECT_DEPTH_BIT;
    if (options & 2) return VK_IMAGE_ASPECT_STENCIL_BIT;
    return texture.view_aspects();
}
} // namespace

#define ALCHEMY_BLIT_PROLOGUE(operation)                                                                  \
    require(EncoderKind::blit, operation);                                                                \
    perf::add(perf::mtl_blits);                                                                           \
    if (blit_started_)                                                                                  \
        barrier(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,                     \
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT); \
    blit_started_ = true

void CommandBuffer::copy_buffer(const std::shared_ptr<Buffer>& source, std::uint64_t source_offset,
                                const std::shared_ptr<Buffer>& destination, std::uint64_t destination_offset, std::uint64_t size) {
    ALCHEMY_BLIT_PROLOGUE("copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:");
    if (!size) return;
    retain(source);
    retain(destination);
    const VkBufferCopy copy{source_offset, destination_offset, size};
    device->vk.vkCmdCopyBuffer(current_, source->buffer, destination->buffer, 1, &copy);
}

void CommandBuffer::copy_buffer_to_texture(const std::shared_ptr<Buffer>& source, std::uint64_t offset,
    std::uint64_t bytes_per_row, std::uint64_t bytes_per_image, const Region& size, const std::shared_ptr<Texture>& destination,
    std::uint32_t slice, std::uint32_t level, std::uint64_t options) {
    ALCHEMY_BLIT_PROLOGUE("copyFromBuffer:...toTexture:");
    if (!size.width || !size.height || !size.depth) return;
    retain(source);
    prepare_texture(destination);
    if (destination->is_buffer()) {
        const VkBufferCopy copy{offset, destination->buffer_offset + static_cast<std::uint64_t>(size.x) * destination->info->block_bytes,
                                static_cast<std::uint64_t>(size.width) * destination->info->block_bytes};
        device->vk.vkCmdCopyBuffer(current_, source->buffer, destination->buffer->buffer, 1, &copy);
        return;
    }
    const auto copy = destination->copy_region(offset, bytes_per_row, bytes_per_image, size, level, slice,
                                               blit_aspect(*destination, options));
    device->vk.vkCmdCopyBufferToImage(current_, source->buffer, destination->image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
}

void CommandBuffer::copy_texture_to_buffer(const std::shared_ptr<Texture>& source, std::uint32_t slice, std::uint32_t level,
    const Region& region, const std::shared_ptr<Buffer>& destination, std::uint64_t offset, std::uint64_t bytes_per_row,
    std::uint64_t bytes_per_image, std::uint64_t options) {
    ALCHEMY_BLIT_PROLOGUE("copyFromTexture:...toBuffer:");
    if (!region.width || !region.height || !region.depth) return;
    prepare_texture(source, false);
    retain(destination);
    if (source->is_buffer()) {
        const VkBufferCopy copy{source->buffer_offset + static_cast<std::uint64_t>(region.x) * source->info->block_bytes, offset,
                                static_cast<std::uint64_t>(region.width) * source->info->block_bytes};
        device->vk.vkCmdCopyBuffer(current_, source->buffer->buffer, destination->buffer, 1, &copy);
        return;
    }
    const auto copy = source->copy_region(offset, bytes_per_row, bytes_per_image, region, level, slice, blit_aspect(*source, options));
    device->vk.vkCmdCopyImageToBuffer(current_, source->image, VK_IMAGE_LAYOUT_GENERAL, destination->buffer, 1, &copy);
}

void CommandBuffer::copy_texture(const std::shared_ptr<Texture>& source, std::uint32_t source_slice, std::uint32_t source_level,
    const Region& region, const std::shared_ptr<Texture>& destination, std::uint32_t destination_slice,
    std::uint32_t destination_level, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    ALCHEMY_BLIT_PROLOGUE("copyFromTexture:...toTexture:");
    if (!region.width || !region.height || !region.depth) return;
    if (source->is_buffer() || destination->is_buffer())
        throw std::runtime_error("Metal texture-to-texture copies of texture buffers are not translated");
    prepare_texture(source, false);
    prepare_texture(destination);
    const bool source_volume = source->descriptor.type == TextureType::type3d;
    const bool destination_volume = destination->descriptor.type == TextureType::type3d;
    VkImageCopy copy{};
    copy.srcSubresource = {source->view_aspects(), source->base_level + source_level,
                           source->base_layer + (source_volume ? 0 : source_slice), 1};
    copy.dstSubresource = {destination->view_aspects(), destination->base_level + destination_level,
                           destination->base_layer + (destination_volume ? 0 : destination_slice), 1};
    copy.srcOffset = {static_cast<std::int32_t>(region.x), static_cast<std::int32_t>(region.y),
                      static_cast<std::int32_t>(source_volume ? region.z : 0)};
    copy.dstOffset = {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
                      static_cast<std::int32_t>(destination_volume ? z : 0)};
    copy.extent = {region.width, region.height, source_volume ? region.depth : 1};
    device->vk.vkCmdCopyImage(current_, source->image, VK_IMAGE_LAYOUT_GENERAL, destination->image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
}

void CommandBuffer::copy_texture_all(const std::shared_ptr<Texture>& source, const std::shared_ptr<Texture>& destination) {
    // copyFromTexture:toTexture: copies every level and slice both have.
    const auto levels = std::min(source->descriptor.mipmap_levels, destination->descriptor.mipmap_levels);
    const auto slices = std::min(source->layer_count(), destination->layer_count());
    for (std::uint32_t level = 0; level < levels; ++level) {
        const auto extent = source->level_extent(level);
        for (std::uint32_t slice = 0; slice < slices; ++slice)
            copy_texture(source, slice, level, {0, 0, 0, extent.width, extent.height, extent.depth}, destination, slice, level, 0, 0, 0);
    }
}

void CommandBuffer::fill_buffer(const std::shared_ptr<Buffer>& buffer, std::uint64_t offset, std::uint64_t length,
                                std::uint8_t value) {
    ALCHEMY_BLIT_PROLOGUE("fillBuffer:range:value:");
    if (!length) return;
    retain(buffer);
    // vkCmdFillBuffer works on aligned 4-byte words; unaligned edges are
    // copied from upload memory holding the pattern.
    const auto head_end = std::min(offset + length, (offset + 3) & ~std::uint64_t{3});
    const auto tail_start = std::max(head_end, (offset + length) & ~std::uint64_t{3});
    const auto copy_edge = [&](std::uint64_t begin, std::uint64_t end) {
        if (end <= begin) return;
        const auto pattern = upload(end - begin, 4);
        std::memset(pattern.pointer, value, static_cast<std::size_t>(end - begin));
        const VkBufferCopy copy{pattern.offset, begin, end - begin};
        device->vk.vkCmdCopyBuffer(current_, pattern.buffer, buffer->buffer, 1, &copy);
    };
    copy_edge(offset, head_end);
    if (tail_start > head_end)
        device->vk.vkCmdFillBuffer(current_, buffer->buffer, head_end, tail_start - head_end, 0x01010101U * value);
    copy_edge(tail_start, offset + length);
}

void CommandBuffer::resolve_counters(const std::shared_ptr<CounterSampleBuffer>& samples, std::uint32_t first, std::uint32_t length,
                                     const std::shared_ptr<Buffer>& destination, std::uint64_t offset) {
    ALCHEMY_BLIT_PROLOGUE("resolveCounters:inRange:destinationBuffer:destinationOffset:");
    if (!samples || !destination) throw std::runtime_error("Metal resolveCounters: with a nil sample buffer or destination");
    if (first > samples->count || length > samples->count - first)
        throw std::runtime_error("Metal resolveCounters: range outside the sample buffer");
    if (offset % 8 || offset + 8ULL * length > destination->length)
        throw std::runtime_error("Metal resolveCounters: destination offset not 8-byte aligned or past the buffer");
    if (!length) return;
    retain(samples);
    retain(destination);
    // Samples no recorded command writes are MTLCounterErrorValue; the
    // others are copied once the GPU wrote them (they precede this blit in
    // submission order, and the encoder began with a full barrier).
    const auto recorded = samples->recorded(first, length);
    for (std::uint32_t begin = 0; begin < length;) {
        std::uint32_t end = begin;
        while (end < length && recorded[end] == recorded[begin]) ++end;
        const auto at = offset + 8ULL * begin;
        if (recorded[begin])
            device->vk.vkCmdCopyQueryPoolResults(current_, samples->pool, first + begin, end - begin, destination->buffer, at, 8,
                                                 VK_QUERY_RESULT_64_BIT);
        else
            device->vk.vkCmdFillBuffer(current_, destination->buffer, at, 8ULL * (end - begin), 0xffffffffU);
        begin = end;
    }
}

void CommandBuffer::generate_mipmaps(const std::shared_ptr<Texture>& texture) {
    ALCHEMY_BLIT_PROLOGUE("generateMipmapsForTexture:");
    prepare_texture(texture);
    if (texture->is_buffer()) throw std::runtime_error("Metal cannot generate mipmaps of a texture buffer");
    const bool linear = (device->format_features(texture->format) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    const bool volume = texture->descriptor.type == TextureType::type3d;
    for (std::uint32_t level = 1; level < texture->descriptor.mipmap_levels; ++level) {
        const auto source = texture->level_extent(level - 1);
        const auto destination = texture->level_extent(level);
        VkImageBlit blit{};
        blit.srcSubresource = {texture->view_aspects(), texture->base_level + level - 1, texture->base_layer, texture->layer_count()};
        blit.srcOffsets[1] = {static_cast<std::int32_t>(source.width), static_cast<std::int32_t>(source.height),
                              static_cast<std::int32_t>(volume ? source.depth : 1)};
        blit.dstSubresource = {texture->view_aspects(), texture->base_level + level, texture->base_layer, texture->layer_count()};
        blit.dstOffsets[1] = {static_cast<std::int32_t>(destination.width), static_cast<std::int32_t>(destination.height),
                              static_cast<std::int32_t>(volume ? destination.depth : 1)};
        device->vk.vkCmdBlitImage(current_, texture->image, VK_IMAGE_LAYOUT_GENERAL, texture->image, VK_IMAGE_LAYOUT_GENERAL, 1,
                                  &blit, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        barrier(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
}

#undef ALCHEMY_BLIT_PROLOGUE

// ---- events, presentation, commit ------------------------------------------------------------

void CommandBuffer::encode_signal(const std::shared_ptr<Event>& event, std::uint64_t value) {
    if (encoder_ != EncoderKind::none) throw std::runtime_error("Metal encodeSignalEvent: while an encoder is active");
    if (!event) throw std::runtime_error("Metal encodeSignalEvent: with a nil event");
    // g3: what waits for the signal sees IOSurface-backed textures' pages.
    if (!surface_writes_.empty()) flush_surface_tails(recording());
    else recording();
    close_recording();
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = event->semaphore;
    signal.value = value;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    batches_.back().signals.push_back(signal);
    batches_.emplace_back();
    events_.push_back(event);
}

void CommandBuffer::encode_wait(const std::shared_ptr<Event>& event, std::uint64_t value) {
    if (encoder_ != EncoderKind::none) throw std::runtime_error("Metal encodeWaitForEvent: while an encoder is active");
    if (!event) throw std::runtime_error("Metal encodeWaitForEvent: with a nil event");
    close_recording();
    if (!batches_.back().command_buffers.empty() || !batches_.back().signals.empty()) batches_.emplace_back();
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = event->semaphore;
    wait.value = value;
    wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    batches_.back().waits.push_back(wait);
    events_.push_back(event);
}

void CommandBuffer::enqueue() {
    std::lock_guard lock(status_mutex_);
    if (status_ == CommandBufferStatus::not_enqueued) status_ = CommandBufferStatus::enqueued;
}

void CommandBuffer::commit() {
    if (value_) throw std::runtime_error("Metal command buffer committed twice");
    perf::add(perf::mtl_commits);
    if (encoder_ != EncoderKind::none) end_encoding();
    const auto command = recording();
    // g3: IOSurface-backed textures written since the last signal are copied
    // into their surfaces' pages (before the host barrier below). Command
    // buffers without such textures (Blender's) skip all of it.
    if (!surface_writes_.empty()) flush_surface_tails(command);
    // Everything the GPU wrote is visible to the CPU after completion
    // (shared/managed contents, synchronizeResource:).
    VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    host.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_HOST_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &host;
    device->vk.vkCmdPipelineBarrier2(command, &dependency);
    close_recording();
    const bool surfaces = !surface_uses_.empty();
    const bool first_uses = surfaces && std::any_of(surface_uses_.begin(), surface_uses_.end(),
                                                    [](const SurfaceUse& use) { return use.batch == 0; });
    if (!head_barriers_.empty() || first_uses) {
        // External images first bound inside a render pass: transitioned
        // ahead of everything this command buffer records; g3: IOSurface-
        // backed textures used in the first batch brought up to date.
        const auto head = recording();
        const bool waited = !batches_.front().waits.empty();
        for (const auto& use : surface_uses_)
            if (use.batch == 0)
                if (auto work = use.backing->record_head(head, waited, use.writes_before)) on_complete(std::move(work));
        if (!head_barriers_.empty()) {
            VkDependencyInfo transitions{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            transitions.imageMemoryBarrierCount = static_cast<std::uint32_t>(head_barriers_.size());
            transitions.pImageMemoryBarriers = head_barriers_.data();
            device->vk.vkCmdPipelineBarrier2(head, &transitions);
        }
        check(device->vk.vkEndCommandBuffer(head), "vkEndCommandBuffer");
        current_ = VK_NULL_HANDLE;
        auto& first = batches_.front().command_buffers;
        first.insert(first.begin(), head);
        head_barriers_.clear();
    }
    // g3: IOSurface-backed textures brought up to date with their pages at
    // the start of every later batch that uses them too, after the batch's
    // event waits (after any wait unconditionally). In batch order, after
    // batch 0's: a backing a batch brought up to date needs nothing in later
    // ones unless they waited.
    if (surfaces && batches_.size() > 1) {
        bool waited = !batches_.front().waits.empty();
        for (std::size_t batch = 1; batch < batches_.size(); ++batch) {
            waited = waited || !batches_[batch].waits.empty();
            VkCommandBuffer head{};
            for (const auto& use : surface_uses_) {
                if (use.batch != batch) continue;
                if (!head) head = recording();
                if (auto work = use.backing->record_head(head, waited, use.writes_before)) on_complete(std::move(work));
            }
            if (!head) continue;
            check(device->vk.vkEndCommandBuffer(head), "vkEndCommandBuffer");
            current_ = VK_NULL_HANDLE;
            auto& buffers = batches_[batch].command_buffers;
            buffers.insert(buffers.begin(), head);
        }
    }
    if (surfaces) surface_uses_.clear();
    {
        std::lock_guard lock(status_mutex_);
        status_ = CommandBufferStatus::committed;
    }
    std::uint64_t value{};
    try {
        value = device->submit(batches_);
    } catch (const std::exception& failure) {
        std::lock_guard lock(status_mutex_);
        status_ = CommandBufferStatus::error;
        error_ = failure.what();
        final_status_ = true;
        status_changed_.notify_all();
        throw;
    }
    {
        // value_ is read by wait() and status() on other threads.
        std::lock_guard lock(status_mutex_);
        value_ = value;
        status_ = CommandBufferStatus::scheduled;
    }
    // The deferred step does not own the command buffer: if the guest drops
    // it first, the destructor hands its resources to the same step.
    std::weak_ptr<CommandBuffer> weak = shared_from_this();
    device->defer([weak] { if (const auto self = weak.lock()) self->finalize(); });
}

void CommandBuffer::release_resources() {
    for (auto& work : completions_) work();
    completions_.clear();
    device->recycle_commands(std::move(commands_));
    for (const auto pool : descriptor_pools_) device->recycle_descriptor_pool(pool);
    descriptor_pools_.clear();
    auto& pool = upload_pool(*device);
    for (auto& chunk : chunks_) pool.release({chunk.buffer, chunk.memory, chunk.address, chunk.size});
    chunks_.clear();
    retained_.clear();
    events_.clear();
    for (const auto view : foreign_views_) device->vk.vkDestroyImageView(device->device, view, nullptr);
    foreign_views_.clear();
    foreign_view_cache_.clear();
    render_stages_ = {};
    compute_stage_ = {};
}

void CommandBuffer::finalize() {
    {
        std::unique_lock lock(status_mutex_);
        if (finalized_) {
            // Another thread (e.g. the completion thread) is finalizing: wait
            // until it publishes the final status, so waitUntilCompleted and
            // status never observe "scheduled" after the GPU finished.
            if (finalizer_ != std::this_thread::get_id())
                status_changed_.wait(lock, [this] { return final_status_; });
            return;
        }
        finalized_ = true;
        finalizer_ = std::this_thread::get_id();
    }
    std::string failure;
    try {
        release_resources();
    } catch (const std::exception& error) {
        failure = error.what();
    } catch (...) {   // e.g. a thread unwind: publish, then keep unwinding
        {
            std::lock_guard lock(status_mutex_);
            status_ = CommandBufferStatus::error;
            error_ = "command buffer finalization was interrupted";
            final_status_ = true;
            status_changed_.notify_all();
        }
        throw;
    }
    std::lock_guard lock(status_mutex_);
    if (!failure.empty()) {
        status_ = CommandBufferStatus::error;
        error_ = std::move(failure);
    } else if (status_ != CommandBufferStatus::error) {
        status_ = CommandBufferStatus::completed;
    }
    final_status_ = true;
    status_changed_.notify_all();
}

CommandBufferStatus CommandBuffer::status() {
    {
        std::lock_guard lock(status_mutex_);
        if (status_ != CommandBufferStatus::scheduled) return status_;
    }
    try {
        if (device->completed_value() >= value_) finalize();
    } catch (const std::exception& failure) {
        std::lock_guard lock(status_mutex_);
        status_ = CommandBufferStatus::error;
        error_ = failure.what();
    }
    std::lock_guard lock(status_mutex_);
    return status_;
}

bool CommandBuffer::wait(std::uint64_t timeout_ns) {
    {
        std::lock_guard lock(status_mutex_);
        if (status_ == CommandBufferStatus::completed || status_ == CommandBufferStatus::error) return true;
        if (!value_) return false;
    }
    try {
        if (!device->wait(value_, timeout_ns)) return false;
    } catch (const std::exception& failure) {
        std::lock_guard lock(status_mutex_);
        status_ = CommandBufferStatus::error;
        error_ = failure.what();
        return true;
    }
    finalize();
    return true;
}

} // namespace alchemy::metal
