#include "alchemy/host/win32/gui/keymap.hpp"

#include "alchemy/gui/window_server.hpp"

#include <array>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace alchemy::gui::win32 {
namespace {

struct ScanEntry {
    std::uint32_t scan;   // 0xE0xx for extended keys
    std::uint16_t key;
};

// Positional mapping between the PC/AT set-1 scan codes Windows reports and
// the macOS ANSI/ISO/JIS key positions (the same correspondence USB HID usage
// tables give both platforms). PrintScreen, ScrollLock and Pause sit where
// Apple extended keyboards have F13-F15; NumLock where they have Clear;
// Insert where they have Help.
constexpr std::array scan_table{
    ScanEntry{0x01, kvk::escape}, ScanEntry{0x02, kvk::ansi_1}, ScanEntry{0x03, kvk::ansi_2},
    ScanEntry{0x04, kvk::ansi_3}, ScanEntry{0x05, kvk::ansi_4}, ScanEntry{0x06, kvk::ansi_5},
    ScanEntry{0x07, kvk::ansi_6}, ScanEntry{0x08, kvk::ansi_7}, ScanEntry{0x09, kvk::ansi_8},
    ScanEntry{0x0a, kvk::ansi_9}, ScanEntry{0x0b, kvk::ansi_0}, ScanEntry{0x0c, kvk::ansi_minus},
    ScanEntry{0x0d, kvk::ansi_equal}, ScanEntry{0x0e, kvk::delete_key}, ScanEntry{0x0f, kvk::tab},
    ScanEntry{0x10, kvk::ansi_q}, ScanEntry{0x11, kvk::ansi_w}, ScanEntry{0x12, kvk::ansi_e},
    ScanEntry{0x13, kvk::ansi_r}, ScanEntry{0x14, kvk::ansi_t}, ScanEntry{0x15, kvk::ansi_y},
    ScanEntry{0x16, kvk::ansi_u}, ScanEntry{0x17, kvk::ansi_i}, ScanEntry{0x18, kvk::ansi_o},
    ScanEntry{0x19, kvk::ansi_p}, ScanEntry{0x1a, kvk::ansi_left_bracket},
    ScanEntry{0x1b, kvk::ansi_right_bracket}, ScanEntry{0x1c, kvk::return_key},
    ScanEntry{0x1d, kvk::control}, ScanEntry{0x1e, kvk::ansi_a}, ScanEntry{0x1f, kvk::ansi_s},
    ScanEntry{0x20, kvk::ansi_d}, ScanEntry{0x21, kvk::ansi_f}, ScanEntry{0x22, kvk::ansi_g},
    ScanEntry{0x23, kvk::ansi_h}, ScanEntry{0x24, kvk::ansi_j}, ScanEntry{0x25, kvk::ansi_k},
    ScanEntry{0x26, kvk::ansi_l}, ScanEntry{0x27, kvk::ansi_semicolon}, ScanEntry{0x28, kvk::ansi_quote},
    ScanEntry{0x29, kvk::ansi_grave}, ScanEntry{0x2a, kvk::shift}, ScanEntry{0x2b, kvk::ansi_backslash},
    ScanEntry{0x2c, kvk::ansi_z}, ScanEntry{0x2d, kvk::ansi_x}, ScanEntry{0x2e, kvk::ansi_c},
    ScanEntry{0x2f, kvk::ansi_v}, ScanEntry{0x30, kvk::ansi_b}, ScanEntry{0x31, kvk::ansi_n},
    ScanEntry{0x32, kvk::ansi_m}, ScanEntry{0x33, kvk::ansi_comma}, ScanEntry{0x34, kvk::ansi_period},
    ScanEntry{0x35, kvk::ansi_slash}, ScanEntry{0x36, kvk::right_shift}, ScanEntry{0x37, kvk::keypad_multiply},
    ScanEntry{0x38, kvk::option}, ScanEntry{0x39, kvk::space}, ScanEntry{0x3a, kvk::caps_lock},
    ScanEntry{0x3b, kvk::f1}, ScanEntry{0x3c, kvk::f2}, ScanEntry{0x3d, kvk::f3}, ScanEntry{0x3e, kvk::f4},
    ScanEntry{0x3f, kvk::f5}, ScanEntry{0x40, kvk::f6}, ScanEntry{0x41, kvk::f7}, ScanEntry{0x42, kvk::f8},
    ScanEntry{0x43, kvk::f9}, ScanEntry{0x44, kvk::f10}, ScanEntry{0x46, kvk::f14},
    ScanEntry{0x47, kvk::keypad_7}, ScanEntry{0x48, kvk::keypad_8}, ScanEntry{0x49, kvk::keypad_9},
    ScanEntry{0x4a, kvk::keypad_minus}, ScanEntry{0x4b, kvk::keypad_4}, ScanEntry{0x4c, kvk::keypad_5},
    ScanEntry{0x4d, kvk::keypad_6}, ScanEntry{0x4e, kvk::keypad_plus}, ScanEntry{0x4f, kvk::keypad_1},
    ScanEntry{0x50, kvk::keypad_2}, ScanEntry{0x51, kvk::keypad_3}, ScanEntry{0x52, kvk::keypad_0},
    ScanEntry{0x53, kvk::keypad_decimal}, ScanEntry{0x56, kvk::iso_section}, ScanEntry{0x57, kvk::f11},
    ScanEntry{0x58, kvk::f12}, ScanEntry{0x59, kvk::keypad_equals}, ScanEntry{0x64, kvk::f13},
    ScanEntry{0x65, kvk::f14}, ScanEntry{0x66, kvk::f15}, ScanEntry{0x67, kvk::f16}, ScanEntry{0x68, kvk::f17},
    ScanEntry{0x69, kvk::f18}, ScanEntry{0x6a, kvk::f19}, ScanEntry{0x6b, kvk::f20},
    ScanEntry{0x70, kvk::jis_kana}, ScanEntry{0x73, kvk::jis_underscore}, ScanEntry{0x7b, kvk::jis_eisu},
    ScanEntry{0x7d, kvk::jis_yen}, ScanEntry{0x7e, kvk::jis_keypad_comma},
    ScanEntry{0xe01c, kvk::keypad_enter}, ScanEntry{0xe01d, kvk::right_control},
    ScanEntry{0xe020, kvk::mute}, ScanEntry{0xe02e, kvk::volume_down}, ScanEntry{0xe030, kvk::volume_up},
    ScanEntry{0xe035, kvk::keypad_divide}, ScanEntry{0xe037, kvk::f13}, ScanEntry{0xe038, kvk::right_option},
    ScanEntry{0xe045, kvk::keypad_clear}, ScanEntry{0xe047, kvk::home}, ScanEntry{0xe048, kvk::up_arrow},
    ScanEntry{0xe049, kvk::page_up}, ScanEntry{0xe04b, kvk::left_arrow}, ScanEntry{0xe04d, kvk::right_arrow},
    ScanEntry{0xe04f, kvk::end}, ScanEntry{0xe050, kvk::down_arrow}, ScanEntry{0xe051, kvk::page_down},
    ScanEntry{0xe052, kvk::help}, ScanEntry{0xe053, kvk::forward_delete}, ScanEntry{0xe05b, kvk::command},
    ScanEntry{0xe05c, kvk::right_command}, ScanEntry{0xe05d, kvk::contextual_menu},
};

struct Tables {
    std::array<std::uint16_t, 512> from_scan{};   // [extended * 256 + scan]
    std::array<std::uint32_t, 128> to_scan{};
    Tables() {
        from_scan.fill(no_key_code);
        for (const auto& entry : scan_table) {
            const auto index = (entry.scan >> 8 ? 256U : 0U) + (entry.scan & 0xffU);
            from_scan[index] = entry.key;
            // First entry wins for the inverse (0x46 ScrollLock and 0x65 F14
            // both map to F14; 0x65 is the real F14 key).
            if (entry.key < to_scan.size() && to_scan[entry.key] == 0) to_scan[entry.key] = entry.scan;
        }
        to_scan[kvk::f14] = 0x65;
        to_scan[kvk::f13] = 0x64;
    }
};

const Tables& tables() {
    static const Tables instance;
    return instance;
}

} // namespace

