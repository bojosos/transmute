#pragma once

// CAMetalLayer presentation engine (owner: A7). Pure C++ over the Vulkan
// device of quartzcore_vulkan.hpp; no Objective-C.
//
// Model (the macOS compositor analog): drawables are QuartzCore-owned
// device-local images of exactly drawableSize and pixelFormat, pooled up to
// maximumDrawableCount. Presenting one blits it (scaled when the sizes
// differ, converting the format) into the window's swapchain image at the
// layer's frame and queues the swapchain present. The swapchain therefore
// always matches the HWND client area (Win32 surfaces require it) and is
// recreated on resize, DPI change or present-mode change without touching
// the drawables, and drawableSize, pixelFormat and the window size stay
// independent exactly as on macOS.
//
// Limits: one presenting layer per HWND (a second fails explicitly; Win32
// allows one swapchain per window); 10-bit XR formats are rejected when the
// pixel format is set; extended-range content is tone-mapped by clamping
// into an 8-bit sRGB swapchain (no HDR swapchain yet).
//
// Threading: every member function may be called from any thread; calls are
// serialized internally. Blocking waits (free drawable, swapchain acquire)
// call the `should_stop` callback about every 100 ms and throw
// PresenterStopped when it returns true.

#include "alchemy/darwin/quartzcore/quartzcore_vulkan.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace alchemy::gui::quartzcore {

struct DrawableConfig {
    std::uint64_t pixel_format{pixel_format::bgra8_unorm};
    std::uint32_t width{}, height{};             // drawableSize, pixels
    std::uint32_t maximum_drawable_count{3};
    bool framebuffer_only{true};
    // Content is linear light (a linear colorspace such as
    // kCGColorSpaceExtendedLinearSRGB, or an _sRGB pixel format whose stores
    // encode): present through an sRGB-encoding swapchain. Otherwise the
    // stored values are already display encoded and are copied verbatim.
    bool linear_content{};
    friend bool operator==(const DrawableConfig&, const DrawableConfig&) = default;
};

struct PresentTarget {
    void* window{};                 // HWND (window server content area); null: not attached
    // Destination in the client area in physical pixels with AppKit's
    // bottom-left origin (the layer frame in content points times the
    // backing scale). Width or height 0: the whole client area.
    double x{}, y{}, width{}, height{};
    bool display_sync{true};        // CAMetalLayer displaySyncEnabled: FIFO, else mailbox/immediate
};

class PresenterStopped : public std::runtime_error {
public:
    PresenterStopped() : std::runtime_error("Alchemy QuartzCore: process is stopping") {}
};

// Presentation diagnostics (tests, harness).
struct PresentStats {
    std::uint64_t presented{}, skipped{}, swapchains_created{};
    std::uint32_t swapchain_width{}, swapchain_height{};
};

// RGBA8 copy of the last presented swapchain image (PresenterOptions::capture).
struct Capture {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> rgba;
};

struct PresenterOptions {
    // Copy every presented swapchain image to host-visible memory (tests;
    // ALCHEMY_PRESENT_CAPTURE enables it for every presenter). Fails
    // explicitly when the surface cannot be a transfer source.
    bool capture{};
};

// Process-wide presentation totals over every presenter: frames queued to a
// window swapchain and frames dropped (no window, destroyed or minimized
// window, out-of-date swapchain). Always counted.
struct PresentTotals {
    std::uint64_t presented{}, skipped{};
};
[[nodiscard]] PresentTotals present_totals();

// ALCHEMY_TRACE_PRESENT=1 logs presentation to stderr as
//   [alchemy present] frame <n>: drawable <w>x<h> -> window 0x<hwnd> swapchain <w>x<h> #<k>
//   [alchemy present] skipped <n> (no presentable window) after <m> frames
// for frames 1-3, every 60th frame, and every frame whose drawable size,
// window or swapchain changed; skips are logged on the first and every 60th.
// The GUI harness scripts parse the last frame number as the presented-frame
// count. Read once per process.
[[nodiscard]] bool present_trace_enabled();

