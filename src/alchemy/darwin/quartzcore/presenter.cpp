#include "alchemy/darwin/quartzcore/presenter.hpp"

#include "alchemy/perf.hpp"
#include "alchemy/fx_profile_window.hpp"
#include "alchemy/host/gpu.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/ui.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <thread>

// CAMetalLayer presentation engine; see presenter.hpp for the model.
namespace alchemy::gui::quartzcore {

std::optional<std::string> layer_pixel_format_problem(std::uint64_t format) {
    using namespace pixel_format;
    switch (format) {
    case bgra8_unorm: case bgra8_unorm_srgb: case rgba16_float: case rgb10a2_unorm: case bgr10a2_unorm:
        return std::nullopt;
    case bgr10_xr: case bgr10_xr_srgb: case bgra10_xr: case bgra10_xr_srgb:
        return "extended-range XR pixel format " + std::to_string(format) + " is unsupported";
    default:
        return "MTLPixelFormat " + std::to_string(format) + " is not a valid CAMetalLayer pixel format";
    }
}

namespace {
std::atomic<std::uint64_t> total_presented{0}, total_skipped{0};

std::filesystem::path environment_path(const wchar_t* wide, const char* narrow) {
#ifdef _MSC_VER
    static_cast<void>(narrow);
    wchar_t* buffer = nullptr;
    std::size_t length = 0;
    std::filesystem::path value;
    if (_wdupenv_s(&buffer, &length, wide) == 0 && buffer) {
        value = buffer;
        std::free(buffer);
    }
    return value;
#else
    static_cast<void>(wide);
    const char* value = std::getenv(narrow);
    return value ? std::filesystem::path(value) : std::filesystem::path{};
#endif
}

// What the present watcher needs from a presenter (Presenter::Impl).
class CaptureSource {
public:
    virtual ~CaptureSource() = default;
    // Writes the last image presented to `window` (0: any window) to `path`
    // (BMP, or <path>.error).
    virtual void serve(std::uintptr_t window, const std::filesystem::path& path) = 0;
    // True when serve(window) has an image (or a reason to fail) to write.
    [[nodiscard]] virtual bool can_serve(std::uintptr_t window) = 0;
    // Process-wide order of this presenter's last present (0: none yet).
    [[nodiscard]] virtual std::uint64_t last_sequence() const = 0;
    struct Shown {
        std::uintptr_t window{};
        std::uint64_t presented{};
        std::uint32_t width{}, height{};
    };
    [[nodiscard]] virtual Shown shown() const = 0;
};

// The watcher thread's state; leaked so that it outlives static destruction
// (the thread is detached and runs until the process ends).
struct Services {
    std::mutex sources_mutex;
    std::vector<std::weak_ptr<CaptureSource>> sources;
    std::mutex stats_mutex;
    bool final_written{};
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
};
Services& services() {
    static auto* value = new Services;
    return *value;
}
std::atomic<std::uint64_t> present_sequence{0};

void register_source(const std::shared_ptr<CaptureSource>& source) {
    auto& state = services();
    std::lock_guard lock(state.sources_mutex);
    std::erase_if(state.sources, [](const std::weak_ptr<CaptureSource>& value) { return value.expired(); });
    state.sources.push_back(source);
}
std::vector<std::shared_ptr<CaptureSource>> live_sources() {
    auto& state = services();
    std::lock_guard lock(state.sources_mutex);
    std::vector<std::shared_ptr<CaptureSource>> result;
    for (const auto& weak : state.sources)
        if (auto source = weak.lock()) result.push_back(std::move(source));
    return result;
}

// <directory>/request: "<hwnd> <output path>". Served from the last present
// of that window; left in place while no presenter can serve it.
void poll_capture_request(const std::filesystem::path& directory) {
    const auto request = directory / "request";
    std::error_code error;
    if (!std::filesystem::exists(request, error)) return;
    unsigned long long target{};
    std::string path;
    {
        std::ifstream in(request);
        if (!(in >> target) || !std::getline(in >> std::ws, path) || path.empty()) return;   // still being written
    }
    while (!path.empty() && (path.back() == '\r' || path.back() == '\n' || path.back() == ' ')) path.pop_back();
    const auto window = static_cast<std::uintptr_t>(target);
    // Any window: the presenter that presented most recently.
    std::shared_ptr<CaptureSource> chosen;
    for (const auto& source : live_sources()) {
        if (!source->can_serve(window)) continue;
        if (!chosen || source->last_sequence() > chosen->last_sequence()) chosen = source;
    }
    if (!chosen) return;   // nothing presented to that window yet: stays pending
    // Consume the request before writing: the harness may write the next one
    // as soon as the image exists.
    std::filesystem::remove(request, error);
    chosen->serve(window, std::filesystem::path(std::u8string(path.begin(), path.end())));   // UTF-8 (the harness)
}

std::string stats_json(bool final) {
    const auto totals = present_totals();
    const auto uptime = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - services().start).count();
    const unsigned long pid = host::process::current_pid();
    std::string text = "{\"pid\":" + std::to_string(pid) + ",\"presented\":" + std::to_string(totals.presented) +
        ",\"skipped\":" + std::to_string(totals.skipped) + ",\"uptime_ms\":" + std::to_string(uptime) +
        ",\"final\":" + (final ? "true" : "false") + ",\"windows\":[";
    bool first = true;
    for (const auto& source : live_sources()) {
        const auto shown = source->shown();
        if (!shown.window) continue;
        if (!first) text += ',';
        first = false;
        text += "{\"window\":" + std::to_string(shown.window) + ",\"presented\":" + std::to_string(shown.presented) +
            ",\"width\":" + std::to_string(shown.width) + ",\"height\":" + std::to_string(shown.height) + "}";
    }
    return text + "]}\n";
}

// Atomic rewrite (temp + rename). A reader holding the file open can make
// the rename fail: the watcher retries on its next tick, the final write a
// few times.
bool write_stats(bool final) {
    const auto& path = present_stats_path();
    if (path.empty()) return true;
    auto& state = services();
    std::lock_guard lock(state.stats_mutex);
    if (state.final_written) return true;
    const auto text = stats_json(final);
    auto temporary = path;
    temporary += ".tmp";
    for (int attempt = 0; attempt < (final ? 20 : 1); ++attempt) {
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out << text;
            if (!out) return false;
        }
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (!error) {
            if (final) state.final_written = true;
            return true;
        }
        if (final) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

void at_exit_totals() {
    static_cast<void>(write_stats(true));
    if (present_trace_enabled()) {
        const auto totals = present_totals();
        std::fprintf(stderr, "[alchemy present] totals: presented %llu skipped %llu\n",
                     static_cast<unsigned long long>(totals.presented), static_cast<unsigned long long>(totals.skipped));
        std::fflush(stderr);
    }
}

void watcher() {
    const auto& directory = present_capture_directory();
    const bool stats = !present_stats_path().empty();
    std::uint64_t written_presented = ~0ULL, written_skipped = ~0ULL;
    auto last_write = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    for (;;) {
        try {
            if (!directory.empty()) poll_capture_request(directory);
            if (stats) {
                const auto now = std::chrono::steady_clock::now();
                const auto totals = present_totals();
                const bool changed = totals.presented != written_presented || totals.skipped != written_skipped;
                if ((changed && now - last_write >= std::chrono::milliseconds(100)) || now - last_write >= std::chrono::seconds(1)) {
                    if (write_stats(false)) {
                        written_presented = totals.presented;
                        written_skipped = totals.skipped;
                        last_write = now;
                    }
                }
            }
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[alchemy present] watcher: %s\n", error.what());
            std::fflush(stderr);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
} // namespace

PresentTotals present_totals() { return {total_presented.load(), total_skipped.load()}; }

bool present_trace_enabled() {
    static const bool enabled = [] {
        const auto value = environment_path(L"ALCHEMY_TRACE_PRESENT", "ALCHEMY_TRACE_PRESENT").string();
        return !value.empty() && value != "0";
    }();
    return enabled;
}

const std::filesystem::path& present_capture_directory() {
    static const std::filesystem::path directory = environment_path(L"ALCHEMY_PRESENT_CAPTURE", "ALCHEMY_PRESENT_CAPTURE");
    return directory;
}

const std::filesystem::path& present_stats_path() {
    static const std::filesystem::path path = environment_path(L"ALCHEMY_PRESENT_STATS", "ALCHEMY_PRESENT_STATS");
    return path;
}

void start_present_services() {
    static std::once_flag once;
    std::call_once(once, [] {
        const bool watch = !present_capture_directory().empty() || !present_stats_path().empty();
        if (!watch && !present_trace_enabled()) return;
        static_cast<void>(services());
        if (!present_stats_path().empty() || present_trace_enabled()) std::atexit(at_exit_totals);
        if (watch) std::thread(watcher).detach();
    });
}

#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto wait_slice = std::chrono::milliseconds(100);
constexpr std::uint64_t slice_nanoseconds = 100'000'000;
constexpr std::size_t frame_slots = 3;

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("Alchemy QuartzCore: " + what); }
void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) fail(std::string(what) + " failed (VkResult " + std::to_string(result) + ")");
}

VkFormat vulkan_format(std::uint64_t format) {
    switch (format) {
    case pixel_format::bgra8_unorm: return VK_FORMAT_B8G8R8A8_UNORM;
    case pixel_format::bgra8_unorm_srgb: return VK_FORMAT_B8G8R8A8_SRGB;
    case pixel_format::rgba16_float: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case pixel_format::rgb10a2_unorm: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case pixel_format::bgr10a2_unorm: return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    default: break;
    }
    if (const auto problem = layer_pixel_format_problem(format)) fail(*problem);
    fail("MTLPixelFormat " + std::to_string(format) + " has no Vulkan format");
}

// Vulkan entry points QuartzCore uses, resolved through the bridge's loader.
#define QC_INSTANCE_FUNCTIONS(X) \
    X(vkGetDeviceProcAddr) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkDestroySurfaceKHR)
