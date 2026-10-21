// Window server pure tables (owner: A3): keycodes and modifier flags,
// mixed-DPI coordinate conversion, clipboard text and image codecs.
#include "alchemy/host/win32/gui/clipboard_codec.hpp"
#include "alchemy/host/win32/gui/keymap.hpp"
#include "alchemy/darwin/windowserver/screen_layout.hpp"

#include <cmath>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace {

namespace gui = alchemy::gui;
namespace win32 = alchemy::gui::win32;
namespace kvk = alchemy::gui::win32::kvk;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool close_to(double a, double b, double tolerance = 1e-9) { return std::abs(a - b) <= tolerance; }

std::string hex(unsigned value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%x", value);
    return buffer;
}

// ---- keycodes ---------------------------------------------------------------------

void test_keycodes() {
    struct Case {
        unsigned scan;
        bool extended;
        std::uint16_t key;
    };
    const Case cases[] = {
        {0x1e, false, kvk::ansi_a}, {0x10, false, kvk::ansi_q}, {0x2c, false, kvk::ansi_z},
        {0x02, false, kvk::ansi_1}, {0x0b, false, kvk::ansi_0}, {0x29, false, kvk::ansi_grave},
        {0x56, false, kvk::iso_section}, {0x1c, false, kvk::return_key}, {0x1c, true, kvk::keypad_enter},
        {0x0e, false, kvk::delete_key}, {0x53, true, kvk::forward_delete}, {0x53, false, kvk::keypad_decimal},
        {0x48, true, kvk::up_arrow}, {0x48, false, kvk::keypad_8}, {0x4b, true, kvk::left_arrow},
        {0x47, true, kvk::home}, {0x4f, true, kvk::end}, {0x49, true, kvk::page_up}, {0x51, true, kvk::page_down},
        {0x52, true, kvk::help}, {0x3b, false, kvk::f1}, {0x58, false, kvk::f12}, {0x64, false, kvk::f13},
        {0x6b, false, kvk::f20}, {0x2a, false, kvk::shift}, {0x36, false, kvk::right_shift},
        {0x1d, false, kvk::control}, {0x1d, true, kvk::right_control}, {0x38, false, kvk::option},
        {0x38, true, kvk::right_option}, {0x5b, true, kvk::command}, {0x5c, true, kvk::right_command},
        {0x3a, false, kvk::caps_lock}, {0x39, false, kvk::space}, {0x0f, false, kvk::tab},
        {0x01, false, kvk::escape}, {0x35, true, kvk::keypad_divide}, {0x37, false, kvk::keypad_multiply},
        {0x4a, false, kvk::keypad_minus}, {0x4e, false, kvk::keypad_plus}, {0x5d, true, kvk::contextual_menu},
        {0x7d, false, kvk::jis_yen}, {0x73, false, kvk::jis_underscore},
    };
    for (const auto& item : cases) {
        const auto key = win32::mac_key_code(0, item.scan, item.extended);
        check(key == item.key, "scan " + hex(item.scan) + (item.extended ? " (E0)" : "") + " -> " + hex(key) +
                                   ", expected " + hex(item.key));
    }
    // Virtual keys resolve ambiguous scan codes and synthesized input.
    check(win32::mac_key_code(VK_PAUSE, 0x45, false) == kvk::f15, "Pause");
    check(win32::mac_key_code(VK_NUMLOCK, 0x45, true) == kvk::keypad_clear, "NumLock");
    check(win32::mac_key_code('A', 0, false) == kvk::ansi_a, "VK A without scan code");
    check(win32::mac_key_code('7', 0, false) == kvk::ansi_7, "VK 7 without scan code");
    check(win32::mac_key_code(VK_F5, 0, false) == kvk::f5, "VK F5 without scan code");
    check(win32::mac_key_code(VK_RETURN, 0, true) == kvk::keypad_enter, "VK Return extended");
    check(win32::mac_key_code(VK_VOLUME_UP, 0x30, true) == kvk::volume_up, "volume up");
    check(win32::mac_key_code(VK_BROWSER_BACK, 0x6a, true) == win32::no_key_code, "browser back has no mac key");

    // Every key of the US extended keyboard has a code, and the inverse
    // (used by translate_key) round-trips.
    const std::uint16_t required[] = {
        kvk::ansi_a, kvk::ansi_s, kvk::ansi_d, kvk::ansi_f, kvk::ansi_h, kvk::ansi_g, kvk::ansi_z, kvk::ansi_x,
        kvk::ansi_c, kvk::ansi_v, kvk::iso_section, kvk::ansi_b, kvk::ansi_q, kvk::ansi_w, kvk::ansi_e,
        kvk::ansi_r, kvk::ansi_y, kvk::ansi_t, kvk::ansi_1, kvk::ansi_2, kvk::ansi_3, kvk::ansi_4, kvk::ansi_6,
        kvk::ansi_5, kvk::ansi_equal, kvk::ansi_9, kvk::ansi_7, kvk::ansi_minus, kvk::ansi_8, kvk::ansi_0,
        kvk::ansi_right_bracket, kvk::ansi_o, kvk::ansi_u, kvk::ansi_left_bracket, kvk::ansi_i, kvk::ansi_p,
        kvk::return_key, kvk::ansi_l, kvk::ansi_j, kvk::ansi_quote, kvk::ansi_k, kvk::ansi_semicolon,
        kvk::ansi_backslash, kvk::ansi_comma, kvk::ansi_slash, kvk::ansi_n, kvk::ansi_m, kvk::ansi_period,
        kvk::tab, kvk::space, kvk::ansi_grave, kvk::delete_key, kvk::escape, kvk::right_command, kvk::command,
        kvk::shift, kvk::caps_lock, kvk::option, kvk::control, kvk::right_shift, kvk::right_option,
        kvk::right_control, kvk::f17, kvk::keypad_decimal, kvk::keypad_multiply, kvk::keypad_plus,
        kvk::keypad_clear, kvk::keypad_divide, kvk::keypad_enter, kvk::keypad_minus, kvk::f18, kvk::f19,
        kvk::keypad_equals, kvk::keypad_0, kvk::keypad_1, kvk::keypad_2, kvk::keypad_3, kvk::keypad_4,
        kvk::keypad_5, kvk::keypad_6, kvk::keypad_7, kvk::f20, kvk::keypad_8, kvk::keypad_9, kvk::f5, kvk::f6,
        kvk::f7, kvk::f3, kvk::f8, kvk::f9, kvk::f11, kvk::f13, kvk::f16, kvk::f14, kvk::f10,
        kvk::contextual_menu, kvk::f12, kvk::f15, kvk::help, kvk::home, kvk::page_up, kvk::forward_delete,
        kvk::f4, kvk::end, kvk::f2, kvk::page_down, kvk::f1, kvk::left_arrow, kvk::right_arrow,
        kvk::down_arrow, kvk::up_arrow, kvk::volume_up, kvk::volume_down, kvk::mute, kvk::jis_yen,
        kvk::jis_underscore, kvk::jis_keypad_comma, kvk::jis_eisu, kvk::jis_kana,
    };
    std::set<std::uint16_t> seen;
    for (const auto key : required) {
        check(seen.insert(key).second, "duplicate required key " + hex(key));
        const auto scan = win32::scan_code_for_mac_key(key);
        if (scan == 0) {
            check(key == kvk::keypad_clear || key == kvk::f15, "mac key " + hex(key) + " has no scan code");
            continue;
        }
        const auto back = win32::mac_key_code(0, scan & 0xff, (scan >> 8) != 0);
        check(back == key, "mac key " + hex(key) + " -> scan " + hex(scan) + " -> " + hex(back));
    }
    // No two scan codes collide on one mac key except the documented
    // aliases (PrintScreen/F13, ScrollLock/F14).
    std::set<std::uint16_t> keys;
    int aliases = 0;
    for (unsigned extended = 0; extended < 2; ++extended)
        for (unsigned scan = 1; scan < 256; ++scan) {
            const auto key = win32::mac_key_code(0, scan, extended != 0);
            if (key == win32::no_key_code) continue;
            if (!keys.insert(key).second) ++aliases;
        }
    check(aliases == 2, "scan-code aliases: " + std::to_string(aliases));
}

