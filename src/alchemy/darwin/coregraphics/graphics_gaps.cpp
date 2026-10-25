#include "alchemy/darwin/kit/fwgaps/fwgaps.hpp"

#include "alchemy/gui/cg_event_record.hpp"
#include "alchemy/host/ui.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// Graphics-side gaps of the Qt WebEngine images: CoreGraphics, QuartzCore,
// CoreText, IOSurface, CoreImage, Carbon and ApplicationServices. (OpenGL
// and CGL are darwin/opengl/'s.)
//
// - CGEvent objects are real (type, location, flags, timestamp, integer
//   fields; CGEventCreate(NULL) samples the mouse location) and round-trip
//   through CGEventCreateData/CGEventCreateFromData. Seconds since the last
//   input event come from the host's last-input time.
// - Screen capture (display streams, window images, window lists) is not
//   available: creation returns NULL and the access checks answer "denied".
// - PDF documents and contexts cannot be created (NULL); the private CGS
//   window-server calls report kCGErrorInvalidConnection.
// - CATransform3D helpers, CoreText font collections, available tables,
//   character sets and system-UI-font checks are real. Font variations are
//   not modelled (NULL).
namespace alchemy::gui::fill {
namespace {

constexpr std::int32_t cg_illegal_argument = 1001, cg_invalid_connection = 1002;

struct CgEvent final : ObjectPayload {
    std::mutex mutex;
    std::uint32_t type{};
    double x{}, y{};
    std::uint64_t flags{}, timestamp{};
    std::map<std::uint32_t, std::int64_t> fields;
    std::map<std::uint32_t, double> doubles;   // fields set as doubles (CGEventSetDoubleValueField)
    std::u16string unicode;                    // CGEventKeyboardSetUnicodeString
};
// CGEventSourceRef: its state ID and the settings events made with it take.
struct CgEventSource final : ObjectPayload {
    std::mutex mutex;
    std::int32_t state_id{};
    std::uint32_t keyboard_type{40};           // kCGAnyInputEventType's ANSI keyboard (unverified)
    double pixels_per_line{10}, suppression_interval{0.25};
    std::int64_t user_data{};
};

struct FontCollection final : ObjectPayload { Address descriptors{}; };

class GraphicsGaps final : public Provider {
public:
    using Provider::Provider;
    Address event_class{}, collection_class{}, source_class{};
    std::mutex mutex;
    std::uint32_t secure_input{};

    // A non-null argument of a type Alchemy can never create is a caller error.
    static void impossible(std::uint64_t object, std::string_view what) {
        if (object) throw std::runtime_error("Alchemy: " + std::string(what) + " received an object that cannot exist under Alchemy");
    }
    std::shared_ptr<CgEvent> event(Address object) {
        const auto value = object ? objc.find_host_state<CgEvent>(object) : nullptr;
        if (!value) throw std::runtime_error("Alchemy: the object is not a CGEvent");
        return value;
    }
    Address make_event(std::shared_ptr<CgEvent> value) {
        const auto object = objc.allocate(event_class);
        static_cast<void>(objc.host_state<CgEvent>(object, [&] { return value; }));
        return object;
    }
    std::shared_ptr<CgEventSource> source(Address object) {
        return object ? objc.find_host_state<CgEventSource>(object) : nullptr;
    }
    // A new event of `type` at the mouse location now; a source gives its
    // state ID and user data.
    std::shared_ptr<CgEvent> new_event(Address source_object, std::uint32_t type) {
        auto value = std::make_shared<CgEvent>();
        value->type = type;
        const auto [x, y] = mouse_location();
        value->x = x;
        value->y = y;
        value->timestamp = call(libsystem, "_mach_absolute_time", {});
        if (const auto origin = source(source_object)) {
            std::lock_guard lock(origin->mutex);
            value->fields[cg_event_source_state_id] = origin->state_id;
            if (origin->user_data) value->fields[cg_event_source_user_data] = origin->user_data;
        }
        return value;
    }
    static constexpr std::uint32_t cg_event_source_user_data = 42, cg_event_source_state_id = 45, cg_event_target_pid = 40;
    // The main display's height in points (CGEventGetUnflippedLocation).
    double main_display_height() {
        try {
            const auto screen_type = objc.find_class("NSScreen");
            const auto screens = screen_type ? objc.send(screen_type, "screens").x0 : 0;
            if (!screens || cf.length(screens) == 0) return 0;
            return objc.send(cf.element(screens, 0), "frame").d(3);
        } catch (const std::exception&) {
            return 0;
        }
    }
    // The mouse location in global display coordinates (top-left origin).
    std::pair<double, double> mouse_location() {
        try {
            const auto event_type = objc.find_class("NSEvent");
            const auto screen_type = objc.find_class("NSScreen");
            if (!event_type || !screen_type) return {0, 0};
            const auto point = objc.send(event_type, "mouseLocation");
            const auto screens = objc.send(screen_type, "screens").x0;
            if (!screens || cf.length(screens) == 0) return {point.d(0), point.d(1)};
            const auto frame = objc.send(cf.element(screens, 0), "frame");
            return {point.d(0), frame.d(3) - point.d(1)};
        } catch (const std::exception&) {
            return {0, 0};
        }
    }

