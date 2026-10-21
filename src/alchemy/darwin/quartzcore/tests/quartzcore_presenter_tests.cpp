// CAMetalLayer presentation engine against a real Vulkan device and Win32
// window (owner: A7). Exit 77 = skipped (no driver or desktop).
#include "alchemy/darwin/quartzcore/presenter.hpp"
#include "alchemy/darwin/quartzcore/tests/vulkan_test_device.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {
using namespace alchemy::gui::quartzcore;
namespace pf = alchemy::gui::quartzcore::pixel_format;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
struct Pixel { int r, g, b, a; };
// (x, y) with a top-left origin.
Pixel at(const Capture& capture, std::uint32_t x, std::uint32_t y) {
    const auto offset = (static_cast<std::size_t>(y) * capture.width + x) * 4;
    return {capture.rgba[offset], capture.rgba[offset + 1], capture.rgba[offset + 2], capture.rgba[offset + 3]};
}
bool close_to(Pixel value, Pixel wanted, int tolerance = 2) {
    return std::abs(value.r - wanted.r) <= tolerance && std::abs(value.g - wanted.g) <= tolerance &&
        std::abs(value.b - wanted.b) <= tolerance;
}
std::string text(Pixel value) {
    return std::to_string(value.r) + "," + std::to_string(value.g) + "," + std::to_string(value.b) + "," + std::to_string(value.a);
}

struct Rig {
    std::shared_ptr<test::VulkanTestDevice> device;
    std::unique_ptr<test::TestWindow> window;
};

Capture present_clear(Presenter& presenter, test::VulkanTestDevice& device, test::TestWindow& window,
                      const DrawableConfig& config, std::array<float, 4> color, PresentTarget target = {}) {
    const auto image = presenter.acquire(config, 1.0);
    require(image != nullptr, "acquire timed out");
    require(image->extent.width == config.width && image->extent.height == config.height, "drawable has the wrong size");
    static_cast<void>(presenter.texture_for(image));
    device.clear(*image, color);
    target.window = window.handle();
    require(presenter.present(image, target), "present was skipped");
    window.pump();
    const auto capture = presenter.last_capture();
    require(capture.has_value(), "no capture after a present");
    return *capture;
}