std::uint16_t mac_key_code(std::uint32_t virtual_key, std::uint32_t scan_code, bool extended) noexcept {
    // Keys whose scan codes Windows reports ambiguously, and keys that only
    // exist as virtual keys (media keys, synthesized input without a scan
    // code).
    switch (virtual_key) {
    case VK_PAUSE: return kvk::f15;
    case VK_NUMLOCK: return kvk::keypad_clear;
    case VK_SNAPSHOT: return kvk::f13;
    case VK_VOLUME_MUTE: return kvk::mute;
    case VK_VOLUME_DOWN: return kvk::volume_down;
    case VK_VOLUME_UP: return kvk::volume_up;
    case VK_LWIN: return kvk::command;
    case VK_RWIN: return kvk::right_command;
    case VK_APPS: return kvk::contextual_menu;
    default: break;
    }
    if (scan_code != 0 && scan_code < 256) {
        const auto key = tables().from_scan[(extended ? 256U : 0U) + scan_code];
        if (key != no_key_code) return key;
    }
    // Fallback for input without a usable scan code.
    if (virtual_key >= 'A' && virtual_key <= 'Z') {
        static constexpr std::uint16_t letters[26] = {kvk::ansi_a, kvk::ansi_b, kvk::ansi_c, kvk::ansi_d,
            kvk::ansi_e, kvk::ansi_f, kvk::ansi_g, kvk::ansi_h, kvk::ansi_i, kvk::ansi_j, kvk::ansi_k, kvk::ansi_l,
            kvk::ansi_m, kvk::ansi_n, kvk::ansi_o, kvk::ansi_p, kvk::ansi_q, kvk::ansi_r, kvk::ansi_s, kvk::ansi_t,
            kvk::ansi_u, kvk::ansi_v, kvk::ansi_w, kvk::ansi_x, kvk::ansi_y, kvk::ansi_z};
        return letters[virtual_key - 'A'];
    }
    if (virtual_key >= '0' && virtual_key <= '9') {
        static constexpr std::uint16_t digits[10] = {kvk::ansi_0, kvk::ansi_1, kvk::ansi_2, kvk::ansi_3,
            kvk::ansi_4, kvk::ansi_5, kvk::ansi_6, kvk::ansi_7, kvk::ansi_8, kvk::ansi_9};
        return digits[virtual_key - '0'];
    }
    switch (virtual_key) {
    case VK_RETURN: return extended ? kvk::keypad_enter : kvk::return_key;
    case VK_BACK: return kvk::delete_key;
    case VK_TAB: return kvk::tab;
    case VK_SPACE: return kvk::space;
    case VK_ESCAPE: return kvk::escape;
    case VK_LSHIFT: case VK_SHIFT: return kvk::shift;
    case VK_RSHIFT: return kvk::right_shift;
    case VK_LCONTROL: case VK_CONTROL: return extended ? kvk::right_control : kvk::control;
    case VK_RCONTROL: return kvk::right_control;
    case VK_LMENU: case VK_MENU: return extended ? kvk::right_option : kvk::option;
    case VK_RMENU: return kvk::right_option;
    case VK_CAPITAL: return kvk::caps_lock;
    case VK_LEFT: return kvk::left_arrow;
    case VK_RIGHT: return kvk::right_arrow;
    case VK_UP: return kvk::up_arrow;
    case VK_DOWN: return kvk::down_arrow;
    case VK_HOME: return kvk::home;
    case VK_END: return kvk::end;
    case VK_PRIOR: return kvk::page_up;
    case VK_NEXT: return kvk::page_down;
    case VK_INSERT: return kvk::help;
    case VK_DELETE: return kvk::forward_delete;
    case VK_NUMPAD0: return kvk::keypad_0;
    case VK_NUMPAD1: return kvk::keypad_1;
    case VK_NUMPAD2: return kvk::keypad_2;
    case VK_NUMPAD3: return kvk::keypad_3;
    case VK_NUMPAD4: return kvk::keypad_4;
    case VK_NUMPAD5: return kvk::keypad_5;
    case VK_NUMPAD6: return kvk::keypad_6;
    case VK_NUMPAD7: return kvk::keypad_7;
    case VK_NUMPAD8: return kvk::keypad_8;
    case VK_NUMPAD9: return kvk::keypad_9;
    case VK_MULTIPLY: return kvk::keypad_multiply;
    case VK_ADD: return kvk::keypad_plus;
    case VK_SUBTRACT: return kvk::keypad_minus;
    case VK_DECIMAL: return kvk::keypad_decimal;
    case VK_DIVIDE: return kvk::keypad_divide;
    case VK_OEM_MINUS: return kvk::ansi_minus;
    case VK_OEM_PLUS: return kvk::ansi_equal;
    case VK_OEM_COMMA: return kvk::ansi_comma;
    case VK_OEM_PERIOD: return kvk::ansi_period;
    case VK_OEM_1: return kvk::ansi_semicolon;
    case VK_OEM_2: return kvk::ansi_slash;
    case VK_OEM_3: return kvk::ansi_grave;
    case VK_OEM_4: return kvk::ansi_left_bracket;
    case VK_OEM_5: return kvk::ansi_backslash;
    case VK_OEM_6: return kvk::ansi_right_bracket;
    case VK_OEM_7: return kvk::ansi_quote;
    case VK_OEM_102: return kvk::iso_section;
    default: break;
    }
    if (virtual_key >= VK_F1 && virtual_key <= VK_F20) {
        static constexpr std::uint16_t functions[20] = {kvk::f1, kvk::f2, kvk::f3, kvk::f4, kvk::f5, kvk::f6,
            kvk::f7, kvk::f8, kvk::f9, kvk::f10, kvk::f11, kvk::f12, kvk::f13, kvk::f14, kvk::f15, kvk::f16,
            kvk::f17, kvk::f18, kvk::f19, kvk::f20};
        return functions[virtual_key - VK_F1];
    }
    return no_key_code;
}

