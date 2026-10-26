#include "alchemy/darwin/coregraphics/q2d_internal.hpp"
#include "alchemy/darwin/coregraphics/quartz2d.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <utility>

// Quartz Display Services (CGDirectDisplay.h, CGDisplayConfiguration.h) over
// the window server's screens (WindowServer::screens(); host::ui's monitors on
// win32). A display's CGDirectDisplayID is its ScreenId, the number
// -[NSScreen deviceDescription] and CVDisplayLink use too. alchemy.exe has no
// window server: coregraphics_headless.cpp answers the same names with no
// displays.
//   lists              CGGet{Active,Online}DisplayList, CGGetDisplaysWith{Point,Rect},
//                      CGMainDisplayID (the primary screen)
//   queries            CGDisplayBounds (global display space: origin at the
//                      top-left of the main display, y down), CGDisplayPixels{Wide,High},
//                      CGDisplayScreenSize, CGDisplayIs{Main,Active,Online,Builtin,Asleep},
//                      CGDisplayMirrorsDisplay (no mirroring), CGDisplayIDToOpenGLDisplayMask
//   modes              CGDisplayCopyDisplayMode and CGDisplayCopyAllDisplayModes
//                      (the current mode only: the host reports no other),
//                      the CGDisplayMode getters, retain and release;
//                      CGDisplaySetDisplayMode accepts only the current mode
//   reconfiguration    CGDisplay{Register,Remove}ReconfigurationCallback keep the
//                      registration; display changes during a run are not
//                      reported through it (AppKit gets them as
//                      NSApplicationDidChangeScreenParametersNotification)
//   screen capture     CGDisplayCreateImage{,ForRect} return NULL (no screen
//                      recording permission, as on macOS without consent)
namespace alchemy::gui::quartz2d {
namespace {

// CGError.h
constexpr std::uint32_t cg_success = 0, cg_illegal_argument = 1001;
// IOGraphicsTypes.h IO32BitDirectPixels, CGDisplayModeCopyPixelEncoding's answer for 8-bit RGB.
constexpr std::string_view direct_32_bit_pixels = "--------RRRRRRRRGGGGGGGGBBBBBBBB";

struct DisplayMode final : ObjectPayload {
    ScreenId display{};
    std::uint32_t width{}, height{}, pixel_width{}, pixel_height{};
    double refresh{};
};

struct Reconfiguration {
    std::mutex mutex;
    std::vector<std::pair<Address, Address>> callbacks;   // (callback, userInfo)
};
Reconfiguration& reconfiguration() {
    static Reconfiguration value;
    return value;
}

std::mutex override_mutex;
std::function<std::vector<ScreenInfo>()> screens_override;

std::vector<ScreenInfo> screens() {
    {
        std::lock_guard lock(override_mutex);
        if (screens_override) return screens_override();
    }
    if (!window_server_available()) return {};
    try {
        return window_server().screens();
    } catch (const std::exception&) {
        return {};   // no display attached
    }
}

const ScreenInfo* find_screen(const std::vector<ScreenInfo>& list, std::uint64_t display) {
    const auto found = std::find_if(list.begin(), list.end(),
                                    [display](const ScreenInfo& info) { return info.id == static_cast<ScreenId>(display); });
    return found == list.end() ? nullptr : &*found;
}
const ScreenInfo* main_screen(const std::vector<ScreenInfo>& list) {
    const auto found = std::find_if(list.begin(), list.end(), [](const ScreenInfo& info) { return info.primary; });
    return found != list.end() ? &*found : list.empty() ? nullptr : &list.front();
}

// The screen's frame in the global display space (AppKit's screens are y up
// from the bottom of the main screen).
Rect display_bounds(const std::vector<ScreenInfo>& list, const ScreenInfo& info) {
    const auto main = main_screen(list);
    const auto top = main ? main->frame.origin.y + main->frame.size.height : 0.0;
    return {{info.frame.origin.x, top - (info.frame.origin.y + info.frame.size.height)}, info.frame.size};
}

bool overlaps(const Rect& a, const Rect& b) {
    return a.origin.x < b.origin.x + b.size.width && b.origin.x < a.origin.x + a.size.width &&
           a.origin.y < b.origin.y + b.size.height && b.origin.y < a.origin.y + a.size.height;
}
bool contains(const Rect& r, Point p) {
    return p.x >= r.origin.x && p.x < r.origin.x + r.size.width && p.y >= r.origin.y && p.y < r.origin.y + r.size.height;
}

// The CGGet*DisplayList calling convention: (maxDisplays, displays, count).
// With displays NULL the count is the number of matching displays.
void write_display_list(CpuState& s, GuestMemory& m, unsigned first, const std::vector<ScreenId>& ids) {
    const auto maximum = static_cast<std::uint32_t>(s.x[first]);
    const auto list = s.x[first + 1], count = s.x[first + 2];
    if (!count) { s.x[0] = cg_illegal_argument; return; }
    std::uint32_t written{};
    for (const auto id : ids) {
        if (list && written >= maximum) break;
        if (list) m.write32(list + 4ULL * written, id);
        ++written;
    }
    m.write32(count, written);
    s.x[0] = cg_success;
}

} // namespace

void set_screens_override(std::function<std::vector<ScreenInfo>()> source) {
    std::lock_guard lock(override_mutex);
    screens_override = std::move(source);
}

void Quartz::install_displays() {
    const auto cg = core_graphics_library;
    const auto mode_class = declare_class("_AlchemyCGDisplayMode");

    // ---- lists ----
    const auto all_displays = [](CpuState& s, GuestMemory& m) {
        std::vector<ScreenId> ids;
        for (const auto& info : screens()) ids.push_back(info.id);
        write_display_list(s, m, 0, ids);
    };
    function(cg, "_CGGetActiveDisplayList", all_displays);
    function(cg, "_CGGetOnlineDisplayList", all_displays);
    // CGGetDisplaysWithPoint(CGPoint, max, displays, count); CGGetDisplaysWithRect(CGRect, max, displays, count).
    function(cg, "_CGGetDisplaysWithPoint", [](CpuState& s, GuestMemory& m) {
        const auto point = point_arg(s);
        const auto list = screens();
        std::vector<ScreenId> ids;
        for (const auto& info : list)
            if (contains(display_bounds(list, info), point)) ids.push_back(info.id);
        write_display_list(s, m, 0, ids);
    });
    function(cg, "_CGGetDisplaysWithRect", [](CpuState& s, GuestMemory& m) {
        const auto rect = standardize(rect_arg(s));
        const auto list = screens();
        std::vector<ScreenId> ids;
        for (const auto& info : list)
            if (overlaps(display_bounds(list, info), rect)) ids.push_back(info.id);
        write_display_list(s, m, 0, ids);
    });
    function(cg, "_CGMainDisplayID", [](CpuState& s, GuestMemory&) {
        const auto list = screens();
        const auto main = main_screen(list);
        s.x[0] = main ? main->id : 0;   // kCGNullDirectDisplay
    });

    // ---- queries ----
    function(cg, "_CGDisplayBounds", [](CpuState& s, GuestMemory&) {
        const auto list = screens();
        const auto info = find_screen(list, s.x[0]);
        return_rect(s, info ? display_bounds(list, *info) : Rect{});   // CGRectZero for an unknown display
    });
    // The display's size in pixels.
    // (Unsure: macOS answers the current mode's width, which is in points on a
    // scaled mode; Alchemy keeps the pixel size it has always answered.)
    for (const bool wide : {true, false})
        function(cg, wide ? "_CGDisplayPixelsWide" : "_CGDisplayPixelsHigh", [wide](CpuState& s, GuestMemory&) {
            const auto list = screens();
            const auto info = find_screen(list, s.x[0]);
            s.x[0] = info ? (wide ? info->pixel_width : info->pixel_height) : 0;
        });
    function(cg, "_CGDisplayScreenSize", [](CpuState& s, GuestMemory&) {
        const auto list = screens();
        const auto info = find_screen(list, s.x[0]);
        return_size(s, info ? Size{info->millimeters_width, info->millimeters_height} : Size{});
    });
    const auto predicate = [this, cg](std::string symbol, bool (*test)(const std::vector<ScreenInfo>&, const ScreenInfo&)) {
        function(cg, std::move(symbol), [test](CpuState& s, GuestMemory&) {
            const auto list = screens();
            const auto info = find_screen(list, s.x[0]);
            return_bool(s, info && test(list, *info));
        });
    };
    predicate("_CGDisplayIsMain", [](const std::vector<ScreenInfo>& list, const ScreenInfo& info) { return &info == main_screen(list); });
    predicate("_CGDisplayIsActive", [](const std::vector<ScreenInfo>&, const ScreenInfo&) { return true; });
    predicate("_CGDisplayIsOnline", [](const std::vector<ScreenInfo>&, const ScreenInfo&) { return true; });
    predicate("_CGDisplayIsBuiltin", [](const std::vector<ScreenInfo>&, const ScreenInfo&) { return false; });
    predicate("_CGDisplayIsAsleep", [](const std::vector<ScreenInfo>&, const ScreenInfo&) { return false; });
    function(cg, "_CGDisplayMirrorsDisplay", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });   // kCGNullDirectDisplay
    function(cg, "_CGDisplayIDToOpenGLDisplayMask", [](CpuState& s, GuestMemory&) {
        const auto list = screens();
        for (std::size_t index = 0; index < list.size() && index < 32; ++index)
            if (list[index].id == static_cast<ScreenId>(s.x[0])) { s.x[0] = 1ULL << index; return; }
        s.x[0] = 0;
    });