    void install();
    void install_core_graphics();
    void install_quartz_core();
    void install_core_text();
    void install_misc();
};

void GraphicsGaps::install_core_graphics() {
    event_class = objc.find_class("__AlchemyCGEvent");
    if (!event_class) event_class = objc.register_native_class("__AlchemyCGEvent", objc.find_class("NSObject"), 8);
    register_type(event_class, TypeId::cg_event);
    raw(core_graphics, "_CGEventCreate", [this](CpuState& s, GuestMemory&) {
        auto value = std::make_shared<CgEvent>();
        const auto [x, y] = mouse_location();
        value->x = x;
        value->y = y;
        value->timestamp = call(libsystem, "_mach_absolute_time", {});
        s.x[0] = make_event(value);
    });
    raw(core_graphics, "_CGEventGetLocation", [this](CpuState& s, GuestMemory&) {
        const auto value = event(s.x[0]);
        std::lock_guard lock(value->mutex);
        return_real(s, value->x, 0);
        return_real(s, value->y, 1);
    });
    raw(core_graphics, "_CGEventGetIntegerValueField", [this](CpuState& s, GuestMemory&) {
        const auto value = event(s.x[0]);
        std::lock_guard lock(value->mutex);
        const auto key = static_cast<std::uint32_t>(s.x[1]);
        if (const auto found = value->fields.find(key); found != value->fields.end()) { s.x[0] = static_cast<std::uint64_t>(found->second); return; }
        const auto as_double = value->doubles.find(key);
        s.x[0] = as_double == value->doubles.end() ? 0 : static_cast<std::uint64_t>(static_cast<std::int64_t>(as_double->second));
    });
    // ---- events made by the application (frameworks wave U6) ----
    // CGEventCreateKeyboardEvent(source, virtualKey, keyDown).
    raw(core_graphics, "_CGEventCreateKeyboardEvent", [this](CpuState& s, GuestMemory&) {
        auto value = new_event(s.x[0], (s.x[2] & 0xff) ? 10u : 11u);   // kCGEventKeyDown / KeyUp
        value->fields[cg_event::field::keyboard_keycode] = static_cast<std::uint16_t>(s.x[1]);
        if (const auto origin = source(s.x[0])) {
            std::lock_guard lock(origin->mutex);
            value->fields[cg_event::field::keyboard_type] = origin->keyboard_type;
        }
        s.x[0] = make_event(std::move(value));
    });
    // CGEventCreateMouseEvent(source, type, location, button).
    raw(core_graphics, "_CGEventCreateMouseEvent", [this](CpuState& s, GuestMemory&) {
        auto value = new_event(s.x[0], static_cast<std::uint32_t>(s.x[1]));
        value->x = real(s, 0);
        value->y = real(s, 1);
        value->fields[cg_event::field::mouse_button_number] = static_cast<std::int64_t>(static_cast<std::uint32_t>(s.x[2]));
        value->fields[cg_event::field::mouse_click_state] = 1;
        s.x[0] = make_event(std::move(value));
    });
    // CGEventCreateScrollWheelEvent2(source, units, wheelCount, wheel1, wheel2, wheel3):
    // line units set the line deltas, pixel units the point deltas of a
    // continuous scroll. The variadic CGEventCreateScrollWheelEvent passes
    // the extra wheels on the stack (Apple arm64).
    const auto scroll = [this](CpuState& s, std::array<std::int32_t, 3> wheels) {
        auto value = new_event(s.x[0], 22);   // kCGEventScrollWheel
        const bool pixels = static_cast<std::uint32_t>(s.x[1]) == 0;   // kCGScrollEventUnitPixel
        const auto count = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(s.x[2]), 1, 3);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto delta = wheels[i];
            if (pixels) value->fields[cg_event::field::scroll_point_delta_axis1 + i] = delta;
            else value->fields[cg_event::field::scroll_delta_axis1 + i] = delta;
        }
        if (pixels) value->fields[cg_event::field::scroll_is_continuous] = 1;
        return make_event(std::move(value));
    };
    raw(core_graphics, "_CGEventCreateScrollWheelEvent2", [scroll](CpuState& s, GuestMemory&) {
        s.x[0] = scroll(s, {static_cast<std::int32_t>(s.x[3]), static_cast<std::int32_t>(s.x[4]), static_cast<std::int32_t>(s.x[5])});
    });
    raw(core_graphics, "_CGEventCreateScrollWheelEvent", [scroll](CpuState& s, GuestMemory& m) {
        // (source, units, wheelCount, wheel1, ...): wheel1 in w3; the variadic rest in 8-byte stack slots.
        const auto count = static_cast<std::uint32_t>(s.x[2]);
        const auto second = count > 1 ? static_cast<std::int32_t>(m.read64(s.sp)) : 0;
        const auto third = count > 2 ? static_cast<std::int32_t>(m.read64(s.sp + 8)) : 0;
        s.x[0] = scroll(s, {static_cast<std::int32_t>(s.x[3]), second, third});
    });
    raw(core_graphics, "_CGEventCreateCopy", [this](CpuState& s, GuestMemory&) {
        if (!s.x[0]) { s.x[0] = 0; return; }
        const auto original = event(s.x[0]);
        auto copy = std::make_shared<CgEvent>();
        {
            std::lock_guard lock(original->mutex);
            copy->type = original->type;
            copy->x = original->x;
            copy->y = original->y;
            copy->flags = original->flags;
            copy->timestamp = original->timestamp;
            copy->fields = original->fields;
            copy->doubles = original->doubles;
            copy->unicode = original->unicode;
        }
        s.x[0] = make_event(std::move(copy));
    });
    raw(core_graphics, "_CGEventGetTypeID", [this](CpuState& s, GuestMemory&) {
        s.x[0] = call("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "_CFGetTypeID", {make_event(std::make_shared<CgEvent>())});
    });
    raw(core_graphics, "_CGEventGetType", [this](CpuState& s, GuestMemory&) {
        const auto value = event(s.x[0]); std::lock_guard lock(value->mutex); s.x[0] = value->type;
    });
    raw(core_graphics, "_CGEventSetType", [this](CpuState& s, GuestMemory&) {
        const auto value = event(s.x[0]); std::lock_guard lock(value->mutex); value->type = static_cast<std::uint32_t>(s.x[1]);
    });
    raw(core_graphics, "_CGEventSetLocation", [this](CpuState& s, GuestMemory&) {
        const auto value = event(s.x[0]); std::lock_guard lock(value->mutex); value->x = real(s, 0); value->y = real(s, 1);
    });
    // The location with the origin at the main display's bottom-left.
    raw(core_graphics, "_CGEventGetUnflippedLocation", [this](CpuState& s, GuestMemory&) {
        const auto value = event(s.x[0]);
        double x{}, y{};
        { std::lock_guard lock(value->mutex); x = value->x; y = value->y; }
        return_real(s, x, 0);
        return_real(s, main_display_height() - y, 1);
    });
    raw(core_graphics, "_CGEventGetFlags", [this](CpuState& s, GuestMemory&) {
        const auto value = event(s.x[0]); std::lock_guard lock(value->mutex); s.x[0] = value->flags;
    });
    raw(core_graphics, "_CGEventSetFlags", [this](CpuState& s, GuestMemory&) {
        const auto value = event(s.x[0]); std::lock_guard lock(value->mutex); value->flags = s.x[1];
    });
    raw(core_graphics, "_CGEventGetTimestamp", [this](CpuState& s, GuestMemory&) {
        const auto value = event(s.x[0]); std::lock_guard lock(value->mutex); s.x[0] = value->timestamp;
    });
    raw(core_graphics, "_CGEventSetTimestamp", [this](CpuState& s, GuestMemory&) {
        const auto value = event(s.x[0]); std::lock_guard lock(value->mutex); value->timestamp = s.x[1];
    });
    raw(core_graphics, "_CGEventSetIntegerValueField", [this](CpuState& s, GuestMemory&) {
        const auto value = event(s.x[0]);
        std::lock_guard lock(value->mutex);
        const auto key = static_cast<std::uint32_t>(s.x[1]);
        value->doubles.erase(key);
        value->fields[key] = static_cast<std::int64_t>(s.x[2]);
    });
    raw(core_graphics, "_CGEventGetDoubleValueField", [this](CpuState& s, GuestMemory&) {
        const auto value = event(s.x[0]);
        std::lock_guard lock(value->mutex);
        const auto key = static_cast<std::uint32_t>(s.x[1]);
        if (const auto found = value->doubles.find(key); found != value->doubles.end()) { return_real(s, found->second); return; }
        const auto as_integer = value->fields.find(key);
        return_real(s, as_integer == value->fields.end() ? 0.0 : static_cast<double>(as_integer->second));
    });
    raw(core_graphics, "_CGEventSetDoubleValueField", [this](CpuState& s, GuestMemory&) {
        const auto value = event(s.x[0]);
        std::lock_guard lock(value->mutex);
        const auto key = static_cast<std::uint32_t>(s.x[1]);
        value->fields.erase(key);
        value->doubles[key] = real(s, 0);
    });
    // The characters a keyboard event carries (UTF-16 units).
    raw(core_graphics, "_CGEventKeyboardSetUnicodeString", [this](CpuState& s, GuestMemory& m) {
        const auto value = event(s.x[0]);
        std::u16string text(static_cast<std::size_t>(s.x[1]), u'\0');
        for (std::size_t i = 0; i < text.size(); ++i) text[i] = static_cast<char16_t>(m.read8(s.x[2] + i * 2) | (m.read8(s.x[2] + i * 2 + 1) << 8));
        std::lock_guard lock(value->mutex);
        value->unicode = std::move(text);
    });
    raw(core_graphics, "_CGEventKeyboardGetUnicodeString", [this](CpuState& s, GuestMemory& m) {
        const auto value = event(s.x[0]);
        std::u16string text;
        { std::lock_guard lock(value->mutex); text = value->unicode; }
        const auto room = static_cast<std::size_t>(s.x[1]);
        if (s.x[2]) m.write64(s.x[2], text.size());   // UniCharCount* actualStringLength
        for (std::size_t i = 0; i < std::min(room, text.size()) && s.x[3]; ++i) {
            m.write8(s.x[3] + i * 2, static_cast<std::uint8_t>(text[i]));
            m.write8(s.x[3] + i * 2 + 1, static_cast<std::uint8_t>(text[i] >> 8));
        }
    });
    // Posting to this process delivers the event to the application's queue
    // as an NSEvent; other processes cannot be reached.
    const auto post_here = [this](Address event_object) {
        const auto ns_event = objc.find_class("NSEvent");
        const auto app_class = objc.find_class("NSApplication");
        if (!ns_event || !app_class) throw std::runtime_error("Alchemy: posting a CGEvent needs AppKit");
        CallRegs make{};
        make.x[2] = event_object;
        const auto converted = objc.send(ns_event, "eventWithCGEvent:", make).x0;
        const auto app = objc.send(app_class, "sharedApplication").x0;
        CallRegs post{};
        post.x[2] = converted;
        post.x[3] = 0;
        if (converted && app) static_cast<void>(objc.send(app, "postEvent:atStart:", post));
    };
    raw(core_graphics, "_CGEventPostToPid", [this, post_here](CpuState& s, GuestMemory&) {
        const auto pid = static_cast<std::int32_t>(s.x[0]);
        if (pid != static_cast<std::int32_t>(call(libsystem, "_getpid", {})))
            throw std::runtime_error("Alchemy: CGEventPostToPid to another process is unsupported (no system event delivery)");
        post_here(s.x[1]);
    });
    // CGEventPostToPSN(ProcessSerialNumber*, event): the current process
    // ({0, kCurrentProcess = 2}) only.
    raw(core_graphics, "_CGEventPostToPSN", [post_here](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || m.read32(s.x[0]) != 0 || m.read32(s.x[0] + 4) != 2)
            throw std::runtime_error("Alchemy: CGEventPostToPSN to another process is unsupported (no system event delivery)");
        post_here(s.x[1]);
    });
    // ---- event sources ----
    source_class = objc.find_class("__AlchemyCGEventSource");
    if (!source_class) source_class = objc.register_native_class("__AlchemyCGEventSource", objc.find_class("NSObject"), 8);
    raw(core_graphics, "_CGEventSourceCreate", [this](CpuState& s, GuestMemory&) {
        auto value = std::make_shared<CgEventSource>();
        value->state_id = static_cast<std::int32_t>(s.x[0]);
        const auto object = objc.allocate(source_class);
        static_cast<void>(objc.host_state<CgEventSource>(object, [&] { return value; }));
        s.x[0] = object;
    });
    const auto with_source = [this](Address object) {
        const auto value = source(object);
        if (!value) throw std::runtime_error("Alchemy: the object is not a CGEventSource");
        return value;
    };
    raw(core_graphics, "_CGEventSourceGetSourceStateID", [with_source](CpuState& s, GuestMemory&) {
        const auto value = with_source(s.x[0]); std::lock_guard lock(value->mutex); s.x[0] = static_cast<std::uint32_t>(value->state_id);
    });
    raw(core_graphics, "_CGEventSourceGetKeyboardType", [with_source](CpuState& s, GuestMemory&) {
        const auto value = with_source(s.x[0]); std::lock_guard lock(value->mutex); s.x[0] = value->keyboard_type;
    });
    raw(core_graphics, "_CGEventSourceSetKeyboardType", [with_source](CpuState& s, GuestMemory&) {
        const auto value = with_source(s.x[0]); std::lock_guard lock(value->mutex); value->keyboard_type = static_cast<std::uint32_t>(s.x[1]);
    });
    raw(core_graphics, "_CGEventSourceGetPixelsPerLine", [with_source](CpuState& s, GuestMemory&) {
        const auto value = with_source(s.x[0]); std::lock_guard lock(value->mutex); return_real(s, value->pixels_per_line);
    });
    raw(core_graphics, "_CGEventSourceSetPixelsPerLine", [with_source](CpuState& s, GuestMemory&) {
        const auto value = with_source(s.x[0]); std::lock_guard lock(value->mutex); value->pixels_per_line = real(s, 0);
    });
    raw(core_graphics, "_CGEventSourceGetLocalEventsSuppressionInterval", [with_source](CpuState& s, GuestMemory&) {
        const auto value = with_source(s.x[0]); std::lock_guard lock(value->mutex); return_real(s, value->suppression_interval);
    });
    raw(core_graphics, "_CGEventSourceSetLocalEventsSuppressionInterval", [with_source](CpuState& s, GuestMemory&) {
        const auto value = with_source(s.x[0]); std::lock_guard lock(value->mutex); value->suppression_interval = real(s, 0);
    });
    raw(core_graphics, "_CGEventSourceGetUserData", [with_source](CpuState& s, GuestMemory&) {
        const auto value = with_source(s.x[0]); std::lock_guard lock(value->mutex); s.x[0] = static_cast<std::uint64_t>(value->user_data);
    });
    raw(core_graphics, "_CGEventSourceSetUserData", [with_source](CpuState& s, GuestMemory&) {
        const auto value = with_source(s.x[0]); std::lock_guard lock(value->mutex); value->user_data = static_cast<std::int64_t>(s.x[1]);
    });
    raw(core_graphics, "_CGEventSourceGetTypeID", [this](CpuState& s, GuestMemory&) {
        const auto probe = objc.allocate(source_class);
        static_cast<void>(objc.host_state<CgEventSource>(probe));
        s.x[0] = call("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "_CFGetTypeID", {probe});
    });
    // The pointer buttons and modifier keys held now (the window server's view).
    raw(core_graphics, "_CGEventSourceButtonState", [this](CpuState& s, GuestMemory&) {
        const auto ns_event = objc.find_class("NSEvent");
        const auto pressed = ns_event ? objc.send(ns_event, "pressedMouseButtons").x0 : 0;
        s.x[0] = (pressed >> (static_cast<std::uint32_t>(s.x[1]) & 63)) & 1;
    });
    raw(core_graphics, "_CGEventSourceFlagsState", [this](CpuState& s, GuestMemory&) {
        const auto ns_event = objc.find_class("NSEvent");
        s.x[0] = ns_event ? objc.send(ns_event, "modifierFlags").x0 : 0;
    });
    // CGEventCreateData / CGEventCreateFromData: Alchemy's own serialization
    // (alchemy/gui/cg_event_record.hpp; AppKit's NSEvent bridging uses it).
    raw(core_graphics, "_CGEventCreateData", [this](CpuState& s, GuestMemory&) {
        const auto value = event(s.x[1]);
        cg_event::Record record;
        {
            std::lock_guard lock(value->mutex);
            record = {value->type, value->x, value->y, value->flags, value->timestamp, value->fields};
        }
        s.x[0] = bridge->data(cg_event::encode(record));
    });
    raw(core_graphics, "_CGEventCreateFromData", [this](CpuState& s, GuestMemory&) {
        if (!s.x[1] || !bridge->is_data(s.x[1])) { s.x[0] = 0; return; }
        const auto bytes = bridge->bytes(s.x[1]);
        const auto record = cg_event::decode(bytes);
        if (!record) { s.x[0] = 0; return; }
        auto value = std::make_shared<CgEvent>();
        value->type = record->type;
        value->x = record->x;
        value->y = record->y;
        value->flags = record->flags;
        value->timestamp = record->timestamp;
        value->fields = record->fields;
        s.x[0] = make_event(value);
    });
    // double CGEventSourceSecondsSinceLastEventType(stateID, eventType): the
    // host's last-input time (all input types share it).
    raw(core_graphics, "_CGEventSourceSecondsSinceLastEventType", [](CpuState& s, GuestMemory&) {
        double seconds = 0;
        if (const auto idle = alchemy::host::ui::idle_ms()) seconds = static_cast<double>(*idle) / 1000.0;
        return_real(s, seconds);
    });
    unsupported(core_graphics, "_CGEventSourceCounterForEventType", "Alchemy does not count input events per type");
    // CGEventTapCreate: listen-only taps are the support kit taps (never fed);
    // filtering taps need accessibility access and are refused (NULL).
    raw(core_graphics, "_CGEventTapCreate", [this](CpuState& s, GuestMemory&) {
        if (s.x[2] != 1) { s.x[0] = 0; return; }
        s.x[0] = call(core_graphics, "_CGEventTapCreateForPid",
            {call(libsystem, "_getpid", {}), s.x[1], s.x[2], s.x[3], s.x[4], s.x[5]});
    });
    // Displays.
    raw(core_graphics, "_CGDisplayCopyColorSpace", [this](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] ? call(core_graphics, "_CGColorSpaceCreateDeviceRGB", {}) : 0;
    });
    constant_real(core_graphics, "_CGDisplayRotation", 0.0);
    constant(core_graphics, "_CGDisplayUsesForceToGray", 0);
    constant(core_graphics, "_CGFontRenderingGetFontSmoothingDisabled", 0);
    // Screen capture is not available.
    constant(core_graphics, "_CGPreflightScreenCaptureAccess", 0);
    constant(core_graphics, "_CGRequestScreenCaptureAccess", 0);
    constant(core_graphics, "_CGDisplayStreamCreate", 0);
    raw(core_graphics, "_CGDisplayStreamGetRunLoopSource", [](CpuState& s, GuestMemory&) { impossible(s.x[0], "CGDisplayStreamGetRunLoopSource"); s.x[0] = 0; });
    for (const auto* name : {"_CGDisplayStreamStart", "_CGDisplayStreamStop"})
        raw(core_graphics, name, [](CpuState& s, GuestMemory&) { s.x[0] = status(cg_illegal_argument); });
    raw(core_graphics, "_CGDisplayStreamUpdateGetRects", [](CpuState& s, GuestMemory& m) {
        if (s.x[2]) m.write64(s.x[2], 0);
        s.x[0] = 0;
    });
    for (const auto* name : {"_CGWindowListCopyWindowInfo", "_CGWindowListCreateDescriptionFromArray", "_CGWindowListCreateImage",
                             "_CGWindowListCreateImageFromArray"})
        constant(core_graphics, name, 0);
    // CGWindowLevel CGWindowLevelForKey(CGWindowLevelKey): CGWindowLevel.h's
    // levels, which AppKit's NS*WindowLevel values equal (normal 0, floating 3,
    // modal panel 8, main menu 24, status 25, pop-up menu 101, screen saver
    // 1000). The maximum and cursor levels are unverified (INT32_MAX - 16 and
    // one less, as commonly reported).
    raw(core_graphics, "_CGWindowLevelForKey", [](CpuState& s, GuestMemory&) {
        constexpr std::int32_t base = std::numeric_limits<std::int32_t>::min(), minimum = base + 1, desktop = minimum + 20,
                               maximum = std::numeric_limits<std::int32_t>::max() - 16;
        constexpr std::array<std::int32_t, 21> levels{
            base, minimum, desktop, -20 /* backstop menu */, 0 /* normal */, 3 /* floating */, 3 /* torn-off menu */,
            20 /* dock */, 24 /* main menu */, 25 /* status */, 8 /* modal panel */, 101 /* pop-up menu */, 500 /* dragging */,
            1000 /* screen saver */, maximum, 102 /* overlay */, 200 /* help */, 19 /* utility */, desktop + 20 /* desktop icon */,
            maximum - 1 /* cursor */, 1500 /* assistive tech high */};
        const auto key = static_cast<std::uint32_t>(s.x[0]);
        s.x[0] = static_cast<std::uint32_t>(key < levels.size() ? levels[key] : 0);
    });
    // Private window-server connection calls.
    constant(core_graphics, "_CGSMainConnectionID", 0);
    constant(core_graphics, "__CGSDefaultConnection", 0);
    for (const auto* name : {"_CGSSetWindowAlpha", "_CGSSetWindowTransform", "_CGSSetWindowWarp", "_CGSSetDenyWindowServerConnections"})
        failure(core_graphics, name, cg_invalid_connection);
    // PDF: no PDF parser or writer.
    constant(core_graphics, "_CGPDFDocumentCreateWithProvider", 0);
    raw(core_graphics, "_CGPDFDocumentGetNumberOfPages", [](CpuState& s, GuestMemory&) { impossible(s.x[0], "CGPDFDocumentGetNumberOfPages"); s.x[0] = 0; });
    raw(core_graphics, "_CGPDFDocumentGetPage", [](CpuState& s, GuestMemory&) { impossible(s.x[0], "CGPDFDocumentGetPage"); s.x[0] = 0; });
    raw(core_graphics, "_CGPDFPageGetBoxRect", [](CpuState& s, GuestMemory&) {
        impossible(s.x[0], "CGPDFPageGetBoxRect");
        const auto infinity = std::numeric_limits<double>::infinity();
        return_real(s, infinity, 0); return_real(s, infinity, 1); return_real(s, 0, 2); return_real(s, 0, 3);   // CGRectNull
    });
    raw(core_graphics, "_CGPDFPageGetRotationAngle", [](CpuState& s, GuestMemory&) { impossible(s.x[0], "CGPDFPageGetRotationAngle"); s.x[0] = 0; });
    raw(core_graphics, "_CGContextDrawPDFPage", [](CpuState& s, GuestMemory&) { impossible(s.x[1], "CGContextDrawPDFPage"); });
    constant(core_graphics, "_CGPDFContextCreate", 0);
    raw(core_graphics, "_CGPDFContextClose", [](CpuState& s, GuestMemory&) { impossible(s.x[0], "CGPDFContextClose"); });
    // Keys.
    for (const auto* name : {"kCGDisplayStreamColorSpace", "kCGDisplayStreamDestinationRect", "kCGDisplayStreamMinimumFrameTime",
                             "kCGDisplayStreamPreserveAspectRatio", "kCGDisplayStreamShowCursor", "kCGWindowBounds",
                             "kCGWindowIsOnscreen", "kCGWindowLayer", "kCGWindowName", "kCGWindowNumber", "kCGWindowOwnerName",
                             "kCGWindowOwnerPID", "kCGColorSpaceITUR_2100_HLG", "kCGColorSpaceITUR_2100_PQ"})
        string(core_graphics, "_" + std::string(name), name);
}