std::uint32_t scan_code_for_mac_key(std::uint16_t key_code) noexcept {
    if (key_code >= tables().to_scan.size()) return 0;
    return tables().to_scan[key_code];
}

std::uint16_t apply_modifier_mapping(std::uint16_t key_code, bool swap_control_command) noexcept {
    if (!swap_control_command) return key_code;
    switch (key_code) {
    case kvk::control: return kvk::command;
    case kvk::right_control: return kvk::right_command;
    case kvk::command: return kvk::control;
    case kvk::right_command: return kvk::right_control;
    default: return key_code;
    }
}

bool is_modifier_key(std::uint16_t key_code) noexcept {
    switch (key_code) {
    case kvk::shift: case kvk::right_shift: case kvk::control: case kvk::right_control: case kvk::option:
    case kvk::right_option: case kvk::command: case kvk::right_command: case kvk::caps_lock: case kvk::function:
        return true;
    default: return false;
    }
}

std::uint64_t modifier_bits(std::uint16_t key_code) noexcept {
    switch (key_code) {
    case kvk::shift: return modifier::shift | device_modifier::left_shift;
    case kvk::right_shift: return modifier::shift | device_modifier::right_shift;
    case kvk::control: return modifier::control | device_modifier::left_control;
    case kvk::right_control: return modifier::control | device_modifier::right_control;
    case kvk::option: return modifier::option | device_modifier::left_option;
    case kvk::right_option: return modifier::option | device_modifier::right_option;
    case kvk::command: return modifier::command | device_modifier::left_command;
    case kvk::right_command: return modifier::command | device_modifier::right_command;
    case kvk::function: return modifier::function;
    default: return 0;
    }
}