    // ---- modes ----
    const auto current_mode = [this, mode_class](std::uint64_t display) -> Address {
        const auto list = screens();
        const auto info = find_screen(list, display);
        if (!info) return 0;
        auto mode = std::make_shared<DisplayMode>();
        mode->display = info->id;
        mode->width = static_cast<std::uint32_t>(std::lround(info->frame.size.width));
        mode->height = static_cast<std::uint32_t>(std::lround(info->frame.size.height));
        mode->pixel_width = info->pixel_width;
        mode->pixel_height = info->pixel_height;
        mode->refresh = info->refresh_rate;
        return create(mode_class, std::move(mode));
    };
    function(cg, "_CGDisplayCopyDisplayMode", [current_mode](CpuState& s, GuestMemory&) { s.x[0] = current_mode(s.x[0]); });
    // CGDisplayCopyAllDisplayModes(display, options): a +1 CFArray of modes.
    function(cg, "_CGDisplayCopyAllDisplayModes", [this, current_mode](CpuState& s, GuestMemory&) {
        const auto mode = current_mode(s.x[0]);
        if (!mode) { s.x[0] = 0; return; }
        const std::array<Address, 1> modes{mode};
        s.x[0] = cf.array(modes);
        release(mode);
    });
    // NULL answers 0, as on macOS; anything else that is not a mode is a caller bug.
    const auto mode_of = [this](Address object) -> std::shared_ptr<DisplayMode> {
        return object ? require<DisplayMode>(object, "CGDisplayModeRef") : nullptr;
    };
    function(cg, "_CGDisplayModeGetWidth", [mode_of](CpuState& s, GuestMemory&) { const auto m = mode_of(s.x[0]); s.x[0] = m ? m->width : 0; });
    function(cg, "_CGDisplayModeGetHeight", [mode_of](CpuState& s, GuestMemory&) { const auto m = mode_of(s.x[0]); s.x[0] = m ? m->height : 0; });
    function(cg, "_CGDisplayModeGetPixelWidth", [mode_of](CpuState& s, GuestMemory&) { const auto m = mode_of(s.x[0]); s.x[0] = m ? m->pixel_width : 0; });
    function(cg, "_CGDisplayModeGetPixelHeight", [mode_of](CpuState& s, GuestMemory&) { const auto m = mode_of(s.x[0]); s.x[0] = m ? m->pixel_height : 0; });
    function(cg, "_CGDisplayModeGetRefreshRate", [mode_of](CpuState& s, GuestMemory&) {
        const auto m = mode_of(s.x[0]);
        set_d(s, 0, m ? m->refresh : 0.0);
    });
    function(cg, "_CGDisplayModeCopyPixelEncoding", [this, mode_of](CpuState& s, GuestMemory&) {
        s.x[0] = mode_of(s.x[0]) ? retain(cf.constant_string(direct_32_bit_pixels)) : 0;
    });
    function(cg, "_CGDisplayModeRetain", [this, mode_of](CpuState& s, GuestMemory&) {
        static_cast<void>(mode_of(s.x[0]));
        s.x[0] = retain(s.x[0]);
    });
    function(cg, "_CGDisplayModeRelease", [this, mode_of](CpuState& s, GuestMemory&) {
        static_cast<void>(mode_of(s.x[0]));
        release(s.x[0]);
    });
    // The current mode is valid, safe and the default (IOGraphicsTypes'
    // kDisplayModeValidFlag | SafeFlag | DefaultFlag), and usable for the desktop.
    function(cg, "_CGDisplayModeGetIOFlags", [mode_of](CpuState& s, GuestMemory&) { s.x[0] = mode_of(s.x[0]) ? 0x7 : 0; });
    function(cg, "_CGDisplayModeGetIODisplayModeID", [mode_of](CpuState& s, GuestMemory&) { s.x[0] = mode_of(s.x[0]) ? 1 : 0; });
    function(cg, "_CGDisplayModeIsUsableForDesktopGUI", [mode_of](CpuState& s, GuestMemory&) { s.x[0] = mode_of(s.x[0]) ? 1 : 0; });
    function(cg, "_CGDisplayModeGetTypeID", [this, current_mode](CpuState& s, GuestMemory&) {
        const auto list = screens();
        const auto* main = main_screen(list);
        const auto probe = main ? current_mode(main->id) : 0;
        s.x[0] = probe ? cf.invoke("CFGetTypeID", std::array<Address, 1>{probe}) : 0;
        release(probe);
    });
    // Displays are never mirrored: each is its own primary display.
    function(cg, "_CGDisplayPrimaryDisplay", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<std::uint32_t>(s.x[0]); });
    // Capturing displays (exclusive use for full-screen drawing) is not
    // available: the window server shares every display. Nothing is ever
    // captured, so releasing succeeds.
    constexpr std::int32_t cg_cannot_complete = 1004;
    for (const auto* name : {"_CGDisplayCapture", "_CGDisplayCaptureWithOptions", "_CGCaptureAllDisplays", "_CGCaptureAllDisplaysWithOptions"})
        function(cg, name, [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<std::uint32_t>(cg_cannot_complete); });
    for (const auto* name : {"_CGDisplayRelease", "_CGReleaseAllDisplays"})
        function(cg, name, [](CpuState& s, GuestMemory&) { s.x[0] = cg_success; });
    // CGDisplaySetDisplayMode(display, mode, options): only the current mode exists.
    function(cg, "_CGDisplaySetDisplayMode", [mode_of](CpuState& s, GuestMemory&) {
        const auto list = screens();
        const auto info = find_screen(list, s.x[0]);
        const auto mode = mode_of(s.x[1]);
        const bool current = info && mode && mode->display == info->id && mode->pixel_width == info->pixel_width &&
                             mode->pixel_height == info->pixel_height;
        s.x[0] = current ? cg_success : cg_illegal_argument;
    });
    // No configuration is ever changed, so there is nothing to restore.
    function(cg, "_CGRestorePermanentDisplayConfiguration", [](CpuState&, GuestMemory&) {});

    // ---- reconfiguration ----
    function(cg, "_CGDisplayRegisterReconfigurationCallback", [](CpuState& s, GuestMemory&) {
        if (!s.x[0]) { s.x[0] = cg_illegal_argument; return; }
        auto& r = reconfiguration();
        std::lock_guard lock(r.mutex);
        r.callbacks.emplace_back(s.x[0], s.x[1]);
        s.x[0] = cg_success;
    });
    function(cg, "_CGDisplayRemoveReconfigurationCallback", [](CpuState& s, GuestMemory&) {
        auto& r = reconfiguration();
        std::lock_guard lock(r.mutex);
        const auto found = std::find(r.callbacks.begin(), r.callbacks.end(), std::pair<Address, Address>{s.x[0], s.x[1]});
        if (found != r.callbacks.end()) r.callbacks.erase(found);
        s.x[0] = cg_success;
    });

    // ---- screen capture ----
    function(cg, "_CGDisplayCreateImage", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    function(cg, "_CGDisplayCreateImageForRect", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
}

} // namespace alchemy::gui::quartz2d