void GraphicsGaps::install_quartz_core() {
    // CATransform3D: 16 doubles (m11 ... m44).
    std::array<double, 16> identity{};
    identity[0] = identity[5] = identity[10] = identity[15] = 1.0;
    std::vector<std::uint8_t> bytes(128);
    std::memcpy(bytes.data(), identity.data(), 128);
    data(quartz_core, "_CATransform3DIdentity", bytes, 8);
    string(quartz_core, "_kCATransactionDisableActions", "disableActions");
    // Remote layer hosting (CAContext, CALayerHost) is not available; EDR
    // metadata and OpenGL / transform layers are class shells. CAContext
    // declares no properties, so class_getProperty(CAContext, "contextId")
    // is NULL and Chrome's RemoteLayerAPISupported probe falls back to
    // IOSurface presentation *(design)*.
    const auto context_class = cls(quartz_core, "CAContext", "NSObject");
    for (const auto* selector : {"contextWithCGSConnection:options:", "remoteContextWithOptions:", "localContext"})
        method(context_class, selector, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, true);
    static_cast<void>(cls(quartz_core, "CAEDRMetadata", "NSObject"));
    static_cast<void>(cls(quartz_core, "CALayerHost", "CALayer"));
    static_cast<void>(cls(quartz_core, "CAOpenGLLayer", "CALayer"));
    static_cast<void>(cls(quartz_core, "CATransformLayer", "CALayer"));
}

