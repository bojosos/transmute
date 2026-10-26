#include "alchemy/darwin/kit/support/support_kit.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <vector>

// AppKit selectors Qt's Cocoa plugin sends that the AppKit provider
// (src/alchemy/appkit, owner A4 / q-appkit-controls) does not implement yet.
// Each is added only when the class does not already respond, so the AppKit
// owner's implementation wins once it exists. Listed for the AppKit owner:
//   -[NSColorSpace ICCProfileData]   nil (AppKit now answers with an authored
//                                    profile for its named RGB spaces)
//   -[NSObject className]            the class name (object_getClassName)
//   -[NSColor type]                  NSColorTypeComponentBased (all provider colors)
//   +[NSColorSpace generic/device CMYK and Gray]  distinct singletons (no model state)
//   +[NSColor <semantic colors>]     light-appearance values; every named color
//                                    (AppKit's too) is a shared immortal object
namespace alchemy::gui::support_kit {
namespace {

// True when instances of `cls` respond to `selector` (checked through the
// class's own instance-method lookup on a throwaway instance).
bool instances_respond(Support& support, Address cls, std::string_view selector) {
    const auto probe = support.objc.allocate(cls);
    const bool answer = support.objc.responds(probe, support.objc.selector(selector));
    support.process.objects().release(probe);
    return answer;
}

} // namespace

void install_appkit_extra(Support& support) {
    const auto add = [&](std::string_view class_name, std::string_view selector, Handler handler, std::string types,
                         const std::source_location& where = std::source_location::current()) {
        const auto cls = support.objc.find_class(class_name);
        if (!cls || instances_respond(support, cls, selector)) return;
        support.method(cls, std::string(selector), std::move(handler), std::move(types), false, where);
    };
    add("NSColorSpace", "ICCProfileData", [](CpuState& c, GuestMemory&) { c.x[0] = 0; }, "@@:");
    // Fallback only: AppKit's modal sessions (appkit/modal.cpp) provide
    // -modalWindow.
    add("NSApplication", "modalWindow", [](CpuState& c, GuestMemory&) { c.x[0] = 0; }, "@@:");
    // A window that is its own first responder is no text input client: no
    // input context (Qt asks the first responder; views keep AppKit's).
    add("NSWindow", "inputContext", [](CpuState& c, GuestMemory&) { c.x[0] = 0; }, "@@:");
    // -[NSApplication dockTile]: one NSDockTile that keeps its badge label,
    // content view and badge flag. Windows has no dock, so -display shows
    // nothing (the taskbar is not driven from it).
    if (const auto application_class = support.objc.find_class("NSApplication");
        application_class && !instances_respond(support, application_class, "dockTile")) {
        struct Tile { std::mutex mutex; Address object{}, badge{}, content{}; bool shows_badge = true; };
        auto tile = std::make_shared<Tile>();
        const auto tile_class = support.declare_class("NSDockTile", "NSObject", "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit");
        support.method(application_class, "dockTile", [&support, tile, tile_class](CpuState& c, GuestMemory&) {
            std::lock_guard lock(tile->mutex);
            if (!tile->object) {
                tile->object = support.objc.allocate(tile_class);
                if (const auto record = support.process.objects().find(tile->object)) record->immortal = true;
            }
            c.x[0] = tile->object;
        }, "@@:");
        support.method(tile_class, "badgeLabel", [tile](CpuState& c, GuestMemory&) { std::lock_guard lock(tile->mutex); c.x[0] = tile->badge; }, "@@:");
        support.method(tile_class, "setBadgeLabel:", [&support, tile](CpuState& c, GuestMemory&) {
            const auto copy = c.x[2] ? support.cf.copy(c.x[2]) : 0;
            std::lock_guard lock(tile->mutex);
            support.release(std::exchange(tile->badge, copy));
        }, "v@:@");
        support.method(tile_class, "contentView", [tile](CpuState& c, GuestMemory&) { std::lock_guard lock(tile->mutex); c.x[0] = tile->content; }, "@@:");
        support.method(tile_class, "setContentView:", [&support, tile](CpuState& c, GuestMemory&) {
            const auto view = support.retain(c.x[2]);
            std::lock_guard lock(tile->mutex);
            support.release(std::exchange(tile->content, view));
        }, "v@:@");
        support.method(tile_class, "showsApplicationBadge", [tile](CpuState& c, GuestMemory&) { std::lock_guard lock(tile->mutex); c.x[0] = tile->shows_badge; }, "B@:");
        support.method(tile_class, "setShowsApplicationBadge:", [tile](CpuState& c, GuestMemory&) {
            std::lock_guard lock(tile->mutex);
            tile->shows_badge = (c.x[2] & 0xff) != 0;
        }, "v@:B");
        support.method(tile_class, "display", [](CpuState&, GuestMemory&) {}, "v@:");
        support.method(tile_class, "size", [](CpuState& c, GuestMemory&) { return_real(c, 0, 128); return_real(c, 1, 128); }, "{CGSize=dd}@:");
    }
    // File-type icons: no system icons are decoded (as +[NSImage imageNamed:]
    // answers nil), but these never answer nil on macOS, so each is an empty
    // 32x32 image without representations. Its CGImage is NULL.
    if (const auto image_class = support.objc.find_class("NSImage")) {
        for (const auto* name : {"iconForFileType:", "iconForFile:", "iconForFiles:", "iconForContentType:"})
            add("NSWorkspace", name, [&support, image_class](CpuState& c, GuestMemory&) {
                CallRegs size;
                size.set_d(0, 32);
                size.set_d(1, 32);
                const auto image = support.send(support.send(image_class, "alloc").x0, "initWithSize:", size).x0;
                c.x[0] = support.send(image, "autorelease").x0;
            }, "@@:@");
        add("NSImage", "CGImageForProposedRect:context:hints:", [&support](CpuState& c, GuestMemory&) {
            const auto representations = support.send(c.x[0], "representations").x0;
            if (representations && support.send(representations, "count").x0)
                unsupported("-[NSImage CGImageForProposedRect:context:hints:] of an image with representations");
            c.x[0] = 0;
        }, "^{CGImage=}@:^{CGRect={CGPoint=dd}{CGSize=dd}}@@");
    }
    // Gray and CMYK NSColorSpace singletons: distinct objects so identity
    // comparisons (Qt's qt_mac_toQColor) answer "not this space"; no provider
    // color uses them. Their model/ICC queries are AppKit's defaults.
    if (const auto space_class = support.objc.find_class("NSColorSpace")) {
        auto spaces = std::make_shared<std::pair<std::mutex, std::map<std::string, Address>>>();
        for (const auto* name : {"genericCMYKColorSpace", "deviceCMYKColorSpace", "genericGrayColorSpace", "deviceGrayColorSpace",
                                 "genericGamma22GrayColorSpace", "extendedGenericGamma22GrayColorSpace"}) {
            if (support.objc.responds(space_class, support.objc.selector(name))) continue;
            const std::string key = name;
            support.method(space_class, key, [&support, space_class, spaces, key](CpuState& c, GuestMemory&) {
                std::lock_guard lock(spaces->first);
                auto& slot = spaces->second[key];
                if (!slot) {
                    slot = support.objc.allocate(space_class);
                    if (const auto record = support.process.objects().find(slot)) record->immortal = true;
                }
                c.x[0] = slot;
            }, "@@:", true);
        }
    }
    // Semantic system colors QCocoaTheme builds its palettes from, with the
    // macOS light-appearance values, made through +colorWithSRGBRed:...
    if (const auto color_class = support.objc.find_class("NSColor")) {
        const auto make = [&support, color_class](double r, double g, double b, double a) {
            CallRegs make;
            make.set_d(0, r); make.set_d(1, g); make.set_d(2, b); make.set_d(3, a);
            return support.send(color_class, "colorWithSRGBRed:green:blue:alpha:", make).x0;
        };
        struct Entry { const char* name; double r, g, b, a; };
        // Colours AppKit does not name (darwin/appkit/{graphics,resources}.cpp and
        // controls/appearance.cpp answer every other one QCocoaTheme reads).
        static constexpr Entry colors[] = {
            {"unemphasizedSelectedTextColor", 0, 0, 0, 1}, {"selectedMenuItemColor", 0, 0.39, 0.88, 1},
            {"secondarySelectedControlColor", 0.86, 0.86, 0.86, 1}, {"systemMintColor", 0, 0.78, 0.75, 1},
            {"systemCyanColor", 0.2, 0.68, 0.9, 1}};
        // Named colors are shared, immortal objects as on macOS (Qt keeps
        // some in statics without retaining them), as AppKit's are.
        auto cache = std::make_shared<std::pair<std::mutex, std::map<std::string, Address>>>();
        const auto keep = [&support, cache](const std::string& name, const std::function<Address()>& create) {
            std::lock_guard lock(cache->first);
            auto& slot = cache->second[name];
            if (!slot) {
                slot = support.retain(create());
                const auto record = support.process.objects().find(slot);
                if (record) record->immortal = true;
                if (std::getenv("ALCHEMY_SUPPORT_TRACE"))
                    std::fprintf(stderr, "[alchemy support] color %s = 0x%llx%s\n", name.c_str(), static_cast<unsigned long long>(slot),
                                 record ? "" : " (not in the object store)");
            }
            return slot;
        };
        for (const auto& entry : colors) {
            if (support.objc.responds(color_class, support.objc.selector(entry.name))) continue;
            const auto value = entry;
            support.method(color_class, entry.name, [make, value, keep](CpuState& c, GuestMemory&) {
                c.x[0] = keep(value.name, [&] { return make(value.r, value.g, value.b, value.a); });
            }, "@@:", true);
        }
        if (!support.objc.responds(color_class, support.objc.selector("alternatingContentBackgroundColors")))
            support.method(color_class, "alternatingContentBackgroundColors", [&support, make, keep](CpuState& c, GuestMemory&) {
                c.x[0] = keep("alternatingContentBackgroundColors", [&] {
                    const std::array<Address, 2> values{make(1, 1, 1, 1), make(0.957, 0.961, 0.961, 1)};
                    return support.send(support.cf.array(values), "autorelease").x0;
                });
            }, "@@:", true);
    }
    // Every provider color is component based (NSColorTypeComponentBased = 0).
    add("NSColor", "type", [](CpuState& c, GuestMemory&) {
        if (std::getenv("ALCHEMY_SUPPORT_TRACE")) std::fprintf(stderr, "[alchemy support] -[NSColor type] 0x%llx\n", static_cast<unsigned long long>(c.x[0]));
        c.x[0] = 0;
    }, "q@:");
    auto* s = &support;
    // Stored window/view properties the provider does not act on (recorded
    // and answered back): mouse-transparency, alpha, restoration, animation,
    // window-menu exclusion, panel modality flags.
    auto store = std::make_shared<std::pair<std::mutex, std::map<std::pair<Address, std::string>, std::uint64_t>>>();
    const auto stored = [&](const char* cls, const char* getter, const char* setter, bool real_value, std::uint64_t initial,
                            std::string getter_types, std::string setter_types) {
        const std::string key = getter;
        add(cls, setter, [store, key, real_value](CpuState& c, GuestMemory&) {
            std::lock_guard lock(store->first);
            store->second[{c.x[0], key}] = real_value ? c.v[0][0] : c.x[2];
        }, std::move(setter_types));
        add(cls, getter, [store, key, real_value, initial](CpuState& c, GuestMemory&) {
            std::lock_guard lock(store->first);
            const auto found = store->second.find({c.x[0], key});
            const auto value = found == store->second.end() ? initial : found->second;
            if (real_value) c.v[0] = {value, 0};
            else c.x[0] = value;
        }, std::move(getter_types));
    };
    const auto one = std::bit_cast<std::uint64_t>(1.0);
    stored("NSWindow", "ignoresMouseEvents", "setIgnoresMouseEvents:", false, 0, "B@:", "v@:B");
    stored("NSWindow", "alphaValue", "setAlphaValue:", true, one, "d@:", "v@:d");
    stored("NSView", "alphaValue", "setAlphaValue:", true, one, "d@:", "v@:d");
    stored("NSWindow", "isRestorable", "setRestorable:", false, 1, "B@:", "v@:B");
    stored("NSWindow", "animationBehavior", "setAnimationBehavior:", false, 0, "q@:", "v@:q");
    stored("NSWindow", "hidesOnDeactivate", "setHidesOnDeactivate:", false, 0, "B@:", "v@:B");
    stored("NSWindow", "isExcludedFromWindowsMenu", "setExcludedFromWindowsMenu:", false, 0, "B@:", "v@:B");
    stored("NSWindow", "worksWhenModal", "setWorksWhenModal:", false, 0, "B@:", "v@:B");
    stored("NSWindow", "becomesKeyOnlyIfNeeded", "setBecomesKeyOnlyIfNeeded:", false, 0, "B@:", "v@:B");
    stored("NSWindow", "isMovable", "setMovable:", false, 1, "B@:", "v@:B");
    stored("NSWindow", "isMovableByWindowBackground", "setMovableByWindowBackground:", false, 0, "B@:", "v@:B");
    stored("NSWindow", "preservesContentDuringLiveResize", "setPreservesContentDuringLiveResize:", false, 1, "B@:", "v@:B");
    stored("NSWindow", "displaysWhenScreenProfileChanges", "setDisplaysWhenScreenProfileChanges:", false, 0, "B@:", "v@:B");
    stored("NSWindow", "colorSpace", "setColorSpace:", false, 0, "@@:", "v@:@");
    // CALayer properties QuartzCore does not model (contentsGravity is
    // QuartzCore's): masks, corner radii and filters are recorded but not
    // applied to the composition.
    stored("CALayer", "mask", "setMask:", false, 0, "@@:", "v@:@");
    stored("CALayer", "contentsFormat", "setContentsFormat:", false, 0, "@@:", "v@:@");
    stored("CALayer", "minificationFilter", "setMinificationFilter:", false, 0, "@@:", "v@:@");
    stored("CALayer", "magnificationFilter", "setMagnificationFilter:", false, 0, "@@:", "v@:@");
    stored("CALayer", "compositingFilter", "setCompositingFilter:", false, 0, "@@:", "v@:@");
    stored("CALayer", "actions", "setActions:", false, 0, "@@:", "v@:@");
    add("NSWindow", "setResizeIncrements:", [](CpuState&, GuestMemory&) {}, "v@:{CGSize=dd}");
    add("NSWindow", "invalidateShadow", [](CpuState&, GuestMemory&) {}, "v@:");
    // Tool tips, sheets and content border thickness are QLab's NSView /
    // NSWindow additions (darwin/appkit/windows/window_extras.cpp).
    // Focus rings are never drawn by the provider: the type is only stored.
    // (Views carry AppKit's host state, so the value lives in a side table.)
    auto rings = std::make_shared<std::pair<std::mutex, std::map<Address, std::uint64_t>>>();
    add("NSView", "setFocusRingType:", [rings](CpuState& c, GuestMemory&) {
        std::lock_guard lock(rings->first);
        rings->second[c.x[0]] = c.x[2];
    }, "v@:Q");
    add("NSView", "focusRingType", [rings](CpuState& c, GuestMemory&) {
        std::lock_guard lock(rings->first);
        const auto found = rings->second.find(c.x[0]);
        c.x[0] = found == rings->second.end() ? 0 : found->second;
    }, "Q@:");
    // -highlightWithLevel: / -shadowWithLevel: / -blendedColorWithFraction:ofColor:
    // (sRGB component blends; the result is autoreleased like AppKit's).
    const auto components = [s](Address color) {
        std::array<double, 4> rgba{};
        const auto cells = s->process.allocate_guest(32, 16);
        CallRegs get;
        for (unsigned index = 0; index < 4; ++index) get.x[2 + index] = cells + 8 * index;
        static_cast<void>(s->send(color, "getRed:green:blue:alpha:", get));
        for (unsigned index = 0; index < 4; ++index) rgba[index] = std::bit_cast<double>(s->memory().read64(cells + 8 * index));
        s->process.release_guest(cells);
        return rgba;
    };
    const auto blend = [s, components](Address color, std::array<double, 4> target, double fraction, bool keep_alpha) -> Address {
        const auto color_class = s->objc.find_class("NSColor");
        const auto from = components(color);
        fraction = std::clamp(fraction, 0.0, 1.0);
        CallRegs make;
        for (unsigned index = 0; index < 4; ++index)
            make.set_d(index, index == 3 && keep_alpha ? from[3] : from[index] + (target[index] - from[index]) * fraction);
        return s->send(color_class, "colorWithSRGBRed:green:blue:alpha:", make).x0;
    };
    add("NSColor", "highlightWithLevel:", [blend](CpuState& c, GuestMemory&) {
        c.x[0] = blend(c.x[0], {1, 1, 1, 1}, real(c, 0), true);
    }, "@@:d");
    add("NSColor", "shadowWithLevel:", [blend](CpuState& c, GuestMemory&) {
        c.x[0] = blend(c.x[0], {0, 0, 0, 1}, real(c, 0), true);
    }, "@@:d");
    add("NSColor", "blendedColorWithFraction:ofColor:", [blend, components](CpuState& c, GuestMemory&) {
        if (!c.x[2]) { c.x[0] = 0; return; }
        c.x[0] = blend(c.x[0], components(c.x[2]), real(c, 0), false);
    }, "@@:d@");
    // -[NSString rangeOfComposedCharacterSequenceAtIndex:]: surrogate pairs and
    // following combining marks (U+0300-036F, U+1AB0-1AFF, U+1DC0-1DFF,
    // U+20D0-20FF, U+FE20-FE2F, variation selectors U+FE00-FE0F, ZWJ) form one sequence.
    add("NSString", "rangeOfComposedCharacterSequenceAtIndex:", [s](CpuState& c, GuestMemory&) {
        const auto units = s->cf.units(c.x[0]);
        auto index = static_cast<std::size_t>(c.x[2]);
        if (index >= units.size()) { c.x[0] = index; c.x[1] = 0; return; }
        const auto low = [&](std::size_t at) { return units[at] >= 0xdc00 && units[at] < 0xe000; };
        const auto high = [&](std::size_t at) { return units[at] >= 0xd800 && units[at] < 0xdc00; };
        const auto combining = [&](std::size_t at) {
            const auto u = units[at];
            return (u >= 0x300 && u < 0x370) || (u >= 0x1ab0 && u < 0x1b00) || (u >= 0x1dc0 && u < 0x1e00) ||
                   (u >= 0x20d0 && u < 0x2100) || (u >= 0xfe00 && u < 0xfe30) || u == 0x200d;
        };
        while (index > 0 && (low(index) && high(index - 1))) --index;
        while (index > 0 && combining(index)) --index;
        if (index > 0 && low(index) && high(index - 1)) --index;
        auto end = index + ((high(index) && index + 1 < units.size() && low(index + 1)) ? 2 : 1);
        while (end < units.size() && combining(end)) ++end;
        c.x[0] = index;
        c.x[1] = end - index;
    }, "{_NSRange=QQ}@:Q");
    // -[NSObject className] (Foundation's scripting addition): the class name.
    add("NSObject", "className", [s](CpuState& c, GuestMemory& m) {
        const auto name = s->call(objc_library, "_object_getClassName", {c.x[0]});
        std::string text;
        for (Address at = name; at && text.size() < 512; ++at) {
            const auto ch = m.read8(at);
            if (!ch) break;
            text.push_back(static_cast<char>(ch));
        }
        c.x[0] = s->send(s->cf.string(text), "autorelease").x0;
    }, "@@:");
}

} // namespace alchemy::gui::support_kit
