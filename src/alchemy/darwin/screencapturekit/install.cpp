#include "alchemy/darwin/screencapturekit/install.hpp"

#include "alchemy/darwin/kit/support/support_kit.hpp"
#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/darwin/windowserver/presented_pixels.hpp"
#include "alchemy/gui/layer_contents.hpp"
#include "alchemy/gui/runloop.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstring>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// ScreenCaptureKit and the CoreGraphics window lists, limited to the
// process's own content: the displays (the screens the window server
// reports, the virtual display the guest's windows are on), the guest's own
// windows and its own application. Nothing else is ever listed or captured:
// the pixels come from what this process presented in its own windows
// (windowserver/presented_pixels.hpp), so no host window, no other process
// (other Alchemy guests included) and no host desktop can appear. Listings
// leave everything else out (they are not errors), and the screen-capture
// permission answers granted for that scope (CGPreflightScreenCaptureAccess,
// coregraphics/graphics_gaps.cpp).
// * Windows: the application's visible NSWindows, except those whose
//   sharingType is NSWindowSharingNone. A window's frame is its content area
//   (the title bar and frame are the host's and are never captured).
// * Frames: a display frame is the guest's windows composited back to front
//   over opaque black at their places on the display; a window frame is the
//   window's content. Areas a window shows through CAMetalLayer views keep
//   what was last presented there in software.
// * SCStream delivers a frame (an IOSurface-backed CVPixelBuffer in a
//   CMSampleBuffer, 'BGRA', SCStreamFrameInfoStatus complete) every
//   minimumFrameInterval (one 60 Hz frame when zero) to each screen output on
//   its sample handler queue (the main queue without one), at most queueDepth
//   undelivered at once. Audio outputs are refused (no audio is captured).
// * SCScreenshotManager makes CGImages of the same frames.
// String constants carry the values of Apple's public headers; values marked
// "(unverified)" follow the framework's naming convention.
namespace alchemy::screencapturekit {
namespace {

namespace sk = gui::support_kit;
using sk::Address;
using sk::CpuState;
using sk::GuestMemory;
using gui::Rect;
using gui::WindowId;

constexpr std::string_view screen_capture_kit = "/System/Library/Frameworks/ScreenCaptureKit.framework/Versions/A/ScreenCaptureKit";
constexpr std::string_view core_graphics = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";
constexpr std::string_view core_foundation = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr std::string_view core_video = "/System/Library/Frameworks/CoreVideo.framework/Versions/A/CoreVideo";
constexpr std::string_view core_media = "/System/Library/Frameworks/CoreMedia.framework/Versions/A/CoreMedia";
constexpr std::string_view error_domain = "com.apple.ScreenCaptureKit.SCStreamErrorDomain";
constexpr std::int64_t sc_stream_error_user_declined = -3801;
constexpr std::uint32_t format_bgra = 0x42475241;   // 'BGRA'

// ---- the process's own content ----------------------------------------------------------------
struct OwnWindow {
    WindowId id{};
    Address object{};
    Rect content;       // CG global coordinates (top-left origin of the primary display, y down)
    Rect frame;         // the whole window, CG coordinates (listings only)
    std::string title;
    std::int64_t level{};
    bool on_screen{}, key{};
};
struct OwnDisplay {
    std::uint64_t id{};
    Rect frame;         // CG coordinates
    double scale{1};
};

struct Image {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> bgra;   // premultiplied, rows top first
};

class Capture {
public:
    explicit Capture(sk::Support& support) : s(support) {}
    sk::Support& s;

    gui::WindowServer* server() const { return gui::layer_contents::presentation_server(); }
    double primary_height() const {
        const auto screens = server() ? server()->screens() : std::vector<gui::ScreenInfo>{};
        return screens.empty() ? 0 : screens.front().frame.origin.y + screens.front().frame.size.height;
    }
    Rect to_cg(const Rect& r) const { return {{r.origin.x, primary_height() - r.origin.y - r.size.height}, r.size}; }

    std::vector<OwnDisplay> displays() const {
        std::vector<OwnDisplay> out;
        if (!server()) return out;
        for (const auto& info : server()->screens()) out.push_back({static_cast<std::uint64_t>(info.id), to_cg(info.frame), info.backing_scale});
        return out;
    }
    // The application's windows, front to back.
    std::vector<OwnWindow> windows(bool on_screen_only) {
        std::vector<OwnWindow> out;
        const auto server_ = server();
        const auto app_class = s.objc.find_class("NSApplication");
        if (!server_ || !app_class) return out;
        const auto app = s.send_id(app_class, "sharedApplication");
        const auto ordered = app ? s.send_id(app, "orderedWindows") : 0;
        const auto count = ordered ? s.send(ordered, "count").x0 : 0;
        for (Address i = 0; i < count; ++i) {
            const auto window = s.send_id(ordered, "objectAtIndex:", {i});
            const auto id = static_cast<WindowId>(s.send(window, "windowNumber").x0);
            if (!id || !server_->window_exists(id)) continue;
            if (s.objc.responds(window, s.objc.selector("sharingType")) && s.send(window, "sharingType").x0 == 0) continue;   // NSWindowSharingNone
            const bool visible = (s.send(window, "isVisible").x0 & 0xff) != 0;
            if (on_screen_only && !visible) continue;
            OwnWindow w;
            w.id = id;
            w.object = window;
            w.content = to_cg(server_->content_rect(id));
            w.frame = to_cg(server_->frame_rect(id));
            if (const auto title = s.send_id(window, "title"); title && s.cf.kind(title) == ObjectKind::string) w.title = s.cf.utf8(title);
            w.level = static_cast<std::int64_t>(s.send(window, "level").x0);
            w.on_screen = visible;
            w.key = (s.send(window, "isKeyWindow").x0 & 0xff) != 0;
            out.push_back(w);
        }
        return out;
    }
    std::string application_name() {
        const auto info = s.send_id(s.objc.find_class("NSProcessInfo"), "processInfo");
        const auto name = info ? s.send_id(info, "processName") : 0;
        return name ? s.cf.utf8(name) : std::string();
    }
    std::string bundle_identifier() {
        const auto bundle_class = s.objc.find_class("NSBundle");
        const auto bundle = bundle_class ? s.send_id(bundle_class, "mainBundle") : 0;
        const auto identifier = bundle ? s.send_id(bundle, "bundleIdentifier") : 0;
        return identifier ? s.cf.utf8(identifier) : std::string();
    }
    std::int64_t pid() { return static_cast<std::int32_t>(s.call(sk::libsystem_library, "_getpid", {})); }

