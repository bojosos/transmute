#pragma once

// Test double for Metal's side of quartzcore_vulkan.hpp: a real Vulkan
// device (vulkan-1.dll loaded at run time) that stands in for A5's MTLDevice
// bridge, plus "Metal rendering" helpers that clear drawable images the way
// a render pass would (submitted to the bridge queue, layout updated).
#include "alchemy/darwin/quartzcore/quartzcore_vulkan.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace alchemy::gui::quartzcore::test {

#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
class VulkanTestDevice final : public MetalVulkanDevice {
public:
    // Empty when no Vulkan driver with a graphics queue and Win32
    // presentation exists (the caller skips); `why` says what was missing.
    static std::shared_ptr<VulkanTestDevice> create(std::string& why);
    ~VulkanTestDevice() override;

    PFN_vkGetInstanceProcAddr get_instance_proc_addr() const override { return get_proc_; }
    VkInstance instance() const override { return instance_; }
    VkPhysicalDevice physical_device() const override { return physical_; }
    VkDevice device() const override { return device_; }
    std::uint32_t queue_family() const override { return family_; }
    void with_queue(const std::function<void(VkQueue)>& work) override;
    std::uint64_t wrap_drawable_texture(const std::shared_ptr<DrawableImage>& image) override;

    // What wrap_drawable_texture returns (default: distinct fake handles).
    std::function<std::uint64_t(const std::shared_ptr<DrawableImage>&)> wrap;
    // "Renders" `rgba` (0..1) into `image` like a Metal render pass with a
    // clear load action, and leaves it in COLOR_ATTACHMENT_OPTIMAL.
    void clear(DrawableImage& image, std::array<float, 4> rgba);
    void wait_idle();
    std::string device_name;

private:
    VulkanTestDevice() = default;
    void* library_{};
    PFN_vkGetInstanceProcAddr get_proc_{};
    VkInstance instance_{};
    VkPhysicalDevice physical_{};
    VkDevice device_{};
    VkQueue queue_{};
    std::uint32_t family_{};
    std::mutex queue_mutex_;
    std::uint64_t next_fake_{0x7000'0000'0000ULL};
};
#endif

// A visible top-level Win32 window with a client area of the given size
// (pixels), pumped by pump(). Empty without an interactive desktop.
class TestWindow {
public:
    static std::unique_ptr<TestWindow> create(int width, int height, std::string& why);
    ~TestWindow();
    void* handle() const { return window_; }
    void resize(int width, int height);
    void pump();
private:
    void* window_{};
};

} // namespace alchemy::gui::quartzcore::test
