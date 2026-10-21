#include "alchemy/darwin/appkit/appkit_internal.hpp"
#include "alchemy/gui/drawn_text.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

// Diagnostic hooks for unattended GUI runs. DIAGNOSTIC ONLY: nothing here
// changes behaviour unless the variables are set. No input is injected into
// the desktop: no window-server or OS input is synthesized, nothing reaches
// other processes, and cloaked windows stay cloaked.
//
// ALCHEMY_CONTENTS_CAPTURE=<directory>: besides the layer-contents capture
// of support kit (window-<n>.bmp), every presentation of a window backing store
// (the -drawRect: windows of AppKit controls and custom views) writes
// <directory>/backing-<windowNumber>.bmp: a top-down 32-bit BMP of the
// backing store's device pixels (premultiplied BGRA), rewritten at most every
// 250 ms, plus backing-<windowNumber>.txt.
//
// ALCHEMY_GUI_AUTOPRESS=<step>|<step>|...: presses controls in order, from
// inside the process. A step is
//   <title>   an NSButton (push button, checkbox, radio button) with this
//             title, or else a custom-drawn view (a wxWidgets wxNSView
//             button) that drew exactly this text in its last -drawRect:
//             (titles compare case-insensitively, ignoring '&', '_' and a
//             trailing "...", U+2026 or ':')
//   #<n>=<text>  sets the n-th (1-based, view order) visible editable
//             NSTextField's value and sends its delegate
//             -controlTextDidChange: (test values such as a license host)
//   @<x>,<y>  the view at this point (window base coordinates, points, origin
//             bottom left) of the front visible window
//   ?<step>   optional: skipped as soon as a later step's control shows (the
//             next one, or past further optional steps)
//             (a last optional step: when not found within 10 s)
// A main-thread timer looks for the step's control in the visible windows.
// Once it has been present for ALCHEMY_GUI_AUTOPRESS_SETTLE seconds (default
// 2) and its window presented nothing for a second, the window is captured as
// <capture directory>/page-<NN>-<step>.bmp (with ALCHEMY_CONTENTS_CAPTURE)
// and the control is pressed: an NSButton receives -performClick: (its action
// is sent exactly as for a mouse click); any other view gets a left mouse
// down and up at its center, posted to the application's own event queue
// (as -[NSApplication postEvent:atStart:] does) and routed by -sendEvent: as
// usual. While waiting for a step longer than 10 s the visible buttons are
// listed on stderr (once per change; ALCHEMY_GUI_AUTOPRESS_VERBOSE=1 lists
// every visible view's class, frame and drawn text), and the front window is
// captured every 20 s as page-<NN>-wait-<k>.bmp (an installation's progress
// page). When the sequence is done the remaining windows are captured as
// page-<NN>-final.bmp.
namespace alchemy::gui {
namespace {
std::atomic<DrawnTextSink> drawn_text_sink{nullptr};
}
void set_drawn_text_sink(DrawnTextSink sink) noexcept { drawn_text_sink.store(sink, std::memory_order_release); }
void report_drawn_text(std::u16string_view text) noexcept {
    if (const auto sink = drawn_text_sink.load(std::memory_order_relaxed)) sink(text);
}
} // namespace alchemy::gui