void presentation(Rig& rig) {
    auto& device = *rig.device;
    auto& window = *rig.window;
    Presenter presenter(rig.device, PresenterOptions{true});
    DrawableConfig config{pf::bgra8_unorm, 320, 240, 3, true, false};

    // Same size: verbatim copy.
    auto capture = present_clear(presenter, device, window, config, {1, 0, 0, 1});
    require(capture.width == 320 && capture.height == 240,
            "swapchain is " + std::to_string(capture.width) + "x" + std::to_string(capture.height));
    require(close_to(at(capture, 160, 120), {255, 0, 0, 255}), "red frame presented as " + text(at(capture, 160, 120)));

    // drawableSize smaller than the window: scaled to the whole client area.
    auto half = config;
    half.width = 160; half.height = 120;
    capture = present_clear(presenter, device, window, half, {0, 1, 0, 1});
    require(close_to(at(capture, 5, 5), {0, 255, 0, 255}) && close_to(at(capture, 315, 235), {0, 255, 0, 255}),
            "scaled green frame presented as " + text(at(capture, 315, 235)));

    // Layer frame covering the bottom-left quadrant (AppKit origin): the
    // rest of the window is cleared to black.
    PresentTarget quadrant;
    quadrant.x = 0; quadrant.y = 0; quadrant.width = 160; quadrant.height = 120;
    capture = present_clear(presenter, device, window, half, {0, 0, 1, 1}, quadrant);
    require(close_to(at(capture, 10, 230), {0, 0, 255, 255}), "bottom-left quadrant is " + text(at(capture, 10, 230)));
    require(close_to(at(capture, 300, 10), {0, 0, 0, 255}), "uncovered area is " + text(at(capture, 300, 10)));

    // Resize: the swapchain follows the client area, the drawable does not.
    const auto created = presenter.stats().swapchains_created;
    window.resize(400, 300);
    capture = present_clear(presenter, device, window, config, {1, 1, 0, 1});
    require(capture.width == 400 && capture.height == 300, "swapchain did not follow the resize");
    require(presenter.stats().swapchains_created == created + 1, "resize did not recreate the swapchain exactly once");
    require(close_to(at(capture, 399, 299), {255, 255, 0, 255}), "resized frame is " + text(at(capture, 399, 299)));

    // Linear content (RGBA16Float + a linear colorspace): sRGB-encoded on
    // the way out. 0.5 linear -> 188.
    auto linear = config;
    linear.pixel_format = pf::rgba16_float;
    linear.linear_content = true;
    capture = present_clear(presenter, device, window, linear, {0.5F, 0.5F, 0.5F, 1});
    require(close_to(at(capture, 200, 150), {188, 188, 188, 255}), "linear grey presented as " + text(at(capture, 200, 150)));
    // The format change retired the BGRA8 images' textures for release.
    require(!presenter.take_retired_textures().empty(), "format change did not retire the old textures");

    // _sRGB storage: stores encode, bytes are shown verbatim.
    auto srgb = config;
    srgb.pixel_format = pf::bgra8_unorm_srgb;
    srgb.linear_content = true;
    capture = present_clear(presenter, device, window, srgb, {0.5F, 0.5F, 0.5F, 1});
    require(close_to(at(capture, 200, 150), {188, 188, 188, 255}), "sRGB grey presented as " + text(at(capture, 200, 150)));

    // displaySyncEnabled = NO: a non-FIFO swapchain, still presenting.
    PresentTarget unsynced;
    unsynced.display_sync = false;
    capture = present_clear(presenter, device, window, config, {0, 1, 1, 1}, unsynced);
    require(close_to(at(capture, 1, 1), {0, 255, 255, 255}), "unsynced frame is " + text(at(capture, 1, 1)));

    // Sustained presentation (frame pacing, slot and semaphore reuse).
    for (int frame = 0; frame < 90; ++frame) {
        const auto image = presenter.acquire(config, 1.0);
        require(image != nullptr, "acquire timed out during sustained presentation");
        device.clear(*image, {frame / 90.0F, 0, 0, 1});
        PresentTarget target;
        target.window = window.handle();
        require(presenter.present(image, target), "sustained present skipped");
        window.pump();
    }
    require(presenter.stats().presented >= 97, "present count is wrong");
    // The process-wide totals (ALCHEMY_TRACE_PRESENT) count every presenter.
    const auto totals = present_totals();
    require(totals.presented >= presenter.stats().presented, "process present total is behind the presenter");

    // Nothing to present to: no window.
    {
        const auto image = presenter.acquire(config, 1.0);
        require(!presenter.present(image, PresentTarget{}), "present without a window succeeded");
    }
    require(present_totals().skipped == totals.skipped + 1 && present_totals().presented == totals.presented,
            "a skipped present was not counted as skipped");
    for (const auto texture : presenter.shutdown()) static_cast<void>(texture);
    std::cout << "presentation ok (" << device.device_name << ", " << presenter.stats().presented << " presents)\n";
}