void test_modifiers() {
    using gui::modifier::caps_lock;
    using gui::modifier::command;
    using gui::modifier::control;
    using gui::modifier::option;
    using gui::modifier::shift;
    namespace device = win32::device_modifier;
    win32::PhysicalModifiers keys;
    check(win32::modifier_flags(keys, false) == 0, "no modifiers");
    keys.left_shift = true;
    check(win32::modifier_flags(keys, false) == (shift | device::left_shift), "left shift");
    keys.right_shift = true;
    check(win32::modifier_flags(keys, false) == (shift | device::left_shift | device::right_shift), "both shifts");
    keys = {};
    keys.left_control = true;
    keys.right_alt = true;
    keys.left_windows = true;
    keys.caps_lock_on = true;
    check(win32::modifier_flags(keys, false) ==
              (control | device::left_control | option | device::right_option | command | device::left_command |
               caps_lock),
          "ctrl+ralt+win+caps");
    // Ctrl <-> Command swap.
    keys = {};
    keys.left_control = true;
    check(win32::modifier_flags(keys, true) == (command | device::left_command), "swapped ctrl is command");
    keys = {};
    keys.right_windows = true;
    check(win32::modifier_flags(keys, true) == (control | device::right_control), "swapped win is control");
    check(win32::apply_modifier_mapping(kvk::control, true) == kvk::command, "swap control key code");
    check(win32::apply_modifier_mapping(kvk::ansi_a, true) == kvk::ansi_a, "swap leaves letters");
    check(win32::is_modifier_key(kvk::caps_lock) && !win32::is_modifier_key(kvk::space), "is_modifier_key");

    check(win32::key_event_flags(kvk::up_arrow) == (gui::modifier::function | gui::modifier::numeric_pad),
          "arrow flags");
    check(win32::key_event_flags(kvk::f5) == gui::modifier::function, "F5 flags");
    check(win32::key_event_flags(kvk::keypad_7) == gui::modifier::numeric_pad, "keypad flags");
    check(win32::key_event_flags(kvk::ansi_a) == 0, "letter flags");

    check(win32::event_special_character(kvk::up_arrow, false) == char32_t{0xf700}, "NSUpArrowFunctionKey");
    check(win32::event_special_character(kvk::f1, false) == char32_t{0xf704}, "NSF1FunctionKey");
    check(win32::event_special_character(kvk::f20, false) == char32_t{0xf717}, "NSF20FunctionKey");
    check(win32::event_special_character(kvk::forward_delete, false) == char32_t{0xf728}, "NSDeleteFunctionKey");
    check(win32::event_special_character(kvk::delete_key, false) == char32_t{0x7f}, "Backspace is DEL");
    check(win32::event_special_character(kvk::return_key, false) == char32_t{U'\r'}, "Return");
    check(win32::event_special_character(kvk::tab, true) == char32_t{0x19}, "Shift+Tab is backtab");
    check(!win32::event_special_character(kvk::ansi_a, false), "letters use the layout");
    check(win32::translate_special_character(kvk::left_arrow) == char32_t{0x1c}, "UCKeyTranslate left arrow");
    check(win32::translate_special_character(kvk::f3) == char32_t{0x10}, "UCKeyTranslate F3");
    check(win32::translate_special_character(kvk::delete_key) == char32_t{0x08}, "UCKeyTranslate delete");
}

