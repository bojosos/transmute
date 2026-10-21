#pragma once

// Metal on Vulkan: the host device (owner: A5).
//
// One Device wraps one VkInstance / VkPhysicalDevice / VkDevice and the
// single graphics+compute queue every MTLCommandQueue submits to (Metal
// orders command buffers per queue; one Vulkan queue in commit order is a
// valid, stricter order). It owns:
//   - the Metal capability profile (DeviceProfile) derived from Vulkan
//     features and limits - every MTLDevice query answers from it;
//   - device memory (block sub-allocator per memory type and resource kind;
//     host-visible blocks stay mapped);
//   - the submission timeline: one timeline semaphore, one value per
//     submission, deferred destruction until the GPU passes the value;
//   - recycled per-command-buffer resources (command pools, descriptor
//     pools, upload chunks);
//   - the persistent VkPipelineCache (ALCHEMY_METAL_PIPELINE_CACHE or
//     %LOCALAPPDATA%/Alchemy/metal-pipeline-cache.bin).
//
// Pure host code: no guest memory, engine or Objective-C access, so the
// headless tests drive it directly. Thread-safe unless noted.

#include "alchemy/darwin/metal/vulkan.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace alchemy::metal {

// MTLGPUFamily values.
namespace gpu_family {
inline constexpr std::int64_t apple1 = 1001, apple7 = 1007, apple9 = 1009, mac1 = 2001, mac2 = 2002, common1 = 3001,
    common2 = 3002, common3 = 3003, mac_catalyst1 = 4001, mac_catalyst2 = 4002, metal3 = 5001;
} // namespace gpu_family

// What the MTLDevice reports. Every answer is backed by a Vulkan feature or
// limit of the physical device (see Device::create); nothing is claimed that
// the translation cannot deliver.
struct DeviceProfile {
    std::string name;
    std::uint64_t registry_id{};
    bool unified_memory{}, low_power{}, removable{}, headless{};
    std::uint64_t recommended_working_set{}, max_buffer_length{};
    std::uint32_t max_threads_per_threadgroup[3]{};
    std::uint32_t max_total_threads_per_threadgroup{};
    std::uint32_t max_threadgroup_memory{};
    std::uint32_t thread_execution_width{32};
    std::uint32_t argument_buffers_tier{};          // MTLArgumentBuffersTier1 = 0, Tier2 = 1
    std::uint32_t read_write_texture_tier{};        // MTLReadWriteTextureTierNone/1/2
    bool barycentric_coordinates{}, bc_compression{}, float32_filtering{}, float32_blending{},
        depth24_stencil8{}, query_texture_lod{true}, shader_draw_parameters{};
    std::uint32_t texture_sample_counts{};          // VkSampleCountFlags valid for color and depth targets
    std::uint32_t max_texture_2d{}, max_texture_3d{}, max_texture_cube{}, max_texture_layers{};
    std::uint32_t max_color_attachments{};
    std::uint32_t max_vertex_attributes{}, max_vertex_bindings{};
    std::uint32_t min_uniform_offset_alignment{}, min_texel_buffer_alignment{};
    std::vector<std::int64_t> families;             // supportsFamily: YES set
    // The timestamp counter set (MTLCommonCounterSetTimestamp): Vulkan
    // timestamps on the graphics+compute queue, in ticks of
    // timestamp_period nanoseconds.
    bool timestamps{};
    double timestamp_period{1};
    [[nodiscard]] bool supports_family(std::int64_t family) const;
};

enum class MemoryKind : std::uint8_t { device_local, host_visible, host_cached };

struct Allocation {
    VkDeviceMemory memory{};
    VkDeviceSize offset{}, size{};
    std::uint8_t* mapped{};                         // host pointer of `offset` for host-visible memory
    std::uint32_t type{};
    std::uint32_t block{UINT32_MAX};                // UINT32_MAX: dedicated allocation
    bool image{};
    [[nodiscard]] explicit operator bool() const noexcept { return memory != VK_NULL_HANDLE; }
};

struct DeviceOptions {
    bool validation{};                              // enable VK_LAYER_KHRONOS_validation when present
    bool presentation{true};                        // enable surface/swapchain extensions when present
    std::optional<std::filesystem::path> pipeline_cache;   // overrides the default path; empty path = no disk cache
};

// A recycled bundle for one Metal command buffer (one Vulkan submission).
struct CommandContext {
    VkCommandPool pool{};
    std::vector<VkCommandBuffer> buffers;           // allocated from pool, reused in order
    std::size_t used{};
};