void GraphicsGaps::install_core_text() {
    for (const auto& [symbol, value] : std::array<std::pair<const char*, const char*>, 10>{{
             {"_kCTFontCollectionRemoveDuplicatesOption", "RemoveDuplicates"},
             {"_kCTFontFeatureTypeIdentifierKey", "CTFeatureTypeIdentifier"},
             {"_kCTFontFeatureSelectorIdentifierKey", "CTFeatureSelectorIdentifier"},
             {"_kCTFontFormatAttribute", "NSCTFontFormatAttribute"},
             {"_kCTFontVariationAttribute", "NSCTFontVariationAttribute"},
             {"_kCTFontVariationAxisDefaultValueKey", "NSCTVariationAxisDefaultValue"},
             {"_kCTFontVariationAxisIdentifierKey", "NSCTVariationAxisIdentifier"},
             {"_kCTFontVariationAxisMaximumValueKey", "NSCTVariationAxisMaximumValue"},
             {"_kCTFontVariationAxisMinimumValueKey", "NSCTVariationAxisMinimumValue"},
             {"_kCTFontVariationAxisNameKey", "NSCTVariationAxisName"}}})
        string(core_text, symbol, value);
    // kCTVersionNumber14_0: the reported system is macOS 14.
    constant(core_text, "_CTGetCoreTextVersion", 0x00100000);
    // Font collections: every available family.
    collection_class = objc.find_class("__AlchemyCTFontCollection");
    if (!collection_class) collection_class = objc.register_native_class("__AlchemyCTFontCollection", objc.find_class("NSObject"), 8);
    if (!has(core_text, "_CTFontCollectionCreateFromAvailableFonts")) {
        static_cast<void>(objc.add_native_method(collection_class, "dealloc", [self = shared_from_this(), this](CpuState& s, GuestMemory&) {
            if (const auto value = objc.find_host_state<FontCollection>(s.x[0])) release(std::exchange(value->descriptors, 0));
            static_cast<void>(objc.send_super(s.x[0], collection_class, objc.selector("dealloc")));
        }, false, "v@:"));
    }
    raw(core_text, "_CTFontCollectionCreateFromAvailableFonts", [this](CpuState& s, GuestMemory&) {
        const auto families = call(core_text, "_CTFontManagerCopyAvailableFontFamilyNames", {});
        const auto key = bridge->constant(core_text, "_kCTFontFamilyNameAttribute");
        std::vector<Address> descriptors;
        for (const auto family : families ? cf.array_values(families) : std::vector<Address>{}) {
            const auto attributes = cf.dictionary(std::vector<std::pair<Address, Address>>{{key, family}});
            const auto descriptor = call(core_text, "_CTFontDescriptorCreateWithAttributes", {attributes});
            release(attributes);
            if (descriptor) descriptors.push_back(descriptor);
        }
        release(families);
        auto value = std::make_shared<FontCollection>();
        value->descriptors = cf.array(descriptors);
        for (const auto descriptor : descriptors) release(descriptor);
        const auto object = objc.allocate(collection_class);
        static_cast<void>(objc.host_state<FontCollection>(object, [&] { return value; }));
        s.x[0] = object;
    });
    raw(core_text, "_CTFontCollectionCreateMatchingFontDescriptors", [this](CpuState& s, GuestMemory&) {
        const auto value = s.x[0] ? objc.find_host_state<FontCollection>(s.x[0]) : nullptr;
        if (!value) throw std::runtime_error("Alchemy: CTFontCollectionCreateMatchingFontDescriptors needs a CTFontCollection");
        s.x[0] = cf.copy(value->descriptors);
    });
    // Collections of given descriptors, and copies with more.
    const auto make_collection = [this](std::vector<Address> descriptors) {
        auto value = std::make_shared<FontCollection>();
        value->descriptors = cf.array(descriptors);
        const auto object = objc.allocate(collection_class);
        static_cast<void>(objc.host_state<FontCollection>(object, [&] { return value; }));
        return object;
    };
    const auto collection_of = [this](Address object, const char* api) {
        const auto value = object ? objc.find_host_state<FontCollection>(object) : nullptr;
        if (!value) throw std::runtime_error(std::string("Alchemy: ") + api + " needs a CTFontCollection");
        return value;
    };
    raw(core_text, "_CTFontCollectionCreateWithFontDescriptors", [this, make_collection](CpuState& s, GuestMemory&) {
        // (descriptors, options)
        s.x[0] = make_collection(s.x[0] ? cf.array_values(s.x[0]) : std::vector<Address>{});
    });
    raw(core_text, "_CTFontCollectionCreateCopyWithFontDescriptors", [this, make_collection, collection_of](CpuState& s, GuestMemory&) {
        // (original, descriptors, options)
        auto descriptors = cf.array_values(collection_of(s.x[0], "CTFontCollectionCreateCopyWithFontDescriptors")->descriptors);
        for (const auto descriptor : s.x[1] ? cf.array_values(s.x[1]) : std::vector<Address>{}) descriptors.push_back(descriptor);
        s.x[0] = make_collection(std::move(descriptors));
    });
    // (collection, sortCallback(first, second, refCon) -> CFComparisonResult, refCon)
    raw(core_text, "_CTFontCollectionCreateMatchingFontDescriptorsSortedWithCallback", [this, collection_of](CpuState& s, GuestMemory&) {
        auto descriptors = cf.array_values(collection_of(s.x[0], "CTFontCollectionCreateMatchingFontDescriptorsSortedWithCallback")->descriptors);
        if (const auto callback = s.x[1]) {
            const auto context = s.x[2];
            std::stable_sort(descriptors.begin(), descriptors.end(), [this, callback, context](Address a, Address b) {
                CallRegs registers{};
                registers.x[0] = a;
                registers.x[1] = b;
                registers.x[2] = context;
                return static_cast<std::int64_t>(EngineAdapter(process.calling_engine()).call_regs(callback, registers).x0) < 0;
            });
        }
        s.x[0] = descriptors.empty() ? 0 : cf.array(descriptors);
    });
    // CFGetTypeID of a collection.
    raw(core_text, "_CTFontCollectionGetTypeID", [this, make_collection](CpuState& s, GuestMemory&) {
        const auto sample = make_collection({});
        s.x[0] = call(core_foundation, "_CFGetTypeID", {sample});
        release(sample);
    });
    // CTFontCopyAvailableTables(font, options): the tags CTFontCopyTable finds
    // (an array of CTFontTableTag values, not CF objects).
    raw(core_text, "_CTFontCopyAvailableTables", [this](CpuState& s, GuestMemory& m) {
        static constexpr std::array<const char*, 64> tags{
            "BASE", "CBDT", "CBLC", "CFF ", "CFF2", "COLR", "CPAL", "DSIG", "EBDT", "EBLC", "EBSC", "GDEF", "GPOS", "GSUB",
            "HVAR", "JSTF", "LTSH", "MATH", "MERG", "MVAR", "OS/2", "PCLT", "STAT", "SVG ", "VDMX", "VORG", "VVAR", "ankr",
            "avar", "bdat", "bhed", "bloc", "bsln", "cmap", "cvar", "cvt ", "feat", "fpgm", "fvar", "gasp", "glyf", "gvar",
            "hdmx", "head", "hhea", "hmtx", "just", "kern", "kerx", "lcar", "loca", "ltag", "maxp", "meta", "morx", "name",
            "opbd", "post", "prep", "prop", "sbix", "trak", "vhea", "vmtx"};
        std::vector<Address> found;
        for (const auto* tag : tags) {
            const auto code = (static_cast<std::uint32_t>(static_cast<unsigned char>(tag[0])) << 24) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(tag[1])) << 16) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(tag[2])) << 8) | static_cast<unsigned char>(tag[3]);
            const auto table = call(core_text, "_CTFontCopyTable", {s.x[0], code, 0});
            if (table) { found.push_back(code); release(table); }
        }
        const auto values = process.allocate_guest(std::max<std::size_t>(found.size(), 1) * 8);
        for (std::size_t index = 0; index < found.size(); ++index) m.write64(values + index * 8, found[index]);
        s.x[0] = call(core_foundation, "_CFArrayCreate", {0, values, found.size(), 0});
        process.release_guest(values);
    });
    // CTFontCopyCharacterSet(font): the BMP characters the font maps to glyphs.
    raw(core_text, "_CTFontCopyCharacterSet", [this](CpuState& s, GuestMemory& m) {
        const auto set = call(core_foundation, "_CFCharacterSetCreateMutable", {0});
        constexpr std::uint32_t chunk = 4096;
        const auto characters = process.allocate_guest(chunk * 2), glyphs = process.allocate_guest(chunk * 2);
        std::vector<std::uint8_t> units(chunk * 2), mapped(chunk * 2);
        for (std::uint32_t base = 0; base < 0x10000; base += chunk) {
            for (std::uint32_t i = 0; i < chunk; ++i) {
                auto c = base + i;
                if (c >= 0xd800 && c < 0xe000) c = 0;   // surrogate units are not characters
                units[i * 2] = static_cast<std::uint8_t>(c);
                units[i * 2 + 1] = static_cast<std::uint8_t>(c >> 8);
            }
            m.write_bytes(characters, units);
            m.fill_bytes(glyphs, 0, chunk * 2);
            static_cast<void>(call(core_text, "_CTFontGetGlyphsForCharacters", {s.x[0], characters, glyphs, chunk}));
            m.read_bytes(glyphs, mapped);
            std::uint32_t run = 0, start = 0;
            for (std::uint32_t i = 0; i <= chunk; ++i) {
                const auto c = base + i;
                const bool present = i < chunk && !(c >= 0xd800 && c < 0xe000) && c != 0 && (mapped[i * 2] | mapped[i * 2 + 1]);
                if (present) { if (!run) start = c; ++run; }
                else if (run) { static_cast<void>(call(core_foundation, "_CFCharacterSetAddCharactersInRange", {set, start, run})); run = 0; }
            }
        }
        process.release_guest(characters);
        process.release_guest(glyphs);
        s.x[0] = set;
    });
    // Font variations are not modelled.
    constant(core_text, "_CTFontCopyVariation", 0);
    constant(core_text, "_CTFontCopyVariationAxes", 0);
    // ATS font references do not exist.
    raw(core_text, "_CTFontGetPlatformFont", [](CpuState& s, GuestMemory& m) { if (s.x[1]) m.write64(s.x[1], 0); s.x[0] = 0; });
    // System UI fonts have names starting with '.' (".SFNS-Regular").
    raw(core_text, "_CTFontDescriptorIsSystemUIFont", [this](CpuState& s, GuestMemory&) {
        if (!s.x[0]) { s.x[0] = 0; return; }
        const auto name = call(core_text, "_CTFontDescriptorCopyAttribute", {s.x[0], bridge->constant(core_text, "_kCTFontNameAttribute")});
        const bool system = name && bridge->is_string(name) && cf.utf8(name).starts_with(".");
        release(name);
        s.x[0] = system;
    });
}