// ---- geometry ---------------------------------------------------------------------

win32::MonitorPixels monitor(int left, int top, int width, int height, double scale, int taskbar = 0) {
    win32::MonitorPixels pixels;
    pixels.bounds = {left, top, left + width, top + height};
    pixels.work = {left, top, left + width, top + height - taskbar};
    pixels.scale = scale;
    pixels.handle = static_cast<std::uintptr_t>(left * 7 + top * 13 + 1000003);
    return pixels;
}

void round_trip_pixels(const win32::ScreenLayout& layout, const std::string& name) {
    for (std::size_t index = 0; index < layout.monitors().size(); ++index) {
        const auto& bounds = layout.monitors()[index].pixels.bounds;
        for (int step = 0; step < 16; ++step) {
            const win32::PixelPoint pixel{bounds.left + (bounds.width() - 1) * (step / 15.0),
                                          bounds.top + (bounds.height() - 1) * ((step * 7 % 16) / 15.0)};
            const auto point = layout.to_points(pixel);
            check(layout.monitor_for_point(point) == index,
                  name + ": point " + std::to_string(point.x) + "," + std::to_string(point.y) +
                      " lands on its monitor " + std::to_string(index));
            const auto back = layout.to_pixels(point);
            check(close_to(back.x, pixel.x, 1e-6) && close_to(back.y, pixel.y, 1e-6),
                  name + ": pixel round trip on monitor " + std::to_string(index));
        }
        // Window-sized rects: pixel -> point -> pixel exactly.
        const auto scale = layout.monitors()[index].pixels.scale;
        const win32::PixelRect rect{bounds.left + 30, bounds.top + 60, bounds.left + 30 + 600, bounds.top + 60 + 450};
        const auto points = layout.to_points(rect, index, scale);
        const auto back = layout.to_pixels(points, index, scale);
        check(back.left == rect.left && back.top == rect.top && back.right == rect.right && back.bottom == rect.bottom,
              name + ": rect round trip on monitor " + std::to_string(index));
        check(layout.monitor_for_rect(points) == index, name + ": rect lands on its monitor");
    }
}