#define QC_DEVICE_FUNCTIONS(X) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) \
    X(vkQueuePresentKHR) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdBlitImage) \
    X(vkCmdClearColorImage) X(vkCmdCopyImageToBuffer) X(vkQueueSubmit) X(vkQueueWaitIdle) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkCreateSemaphore) X(vkDestroySemaphore)

struct Vk {
    std::shared_ptr<MetalVulkanDevice> bridge;
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    std::uint32_t family{};
    PFN_vkGetInstanceProcAddr get_instance{};   // the window surface is created through it (host::gpu)
#define QC_DECLARE(name) PFN_##name name{};
    QC_INSTANCE_FUNCTIONS(QC_DECLARE)
    QC_DEVICE_FUNCTIONS(QC_DECLARE)
#undef QC_DECLARE
    VkPhysicalDeviceMemoryProperties memory{};

    explicit Vk(std::shared_ptr<MetalVulkanDevice> source) : bridge(std::move(source)) {
        if (!bridge) fail("no Metal device bridge");
        const auto get = bridge->get_instance_proc_addr();
        instance = bridge->instance(); physical = bridge->physical_device(); device = bridge->device();
        family = bridge->queue_family();
        if (!get || !instance || !physical || !device) fail("the Metal device bridge has no Vulkan device");
        get_instance = get;
#define QC_LOAD_INSTANCE(name) name = reinterpret_cast<PFN_##name>(get(instance, #name));
        QC_INSTANCE_FUNCTIONS(QC_LOAD_INSTANCE)
#undef QC_LOAD_INSTANCE
        if (!vkGetDeviceProcAddr || !vkGetPhysicalDeviceMemoryProperties || !vkGetPhysicalDeviceFormatProperties)
            fail("the Vulkan loader lacks core instance functions");
#define QC_LOAD_DEVICE(name) \
        name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name)); \
        if (!name) fail("Vulkan device function " #name " is unavailable (VK_KHR_swapchain not enabled?)");
        QC_DEVICE_FUNCTIONS(QC_LOAD_DEVICE)
#undef QC_LOAD_DEVICE
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    }
    std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags wanted) const {
        for (std::uint32_t index = 0; index < memory.memoryTypeCount; ++index)
            if ((bits & (1U << index)) && (memory.memoryTypes[index].propertyFlags & wanted) == wanted) return index;
        fail("no Vulkan memory type with properties " + std::to_string(wanted));
    }
    VkFormatFeatureFlags features(VkFormat format) const {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
        return properties.optimalTilingFeatures;
    }
};