namespace alchemy::gui::appkit {
namespace {

void write_bmp(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height, std::size_t stride,
               const std::uint8_t* bgra) {
    const auto temporary = std::filesystem::path(path).concat(".tmp");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        const std::uint32_t row = width * 4;
        const std::uint32_t image_size = row * height;
        const std::uint32_t file_size = 54 + image_size;
        std::uint8_t header[54]{};
        header[0] = 'B'; header[1] = 'M';
        std::memcpy(header + 2, &file_size, 4);
        const std::uint32_t offset = 54, info_size = 40;
        std::memcpy(header + 10, &offset, 4);
        std::memcpy(header + 14, &info_size, 4);
        const std::int32_t w = static_cast<std::int32_t>(width), h = -static_cast<std::int32_t>(height);
        std::memcpy(header + 18, &w, 4);
        std::memcpy(header + 22, &h, 4);
        const std::uint16_t planes = 1, bits = 32;
        std::memcpy(header + 26, &planes, 2);
        std::memcpy(header + 28, &bits, 2);
        std::memcpy(header + 34, &image_size, 4);
        out.write(reinterpret_cast<const char*>(header), sizeof header);
        for (std::uint32_t y = 0; y < height; ++y)
            out.write(reinterpret_cast<const char*>(bgra + static_cast<std::size_t>(y) * stride), row);
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) std::filesystem::remove(temporary, error);
}

const std::filesystem::path& capture_directory() {
    static const std::filesystem::path directory = [] {
        const auto value = host_environment("ALCHEMY_CONTENTS_CAPTURE");
        std::filesystem::path result(value);
        std::error_code error;
        if (!result.empty()) std::filesystem::create_directories(result, error);
        return result;
    }();
    return directory;
}

std::string normalize(std::string_view title) {
    std::string out;
    for (const char c : title) {
        if (c == '&' || c == '_') continue;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    const auto trim_back = [&] { while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back(); };
    trim_back();
    if (out.size() >= 3 && out.compare(out.size() - 3, 3, "...") == 0) out.resize(out.size() - 3);
    if (out.size() >= 3 && out.compare(out.size() - 3, 3, "\xE2\x80\xA6") == 0) out.resize(out.size() - 3);   // U+2026
    if (!out.empty() && out.back() == ':') out.pop_back();
    trim_back();
    std::size_t start = 0;
    while (start < out.size() && std::isspace(static_cast<unsigned char>(out[start]))) ++start;
    return out.substr(start);
}

std::string file_safe(std::string_view title) {
    std::string out;
    for (const char c : title) out.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    if (out.size() > 40) out.resize(40);
    return out;
}

AppKit* drawn_text_owner{};   // the AppKit whose autopress collects drawn labels (main thread)
std::thread::id drawn_text_thread{};

} // namespace

struct AppKit::Autopress {
    struct Step {
        std::string title, wanted;
        bool optional{};
        std::optional<Point> point;   // @x,y
        std::size_t field{};          // #n=text: the n-th (1-based) visible editable NSTextField
        std::string field_text;
    };
    std::vector<Step> steps;
    std::size_t next{};
    double settle{2.0};
    TimerId timer{};
    Address candidate{};           // the control seen for the current step
    double candidate_since{};
    double step_since{};
    unsigned page{};
    std::string last_listing;
    bool listed{};
    double last_wait_capture{};
    unsigned wait_captures{};
    bool finished{};
};

void AppKit::note_drawn_text(std::u16string_view text) {
    if (!drawing_view || text.empty()) return;
    drawn_labels[drawing_view].emplace_back(text);
}

void AppKit::capture_backing(Address target, const std::string& stem) {
    const auto& directory = capture_directory();
    if (directory.empty()) return;
    const auto state = window_state(target);
    const auto backing = state->backing;
    if (!backing || !backing->pixels) return;
    std::vector<std::uint8_t> pixels(backing->stride * backing->height);
    memory().read_bytes(backing->pixels, std::span(pixels.data(), pixels.size()));
    write_bmp(directory / (stem + ".bmp"), backing->width, backing->height, backing->stride, pixels.data());
    std::ofstream info(directory / (stem + ".txt"), std::ios::trunc);
    info << "window " << state->id << " width " << backing->width << " height " << backing->height << " scale "
         << backing->scale << " title \"" << (state->title ? text(state->title) : std::string()) << "\"\n";
}

void AppKit::capture_presented(Address target) {
    const auto state = window_state(target);
    if (!state->id) return;
    last_present_time[state->id] = monotonic_seconds();
    ++present_count[state->id];
    if (capture_directory().empty()) return;
    auto& [frames, last] = capture_throttle[state->id];
    const auto now = monotonic_seconds();
    ++frames;
    if (frames > 3 && now - last < 0.25) {
        pending_capture.insert(target);
        return;
    }
    last = now;
    pending_capture.erase(target);
    capture_backing(target, "backing-" + std::to_string(state->id));
}

void AppKit::start_autopress() {
    if (autopress) return;
    const auto sequence = host_environment("ALCHEMY_GUI_AUTOPRESS");
    const bool capturing = !capture_directory().empty();
    if (sequence.empty() && !capturing) return;
    const auto loop = run_loop_or_null();
    if (!loop) return;
    autopress = std::make_shared<Autopress>();
    if (const auto settle = host_environment("ALCHEMY_GUI_AUTOPRESS_SETTLE"); !settle.empty()) {
        try { autopress->settle = std::stod(settle); } catch (...) { fail("ALCHEMY_GUI_AUTOPRESS_SETTLE must be a number of seconds"); }
    }
    std::size_t start = 0;
    while (start <= sequence.size() && !sequence.empty()) {
        const auto end = std::min(sequence.find('|', start), sequence.size());
        auto title = sequence.substr(start, end - start);
        Autopress::Step step;
        if (!title.empty() && title.front() == '?') { step.optional = true; title.erase(0, 1); }
        step.title = title;
        if (!title.empty() && title.front() == '#') {
            const auto equals = title.find('=');
            std::size_t index{};
            if (equals == std::string::npos || std::sscanf(title.c_str() + 1, "%zu", &index) != 1 || index == 0)
                fail("ALCHEMY_GUI_AUTOPRESS: bad text step \"" + title + "\" (expected #<n>=<text>)");
            step.field = index;
            step.field_text = title.substr(equals + 1);
            step.wanted = title;
        } else if (!title.empty() && title.front() == '@') {
            double x{}, y{};
            if (std::sscanf(title.c_str() + 1, "%lf,%lf", &x, &y) != 2) fail("ALCHEMY_GUI_AUTOPRESS: bad point step \"" + title + "\"");
            step.point = Point{x, y};
            step.wanted = title;
        } else {
            step.wanted = normalize(title);
        }
        if (!step.wanted.empty()) autopress->steps.push_back(std::move(step));
        start = end + 1;
    }
    if (!autopress->steps.empty()) {
        std::fprintf(stderr, "Alchemy autopress (diagnostic): %zu press(es) scheduled\n", autopress->steps.size());
        drawn_text_owner = this;
        drawn_text_thread = std::this_thread::get_id();
        set_drawn_text_sink([](std::u16string_view text) {
            if (drawn_text_owner && std::this_thread::get_id() == drawn_text_thread) drawn_text_owner->note_drawn_text(text);
        });
    }
    autopress->step_since = monotonic_seconds();
    autopress->timer = loop->add_timer(monotonic_seconds() + 0.25, 0.25, common_modes, [this](TimerId) {
        try { autopress_tick(); } catch (const std::exception& error) {
            std::fprintf(stderr, "Alchemy autopress: %s\n", error.what());
        }
    });
}

// Left mouse down and up at the center of `value`, through the application's
// event queue (the -postEvent:atStart: path).
void AppKit::autopress_click_view(Address target, Address value) {
    const auto frame = view_rect_in_window(value, view_bounds(*view_state(value)));
    Event down;
    down.kind = EventKind::input;
    down.type = event_type::left_mouse_down;
    down.window = window_state(target)->id;
    down.timestamp = monotonic_seconds();
    down.location = {frame.origin.x + frame.size.width / 2, frame.origin.y + frame.size.height / 2};
    down.screen_location = down.location;
    down.click_count = 1;
    down.button_number = 0;
    down.pressed_buttons = 1;
    down.pressure = 1;
    Event up = down;
    up.type = event_type::left_mouse_up;
    up.pressed_buttons = 0;
    up.pressure = 0;
    up.timestamp = down.timestamp + 0.05;
    enqueue(make_event(down, target), false);
    enqueue(make_event(up, target), false);
    if (const auto loop = run_loop_or_null()) loop->wake();
}

void AppKit::autopress_tick() {
    auto& a = *autopress;
    // Throttled backing captures that are due.
    if (!pending_capture.empty()) {
        const auto due = pending_capture;
        for (const auto target : due) {
            if (objc.find_host_state<WindowState>(target)) capture_presented(target);
            else pending_capture.erase(target);
        }
    }
    if (a.finished || a.next >= a.steps.size()) {
        if (!a.finished && !a.steps.empty()) {
            // Final capture once the last press settled.
            if (monotonic_seconds() - a.step_since < a.settle) return;
            a.finished = true;
            for (const auto target : std::vector<Address>(window_order)) {
                const auto state = window_state(target);
                if (state->visible && !state->closed && state->backing)
                    { ++a.page; capture_backing(target, "page-" + std::string(a.page < 10 ? "0" : "") + std::to_string(a.page) + "-final"); }
            }
            std::fprintf(stderr, "Alchemy autopress: sequence done\n");
        }
        return;
    }
    const auto& step = a.steps[a.next];
    const auto now = monotonic_seconds();
    static const bool verbose = !host_environment("ALCHEMY_GUI_AUTOPRESS_VERBOSE").empty();
    const auto visible = [this](Address value) {
        for (auto current = value; current; current = view_state(current)->superview)
            if (view_state(current)->hidden) return false;
        return true;
    };
    const auto labels_of = [this](Address value) {
        std::vector<std::string> out;
        if (const auto found = drawn_labels.find(value); found != drawn_labels.end())
            for (const auto& label : found->second) out.push_back(drawing::utf8(label));
        return out;
    };
    // Find a step's control (and, with `titles`, list what is visible).
    const auto search = [&](const Autopress::Step& which, std::vector<std::string>* titles) -> std::pair<Address, Address> {
        Address found{}, found_window{};
        bool found_button = false;
        double found_area = 0;
        for (const auto target : std::vector<Address>(window_order)) {
            const auto state = window_state(target);
            if (!state->visible || state->closed || !state->content_view) continue;
            if (titles) titles->push_back("window " + std::to_string(state->id) + ":");
            if (which.field) {
                std::size_t seen = 0;
                for (const auto value : all_views(state->content_view)) {
                    if (!visible(value) || !is_kind(value, text_field) || !send_bool(value, "isEditable")) continue;
                    if (++seen == which.field) { found = value; found_window = target; break; }
                }
                continue;
            }
            if (which.point) {
                if (found) continue;   // the front window only
                CallRegs registers;
                put_point(registers, 0, *which.point);
                const auto hit = send(state->content_view, "hitTest:", registers).x0;
                if (hit) { found = hit; found_window = target; }
                continue;
            }
            for (const auto value : all_views(state->content_view)) {
                if (!visible(value)) continue;
                const auto frame = view_rect_in_window(value, view_bounds(*view_state(value)));
                if (is_kind(value, button)) {
                    const auto title = text(send(value, "title").x0);
                    const bool enabled = (send(value, "isEnabled").x0 & 0xff) != 0;
                    if (titles) titles->push_back(title + (enabled ? "" : " (disabled)"));
                    if ((!found || !found_button) && enabled && normalize(title) == which.wanted) {
                        found = value; found_window = target; found_button = true;
                    }
                    continue;
                }
                const auto labels = labels_of(value);
                if (titles && verbose) {
                    char buffer[96];
                    std::snprintf(buffer, sizeof buffer, "@%.0f,%.0f %.0fx%.0f", frame.origin.x, frame.origin.y, frame.size.width, frame.size.height);
                    std::string entry = text(send(value, "className").x0) + buffer;
                    for (const auto& label : labels) entry += " '" + label + "'";
                    titles->push_back(entry);
                } else if (titles && !labels.empty()) {
                    titles->push_back("'" + labels.front() + "'");
                }
                if (found_button) continue;
                for (const auto& label : labels) {
                    if (normalize(label) != which.wanted) continue;
                    // The smallest view that drew the label.
                    const auto area = frame.size.width * frame.size.height;
                    if (!found || area < found_area) { found = value; found_window = target; found_area = area; }
                }
            }
        }
        return {found, found_window};
    };
    std::vector<std::string> titles;
    const auto [found, found_window] = search(step, &titles);
    if (!found) {
        a.candidate = 0;
        // Long waits (an installation's progress page): the front window
        // every 20 s as page-<NN>-wait-<k>.
        if (now - a.step_since > 10 && now - a.last_wait_capture > 20) {
            a.last_wait_capture = now;
            for (const auto target : std::vector<Address>(window_order)) {
                const auto state = window_state(target);
                if (!state->visible || state->closed || !state->backing) continue;
                capture_backing(target, "page-" + std::string(a.page < 9 ? "0" : "") + std::to_string(a.page + 1) + "-wait-" +
                                            std::string(a.wait_captures < 9 ? "0" : "") + std::to_string(++a.wait_captures));
                break;
            }
        }
        // An optional step is skipped as soon as a later step's control shows.
        const auto later_showing = [&]() -> const Autopress::Step* {
            for (auto index = a.next + 1; index < a.steps.size(); ++index) {
                if (search(a.steps[index], nullptr).first) return &a.steps[index];
                if (!a.steps[index].optional) break;
            }
            return nullptr;
        };
        const auto* later = step.optional ? later_showing() : nullptr;
        if (later) {
            std::fprintf(stderr, "Alchemy autopress: optional \"%s\" skipped (\"%s\" is showing)\n", step.title.c_str(),
                         later->title.c_str());
            ++a.next;
            a.step_since = now;
            return;
        }
        if (now - a.step_since > 10) {
            std::string listing;
            for (const auto& title : titles) listing += " [" + title + "]";
            if (listing != a.last_listing || !a.listed) {
                a.last_listing = listing;
                a.listed = true;
                std::fprintf(stderr, "Alchemy autopress: waiting for \"%s\"; visible:%s\n", step.title.c_str(),
                             listing.empty() ? " none" : listing.c_str());
            }
            if (step.optional && a.next + 1 >= a.steps.size()) {
                std::fprintf(stderr, "Alchemy autopress: optional \"%s\" not found, skipped\n", step.title.c_str());
                ++a.next;
                a.step_since = now;
            }
        }
        return;
    }
    if (found != a.candidate) { a.candidate = found; a.candidate_since = now; return; }
    const auto window_id = window_state(found_window)->id;
    const auto presented = last_present_time.contains(window_id) ? last_present_time[window_id] : 0.0;
    // Settled: present for `settle` seconds and no presentation for 0.3 s
    // (a window that keeps presenting is taken as it is after 4 x settle).
    if (now - a.candidate_since < a.settle) return;
    if (now - presented < 0.3 && now - a.candidate_since < 4 * a.settle) return;
    // Settled: capture the page, then press.
    display_window(found_window);
    ++a.page;
    capture_backing(found_window, "page-" + std::string(a.page < 10 ? "0" : "") + std::to_string(a.page) + "-" + file_safe(step.title));
    const bool push = is_kind(found, button) && !step.field;
    std::fprintf(stderr, "Alchemy autopress: pressing \"%s\" (page %u, %s %s; %.1f s after the previous press, %llu presentations)\n",
                 step.title.c_str(), a.page, text(send(found, "className").x0).c_str(),
                 step.field ? "text entry" : push ? "-performClick:" : "mouse down/up at its center", now - a.step_since,
                 static_cast<unsigned long long>(present_count[window_id]));
    std::fflush(stderr);
    ++a.next;
    a.candidate = 0;
    a.step_since = now;
    a.last_listing.clear();
    a.listed = false;
    a.wait_captures = 0;
    if (step.field) {
        // Text entry: the field's value, then the change notification its
        // delegate observes (NSControlTextDidChangeNotification), as typing
        // would send.
        static_cast<void>(send_id(found, "setStringValue:", {auto_string(step.field_text)}));
        const auto delegate = send(found, "delegate").x0;
        if (delegate && responds(delegate, "controlTextDidChange:")) {
            const auto notification_class = objc.find_class("NSNotification");
            const auto note = send_id(notification_class, "notificationWithName:object:",
                                      {auto_string("NSControlTextDidChangeNotification"), found});
            static_cast<void>(send_id(delegate, "controlTextDidChange:", {note}));
        }
        invalidate_view(found);
        return;
    }
    if (!push) { autopress_click_view(found_window, found); return; }
    // The click runs from a one-shot timer, so a modal loop it enters keeps
    // ticking the repeating timer (which is then not in its callout).
    const auto button_object = retain(found);
    static_cast<void>(run_loop_or_null()->add_timer(now, 0, common_modes, [this, button_object](TimerId) {
        try { static_cast<void>(send_id(button_object, "performClick:", {0})); } catch (...) { release(button_object); throw; }
        release(button_object);
    }));
}

} // namespace alchemy::gui::appkit