void test_geometry() {
    {
        const win32::ScreenLayout layout({monitor(0, 0, 1920, 1080, 1.0, 40)});
        check(close_to(layout.primary_height(), 1080), "single: height");
        const auto origin = layout.to_points(win32::PixelPoint{0, 0});
        check(close_to(origin.x, 0) && close_to(origin.y, 1080), "single: top-left pixel is (0, height)");
        const auto bottom = layout.to_points(win32::PixelPoint{0, 1080});
        check(close_to(bottom.y, 0), "single: bottom edge is y 0");
        const auto frame = layout.frame(0);
        check(close_to(frame.origin.x, 0) && close_to(frame.origin.y, 0) && close_to(frame.size.width, 1920), "single: frame");
        const auto visible = layout.visible_frame(0);
        check(close_to(visible.origin.y, 40) && close_to(visible.size.height, 1040), "single: visible frame above taskbar");
        round_trip_pixels(layout, "single");
    }
    {
        // 4K at 200% with a 1080p monitor at 100% to its right, top-aligned.
        const win32::ScreenLayout layout({monitor(0, 0, 3840, 2160, 2.0), monitor(3840, 0, 1920, 1080, 1.0)});
        const auto& second = layout.monitors()[1];
        check(close_to(second.x, 1920) && close_to(second.y, 0) && close_to(second.width, 1920), "right: placed against edge");
        const auto frame = layout.frame(1);
        check(close_to(frame.origin.x, 1920) && close_to(frame.origin.y, 0), "right: AppKit frame");
        const auto point = layout.to_points(win32::PixelPoint{3840, 0});
        check(close_to(point.x, 1920) && close_to(point.y, 1080), "right: shared corner");
        round_trip_pixels(layout, "right");
    }
    {
        // 125% monitor left of a 150% primary, bottom-aligned.
        const win32::ScreenLayout layout({monitor(0, 0, 2880, 1620, 1.5), monitor(-2560, 180, 2560, 1440, 1.25)});
        const auto& second = layout.monitors()[1];
        check(close_to(second.x, -2048) && close_to(second.width, 2048), "left: placed against edge");
        check(close_to(second.y, 180 / 1.5), "left: offset along the edge in the neighbour's points");
        round_trip_pixels(layout, "left");
    }
    {
        // Above the primary, and a third monitor chained to the right of
        // the second: edges stay shared in point space.
        const win32::ScreenLayout layout({monitor(0, 0, 1920, 1080, 1.0), monitor(0, -1440, 2560, 1440, 1.5),
                                          monitor(2560, -1440, 1920, 1080, 1.0)});
        const auto& above = layout.monitors()[1];
        const auto& chained = layout.monitors()[2];
        check(close_to(above.y, -960) && close_to(above.x, 0), "above: placed against the top edge");
        check(close_to(chained.x, above.x + above.width), "chained: placed against the second monitor");
        const auto frame = layout.frame(1);
        check(close_to(frame.origin.y, 1080), "above: AppKit origin y is the primary height");
        round_trip_pixels(layout, "stacked");
    }
    {
        const win32::ScreenLayout layout({monitor(0, 0, 1920, 1080, 1.0)});
        // Off-screen points extrapolate through the nearest monitor.
        const auto pixel = layout.to_pixels(gui::Point{-100, 2000});
        check(close_to(pixel.x, -100) && close_to(pixel.y, 1080 - 2000), "off-screen extrapolation");
    }
    {
        bool threw = false;
        try {
            static_cast<void>(win32::ScreenLayout(std::vector<win32::MonitorPixels>{}).to_points(win32::PixelPoint{0, 0}));
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check(threw, "empty layout fails explicitly");
    }
}

// ---- clipboard codecs -------------------------------------------------------------

void test_text_codec() {
    const std::string text = "Alchemy \xe2\x9c\x93 \xf0\x9f\x98\x80 caf\xc3\xa9";
    const auto wide = win32::utf8_to_wide(text);
    check(wide.size() == 17, "UTF-16 length with a surrogate pair: " + std::to_string(wide.size()));
    check(win32::wide_to_utf8(wide) == text, "UTF-8 round trip");
    check(win32::utf8_to_wide("a\xff" "b") == L"a\xfffd" L"b", "invalid UTF-8 becomes U+FFFD");
    check(win32::utf8_to_wide("\xc0\xaf") == L"\xfffd\xfffd", "overlong UTF-8 is rejected");
    check(win32::wide_to_utf8(std::wstring(1, static_cast<wchar_t>(0xd800))) == "\xef\xbf\xbd",
          "lone surrogate becomes U+FFFD");
    check(win32::clipboard_text_to_windows("a\nb\r\nc") == L"a\r\nb\r\nc", "LF -> CRLF");
    check(win32::clipboard_text_from_windows(L"a\r\nb\r\nc\0junk") == "a\nb\nc", "CRLF -> LF, stop at NUL");
}

void test_image_codec() {
    gui::Image image;
    image.width = 3;
    image.height = 2;
    for (std::uint8_t index = 0; index < 6; ++index) {
        image.rgba.push_back(static_cast<std::uint8_t>(index * 40));
        image.rgba.push_back(static_cast<std::uint8_t>(255 - index * 30));
        image.rgba.push_back(static_cast<std::uint8_t>(index * 7));
        image.rgba.push_back(static_cast<std::uint8_t>(index == 0 ? 0 : 50 + index * 20));
    }
    const auto dib = win32::image_to_dibv5(image);
    check(dib.size() == sizeof(BITMAPV5HEADER) + 24, "DIBV5 size");
    const auto back = win32::image_from_dib(dib.data(), dib.size());
    check(back.width == 3 && back.height == 2 && back.rgba == image.rgba, "RGBA -> DIBV5 -> RGBA");

    // A 24 bpp bottom-up BI_RGB DIB with row padding.
    {
        BITMAPINFOHEADER header{sizeof(header), 3, 2, 1, 24, BI_RGB, 0, 0, 0, 0, 0};
        std::vector<std::uint8_t> data(sizeof(header) + 2 * 12);
        std::memcpy(data.data(), &header, sizeof(header));
        auto* pixels = data.data() + sizeof(header);
        // bottom row (y = 1): pure blue pixels; top row (y = 0): red, green, white
        const std::uint8_t bottom[] = {255, 0, 0, 255, 0, 0, 255, 0, 0};
        const std::uint8_t top[] = {0, 0, 255, 0, 255, 0, 255, 255, 255};
        std::memcpy(pixels, bottom, 9);
        std::memcpy(pixels + 12, top, 9);
        const auto decoded = win32::image_from_dib(data.data(), data.size());
        const std::vector<std::uint8_t> expected = {255, 0,   0,   255, 0,   255, 0,   255, 255, 255, 255, 255,
                                                    0,   0,   255, 255, 0,   0,   255, 255, 0,   0,   255, 255};
        check(decoded.rgba == expected, "24 bpp bottom-up DIB");
    }
    // 32 bpp BI_RGB with an all-zero alpha byte is opaque.
    {
        BITMAPINFOHEADER header{sizeof(header), 1, -1, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
        std::vector<std::uint8_t> data(sizeof(header) + 4);
        std::memcpy(data.data(), &header, sizeof(header));
        const std::uint8_t pixel[] = {10, 20, 30, 0};
        std::memcpy(data.data() + sizeof(header), pixel, 4);
        const auto decoded = win32::image_from_dib(data.data(), data.size());
        check(decoded.rgba == std::vector<std::uint8_t>({30, 20, 10, 255}), "32 bpp without alpha is opaque");
    }
    // 8 bpp palettized.
    {
        BITMAPINFOHEADER header{sizeof(header), 2, -1, 1, 8, BI_RGB, 0, 0, 0, 2, 0};
        std::vector<std::uint8_t> data(sizeof(header) + 2 * sizeof(RGBQUAD) + 4);
        std::memcpy(data.data(), &header, sizeof(header));
        const RGBQUAD palette[] = {{1, 2, 3, 0}, {200, 100, 50, 0}};
        std::memcpy(data.data() + sizeof(header), palette, sizeof(palette));
        data[sizeof(header) + sizeof(palette)] = 1;
        data[sizeof(header) + sizeof(palette) + 1] = 0;
        const auto decoded = win32::image_from_dib(data.data(), data.size());
        check(decoded.rgba == std::vector<std::uint8_t>({50, 100, 200, 255, 3, 2, 1, 255}), "8 bpp palette");
    }
    // Compressed bitmaps fail explicitly.
    {
        BITMAPINFOHEADER header{sizeof(header), 1, 1, 1, 8, BI_RLE8, 0, 0, 0, 0, 0};
        std::vector<std::uint8_t> data(sizeof(header) + 64);
        std::memcpy(data.data(), &header, sizeof(header));
        bool threw = false;
        try {
            static_cast<void>(win32::image_from_dib(data.data(), data.size()));
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check(threw, "RLE DIB is unsupported explicitly");
    }
}

} // namespace

int main() {
    try {
        test_keycodes();
        test_modifiers();
        test_geometry();
        test_text_codec();
        test_image_codec();
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " window server table check(s) failed\n";
        return 1;
    }
    std::cout << "Alchemy GUI window server tables passed\n";
    return 0;
}
