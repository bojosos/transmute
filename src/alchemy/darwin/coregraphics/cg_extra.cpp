#include "alchemy/darwin/kit/support/support_kit.hpp"

#include <algorithm>
#include <cmath>

// CoreGraphics event-tap functions Qt's Cocoa plugin imports that no other
// provider exports (each is installed only when missing). The CGDisplay*
// functions it imports are Quartz's (darwin/coregraphics/displays.cpp).
//   event taps         CGEventTapCreateForPid creates a listen-only tap object
//                      whose run-loop source (CFMachPortCreateRunLoopSource)
//                      never fires: synthesized events are not observed by
//                      taps. Enabling a tap fails explicitly.
//   CGEventPost: unsupported (no system-wide synthetic input; events are made
//   in graphics_gaps.cpp, and CGEventPostToPid reaches this process).
namespace alchemy::gui::support_kit {
namespace {

struct EventTap final : ObjectPayload {
    Address source{};   // +1 CFRunLoopSource (created on first request)
    bool enabled{};
};
} // namespace

void install_cg_extra(Support& support) {
    auto* s = &support;
    const auto cg = core_graphics_library;
    const auto tap_class = support.declare_class("_AlchemyCGEventTap", "NSObject");

    // Event taps: listen-only, never fed.
    support.fill_function(cg, "_CGEventTapCreateForPid", [s, tap_class](CpuState& c, GuestMemory&) {
        if (c.x[2] != 1 /* kCGEventTapOptionListenOnly */) unsupported("CGEventTapCreateForPid with a filtering (non listen-only) tap");
        auto tap = std::make_shared<EventTap>();
        tap->enabled = true;
        c.x[0] = s->create(tap_class, tap, [weak = std::weak_ptr<Support>(s->shared_from_this())](Address self) {
            const auto keeper = weak.lock();
            if (!keeper) return;
            if (const auto state = keeper->objc.find_host_state<EventTap>(self)) keeper->release(std::exchange(state->source, 0));
        });
    });
    support.fill_function(cg, "_CGEventTapEnable", [s](CpuState& c, GuestMemory&) {
        const auto tap = c.x[0] ? s->objc.find_host_state<EventTap>(c.x[0]) : nullptr;
        if (!tap) fail("CGEventTapEnable: the argument is not an event tap");
        const bool enable = (c.x[1] & 0xff) != 0;
        if (enable) unsupported("CGEventTapEnable(true): event taps are never fed under Alchemy");
        tap->enabled = false;
    });
    support.fill_function(cg, "_CGEventTapIsEnabled", [s](CpuState& c, GuestMemory&) {
        const auto tap = c.x[0] ? s->objc.find_host_state<EventTap>(c.x[0]) : nullptr;
        c.x[0] = tap && tap->enabled ? 1 : 0;
    });
    // The never-performed callback of an event tap's run-loop source.
    static_cast<void>(support.process.install_function("__alchemy_support_event_tap_perform", [](CpuState&, GuestMemory&) {
        fail("an event tap run-loop source fired (event taps are never fed)");
    }, "alchemy.internal"));
    const auto core_foundation = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
    support.fill_function(core_foundation, "_CFMachPortCreateRunLoopSource", [s](CpuState& c, GuestMemory& m) {
        const auto tap = c.x[1] ? s->objc.find_host_state<EventTap>(c.x[1]) : nullptr;
        if (!tap) unsupported("CFMachPortCreateRunLoopSource for a Mach port that is not a CGEventTap");
        if (!tap->source) {
            const auto perform = s->process.resolve("alchemy.internal", "__alchemy_support_event_tap_perform");
            if (!perform) fail("event tap perform callback is missing");
            // CFRunLoopSourceContext (version 0): 10 pointer-sized fields, perform last.
            const auto context = s->process.allocate_guest(80, 16);
            m.fill_bytes(context, 0, 80);
            m.write64(context + 72, perform->address);
            tap->source = s->call("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation",
                                  "_CFRunLoopSourceCreate", {c.x[0], c.x[2], context});
            s->process.release_guest(context);
        }
        c.x[0] = s->retain(tap->source);
    });
    support.fill_function(cg, "_CGEventPost", [](CpuState&, GuestMemory&) { unsupported("CGEventPost (synthetic input events)"); });
}

} // namespace alchemy::gui::support_kit