void pool(Rig& rig) {
    Presenter presenter(rig.device);
    DrawableConfig config{pf::bgra8_unorm, 64, 64, 2, true, false};
    const auto first = presenter.acquire(config, 1.0);
    const auto second = presenter.acquire(config, 1.0);
    require(first && second && first != second, "two distinct drawables expected");
    require(presenter.acquire(config, 0.2) == nullptr, "a third drawable beyond maximumDrawableCount 2");
    bool threw = false;
    try { static_cast<void>(presenter.acquire(config, std::numeric_limits<double>::infinity())); }
    catch (const std::runtime_error& error) { threw = std::string(error.what()).find("held by the application") != std::string::npos; }
    require(threw, "an unbounded wait with every drawable held did not fail explicitly");
    presenter.release(first);
    const auto again = presenter.acquire(config, 0.2);
    require(again == first, "a released drawable was not reused");
    presenter.release(again);
    presenter.release(second);
    // Presenting frees the image after its blit.
    const auto image = presenter.acquire(config, 1.0);
    rig.device->clear(*image, {1, 1, 1, 1});
    PresentTarget target;
    target.window = rig.window->handle();
    require(presenter.present(image, target), "pool present skipped");
    for (int index = 0; index < 4; ++index) {
        const auto next = presenter.acquire(config, 1.0);
        require(next != nullptr, "presented drawables were not recycled");
        presenter.release(next);
    }
    // Invalid configurations fail explicitly.
    auto bad = config;
    bad.maximum_drawable_count = 4;
    threw = false;
    try { static_cast<void>(presenter.acquire(bad, 1.0)); } catch (const std::runtime_error&) { threw = true; }
    require(threw, "maximumDrawableCount 4 accepted");
    bad = config;
    bad.pixel_format = pf::bgra10_xr;
    threw = false;
    try { static_cast<void>(presenter.acquire(bad, 1.0)); }
    catch (const std::runtime_error& error) { threw = std::string(error.what()).find("unsupported") != std::string::npos; }
    require(threw, "XR pixel format not rejected as unsupported");
    // One presenting layer per window.
    Presenter other(rig.device);
    const auto foreign = other.acquire(config, 1.0);
    threw = false;
    try { static_cast<void>(other.present(foreign, target)); } catch (const std::runtime_error&) { threw = true; }
    require(threw, "two presenters on one window were accepted");
    static_cast<void>(presenter.shutdown());
    std::cout << "pool ok\n";
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
bool wait_for(const std::function<bool()>& condition, int milliseconds) {
    for (int waited = 0; waited < milliseconds; waited += 20) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return condition();
}
void write_request(const std::filesystem::path& directory, std::uintptr_t window, const std::filesystem::path& output) {
    const auto request = directory / "request";
    {
        std::ofstream(request.string() + ".tmp") << window << ' ' << output.string();
    }
    std::filesystem::rename(request.string() + ".tmp", request);
}

// ALCHEMY_PRESENT_CAPTURE / ALCHEMY_PRESENT_STATS (set by main before the
// first presenter): the watcher serves a capture request from the last
// presented image while nothing presents any more, and the stats file has
// the exact totals.
void watcher(Rig& rig, const std::filesystem::path& directory) {
    Presenter presenter(rig.device);   // capture enabled by the environment only
    DrawableConfig config{pf::bgra8_unorm, 200, 100, 3, true, false};
    std::array<std::array<float, 4>, 2> colors{{{0, 1, 0, 1}, {1, 0, 1, 1}}};
    for (const auto& color : colors) {
        const auto image = presenter.acquire(config, 1.0);
        require(image != nullptr, "watcher: acquire timed out");
        rig.device->clear(*image, color);
        PresentTarget target;
        target.window = rig.window->handle();
        require(presenter.present(image, target), "watcher: present skipped");
        rig.window->pump();
    }
    // Idle: no further present. The request is served anyway.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto output = directory / "capture.bmp";
    const auto hwnd = reinterpret_cast<std::uintptr_t>(rig.window->handle());
    write_request(directory, hwnd, output);
    require(wait_for([&] { return std::filesystem::exists(output) || std::filesystem::exists(output.string() + ".error"); }, 5000),
            "watcher: the capture request was not served while idle");
    require(!std::filesystem::exists(output.string() + ".error"), "watcher: capture failed: " + read_file(output.string() + ".error"));
    const auto bmp = read_file(output);
    require(bmp.size() > 54 && bmp[0] == 'B' && bmp[1] == 'M', "watcher: the capture is not a BMP");
    const auto u32 = [&](std::size_t offset) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(bmp[offset])) |
            static_cast<std::uint32_t>(static_cast<unsigned char>(bmp[offset + 1])) << 8 |
            static_cast<std::uint32_t>(static_cast<unsigned char>(bmp[offset + 2])) << 16 |
            static_cast<std::uint32_t>(static_cast<unsigned char>(bmp[offset + 3])) << 24;
    };
    const auto width = u32(18), height = static_cast<std::uint32_t>(-static_cast<std::int32_t>(u32(22)));
    const auto stats = presenter.stats();
    require(width == stats.swapchain_width && height == stats.swapchain_height,
            "watcher: capture is " + std::to_string(width) + "x" + std::to_string(height));
    const auto pixel = 54 + (static_cast<std::size_t>(height / 2) * width + width / 2) * 4;   // BGRA
    const Pixel centre{static_cast<unsigned char>(bmp[pixel + 2]), static_cast<unsigned char>(bmp[pixel + 1]),
                       static_cast<unsigned char>(bmp[pixel]), 255};
    require(close_to(centre, {255, 0, 255, 255}), "watcher: the last presented frame is not the captured one: " + text(centre));

    // A window that never presented: the request stays pending.
    write_request(directory, 4, directory / "never.bmp");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    require(std::filesystem::exists(directory / "request") && !std::filesystem::exists(directory / "never.bmp"),
            "watcher: a request for a window without presents was consumed");
    // The watcher polls the pending request (opens and reads it): on Windows
    // a remove during a poll fails with a sharing violation, so retry.
    require(wait_for([&] {
        std::error_code error;
        std::filesystem::remove(directory / "request", error);
        return !error;
    }, 3000), "watcher: the pending request could not be removed");

    // Exact totals in the stats file.
    const auto wanted = present_totals();
    const auto stats_file = directory / "stats.json";
    const auto expected = "\"presented\":" + std::to_string(wanted.presented) + ",\"skipped\":" + std::to_string(wanted.skipped) + ",";
    require(wait_for([&] { return read_file(stats_file).find(expected) != std::string::npos; }, 3000),
            "watcher: stats file lacks " + expected + ": " + read_file(stats_file));
    require(read_file(stats_file).find("\"window\":" + std::to_string(hwnd) + ",\"presented\":2,") != std::string::npos,
            "watcher: stats file lacks the window's count: " + read_file(stats_file));
    static_cast<void>(presenter.shutdown());
    std::cout << "watcher ok (" << width << "x" << height << ")\n";
}
#endif
} // namespace

