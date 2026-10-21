// Host layer tests for the GUI-only areas (ui, audio, midi; compiled into
// alchemy-fx, so alchemy_host_tests, which links alchemy_core alone, cannot
// reach them). A real backend answers its queries and refuses what does not
// exist without opening anything; on the null backend every call fails with
// Error::unsupported.
#include "alchemy/host/audio.hpp"
#include "alchemy/host/gpu.hpp"
#include "alchemy/host/host.hpp"
#include "alchemy/host/midi.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/sync.hpp"
#include "alchemy/host/time.hpp"
#include "alchemy/host/ui.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

namespace host = alchemy::host;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void null_backend() {
    host::midi::Devices devices;
    require(devices.inputs().error() == host::Error::unsupported && devices.outputs().error() == host::Error::unsupported,
        "null MIDI lists fail");
    require(devices.open_input(0, {}).error() == host::Error::unsupported, "null MIDI input fails");
    require(host::audio::endpoints().error() == host::Error::unsupported, "null audio endpoints fail");
    require(host::ui::monitors().error() == host::Error::unsupported && host::ui::idle_ms().error() == host::Error::unsupported &&
                host::ui::keyboard_kind().error() == host::Error::unsupported &&
                host::ui::reduce_motion().error() == host::Error::unsupported &&
                host::ui::high_contrast().error() == host::Error::unsupported,
        "null ui queries fail");
    require(host::ui::FrameTimer::create().error() == host::Error::unsupported, "null frame timer fails");
    require(host::ui::sample_thread_stack(host::process::current_thread_id(), 4).error() == host::Error::unsupported,
        "null stack sampling fails");
    host::ui::AlertDialog alert;
    alert.buttons = {"OK"};
    require(host::ui::show_alert(alert).error() == host::Error::unsupported &&
                host::ui::run_file_dialog({}).error() == host::Error::unsupported,
        "null dialogs fail");
    require(!host::ui::create_window_server(), "the null backend has no window server");
    require(host::ui::screen_reader().error() == host::Error::unsupported && host::ui::dark_appearance().error() == host::Error::unsupported &&
                host::ui::shell_open("x").error() == host::Error::unsupported && host::ui::desktop_picture().error() == host::Error::unsupported,
        "null shell calls fail");
    require(host::ui::spelling_languages().error() == host::Error::unsupported &&
                host::ui::check_spelling("en-US", u"teh").error() == host::Error::unsupported,
        "null spell checking fails");
    require(host::ui::wait_for_input({}, 0, false).error() == host::Error::unsupported && !host::ui::is_gui_thread(),
        "null run-loop waits fail");
    require(host::gpu::decode_image(std::vector<std::uint8_t>(8, 1)).error() == host::Error::unsupported &&
                host::gpu::vulkan_get_instance_proc_addr().error() == host::Error::not_found,
        "null gpu calls fail");
    require(host::gpu::system_fonts().error() == host::Error::unsupported && !host::gpu::registered_fonts() &&
                host::gpu::layout_text({}).error() == host::Error::unsupported,
        "null fonts fail");
    require(host::gpu::raster_metrics({"Arial", 64}).error() == host::Error::unsupported, "null raster text fails");
}