    // `area` (CG points) at `width` x `height` pixels: the given windows,
    // back to front, over `background` (premultiplied BGRA).
    Image compose(const Rect& area, std::uint32_t width, std::uint32_t height, const std::vector<OwnWindow>& front_to_back,
                  std::array<std::uint8_t, 4> background) {
        Image out{width, height, {}};
        out.bgra.resize(static_cast<std::size_t>(width) * height * 4);
        for (std::size_t i = 0; i < out.bgra.size(); i += 4) std::copy(background.begin(), background.end(), out.bgra.begin() + i);
        if (!(area.size.width > 0) || !(area.size.height > 0)) return out;
        const double sx = width / area.size.width, sy = height / area.size.height;
        for (auto it = front_to_back.rbegin(); it != front_to_back.rend(); ++it) {
            if (!it->on_screen) continue;
            const auto pixels = gui::presented::snapshot(it->id);
            if (!pixels || !pixels->width || !pixels->height) continue;
            const double x0 = (it->content.origin.x - area.origin.x) * sx, y0 = (it->content.origin.y - area.origin.y) * sy;
            const double dw = it->content.size.width * sx, dh = it->content.size.height * sy;
            const long px0 = std::max(0L, static_cast<long>(std::floor(x0))), py0 = std::max(0L, static_cast<long>(std::floor(y0)));
            const long px1 = std::min<long>(width, static_cast<long>(std::ceil(x0 + dw))), py1 = std::min<long>(height, static_cast<long>(std::ceil(y0 + dh)));
            for (long y = py0; y < py1; ++y) {
                const auto sy_ = std::clamp<long>(static_cast<long>((y + 0.5 - y0) / dh * pixels->height), 0, pixels->height - 1);
                for (long x = px0; x < px1; ++x) {
                    const auto sx_ = std::clamp<long>(static_cast<long>((x + 0.5 - x0) / dw * pixels->width), 0, pixels->width - 1);
                    const auto* src = &pixels->bgra[(static_cast<std::size_t>(sy_) * pixels->width + sx_) * 4];
                    auto* dst = &out.bgra[(static_cast<std::size_t>(y) * width + x) * 4];
                    const unsigned inverse = 255 - src[3];
                    for (int c = 0; c < 4; ++c) dst[c] = static_cast<std::uint8_t>(src[c] + (dst[c] * inverse + 127) / 255);
                }
            }
        }
        return out;
    }
    // +1 CGImage of `image`.
    Address cg_image(const Image& image) {
        if (!image.width || !image.height) return 0;
        const auto space = s.call(core_graphics, "_CGColorSpaceCreateDeviceRGB", {});
        const auto context = s.call(core_graphics, "_CGBitmapContextCreate", {0, image.width, image.height, 8, image.width * 4ULL, space,
                                                                              2 /* premultiplied first */ | 0x2000 /* 32 little */});
        s.call(core_graphics, "_CGColorSpaceRelease", {space});
        if (!context) return 0;
        const auto data = s.call(core_graphics, "_CGBitmapContextGetData", {context});
        const auto stride = s.call(core_graphics, "_CGBitmapContextGetBytesPerRow", {context});
        for (std::uint32_t row = 0; row < image.height; ++row)
            s.memory().write_bytes(data + row * stride, std::span(image.bgra.data() + static_cast<std::size_t>(row) * image.width * 4,
                                                                  static_cast<std::size_t>(image.width) * 4));
        const auto made = s.call(core_graphics, "_CGBitmapContextCreateImage", {context});
        s.call(core_graphics, "_CGContextRelease", {context});
        return made;
    }
};

// ---- the classes' state -----------------------------------------------------------------------
struct DisplayState final : ObjectPayload { OwnDisplay display; };
struct WindowState final : ObjectPayload { OwnWindow window; Address application{}; };   // application +1
struct ApplicationState final : ObjectPayload {};
struct ContentState final : ObjectPayload { Address displays{}, windows{}, applications{}; };   // +1 arrays
struct FilterState final : ObjectPayload {
    bool single_window{};
    WindowId window{};
    std::uint64_t display{};
    bool only_listed{};          // the listed windows only; otherwise every window except the listed ones
    std::set<WindowId> listed;
};
struct ConfigState final : ObjectPayload {
    std::map<std::string, std::int64_t> integers;
    std::map<std::string, double> reals;
    std::map<std::string, Rect> rects;
    std::map<std::string, Address> objects;   // +1
    std::array<std::uint8_t, 24> minimum_frame_interval{};   // CMTime
};
struct StreamOutput { Address output{}, queue{}; std::int64_t type{}; };   // output +1
struct StreamState final : ObjectPayload {
    std::mutex mutex;
    Address filter{}, configuration{}, delegate{};   // filter, configuration +1; delegate weak
    std::vector<StreamOutput> outputs;
    gui::TimerId timer{};
    std::uint32_t in_flight{};
};

} // namespace

void install_screencapturekit(gui::InstallContext&) {
    auto* const sp = sk::support();
    if (!sp) throw std::runtime_error("ScreenCaptureKit: the support kit is not installed");
    auto& support = *sp;
    ProcessContext::add_provider_library(std::string(screen_capture_kit));
    const auto capture = std::make_shared<Capture>(support);
    const auto lib = screen_capture_kit;
    support.string_constant(lib, "_SCStreamErrorDomain", error_domain);
    // The SCStreamFrameInfo keys of a stream sample buffer's attachments (unverified: the constants' names).
    for (const auto* key : {"SCStreamFrameInfoStatus", "SCStreamFrameInfoDisplayTime", "SCStreamFrameInfoScaleFactor",
             "SCStreamFrameInfoContentScale", "SCStreamFrameInfoContentRect", "SCStreamFrameInfoBoundingRect",
             "SCStreamFrameInfoScreenRect", "SCStreamFrameInfoDirtyRects", "SCStreamFrameInfoPresenterOverlayContentRect"})
        support.string_constant(lib, "_" + std::string(key), key);
    const auto key_object = [sp, lib](const char* name) { return sp->memory().read64(sp->process.resolve(lib, std::string("_") + name)->address); };

    const auto display_class = support.declare_class("SCDisplay", "NSObject", lib);
    const auto window_class = support.declare_class("SCWindow", "NSObject", lib);
    const auto application_class = support.declare_class("SCRunningApplication", "NSObject", lib);
    const auto content_class = support.declare_class("SCShareableContent", "NSObject", lib);
    const auto info_class = support.declare_class("SCShareableContentInfo", "NSObject", lib);
    const auto filter_class = support.declare_class("SCContentFilter", "NSObject", lib);
    const auto config_class = support.declare_class("SCStreamConfiguration", "NSObject", lib);
    const auto stream_class = support.declare_class("SCStream", "NSObject", lib);
    const auto screenshot_class = support.declare_class("SCScreenshotManager", "NSObject", lib);
    const auto picker_class = support.declare_class("SCContentSharingPicker", "NSObject", lib);
    const auto make = [sp](Address cls) { return sp->send_id(sp->objc.allocate(cls), "init"); };   // +1
    const auto rect_return = [](CpuState& c, const Rect& r) {
        sk::return_real(c, 0, r.origin.x); sk::return_real(c, 1, r.origin.y);
        sk::return_real(c, 2, r.size.width); sk::return_real(c, 3, r.size.height);
    };

    // ---- SCRunningApplication, SCDisplay, SCWindow ----
    const auto application_object = [sp, application_class, make]() {
        static Address shared{};
        if (!shared) {
            shared = make(application_class);
            static_cast<void>(sp->objc.host_state<ApplicationState>(shared));
        }
        return shared;
    };
    support.method(application_class, "bundleIdentifier", [sp, capture](CpuState& c, GuestMemory&) { c.x[0] = sp->send_id(sp->cf.string(capture->bundle_identifier()), "autorelease"); }, "@@:");
    support.method(application_class, "applicationName", [sp, capture](CpuState& c, GuestMemory&) { c.x[0] = sp->send_id(sp->cf.string(capture->application_name()), "autorelease"); }, "@@:");
    support.method(application_class, "processID", [capture](CpuState& c, GuestMemory&) { c.x[0] = static_cast<std::uint32_t>(capture->pid()); }, "i@:");
    const auto display_of = [sp](Address self) { return sp->objc.host_state<DisplayState>(self); };
    support.method(display_class, "displayID", [display_of](CpuState& c, GuestMemory&) { c.x[0] = static_cast<std::uint32_t>(display_of(c.x[0])->display.id); }, "I@:");
    support.method(display_class, "width", [display_of](CpuState& c, GuestMemory&) { c.x[0] = static_cast<Address>(display_of(c.x[0])->display.frame.size.width); }, "q@:");
    support.method(display_class, "height", [display_of](CpuState& c, GuestMemory&) { c.x[0] = static_cast<Address>(display_of(c.x[0])->display.frame.size.height); }, "q@:");
    support.method(display_class, "frame", [display_of, rect_return](CpuState& c, GuestMemory&) { rect_return(c, display_of(c.x[0])->display.frame); },
                   "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    const auto window_of = [sp](Address self) { return sp->objc.host_state<WindowState>(self); };
    support.method(window_class, "windowID", [window_of](CpuState& c, GuestMemory&) { c.x[0] = static_cast<std::uint32_t>(window_of(c.x[0])->window.id); }, "I@:");
    support.method(window_class, "frame", [window_of, rect_return](CpuState& c, GuestMemory&) { rect_return(c, window_of(c.x[0])->window.content); },
                   "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    support.method(window_class, "title", [sp, window_of](CpuState& c, GuestMemory&) { c.x[0] = sp->send_id(sp->cf.string(window_of(c.x[0])->window.title), "autorelease"); }, "@@:");
    support.method(window_class, "windowLayer", [window_of](CpuState& c, GuestMemory&) { c.x[0] = static_cast<Address>(window_of(c.x[0])->window.level); }, "q@:");
    support.method(window_class, "owningApplication", [window_of](CpuState& c, GuestMemory&) { c.x[0] = window_of(c.x[0])->application; }, "@@:");
    support.method(window_class, "isOnScreen", [window_of](CpuState& c, GuestMemory&) { c.x[0] = window_of(c.x[0])->window.on_screen; }, "B@:");
    support.method(window_class, "isActive", [window_of](CpuState& c, GuestMemory&) { c.x[0] = window_of(c.x[0])->window.key; }, "B@:");
    support.method(window_class, "dealloc", [sp, window_class](CpuState& c, GuestMemory&) {
        if (const auto state = sp->objc.find_host_state<WindowState>(c.x[0])) sp->release(std::exchange(state->application, 0));
        static_cast<void>(sp->objc.send_super(c.x[0], window_class, sp->objc.selector("dealloc")));
    }, "v@:");

    // ---- SCShareableContent ----
    // +1 content listing the displays, the own windows (on screen only when asked) and the own application.
    const auto listing = [sp, capture, make, content_class, display_class, window_class, application_object](bool on_screen_only) {
        std::vector<Address> displays, windows;
        for (const auto& display : capture->displays()) {
            const auto made = make(display_class);
            sp->objc.host_state<DisplayState>(made)->display = display;
            displays.push_back(made);
        }
        const auto app = application_object();
        for (const auto& window : capture->windows(on_screen_only)) {
            const auto made = make(window_class);
            const auto state = sp->objc.host_state<WindowState>(made);
            state->window = window;
            state->application = sp->retain(app);
            windows.push_back(made);
        }
        const auto content = make(content_class);
        const auto state = sp->objc.host_state<ContentState>(content);
        state->displays = sp->cf.array(displays);
        state->windows = sp->cf.array(windows);
        state->applications = sp->cf.array(std::array<Address, 1>{app});
        for (const auto object : displays) sp->release(object);
        for (const auto object : windows) sp->release(object);
        return content;
    };
    const auto content_of = [sp](Address self) { return sp->objc.host_state<ContentState>(self); };
    support.method(content_class, "displays", [content_of](CpuState& c, GuestMemory&) { c.x[0] = content_of(c.x[0])->displays; }, "@@:");
    support.method(content_class, "windows", [content_of](CpuState& c, GuestMemory&) { c.x[0] = content_of(c.x[0])->windows; }, "@@:");
    support.method(content_class, "applications", [content_of](CpuState& c, GuestMemory&) { c.x[0] = content_of(c.x[0])->applications; }, "@@:");
    support.method(content_class, "dealloc", [sp, content_class](CpuState& c, GuestMemory&) {
        if (const auto state = sp->objc.find_host_state<ContentState>(c.x[0]))
            for (auto* field : {&state->displays, &state->windows, &state->applications}) sp->release(std::exchange(*field, 0));
        static_cast<void>(sp->objc.send_super(c.x[0], content_class, sp->objc.selector("dealloc")));
    }, "v@:");
    // Completion handlers run on the main queue: handler(value, error).
    const auto complete = [sp](Address block, Address value, Address error) {
        if (!block) { sp->release(value); sp->release(error); return; }
        const auto bridge = gui::security::bridge();
        const auto copied = bridge->block_copy(block);
        bridge->dispatch_async(bridge->main_queue(), [sp, bridge, copied, value, error] {
            static_cast<void>(bridge->invoke_block(copied, {value, error}));
            sp->release(value);
            sp->release(error);
            bridge->block_release(copied);
        });
    };
    // handler(error) on the main queue.
    const auto done = [](Address block, Address error) {
        const auto bridge = gui::security::bridge();
        if (!block) { if (error) bridge->release(error); return; }
        const auto copied = bridge->block_copy(block);
        bridge->dispatch_async(bridge->main_queue(), [bridge, copied, error] {
            static_cast<void>(bridge->invoke_block(copied, {error}));
            if (error) bridge->release(error);
            bridge->block_release(copied);
        });
    };
    const auto share = [listing, complete](bool on_screen_only, Address block) { complete(block, listing(on_screen_only), 0); };
    support.method(content_class, "getShareableContentWithCompletionHandler:", [share](CpuState& c, GuestMemory&) { share(false, c.x[2]); }, "v@:@?", true);
    support.method(content_class, "getShareableContentExcludingDesktopWindows:onScreenWindowsOnly:completionHandler:",
                   [share](CpuState& c, GuestMemory&) { share((c.x[3] & 0xff) != 0, c.x[4]); }, "v@:BB@?", true);
    // (The windows above or below one: every own window is listed; the order is front to back.)
    for (const auto* name : {"getShareableContentExcludingDesktopWindows:onScreenWindowsOnlyBelowWindow:completionHandler:",
                             "getShareableContentExcludingDesktopWindows:onScreenWindowsOnlyAboveWindow:completionHandler:"})
        support.method(content_class, name, [share](CpuState& c, GuestMemory&) { share(true, c.x[4]); }, "v@:B@@?", true);
    support.method(content_class, "getCurrentProcessShareableContentWithCompletionHandler:", [share](CpuState& c, GuestMemory&) { share(false, c.x[2]); }, "v@:@?", true);

    // ---- SCContentFilter ----
    const auto filter_of = [sp](Address self) { return sp->objc.host_state<FilterState>(self); };
    const auto ids = [sp](Address array) {
        std::set<WindowId> out;
        for (const auto window : array ? sp->cf.array_values(array) : std::vector<Address>{})
            if (const auto state = sp->objc.find_host_state<WindowState>(window)) out.insert(state->window.id);
        return out;
    };
    const auto display_id = [sp](Address display) {
        const auto state = display ? sp->objc.find_host_state<DisplayState>(display) : nullptr;
        return state ? state->display.id : 0;
    };
    const auto own_application = [sp](Address array) {
        for (const auto app : array ? sp->cf.array_values(array) : std::vector<Address>{})
            if (sp->objc.find_host_state<ApplicationState>(app)) return true;
        return false;
    };
    support.method(filter_class, "initWithDesktopIndependentWindow:", [sp, filter_of](CpuState& c, GuestMemory&) {
        const auto state = filter_of(c.x[0]);
        state->single_window = true;
        if (const auto window = c.x[2] ? sp->objc.find_host_state<WindowState>(c.x[2]) : nullptr) state->window = window->window.id;
    }, "@@:@");
    support.method(filter_class, "initWithDisplay:excludingWindows:", [filter_of, ids, display_id](CpuState& c, GuestMemory&) {
        const auto state = filter_of(c.x[0]);
        state->display = display_id(c.x[2]);
        state->listed = ids(c.x[3]);
    }, "@@:@@");
    support.method(filter_class, "initWithDisplay:includingWindows:", [filter_of, ids, display_id](CpuState& c, GuestMemory&) {
        const auto state = filter_of(c.x[0]);
        state->display = display_id(c.x[2]);
        state->only_listed = true;
        state->listed = ids(c.x[3]);
    }, "@@:@@");
    support.method(filter_class, "initWithDisplay:includingApplications:exceptingWindows:", [filter_of, ids, display_id, own_application](CpuState& c, GuestMemory&) {
        const auto state = filter_of(c.x[0]);
        state->display = display_id(c.x[2]);
        if (own_application(c.x[3])) state->listed = ids(c.x[4]);
        else state->only_listed = true;   // (other applications have nothing to show)
    }, "@@:@@@");
    support.method(filter_class, "initWithDisplay:excludingApplications:exceptingWindows:", [filter_of, ids, display_id, own_application](CpuState& c, GuestMemory&) {
        const auto state = filter_of(c.x[0]);
        state->display = display_id(c.x[2]);
        if (own_application(c.x[3])) { state->only_listed = true; state->listed = ids(c.x[4]); }
    }, "@@:@@@");
    // The windows a filter shows, front to back, and the area it covers (CG points).
    const auto resolve = [capture](const FilterState& filter, Rect& area, double& scale) {
        auto windows = capture->windows(true);
        std::erase_if(windows, [&](const OwnWindow& w) {
            if (filter.single_window) return w.id != filter.window;
            return filter.only_listed != filter.listed.contains(w.id);
        });
        scale = 1;
        if (filter.single_window) {
            area = windows.empty() ? Rect{} : windows.front().content;
            for (const auto& display : capture->displays())
                if (!windows.empty() && display.frame.origin.x <= area.origin.x && display.frame.origin.y <= area.origin.y) scale = display.scale;
            return windows;
        }
        const auto displays = capture->displays();
        const auto found = std::find_if(displays.begin(), displays.end(), [&](const OwnDisplay& d) { return d.id == filter.display; });
        const auto& display = found != displays.end() ? *found : (displays.empty() ? OwnDisplay{} : displays.front());
        area = display.frame;
        scale = display.scale;
        return windows;
    };
    support.method(filter_class, "contentRect", [filter_of, resolve, rect_return](CpuState& c, GuestMemory&) {
        Rect area; double scale{};
        static_cast<void>(resolve(*filter_of(c.x[0]), area, scale));
        rect_return(c, {{0, 0}, area.size});
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    support.method(filter_class, "pointPixelScale", [filter_of, resolve](CpuState& c, GuestMemory&) {
        Rect area; double scale{};
        static_cast<void>(resolve(*filter_of(c.x[0]), area, scale));
        c.v[0] = {std::bit_cast<std::uint32_t>(static_cast<float>(scale)), 0};   // (a float)
    }, "f@:");
    support.method(filter_class, "style", [filter_of](CpuState& c, GuestMemory&) { c.x[0] = filter_of(c.x[0])->single_window ? 1 : 2; }, "q@:");   // window / display

    // ---- SCStreamConfiguration: properties kept by name ----
    const auto config_of = [sp](Address self) { return sp->objc.host_state<ConfigState>(self); };
    const auto setter_name = [](std::string name) { name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0]))); return "set" + name + ":"; };
    for (const auto* name : {"width", "height", "pixelFormat", "queueDepth", "showsCursor", "scalesToFit", "capturesAudio",
                             "sampleRate", "channelCount", "excludesCurrentProcessAudio", "preservesAspectRatio",
                             "ignoreShadowsDisplay", "ignoreShadowsSingleWindow", "ignoreGlobalClipDisplay",
                             "ignoreGlobalClipSingleWindow", "shouldBeOpaque", "includeChildWindows", "showMouseClicks",
                             "capturesShadowsOnly", "captureResolution", "presenterOverlayPrivacyAlertSetting",
                             "streamName", "captureDynamicRange", "captureMicrophone", "colorMatrix", "colorSpaceName"}) {
        const std::string key = name;
        const bool object = key == "streamName" || key == "colorMatrix" || key == "colorSpaceName";
        if (object) {
            support.method(config_class, key, [config_of, key](CpuState& c, GuestMemory&) {
                const auto& objects = config_of(c.x[0])->objects;
                const auto found = objects.find(key);
                c.x[0] = found == objects.end() ? 0 : found->second;
            }, "@@:");
            support.method(config_class, setter_name(key), [sp, config_of, key](CpuState& c, GuestMemory&) {
                auto& slot = config_of(c.x[0])->objects[key];
                sp->release(std::exchange(slot, sp->retain(c.x[2])));
            }, "v@:@");
        } else {
            support.method(config_class, key, [config_of, key](CpuState& c, GuestMemory&) {
                const auto& values = config_of(c.x[0])->integers;
                const auto found = values.find(key);
                c.x[0] = static_cast<Address>(found == values.end() ? (key == "queueDepth" ? 8 : key == "pixelFormat" ? format_bgra : 0) : found->second);
            }, "q@:");
            support.method(config_class, setter_name(key), [config_of, key](CpuState& c, GuestMemory&) {
                config_of(c.x[0])->integers[key] = static_cast<std::int64_t>(c.x[2]);
            }, "v@:q");
        }
    }
    for (const auto* name : {"sourceRect", "destinationRect"}) {
        const std::string key = name;
        support.method(config_class, key, [config_of, key, rect_return](CpuState& c, GuestMemory&) {
            const auto& rects = config_of(c.x[0])->rects;
            const auto found = rects.find(key);
            rect_return(c, found == rects.end() ? Rect{} : found->second);
        }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
        support.method(config_class, setter_name(key), [config_of, key](CpuState& c, GuestMemory&) {
            config_of(c.x[0])->rects[key] = {{sk::real(c, 0), sk::real(c, 1)}, {sk::real(c, 2), sk::real(c, 3)}};
        }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    }
    // (CMTime: 24 bytes, returned through x8 and passed indirectly.)
    support.method(config_class, "minimumFrameInterval", [sp, config_of](CpuState& c, GuestMemory& m) {
        if (c.x[8]) m.write_bytes(c.x[8], config_of(c.x[0])->minimum_frame_interval);
    }, "{?=qiIq}@:");
    support.method(config_class, "setMinimumFrameInterval:", [config_of](CpuState& c, GuestMemory& m) {
        auto& slot = config_of(c.x[0])->minimum_frame_interval;
        if (c.x[2]) m.read_bytes(c.x[2], slot);
    }, "v@:{?=qiIq}");
    support.method(config_class, "dealloc", [sp, config_class](CpuState& c, GuestMemory&) {
        if (const auto state = sp->objc.find_host_state<ConfigState>(c.x[0]))
            for (auto& [key, value] : state->objects) sp->release(std::exchange(value, 0));
        static_cast<void>(sp->objc.send_super(c.x[0], config_class, sp->objc.selector("dealloc")));
    }, "v@:");

    // A frame of `filter` as `config` asks: the configured size (else the area at its scale).
    const auto frame = [sp, capture, resolve](Address filter, Address config, Rect& area, double& scale) -> Image {
        const auto filter_state = filter ? sp->objc.find_host_state<FilterState>(filter) : nullptr;
        if (!filter_state) return {};
        const auto windows = resolve(*filter_state, area, scale);
        const auto config_state = config ? sp->objc.find_host_state<ConfigState>(config) : nullptr;
        if (config_state) {
            if (const auto found = config_state->rects.find("sourceRect");
                found != config_state->rects.end() && found->second.size.width > 0 && found->second.size.height > 0)
                area = {{area.origin.x + found->second.origin.x, area.origin.y + found->second.origin.y}, found->second.size};
        }
        auto width = static_cast<std::uint32_t>(std::lround(area.size.width * scale));
        auto height = static_cast<std::uint32_t>(std::lround(area.size.height * scale));
        if (config_state) {
            if (const auto w = config_state->integers.find("width"); w != config_state->integers.end() && w->second > 0) width = static_cast<std::uint32_t>(w->second);
            if (const auto h = config_state->integers.find("height"); h != config_state->integers.end() && h->second > 0) height = static_cast<std::uint32_t>(h->second);
        }
        if (!width || !height || width > 16384 || height > 16384) return {};
        const std::array<std::uint8_t, 4> background = filter_state->single_window ? std::array<std::uint8_t, 4>{0, 0, 0, 0}
                                                                                   : std::array<std::uint8_t, 4>{0, 0, 0, 255};
        return capture->compose(area, width, height, windows, background);
    };

    // ---- SCScreenshotManager ----
    support.method(screenshot_class, "captureImageWithFilter:configuration:completionHandler:", [capture, frame, complete](CpuState& c, GuestMemory&) {
        Rect area; double scale{};
        const auto image = capture->cg_image(frame(c.x[2], c.x[3], area, scale));
        complete(c.x[4], image, image ? 0 : gui::security::bridge()->error(error_domain, sc_stream_error_user_declined, "No content to capture"));
    }, "v@:@@@?", true);
    support.method(screenshot_class, "captureImageInRect:completionHandler:", [capture, complete](CpuState& c, GuestMemory&) {
        const Rect area{{sk::real(c, 0), sk::real(c, 1)}, {sk::real(c, 2), sk::real(c, 3)}};
        const auto displays = capture->displays();
        const double scale = displays.empty() ? 1 : displays.front().scale;
        const auto image = capture->cg_image(capture->compose(area, static_cast<std::uint32_t>(std::lround(area.size.width * scale)),
                                                              static_cast<std::uint32_t>(std::lround(area.size.height * scale)),
                                                              capture->windows(true), {0, 0, 0, 255}));
        complete(c.x[2], image, image ? 0 : gui::security::bridge()->error(error_domain, sc_stream_error_user_declined, "No content to capture"));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@?", true);

    // +[SCShareableContent infoForFilter:]: the filter's style, scale and content rectangle.
    struct InfoState final : ObjectPayload { std::int64_t style{}; double scale{1}; Rect rect; };
    support.method(content_class, "infoForFilter:", [sp, make, info_class, resolve](CpuState& c, GuestMemory&) {
        const auto filter = c.x[2] ? sp->objc.find_host_state<FilterState>(c.x[2]) : nullptr;
        const auto made = make(info_class);
        const auto state = sp->objc.host_state<InfoState>(made);
        if (filter) {
            Rect area; double scale{};
            static_cast<void>(resolve(*filter, area, scale));
            state->style = filter->single_window ? 1 : 2;
            state->scale = scale;
            state->rect = {{0, 0}, area.size};
        }
        c.x[0] = sp->send_id(made, "autorelease");
    }, "@@:@", true);
    support.method(info_class, "style", [sp](CpuState& c, GuestMemory&) { c.x[0] = static_cast<Address>(sp->objc.host_state<InfoState>(c.x[0])->style); }, "q@:");
    support.method(info_class, "pointPixelScale", [sp](CpuState& c, GuestMemory&) {
        c.v[0] = {std::bit_cast<std::uint32_t>(static_cast<float>(sp->objc.host_state<InfoState>(c.x[0])->scale)), 0};
    }, "f@:");
    support.method(info_class, "contentRect", [sp, rect_return](CpuState& c, GuestMemory&) { rect_return(c, sp->objc.host_state<InfoState>(c.x[0])->rect); },
                   "{CGRect={CGPoint=dd}{CGSize=dd}}@:");

    // ---- SCStream ----
    const auto stream_of = [sp](Address self) { return sp->objc.host_state<StreamState>(self); };
    support.method(stream_class, "initWithFilter:configuration:delegate:", [sp, stream_of](CpuState& c, GuestMemory&) {
        const auto state = stream_of(c.x[0]);
        state->filter = sp->retain(c.x[2]);
        state->configuration = sp->send_id(c.x[3], "retain");
        state->delegate = c.x[4];
    }, "@@:@@@");
    // +1 CMSampleBuffer of `image` ('BGRA' IOSurface-backed pixel buffer) with its frame info attachments.
    const auto sample_buffer = [sp, key_object](const Image& image, const Rect& content, double scale) -> Address {
        const auto number = [sp](std::int64_t value) { return sp->cf.integer(value); };
        std::vector<std::pair<Address, Address>> entries{
            {sp->cf.string("IOSurfaceWidth"), number(image.width)}, {sp->cf.string("IOSurfaceHeight"), number(image.height)},
            {sp->cf.string("IOSurfaceBytesPerElement"), number(4)}, {sp->cf.string("IOSurfacePixelFormat"), number(format_bgra)}};
        const auto properties = sp->cf.dictionary(entries);
        for (const auto& [key, value] : entries) { sp->release(key); sp->release(value); }
        const auto surface = sp->call(sk::iosurface_library, "_IOSurfaceCreate", {properties});
        sp->release(properties);
        if (!surface) return 0;
        const auto base = sp->call(sk::iosurface_library, "_IOSurfaceGetBaseAddress", {surface});
        const auto row = sp->call(sk::iosurface_library, "_IOSurfaceGetBytesPerRow", {surface});
        for (std::uint32_t y = 0; y < image.height; ++y)
            sp->memory().write_bytes(base + y * row, std::span(image.bgra.data() + static_cast<std::size_t>(y) * image.width * 4,
                                                               static_cast<std::size_t>(image.width) * 4));
        const auto out = sp->process.allocate_guest(16 + 72, 8);   // pixel buffer, format, CMSampleTimingInfo
        struct Free { sk::Support& s; Address a; ~Free() { s.process.release_guest(a); } } owned{*sp, out};
        const auto buffer_slot = out, format_slot = out + 8, timing = out + 16;
        const bool made_buffer = sp->call(core_video, "_CVPixelBufferCreateWithIOSurface", {0, surface, 0, buffer_slot}) == 0;
        sp->release(surface);   // (the pixel buffer holds it)
        if (!made_buffer) return 0;
        const auto pixel_buffer = sp->memory().read64(buffer_slot);
        Address sample{};
        if (sp->call(core_media, "_CMVideoFormatDescriptionCreateForImageBuffer", {0, pixel_buffer, format_slot}) == 0) {
            const auto format = sp->memory().read64(format_slot);
            const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            const std::array<std::uint8_t, 72> zero{};
            sp->memory().write_bytes(timing, zero);   // duration and decode time invalid
            sp->memory().write64(timing + 24, static_cast<std::uint64_t>(nanoseconds));   // presentation time: value
            sp->memory().write32(timing + 32, 1000000000);                                // timescale
            sp->memory().write32(timing + 36, 1);                                         // kCMTimeFlags_Valid
            if (sp->call(core_media, "_CMSampleBufferCreateReadyWithImageBuffer", {0, pixel_buffer, format, timing, buffer_slot}) == 0)
                sample = sp->memory().read64(buffer_slot);
            sp->release(format);
        }
        sp->release(pixel_buffer);
        if (!sample) return 0;
        if (const auto attachments = sp->call(core_media, "_CMSampleBufferGetSampleAttachmentsArray", {sample, 1})) {
            const auto dictionary = sp->cf.element(attachments, 0);
            const auto set = [&](const char* key, Address value) {
                static_cast<void>(sp->call(core_foundation, "_CFDictionarySetValue", {dictionary, key_object(key), value}));
                sp->release(value);
            };
            set("SCStreamFrameInfoStatus", sp->cf.integer(0));   // SCFrameStatusComplete
            set("SCStreamFrameInfoDisplayTime", sp->cf.integer(static_cast<std::int64_t>(sp->call(sk::libsystem_library, "_mach_absolute_time", {}))));
            set("SCStreamFrameInfoScaleFactor", sp->cf.real(scale));
            set("SCStreamFrameInfoContentScale", sp->cf.real(1));
            CallRegs rect;
            rect.set_d(0, 0); rect.set_d(1, 0); rect.set_d(2, content.size.width); rect.set_d(3, content.size.height);
            set("SCStreamFrameInfoContentRect", sp->call(core_graphics, "_CGRectCreateDictionaryRepresentation", rect).x0);
        }
        return sample;
    };
    // One frame to every screen output (on its queue), within the queue depth.
    const auto tick = [sp, frame, sample_buffer](Address stream) {
        const auto state = sp->objc.find_host_state<StreamState>(stream);
        if (!state) return;
        std::vector<StreamOutput> outputs;
        Address filter{}, config{};
        {
            std::lock_guard lock(state->mutex);
            const auto config_state = state->configuration ? sp->objc.find_host_state<ConfigState>(state->configuration) : nullptr;
            const auto depth = config_state && config_state->integers.contains("queueDepth") ? config_state->integers.at("queueDepth") : 8;
            if (state->in_flight >= static_cast<std::uint32_t>(std::max<std::int64_t>(depth, 1))) return;
            for (const auto& output : state->outputs) if (output.type == 0) outputs.push_back(output);
            filter = state->filter;
            config = state->configuration;
        }
        if (outputs.empty()) return;
        Rect area; double scale{};
        const auto image = frame(filter, config, area, scale);
        if (!image.width) return;
        const auto sample = sample_buffer(image, area, scale);
        if (!sample) return;
        const auto bridge = gui::security::bridge();
        for (const auto& output : outputs) {
            { std::lock_guard lock(state->mutex); ++state->in_flight; }
            const auto held_stream = sp->retain(stream), held_output = sp->retain(output.output), held_sample = sp->retain(sample);
            bridge->dispatch_async(output.queue ? output.queue : bridge->main_queue(), [sp, held_stream, held_output, held_sample] {
                CallRegs deliver;
                deliver.x[2] = held_stream;
                deliver.x[3] = held_sample;
                deliver.x[4] = 0;   // SCStreamOutputTypeScreen
                if (sp->objc.responds(held_output, sp->objc.selector("stream:didOutputSampleBuffer:ofType:")))
                    static_cast<void>(sp->send(held_output, "stream:didOutputSampleBuffer:ofType:", deliver));
                if (const auto state = sp->objc.find_host_state<StreamState>(held_stream)) {
                    std::lock_guard lock(state->mutex);
                    if (state->in_flight) --state->in_flight;
                }
                sp->release(held_sample);
                sp->release(held_output);
                sp->release(held_stream);
            });
        }
        sp->release(sample);
    };
    support.method(stream_class, "addStreamOutput:type:sampleHandlerQueue:error:", [sp, stream_of](CpuState& c, GuestMemory& m) {
        const auto type = static_cast<std::int64_t>(c.x[3]);
        if (type != 0) {   // audio / microphone: no audio is captured
            if (c.x[5]) m.write64(c.x[5], sp->send_id(gui::security::bridge()->error(error_domain, sc_stream_error_user_declined, "Audio capture is not available"), "autorelease"));
            c.x[0] = 0;
            return;
        }
        const auto state = stream_of(c.x[0]);
        std::lock_guard lock(state->mutex);
        state->outputs.push_back({sp->retain(c.x[2]), c.x[4], type});
        c.x[0] = 1;
    }, "B@:@q@^@");
    support.method(stream_class, "removeStreamOutput:type:error:", [sp, stream_of](CpuState& c, GuestMemory&) {
        const auto state = stream_of(c.x[0]);
        Address removed{};
        {
            std::lock_guard lock(state->mutex);
            const auto found = std::find_if(state->outputs.begin(), state->outputs.end(),
                                            [&](const StreamOutput& o) { return o.output == c.x[2] && o.type == static_cast<std::int64_t>(c.x[3]); });
            if (found != state->outputs.end()) { removed = found->output; state->outputs.erase(found); }
        }
        sp->release(removed);
        c.x[0] = removed ? 1 : 0;
    }, "B@:@q^@");
    const auto interval_of = [sp](Address config) {
        const auto state = config ? sp->objc.find_host_state<ConfigState>(config) : nullptr;
        if (!state) return 1.0 / 60;
        const auto& t = state->minimum_frame_interval;
        std::int64_t value{}; std::int32_t timescale{}; std::uint32_t flags{};
        std::memcpy(&value, t.data(), 8); std::memcpy(&timescale, t.data() + 8, 4); std::memcpy(&flags, t.data() + 12, 4);
        const double seconds = (flags & 1) && timescale > 0 ? static_cast<double>(value) / timescale : 0;
        return seconds > 0 ? seconds : 1.0 / 60;
    };
    const auto stop = [sp, stream_of](Address stream) {
        const auto state = stream_of(stream);
        gui::TimerId timer{};
        { std::lock_guard lock(state->mutex); timer = std::exchange(state->timer, 0); }
        if (timer && gui::main_run_loop_available()) gui::main_run_loop().cancel_timer(timer);
        if (timer) sp->release(stream);   // the timer's reference
    };
    support.method(stream_class, "startCaptureWithCompletionHandler:", [sp, stream_of, tick, interval_of, done](CpuState& c, GuestMemory&) {
        const auto stream = c.x[0];
        const auto state = stream_of(stream);
        if (!gui::main_run_loop_available()) {
            done(c.x[2], gui::security::bridge()->error(error_domain, sc_stream_error_user_declined, "No run loop to capture on"));
            return;
        }
        Address config{};
        { std::lock_guard lock(state->mutex); config = state->configuration; }
        const auto interval = interval_of(config);
        {
            std::lock_guard lock(state->mutex);
            if (!state->timer) {
                sp->retain(stream);   // held by the timer until the capture stops
                state->timer = gui::main_run_loop().add_timer(gui::monotonic_seconds() + interval, interval, gui::common_modes, [tick, stream](gui::TimerId) { tick(stream); });
            }
        }
        done(c.x[2], 0);
    }, "v@:@?");
    support.method(stream_class, "stopCaptureWithCompletionHandler:", [stop, done](CpuState& c, GuestMemory&) {
        stop(c.x[0]);
        done(c.x[2], 0);
    }, "v@:@?");
    const auto replace = [sp, stream_of](Address stream, Address value, bool configuration) {
        const auto state = stream_of(stream);
        Address old{};
        {
            std::lock_guard lock(state->mutex);
            auto& slot = configuration ? state->configuration : state->filter;
            old = std::exchange(slot, sp->retain(value));
        }
        sp->release(old);
    };
    for (const auto& [selector, configuration] : std::initializer_list<std::pair<const char*, bool>>{
             {"updateConfiguration:completionHandler:", true}, {"updateContentFilter:completionHandler:", false}}) {
        support.method(stream_class, selector, [replace, done, configuration = configuration](CpuState& c, GuestMemory&) {
            replace(c.x[0], c.x[2], configuration);
            done(c.x[3], 0);
        }, "v@:@@?");
    }
    support.method(stream_class, "dealloc", [sp, stream_class](CpuState& c, GuestMemory&) {
        if (const auto state = sp->objc.find_host_state<StreamState>(c.x[0])) {
            for (auto& output : state->outputs) sp->release(std::exchange(output.output, 0));
            sp->release(std::exchange(state->filter, 0));
            sp->release(std::exchange(state->configuration, 0));
        }
        static_cast<void>(sp->objc.send_super(c.x[0], stream_class, sp->objc.selector("dealloc")));
    }, "v@:");

    // ---- SCContentSharingPicker: the system picker's singleton (no picker window is shown) ----
    struct PickerState final : ObjectPayload { std::int64_t active{}; Address maximum{}; };   // maximum: an NSNumber (+1)
    support.method(picker_class, "sharedPicker", [picker_class, make](CpuState& c, GuestMemory&) {
        static Address shared{};
        if (!shared) shared = make(picker_class);
        c.x[0] = shared;
    }, "@@:", true);
    support.method(picker_class, "isActive", [sp](CpuState& c, GuestMemory&) { c.x[0] = static_cast<Address>(sp->objc.host_state<PickerState>(c.x[0])->active); }, "B@:");
    support.method(picker_class, "setActive:", [sp](CpuState& c, GuestMemory&) { sp->objc.host_state<PickerState>(c.x[0])->active = c.x[2] & 0xff; }, "v@:B");
    support.method(picker_class, "maximumStreamCount", [sp](CpuState& c, GuestMemory&) { c.x[0] = sp->objc.host_state<PickerState>(c.x[0])->maximum; }, "@@:");
    support.method(picker_class, "setMaximumStreamCount:", [sp](CpuState& c, GuestMemory&) {
        auto& slot = sp->objc.host_state<PickerState>(c.x[0])->maximum;
        sp->release(std::exchange(slot, sp->retain(c.x[2])));
    }, "v@:@");
    for (const auto* name : {"addObserver:", "removeObserver:"}) support.method(picker_class, name, [](CpuState&, GuestMemory&) {}, "v@:@");

    // ---- CoreGraphics window lists (the same scope) ----
    // kCGWindowListOption: OnScreenOnly 1, OnScreenAboveWindow 2, OnScreenBelowWindow 4, IncludingWindow 8.
    const auto listed = [capture](std::uint64_t option, WindowId relative) {
        auto windows = capture->windows((option & 7) != 0);
        if (option & 8) std::erase_if(windows, [&](const OwnWindow& w) { return w.id != relative; });
        else if (option & 6) {
            const auto at = std::find_if(windows.begin(), windows.end(), [&](const OwnWindow& w) { return w.id == relative; });
            if (at != windows.end()) {
                if (option & 2) windows.erase(at, windows.end());   // above: in front of it
                else windows.erase(windows.begin(), at + 1);        // below: behind it
            }
        }
        return windows;
    };
    const auto description = [sp, capture](const OwnWindow& w) {   // +1
        CallRegs bounds;
        bounds.set_d(0, w.frame.origin.x); bounds.set_d(1, w.frame.origin.y); bounds.set_d(2, w.frame.size.width); bounds.set_d(3, w.frame.size.height);
        std::vector<std::pair<Address, Address>> entries{
            {sp->cf.string("kCGWindowNumber"), sp->cf.integer(static_cast<std::int64_t>(w.id), 3)},
            {sp->cf.string("kCGWindowOwnerPID"), sp->cf.integer(capture->pid(), 3)},
            {sp->cf.string("kCGWindowLayer"), sp->cf.integer(w.level, 3)},
            {sp->cf.string("kCGWindowBounds"), sp->call(core_graphics, "_CGRectCreateDictionaryRepresentation", bounds).x0},
            {sp->cf.string("kCGWindowOwnerName"), sp->cf.string(capture->application_name())},
            {sp->cf.string("kCGWindowName"), sp->cf.string(w.title)},
            {sp->cf.string("kCGWindowIsOnscreen"), sp->retain(sp->cf.boolean(w.on_screen))},
            {sp->cf.string("kCGWindowAlpha"), sp->cf.real(1)},
            {sp->cf.string("kCGWindowSharingState"), sp->cf.integer(1, 3)},   // kCGWindowSharingReadOnly
            {sp->cf.string("kCGWindowStoreType"), sp->cf.integer(2, 3)}};     // kCGBackingStoreBuffered
        const auto dictionary = sp->cf.dictionary(entries);
        for (const auto& [key, value] : entries) { sp->release(key); sp->release(value); }
        return dictionary;
    };
    support.function(core_graphics, "_CGWindowListCopyWindowInfo", [sp, listed, description](CpuState& c, GuestMemory&) {
        std::vector<Address> values;
        for (const auto& w : listed(c.x[0], static_cast<WindowId>(static_cast<std::uint32_t>(c.x[1])))) values.push_back(description(w));
        c.x[0] = sp->cf.array(values);
        for (const auto value : values) sp->release(value);
    });
    // CFArrays of CGWindowIDs (no callbacks: the values are the numbers).
    const auto id_array = [sp](const std::vector<OwnWindow>& windows) {
        const auto values = sp->process.allocate_guest(std::max<std::size_t>(windows.size(), 1) * 8, 8);
        for (std::size_t i = 0; i < windows.size(); ++i) sp->memory().write64(values + i * 8, windows[i].id);
        const auto array = sp->call(core_foundation, "_CFArrayCreate", {0, values, windows.size(), 0});
        sp->process.release_guest(values);
        return array;
    };
    support.function(core_graphics, "_CGWindowListCreate", [listed, id_array](CpuState& c, GuestMemory&) {
        c.x[0] = id_array(listed(c.x[0], static_cast<WindowId>(static_cast<std::uint32_t>(c.x[1]))));
    });
    // The own windows among `array`'s CGWindowIDs (the others are left out), front to back.
    const auto chosen = [sp, capture](Address array) {
        std::set<WindowId> wanted;
        const auto count = array ? sp->call(core_foundation, "_CFArrayGetCount", {array}) : 0;
        for (Address i = 0; i < count; ++i) wanted.insert(static_cast<WindowId>(static_cast<std::uint32_t>(sp->call(core_foundation, "_CFArrayGetValueAtIndex", {array, i}))));
        auto windows = capture->windows(false);
        std::erase_if(windows, [&](const OwnWindow& w) { return !wanted.contains(w.id); });
        return windows;
    };
    support.function(core_graphics, "_CGWindowListCreateDescriptionFromArray", [sp, chosen, description](CpuState& c, GuestMemory&) {
        std::vector<Address> values;
        for (const auto& w : chosen(c.x[0])) values.push_back(description(w));
        c.x[0] = sp->cf.array(values);
        for (const auto value : values) sp->release(value);
    });
    // CGWindowListCreateImage(screenBounds d0-d3 (CGRectNull: the windows' union), option, windowID, imageOption).
    const auto window_image = [capture](Rect area, const std::vector<OwnWindow>& windows) -> Address {
        if (std::isinf(area.origin.x) || std::isinf(area.origin.y) || !(area.size.width > 0) || !(area.size.height > 0)) {
            if (windows.empty()) return 0;
            double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
            for (const auto& w : windows) {
                x0 = std::min(x0, w.content.origin.x); y0 = std::min(y0, w.content.origin.y);
                x1 = std::max(x1, w.content.origin.x + w.content.size.width); y1 = std::max(y1, w.content.origin.y + w.content.size.height);
            }
            area = {{x0, y0}, {x1 - x0, y1 - y0}};
        }
        const auto displays = capture->displays();
        const double scale = displays.empty() ? 1 : displays.front().scale;
        const auto width = static_cast<std::uint32_t>(std::lround(area.size.width * scale)), height = static_cast<std::uint32_t>(std::lround(area.size.height * scale));
        if (!width || !height || width > 16384 || height > 16384) return 0;
        return capture->cg_image(capture->compose(area, width, height, windows, {0, 0, 0, 0}));
    };
    support.function(core_graphics, "_CGWindowListCreateImage", [listed, window_image](CpuState& c, GuestMemory&) {
        const Rect area{{sk::real(c, 0), sk::real(c, 1)}, {sk::real(c, 2), sk::real(c, 3)}};
        c.x[0] = window_image(area, listed(c.x[0], static_cast<WindowId>(static_cast<std::uint32_t>(c.x[1]))));
    });
    support.function(core_graphics, "_CGWindowListCreateImageFromArray", [chosen, window_image](CpuState& c, GuestMemory&) {
        const Rect area{{sk::real(c, 0), sk::real(c, 1)}, {sk::real(c, 2), sk::real(c, 3)}};
        c.x[0] = window_image(area, chosen(c.x[0]));
    });
}

} // namespace alchemy::screencapturekit