struct Fence {
    std::shared_ptr<Vk> vk;
    VkFence fence{};
    explicit Fence(std::shared_ptr<Vk> owner) : vk(std::move(owner)) {
        VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vk->vkCreateFence(vk->device, &info, nullptr, &fence), "vkCreateFence");
    }
    ~Fence() { vk->vkDestroyFence(vk->device, fence, nullptr); }
    Fence(const Fence&) = delete;
    Fence& operator=(const Fence&) = delete;
    // True when signalled within `nanoseconds`.
    bool wait(std::uint64_t nanoseconds) const {
        const auto result = vk->vkWaitForFences(vk->device, 1, &fence, VK_TRUE, nanoseconds);
        if (result == VK_TIMEOUT) return false;
        check(result, "vkWaitForFences");
        return true;
    }
};

struct PoolImage final : DrawableImage {
    std::shared_ptr<Vk> vk;
    VkDeviceMemory memory{};
    DrawableConfig config;
    std::shared_ptr<Fence> last_blit;   // guarded by Presenter::Impl::pool_mutex
    std::uint64_t texture{};            // guest MTLTexture, owned by the presenter
    ~PoolImage() override {
        if (last_blit) static_cast<void>(last_blit->wait(UINT64_MAX));
        if (image) vk->vkDestroyImage(vk->device, image, nullptr);
        if (memory) vk->vkFreeMemory(vk->device, memory, nullptr);
    }
};

// One swapchain per HWND (Win32 surfaces accept a single swapchain).
std::mutex window_mutex;
std::map<void*, const void*> window_owners;

struct Slot {
    VkCommandBuffer commands{};
    VkSemaphore acquired{};
    std::shared_ptr<Fence> fence;
};

// Host-visible copy target of one frame slot's present (capture enabled),
// persistently mapped; grows with the swapchain.
struct StagingBuffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    VkDeviceSize capacity{};
    const std::uint8_t* mapped{};
};

// Top-down 32-bit BMP of BGRA pixels, written atomically (temp + rename).
void write_bmp(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
               const std::vector<std::uint8_t>& bgra) {
    const auto bytes = static_cast<std::uint32_t>(bgra.size());
    const auto put = [](std::ofstream& out, std::uint32_t value, int size) {
        for (int index = 0; index < size; ++index) out.put(static_cast<char>((value >> (8 * index)) & 0xff));
    };
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) fail("cannot write " + temporary.string());
        out.put('B'); out.put('M');
        put(out, 54 + bytes, 4); put(out, 0, 4); put(out, 54, 4);
        put(out, 40, 4); put(out, width, 4);
        put(out, static_cast<std::uint32_t>(-static_cast<std::int32_t>(height)), 4);   // top-down
        put(out, 1, 2); put(out, 32, 2); put(out, 0, 4); put(out, bytes, 4);
        put(out, 2835, 4); put(out, 2835, 4); put(out, 0, 4); put(out, 0, 4);
        out.write(reinterpret_cast<const char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
        if (!out) fail("cannot write " + temporary.string());
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) fail("cannot rename " + temporary.string() + ": " + error.message());
}
void write_capture_failure(const std::filesystem::path& path, const std::string& why) {
    auto target = path;
    target += ".error";
    std::ofstream(target) << "Alchemy QuartzCore: present capture failed: " << why << '\n';
}
} // namespace

struct Presenter::Impl final : CaptureSource {
    std::shared_ptr<Vk> vk;
    PresenterOptions options;

    // Drawable pool.
    std::mutex pool_mutex;
    std::condition_variable released;
    DrawableConfig config;
    std::vector<std::shared_ptr<PoolImage>> free_images, in_flight;
    std::size_t held{};
    std::vector<std::uint64_t> retired_textures;

    // Presentation (present_mutex; lock order present_mutex -> pool_mutex).
    std::mutex present_mutex;
    void* window{};
    VkSurfaceKHR surface{};
    VkSwapchainKHR swapchain{};
    VkFormat swap_format{};
    VkExtent2D extent{};
    VkPresentModeKHR mode{VK_PRESENT_MODE_FIFO_KHR};
    bool swap_linear{}, swap_sync{true}, stale{};
    std::vector<VkImage> swap_images;
    std::vector<VkSemaphore> rendered;   // per swapchain image
    VkCommandPool pool{};
    std::array<Slot, frame_slots> slots{};
    std::size_t next_slot{};
    bool capture_capable{};   // the swapchain images are transfer sources
    PresentStats counters;
    // Watcher-visible summary of the last present (lock-free reads).
    std::atomic<std::uintptr_t> shown_window{0};
    std::atomic<std::uint64_t> shown_presented{0}, shown_sequence{0};
    std::atomic<std::uint32_t> shown_width{0}, shown_height{0};

    // ---- capture (capture_mutex; lock order present_mutex -> capture_mutex) ----
    // Every present with capture enabled copies its swapchain image into the
    // staging buffer of its frame slot; `last` names the newest one. Readers
    // (last_capture, the watcher) hold capture_mutex while they wait for its
    // fence and copy, and present() holds it while it records and submits a
    // copy, so a buffer is never rewritten while it is read.
    std::mutex capture_mutex;
    std::array<StagingBuffer, frame_slots> staging{};
    struct Last {
        std::size_t slot{};
        std::shared_ptr<Fence> fence;   // null: nothing captured (see problem)
        std::uint32_t width{}, height{};
        VkFormat format{};
        std::uintptr_t window{};
        std::uint64_t frame{};          // process-wide presented-frame number
        std::string problem;            // why the last present to `window` was not captured
    } last;
    // ALCHEMY_TRACE_PRESENT: what the last logged frame showed (present_mutex).
    struct Traced {
        std::uint32_t width{}, height{};
        void* window{};
        std::uint64_t swapchains{};
    } traced;

    [[nodiscard]] bool capture_enabled() const { return options.capture || !present_capture_directory().empty(); }