class Device : public std::enable_shared_from_this<Device> {
public:
    // Creates the instance and device. Throws VulkanError when no Vulkan 1.3
    // device with the required features exists (the message names what is
    // missing). Never called at install time: MTLCreateSystemDefaultDevice
    // calls it on first use.
    static std::shared_ptr<Device> create(const DeviceOptions& options = {});
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    const VulkanFunctions vk;   // instance + device entry points
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    std::uint32_t queue_family{};
    std::uint32_t api_version{};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkPhysicalDeviceVulkan12Features features12{};
    VkPhysicalDeviceVulkan13Features features13{};
    VkPhysicalDeviceFeatures features10{};
    // VK_KHR_dynamic_rendering_local_read: Metal framebuffer fetch
    // ([[color(n)]] fragment inputs read as input attachments).
    bool dynamic_rendering_local_read{};
    std::uint32_t timestamp_valid_bits{};           // of the queue family
    // The GPU's timestamp counter now, in the ticks timestamp queries write
    // (VK_KHR/EXT_calibrated_timestamps); nullopt without the extension.
    [[nodiscard]] std::optional<std::uint64_t> gpu_timestamp() const;
    std::vector<std::string> instance_extensions, device_extensions;   // enabled
    [[nodiscard]] bool has_device_extension(std::string_view name) const;
    [[nodiscard]] bool has_instance_extension(std::string_view name) const;
    [[nodiscard]] const DeviceProfile& profile() const noexcept { return profile_; }

    // ---- formats ---------------------------------------------------------------
    [[nodiscard]] VkFormatFeatureFlags format_features(VkFormat format, bool optimal = true) const;
    // The VkFormat textures of `metal` use on this device (Depth24Unorm_Stencil8
    // falls back to D32_SFLOAT_S8_UINT, Stencil8 to a combined format when
    // S8_UINT is unsupported). VK_FORMAT_UNDEFINED when untranslatable.
    [[nodiscard]] VkFormat texture_format(std::uint32_t metal) const;

    // ---- memory ----------------------------------------------------------------
    [[nodiscard]] Allocation allocate(const VkMemoryRequirements& requirements, MemoryKind kind, bool image);
    void free(Allocation& allocation);
    [[nodiscard]] std::uint64_t allocated_bytes() const noexcept { return allocated_.load(); }

    // ---- submission ------------------------------------------------------------
    // The submission timeline. Every submit() signals the next value.
    VkSemaphore timeline{};
    [[nodiscard]] std::uint64_t completed_value() const;
    [[nodiscard]] std::uint64_t submitted_value() const noexcept { return submitted_.load(); }
    // Waits until completed_value() >= value or `timeout_ns` passes; true when
    // reached. Throws VulkanError on device loss.
    bool wait(std::uint64_t value, std::uint64_t timeout_ns) const;
    // Submits batches in order; the last batch also signals the timeline with
    // the returned value. `prepare` runs under the queue lock right before
    // vkQueueSubmit2 with the value about to be signalled.
    struct Batch {
        std::vector<VkCommandBuffer> command_buffers;
        std::vector<VkSemaphoreSubmitInfo> waits, signals;
    };
    std::uint64_t submit(std::vector<Batch>& batches);
    // The queue is externally synchronized: presentation (QuartzCore) and
    // submit() take this lock.
    [[nodiscard]] std::mutex& queue_mutex() noexcept { return queue_mutex_; }
    // Runs `record` in a one-shot command buffer, submits it and waits.
    void immediate(const std::function<void(VkCommandBuffer)>& record);
    // New images start in UNDEFINED; the next submission (of any command
    // buffer) first moves every queued image to VK_IMAGE_LAYOUT_GENERAL, so a
    // texture is in GENERAL before any GPU work can reference it.
    void queue_initial_layout(VkImage image, VkImageAspectFlags aspects);
    void cancel_initial_layout(VkImage image);   // the image is destroyed before any submission
    // Destroys `destroy` once the GPU completed everything submitted so far.
    void defer(std::function<void()> destroy);
    // Runs due deferred work (called by submit and the completion paths).
    void collect();

    // ---- recycled per-submission state ---------------------------------------
    [[nodiscard]] CommandContext acquire_commands();
    void recycle_commands(CommandContext context);   // after the GPU finished it
    [[nodiscard]] VkDescriptorPool acquire_descriptor_pool();
    void recycle_descriptor_pool(VkDescriptorPool pool);   // after the GPU finished with it

