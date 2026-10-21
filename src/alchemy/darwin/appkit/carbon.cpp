#include "alchemy/darwin/appkit/appkit_internal.hpp"

#include <algorithm>
#include <cstring>

// The Carbon (HIToolbox, CoreServices) keyboard and event-state functions and
// the CoreGraphics cursor functions AppKit applications use, over the window
// server (the display queries are Quartz's: darwin/coregraphics/displays.cpp).
// Without a window server they keep the headless answers of
// darwin_frameworks.cpp (no input source, no display).
//
// Coordinates: CoreGraphics global display space has its origin at the
// top-left of the main display with y down, in points; the window server
// uses AppKit's bottom-left, y-up space.
namespace alchemy::gui::appkit {
namespace {
// MacErrors.h / UnicodeUtilities.h / CGError.h
constexpr std::int32_t no_err = 0, param_err = -50, output_buffer_too_small = -25340;
constexpr std::int32_t cg_success = 0, cg_illegal_argument = 1001, cg_none_available = 1011;
// UCKeyTranslate key actions and options.
constexpr std::uint64_t key_action_up = 1, key_action_display = 3, no_dead_keys = 1;
// Events.h modifier bits shifted right by 8 (UCKeyTranslate modifierKeyState).
constexpr std::uint32_t carbon_command = 1, carbon_shift = 2, carbon_caps_lock = 4, carbon_option = 8, carbon_control = 16;
// The layout data Alchemy's input sources hand out: an opaque blob naming the
// window server's layout, not an Apple 'uchr' table.
constexpr std::array<std::uint8_t, 8> layout_magic{'A', 'L', 'C', 'H', 'u', 'c', 'h', 'r'};
constexpr std::uint8_t ansi_keyboard = 40;   // gestaltUSBAndyANSIKbd

std::uint64_t status(std::int32_t value) { return static_cast<std::uint32_t>(value); }

std::vector<std::uint16_t> utf16(std::string_view text) {
    std::vector<std::uint16_t> units;
    for (std::size_t index = 0; index < text.size();) {
        const auto byte = static_cast<unsigned char>(text[index]);
        std::uint32_t scalar = byte;
        std::size_t more = 0;
        if (byte >= 0xf0) { scalar = byte & 0x07; more = 3; }
        else if (byte >= 0xe0) { scalar = byte & 0x0f; more = 2; }
        else if (byte >= 0xc0) { scalar = byte & 0x1f; more = 1; }
        if (index + more >= text.size()) break;   // truncated sequence
        for (std::size_t next = 1; next <= more; ++next) scalar = (scalar << 6) | (static_cast<unsigned char>(text[index + next]) & 0x3f);
        index += more + 1;
        if (scalar >= 0x10000) {
            scalar -= 0x10000;
            units.push_back(static_cast<std::uint16_t>(0xd800 + (scalar >> 10)));
            units.push_back(static_cast<std::uint16_t>(0xdc00 + (scalar & 0x3ff)));
        } else {
            units.push_back(static_cast<std::uint16_t>(scalar));
        }
    }
    return units;
}

std::optional<ScreenInfo> find_screen(const std::vector<ScreenInfo>& list, ScreenId id) {
    for (const auto& info : list) if (info.id == id) return info;
    return std::nullopt;
}
double primary_height(const std::vector<ScreenInfo>& list) { return list.empty() ? 0 : list.front().frame.size.height; }
} // namespace

void AppKit::install_carbon() {
    const auto carbon_string = [this](std::string_view library, std::string symbol, std::string_view value) {
        const auto object = cf.string(value);
        if (const auto record = process.objects().find(object)) record->immortal = true;
        const auto slot = process.allocate_guest(8);
        process.main_engine().write64(slot, object);
        process.install_export(std::string(library), std::move(symbol), {slot, ExportKind::data});
    };
    for (const auto* key : {"TISPropertyInputSourceID", "TISPropertyLocalizedName", "TISPropertyInputSourceLanguages",
             "TISPropertyInputSourceType", "TISPropertyInputSourceCategory", "TISPropertyInputSourceIsEnabled",
             "TISPropertyInputSourceIsSelected", "TISPropertyInputSourceIsSelectCapable", "TISTypeKeyboardLayout",
             "TISTypeKeyboardInputMode", "TISCategoryKeyboardInputSource"})
        carbon_string(carbon_library, "_k" + std::string(key), key);
    // The distributed notification sent when the keyboard input source changes
    // (never posted: the window server's layout is fixed for the process).
    // The value is unverified.
    carbon_string(carbon_library, "_kTISNotifySelectedKeyboardInputSourceChanged", "AppleSelectedInputSourcesChangedNotification");

    // ---- Text Input Sources ----
    const auto copy_source = [this](CpuState& s, GuestMemory&) {
        const auto server = window_server_or_null();
        if (!server) { s.x[0] = 0; return; }   // no input source without a window server
        auto payload = std::make_shared<InputSourceState>();
        payload->layout = server->keyboard_layout();
        // CFRelease of the last reference releases the cached properties.
        s.x[0] = process.objects().create(16, input_source, ObjectKind::instance, payload, [this, payload](Address) {
            for (auto* slot : {&payload->layout_data, &payload->identifier, &payload->name, &payload->languages})
                if (const auto value = std::exchange(*slot, 0)) process.objects().release(value);
        });
    };
    for (const auto* name : {"_TISCopyCurrentKeyboardInputSource", "_TISCopyCurrentKeyboardLayoutInputSource",
             "_TISCopyCurrentASCIICapableKeyboardLayoutInputSource", "_TISCopyCurrentASCIICapableKeyboardInputSource"})
        function(carbon_library, name, copy_source);
    function(carbon_library, "_TISCopyInputMethodKeyboardLayoutOverride", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    function(carbon_library, "_TISGetInputSourceProperty", [this](CpuState& s, GuestMemory& m) {
        // "Get" rule: the values belong to the input source.
        const auto source = s.x[0], key = s.x[1];
        s.x[0] = 0;
        if (!source || !key) return;
        const auto record = process.objects().require(source);
        const auto state = std::dynamic_pointer_cast<InputSourceState>(record->payload);
        if (!state) fail("TISGetInputSourceProperty: object is not a TISInputSource");
        const auto& layout = state->layout;
        const bool input_method = layout.identifier.starts_with("com.apple.inputmethod.");
        const auto name = cf.utf8(key);
        const auto cached = [&](Address& slot, auto make) { if (!slot) slot = make(); return slot; };
        if (name == "TISPropertyUnicodeKeyLayoutData") {
            s.x[0] = cached(state->layout_data, [&] {
                std::vector<std::uint8_t> bytes(layout_magic.begin(), layout_magic.end());
                bytes.insert(bytes.end(), layout.identifier.begin(), layout.identifier.end());
                const auto buffer = process.allocate_guest(bytes.size());
                m.write_bytes(buffer, bytes);
                const auto data = cf.invoke("CFDataCreate", std::array<Address, 3>{0, buffer, bytes.size()});
                process.release_guest(buffer);
                return data;
            });
        } else if (name == "TISPropertyInputSourceIsASCIICapable") {
            s.x[0] = cf.boolean(!input_method);
        } else if (name == "TISPropertyInputSourceID") {
            s.x[0] = cached(state->identifier, [&] { return cf.string(layout.identifier); });
        } else if (name == "TISPropertyLocalizedName") {
            s.x[0] = cached(state->name, [&] { return cf.string(layout.localized_name); });
        } else if (name == "TISPropertyInputSourceLanguages") {
            s.x[0] = cached(state->languages, [&] {
                const auto language = cf.string(layout.language);
                const auto array = cf.array(std::array<Address, 1>{language});
                process.objects().release(language);
                return array;
            });
        } else if (name == "TISPropertyInputSourceType") {
            s.x[0] = m.read64(process.resolve(carbon_library, input_method ? "_kTISTypeKeyboardInputMode" : "_kTISTypeKeyboardLayout")->address);
        } else if (name == "TISPropertyInputSourceCategory") {
            s.x[0] = m.read64(process.resolve(carbon_library, "_kTISCategoryKeyboardInputSource")->address);
        } else if (name == "TISPropertyInputSourceIsEnabled" || name == "TISPropertyInputSourceIsSelected" ||
                   name == "TISPropertyInputSourceIsSelectCapable") {
            s.x[0] = cf.boolean(true);
        }
    });

    // UCKeyTranslate(layout, keyCode, keyAction, modifierKeyState, keyboardType,
    //   options, deadKeyState*, maxStringLength, actualStringLength*, unicodeString*)
    function(core_services_library, "_UCKeyTranslate", [](CpuState& s, GuestMemory& m) {
        const auto layout = s.x[0];
        const auto key_code = static_cast<std::uint16_t>(s.x[1]);
        const auto action = s.x[2] & 0xffff;
        const auto carbon = static_cast<std::uint32_t>(s.x[3]);
        const auto options = s.x[5];
        const auto dead_pointer = s.x[6], maximum = s.x[7];
        const auto actual_pointer = m.read64(s.sp), output = m.read64(s.sp + 8);
        if (actual_pointer) m.write64(actual_pointer, 0);
        const auto server = window_server_or_null();
        std::array<std::uint8_t, 8> magic{};
        if (layout) m.read_bytes(layout, magic);
        if (!layout || !dead_pointer || !server || magic != layout_magic) { s.x[0] = status(param_err); return; }
        if (action == key_action_up) { s.x[0] = status(no_err); return; }   // key-up events produce no text
        std::uint64_t flags{};
        if (carbon & carbon_command) flags |= modifier::command;
        if (carbon & carbon_shift) flags |= modifier::shift;
        if (carbon & carbon_caps_lock) flags |= modifier::caps_lock;
        if (carbon & carbon_option) flags |= modifier::option;
        if (carbon & carbon_control) flags |= modifier::control;
        const bool keep_dead = !(options & no_dead_keys) && action != key_action_display;
        DeadKeyState dead{keep_dead ? m.read32(dead_pointer) : 0};
        const auto units = utf16(server->translate_key(key_code, flags, dead));
        if (keep_dead) m.write32(dead_pointer, dead.value);
        const auto count = std::min<std::uint64_t>(units.size(), maximum);
        for (std::uint64_t index = 0; index < count; ++index) {
            const std::array<std::uint8_t, 2> unit{static_cast<std::uint8_t>(units[index]), static_cast<std::uint8_t>(units[index] >> 8)};
            m.write_bytes(output + index * 2, unit);
        }
        if (actual_pointer) m.write64(actual_pointer, count);
        s.x[0] = status(count < units.size() ? output_buffer_too_small : no_err);
    });
    function(carbon_library, "_LMGetKbdType", [](CpuState& s, GuestMemory&) { s.x[0] = window_server_or_null() ? ansi_keyboard : 0; });
    function(carbon_library, "_GetCurrentEventButtonState", [](CpuState& s, GuestMemory&) {
        const auto server = window_server_or_null();
        s.x[0] = server ? static_cast<std::uint32_t>(server->pressed_mouse_buttons()) : 0;
    });
    for (const auto library : {appkit_library, cocoa_library})
        function(library, "_NSBeep", [](CpuState&, GuestMemory&) { if (const auto server = window_server_or_null()) server->beep(); });

    // ---- CoreGraphics cursor (the display queries are Quartz's: darwin/coregraphics/displays.cpp) ----
    function(core_graphics_library, "_CGDisplayMoveCursorToPoint", [](CpuState& s, GuestMemory&) {
        // The point is relative to the top-left corner of the display.
        const auto server = window_server_or_null();
        const auto screens = server ? server->screens() : std::vector<ScreenInfo>{};
        const auto info = find_screen(screens, static_cast<ScreenId>(s.x[0]));
        if (!info) { s.x[0] = status(cg_illegal_argument); return; }
        const auto point = point_arg(s);
        const auto& frame = info->frame;
        server->warp_cursor({frame.origin.x + point.x, frame.origin.y + frame.size.height - point.y});
        s.x[0] = status(cg_success);
    });
    function(core_graphics_library, "_CGWarpMouseCursorPosition", [](CpuState& s, GuestMemory&) {
        const auto server = window_server_or_null();
        if (!server) { s.x[0] = status(cg_none_available); return; }
        const auto point = point_arg(s);
        server->warp_cursor({point.x, primary_height(server->screens()) - point.y});
        s.x[0] = status(cg_success);
    });
    function(core_graphics_library, "_CGAssociateMouseAndMouseCursorPosition", [](CpuState& s, GuestMemory&) {
        const auto server = window_server_or_null();
        if (!server) { s.x[0] = status(cg_none_available); return; }
        server->associate_mouse_and_cursor((s.x[0] & 0xff) != 0);
        s.x[0] = status(cg_success);
    });
    for (const bool show : {false, true})
        function(core_graphics_library, show ? "_CGDisplayShowCursor" : "_CGDisplayHideCursor", [show](CpuState& s, GuestMemory&) {
            const auto server = window_server_or_null();
            if (!server) { s.x[0] = status(cg_none_available); return; }
            server->set_cursor_visible(show);
            s.x[0] = status(cg_success);
        });
}

} // namespace alchemy::gui::appkit