std::uint64_t modifier_flags(const PhysicalModifiers& keys, bool swap) noexcept {
    std::uint64_t flags = keys.caps_lock_on ? modifier::caps_lock : 0;
    const auto add = [&](bool down, std::uint16_t key) {
        if (down) flags |= modifier_bits(apply_modifier_mapping(key, swap));
    };
    add(keys.left_shift, kvk::shift);
    add(keys.right_shift, kvk::right_shift);
    add(keys.left_control, kvk::control);
    add(keys.right_control, kvk::right_control);
    add(keys.left_alt, kvk::option);
    add(keys.right_alt, kvk::right_option);
    add(keys.left_windows, kvk::command);
    add(keys.right_windows, kvk::right_command);
    return flags;
}

std::uint64_t key_event_flags(std::uint16_t key_code) noexcept {
    switch (key_code) {
    case kvk::left_arrow: case kvk::right_arrow: case kvk::up_arrow: case kvk::down_arrow:
        return modifier::function | modifier::numeric_pad;
    case kvk::home: case kvk::end: case kvk::page_up: case kvk::page_down: case kvk::forward_delete:
    case kvk::help: case kvk::f1: case kvk::f2: case kvk::f3: case kvk::f4: case kvk::f5: case kvk::f6:
    case kvk::f7: case kvk::f8: case kvk::f9: case kvk::f10: case kvk::f11: case kvk::f12: case kvk::f13:
    case kvk::f14: case kvk::f15: case kvk::f16: case kvk::f17: case kvk::f18: case kvk::f19: case kvk::f20:
        return modifier::function;
    case kvk::keypad_0: case kvk::keypad_1: case kvk::keypad_2: case kvk::keypad_3: case kvk::keypad_4:
    case kvk::keypad_5: case kvk::keypad_6: case kvk::keypad_7: case kvk::keypad_8: case kvk::keypad_9:
    case kvk::keypad_decimal: case kvk::keypad_multiply: case kvk::keypad_plus: case kvk::keypad_clear:
    case kvk::keypad_divide: case kvk::keypad_enter: case kvk::keypad_minus: case kvk::keypad_equals:
        return modifier::numeric_pad;
    default: return 0;
    }
}