void GraphicsGaps::install_misc() {
    // IOSurface (its Mach ports are QuartzCore's iosurface.cpp).
    string(iosurface, "_kIOSurfacePlaneBytesPerElement", "IOSurfacePlaneBytesPerElement");
    // CoreImage: no feature detectors are available.
    for (const auto* name : {"CIDetectorAccuracy", "CIDetectorAccuracyHigh", "CIDetectorAccuracyLow", "CIDetectorTypeFace",
                             "CIDetectorTypeQRCode", "CIDetectorTypeText"})
        string(core_image, "_" + std::string(name), name);
    const auto detector = cls(core_image, "CIDetector", "NSObject");
    method(detector, "detectorOfType:context:options:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, true);
    // Carbon.
    raw(carbon, "_EnableSecureEventInput", [this](CpuState& s, GuestMemory&) { std::lock_guard lock(mutex); ++secure_input; s.x[0] = 0; });
    raw(carbon, "_DisableSecureEventInput", [this](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        if (secure_input) --secure_input;
        s.x[0] = 0;
    });
    // PhysicalKeyboardLayoutType KBGetLayoutType(keyboardType): from the host keyboard.
    raw(carbon, "_KBGetLayoutType", [](CpuState& s, GuestMemory&) {
        std::uint32_t layout = fourcc("ANSI");
        if (const auto kind = alchemy::host::ui::keyboard_kind()) {
            if (*kind == alchemy::host::ui::KeyboardKind::jis) layout = fourcc("JIS ");
            else if (*kind == alchemy::host::ui::KeyboardKind::iso) layout = fourcc("ISO ");
        }
        s.x[0] = layout;
    });
    raw(carbon, "_LMGetKbdLast", [this](CpuState& s, GuestMemory&) { s.x[0] = call(carbon, "_LMGetKbdType", {}) & 0xff; });
    raw(carbon, "_TISCreateASCIICapableInputSourceList", [this](CpuState& s, GuestMemory&) {
        const auto source = call(carbon, "_TISCopyCurrentASCIICapableKeyboardInputSource", {});
        std::vector<Address> values;
        if (source) values.push_back(source);
        s.x[0] = cf.array(values);
        release(source);
    });
    // Text Services Manager documents are not modelled.
    failure(carbon, "_TSMSetDocumentProperty", -50);
    failure(carbon, "_TSMRemoveDocumentProperty", -50);
    // ApplicationServices (process manager / universal access).
    failure(application_services, "_GetProcessInformation", -600);   // procNotFound
    // No Dock exists, so every Alchemy process already behaves as a daemon (no Dock tile).
    constant(application_services, "_SetApplicationIsDaemon", 0);
    constant(application_services, "_UAZoomEnabled", 0);
}

void GraphicsGaps::install() {
    install_core_graphics();
    install_quartz_core();
    install_core_text();
    install_misc();
}

} // namespace

void install_graphics_gaps(InstallContext& context) { std::make_shared<GraphicsGaps>(context)->install(); }

} // namespace alchemy::gui::fill
