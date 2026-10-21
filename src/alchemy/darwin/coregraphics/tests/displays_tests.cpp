// Quartz Display Services unit tests (darwin/coregraphics/displays.cpp): the
// display lists, bounds, sizes, modes and callbacks over a fixed screen list,
// then the ids and pixel sizes against host::ui's monitor list through the
// host's own window server.
#include "alchemy/darwin/coregraphics/tests/quartz2d_fixture.hpp"
#include "alchemy/gui/install.hpp"
#include "alchemy/host/ui.hpp"

#include <algorithm>
#include <iostream>
#include <optional>
#include <set>
#include <tuple>

namespace {
using alchemy::gui::Rect;
using alchemy::gui::ScreenInfo;

constexpr Address cg_success = 0, cg_illegal_argument = 1001;

// Listed secondary first: the main display is the primary one, not the first.
std::vector<ScreenInfo> two_screens() {
    ScreenInfo secondary;
    secondary.id = 5;
    secondary.frame = {{-1280, 100}, {1280, 1024}};   // AppKit space: left of the primary, raised 100 points
    secondary.pixel_width = 1280;
    secondary.pixel_height = 1024;
    secondary.refresh_rate = 75;
    ScreenInfo primary;
    primary.id = 2;
    primary.frame = {{0, 0}, {1920, 1080}};
    primary.backing_scale = 2;
    primary.pixel_width = 3840;
    primary.pixel_height = 2160;
    primary.refresh_rate = 60;
    primary.millimeters_width = 600;
    primary.millimeters_height = 340;
    primary.primary = true;
    return {secondary, primary};
}

bool same(const Rect& a, const Rect& b) {
    return close_to(a.origin.x, b.origin.x, 1e-9) && close_to(a.origin.y, b.origin.y, 1e-9) &&
           close_to(a.size.width, b.size.width, 1e-9) && close_to(a.size.height, b.size.height, 1e-9);
}

Rect bounds(Fixture& f, Address display) { return q2d::result_rect(f.call(cg, "_CGDisplayBounds", {display})); }

// (status, ids) of a CGGet*DisplayList-style call with room for `maximum` ids.
std::pair<Address, std::vector<std::uint32_t>> list(Fixture& f, std::string_view name, std::uint32_t maximum,
                                                     std::initializer_list<double> v = {}) {
    const auto ids = f.allocate(4ULL * std::max<std::uint32_t>(maximum, 1)), count = f.allocate(4);
    f.engine.write32(count, 77);
    const auto status = f.call(cg, name, {maximum, ids, count}, v).x0;
    std::vector<std::uint32_t> result;
    for (std::uint32_t index = 0; index < f.engine.read32(count) && index < maximum; ++index) result.push_back(f.engine.read32(ids + 4ULL * index));
    return {status, result};
}

void fixed_screens(Fixture& f) {
    q2d::set_screens_override(two_screens);

    // Lists: every display, the count alone with a NULL list, at most maxDisplays.
    for (const auto* name : {"_CGGetActiveDisplayList", "_CGGetOnlineDisplayList"}) {
        const auto [status, ids] = list(f, name, 8);
        require(status == cg_success && ids == std::vector<std::uint32_t>{5, 2}, std::string(name) + " ids");
        const auto count = f.allocate(4);
        require(f.call(cg, name, {0, 0, count}).x0 == cg_success && f.engine.read32(count) == 2, std::string(name) + " count");
        require(list(f, name, 1).second == std::vector<std::uint32_t>{5}, std::string(name) + " maxDisplays");
        require(f.call(cg, name, {8, f.allocate(32), 0}).x0 == cg_illegal_argument, std::string(name) + " NULL count");
    }
    require(f.id(cg, "_CGMainDisplayID") == 2, "CGMainDisplayID is the primary screen");

    // Global display space: the main display's top-left is the origin, y down.
    require(same(bounds(f, 2), {{0, 0}, {1920, 1080}}), "CGDisplayBounds(main)");
    require(same(bounds(f, 5), {{-1280, 1080 - (100 + 1024)}, {1280, 1024}}), "CGDisplayBounds(secondary)");
    require(same(bounds(f, 9), {{0, 0}, {0, 0}}), "CGDisplayBounds of an unknown display is CGRectZero");
    require(f.id(cg, "_CGDisplayPixelsWide", {2}) == 3840 && f.id(cg, "_CGDisplayPixelsHigh", {2}) == 2160 &&
                f.id(cg, "_CGDisplayPixelsWide", {5}) == 1280 && f.id(cg, "_CGDisplayPixelsHigh", {9}) == 0,
            "CGDisplayPixelsWide/High");
    const auto size = q2d::result_size(f.call(cg, "_CGDisplayScreenSize", {2}));
    require(size.width == 600 && size.height == 340, "CGDisplayScreenSize");

    require(f.id(cg, "_CGDisplayIsMain", {2}) == 1 && f.id(cg, "_CGDisplayIsMain", {5}) == 0, "CGDisplayIsMain");
    require(f.id(cg, "_CGDisplayIsActive", {5}) == 1 && f.id(cg, "_CGDisplayIsOnline", {2}) == 1 &&
                f.id(cg, "_CGDisplayIsActive", {9}) == 0, "CGDisplayIsActive/Online");
    require(f.id(cg, "_CGDisplayIsBuiltin", {2}) == 0 && f.id(cg, "_CGDisplayIsAsleep", {2}) == 0, "CGDisplayIsBuiltin/Asleep");
    require(f.id(cg, "_CGDisplayMirrorsDisplay", {5}) == 0, "CGDisplayMirrorsDisplay");
    require(f.id(cg, "_CGDisplayIDToOpenGLDisplayMask", {5}) == 1 && f.id(cg, "_CGDisplayIDToOpenGLDisplayMask", {2}) == 2 &&
                f.id(cg, "_CGDisplayIDToOpenGLDisplayMask", {9}) == 0, "CGDisplayIDToOpenGLDisplayMask");

    // Displays at a point and in a rect (global display space).
    require(list(f, "_CGGetDisplaysWithPoint", 8, {10, 10}).second == std::vector<std::uint32_t>{2}, "CGGetDisplaysWithPoint(main)");
    require(list(f, "_CGGetDisplaysWithPoint", 8, {-10, -30}).second == std::vector<std::uint32_t>{5}, "CGGetDisplaysWithPoint(secondary)");
    require(list(f, "_CGGetDisplaysWithPoint", 8, {5000, 0}).second.empty(), "CGGetDisplaysWithPoint(outside)");
    require(list(f, "_CGGetDisplaysWithRect", 8, {-100, -100, 200, 200}).second == std::vector<std::uint32_t>{5, 2}, "CGGetDisplaysWithRect(both)");
    require(list(f, "_CGGetDisplaysWithRect", 8, {100, 100, -50, -50}).second == std::vector<std::uint32_t>{2}, "CGGetDisplaysWithRect(negative size)");

    // The current mode.
    const auto mode = f.id(cg, "_CGDisplayCopyDisplayMode", {2});
    require(mode != 0, "CGDisplayCopyDisplayMode");
    require(f.id(cg, "_CGDisplayModeGetWidth", {mode}) == 1920 && f.id(cg, "_CGDisplayModeGetHeight", {mode}) == 1080 &&
                f.id(cg, "_CGDisplayModeGetPixelWidth", {mode}) == 3840 && f.id(cg, "_CGDisplayModeGetPixelHeight", {mode}) == 2160,
            "CGDisplayMode sizes");
    require(f.call(cg, "_CGDisplayModeGetRefreshRate", {mode}).d(0) == 60, "CGDisplayModeGetRefreshRate");
    const auto encoding = f.id(cg, "_CGDisplayModeCopyPixelEncoding", {mode});
    require(f.cf.utf8(encoding) == "--------RRRRRRRRGGGGGGGGBBBBBBBB", "CGDisplayModeCopyPixelEncoding");
    f.process.objects().release(encoding);
    require(f.id(cg, "_CGDisplaySetDisplayMode", {2, mode, 0}) == cg_success, "CGDisplaySetDisplayMode(current mode)");
    require(f.id(cg, "_CGDisplaySetDisplayMode", {5, mode, 0}) == cg_illegal_argument, "CGDisplaySetDisplayMode(another display's mode)");
    require(f.id(cg, "_CGDisplayModeRetain", {mode}) == mode, "CGDisplayModeRetain");
    f.call(cg, "_CGDisplayModeRelease", {mode});
    f.call(cg, "_CGDisplayModeRelease", {mode});
    f.call(cg, "_CGDisplayModeRelease", {0});
    require(f.id(cg, "_CGDisplayModeGetWidth", {0}) == 0 && f.call(cg, "_CGDisplayModeGetRefreshRate", {0}).d(0) == 0, "NULL mode");
    require(f.id(cg, "_CGDisplayCopyDisplayMode", {9}) == 0, "CGDisplayCopyDisplayMode of an unknown display");
    const auto modes = f.id(cg, "_CGDisplayCopyAllDisplayModes", {5, 0});
    const auto values = f.cf.array_values(modes);
    require(values.size() == 1 && f.id(cg, "_CGDisplayModeGetPixelWidth", {values.front()}) == 1280 &&
                f.call(cg, "_CGDisplayModeGetRefreshRate", {values.front()}).d(0) == 75, "CGDisplayCopyAllDisplayModes");
    f.process.objects().release(modes);

    // Reconfiguration callbacks and screen capture.
    require(f.id(cg, "_CGDisplayRegisterReconfigurationCallback", {0, 0}) == cg_illegal_argument, "NULL reconfiguration callback");
    require(f.id(cg, "_CGDisplayRegisterReconfigurationCallback", {0x1000, 7}) == cg_success &&
                f.id(cg, "_CGDisplayRemoveReconfigurationCallback", {0x1000, 7}) == cg_success, "reconfiguration callbacks");
    require(f.id(cg, "_CGDisplayCreateImage", {2}) == 0 && f.id(cg, "_CGDisplayCreateImageForRect", {2}, {0, 0, 10, 10}) == 0,
            "CGDisplayCreateImage needs screen recording consent");

    q2d::set_screens_override({});
}

// The ids and pixel sizes the host's window server reports for its screens,
// against host::ui's monitor list.
void host_screens(Fixture& f) {
    alchemy::gui::InstallContext context{f.process, f.objc, f.cf};
    alchemy::gui::install_windowserver(context);
    if (!alchemy::gui::window_server_available()) {
        std::cout << "displays: host screens skipped (no interactive desktop)\n";
        return;
    }
    const auto monitors = alchemy::host::ui::monitors();
    require(static_cast<bool>(monitors), "host::ui::monitors failed");
    // Clone (mirrored) paths share one desktop rectangle: one display each.
    std::set<std::tuple<std::int32_t, std::int32_t, std::uint32_t, std::uint32_t>> desktop;
    std::multiset<std::pair<std::uint32_t, std::uint32_t>> sizes;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> primary;
    for (const auto& monitor : *monitors) {
        if (!desktop.emplace(monitor.left, monitor.top, monitor.width, monitor.height).second) continue;
        sizes.emplace(monitor.width, monitor.height);
        if (monitor.primary) primary = std::pair{monitor.width, monitor.height};
    }

    const auto [status, ids] = list(f, "_CGGetActiveDisplayList", 32);
    require(status == cg_success && ids.size() == sizes.size(),
            "CGGetActiveDisplayList: " + std::to_string(ids.size()) + " displays, host::ui has " + std::to_string(sizes.size()));
    require(list(f, "_CGGetOnlineDisplayList", 32).second == ids, "CGGetOnlineDisplayList == CGGetActiveDisplayList");
    std::multiset<std::pair<std::uint32_t, std::uint32_t>> reported;
    for (const auto id : ids) {
        require(id != 0, "a display id is kCGNullDirectDisplay");
        reported.emplace(static_cast<std::uint32_t>(f.id(cg, "_CGDisplayPixelsWide", {id})),
                         static_cast<std::uint32_t>(f.id(cg, "_CGDisplayPixelsHigh", {id})));
        const auto mode = f.id(cg, "_CGDisplayCopyDisplayMode", {id});
        require(mode != 0 && f.id(cg, "_CGDisplayModeGetPixelWidth", {mode}) == f.id(cg, "_CGDisplayPixelsWide", {id}),
                "CGDisplayCopyDisplayMode of display " + std::to_string(id));
        f.call(cg, "_CGDisplayModeRelease", {mode});
    }
    require(reported == sizes, "CGDisplayPixelsWide/High differ from host::ui's monitor sizes");
    const auto main = static_cast<std::uint32_t>(f.id(cg, "_CGMainDisplayID"));
    require(std::find(ids.begin(), ids.end(), main) != ids.end(), "CGMainDisplayID is not in the display list");
    require(f.id(cg, "_CGDisplayIsMain", {main}) == 1, "CGDisplayIsMain(CGMainDisplayID())");
    const auto main_bounds = bounds(f, main);
    require(main_bounds.origin.x == 0 && main_bounds.origin.y == 0, "the main display is not at the global origin");
    if (primary)
        require(f.id(cg, "_CGDisplayPixelsWide", {main}) == primary->first && f.id(cg, "_CGDisplayPixelsHigh", {main}) == primary->second,
                "the main display is not host::ui's primary monitor");
    std::cout << "displays: " << ids.size() << " host screen(s), main " << main << "\n";
}
} // namespace

int main() {
    try {
        Fixture f;
        fixed_screens(f);
        host_screens(f);
        std::cout << "displays: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "displays: FAILED: " << error.what() << "\n";
        return 1;
    }
}
