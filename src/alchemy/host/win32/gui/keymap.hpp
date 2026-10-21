#pragma once

// Windows keyboard -> macOS key codes and modifier flags (pure tables, no
// Win32 calls). Mac key codes are the positional kVK_* values of the public
// Carbon HIToolbox Events.h; Windows keys are identified by their set-1 scan
// code (0xE0xx for extended keys) so that, as on macOS, the code names the
// physical key position and not the character the active layout puts there.

#include <cstdint>
#include <optional>

namespace alchemy::gui::win32 {

inline constexpr std::uint16_t no_key_code = 0xffff;

namespace kvk {
inline constexpr std::uint16_t ansi_a = 0x00, ansi_s = 0x01, ansi_d = 0x02, ansi_f = 0x03, ansi_h = 0x04,
    ansi_g = 0x05, ansi_z = 0x06, ansi_x = 0x07, ansi_c = 0x08, ansi_v = 0x09, iso_section = 0x0a,
    ansi_b = 0x0b, ansi_q = 0x0c, ansi_w = 0x0d, ansi_e = 0x0e, ansi_r = 0x0f, ansi_y = 0x10, ansi_t = 0x11,
    ansi_1 = 0x12, ansi_2 = 0x13, ansi_3 = 0x14, ansi_4 = 0x15, ansi_6 = 0x16, ansi_5 = 0x17,
    ansi_equal = 0x18, ansi_9 = 0x19, ansi_7 = 0x1a, ansi_minus = 0x1b, ansi_8 = 0x1c, ansi_0 = 0x1d,
    ansi_right_bracket = 0x1e, ansi_o = 0x1f, ansi_u = 0x20, ansi_left_bracket = 0x21, ansi_i = 0x22,
    ansi_p = 0x23, return_key = 0x24, ansi_l = 0x25, ansi_j = 0x26, ansi_quote = 0x27, ansi_k = 0x28,
    ansi_semicolon = 0x29, ansi_backslash = 0x2a, ansi_comma = 0x2b, ansi_slash = 0x2c, ansi_n = 0x2d,
    ansi_m = 0x2e, ansi_period = 0x2f, tab = 0x30, space = 0x31, ansi_grave = 0x32, delete_key = 0x33,
    escape = 0x35, right_command = 0x36, command = 0x37, shift = 0x38, caps_lock = 0x39, option = 0x3a,
    control = 0x3b, right_shift = 0x3c, right_option = 0x3d, right_control = 0x3e, function = 0x3f,
    f17 = 0x40, keypad_decimal = 0x41, keypad_multiply = 0x43, keypad_plus = 0x45, keypad_clear = 0x47,
    volume_up = 0x48, volume_down = 0x49, mute = 0x4a, keypad_divide = 0x4b, keypad_enter = 0x4c,
    keypad_minus = 0x4e, f18 = 0x4f, f19 = 0x50, keypad_equals = 0x51, keypad_0 = 0x52, keypad_1 = 0x53,
    keypad_2 = 0x54, keypad_3 = 0x55, keypad_4 = 0x56, keypad_5 = 0x57, keypad_6 = 0x58, keypad_7 = 0x59,
    f20 = 0x5a, keypad_8 = 0x5b, keypad_9 = 0x5c, jis_yen = 0x5d, jis_underscore = 0x5e,
    jis_keypad_comma = 0x5f, f5 = 0x60, f6 = 0x61, f7 = 0x62, f3 = 0x63, f8 = 0x64, f9 = 0x65,
    jis_eisu = 0x66, f11 = 0x67, jis_kana = 0x68, f13 = 0x69, f16 = 0x6a, f14 = 0x6b, f10 = 0x6d,
    contextual_menu = 0x6e, f12 = 0x6f, f15 = 0x71, help = 0x72, home = 0x73, page_up = 0x74,
    forward_delete = 0x75, f4 = 0x76, end = 0x77, f2 = 0x78, page_down = 0x79, f1 = 0x7a,
    left_arrow = 0x7b, right_arrow = 0x7c, down_arrow = 0x7d, up_arrow = 0x7e;
} // namespace kvk

// Device-dependent modifier bits of NSEventModifierFlags (the NX_DEVICE*
// masks of IOKit's IOLLEvent.h). AppKit reports them in the low bits of
// modifierFlags next to the device-independent modifier:: bits.
namespace device_modifier {
inline constexpr std::uint64_t left_control = 0x0001, left_shift = 0x0002, right_shift = 0x0004,
    left_command = 0x0008, right_command = 0x0010, left_option = 0x0020, right_option = 0x0040,
    right_control = 0x2000;
} // namespace device_modifier

// Mac key code of a Windows key. `scan_code` is the 8-bit set-1 scan code
// (lParam bits 16-23), `extended` lParam bit 24. `virtual_key` resolves keys
// whose scan code is ambiguous or absent (Pause vs NumLock, synthesized input
// with scan code 0, media keys). Modifier keys use the default mapping
// (Ctrl -> Control, Alt -> Option, Windows -> Command); see
// apply_modifier_mapping. Returns no_key_code for keys macOS has no code for.
[[nodiscard]] std::uint16_t mac_key_code(std::uint32_t virtual_key, std::uint32_t scan_code, bool extended) noexcept;
// The scan code (0xE0xx for extended keys) of a mac key code, or 0.
[[nodiscard]] std::uint32_t scan_code_for_mac_key(std::uint16_t key_code) noexcept;

// Ctrl <-> Command swap (ALCHEMY_GUI_SWAP_CONTROL_COMMAND=1): the physical
// Ctrl keys become Command and the Windows keys become Control.
[[nodiscard]] std::uint16_t apply_modifier_mapping(std::uint16_t key_code, bool swap_control_command) noexcept;

[[nodiscard]] bool is_modifier_key(std::uint16_t key_code) noexcept;
// modifier:: | device_modifier:: bit a held modifier key contributes (caps
// lock contributes nothing: its flag is the toggle state).
[[nodiscard]] std::uint64_t modifier_bits(std::uint16_t key_code) noexcept;

// Which physical modifier keys are down (Windows view, before mapping).
struct PhysicalModifiers {
    bool left_shift{}, right_shift{}, left_control{}, right_control{}, left_alt{}, right_alt{}, left_windows{},
        right_windows{}, caps_lock_on{};
};
// NSEventModifierFlags for held modifiers (device-independent and
// device-dependent bits).
[[nodiscard]] std::uint64_t modifier_flags(const PhysicalModifiers& keys, bool swap_control_command) noexcept;

// NSEventModifierFlagFunction / NumericPad bits AppKit adds to key events of
// this key (arrows, function and navigation keys, keypad).
[[nodiscard]] std::uint64_t key_event_flags(std::uint16_t key_code) noexcept;

// NSEvent characters of keys AppKit reports with fixed characters (arrows
// and function keys use the NSF*FunctionKey private-use code points, Return
// "\r", Delete 0x7f, ...), independent of the layout. nullopt: use the
// layout's character.
[[nodiscard]] std::optional<char32_t> event_special_character(std::uint16_t key_code, bool shift) noexcept;
// The same for UCKeyTranslate with a standard 'uchr' layout, which reports
// these keys with ASCII control characters (arrows 0x1c-0x1f, function keys
// 0x10, ...).
[[nodiscard]] std::optional<char32_t> translate_special_character(std::uint16_t key_code) noexcept;

} // namespace alchemy::gui::win32