int main() {
    try {
        require(layer_pixel_format_problem(pf::bgra8_unorm) == std::nullopt, "BGRA8Unorm rejected");
        require(layer_pixel_format_problem(pf::rgba16_float) == std::nullopt, "RGBA16Float rejected");
        require(layer_pixel_format_problem(70).has_value(), "RGBA8Unorm accepted for a layer");
#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
        // The present watcher: capture requests and the stats file in a
        // private directory (read once, before the first presenter).
        auto directory = std::filesystem::temp_directory_path() /
            ("alchemy-present-watcher-" + std::to_string(GetCurrentProcessId()));
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        _putenv_s("ALCHEMY_PRESENT_CAPTURE", directory.string().c_str());
        _putenv_s("ALCHEMY_PRESENT_STATS", (directory / "stats.json").string().c_str());
        std::string why;
        Rig rig;
        rig.device = test::VulkanTestDevice::create(why);
        if (!rig.device) { std::cout << "SKIP: " << why << '\n'; return 77; }
        rig.window = test::TestWindow::create(320, 240, why);
        if (!rig.window) { std::cout << "SKIP: " << why << '\n'; return 77; }
        presentation(rig);
        pool(rig);
        watcher(rig, directory);
        rig.device->wait_idle();
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
#else
        std::cout << "SKIP: built without Vulkan headers\n";
        return 77;
#endif
        std::cout << "Alchemy GUI QuartzCore presenter tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