    // capture_mutex held. The staging buffer of `slot`, at least `bytes` big.
    StagingBuffer& staging_for(std::size_t slot, VkDeviceSize bytes) {
        auto& target = staging[slot];
        if (target.capacity >= bytes) return target;
        destroy_staging(target);
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = bytes;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        check(vk->vkCreateBuffer(vk->device, &buffer, nullptr, &target.buffer), "vkCreateBuffer (capture)");
        try {
            VkMemoryRequirements requirements{};
            vk->vkGetBufferMemoryRequirements(vk->device, target.buffer, &requirements);
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocate.allocationSize = requirements.size;
            allocate.memoryTypeIndex = vk->memory_type(requirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            check(vk->vkAllocateMemory(vk->device, &allocate, nullptr, &target.memory), "vkAllocateMemory (capture)");
            check(vk->vkBindBufferMemory(vk->device, target.buffer, target.memory, 0), "vkBindBufferMemory (capture)");
            void* mapped{};
            check(vk->vkMapMemory(vk->device, target.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory (capture)");
            target.mapped = static_cast<const std::uint8_t*>(mapped);
            target.capacity = bytes;
        } catch (...) {
            destroy_staging(target);
            throw;
        }
        return target;
    }
    void destroy_staging(StagingBuffer& target) {
        if (target.mapped) vk->vkUnmapMemory(vk->device, target.memory);
        if (target.buffer) vk->vkDestroyBuffer(vk->device, target.buffer, nullptr);
        if (target.memory) vk->vkFreeMemory(vk->device, target.memory, nullptr);
        target = {};
    }

    // capture_mutex held. The last captured image in `bgra` order (BGRA
    // bytes, as a BMP stores them) or RGBA; waits for its copy.
    std::vector<std::uint8_t> read_last(bool bgra) {
        static_cast<void>(last.fence->wait(UINT64_MAX));
        const auto bytes = static_cast<std::size_t>(last.width) * last.height * 4;
        const auto* source = staging[last.slot].mapped;
        if (!source || staging[last.slot].capacity < bytes) fail("the capture staging buffer is gone");
        std::vector<std::uint8_t> pixels(source, source + bytes);
        const bool stored_bgra = last.format == VK_FORMAT_B8G8R8A8_UNORM || last.format == VK_FORMAT_B8G8R8A8_SRGB;
        if (stored_bgra != bgra)
            for (std::size_t offset = 0; offset < bytes; offset += 4) std::swap(pixels[offset], pixels[offset + 2]);
        return pixels;
    }

    // CaptureSource (the watcher thread).
    bool can_serve(std::uintptr_t target) override {
        std::lock_guard lock(capture_mutex);
        if (!last.fence && last.problem.empty()) return false;
        return !target || target == last.window;
    }
    void serve(std::uintptr_t target, const std::filesystem::path& path) override {
        try {
            std::vector<std::uint8_t> pixels;
            std::uint32_t width{}, height{};
            std::uint64_t frame{};
            {
                std::lock_guard lock(capture_mutex);
                if (target && target != last.window) fail("the window's presenter now presents to another window");
                if (!last.fence) fail(last.problem.empty() ? std::string("nothing was presented") : last.problem);
                pixels = read_last(true);
                width = last.width; height = last.height; frame = last.frame;
            }
            write_bmp(path, width, height, pixels);
            if (present_trace_enabled()) {
                std::fprintf(stderr, "[alchemy present] captured frame %llu (%ux%u) to %s\n",
                             static_cast<unsigned long long>(frame), width, height, path.string().c_str());
                std::fflush(stderr);
            }
        } catch (const std::exception& error) {
            write_capture_failure(path, error.what());
        }
    }
    std::uint64_t last_sequence() const override { return shown_sequence.load(); }
    Shown shown() const override {
        return {shown_window.load(), shown_presented.load(), shown_width.load(), shown_height.load()};
    }

    // present_mutex held. `frame`/`skip`: the process-wide totals after this present.
    void trace(const PoolImage& image, bool shown, std::uint64_t frame, std::uint64_t skip) {
        if (!shown) {
            if (skip == 1 || skip % 60 == 0) {
                std::fprintf(stderr, "[alchemy present] skipped %llu (no presentable window) after %llu frames\n",
                             static_cast<unsigned long long>(skip), static_cast<unsigned long long>(frame));
                std::fflush(stderr);
            }
            return;
        }
        const bool changed = traced.width != image.extent.width || traced.height != image.extent.height ||
            traced.window != window || traced.swapchains != counters.swapchains_created;
        if (!(frame <= 3 || frame % 60 == 0 || changed)) return;
        traced = {image.extent.width, image.extent.height, window, counters.swapchains_created};
        std::fprintf(stderr, "[alchemy present] frame %llu: drawable %ux%u -> window 0x%llx swapchain %ux%u #%llu\n",
                     static_cast<unsigned long long>(frame), image.extent.width, image.extent.height,
                     static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(window)), extent.width,
                     extent.height, static_cast<unsigned long long>(counters.swapchains_created));
        std::fflush(stderr);
    }

    explicit Impl(std::shared_ptr<MetalVulkanDevice> device, PresenterOptions presenter_options)
        : vk(std::make_shared<Vk>(std::move(device))), options(presenter_options) {
        VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        info.queueFamilyIndex = vk->family;
        check(vk->vkCreateCommandPool(vk->device, &info, nullptr, &pool), "vkCreateCommandPool");
        try {
            VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocate.commandPool = pool;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            for (auto& slot : slots) {
                check(vk->vkAllocateCommandBuffers(vk->device, &allocate, &slot.commands), "vkAllocateCommandBuffers");
                check(vk->vkCreateSemaphore(vk->device, &semaphore, nullptr, &slot.acquired), "vkCreateSemaphore");
            }
        } catch (...) { destroy_frames(); throw; }
    }
    ~Impl() {
        try {
            wait_frames();
            destroy_swapchain(true);
        } catch (...) {}
        destroy_frames();
    }
    void destroy_frames() {
        {
            std::lock_guard lock(capture_mutex);
            if (last.fence) static_cast<void>(last.fence->wait(UINT64_MAX));
            last = {};
            for (auto& buffer : staging) destroy_staging(buffer);
        }
        for (auto& slot : slots) {
            if (slot.acquired) vk->vkDestroySemaphore(vk->device, slot.acquired, nullptr);
            slot = {};
        }
        if (pool) vk->vkDestroyCommandPool(vk->device, pool, nullptr);   // frees the command buffers
        pool = {};
    }
    void wait_frames() {
        for (auto& slot : slots) if (slot.fence) static_cast<void>(slot.fence->wait(UINT64_MAX));
    }

    // ---- pool -----------------------------------------------------------------
    // Moves completed blits back to the free list. pool_mutex held.
    void reap() {
        for (auto it = in_flight.begin(); it != in_flight.end();) {
            const auto& fence = (*it)->last_blit;
            if (fence && !fence->wait(0)) { ++it; continue; }
            give_back(*it);
            it = in_flight.erase(it);
        }
    }
    // An image leaves use: reuse it if it still matches, else retire it.
    void give_back(const std::shared_ptr<PoolImage>& image) {
        if (image->config == config) free_images.push_back(image);
        else retire(image);
    }
    void retire(const std::shared_ptr<PoolImage>& image) {
        if (image->texture) retired_textures.push_back(std::exchange(image->texture, 0));
    }
    std::shared_ptr<PoolImage> create(const DrawableConfig& wanted) {
        auto image = std::make_shared<PoolImage>();
        image->vk = vk;
        image->config = wanted;
        image->format = vulkan_format(wanted.pixel_format);
        image->pixel_format = wanted.pixel_format;
        image->extent = {wanted.width, wanted.height};
        const auto features = vk->features(image->format);
        if (!(features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) || !(features & VK_FORMAT_FEATURE_BLIT_SRC_BIT))
            fail("the Vulkan device cannot render to or blit from MTLPixelFormat " + std::to_string(wanted.pixel_format));
        image->usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image->texture_usage = texture_usage::render_target;
        if (!wanted.framebuffer_only) {
            image->texture_usage |= texture_usage::shader_read;
            if (features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) {
                image->usage |= VK_IMAGE_USAGE_STORAGE_BIT;
                image->texture_usage |= texture_usage::shader_write;
            }
        }
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = image->format;
        info.extent = {wanted.width, wanted.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = image->usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vk->vkCreateImage(vk->device, &info, nullptr, &image->image), "vkCreateImage (drawable)");
        VkMemoryRequirements requirements{};
        vk->vkGetImageMemoryRequirements(vk->device, image->image, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = vk->memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vk->vkAllocateMemory(vk->device, &allocate, nullptr, &image->memory), "vkAllocateMemory (drawable)");
        check(vk->vkBindImageMemory(vk->device, image->image, image->memory, 0), "vkBindImageMemory (drawable)");
        return image;
    }

    // ---- surface and swapchain ------------------------------------------------
    void destroy_swapchain(bool with_surface) {
        if (swapchain || !rendered.empty()) {
            // Pending presents still wait on `rendered`.
            vk->bridge->with_queue([&](VkQueue queue) { static_cast<void>(vk->vkQueueWaitIdle(queue)); });
        }
        for (const auto semaphore : rendered) vk->vkDestroySemaphore(vk->device, semaphore, nullptr);
        rendered.clear();
        swap_images.clear();
        if (swapchain) vk->vkDestroySwapchainKHR(vk->device, swapchain, nullptr);
        swapchain = {};
        capture_capable = false;
        // The staging buffers (the last presented image) outlive the swapchain.
        if (with_surface) {
            if (surface) vk->vkDestroySurfaceKHR(vk->instance, surface, nullptr);
            surface = {};
            if (window) {
                std::lock_guard lock(window_mutex);
                if (const auto found = window_owners.find(window); found != window_owners.end() && found->second == this)
                    window_owners.erase(found);
            }
            window = nullptr;
        }
    }
    void attach(void* hwnd) {
        if (window == hwnd && surface) return;
        wait_frames();
        destroy_swapchain(true);
        {
            std::lock_guard lock(window_mutex);
            const auto [found, inserted] = window_owners.emplace(hwnd, this);
            if (!inserted && found->second != this)
                fail("a second CAMetalLayer presents to the same window (one presenting layer per window is supported)");
        }
        window = hwnd;
        const auto extension = std::string(host::gpu::vulkan_surface_extension());
        if (!vk->vkDestroySurfaceKHR || !vk->vkGetPhysicalDeviceSurfaceSupportKHR)
            fail("the Vulkan instance lacks " + extension);
        std::uint64_t created{};
        const auto result = host::gpu::create_vulkan_surface(vk->instance, reinterpret_cast<void*>(vk->get_instance),
                                                             reinterpret_cast<std::uintptr_t>(hwnd), created);
        if (result == VK_ERROR_EXTENSION_NOT_PRESENT) fail("the Vulkan instance lacks " + extension);
        check(static_cast<VkResult>(result), "create_vulkan_surface");
        surface = reinterpret_cast<VkSurfaceKHR>(created);
        VkBool32 supported{};
        check(vk->vkGetPhysicalDeviceSurfaceSupportKHR(vk->physical, vk->family, surface, &supported),
              "vkGetPhysicalDeviceSurfaceSupportKHR");
        if (!supported) fail("the Metal queue family cannot present to the window surface");
    }
    void create_swapchain(VkExtent2D wanted, bool linear, bool display_sync) {
        wait_frames();
        VkSurfaceCapabilitiesKHR caps{};
        check(vk->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk->physical, surface, &caps),
              "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        std::uint32_t count{};
        check(vk->vkGetPhysicalDeviceSurfaceFormatsKHR(vk->physical, surface, &count, nullptr), "vkGetPhysicalDeviceSurfaceFormatsKHR");
        std::vector<VkSurfaceFormatKHR> formats(count);
        check(vk->vkGetPhysicalDeviceSurfaceFormatsKHR(vk->physical, surface, &count, formats.data()), "vkGetPhysicalDeviceSurfaceFormatsKHR");
        if (formats.empty()) fail("the window surface reports no formats");
        const std::array<VkFormat, 2> preferred = linear
            ? std::array{VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB}
            : std::array{VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
        auto chosen = formats.front();
        for (const auto format : preferred) {
            const auto found = std::find_if(formats.begin(), formats.end(), [&](const VkSurfaceFormatKHR& value) {
                return value.format == format && value.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            });
            if (found != formats.end()) { chosen = *found; break; }
        }
        if (!(vk->features(chosen.format) & VK_FORMAT_FEATURE_BLIT_DST_BIT))
            fail("the window surface format cannot be a blit destination");
        check(vk->vkGetPhysicalDeviceSurfacePresentModesKHR(vk->physical, surface, &count, nullptr), "vkGetPhysicalDeviceSurfacePresentModesKHR");
        std::vector<VkPresentModeKHR> modes(count);
        check(vk->vkGetPhysicalDeviceSurfacePresentModesKHR(vk->physical, surface, &count, modes.data()), "vkGetPhysicalDeviceSurfacePresentModesKHR");
        const auto has = [&](VkPresentModeKHR value) { return std::find(modes.begin(), modes.end(), value) != modes.end(); };
        auto present_mode = VK_PRESENT_MODE_FIFO_KHR;   // always available
        if (!display_sync) {
            if (has(VK_PRESENT_MODE_MAILBOX_KHR)) present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
            else if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
        VkExtent2D size = wanted;
        if (caps.currentExtent.width != UINT32_MAX) size = caps.currentExtent;
        size.width = std::clamp(size.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        size.height = std::clamp(size.height, caps.minImageExtent.height, caps.maxImageExtent.height);
        if (!size.width || !size.height) { stale = true; return; }
        auto images = std::max<std::uint32_t>(caps.minImageCount, std::min<std::uint32_t>(config.maximum_drawable_count, 3));
        if (caps.maxImageCount) images = std::min(images, caps.maxImageCount);
        VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (capture_enabled()) {
            if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            else if (options.capture) fail("swapchain capture is unsupported by the surface");
            // ALCHEMY_PRESENT_CAPTURE requests then fail explicitly (Last::problem).
        }
        if ((caps.supportedUsageFlags & usage) != usage) fail("the window surface cannot be a transfer destination");
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if (!(caps.supportedCompositeAlpha & alpha)) {
            for (const auto bit : {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                     VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
                if (caps.supportedCompositeAlpha & bit) { alpha = bit; break; }
        }
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface;
        info.minImageCount = images;
        info.imageFormat = chosen.format;
        info.imageColorSpace = chosen.colorSpace;
        info.imageExtent = size;
        info.imageArrayLayers = 1;
        info.imageUsage = usage;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
            ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
        info.compositeAlpha = alpha;
        info.presentMode = present_mode;
        info.clipped = VK_TRUE;
        info.oldSwapchain = swapchain;
        VkSwapchainKHR created{};
        check(vk->vkCreateSwapchainKHR(vk->device, &info, nullptr, &created), "vkCreateSwapchainKHR");
        destroy_swapchain(false);   // the retired one
        swapchain = created;
        swap_format = chosen.format;
        extent = size;
        mode = present_mode;
        swap_linear = linear;
        swap_sync = display_sync;
        stale = false;
        check(vk->vkGetSwapchainImagesKHR(vk->device, swapchain, &count, nullptr), "vkGetSwapchainImagesKHR");
        swap_images.resize(count);
        check(vk->vkGetSwapchainImagesKHR(vk->device, swapchain, &count, swap_images.data()), "vkGetSwapchainImagesKHR");
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        rendered.resize(count);
        for (auto& value : rendered) check(vk->vkCreateSemaphore(vk->device, &semaphore, nullptr, &value), "vkCreateSemaphore");
        capture_capable = (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
        ++counters.swapchains_created;
        counters.swapchain_width = size.width;
        counters.swapchain_height = size.height;
    }

    // ---- present ------------------------------------------------------------------
    // `capture`: also copy the swapchain image into that staging buffer.
    void record(VkCommandBuffer commands, PoolImage& image, VkImage target, const PresentTarget& where, VkBuffer capture) {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vk->vkResetCommandBuffer(commands, 0), "vkResetCommandBuffer");
        check(vk->vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer");
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const auto barrier = [&](VkImage subject, VkImageLayout from, VkImageLayout to, VkAccessFlags source_access,
                                 VkAccessFlags destination_access, VkPipelineStageFlags source_stage,
                                 VkPipelineStageFlags destination_stage) {
            VkImageMemoryBarrier value{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            value.srcAccessMask = source_access;
            value.dstAccessMask = destination_access;
            value.oldLayout = from;
            value.newLayout = to;
            value.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            value.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            value.image = subject;
            value.subresourceRange = range;
            vk->vkCmdPipelineBarrier(commands, source_stage, destination_stage, 0, 0, nullptr, 0, nullptr, 1, &value);
        };
        // Everything Metal submitted before this blit (queue order) finished
        // writing the drawable.
        barrier(image.image, image.layout.load(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        // Chained to the acquire semaphore wait at the transfer stage.
        barrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        // Destination rectangle, clipped to the swapchain; the source is
        // clipped proportionally.
        double dx0 = 0, dy0 = 0, dx1 = extent.width, dy1 = extent.height;
        if (where.width > 0 && where.height > 0) {
            dx0 = where.x; dx1 = where.x + where.width;
            dy0 = extent.height - (where.y + where.height); dy1 = dy0 + where.height;   // to top-left origin
        }
        const double sw = image.extent.width, sh = image.extent.height;
        const auto map_x = [&](double value) { return (value - dx0) / (dx1 - dx0) * sw; };
        const auto map_y = [&](double value) { return (value - dy0) / (dy1 - dy0) * sh; };
        const double cx0 = std::max(dx0, 0.0), cy0 = std::max(dy0, 0.0);
        const double cx1 = std::min(dx1, static_cast<double>(extent.width)), cy1 = std::min(dy1, static_cast<double>(extent.height));
        const bool covers = cx0 <= 0 && cy0 <= 0 && cx1 >= extent.width && cy1 >= extent.height;
        if (!covers) {
            const VkClearColorValue black{{0.0F, 0.0F, 0.0F, 1.0F}};
            vk->vkCmdClearColorImage(commands, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
            barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
        }
        if (cx1 > cx0 && cy1 > cy0) {
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstSubresource = blit.srcSubresource;
            const auto source = [](double value, double limit) {
                return static_cast<std::int32_t>(std::lround(std::clamp(value, 0.0, limit)));
            };
            blit.srcOffsets[0] = {source(map_x(cx0), sw), source(map_y(cy0), sh), 0};
            blit.srcOffsets[1] = {source(map_x(cx1), sw), source(map_y(cy1), sh), 1};
            blit.dstOffsets[0] = {static_cast<std::int32_t>(cx0), static_cast<std::int32_t>(cy0), 0};
            blit.dstOffsets[1] = {static_cast<std::int32_t>(cx1), static_cast<std::int32_t>(cy1), 1};
            const bool scaled = blit.srcOffsets[1].x - blit.srcOffsets[0].x != blit.dstOffsets[1].x - blit.dstOffsets[0].x ||
                blit.srcOffsets[1].y - blit.srcOffsets[0].y != blit.dstOffsets[1].y - blit.dstOffsets[0].y;
            const bool linear_filter = scaled && (vk->features(image.format) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
            if (blit.srcOffsets[1].x > blit.srcOffsets[0].x && blit.srcOffsets[1].y > blit.srcOffsets[0].y)
                vk->vkCmdBlitImage(commands, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                                   linear_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        }
        auto final_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        if (capture) {
            barrier(target, final_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {extent.width, extent.height, 1};
            vk->vkCmdCopyImageToBuffer(commands, target, final_layout, capture, 1, &copy);
            VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.buffer = capture;
            host.size = VK_WHOLE_SIZE;
            vk->vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,
                                     1, &host, 0, nullptr);
        }
        barrier(target, final_layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        check(vk->vkEndCommandBuffer(commands), "vkEndCommandBuffer");
    }

    // Returns the fence of the queued blit, or null when nothing was presented.
    std::shared_ptr<Fence> present(PoolImage& image, const PresentTarget& where, const std::function<bool()>& should_stop) {
        if (!where.window) return nullptr;
        const auto client = host::ui::window_client_size(reinterpret_cast<std::uintptr_t>(where.window));
        if (!client) {
            // The window is gone: drop its surface.
            if (window == where.window) { wait_frames(); destroy_swapchain(true); }
            return nullptr;
        }
        const VkExtent2D size{client->width, client->height};
        if (!size.width || !size.height) return nullptr;   // minimized
        attach(where.window);
        if (!swapchain || stale || extent.width != size.width || extent.height != size.height ||
            swap_linear != image.config.linear_content || swap_sync != where.display_sync)
            create_swapchain(size, image.config.linear_content, where.display_sync);
        if (!swapchain || stale) return nullptr;   // nothing presentable (0x0 surface)

        const auto slot_index = next_slot;
        auto& slot = slots[slot_index];
        next_slot = (next_slot + 1) % slots.size();
        if (slot.fence) {
            alchemy::perf::Scope timed(alchemy::perf::present_slot_wait_ticks);
            static_cast<void>(slot.fence->wait(UINT64_MAX));
        }
        std::uint32_t index{};
        {
        alchemy::perf::Scope acquire_timed(alchemy::perf::present_acquire_ticks);
        for (int attempt = 0;;) {
            const auto result = vk->vkAcquireNextImageKHR(vk->device, swapchain, slice_nanoseconds, slot.acquired,
                                                          VK_NULL_HANDLE, &index);
            if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
                if (result == VK_SUBOPTIMAL_KHR) stale = true;
                break;
            }
            if (result == VK_TIMEOUT || result == VK_NOT_READY) {
                if (should_stop && should_stop()) throw PresenterStopped();
                continue;
            }
            if ((result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR) && attempt++ < 2) {
                if (result == VK_ERROR_SURFACE_LOST_KHR) { wait_frames(); destroy_swapchain(true); attach(where.window); }
                create_swapchain(size, image.config.linear_content, where.display_sync);
                if (!swapchain || stale) return nullptr;   // nothing presentable (0x0 surface)
                continue;
            }
            // Still out of date (a live resize outran us): drop this frame.
            if (result == VK_ERROR_OUT_OF_DATE_KHR) { stale = true; return nullptr; }
            check(result, "vkAcquireNextImageKHR");
        }
        }
        // Capture enabled: copy this image into the slot's staging buffer
        // (the watcher serves requests from the newest one). capture_mutex
        // is held until `last` names this present, so no reader sees the
        // buffer while the copy is queued.
        std::unique_lock capture_lock(capture_mutex, std::defer_lock);
        VkBuffer capture_target{};
        if (capture_enabled()) {
            capture_lock.lock();
            if (capture_capable)
                capture_target = staging_for(slot_index, static_cast<VkDeviceSize>(extent.width) * extent.height * 4).buffer;
        }
        record(slot.commands, image, swap_images[index], where, capture_target);
        auto fence = std::make_shared<Fence>(vk);
        VkResult presented = VK_SUCCESS;
        alchemy::perf::add(alchemy::perf::mtl_queue_submits);
        alchemy::perf::Scope queue_timed(alchemy::perf::present_queue_ticks);
        vk->bridge->with_queue([&](VkQueue queue) {
            const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &slot.acquired;
            submit.pWaitDstStageMask = &stage;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &slot.commands;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &rendered[index];
            check(vk->vkQueueSubmit(queue, 1, &submit, fence->fence), "vkQueueSubmit (present blit)");
            VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            info.waitSemaphoreCount = 1;
            info.pWaitSemaphores = &rendered[index];
            info.swapchainCount = 1;
            info.pSwapchains = &swapchain;
            info.pImageIndices = &index;
            presented = vk->vkQueuePresentKHR(queue, &info);
        });
        slot.fence = fence;
        image.layout.store(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        const auto frame = ++total_presented;
        const auto hwnd = reinterpret_cast<std::uintptr_t>(window);
        if (capture_lock.owns_lock()) {
            if (capture_target) last = {slot_index, fence, extent.width, extent.height, swap_format, hwnd, frame, {}};
            else last = {0, nullptr, extent.width, extent.height, swap_format, hwnd, frame,
                         "the window surface cannot be a transfer source"};
            capture_lock.unlock();
        }
        ++counters.presented;
        shown_window.store(hwnd);
        shown_width.store(extent.width);
        shown_height.store(extent.height);
        shown_presented.fetch_add(1);
        shown_sequence.store(present_sequence.fetch_add(1) + 1);
        if (presented == VK_SUBOPTIMAL_KHR || presented == VK_ERROR_OUT_OF_DATE_KHR) stale = true;
        else if (presented == VK_ERROR_SURFACE_LOST_KHR) { stale = true; }
        else check(presented, "vkQueuePresentKHR");
        return fence;
    }
};

Presenter::Presenter(std::shared_ptr<MetalVulkanDevice> device, PresenterOptions options)
    : impl_(std::make_shared<Impl>(std::move(device), options)) {
    start_present_services();
    register_source(impl_);
}
Presenter::~Presenter() = default;

std::shared_ptr<DrawableImage> Presenter::acquire(const DrawableConfig& wanted, double timeout_seconds,
                                                  const std::function<bool()>& should_stop) {
    if (!wanted.width || !wanted.height) fail("drawable size is empty");
    if (wanted.maximum_drawable_count < 2 || wanted.maximum_drawable_count > 3)
        fail("maximumDrawableCount must be 2 or 3");
    static_cast<void>(vulkan_format(wanted.pixel_format));
    auto& impl = *impl_;
    std::unique_lock lock(impl.pool_mutex);
    if (!(impl.config == wanted)) {
        impl.config = wanted;
        for (const auto& image : impl.free_images) impl.retire(image);
        impl.free_images.clear();
    }
    const bool bounded = std::isfinite(timeout_seconds);
    const auto deadline = bounded
        ? Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(std::max(timeout_seconds, 0.0)))
        : Clock::time_point::max();
    for (;;) {
        impl.reap();
        if (!impl.free_images.empty()) {
            auto image = std::move(impl.free_images.back());
            impl.free_images.pop_back();
            ++impl.held;
            return image;
        }
        if (impl.held + impl.in_flight.size() < wanted.maximum_drawable_count) {
            ++impl.held;
            lock.unlock();
            try { return impl.create(wanted); }
            catch (...) { lock.lock(); --impl.held; impl.released.notify_all(); throw; }
        }
        if (bounded && Clock::now() >= deadline) return nullptr;
        if (impl.in_flight.empty()) {
            // Only release() can free an image now.
            if (!bounded)
                fail("all " + std::to_string(impl.held) +
                     " drawables are held by the application and allowsNextDrawableTimeout is NO; nextDrawable would never return");
            alchemy::perf::Scope timed(alchemy::perf::next_drawable_wait_ticks);
            impl.released.wait_for(lock, std::min<Clock::duration>(wait_slice, deadline - Clock::now()));
        } else {
            const auto fence = impl.in_flight.front()->last_blit;
            lock.unlock();
            auto slice = slice_nanoseconds;
            if (bounded) {
                const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - Clock::now()).count();
                slice = std::min<std::uint64_t>(slice, static_cast<std::uint64_t>(std::max<long long>(left, 0)));
            }
            if (fence) {
                alchemy::perf::Scope timed(alchemy::perf::next_drawable_wait_ticks);
                static_cast<void>(fence->wait(slice));
            }
            lock.lock();
        }
        if (should_stop && should_stop()) throw PresenterStopped();
    }
}

void Presenter::release(const std::shared_ptr<DrawableImage>& image) {
    const auto pooled = std::dynamic_pointer_cast<PoolImage>(image);
    if (!pooled) fail("released image does not belong to a presenter");
    std::lock_guard lock(impl_->pool_mutex);
    if (impl_->held) --impl_->held;
    impl_->give_back(pooled);
    impl_->released.notify_all();
}

bool Presenter::present(const std::shared_ptr<DrawableImage>& image, const PresentTarget& target,
                        const std::function<bool()>& should_stop) {
    const auto pooled = std::dynamic_pointer_cast<PoolImage>(image);
    if (!pooled) fail("presented image does not belong to a presenter");
    auto& impl = *impl_;
    std::shared_ptr<Fence> fence;
    alchemy::perf::Scope timed(alchemy::perf::present_ticks);
    try {
        std::lock_guard present(impl.present_mutex);
        fence = impl.present(*pooled, target, should_stop);   // counts total_presented
        if (fence) alchemy::fx_profile_window::frame_presented();   // ALCHEMY_FX_PROFILE_WINDOW
        if (!fence) ++impl.counters.skipped;
        const auto frame = total_presented.load();
        const auto skip = fence ? total_skipped.load() : ++total_skipped;
        if (present_trace_enabled()) impl.trace(*pooled, fence != nullptr, frame, skip);
    } catch (...) {
        release(image);
        throw;
    }
    std::lock_guard lock(impl.pool_mutex);
    if (impl.held) --impl.held;
    if (fence) {
        pooled->last_blit = fence;
        impl.in_flight.push_back(pooled);
    } else {
        impl.give_back(pooled);
    }
    impl.released.notify_all();
    if (fence) alchemy::perf::frame_presented();
    return fence != nullptr;
}

void Presenter::detach() {
    std::lock_guard present(impl_->present_mutex);
    impl_->wait_frames();
    impl_->destroy_swapchain(true);
}

std::vector<std::uint64_t> Presenter::shutdown() {
    detach();
    std::lock_guard lock(impl_->pool_mutex);
    impl_->config = DrawableConfig{pixel_format::invalid, 0, 0};   // matches no image again
    for (const auto& image : impl_->free_images) impl_->retire(image);
    for (const auto& image : impl_->in_flight) impl_->retire(image);
    impl_->free_images.clear();
    impl_->in_flight.clear();
    return std::exchange(impl_->retired_textures, {});
}

PresentStats Presenter::stats() const {
    std::lock_guard present(impl_->present_mutex);
    return impl_->counters;
}

std::optional<Capture> Presenter::last_capture() {
    auto& impl = *impl_;
    std::lock_guard lock(impl.capture_mutex);
    if (!impl.last.fence) return std::nullopt;
    return Capture{impl.last.width, impl.last.height, impl.read_last(false)};
}

std::shared_ptr<MetalVulkanDevice> Presenter::device() const { return impl_->vk->bridge; }

std::vector<std::uint64_t> Presenter::take_retired_textures() {
    std::lock_guard lock(impl_->pool_mutex);
    return std::exchange(impl_->retired_textures, {});
}

std::uint64_t Presenter::texture_for(const std::shared_ptr<DrawableImage>& image) {
    const auto pooled = std::dynamic_pointer_cast<PoolImage>(image);
    if (!pooled) fail("texture requested for an image that does not belong to a presenter");
    {
        std::lock_guard lock(impl_->pool_mutex);
        if (pooled->texture) return pooled->texture;
    }
    // Guest object creation runs outside the pool lock.
    const auto texture = impl_->vk->bridge->wrap_drawable_texture(image);
    if (!texture) fail("the Metal device could not wrap a drawable texture");
    std::lock_guard lock(impl_->pool_mutex);
    if (pooled->texture) {
        impl_->retired_textures.push_back(texture);   // lost a race; released by the caller later
        return pooled->texture;
    }
    pooled->texture = texture;
    return texture;
}

#endif // ALCHEMY_HAVE_VULKAN_HEADERS

} // namespace alchemy::gui::quartzcore