    // ---- pipeline cache --------------------------------------------------------
    VkPipelineCache pipeline_cache{};
    void save_pipeline_cache();
    // A pipeline was created: the next submission at least 2 s after the last
    // creation saves the cache (a process that is killed, as GUI harnesses
    // do, never reaches ~Device).
    void note_pipeline_cache_dirty() noexcept;
    [[nodiscard]] const std::filesystem::path& pipeline_cache_path() const noexcept { return cache_path_; }

    // Debug names (VK_EXT_debug_utils when enabled; otherwise nothing).
    void set_name(VkObjectType type, std::uint64_t handle, const std::string& name) const;

    // Per-device state of other Metal layers (upload chunk pools, fallback
    // descriptors), created once per key. Extensions hold the Device by raw
    // pointer (never a shared_ptr: no cycles); `cleanup` runs in ~Device
    // after the GPU is idle and before the VkDevice is destroyed.
    template <class T, class Make>
    T& extension(std::string_view key, Make&& make) {
        std::lock_guard lock(extension_mutex_);
        auto& slot = extensions_[std::string(key)];
        if (!slot) {
            auto value = make();
            slot = value;
            cleanups_.push_back([value] { value->cleanup(); });
        }
        return *static_cast<T*>(slot.get());
    }

private:
    struct Block {
        VkDeviceMemory memory{};
        VkDeviceSize size{};
        std::uint8_t* mapped{};
        std::uint32_t type{};
        bool image{};
        std::map<VkDeviceSize, VkDeviceSize> free;   // offset -> size
    };
    struct Deferred { std::uint64_t value; std::function<void()> destroy; };
    explicit Device(VulkanFunctions table);
    void initialize(const DeviceOptions& options);
    void build_profile();
    [[nodiscard]] std::uint32_t memory_type(std::uint32_t bits, MemoryKind kind) const;

    DeviceProfile profile_;
    VkDebugUtilsMessengerEXT messenger_{};
    mutable std::mutex memory_mutex_;
    std::vector<Block> blocks_;
    std::atomic<std::uint64_t> allocated_{0};
    std::mutex queue_mutex_;
    std::atomic<std::uint64_t> submitted_{0};
    std::mutex layout_mutex_;
    std::map<VkImage, VkImageAspectFlags> initial_layouts_;
    std::mutex deferred_mutex_;
    std::deque<Deferred> deferred_;
    std::mutex recycle_mutex_;
    std::vector<CommandContext> free_commands_;
    std::vector<VkDescriptorPool> free_descriptor_pools_;
    std::filesystem::path cache_path_;
    std::size_t cache_saved_size_{};
    std::atomic<std::int64_t> cache_dirty_ms_{0};   // steady ms of the last unsaved creation, 0 = saved
    std::mutex extension_mutex_;
    std::map<std::string, std::shared_ptr<void>> extensions_;
    std::vector<std::function<void()>> cleanups_;
};

// The physical-device rule of Device::create, shared with gpu_identities()
// (gpu_identity.hpp): -1 when the device cannot be the Metal device (below
// Vulkan 1.3, or excluded by `selection`, ALCHEMY_METAL_DEVICE's index or
// name substring); otherwise the highest score wins (discrete 4, integrated
// 3, virtual or other 1, CPU 0), the first of equal scores.
[[nodiscard]] int physical_device_score(const VkPhysicalDeviceProperties& properties, std::size_t index, std::string_view selection);
[[nodiscard]] std::string metal_device_selection();
// MTLDevice.name (ALCHEMY_METAL_DEVICE_NAME overrides it for diagnostics).
[[nodiscard]] std::string metal_device_name(const VkPhysicalDeviceProperties& properties);
// MTLDevice.registryID of a physical device.
[[nodiscard]] std::uint64_t physical_device_registry_id(const VkPhysicalDeviceProperties& properties,
                                                        const VkPhysicalDeviceIDProperties& id);

// MTLCreateSystemDefaultDevice's device: created on first call, shared by
// every later call. Throws VulkanError when no usable device exists (the
// Objective-C layer turns that into nil and reports the reason once).
[[nodiscard]] std::shared_ptr<Device> system_device();
// The reason system_device() failed, empty when it succeeded or was not tried.
[[nodiscard]] std::string system_device_error();

} // namespace alchemy::metal