std::optional<char32_t> event_special_character(std::uint16_t key_code, bool shift) noexcept {
    // NSUpArrowFunctionKey = 0xF700 ... (NSEvent.h function-key constants).
    switch (key_code) {
    case kvk::up_arrow: return 0xf700;
    case kvk::down_arrow: return 0xf701;
    case kvk::left_arrow: return 0xf702;
    case kvk::right_arrow: return 0xf703;
    case kvk::f1: return 0xf704;
    case kvk::f2: return 0xf705;
    case kvk::f3: return 0xf706;
    case kvk::f4: return 0xf707;
    case kvk::f5: return 0xf708;
    case kvk::f6: return 0xf709;
    case kvk::f7: return 0xf70a;
    case kvk::f8: return 0xf70b;
    case kvk::f9: return 0xf70c;
    case kvk::f10: return 0xf70d;
    case kvk::f11: return 0xf70e;
    case kvk::f12: return 0xf70f;
    case kvk::f13: return 0xf710;
    case kvk::f14: return 0xf711;
    case kvk::f15: return 0xf712;
    case kvk::f16: return 0xf713;
    case kvk::f17: return 0xf714;
    case kvk::f18: return 0xf715;
    case kvk::f19: return 0xf716;
    case kvk::f20: return 0xf717;
    case kvk::forward_delete: return 0xf728;
    case kvk::home: return 0xf729;
    case kvk::end: return 0xf72b;
    case kvk::page_up: return 0xf72c;
    case kvk::page_down: return 0xf72d;
    case kvk::contextual_menu: return 0xf735;
    case kvk::keypad_clear: return 0xf739;
    case kvk::help: return 0xf746;
    case kvk::return_key: return U'\r';
    case kvk::keypad_enter: return 0x03;
    case kvk::delete_key: return 0x7f;
    case kvk::escape: return 0x1b;
    case kvk::tab: return shift ? 0x19 : U'\t';
    default: return std::nullopt;
    }
}

std::optional<char32_t> translate_special_character(std::uint16_t key_code) noexcept {
    switch (key_code) {
    case kvk::left_arrow: return 0x1c;
    case kvk::right_arrow: return 0x1d;
    case kvk::up_arrow: return 0x1e;
    case kvk::down_arrow: return 0x1f;
    case kvk::f1: case kvk::f2: case kvk::f3: case kvk::f4: case kvk::f5: case kvk::f6: case kvk::f7:
    case kvk::f8: case kvk::f9: case kvk::f10: case kvk::f11: case kvk::f12: case kvk::f13: case kvk::f14:
    case kvk::f15: case kvk::f16: case kvk::f17: case kvk::f18: case kvk::f19: case kvk::f20:
        return 0x10;
    case kvk::home: return 0x01;
    case kvk::end: return 0x04;
    case kvk::help: return 0x05;
    case kvk::page_up: return 0x0b;
    case kvk::page_down: return 0x0c;
    case kvk::forward_delete: return 0x7f;
    case kvk::delete_key: return 0x08;
    case kvk::return_key: return U'\r';
    case kvk::keypad_enter: return 0x03;
    case kvk::escape: return 0x1b;
    case kvk::tab: return U'\t';
    case kvk::keypad_clear: return 0x1b;
    default: return std::nullopt;
    }
}

} // namespace alchemy::gui::win32