void real_backend() {
    host::midi::Devices devices;
    const auto inputs = devices.inputs();
    const auto outputs = devices.outputs();
    require(inputs && outputs, "MIDI ports list");
    const std::uint8_t note[] = {0x90, 60, 0};
    require(!devices.open_input(inputs->size() + 1000, {}), "a missing MIDI input is refused");
    require(!devices.send(outputs->size() + 1000, note), "a missing MIDI output is refused");

    const auto endpoints = host::audio::endpoints();
    require(static_cast<bool>(endpoints), "audio endpoints list");
    for (const auto& endpoint : endpoints->endpoints) require(!endpoint.uid.empty(), "audio endpoints have uids");

    const auto monitors = host::ui::monitors();
    require(static_cast<bool>(monitors), "monitors list");
    for (const auto& monitor : *monitors) require(!monitor.primary || (monitor.left == 0 && monitor.top == 0), "the primary monitor is at the origin");
    require(static_cast<bool>(host::ui::idle_ms()) && static_cast<bool>(host::ui::keyboard_kind()), "input queries answer");
    require(static_cast<bool>(host::ui::reduce_motion()) && static_cast<bool>(host::ui::high_contrast()), "accessibility settings answer");

    auto timer = host::ui::FrameTimer::create();
    require(timer && static_cast<bool>(*timer), "a frame timer");
    const auto start = host::time::monotonic_ns();
    require(static_cast<bool>(timer->wait(2'000'000, 1000)), "the frame timer waits");
    const auto waited = host::time::monotonic_ns() - start;
    require(waited >= 1'000'000 && waited < 900'000'000, "the frame timer waits about its delay");
    require(static_cast<bool>(timer->wait(5'000'000'000, 20)), "a wait ends at its limit");
    require(!host::ui::FrameTimer().wait(1, 1), "no timer, no wait");

    // Dialogs (nothing is shown: an alert needs a button).
    require(host::ui::show_alert({}).error() == host::Error::invalid_argument, "an alert without buttons is refused");
    require(host::ui::disable_windows({}, 0).empty(), "no windows, none disabled");
    host::ui::enable_windows({});
    require(static_cast<bool>(host::ui::screen_reader()) && static_cast<bool>(host::ui::desktop_picture()), "shell settings answer");
    require(static_cast<bool>(host::ui::reveal_files({})), "revealing nothing opens nothing");
    // Image files: a PNG round trip; bytes that are no image are refused.
    {
        const host::gpu::Pixels pixels{2, 1, {255, 0, 0, 255, 0, 0, 255, 128}};
        const auto png = host::gpu::encode_image(pixels, host::gpu::ImageFormat::png);
        require(png && png->size() > 8, "encode a PNG");
        const auto decoded = host::gpu::decode_image(*png);
        require(decoded && decoded->width == 2 && decoded->height == 1 && decoded->rgba == pixels.rgba, "the PNG decodes to its pixels");
        require(!host::gpu::decode_image(std::vector<std::uint8_t>(16, 7)), "bytes that are no image");
        // ImageIO's frames: a two-frame GIF; a header read has no pixels.
        const host::gpu::Pixels frames[] = {pixels, pixels};
        const auto gif = host::gpu::encode_image_frames(host::gpu::ImageFormat::gif, frames, 0.85);
        require(gif && host::gpu::image_frame_count(*gif).value_or(0) == 2, "a two-frame GIF");
        const auto header = host::gpu::image_frame(*png, 0, false);
        require(header && header->width == 2 && header->height == 1 && header->rgba.empty() && header->has_alpha, "a PNG frame header");
        const auto frame = host::gpu::image_frame(*png, 0, true);
        require(frame && frame->rgba == pixels.rgba && frame->orientation == 1, "a PNG frame's pixels");
        require(host::gpu::image_frame(*png, 1, false).error() == host::Error::out_of_range, "no second PNG frame");
    }
    // Fonts: a system family, its glyphs, metrics, outlines and tables; a layout and a fallback.
    {
        namespace gpu = host::gpu;
        const auto system = gpu::system_fonts();
        require(system && *system && *system == *gpu::system_fonts(), "the system fonts (one collection)");
        const auto arial = gpu::family_font(*system, "Arial", gpu::font_weight::normal, gpu::font_style::normal, gpu::font_stretch::normal);
        require(static_cast<bool>(arial), "Arial");
        const auto info = arial->info();
        require(info && info->family == "Arial" && info->metrics.units_per_em > 0 && info->metrics.ascent > 0 && !info->monospace,
            "Arial's names and metrics");
        require(arial->same_face(*arial) && arial->glyph_count() > 100, "a face is itself; Arial has glyphs");
        const std::uint32_t letters[] = {'A', 0x10ffff};
        const auto glyphs = arial->glyphs(letters);
        require(glyphs && (*glyphs)[0] != 0 && (*glyphs)[1] == 0, "a glyph for A, none for U+10FFFF");
        const std::uint16_t a[] = {(*glyphs)[0]};
        const auto metrics = arial->glyph_metrics(a);
        require(metrics && (*metrics)[0].advance_width > 0, "A's advance");
        const gpu::Point origin[] = {{0, 0}};
        const auto outline = arial->outlines(12, a, origin, {});
        require(outline && !outline->empty() && outline->front().op == gpu::PathOp::move, "A's outline");
        const auto head = arial->table(0x68656164);   // 'head'
        require(head && head->size() >= 54 && arial->table(0x7a7a7a7a).error() == host::Error::not_found, "font tables");
        const auto named = gpu::named_font(*system, gpu::FontName::postscript, "ArialMT");
        require(named && named->same_face(*arial), "Arial by PostScript name");
        const auto names = gpu::family_names(*system);
        require(names && std::find(names->begin(), names->end(), "Arial") != names->end(), "Arial is a system family");
        const auto faces = gpu::family_faces(*system, "Arial");
        require(faces && faces->size() >= 2, "Arial's faces");
        require(!gpu::family_font(*system, "No Such Family 4711", 400, 0, 5), "an unknown family");
        require(gpu::register_font_file({}).error() == host::Error::invalid_argument &&
                    gpu::register_font_file(std::vector<std::uint8_t>(64, 1)).error() == host::Error::unsupported,
            "font files that are not fonts");

        gpu::TextLayoutRequest request;
        request.text = u"Hello";
        request.base.collection = *system;
        request.base.family = "Arial";
        request.width = request.height = 10000;
        const auto layout = gpu::layout_text(request);
        require(layout && layout->lines.size() == 1 && layout->lines[0].length == 5 && layout->runs.size() == 1 &&
                    layout->runs[0].glyphs.size() == 5 && layout->runs[0].font.same_face(*arial),
            "a one-line layout in Arial");
        const auto fallback = gpu::fallback_font(u"\u4e2d", "Arial", 400, 0, 5);
        require(fallback && fallback->length == 1 && !fallback->font.same_face(*arial), "a fallback face for a CJK character");
    }

    // 2D: a square filled on a surface, read back.
    {
        namespace gpu = host::gpu;
        auto surface = gpu::Surface::create(8, 8);
        require(surface && *surface, "a surface");
        const gpu::PathElement square[] = {{gpu::PathOp::move, {gpu::Point{2, 2}}}, {gpu::PathOp::line, {gpu::Point{6, 2}}},
                                           {gpu::PathOp::line, {gpu::Point{6, 6}}}, {gpu::PathOp::line, {gpu::Point{2, 6}}},
                                           {gpu::PathOp::close, {}}};
        const auto geometry = gpu::Geometry::path(square, false);
        require(geometry && *geometry, "a square geometry");
        require(static_cast<bool>((*surface)->begin()), "begin drawing");
        (*surface)->set_transform({});
        gpu::Brush red;
        red.color = {1, 0, 0, 1};
        require(static_cast<bool>((*surface)->fill(*geometry, red)), "fill the square");
        require(static_cast<bool>((*surface)->end()), "end drawing");
        const auto locked = (*surface)->lock({0, 0, 8, 8}, false);
        require(locked && locked->at(4, 4, {0, 0, 8, 8})[2] == 255 && locked->at(4, 4, {0, 0, 8, 8})[3] == 255 &&
                    locked->at(0, 0, {0, 0, 8, 8})[3] == 0,
            "the square is red, the corner clear");
    }

    // System text rasterizing: Arial at 64 pixels.
    {
        namespace gpu = host::gpu;
        const gpu::RasterFont arial{"Arial", 256};
        const auto metrics = gpu::raster_metrics(arial);
        require(metrics && metrics->ascent > 40 && metrics->descent > 5 && metrics->cap_height, "Arial's raster metrics");
        const auto width = gpu::raster_width(arial, u"Hello");
        const auto prefix = gpu::raster_prefix_widths(arial, u"Hello");
        require(width && *width > 50 && prefix && prefix->size() == 5 && prefix->back() == *width && (*prefix)[0] < (*prefix)[1],
            "advances of a line");
        const auto coverage = gpu::raster_coverage(arial, u"H", 64, 80, 2, 60);
        require(coverage && coverage->size() == 64u * 80u && *std::max_element(coverage->begin(), coverage->end()) > 0.9F,
            "an H is drawn");
    }

    // Vulkan: the loader is the system's (or there is none).
    {
        const auto loader = host::gpu::vulkan_get_instance_proc_addr();
        require(loader ? *loader != nullptr : loader.error() == host::Error::not_found, "the Vulkan loader");
        require(host::gpu::vulkan_surface_extension() == "VK_KHR_win32_surface", "the window surface extension");
        require(host::ui::window_client_size(0).error() == host::Error::not_found, "no window, no client area");
    }

    // The run loop's waits: a signaled event, a timeout, the handle limit.
    {
        const auto event = host::sync::create_event(false, false);
        require(static_cast<bool>(event), "an event");
        const host::OwnedHandle owned(*event);
        const host::Handle handles[] = {*event};
        require(host::ui::wait_for_input(handles, 0, false).error() == host::Error::timed_out, "an unsignaled event times out");
        require(static_cast<bool>(host::sync::set_event(*event)), "set the event");
        const auto woken = host::ui::wait_for_input(handles, 1000, true);
        require(woken && *woken == 0, "a signaled event wakes the wait");
        require(host::ui::wait_limit() >= 2 && host::ui::wait_for_input({}, 0, false).error() == host::Error::invalid_argument,
            "the wait needs a handle");
        require(!host::ui::in_modal_loop(), "not in a modal loop");
    }

    // Spell checking: whatever languages the host has; an unknown one finds nothing.
    const auto languages = host::ui::spelling_languages();
    require(static_cast<bool>(languages), "spelling languages list");
    const auto unknown = host::ui::check_spelling("xx-XX", u"teh qiuck");
    require(unknown && unknown->empty(), "an unknown language finds nothing");
    for (const auto& language : *languages)
        if (language == "en-US") {
            const auto found = host::ui::check_spelling(language, u"the qiuck fox");
            require(found && found->size() == 1 && (*found)[0].start == 4 && (*found)[0].length == 5, "a misspelled word is found");
            const auto guesses = host::ui::spelling_suggestions(language, u"qiuck");
            require(guesses && !guesses->empty(), "suggestions for a misspelled word");
        }
    const auto server = host::ui::create_window_server();
    require(server != nullptr, "a window server");

    // Another thread's host stack, sampled while it sleeps.
    std::atomic<std::uint64_t> sleeper_id{0};
    std::atomic<bool> stop{false};
    std::thread sleeper([&] {
        sleeper_id = host::process::current_thread_id();
        while (!stop) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    });
    while (!sleeper_id) std::this_thread::yield();
    const auto stack = host::ui::sample_thread_stack(sleeper_id, 24);
    stop = true;
    sleeper.join();
    require(stack && !stack->empty() && stack->size() <= 24 && !stack->front().module.empty(), "a sleeping thread's host stack");
    require(host::ui::sample_thread_stack(sleeper_id, 24).error() != host::Error::none, "an exited thread cannot be sampled");
}

} // namespace

int main() {
    try {
        if (host::backend() == "null") null_backend();
        else real_backend();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "host GUI tests passed (" << host::backend() << ")\n";
    return 0;
}
