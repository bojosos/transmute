#include "alchemy/gui/value_provider.hpp"

#include <array>
#include <atomic>

// Window API Chrome's browser window set-up reaches, added to the AppKit
// provider's classes where they lack it. Alchemy's windows draw their own
// title bar, so titlebar accessories are kept and returned, never shown:
// - NSTitlebarAccessoryViewController: layoutAttribute (NSLayoutAttribute,
//   default NSLayoutAttributeBottom 4), fullScreenMinHeight, hidden,
//   automaticallyAdjustsSize (default YES).
// - NSWindow: titlebarAccessoryViewControllers with add/insert/remove,
//   titlebarSeparatorStyle (NSTitlebarSeparatorStyleAutomatic 0),
//   windowTitlebarLayoutDirection (the application's layout direction).
// - NSApplication -registerServicesMenuSendTypes:returnTypes: (no Services
//   exist, so nothing asks for them; the types are recorded).
// - NSAccessibilityRemoteUIElement (private; Chrome's RemoteCocoa): remote
//   tokens are in-process: NSData {"AXRT", pid (4 bytes), element (8 bytes)};
//   -initWithRemoteToken: resolves a token of this process to its element
//   (-localUIElement). The window / top-level elements and the remote-UI
//   process registration are stored (there is no other process to reach).
// - NSImage -bestRepresentationForRect:context:hints:: the smallest bitmap
//   representation that covers the rect in pixels (scaled by the hints' or
//   context's backing scale when given as NSImageHintCTM is not modelled: 1x),
//   else the largest; a representation without pixel dimensions (vector)
//   when there is no bitmap; nil for an image without representations.
// - NSBitmapImageRep -colorSpace: the NSColorSpace of its colorSpaceName
//   (calibrated RGB -> genericRGB, device RGB -> deviceRGB, gray -> the gray
//   spaces); -bitmapImageRepByRetaggingWithColorSpace: a copy tagged with an
//   RGB space (the pixels are unchanged, as documented); nil for a space of
//   another color model.
namespace alchemy::gui::values {
namespace {

constexpr std::int64_t layout_attribute_bottom = 4;

} // namespace

void Provider::install_windows() {
    if (!objc.find_class("NSWindow")) return;
    observe_appkit_lifetimes();   // NSWindow's -dealloc (AppKit's) clears its values
    track_dealloc("NSTitlebarAccessoryViewController");
    // An integer/BOOL property with a default (stored under the setter's name).
    const auto integer_property = [this](std::string_view cls, std::string_view getter, std::string_view setter,
                                         std::int64_t default_value, const char* type) {
        const std::string key(setter);
        extend(cls, getter, [this, key, default_value](CpuState& s, GuestMemory&) {
            const auto state = properties(s.x[0], false);
            std::int64_t value = default_value;
            if (state) {
                std::lock_guard lock(state->mutex);
                if (const auto found = state->integers.find(key); found != state->integers.end()) value = found->second;
            }
            s.x[0] = static_cast<Address>(value);
        }, false, std::string(type) + "@:");
        extend(cls, setter, [this, key, type](CpuState& s, GuestMemory&) {
            const std::int64_t value = std::string_view(type) == "B" ? static_cast<std::int64_t>(s.x[2] & 0xff) : static_cast<std::int64_t>(s.x[2]);
            store_integer(s.x[0], key, value);
        }, false, std::string("v@:") + type);
    };
    const auto real_property = [this](std::string_view cls, std::string_view getter, std::string_view setter) {
        const std::string key(setter);
        extend(cls, getter, [this, key](CpuState& s, GuestMemory&) {
            const auto state = properties(s.x[0], false);
            double value{};
            if (state) {
                std::lock_guard lock(state->mutex);
                if (const auto found = state->reals.find(key); found != state->reals.end()) value = found->second;
            }
            fill::return_real(s, value);
        }, false, "d@:");
        extend(cls, setter, [this, key](CpuState& s, GuestMemory&) {
            const auto state = properties(s.x[0]);
            std::lock_guard lock(state->mutex);
            state->reals[key] = fill::real(s, 0);
        }, false, "v@:d");
    };

    // ---- NSTitlebarAccessoryViewController ----
    integer_property("NSTitlebarAccessoryViewController", "layoutAttribute", "setLayoutAttribute:", layout_attribute_bottom, "q");
    integer_property("NSTitlebarAccessoryViewController", "isHidden", "setHidden:", 0, "B");
    integer_property("NSTitlebarAccessoryViewController", "automaticallyAdjustsSize", "setAutomaticallyAdjustsSize:", 1, "B");
    real_property("NSTitlebarAccessoryViewController", "fullScreenMinHeight", "setFullScreenMinHeight:");

    // ---- NSWindow titlebar accessories ----
    // The list is an NSMutableArray in the window's side table.
    const auto list = [this](Address window) {
        auto value = stored(window, "titlebarAccessoryViewControllers");
        if (!value) {
            const auto created = objc.send(objc.send(objc.find_class("NSMutableArray"), "alloc").x0, "init").x0;
            store(window, "titlebarAccessoryViewControllers", created);
            release(created);
            value = stored(window, "titlebarAccessoryViewControllers");
        }
        return value;
    };
    extend("NSWindow", "titlebarAccessoryViewControllers", [this, list](CpuState& s, GuestMemory&) {
        s.x[0] = bridge->autorelease(cf.copy(list(s.x[0])));
    });
    extend("NSWindow", "setTitlebarAccessoryViewControllers:", [this, list](CpuState& s, GuestMemory&) {
        const auto items = list(s.x[0]);
        static_cast<void>(objc.send(items, "removeAllObjects"));
        if (s.x[2]) {
            CallRegs registers{};
            registers.x[2] = s.x[2];
            static_cast<void>(objc.send(items, "addObjectsFromArray:", registers));
        }
    }, false, "v@:@");
    extend("NSWindow", "addTitlebarAccessoryViewController:", [this, list](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;
        CallRegs registers{};
        registers.x[2] = s.x[2];
        static_cast<void>(objc.send(list(s.x[0]), "addObject:", registers));
    }, false, "v@:@");
    extend("NSWindow", "insertTitlebarAccessoryViewController:atIndex:", [this, list](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;
        CallRegs registers{};
        registers.x[2] = s.x[2];
        registers.x[3] = s.x[3];
        static_cast<void>(objc.send(list(s.x[0]), "insertObject:atIndex:", registers));
    }, false, "v@:@q");
    extend("NSWindow", "removeTitlebarAccessoryViewControllerAtIndex:", [this, list](CpuState& s, GuestMemory&) {
        CallRegs registers{};
        registers.x[2] = s.x[2];
        static_cast<void>(objc.send(list(s.x[0]), "removeObjectAtIndex:", registers));
    }, false, "v@:q");
    integer_property("NSWindow", "titlebarSeparatorStyle", "setTitlebarSeparatorStyle:", 0, "q");
    // NSUserInterfaceLayoutDirection: LeftToRight 0, RightToLeft 1.
    extend("NSWindow", "windowTitlebarLayoutDirection", [this](CpuState& s, GuestMemory&) {
        const auto app_class = objc.find_class("NSApplication");
        const auto app = app_class ? objc.send(app_class, "sharedApplication").x0 : 0;
        s.x[0] = app && objc.responds(app, objc.selector("userInterfaceLayoutDirection")) ? objc.send(app, "userInterfaceLayoutDirection").x0 : 0;
    }, false, "q@:");
    extend("NSApplication", "registerServicesMenuSendTypes:returnTypes:", [this](CpuState& s, GuestMemory&) {
        store(s.x[0], "servicesSendTypes", s.x[2]);
        store(s.x[0], "servicesReturnTypes", s.x[3]);
    }, false, "v@:@@");

    // ---- NSImage ----
    extend("NSImage", "bestRepresentationForRect:context:hints:", [this](CpuState& s, GuestMemory&) {
        const double width = fill::real(s, 2), height = fill::real(s, 3);
        const auto reps = objc.send(s.x[0], "representations").x0;
        Address best{}, largest{}, vector{};
        std::int64_t best_area = -1, largest_area = -1;
        for (const auto rep : reps ? cf.array_values(reps) : std::vector<Address>{}) {
            const auto wide = static_cast<std::int64_t>(objc.send(rep, "pixelsWide").x0);
            const auto high = static_cast<std::int64_t>(objc.send(rep, "pixelsHigh").x0);
            if (wide <= 0 || high <= 0) { if (!vector) vector = rep; continue; }
            const auto area = wide * high;
            if (area > largest_area) { largest = rep; largest_area = area; }
            if (wide >= width && high >= height && (best_area < 0 || area < best_area)) { best = rep; best_area = area; }
        }
        s.x[0] = best ? best : largest ? largest : vector;
    }, false, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}@@");

    // ---- NSBitmapImageRep color spaces ----
    observe_appkit_lifetimes();   // a copy keeps its color space (AppKit's -copyWithZone:)
    const auto named_space = [this](const std::string& name) -> Address {
        const auto space_class = objc.find_class("NSColorSpace");
        if (!space_class) return 0;
        const char* selector = name == "NSCalibratedRGBColorSpace" ? "genericRGBColorSpace"
                             : name == "NSDeviceRGBColorSpace" ? "deviceRGBColorSpace"
                             : name == "NSCalibratedWhiteColorSpace" ? "genericGrayColorSpace"
                             : name == "NSDeviceWhiteColorSpace" ? "deviceGrayColorSpace" : nullptr;
        if (!selector || !objc.responds(space_class, objc.selector(selector)))
            throw std::runtime_error("unsupported: Alchemy: NSColorSpace for " + name);
        return objc.send(space_class, selector).x0;
    };
    extend("NSBitmapImageRep", "colorSpace", [this, named_space](CpuState& s, GuestMemory&) {
        if (const auto tagged = stored(s.x[0], "colorSpace")) { s.x[0] = tagged; return; }
        const auto name = objc.send(s.x[0], "colorSpaceName").x0;
        s.x[0] = named_space(name ? cf.utf8(name) : std::string("NSDeviceRGBColorSpace"));
    });
    extend("NSBitmapImageRep", "bitmapImageRepByRetaggingWithColorSpace:", [this](CpuState& s, GuestMemory&) {
        // NSColorSpaceModel: RGB 1. Only RGB bitmaps are created by AppKit here.
        if (!s.x[2] || static_cast<std::int64_t>(objc.send(s.x[2], "colorSpaceModel").x0) != 1) { s.x[0] = 0; return; }
        Address copy{};
        if (objc.responds(s.x[0], objc.selector("copyWithZone:"))) {
            copy = objc.send(s.x[0], "copy").x0;
        } else if (objc.responds(s.x[0], objc.selector("TIFFRepresentation"))) {
            // An equal bitmap through its TIFF data (+1 like -copy).
            CallRegs registers{};
            registers.x[2] = objc.send(s.x[0], "TIFFRepresentation").x0;
            copy = registers.x[2] ? retain(objc.send(objc.object_class(s.x[0]), "imageRepWithData:", registers).x0) : 0;
        } else {
            throw std::runtime_error("unsupported: Alchemy: -bitmapImageRepByRetaggingWithColorSpace: cannot copy this bitmap");
        }
        if (!copy) { s.x[0] = 0; return; }
        store(copy, "colorSpace", s.x[2]);
        s.x[0] = bridge->autorelease(copy);
    });

    // ---- NSAccessibilityRemoteUIElement ----
    if (objc.find_class("NSAccessibilityRemoteUIElement")) {
        track_dealloc("NSAccessibilityRemoteUIElement");
        const auto pid = static_cast<std::uint32_t>(call(fill::libsystem, "_getpid", {}));
        extend("NSAccessibilityRemoteUIElement", "remoteTokenForLocalUIElement:", [this, pid](CpuState& s, GuestMemory&) {
            if (!s.x[2]) { s.x[0] = 0; return; }
            std::array<std::uint8_t, 16> token{'A', 'X', 'R', 'T'};
            for (int i = 0; i < 4; ++i) token[4 + i] = static_cast<std::uint8_t>(pid >> (8 * i));
            for (int i = 0; i < 8; ++i) token[8 + i] = static_cast<std::uint8_t>(s.x[2] >> (8 * i));
            s.x[0] = bridge->autorelease(bridge->data(token));
        }, true);
        extend("NSAccessibilityRemoteUIElement", "initWithRemoteToken:", [this, pid](CpuState& s, GuestMemory&) {
            const auto token = s.x[2] && bridge->is_data(s.x[2]) ? bridge->bytes(s.x[2]) : security::Bytes{};
            store(s.x[0], "remoteToken", s.x[2]);
            if (token.size() == 16 && token[0] == 'A' && token[1] == 'X' && token[2] == 'R' && token[3] == 'T') {
                std::uint32_t owner{};
                std::uint64_t element{};
                for (int i = 0; i < 4; ++i) owner |= static_cast<std::uint32_t>(token[4 + i]) << (8 * i);
                for (int i = 0; i < 8; ++i) element |= static_cast<std::uint64_t>(token[8 + i]) << (8 * i);
                if (owner == pid) store_integer(s.x[0], "localUIElement", static_cast<std::int64_t>(element));
            }
        });
        extend("NSAccessibilityRemoteUIElement", "localUIElement", [this](CpuState& s, GuestMemory&) {
            s.x[0] = static_cast<Address>(stored_integer(s.x[0], "localUIElement"));
        });
        extend("NSAccessibilityRemoteUIElement", "remoteToken", [this](CpuState& s, GuestMemory&) { s.x[0] = stored(s.x[0], "remoteToken"); });
        for (const auto* name : {"windowUIElement", "topLevelUIElement"}) {
            const std::string key(name);
            extend("NSAccessibilityRemoteUIElement", name, [this, key](CpuState& s, GuestMemory&) {
                s.x[0] = static_cast<Address>(stored_integer(s.x[0], key));
            });
            extend("NSAccessibilityRemoteUIElement", setter_name(name), [this, key](CpuState& s, GuestMemory&) {
                store_integer(s.x[0], key, static_cast<std::int64_t>(s.x[2]));   // weak, as in AppKit
            }, false, "v@:@");
        }
        auto remote_app = std::make_shared<std::atomic<bool>>(false);
        extend("NSAccessibilityRemoteUIElement", "setRemoteUIApp:", [remote_app](CpuState& s, GuestMemory&) { *remote_app = (s.x[2] & 0xff) != 0; }, true, "v@:B");
        extend("NSAccessibilityRemoteUIElement", "isRemoteUIApp", [remote_app](CpuState& s, GuestMemory&) { s.x[0] = remote_app->load(); }, true, "B@:");
        extend("NSAccessibilityRemoteUIElement", "registerRemoteUIProcessIdentifier:", [](CpuState&, GuestMemory&) {}, true, "v@:i");
        extend("NSAccessibilityRemoteUIElement", "unregisterRemoteUIProcessIdentifier:", [](CpuState&, GuestMemory&) {}, true, "v@:i");
    }
}

} // namespace alchemy::gui::values