// ALCHEMY_PRESENT_CAPTURE=<directory>: on-demand capture of what a window
// shows, for harnesses on desktops where PrintWindow/BitBlt cannot read a
// Vulkan swapchain (independent flip, locked workstation). Swapchains are
// created with transfer-source usage and every present also copies the
// swapchain image into a host-visible staging buffer, so the last presented
// image of every window stays readable while the guest is idle. A harness
// writes <directory>/request = "<hwnd decimal, 0 = any> <output path>"; the
// present watcher thread (start_present_services) serves it from the last
// image presented to that window (0: the most recent present of any window)
// without waiting for another present, and writes it as a top-down 32-bit
// BMP to <output path> (atomically), or <output path>.error with the reason
// (for example a surface without transfer-source support). A request for a
// window that has not presented yet stays pending until it does. Empty when
// unset. Read once per process.
[[nodiscard]] const std::filesystem::path& present_capture_directory();

// ALCHEMY_PRESENT_STATS=<file>: the exact presentation totals as JSON,
//   {"pid":..,"presented":N,"skipped":M,"uptime_ms":..,"final":false,
//    "windows":[{"window":<hwnd decimal>,"presented":n,"width":w,"height":h},..]}
// rewritten atomically by the watcher thread about every 100 ms while the
// counts change (and at least once a second), and once more with
// "final":true when the process exits normally (atexit). With
// ALCHEMY_TRACE_PRESENT=1 the exit also logs
//   [alchemy present] totals: presented <n> skipped <m>
// Empty when unset. Read once per process.
[[nodiscard]] const std::filesystem::path& present_stats_path();

// Starts the present watcher thread (capture requests, stats file) and the
// at-exit totals when ALCHEMY_PRESENT_CAPTURE, ALCHEMY_PRESENT_STATS or
// ALCHEMY_TRACE_PRESENT is set; otherwise does nothing. Idempotent; called
// by install_quartzcore and by every Presenter.
void start_present_services();

#ifdef ALCHEMY_HAVE_VULKAN_HEADERS

class Presenter {
public:
    explicit Presenter(std::shared_ptr<MetalVulkanDevice> device, PresenterOptions options = {});
    ~Presenter();
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    // -nextDrawable: a free image matching `config`, blocking up to
    // `timeout_seconds` (infinity: no timeout) while every image is in use;
    // nullptr on timeout. Throws when the device cannot create the image
    // or when every image is held by the application and no timeout is
    // allowed (the wait could never end).
    [[nodiscard]] std::shared_ptr<DrawableImage> acquire(const DrawableConfig& config, double timeout_seconds,
                                                         const std::function<bool()>& should_stop = {});
    // Returns a drawable image that was not presented (the drawable object
    // died first).
    void release(const std::shared_ptr<DrawableImage>& image);
    // -present: blits `image` into `target`'s swapchain and queues the
    // present after all work previously submitted to the device queue.
    // Returns false (and frees the image) when there is nothing to present
    // to: no window, a destroyed window, or a minimized (0x0) client area.
    bool present(const std::shared_ptr<DrawableImage>& image, const PresentTarget& target,
                 const std::function<bool()>& should_stop = {});
    // Destroys the surface and swapchain (the layer left its window).
    void detach();
    // The layer is going away: detaches, retires every image (images still
    // held by drawables retire when they come back) and returns every
    // texture the caller must now release (see take_retired_textures).
    [[nodiscard]] std::vector<std::uint64_t> shutdown();

    [[nodiscard]] PresentStats stats() const;
    // The last presented swapchain image (capture enabled), whenever it was
    // presented; waits for that present's copy. Empty before the first
    // captured present.
    [[nodiscard]] std::optional<Capture> last_capture();
    [[nodiscard]] std::shared_ptr<MetalVulkanDevice> device() const;

    // Every guest texture object a pool image wrapped (MetalVulkanDevice::
    // wrap_drawable_texture), handed out once per image as images are
    // destroyed; the caller releases them on a guest thread.
    [[nodiscard]] std::vector<std::uint64_t> take_retired_textures();
    // The guest texture object for `image`, created on first use with
    // `wrap` (MetalVulkanDevice::wrap_drawable_texture) and owned by the
    // presenter until the image is destroyed.
    [[nodiscard]] std::uint64_t texture_for(const std::shared_ptr<DrawableImage>& image);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

#endif // ALCHEMY_HAVE_VULKAN_HEADERS

// MTLPixelFormat -> CAMetalLayer acceptance: empty when the format is valid
// and supported, otherwise the reason (invalid for a layer, or unsupported).
[[nodiscard]] std::optional<std::string> layer_pixel_format_problem(std::uint64_t pixel_format);

} // namespace alchemy::gui::quartzcore
