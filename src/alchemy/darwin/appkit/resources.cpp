#include "alchemy/darwin/appkit/appkit_internal.hpp"
#include "alchemy/path.hpp"
#include "alchemy/darwin/coregraphics/imageio/codec.hpp"

#include <algorithm>
#include <mutex>
#include <memory>
#include <map>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>

#include "alchemy/host/ui.hpp"

// AppKit's resource classes: NSMenu/NSMenuItem (kept as a tree; Windows has
// no application menu bar, key equivalents still work), NSPasteboard (the
// window server's clipboard, drag pasteboards), NSCursor, NSImage /
// NSBitmapImageRep / NSBezierPath with a small software renderer for
// -lockFocus drawing, NSColor / NSColorSpace / NSAppearance, NSColorSampler,
// NSWorkspace,
// NSDocumentController and NSScreen.
namespace alchemy::gui::appkit {
namespace {
// NSBitmapFormat
constexpr std::uint64_t alpha_first = 1, alpha_nonpremultiplied = 2, floating_point = 4;

constexpr std::string_view general_name = "Apple CFPasteboard general", drag_name = "Apple CFPasteboard drag";
bool string_type(std::string_view type) {
    return type == "public.utf8-plain-text" || type == "NSStringPboardType" || type == "public.plain-text" ||
           type == "public.utf16-plain-text" || type == "public.text";
}
bool image_type(std::string_view type) {
    return type == "public.png" || type == "public.tiff" || type == "NSTIFFPboardType" || type == "Apple PNG pasteboard type";
}
bool file_type(std::string_view type) { return type == "public.file-url" || type == "NSFilenamesPboardType"; }

std::string percent_encode_path(std::string_view path) {
    std::string out;
    constexpr char hex[] = "0123456789ABCDEF";
    for (const auto c : path) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
            std::string_view("/-._~!$&'()*+,;=:@").find(c) != std::string_view::npos) out += c;
        else { out += '%'; out += hex[byte >> 4]; out += hex[byte & 15]; }
    }
    return out;
}

// ---- software rendering into NSBitmapImageRep pixels ----------------------------------
struct Pixels {
    GuestMemory& memory;
    const BitmapState& rep;
    [[nodiscard]] bool supported() const {
        return rep.data && rep.bits_per_sample == 8 && !rep.planar && !(rep.format & floating_point) &&
               (rep.samples_per_pixel == 3 || rep.samples_per_pixel == 4) && rep.bits_per_pixel >= rep.samples_per_pixel * 8;
    }
    [[nodiscard]] Address at(std::int64_t x, std::int64_t row) const {
        return rep.data + static_cast<Address>(row * rep.bytes_per_row + x * (rep.bits_per_pixel / 8));
    }
    // Straight RGBA in 0..1; `row` counts from the top.
    [[nodiscard]] std::array<double, 4> get(std::int64_t x, std::int64_t row) const {
        std::array<std::uint8_t, 4> raw{};
        memory.read_bytes(at(x, row), std::span(raw.data(), static_cast<std::size_t>(rep.samples_per_pixel)));
        std::array<double, 4> value{0, 0, 0, 1};
        const bool first = rep.has_alpha && (rep.format & alpha_first);
        const unsigned color = first ? 1 : 0;
        for (unsigned index = 0; index < 3; ++index) value[index] = raw[color + index] / 255.0;
        if (rep.has_alpha) value[3] = raw[first ? 0 : 3] / 255.0;
        if (rep.has_alpha && !(rep.format & alpha_nonpremultiplied) && value[3] > 0)
            for (unsigned index = 0; index < 3; ++index) value[index] = std::min(1.0, value[index] / value[3]);
        return value;
    }
    void set(std::int64_t x, std::int64_t row, std::array<double, 4> value) const {
        std::array<std::uint8_t, 4> raw{};
        const bool first = rep.has_alpha && (rep.format & alpha_first);
        const unsigned color = first ? 1 : 0;
        const bool premultiply = rep.has_alpha && !(rep.format & alpha_nonpremultiplied);
        const auto byte = [](double v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
        for (unsigned index = 0; index < 3; ++index) raw[color + index] = byte(premultiply ? value[index] * value[3] : value[index]);
        if (rep.has_alpha) raw[first ? 0 : 3] = byte(value[3]);
        memory.write_bytes(at(x, row), std::span<const std::uint8_t>(raw.data(), static_cast<std::size_t>(rep.samples_per_pixel)));
    }
};
// An ICC v2.1 RGB display profile (matrix/TRC) authored here from the color
// space's published parameters: the D50-adapted (Bradford) primaries and the
// IEC 61966-2-1 transfer function sampled into a 1024-entry curve. Every RGB
// space the provider names uses the sRGB transfer function.
std::vector<std::uint8_t> rgb_icc_profile(std::string_view description, const std::array<std::array<double, 3>, 3>& primaries) {
    std::vector<std::uint8_t> out(128, 0);
    const auto u32 = [&out](std::size_t at, std::uint32_t value) {
        for (unsigned index = 0; index < 4; ++index) out[at + index] = static_cast<std::uint8_t>(value >> (24 - 8 * index));
    };
    const auto put32 = [&out](std::uint32_t value) {
        for (unsigned index = 0; index < 4; ++index) out.push_back(static_cast<std::uint8_t>(value >> (24 - 8 * index)));
    };
    const auto fixed = [](double value) { return static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(value * 65536.0))); };
    const auto sig = [](const char (&text)[5]) {
        return (static_cast<std::uint32_t>(static_cast<unsigned char>(text[0])) << 24) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(text[1])) << 16) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(text[2])) << 8) | static_cast<unsigned char>(text[3]);
    };
    constexpr std::array<double, 3> d50{0.9642, 1.0, 0.8249};
    u32(8, 0x02100000);   // version 2.1
    u32(12, sig("mntr")); u32(16, sig("RGB ")); u32(20, sig("XYZ "));
    out[24] = 0x07; out[25] = 0xEA; out[27] = 1; out[29] = 1;   // 2026-01-01 00:00:00
    u32(36, sig("acsp"));
    for (unsigned index = 0; index < 3; ++index) u32(68 + 4 * index, fixed(d50[index]));
    // Tag elements, each 4-byte aligned; the table follows the header.
    std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> elements;
    const auto element = [&](const char (&tag)[5], std::vector<std::uint8_t> bytes) {
        while (bytes.size() % 4) bytes.push_back(0);
        elements.emplace_back(sig(tag), std::move(bytes));
    };
    const auto be32 = [](std::vector<std::uint8_t>& bytes, std::uint32_t value) {
        for (unsigned index = 0; index < 4; ++index) bytes.push_back(static_cast<std::uint8_t>(value >> (24 - 8 * index)));
    };
    const auto xyz = [&](const std::array<double, 3>& value) {
        std::vector<std::uint8_t> bytes;
        be32(bytes, sig("XYZ ")); be32(bytes, 0);
        for (const auto component : value) be32(bytes, fixed(component));
        return bytes;
    };
    {
        std::vector<std::uint8_t> bytes;   // textDescriptionType
        be32(bytes, sig("desc")); be32(bytes, 0);
        be32(bytes, static_cast<std::uint32_t>(description.size() + 1));
        bytes.insert(bytes.end(), description.begin(), description.end());
        bytes.push_back(0);
        be32(bytes, 0); be32(bytes, 0);    // no Unicode description
        bytes.insert(bytes.end(), 2 + 1 + 67, 0);   // no ScriptCode description
        element("desc", std::move(bytes));
    }
    {
        static constexpr std::string_view notice = "No copyright, use freely";
        std::vector<std::uint8_t> bytes;   // textType
        be32(bytes, sig("text")); be32(bytes, 0);
        bytes.insert(bytes.end(), notice.begin(), notice.end());
        bytes.push_back(0);
        element("cprt", std::move(bytes));
    }
    element("wtpt", xyz(d50));
    element("rXYZ", xyz({primaries[0][0], primaries[0][1], primaries[0][2]}));
    element("gXYZ", xyz({primaries[1][0], primaries[1][1], primaries[1][2]}));
    element("bXYZ", xyz({primaries[2][0], primaries[2][1], primaries[2][2]}));
    std::vector<std::uint8_t> curve;   // curveType, shared by the three channels
    constexpr unsigned points = 1024;
    be32(curve, sig("curv")); be32(curve, 0); be32(curve, points);
    for (unsigned index = 0; index < points; ++index) {
        const double x = static_cast<double>(index) / (points - 1);
        const double y = x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
        const auto value = static_cast<std::uint16_t>(std::lround(std::clamp(y, 0.0, 1.0) * 65535.0));
        curve.push_back(static_cast<std::uint8_t>(value >> 8));
        curve.push_back(static_cast<std::uint8_t>(value));
    }
    const auto curve_index = elements.size();
    element("rTRC", curve);
    const auto count = static_cast<std::uint32_t>(elements.size() + 2);   // gTRC and bTRC share rTRC's data
    put32(count);
    auto offset = static_cast<std::uint32_t>(128 + 4 + 12 * count);
    std::uint32_t curve_offset{}, curve_size{};
    for (std::size_t index = 0; index < elements.size(); ++index) {
        const auto size = static_cast<std::uint32_t>(elements[index].second.size());
        if (index == curve_index) { curve_offset = offset; curve_size = size; }
        put32(elements[index].first); put32(offset); put32(size);
        offset += size;
    }
    for (const char* tag : {"gTRC", "bTRC"}) {
        put32((static_cast<std::uint32_t>(tag[0]) << 24) | (static_cast<std::uint32_t>(tag[1]) << 16) |
              (static_cast<std::uint32_t>(tag[2]) << 8) | static_cast<std::uint32_t>(tag[3]));
        put32(curve_offset); put32(curve_size);
    }
    for (const auto& [tag, bytes] : elements) out.insert(out.end(), bytes.begin(), bytes.end());
    u32(0, static_cast<std::uint32_t>(out.size()));
    return out;
}
} // namespace

// ---- images ------------------------------------------------------------------------
std::shared_ptr<BitmapState> AppKit::first_bitmap(Address target) {
    for (const auto rep : objc.host_state<ImageState>(target)->representations)
        if (is_kind(rep, bitmap_rep)) return objc.host_state<BitmapState>(rep);
    return nullptr;
}
Address AppKit::make_bitmap(std::int64_t width, std::int64_t height) {
    const auto rep = objc.allocate(bitmap_rep);
    try { setup_bitmap(rep, width, height); } catch (...) { release(rep); throw; }
    return rep;
}
void AppKit::fill_image(Address target, const DecodedImage& decoded) {
    // One point per pixel: WIC reports 96 dpi for files without resolution
    // metadata, where AppKit assumes 72, so the stored resolution is not used.
    const Size points{static_cast<double>(decoded.pixels.width), static_cast<double>(decoded.pixels.height)};
    const auto rep = make_bitmap(decoded.pixels.width, decoded.pixels.height);
    const auto state = objc.host_state<BitmapState>(rep);
    memory().write_bytes(state->data, decoded.pixels.rgba);
    state->size = points;
    const auto info = objc.host_state<ImageState>(target);
    info->representations.push_back(rep);
    info->size = points;
}
std::vector<std::uint8_t> AppKit::data_bytes(Address data) {
    std::vector<std::uint8_t> bytes;
    if (!data) return bytes;
    const auto length = send(data, "length").x0;
    if (length > (1ULL << 31)) fail("NSData is too large");
    bytes.resize(static_cast<std::size_t>(length));
    if (length) memory().read_bytes(send(data, "bytes").x0, bytes);
    return bytes;
}
Address AppKit::make_data(std::span<const std::uint8_t> bytes) {
    const auto buffer = process.allocate_guest(std::max<std::size_t>(bytes.size(), 1));
    if (!bytes.empty()) memory().write_bytes(buffer, bytes);
    Address data{};
    try { data = send_id(objc.find_class("NSData"), "dataWithBytes:length:", {buffer, bytes.size()}); }
    catch (...) { process.release_guest(buffer); throw; }
    process.release_guest(buffer);
    return data;
}
std::optional<std::vector<std::uint8_t>> AppKit::read_file(std::string_view guest_path) {
    std::ifstream file(utf8_path(host_path(guest_path)), std::ios::binary);
    if (!file) return std::nullopt;
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}
void AppKit::setup_bitmap(Address rep, std::int64_t width, std::int64_t height) {
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768) fail("invalid bitmap size");
    const auto state = objc.host_state<BitmapState>(rep);
    if (state->data) fail("NSBitmapImageRep initialized twice");
    state->width = width; state->height = height;
    state->bytes_per_row = width * 4;
    state->format = alpha_nonpremultiplied;
    state->size = {static_cast<double>(width), static_cast<double>(height)};
    const auto bytes = static_cast<Address>(state->bytes_per_row * height);
    state->data = process.allocate_guest(bytes);
    state->owns = true;
    memory().fill_bytes(state->data, 0, bytes);
    state->color_space_name = string("NSDeviceRGBColorSpace");
}
// The main bundle's icon: CFBundleIconFile, an .icns decoded by ImageIO's
// codec, its largest image (+1). CFBundleIconName (an Assets.car app icon)
// without an .icns fails explicitly: catalog images are not decoded here.
Address AppKit::make_application_icon() {
    const auto bundle = send(objc.find_class("NSBundle"), "mainBundle").x0;
    if (!bundle) return 0;
    const auto info = send(bundle, "infoDictionary").x0;
    const auto value = [&](const char* key) { return info ? send_id(info, "objectForKey:", {auto_string(key)}) : Address{}; };
    std::string file = value("CFBundleIconFile") ? text(value("CFBundleIconFile")) : std::string();
    if (file.empty()) {
        if (value("CFBundleIconName")) unsupported("NSApplicationIcon from an asset catalog (CFBundleIconName without CFBundleIconFile)");
        return 0;
    }
    std::string base = file, type = "icns";
    if (const auto dot = file.find_last_of('.'); dot != std::string::npos) { base = file.substr(0, dot); type = file.substr(dot + 1); }
    const auto path = send_id(bundle, "pathForResource:ofType:", {auto_string(base), auto_string(type)});
    if (!path) return 0;
    const auto bytes = read_file(text(path));
    if (!bytes) return 0;
    const std::span<const std::uint8_t> data(*bytes);
    std::size_t best{}, best_width{};
    const auto frames = quartz2d::codec::frame_count(data);
    for (std::size_t i = 0; i < frames; ++i)
        if (const auto described = quartz2d::codec::describe(data, i); described && described->width > best_width) {
            best_width = described->width;
            best = i;
        }
    const auto frame = quartz2d::codec::decode(data, best);
    if (!frame) return 0;
    const auto made = image_from_pixels(Image{frame->width, frame->height, frame->rgba});
    // Points: the largest icon images are drawn at half their pixel size (@2x).
    if (frame->width >= 512 && responds(made, "setSize:")) {
        CallRegs size;
        put_size(size, 0, {frame->width / 2.0, frame->height / 2.0});
        static_cast<void>(send(made, "setSize:", size));
    }
    return made;
}

Address AppKit::shared_color(const std::string& name, const std::function<Address()>& make) {
    {
        std::lock_guard lock(shared_colors_mutex);
        if (const auto found = shared_colors.find(name); found != shared_colors.end()) return found->second;
    }
    // Made outside the lock (it may send messages); the first one kept wins.
    const auto made = retain(make());
    std::lock_guard lock(shared_colors_mutex);
    const auto [slot, inserted] = shared_colors.emplace(name, made);
    if (!inserted) {
        release(made);
        return slot->second;
    }
    if (const auto record = process.objects().find(made)) record->immortal = true;
    return made;
}
std::optional<Image> AppKit::image_pixels(Address target) { return bitmap_pixels(first_bitmap(target)); }
std::optional<Image> AppKit::bitmap_pixels(const std::shared_ptr<BitmapState>& rep) {
    if (!rep) return std::nullopt;
    const Pixels pixels{memory(), *rep};
    if (!pixels.supported()) unsupported("NSBitmapImageRep pixel format (8-bit RGB/RGBA meshed only)");
    Image result;
    result.width = static_cast<std::uint32_t>(rep->width); result.height = static_cast<std::uint32_t>(rep->height);
    result.rgba.resize(static_cast<std::size_t>(rep->width * rep->height * 4));
    for (std::int64_t row = 0; row < rep->height; ++row)
        for (std::int64_t x = 0; x < rep->width; ++x) {
            const auto value = pixels.get(x, row);
            for (unsigned index = 0; index < 4; ++index)
                result.rgba[static_cast<std::size_t>((row * rep->width + x) * 4 + index)] =
                    static_cast<std::uint8_t>(std::lround(std::clamp(value[index], 0.0, 1.0) * 255));
        }
    return result;
}
Address AppKit::image_from_pixels(const Image& source) {
    const auto rep = make_bitmap(source.width, source.height);
    const auto state = objc.host_state<BitmapState>(rep);
    memory().write_bytes(state->data, source.rgba);
    const auto result = objc.allocate(image);
    const auto info = objc.host_state<ImageState>(result);
    info->size = state->size;
    info->representations.push_back(rep);
    return result;
}

Address AppKit::color_object(double r, double g, double b, double a) {
    const auto value = objc.allocate(color);
    const auto state = objc.host_state<ColorState>(value);
    state->red = r; state->green = g; state->blue = b; state->alpha = a;
    return autorelease(value);
}

std::string AppKit::system_appearance_name() {
    if (host::ui::dark_appearance().value_or(false)) return "NSAppearanceNameDarkAqua";
    return "NSAppearanceNameAqua";
}
Address AppKit::appearance_named(std::string_view name) {
    static constexpr std::string_view known[] = {"NSAppearanceNameAqua", "NSAppearanceNameDarkAqua",
        "NSAppearanceNameVibrantLight", "NSAppearanceNameVibrantDark", "NSAppearanceNameAccessibilityAqua",
        "NSAppearanceNameAccessibilityDarkAqua"};
    if (std::find(std::begin(known), std::end(known), name) == std::end(known)) return 0;
    std::lock_guard lock(mutex);
    if (const auto found = appearances.find(std::string(name)); found != appearances.end()) return found->second;
    const auto value = objc.allocate(appearance);
    if (const auto record = process.objects().find(value)) record->immortal = true;
    objc.host_state<AppearanceState>(value)->name = string(name);
    appearances.emplace(std::string(name), value);
    return value;
}

std::string AppKit::host_path(std::string_view guest) {
    const auto path = process.map_guest_path ? process.map_guest_path(guest) : utf8_path(guest);
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// ---- screens -------------------------------------------------------------------------
Address AppKit::screen_object(ScreenId id) {
    const auto server = window_server_or_null();
    std::optional<ScreenInfo> current;
    if (server) for (const auto& info : server->screens()) if (info.id == id) current = info;
    std::lock_guard lock(mutex);
    if (const auto found = screens.find(id); found != screens.end()) {
        if (current) objc.host_state<ScreenState>(found->second)->info = *current;
        return found->second;
    }
    if (!current) return 0;
    const auto value = objc.allocate(screen);
    if (const auto record = process.objects().find(value)) record->immortal = true;
    objc.host_state<ScreenState>(value)->info = *current;
    screens.emplace(id, value);
    return value;
}
Address AppKit::main_screen() {
    const auto server = window_server_or_null();
    if (!server) return 0;
    Address key{};
    { std::lock_guard lock(mutex); key = key_window; }
    if (key && window_state(key)->id) return screen_object(server->window_screen(window_state(key)->id));
    const auto list = server->screens();
    return list.empty() ? 0 : screen_object(list.front().id);
}

// ---- pasteboards ---------------------------------------------------------------------
Address AppKit::pasteboard_named(std::string_view name) {
    std::lock_guard lock(mutex);
    if (const auto found = pasteboards.find(std::string(name)); found != pasteboards.end()) return found->second;
    const auto value = objc.allocate(pasteboard);
    if (const auto record = process.objects().find(value)) record->immortal = true;
    const auto state = objc.host_state<PasteboardState>(value);
    state->name = name;
    state->general = name == general_name;
    state->drag = name == drag_name;
    pasteboards.emplace(std::string(name), value);
    return value;
}
Address AppKit::general_pasteboard() { return pasteboard_named(general_name); }

// ---- cursors -------------------------------------------------------------------------
Address AppKit::make_cursor(SystemCursor kind) {
    std::lock_guard lock(mutex);
    if (const auto found = system_cursors.find(static_cast<int>(kind)); found != system_cursors.end()) return found->second;
    const auto value = objc.allocate(cursor);
    if (const auto record = process.objects().find(value)) record->immortal = true;
    objc.host_state<CursorState>(value)->system = kind;
    system_cursors.emplace(static_cast<int>(kind), value);
    return value;
}
void AppKit::set_cursor(Address value) {
    auto& server = require_window_server("-[NSCursor set]");
    const auto state = objc.host_state<CursorState>(value);
    if (!state->id) {
        if (state->system) {
            state->id = server.system_cursor(*state->system);
        } else {
            const auto pixels = state->image ? image_pixels(state->image) : std::nullopt;
            if (!pixels) fail("NSCursor image has no bitmap representation");
            state->id = server.create_cursor(*pixels, state->hot_spot);
        }
    }
    server.set_cursor(state->id);
    // The current cursor is retained (a custom cursor may be released by its
    // creator while it is shown); the previous one is released outside the lock.
    static_cast<void>(retain(value));
    Address previous{};
    {
        std::lock_guard lock(mutex);
        previous = std::exchange(current_cursor, value);
    }
    release(previous);
}

void AppKit::install_resources() {
    // ---- NSMenu ----
    const auto mn = menu;
    const auto menu_of = [this](Address value) { return objc.host_state<MenuState>(value); };
    const auto item_of = [this](Address value) { return objc.host_state<MenuItemState>(value); };
    method(mn, "initWithTitle:", [this, menu_of](CpuState& s, GuestMemory&) {
        menu_of(s.x[0])->title = s.x[2] ? cf.copy(s.x[2]) : string("");
    });
    method(mn, "init", [this](CpuState& s, GuestMemory&) { s.x[0] = send_id(s.x[0], "initWithTitle:", {auto_string("")}); });
    method(mn, "title", [this, menu_of](CpuState& s, GuestMemory&) {
        const auto title = menu_of(s.x[0])->title;
        s.x[0] = title ? title : auto_string("");
    });
    method(mn, "setTitle:", [this, menu_of](CpuState& s, GuestMemory&) {
        release(std::exchange(menu_of(s.x[0])->title, s.x[2] ? cf.copy(s.x[2]) : 0));
    }, "v@:@");
    const auto insert = [this, menu_of, item_of](Address self, Address item, std::size_t index) {
        if (!item) fail("-[NSMenu insertItem:atIndex:] needs an item");
        const auto entry = item_of(item);
        if (entry->menu) fail("NSMenuItem is already in a menu");
        auto& list = menu_of(self)->items;
        if (index > list.size()) fail("NSMenu index out of range");
        list.insert(list.begin() + static_cast<std::ptrdiff_t>(index), retain(item));
        entry->menu = self;
    };
    method(mn, "addItem:", [menu_of, insert](CpuState& s, GuestMemory&) { insert(s.x[0], s.x[2], menu_of(s.x[0])->items.size()); }, "v@:@");
    method(mn, "insertItem:atIndex:", [insert](CpuState& s, GuestMemory&) { insert(s.x[0], s.x[2], static_cast<std::size_t>(s.x[3])); }, "v@:@q");
    method(mn, "addItemWithTitle:action:keyEquivalent:", [this, menu_of, insert](CpuState& s, GuestMemory&) {
        const auto item = send(menu_item, "alloc").x0;
        const auto ready = send_id(item, "initWithTitle:action:keyEquivalent:", {s.x[2], s.x[3], s.x[4]});
        insert(s.x[0], ready, menu_of(s.x[0])->items.size());
        release(ready);
        s.x[0] = ready;   // owned by the menu
    }, "@@:@:@");
    const auto remove_at = [this, menu_of, item_of](Address self, std::size_t index) {
        auto& list = menu_of(self)->items;
        if (index >= list.size()) fail("NSMenu index out of range");
        const auto item = list[index];
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
        item_of(item)->menu = 0;
        release(item);
    };
    method(mn, "removeItemAtIndex:", [remove_at](CpuState& s, GuestMemory&) { remove_at(s.x[0], static_cast<std::size_t>(s.x[2])); }, "v@:q");
    method(mn, "removeItem:", [menu_of, remove_at](CpuState& s, GuestMemory&) {
        const auto& list = menu_of(s.x[0])->items;
        const auto found = std::find(list.begin(), list.end(), s.x[2]);
        if (found != list.end()) remove_at(s.x[0], static_cast<std::size_t>(found - list.begin()));
    }, "v@:@");
    method(mn, "removeAllItems", [menu_of, remove_at](CpuState& s, GuestMemory&) {
        while (!menu_of(s.x[0])->items.empty()) remove_at(s.x[0], menu_of(s.x[0])->items.size() - 1);
    }, "v@:");
    method(mn, "itemArray", [this, menu_of](CpuState& s, GuestMemory&) { s.x[0] = auto_array(menu_of(s.x[0])->items); });
    method(mn, "numberOfItems", [menu_of](CpuState& s, GuestMemory&) { s.x[0] = menu_of(s.x[0])->items.size(); }, "q@:");
    method(mn, "itemAtIndex:", [menu_of](CpuState& s, GuestMemory&) {
        const auto& list = menu_of(s.x[0])->items;
        s.x[0] = s.x[2] < list.size() ? list[static_cast<std::size_t>(s.x[2])] : 0;
    }, "@@:q");
    method(mn, "indexOfItem:", [menu_of](CpuState& s, GuestMemory&) {
        const auto& list = menu_of(s.x[0])->items;
        const auto found = std::find(list.begin(), list.end(), s.x[2]);
        s.x[0] = found == list.end() ? static_cast<Address>(-1) : static_cast<Address>(found - list.begin());
    }, "q@:@");
    method(mn, "itemWithTitle:", [this, menu_of, item_of](CpuState& s, GuestMemory&) {
        const auto wanted = text(s.x[2]);
        for (const auto item : menu_of(s.x[0])->items)
            if (text(item_of(item)->title) == wanted) { s.x[0] = item; return; }
        s.x[0] = 0;
    });
    method(mn, "setSubmenu:forItem:", [this](CpuState& s, GuestMemory&) { static_cast<void>(send_id(s.x[3], "setSubmenu:", {s.x[2]})); }, "v@:@@");
    method(mn, "supermenu", [menu_of](CpuState& s, GuestMemory&) { s.x[0] = menu_of(s.x[0])->supermenu; });
    method(mn, "autoenablesItems", [menu_of](CpuState& s, GuestMemory&) { return_bool(s, menu_of(s.x[0])->autoenables); }, "B@:");
    method(mn, "setAutoenablesItems:", [menu_of](CpuState& s, GuestMemory&) { menu_of(s.x[0])->autoenables = bool_arg(s, 2); }, "v@:B");
    method(mn, "performKeyEquivalent:", [this](CpuState& s, GuestMemory&) { return_bool(s, perform_key_equivalent(s.x[0], s.x[2])); }, "B@:@");
    // The menu is never displayed, so there is no item state to refresh.
    method(mn, "update", [](CpuState&, GuestMemory&) {}, "v@:");
    // Properties wxWidgets' wxNSMenu and Qt's QCocoaNSMenu set (Qt's menu is
    // its own delegate). The delegate is weak, as in AppKit; the menu is never
    // displayed, so the delegate's menuWillOpen:/menuNeedsUpdate: are not sent
    // and the presentation properties are only stored.
    method(mn, "delegate", [menu_of](CpuState& s, GuestMemory&) { s.x[0] = menu_of(s.x[0])->delegate; });
    method(mn, "setDelegate:", [menu_of](CpuState& s, GuestMemory&) { menu_of(s.x[0])->delegate = s.x[2]; }, "v@:@");
    method(mn, "font", [menu_of](CpuState& s, GuestMemory&) { s.x[0] = menu_of(s.x[0])->font; });
    method(mn, "setFont:", [this, menu_of](CpuState& s, GuestMemory&) { assign(menu_of(s.x[0])->font, s.x[2]); }, "v@:@");
    method(mn, "minimumWidth", [menu_of](CpuState& s, GuestMemory&) { set_d(s, 0, menu_of(s.x[0])->minimum_width); }, "d@:");
    method(mn, "setMinimumWidth:", [menu_of](CpuState& s, GuestMemory&) { menu_of(s.x[0])->minimum_width = d(s, 0); }, "v@:d");
    method(mn, "showsStateColumn", [menu_of](CpuState& s, GuestMemory&) { return_bool(s, menu_of(s.x[0])->shows_state_column); }, "B@:");
    method(mn, "setShowsStateColumn:", [menu_of](CpuState& s, GuestMemory&) { menu_of(s.x[0])->shows_state_column = bool_arg(s, 2); }, "v@:B");
    method(mn, "allowsContextMenuPlugIns", [menu_of](CpuState& s, GuestMemory&) { return_bool(s, menu_of(s.x[0])->allows_context_menu_plugins); }, "B@:");
    method(mn, "setAllowsContextMenuPlugIns:", [menu_of](CpuState& s, GuestMemory&) { menu_of(s.x[0])->allows_context_menu_plugins = bool_arg(s, 2); }, "v@:B");
    // No menu is ever being tracked here (popUpMenuPositioningItem: chooses
    // synchronously), so there is nothing to cancel and nothing highlighted.
    method(mn, "cancelTracking", [](CpuState&, GuestMemory&) {}, "v@:");
    method(mn, "cancelTrackingWithoutAnimation", [](CpuState&, GuestMemory&) {}, "v@:");
    method(mn, "highlightedItem", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    method(mn, "itemChanged:", [](CpuState&, GuestMemory&) {}, "v@:@");
    const auto index_where = [menu_of](Address self, const std::function<bool(Address)>& wanted) {
        const auto& list = menu_of(self)->items;
        for (std::size_t index = 0; index < list.size(); ++index)
            if (wanted(list[index])) return static_cast<std::int64_t>(index);
        return std::int64_t{-1};
    };
    method(mn, "indexOfItemWithTag:", [item_of, index_where](CpuState& s, GuestMemory&) {
        const auto tag = static_cast<std::int64_t>(s.x[2]);
        s.x[0] = static_cast<Address>(index_where(s.x[0], [&](Address item) { return item_of(item)->tag == tag; }));
    }, "q@:q");
    method(mn, "itemWithTag:", [menu_of, item_of, index_where](CpuState& s, GuestMemory&) {
        const auto tag = static_cast<std::int64_t>(s.x[2]);
        const auto index = index_where(s.x[0], [&](Address item) { return item_of(item)->tag == tag; });
        s.x[0] = index < 0 ? 0 : menu_of(s.x[0])->items[static_cast<std::size_t>(index)];
    }, "@@:q");
    method(mn, "indexOfItemWithSubmenu:", [item_of, index_where](CpuState& s, GuestMemory&) {
        const auto submenu = s.x[2];
        s.x[0] = static_cast<Address>(index_where(s.x[0], [&](Address item) { return submenu && item_of(item)->submenu == submenu; }));
    }, "q@:@");
    method(mn, "indexOfItemWithTitle:", [this, item_of, index_where](CpuState& s, GuestMemory&) {
        const auto wanted = text(s.x[2]);
        s.x[0] = static_cast<Address>(index_where(s.x[0], [&](Address item) { return text(item_of(item)->title) == wanted; }));
    }, "q@:@");
    method(mn, "indexOfItemWithRepresentedObject:", [this, item_of, index_where](CpuState& s, GuestMemory&) {
        const auto wanted = s.x[2];
        s.x[0] = static_cast<Address>(index_where(s.x[0], [&](Address item) {
            const auto value = item_of(item)->represented_object;
            return value == wanted || (value && wanted && send_bool(value, "isEqual:", {wanted}));
        }));
    }, "q@:@");
    // A NULL action matches any item of the target.
    method(mn, "indexOfItemWithTarget:andAction:", [item_of, index_where](CpuState& s, GuestMemory&) {
        const auto target = s.x[2], action = s.x[3];
        s.x[0] = static_cast<Address>(index_where(s.x[0], [&](Address item) {
            const auto entry = item_of(item);
            return entry->target == target && (!action || entry->action == action);
        }));
    }, "q@:@:");
    // Sends the item's action as a click would, between the will/did send notifications.
    method(mn, "performActionForItemAtIndex:", [this, menu_of, item_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto& list = menu_of(self)->items;
        if (s.x[2] >= list.size()) fail("-[NSMenu performActionForItemAtIndex:] index beyond the items (NSRangeException)");
        const auto item = list[s.x[2]];
        const auto entry = item_of(item);
        const auto key = string("MenuItem");
        const std::vector<std::pair<Address, Address>> entries{{key, item}};
        const auto info = autorelease(cf.dictionary(entries));
        release(key);
        post("NSMenuWillSendActionNotification", self, 0, {}, info);
        if (entry->action) static_cast<void>(send_action(entry->action, entry->target, item));
        post("NSMenuDidSendActionNotification", self, 0, {}, info);
    }, "v@:q");
    // NSCopying: a menu of the receiver's class with the same title, flags,
    // font and (weak) delegate, holding a copy of every item (-copyWithZone:
    // of each, so their submenus are copied too). The copy has no supermenu.
    method(mn, "copyWithZone:", [this, menu_of, item_of, insert](CpuState& s, GuestMemory&) {
        const auto from = menu_of(s.x[0]);
        const auto copy = objc.allocate(objc.object_class(s.x[0]));
        auto to = std::make_shared<MenuState>(*from);
        to->items.clear();
        to->supermenu = 0;
        to->title = retain(to->title);
        to->font = retain(to->font);
        static_cast<void>(objc.host_state<MenuState>(copy, [&] { return to; }));
        const auto originals = from->items;
        for (const auto item : originals) {
            const auto duplicate = send_id(item, "copyWithZone:", {s.x[2]});
            insert(copy, duplicate, menu_of(copy)->items.size());
            release(duplicate);
            if (const auto submenu = item_of(duplicate)->submenu) menu_of(submenu)->supermenu = copy;
        }
        object_copied(s.x[0], copy);
        s.x[0] = copy;
    }, "@@:^v");
    method(mn, "dealloc", [this](CpuState& s, GuestMemory&) {
        object_deallocated(s.x[0]);
        if (const auto state = objc.find_host_state<MenuState>(s.x[0])) {
            for (const auto item : std::exchange(state->items, {})) {
                objc.host_state<MenuItemState>(item)->menu = 0;
                release(item);
            }
            release(std::exchange(state->title, 0));
            release(std::exchange(state->font, 0));
        }
        static_cast<void>(objc.send_super(s.x[0], menu, sel("dealloc")));
    }, "v@:");

    // ---- NSMenuItem ----
    const auto mi = menu_item;
    method(mi, "initWithTitle:action:keyEquivalent:", [this, item_of](CpuState& s, GuestMemory&) {
        const auto entry = item_of(s.x[0]);
        entry->title = s.x[2] ? cf.copy(s.x[2]) : string("");
        entry->action = s.x[3];
        entry->key_equivalent = s.x[4] ? cf.copy(s.x[4]) : string("");
    }, "@@:@:@");
    method(mi, "init", [this](CpuState& s, GuestMemory&) {
        s.x[0] = send_id(s.x[0], "initWithTitle:action:keyEquivalent:", {auto_string(""), 0, auto_string("")});
    });
    class_method(mi, "separatorItem", [this, item_of](CpuState& s, GuestMemory&) {
        const auto item = alloc_init(menu_item);
        item_of(item)->separator = true;
        item_of(item)->enabled = false;
        s.x[0] = autorelease(item);
    });
    const auto string_property = [this, item_of](Address MenuItemState::* field, const char* getter, const char* setter) {
        method(menu_item, getter, [this, item_of, field](CpuState& s, GuestMemory&) {
            const auto value = (*item_of(s.x[0])).*field;
            s.x[0] = value ? value : auto_string("");
        });
        method(menu_item, setter, [this, item_of, field](CpuState& s, GuestMemory&) {
            release(std::exchange((*item_of(s.x[0])).*field, s.x[2] ? cf.copy(s.x[2]) : 0));
        }, "v@:@");
    };
    string_property(&MenuItemState::title, "title", "setTitle:");
    string_property(&MenuItemState::key_equivalent, "keyEquivalent", "setKeyEquivalent:");
    method(mi, "keyEquivalentModifierMask", [item_of](CpuState& s, GuestMemory&) { s.x[0] = item_of(s.x[0])->modifier_mask; }, "Q@:");
    method(mi, "setKeyEquivalentModifierMask:", [item_of](CpuState& s, GuestMemory&) { item_of(s.x[0])->modifier_mask = s.x[2]; }, "v@:Q");
    method(mi, "action", [item_of](CpuState& s, GuestMemory&) { s.x[0] = item_of(s.x[0])->action; }, ":@:");
    method(mi, "setAction:", [item_of](CpuState& s, GuestMemory&) { item_of(s.x[0])->action = s.x[2]; }, "v@::");
    method(mi, "target", [item_of](CpuState& s, GuestMemory&) { s.x[0] = item_of(s.x[0])->target; });
    method(mi, "setTarget:", [item_of](CpuState& s, GuestMemory&) { item_of(s.x[0])->target = s.x[2]; }, "v@:@");
    method(mi, "submenu", [item_of](CpuState& s, GuestMemory&) { s.x[0] = item_of(s.x[0])->submenu; });
    method(mi, "setSubmenu:", [this, item_of, menu_of](CpuState& s, GuestMemory&) {
        const auto entry = item_of(s.x[0]);
        if (entry->submenu) menu_of(entry->submenu)->supermenu = 0;
        assign(entry->submenu, s.x[2]);
        if (s.x[2]) menu_of(s.x[2])->supermenu = entry->menu;
    }, "v@:@");
    method(mi, "hasSubmenu", [item_of](CpuState& s, GuestMemory&) { return_bool(s, item_of(s.x[0])->submenu != 0); }, "B@:");
    method(mi, "menu", [item_of](CpuState& s, GuestMemory&) { s.x[0] = item_of(s.x[0])->menu; });
    // The menu that holds the item (not retained); the menu sets it on insertion.
    method(mi, "setMenu:", [item_of](CpuState& s, GuestMemory&) { item_of(s.x[0])->menu = s.x[2]; }, "v@:@");
    // The item whose submenu holds this one.
    method(mi, "parentItem", [this, item_of](CpuState& s, GuestMemory&) {
        const auto holder = item_of(s.x[0])->menu;
        s.x[0] = 0;
        const auto super_menu = holder ? objc.host_state<MenuState>(holder)->supermenu : 0;
        if (!super_menu) return;
        for (const auto candidate : objc.host_state<MenuState>(super_menu)->items)
            if (item_of(candidate)->submenu == holder) { s.x[0] = candidate; return; }
    });
    method(mi, "isHiddenOrHasHiddenAncestor", [this, item_of](CpuState& s, GuestMemory&) {
        bool hidden{};
        for (auto item = s.x[0]; item && !hidden; item = send(item, "parentItem").x0) hidden = item_of(item)->hidden;
        return_bool(s, hidden);
    }, "B@:");
    method(mi, "isSeparatorItem", [item_of](CpuState& s, GuestMemory&) { return_bool(s, item_of(s.x[0])->separator); }, "B@:");
    method(mi, "isEnabled", [item_of](CpuState& s, GuestMemory&) { return_bool(s, item_of(s.x[0])->enabled); }, "B@:");
    method(mi, "setEnabled:", [item_of](CpuState& s, GuestMemory&) { item_of(s.x[0])->enabled = bool_arg(s, 2); }, "v@:B");
    method(mi, "isHidden", [item_of](CpuState& s, GuestMemory&) { return_bool(s, item_of(s.x[0])->hidden); }, "B@:");
    method(mi, "setHidden:", [item_of](CpuState& s, GuestMemory&) { item_of(s.x[0])->hidden = bool_arg(s, 2); }, "v@:B");
    method(mi, "state", [item_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(item_of(s.x[0])->state); }, "q@:");
    method(mi, "setState:", [item_of](CpuState& s, GuestMemory&) { item_of(s.x[0])->state = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    method(mi, "tag", [item_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(item_of(s.x[0])->tag); }, "q@:");
    method(mi, "setTag:", [item_of](CpuState& s, GuestMemory&) { item_of(s.x[0])->tag = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    method(mi, "representedObject", [item_of](CpuState& s, GuestMemory&) { s.x[0] = item_of(s.x[0])->represented_object; });
    method(mi, "setRepresentedObject:", [this, item_of](CpuState& s, GuestMemory&) { assign(item_of(s.x[0])->represented_object, s.x[2]); }, "v@:@");
    // Presentation properties Qt's QCocoaMenuItem syncs. Menus are never
    // displayed here, so they are stored and returned only.
    const auto item_object = [this, item_of](const char* getter, const char* setter, Address MenuItemState::*member, bool copy) {
        method(menu_item, getter, [item_of, member](CpuState& s, GuestMemory&) { s.x[0] = (*item_of(s.x[0])).*member; });
        method(menu_item, setter, [this, item_of, member, copy](CpuState& s, GuestMemory&) {
            auto& slot = (*item_of(s.x[0])).*member;
            if (copy) release(std::exchange(slot, s.x[2] ? cf.copy(s.x[2]) : 0));
            else assign(slot, s.x[2]);
        }, "v@:@");
    };
    item_object("toolTip", "setToolTip:", &MenuItemState::tool_tip, true);
    item_object("image", "setImage:", &MenuItemState::image, false);
    item_object("attributedTitle", "setAttributedTitle:", &MenuItemState::attributed_title, true);
    item_object("view", "setView:", &MenuItemState::view, false);
    method(mi, "isAlternate", [item_of](CpuState& s, GuestMemory&) { return_bool(s, item_of(s.x[0])->alternate); }, "B@:");
    method(mi, "setAlternate:", [item_of](CpuState& s, GuestMemory&) { item_of(s.x[0])->alternate = bool_arg(s, 2); }, "v@:B");
    method(mi, "indentationLevel", [item_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(item_of(s.x[0])->indentation); }, "q@:");
    method(mi, "setIndentationLevel:", [item_of](CpuState& s, GuestMemory&) {
        item_of(s.x[0])->indentation = std::clamp<std::int64_t>(static_cast<std::int64_t>(s.x[2]), 0, 15);
    }, "v@:q");
    // NSCopying: an item of the receiver's class in no menu, with the same
    // title, key equivalent and mask, action and target, tag, state, flags,
    // indentation, image, attributed title, tool tip and represented object
    // (shared, retained) and view (shared: a view cannot be copied), and a
    // copy of its submenu.
    method(mi, "copyWithZone:", [this, item_of, menu_of](CpuState& s, GuestMemory&) {
        const auto from = item_of(s.x[0]);
        const auto copy = objc.allocate(objc.object_class(s.x[0]));
        auto to = std::make_shared<MenuItemState>(*from);
        to->menu = 0;
        for (auto* slot : {&to->title, &to->key_equivalent, &to->represented_object, &to->tool_tip, &to->image, &to->attributed_title, &to->view})
            *slot = retain(*slot);
        to->submenu = 0;
        static_cast<void>(objc.host_state<MenuItemState>(copy, [&] { return to; }));
        if (from->submenu) {
            to->submenu = send_id(from->submenu, "copyWithZone:", {s.x[2]});
            menu_of(to->submenu)->supermenu = 0;   // the copy is in no menu yet
        }
        object_copied(s.x[0], copy);
        s.x[0] = copy;
    }, "@@:^v");
    method(mi, "dealloc", [this](CpuState& s, GuestMemory&) {
        object_deallocated(s.x[0]);
        if (const auto entry = objc.find_host_state<MenuItemState>(s.x[0]))
            for (auto* slot : {&entry->title, &entry->key_equivalent, &entry->submenu, &entry->represented_object,
                               &entry->tool_tip, &entry->image, &entry->attributed_title, &entry->view})
                release(std::exchange(*slot, 0));
        static_cast<void>(objc.send_super(s.x[0], menu_item, sel("dealloc")));
    }, "v@:");

    // ---- NSPasteboard ----
    const auto pb = pasteboard;
    const auto board = [this](Address value) { return objc.host_state<PasteboardState>(value); };
    const auto available = [this](Address value) {
        const auto state = objc.host_state<PasteboardState>(value);
        std::vector<std::string> types = state->declared;
        const auto add = [&](std::string_view type) {
            if (std::find(types.begin(), types.end(), type) == types.end()) types.emplace_back(type);
        };
        if (state->general) {
            if (const auto server = window_server_or_null()) {
                if (server->clipboard_text()) { add("public.utf8-plain-text"); add("NSStringPboardType"); }
                if (server->clipboard_image()) { add("public.tiff"); add("public.png"); add("NSTIFFPboardType"); }
            }
        } else if (!state->drag) {
            for (const auto& [type, value_text] : state->strings) { static_cast<void>(value_text); add(type); }
        }
        return types;
    };
    const auto change_count = [this, board](Address value) -> std::int64_t {
        const auto state = board(value);
        if (state->general) if (const auto server = window_server_or_null()) return server->clipboard_change_count();
        return state->change_count;
    };
    class_method(pb, "generalPasteboard", [this](CpuState& s, GuestMemory&) { s.x[0] = general_pasteboard(); });
    class_method(pb, "pasteboardWithName:", [this](CpuState& s, GuestMemory&) { s.x[0] = pasteboard_named(text(s.x[2])); });
    class_method(pb, "pasteboardWithUniqueName", [this](CpuState& s, GuestMemory&) {
        s.x[0] = pasteboard_named("Alchemy unique pasteboard " + std::to_string(++unique_pasteboards));
    });
    method(pb, "name", [this, board](CpuState& s, GuestMemory&) { s.x[0] = auto_string(board(s.x[0])->name); });
    method(pb, "changeCount", [change_count](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(change_count(s.x[0])); }, "q@:");
    method(pb, "clearContents", [board, change_count](CpuState& s, GuestMemory&) {
        const auto state = board(s.x[0]);
        state->declared.clear(); state->strings.clear(); state->file_paths.clear();
        ++state->change_count;
        s.x[0] = static_cast<Address>(change_count(s.x[0]));
    }, "q@:");
    // A fresh start of the contents (the options only restrict where they
    // may go: the current host is the only one).
    method(pb, "prepareForNewContentsWithOptions:", [this](CpuState& s, GuestMemory&) { s.x[0] = send(s.x[0], "clearContents").x0; }, "q@:Q");
    // The contents go away with the pasteboard's resources.
    method(pb, "releaseGlobally", [this, board](CpuState& s, GuestMemory&) {
        if (!board(s.x[0])->general) static_cast<void>(send(s.x[0], "clearContents"));
    }, "v@:");
    // Windows lets every application read the clipboard without asking:
    // NSPasteboardAccessBehaviorAlwaysAllow.
    method(pb, "accessBehavior", [](CpuState& s, GuestMemory&) { s.x[0] = 2; }, "q@:");
    // Data providers: each type is declared, and the provider writes its
    // data at once (-pasteboard:provideDataForType:), which is what a
    // reader would get from it.
    method(pb, "setDataProvider:forTypes:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], provider = s.x[2], types = s.x[3];
        if (!provider || !responds(provider, "pasteboard:provideDataForType:")) fail("-[NSPasteboard setDataProvider:forTypes:] without a data provider");
        static_cast<void>(send_id(self, "addTypes:owner:", {types, 0}));
        for (const auto type : items(types)) static_cast<void>(send_id(provider, "pasteboard:provideDataForType:", {self, type}));
        s.x[0] = 1;
    }, "B@:@@");
    method(pb, "declareTypes:owner:", [this, board, change_count](CpuState& s, GuestMemory&) {
        const auto state = board(s.x[0]);
        state->declared.clear(); state->strings.clear();
        for (const auto value : items(s.x[2])) state->declared.push_back(text(value));
        ++state->change_count;
        s.x[0] = static_cast<Address>(change_count(s.x[0]));
    }, "q@:@@");
    method(pb, "addTypes:owner:", [this, board, change_count](CpuState& s, GuestMemory&) {
        const auto state = board(s.x[0]);
        for (const auto value : items(s.x[2])) state->declared.push_back(text(value));
        s.x[0] = static_cast<Address>(change_count(s.x[0]));
    }, "q@:@@");
    method(pb, "types", [this, available](CpuState& s, GuestMemory&) {
        std::vector<Address> names;
        for (const auto& type : available(s.x[0])) names.push_back(auto_string(type));
        s.x[0] = auto_array(names);
    });
    method(pb, "availableTypeFromArray:", [this, available](CpuState& s, GuestMemory&) {
        const auto types = available(s.x[0]);
        for (const auto value : items(s.x[2]))
            if (std::find(types.begin(), types.end(), text(value)) != types.end()) { s.x[0] = value; return; }
        s.x[0] = 0;
    });
    const auto write_string = [this, board](Address self, std::string_view type, std::string_view value) {
        const auto state = board(self);
        if (state->drag) return false;
        if (state->general) {
            if (!string_type(type)) return false;
            require_window_server("-[NSPasteboard setString:forType:]").set_clipboard_text(value);
        } else {
            state->strings[std::string(type)] = value;
        }
        if (std::find(state->declared.begin(), state->declared.end(), type) == state->declared.end()) state->declared.emplace_back(type);
        return true;
    };
    const auto read_string = [this, board](Address self, std::string_view type) -> std::optional<std::string> {
        const auto state = board(self);
        if (state->drag) {
            if (type == "public.file-url" && !state->file_paths.empty()) return "file://" + percent_encode_path(state->file_paths.front());
            return std::nullopt;
        }
        if (state->general) {
            if (!string_type(type)) return std::nullopt;
            const auto server = window_server_or_null();
            return server ? server->clipboard_text() : std::nullopt;
        }
        const auto found = state->strings.find(std::string(type));
        if (found == state->strings.end()) return std::nullopt;
        return found->second;
    };
    method(pb, "setString:forType:", [this, write_string](CpuState& s, GuestMemory&) {
        return_bool(s, s.x[2] && write_string(s.x[0], text(s.x[3]), text(s.x[2])));
    }, "B@:@@");
    method(pb, "stringForType:", [this, read_string](CpuState& s, GuestMemory&) {
        const auto value = read_string(s.x[0], text(s.x[2]));
        s.x[0] = value ? auto_string(*value) : 0;
    });
    method(pb, "propertyListForType:", [this, board, read_string](CpuState& s, GuestMemory&) {
        const auto type = text(s.x[2]);
        const auto state = board(s.x[0]);
        if (type == "NSFilenamesPboardType") {
            if (state->file_paths.empty()) { s.x[0] = 0; return; }
            std::vector<Address> names;
            for (const auto& path : state->file_paths) names.push_back(auto_string(path));
            s.x[0] = auto_array(names);
            return;
        }
        const auto value = read_string(s.x[0], type);
        s.x[0] = value ? auto_string(*value) : 0;
    });
    method(pb, "setPropertyList:forType:", [this, write_string](CpuState& s, GuestMemory&) {
        const auto type = text(s.x[3]);
        if (!s.x[2] || cf.kind(s.x[2]) != ObjectKind::string) unsupported("-[NSPasteboard setPropertyList:forType:] with a non-string property list");
        return_bool(s, write_string(s.x[0], type, text(s.x[2])));
    }, "B@:@@");
    method(pb, "dataForType:", [this, board, read_string](CpuState& s, GuestMemory&) {
        const auto type = text(s.x[2]);
        if (image_type(type)) {
            // The clipboard image in the requested file format.
            const auto server = board(s.x[0])->general ? window_server_or_null() : nullptr;
            const auto pixels = server ? server->clipboard_image() : std::nullopt;
            const auto bytes = pixels ? encode_image(*pixels, type.find("png") != std::string::npos || type.find("PNG") != std::string::npos
                                                                   ? ImageFileType::png : ImageFileType::tiff)
                                      : std::nullopt;
            s.x[0] = bytes ? make_data(*bytes) : 0;
            return;
        }
        const auto value = read_string(s.x[0], type);
        s.x[0] = value ? make_data(std::span(reinterpret_cast<const std::uint8_t*>(value->data()), value->size())) : 0;
    });
    method(pb, "setData:forType:", [this, board, write_string](CpuState& s, GuestMemory&) {
        const auto type = text(s.x[3]);
        if (!s.x[2]) { s.x[0] = 0; return; }
        const auto bytes = data_bytes(s.x[2]);
        if (image_type(type)) {
            const auto decoded = board(s.x[0])->general ? decode_image(bytes) : std::nullopt;
            if (!decoded) { s.x[0] = 0; return; }
            require_window_server("-[NSPasteboard setData:forType:]").set_clipboard_image(decoded->pixels);
            s.x[0] = 1;
            return;
        }
        return_bool(s, write_string(s.x[0], type, std::string(bytes.begin(), bytes.end())));
    }, "B@:@@");
    const auto read_objects = [this, board, read_string](Address self, Address classes) {
        std::vector<Address> found;
        const auto state = board(self);
        const auto url_class = objc.find_class("NSURL"), string_class = objc.find_class("NSString");
        for (const auto wanted : items(classes)) {
            if (wanted == url_class) {
                for (const auto& path : state->file_paths) found.push_back(autorelease(cf.file_url(path, false)));
            } else if (wanted == string_class) {
                if (const auto value = read_string(self, "public.utf8-plain-text")) found.push_back(auto_string(*value));
            } else if (wanted == image) {
                if (state->general) if (const auto server = window_server_or_null())
                    if (const auto pixels = server->clipboard_image()) found.push_back(autorelease(image_from_pixels(*pixels)));
            }
            if (!found.empty()) break;
        }
        return found;
    };
    method(pb, "readObjectsForClasses:options:", [this, read_objects](CpuState& s, GuestMemory&) {
        const auto found = read_objects(s.x[0], s.x[2]);
        s.x[0] = found.empty() ? 0 : auto_array(found);
    }, "@@:@@");
    method(pb, "canReadObjectForClasses:options:", [read_objects](CpuState& s, GuestMemory&) {
        return_bool(s, !read_objects(s.x[0], s.x[2]).empty());
    }, "B@:@@");
    method(pb, "writeObjects:", [this, board, write_string](CpuState& s, GuestMemory&) {
        bool written = true;
        for (const auto value : items(s.x[2])) {
            if (cf.kind(value) == ObjectKind::string) {
                written = write_string(s.x[0], "public.utf8-plain-text", text(value)) && written;
            } else if (is_kind(value, image)) {
                const auto pixels = image_pixels(value);
                if (!pixels || !board(s.x[0])->general) { written = false; continue; }
                require_window_server("-[NSPasteboard writeObjects:]").set_clipboard_image(*pixels);
            } else if (cf.kind(value) == ObjectKind::url) {
                written = write_string(s.x[0], "public.utf8-plain-text", cf.url_path(value)) && written;
            } else {
                written = false;
            }
        }
        return_bool(s, written);
    }, "B@:@");

    // ---- NSCursor ----
    const auto cu = cursor;
    for (const auto& [name, kind] : std::initializer_list<std::pair<const char*, SystemCursor>>{
             {"arrowCursor", SystemCursor::arrow}, {"IBeamCursor", SystemCursor::ibeam},
             {"crosshairCursor", SystemCursor::crosshair}, {"closedHandCursor", SystemCursor::closed_hand},
             {"openHandCursor", SystemCursor::open_hand}, {"pointingHandCursor", SystemCursor::pointing_hand},
             {"resizeLeftCursor", SystemCursor::resize_left}, {"resizeRightCursor", SystemCursor::resize_right},
             {"resizeLeftRightCursor", SystemCursor::resize_left_right}, {"resizeUpCursor", SystemCursor::resize_up},
             {"resizeDownCursor", SystemCursor::resize_down}, {"resizeUpDownCursor", SystemCursor::resize_up_down},
             {"disappearingItemCursor", SystemCursor::disappearing_item},
             {"operationNotAllowedCursor", SystemCursor::operation_not_allowed},
             {"dragLinkCursor", SystemCursor::drag_link}, {"dragCopyCursor", SystemCursor::drag_copy},
             {"contextualMenuCursor", SystemCursor::contextual_menu},
             {"IBeamCursorForVerticalLayout", SystemCursor::ibeam_vertical},
             {"busyButClickableCursor", SystemCursor::wait},
             // Column and row resizing use the horizontal and vertical resize shapes.
             {"columnResizeCursor", SystemCursor::resize_left_right}, {"rowResizeCursor", SystemCursor::resize_up_down}}) {
        const auto value = kind;
        class_method(cu, name, [this, value](CpuState& s, GuestMemory&) { s.x[0] = make_cursor(value); });
    }
    // Cursors Windows has no system shape for, drawn once as image cursors
    // (24 x 24, black on white): `key` names each beside the SystemCursor
    // values in system_cursors.
    const auto drawn_cursor = [this](int key, const std::function<void(const std::function<void(int, int, std::uint8_t)>&)>& draw,
                                     Point hot_spot) {
        {
            std::lock_guard lock(mutex);
            if (const auto found = system_cursors.find(key); found != system_cursors.end()) return found->second;
        }
        constexpr std::uint32_t size = 24;
        Image pixels{size, size, std::vector<std::uint8_t>(size * size * 4, 0)};
        draw([&](int x, int y, std::uint8_t value) {
            if (x < 0 || y < 0 || x >= static_cast<int>(size) || y >= static_cast<int>(size)) return;
            auto* p = &pixels.rgba[(static_cast<std::size_t>(y) * size + static_cast<std::size_t>(x)) * 4];
            p[0] = p[1] = p[2] = value;
            p[3] = 255;
        });
        const auto picture = image_from_pixels(pixels);
        CallRegs registers;
        registers.x[2] = picture;
        put_point(registers, 0, hot_spot);
        const auto value = send(send(cursor, "alloc").x0, "initWithImage:hotSpot:", registers).x0;
        release(picture);
        if (const auto record = process.objects().find(value)) record->immortal = true;
        std::lock_guard lock(mutex);
        return system_cursors.emplace(key, value).first->second;
    };
    // A double-headed diagonal arrow: from top-left to bottom-right, or
    // mirrored from top-right to bottom-left.
    const auto diagonal_cursor = [drawn_cursor](bool falling) {
        return drawn_cursor(falling ? -3 : -4, [falling](const std::function<void(int, int, std::uint8_t)>& plot) {
            const auto mirror = [falling](int x) { return falling ? x : 23 - x; };
            for (const std::uint8_t value : {std::uint8_t{255}, std::uint8_t{0}}) {
                const int grow = value == 255 ? 1 : 0;   // a white outline under the black arrow
                for (int i = 4 - grow; i <= 19 + grow; ++i)
                    for (int w = -grow; w <= 1 + grow; ++w) plot(mirror(i + w), i, value);
                for (int i = 0; i <= 5 + grow; ++i)
                    for (int w = -grow; w <= grow; ++w) {
                        plot(mirror(3 + i + w), 3, value); plot(mirror(3 + w), 3 + i, value);       // the top-left head
                        plot(mirror(20 - i + w), 20, value); plot(mirror(20 + w), 20 - i, value);   // the bottom-right head
                    }
            }
        }, {11.5, 11.5});
    };
    // +frameResizeCursorFromPosition:inDirections: (NSCursorFrameResizePosition
    // top 1, left 2, bottom 4, right 8; NSCursorFrameResizeDirections inward 1,
    // outward 2): edges get the arrow of the direction, corners the diagonal shape.
    class_method(cu, "frameResizeCursorFromPosition:inDirections:", [this, diagonal_cursor](CpuState& s, GuestMemory&) {
        constexpr std::uint64_t top = 1, left = 2, bottom = 4, right = 8, inward = 1, outward = 2;
        const auto position = s.x[2], directions = s.x[3] & (inward | outward);
        const bool vertical_edge = (position & (top | bottom)) != 0, horizontal_edge = (position & (left | right)) != 0;
        if (vertical_edge && horizontal_edge) {
            s.x[0] = diagonal_cursor(((position & top) != 0) == ((position & left) != 0));
            return;
        }
        SystemCursor kind{};
        if (vertical_edge) {
            const bool grows_up = (position & top) != 0;   // outward from the top edge is up
            kind = directions == outward ? (grows_up ? SystemCursor::resize_up : SystemCursor::resize_down)
                 : directions == inward ? (grows_up ? SystemCursor::resize_down : SystemCursor::resize_up)
                                        : SystemCursor::resize_up_down;
        } else if (horizontal_edge) {
            const bool grows_left = (position & left) != 0;
            kind = directions == outward ? (grows_left ? SystemCursor::resize_left : SystemCursor::resize_right)
                 : directions == inward ? (grows_left ? SystemCursor::resize_right : SystemCursor::resize_left)
                                        : SystemCursor::resize_left_right;
        } else {
            fail("+[NSCursor frameResizeCursorFromPosition:inDirections:] with no edge");
        }
        s.x[0] = make_cursor(kind);
    });
    // Zoom in / out: a magnifying glass with a plus or a minus.
    for (const bool zoom_in : {true, false}) {
        class_method(cu, zoom_in ? "zoomInCursor" : "zoomOutCursor", [drawn_cursor, zoom_in](CpuState& s, GuestMemory&) {
            constexpr double cx = 9.5, cy = 9.5;
            s.x[0] = drawn_cursor(zoom_in ? -1 : -2, [zoom_in](const std::function<void(int, int, std::uint8_t)>& plot) {
                for (int y = 0; y < 24; ++y)
                    for (int x = 0; x < 24; ++x) {
                        const double r = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
                        if (r <= 8.5) plot(x, y, r >= 6.0 ? 0 : 255);                    // the ring over a white lens
                        const double along = (x + y + 1 - 2 * cx) / 2.0, across = std::abs(x - y) / 1.4142;
                        if (along > 6.5 && along < 13.0 && across <= 1.8) plot(x, y, 0);  // the handle
                    }
                for (int i = 6; i <= 13; ++i) {
                    plot(i, 9, 0); plot(i, 10, 0);                                    // minus
                    if (zoom_in) { plot(9, i, 0); plot(10, i, 0); }                   // plus
                }
            }, {cx, cy});
        });
    }
    const auto custom = [this](Address self, Address picture, Point hot_spot) {
        const auto state = objc.host_state<CursorState>(self);
        state->image = retain(picture);
        state->hot_spot = hot_spot;
        return self;
    };
    method(cu, "initWithImage:hotSpot:", [custom](CpuState& s, GuestMemory&) { s.x[0] = custom(s.x[0], s.x[2], point_arg(s)); },
           "@@:@{CGPoint=dd}");
    method(cu, "initWithImage:foregroundColorHint:backgroundColorHint:hotSpot:", [custom](CpuState& s, GuestMemory&) {
        s.x[0] = custom(s.x[0], s.x[2], point_arg(s));
    }, "@@:@@@{CGPoint=dd}");
    method(cu, "image", [this](CpuState& s, GuestMemory&) { s.x[0] = objc.host_state<CursorState>(s.x[0])->image; });
    method(cu, "hotSpot", [this](CpuState& s, GuestMemory&) { return_point(s, objc.host_state<CursorState>(s.x[0])->hot_spot); }, "{CGPoint=dd}@:");
    method(cu, "set", [this](CpuState& s, GuestMemory&) { set_cursor(s.x[0]); }, "v@:");
    // -push / -pop set the cursor with -set (a subclass may override it).
    // The stack holds +1 references to the cursors to restore.
    method(cu, "push", [this](CpuState& s, GuestMemory&) {
        Address previous{};
        {
            std::lock_guard lock(mutex);
            previous = current_cursor;
        }
        static_cast<void>(retain(previous));
        {
            std::lock_guard lock(mutex);
            cursor_stack.push_back(previous);
        }
        static_cast<void>(send(s.x[0], "set"));
    }, "v@:");
    const auto pop = [this](CpuState&, GuestMemory&) {
        Address previous{};
        {
            std::lock_guard lock(mutex);
            if (cursor_stack.empty()) return;
            previous = cursor_stack.back();
            cursor_stack.pop_back();
        }
        try {
            static_cast<void>(send(previous ? previous : make_cursor(SystemCursor::arrow), "set"));
        } catch (...) { release(previous); throw; }
        release(previous);
    };
    method(cu, "pop", pop, "v@:");
    class_method(cu, "pop", pop, "v@:");
    for (const auto* name : {"currentCursor", "currentSystemCursor"})
        class_method(cu, name, [this](CpuState& s, GuestMemory&) {
            Address value{};
            { std::lock_guard lock(mutex); value = current_cursor; }
            s.x[0] = value ? value : make_cursor(SystemCursor::arrow);
        });
    class_method(cu, "hide", [](CpuState&, GuestMemory&) { require_window_server("+[NSCursor hide]").set_cursor_visible(false); }, "v@:");
    class_method(cu, "unhide", [](CpuState&, GuestMemory&) { require_window_server("+[NSCursor unhide]").set_cursor_visible(true); }, "v@:");
    class_method(cu, "setHiddenUntilMouseMoves:", [this](CpuState& s, GuestMemory&) {
        const bool hide = bool_arg(s, 2);
        if (hide == cursor_hidden_until_moved) return;
        cursor_hidden_until_moved = hide;
        require_window_server("+[NSCursor setHiddenUntilMouseMoves:]").set_cursor_visible(!hide);
    }, "v@:B");
    method(cu, "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<CursorState>(s.x[0])) {
            if (state->id && !state->system) if (const auto server = window_server_or_null()) server->destroy_cursor(state->id);
            release(std::exchange(state->image, 0));
        }
        static_cast<void>(objc.send_super(s.x[0], cursor, sel("dealloc")));
    }, "v@:");

    // ---- NSImage ----
    const auto im = image;
    const auto image_of = [this](Address value) { return objc.host_state<ImageState>(value); };
    method(im, "initWithSize:", [image_of](CpuState& s, GuestMemory&) { image_of(s.x[0])->size = size_arg(s); }, "@@:{CGSize=dd}");
    method(im, "size", [image_of](CpuState& s, GuestMemory&) { return_size(s, image_of(s.x[0])->size); }, "{CGSize=dd}@:");
    method(im, "setSize:", [image_of](CpuState& s, GuestMemory&) { image_of(s.x[0])->size = size_arg(s); }, "v@:{CGSize=dd}");
    method(im, "addRepresentation:", [this, image_of](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;
        const auto state = image_of(s.x[0]);
        state->representations.push_back(retain(s.x[2]));
        if (rect_empty({{0, 0}, state->size}) && is_kind(s.x[2], bitmap_rep)) state->size = objc.host_state<BitmapState>(s.x[2])->size;
    }, "v@:@");
    method(im, "removeRepresentation:", [this, image_of](CpuState& s, GuestMemory&) {
        auto& list = image_of(s.x[0])->representations;
        const auto found = std::find(list.begin(), list.end(), s.x[2]);
        if (found == list.end()) return;
        list.erase(found);
        release(s.x[2]);
    }, "v@:@");
    method(im, "representations", [this, image_of](CpuState& s, GuestMemory&) { s.x[0] = auto_array(image_of(s.x[0])->representations); });
    method(im, "isValid", [image_of](CpuState& s, GuestMemory&) {
        const auto state = image_of(s.x[0]);
        return_bool(s, !state->representations.empty() || !rect_empty({{0, 0}, state->size}));
    }, "B@:");
    method(im, "name", [image_of](CpuState& s, GuestMemory&) { s.x[0] = image_of(s.x[0])->name; });
    method(im, "setName:", [this, image_of](CpuState& s, GuestMemory&) {
        release(std::exchange(image_of(s.x[0])->name, s.x[2] ? cf.copy(s.x[2]) : 0));
        s.x[0] = 1;
    }, "B@:@");
    method(im, "isTemplate", [image_of](CpuState& s, GuestMemory&) { return_bool(s, image_of(s.x[0])->template_image); }, "B@:");
    method(im, "setTemplate:", [image_of](CpuState& s, GuestMemory&) { image_of(s.x[0])->template_image = bool_arg(s, 2); }, "v@:B");
    // The colour drawn behind the representations (clear by default).
    method(im, "backgroundColor", [this, image_of](CpuState& s, GuestMemory&) {
        const auto value = image_of(s.x[0])->background_color;
        s.x[0] = value ? value : send(color, "clearColor").x0;
    });
    method(im, "setBackgroundColor:", [this, image_of](CpuState& s, GuestMemory&) { assign(image_of(s.x[0])->background_color, s.x[2]); }, "v@:@");
    method(im, "accessibilityDescription", [image_of](CpuState& s, GuestMemory&) { s.x[0] = image_of(s.x[0])->accessibility_description; });
    method(im, "setAccessibilityDescription:", [this, image_of](CpuState& s, GuestMemory&) {
        release(std::exchange(image_of(s.x[0])->accessibility_description, s.x[2] ? cf.copy(s.x[2]) : 0));
    }, "v@:@");
    // Cap insets and the resizing mode (how the middle stretches when the
    // image is drawn larger).
    method(im, "capInsets", [image_of](CpuState& s, GuestMemory&) {
        const auto insets = image_of(s.x[0])->cap_insets;
        for (unsigned i = 0; i < 4; ++i) set_d(s, i, insets[i]);
    }, "{NSEdgeInsets=dddd}@:");
    method(im, "setCapInsets:", [image_of](CpuState& s, GuestMemory&) {
        auto& insets = image_of(s.x[0])->cap_insets;
        for (unsigned i = 0; i < 4; ++i) insets[i] = d(s, i);
    }, "v@:{NSEdgeInsets=dddd}");
    method(im, "resizingMode", [image_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(image_of(s.x[0])->resizing_mode); }, "q@:");
    method(im, "setResizingMode:", [image_of](CpuState& s, GuestMemory&) { image_of(s.x[0])->resizing_mode = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    // Named images: the system name NSApplicationIcon (nibs archive the
    // application icon as an NSCustomResource) is the main bundle's icon,
    // kept once made (make_application_icon). Image catalogs and other
    // resources are not looked up, so every other name reports no image.
    class_method(im, "imageNamed:", [this](CpuState& s, GuestMemory&) {
        if (!s.x[2] || text(s.x[2]) != "NSApplicationIcon") { s.x[0] = 0; return; }
        if (application_icon_image) { s.x[0] = application_icon_image; return; }
        const auto icon = make_application_icon();
        if (icon) {
            static_cast<void>(send_id(icon, "setName:", {s.x[2]}));
            application_icon_image = icon;
        }
        s.x[0] = icon;
    });
    // Image files decode eagerly (PNG, TIFF, JPEG, BMP, GIF, ICO); data the
    // host cannot read gives nil, as AppKit does for undecodable data.
    const auto init_from = [this](CpuState& s, std::optional<std::vector<std::uint8_t>> bytes) {
        const auto decoded = bytes ? decode_image(*bytes) : std::nullopt;
        if (!decoded) { release(s.x[0]); s.x[0] = 0; return; }
        fill_image(s.x[0], *decoded);
    };
    for (const auto* name : {"initWithContentsOfFile:", "initByReferencingFile:"})
        method(im, name, [this, init_from](CpuState& s, GuestMemory&) { init_from(s, s.x[2] ? read_file(text(s.x[2])) : std::nullopt); });
    for (const auto* name : {"initWithContentsOfURL:", "initByReferencingURL:"})
        method(im, name, [this, init_from](CpuState& s, GuestMemory&) { init_from(s, s.x[2] ? read_file(cf.url_path(s.x[2])) : std::nullopt); });
    method(im, "initWithData:", [this, init_from](CpuState& s, GuestMemory&) { init_from(s, data_bytes(s.x[2])); });
    method(im, "TIFFRepresentation", [this](CpuState& s, GuestMemory&) {
        const auto pixels = image_pixels(s.x[0]);
        const auto bytes = pixels ? encode_image(*pixels, ImageFileType::tiff) : std::nullopt;
        s.x[0] = bytes ? make_data(*bytes) : 0;
    });
    for (const auto* name : {"imageTypes", "imageUnfilteredTypes"})
        class_method(im, name, [this](CpuState& s, GuestMemory&) { s.x[0] = auto_array({auto_string("public.tiff"), auto_string("public.png")}); });
    class_method(im, "canInitWithPasteboard:", [this](CpuState& s, GuestMemory&) {
        const auto state = objc.host_state<PasteboardState>(s.x[2]);
        const auto server = state->general ? window_server_or_null() : nullptr;
        return_bool(s, server && server->clipboard_image().has_value());
    }, "B@:@");
    method(im, "initWithPasteboard:", [this, image_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = objc.host_state<PasteboardState>(s.x[2]);
        const auto server = state->general ? window_server_or_null() : nullptr;
        const auto pixels = server ? server->clipboard_image() : std::nullopt;
        if (!pixels) { release(self); s.x[0] = 0; return; }
        const auto source = image_from_pixels(*pixels);
        const auto info = image_of(self);
        info->size = image_of(source)->size;
        for (const auto rep : image_of(source)->representations) info->representations.push_back(retain(rep));
        release(source);
        s.x[0] = self;
    });
    // -lockFocus / -unlockFocus and NSImage / NSImageRep drawing are Quartz
    // 2D's (darwin/coregraphics/appkit_drawing.cpp).
    // NSCopying: an image of the receiver's class with the same size and
    // template flag and a copy of each representation; the copy has no name
    // (a name identifies the original).
    method(im, "copyWithZone:", [this, image_of](CpuState& s, GuestMemory&) {
        const auto from = image_of(s.x[0]);
        const auto copy = objc.allocate(objc.object_class(s.x[0]));
        auto to = std::make_shared<ImageState>();
        to->size = from->size;
        to->template_image = from->template_image;
        to->background_color = retain(from->background_color);
        to->accessibility_description = retain(from->accessibility_description);
        to->cap_insets = from->cap_insets;
        to->resizing_mode = from->resizing_mode;
        const auto originals = from->representations;
        for (const auto rep : originals)
            to->representations.push_back(objc.responds(rep, sel("copyWithZone:")) ? send_id(rep, "copyWithZone:", {s.x[2]}) : retain(rep));
        static_cast<void>(objc.host_state<ImageState>(copy, [&] { return to; }));
        s.x[0] = copy;
    }, "@@:^v");
    method(im, "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<ImageState>(s.x[0])) {
            for (const auto rep : std::exchange(state->representations, {})) release(rep);
            release(std::exchange(state->name, 0));
            release(std::exchange(state->background_color, 0));
            release(std::exchange(state->accessibility_description, 0));
        }
        static_cast<void>(objc.send_super(s.x[0], image, sel("dealloc")));
    }, "v@:");

    // ---- NSImageRep / NSBitmapImageRep ----
    const auto rep_of = [this](Address value) { return objc.host_state<BitmapState>(value); };
    const auto ir = image_rep, br = bitmap_rep;
    method(ir, "pixelsWide", [rep_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(rep_of(s.x[0])->width); }, "q@:");
    method(ir, "pixelsHigh", [rep_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(rep_of(s.x[0])->height); }, "q@:");
    method(ir, "bitsPerSample", [rep_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(rep_of(s.x[0])->bits_per_sample); }, "q@:");
    method(ir, "hasAlpha", [rep_of](CpuState& s, GuestMemory&) { return_bool(s, rep_of(s.x[0])->has_alpha); }, "B@:");
    method(ir, "size", [rep_of](CpuState& s, GuestMemory&) { return_size(s, rep_of(s.x[0])->size); }, "{CGSize=dd}@:");
    method(ir, "setSize:", [rep_of](CpuState& s, GuestMemory&) { rep_of(s.x[0])->size = size_arg(s); }, "v@:{CGSize=dd}");
    method(ir, "colorSpaceName", [this, rep_of](CpuState& s, GuestMemory&) {
        const auto name = rep_of(s.x[0])->color_space_name;
        s.x[0] = name ? name : auto_string("NSDeviceRGBColorSpace");
    });
    method(ir, "setColorSpaceName:", [this, rep_of](CpuState& s, GuestMemory&) {
        release(std::exchange(rep_of(s.x[0])->color_space_name, s.x[2] ? cf.copy(s.x[2]) : 0));
    }, "v@:@");
    method(ir, "isOpaque", [rep_of](CpuState& s, GuestMemory&) {
        const auto state = rep_of(s.x[0]);
        return_bool(s, state->opaque.value_or(!state->has_alpha));
    }, "B@:");
    method(ir, "setOpaque:", [rep_of](CpuState& s, GuestMemory&) { rep_of(s.x[0])->opaque = bool_arg(s, 2); }, "v@:B");
    method(ir, "setAlpha:", [rep_of](CpuState& s, GuestMemory&) { rep_of(s.x[0])->has_alpha = bool_arg(s, 2); }, "v@:B");
    method(ir, "setPixelsWide:", [rep_of](CpuState& s, GuestMemory&) { rep_of(s.x[0])->width = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    method(ir, "setPixelsHigh:", [rep_of](CpuState& s, GuestMemory&) { rep_of(s.x[0])->height = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    method(ir, "setBitsPerSample:", [rep_of](CpuState& s, GuestMemory&) { rep_of(s.x[0])->bits_per_sample = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    // A bitmap representation is its own CGImage.
    method(ir, "CGImageForProposedRect:context:hints:", [this](CpuState& s, GuestMemory&) {
        if (!responds(s.x[0], "CGImage"))
            unsupported("-[NSImageRep CGImageForProposedRect:context:hints:] of a representation that is not a bitmap");
        s.x[0] = send(s.x[0], "CGImage").x0;
    }, "^{CGImage=}@:^{CGRect={CGPoint=dd}{CGSize=dd}}@@");
    const auto init_bitmap = [this, rep_of](CpuState& s, GuestMemory& m, bool with_format) {
        const auto state = rep_of(s.x[0]);
        if (state->data) fail("NSBitmapImageRep initialized twice");
        const auto planes = s.x[2];
        state->width = static_cast<std::int64_t>(s.x[3]);
        state->height = static_cast<std::int64_t>(s.x[4]);
        state->bits_per_sample = static_cast<std::int64_t>(s.x[5]);
        state->samples_per_pixel = static_cast<std::int64_t>(s.x[6]);
        state->has_alpha = bool_arg(s, 7);
        // Stack: BOOL isPlanar, NSString *colorSpaceName, [NSBitmapFormat],
        // NSInteger bytesPerRow, NSInteger bitsPerPixel (Apple ARM64 packing).
        state->planar = m.read64(s.sp) & 0xff;
        Address offset = 8;
        const auto space = m.read64(s.sp + offset); offset += 8;
        if (with_format) { state->format = m.read64(s.sp + offset); offset += 8; }
        state->bytes_per_row = static_cast<std::int64_t>(m.read64(s.sp + offset)); offset += 8;
        state->bits_per_pixel = static_cast<std::int64_t>(m.read64(s.sp + offset));
        if (state->width <= 0 || state->height <= 0 || state->width > 32768 || state->height > 32768 ||
            state->samples_per_pixel <= 0 || state->samples_per_pixel > 5 || state->bits_per_sample <= 0) {
            release(s.x[0]); s.x[0] = 0; return;
        }
        if (state->planar && state->samples_per_pixel > 1) unsupported("planar NSBitmapImageRep data");
        if (!state->bits_per_pixel) state->bits_per_pixel = state->bits_per_sample * state->samples_per_pixel;
        if (!state->bytes_per_row) state->bytes_per_row = (state->width * state->bits_per_pixel + 7) / 8;
        state->size = {static_cast<double>(state->width), static_cast<double>(state->height)};
        state->color_space_name = space ? cf.copy(space) : string("NSDeviceRGBColorSpace");
        const auto first_plane = planes ? m.read64(planes) : 0;
        if (first_plane) {
            state->data = first_plane;
        } else {
            const auto bytes = static_cast<Address>(state->bytes_per_row * state->height);
            state->data = process.allocate_guest(bytes);
            state->owns = true;
            m.fill_bytes(state->data, 0, bytes);
        }
    };
    method(br, "initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:samplesPerPixel:hasAlpha:isPlanar:colorSpaceName:bytesPerRow:bitsPerPixel:",
           [init_bitmap](CpuState& s, GuestMemory& m) { init_bitmap(s, m, false); }, "@@:^*qqqqBB@qq");
    method(br, "initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:samplesPerPixel:hasAlpha:isPlanar:colorSpaceName:bitmapFormat:bytesPerRow:bitsPerPixel:",
           [init_bitmap](CpuState& s, GuestMemory& m) { init_bitmap(s, m, true); }, "@@:^*qqqqBB@Qqq");
    method(br, "bitmapData", [rep_of](CpuState& s, GuestMemory&) { s.x[0] = rep_of(s.x[0])->data; }, "*@:");
    method(br, "getBitmapDataPlanes:", [rep_of](CpuState& s, GuestMemory& m) { if (s.x[2]) m.write64(s.x[2], rep_of(s.x[0])->data); }, "v@:^*");
    method(br, "bitmapFormat", [rep_of](CpuState& s, GuestMemory&) { s.x[0] = rep_of(s.x[0])->format; }, "Q@:");
    method(br, "bitsPerPixel", [rep_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(rep_of(s.x[0])->bits_per_pixel); }, "q@:");
    method(br, "bytesPerRow", [rep_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(rep_of(s.x[0])->bytes_per_row); }, "q@:");
    method(br, "bytesPerPlane", [rep_of](CpuState& s, GuestMemory&) {
        const auto state = rep_of(s.x[0]);
        s.x[0] = static_cast<Address>(state->bytes_per_row * state->height);
    }, "q@:");
    method(br, "samplesPerPixel", [rep_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(rep_of(s.x[0])->samples_per_pixel); }, "q@:");
    method(br, "isPlanar", [rep_of](CpuState& s, GuestMemory&) { return_bool(s, rep_of(s.x[0])->planar); }, "B@:");
    // Raw samples of one pixel (top-left origin), one NSUInteger each.
    const auto sample_at = [this, rep_of](Address self, Address x, Address y, std::string_view what) {
        const auto state = rep_of(self);
        if (state->planar && state->samples_per_pixel > 1) unsupported("planar NSBitmapImageRep data");
        if (state->bits_per_sample != 8 && state->bits_per_sample != 16)
            unsupported(std::string(what) + " with " + std::to_string(state->bits_per_sample) + "-bit samples");
        if (static_cast<std::int64_t>(x) < 0 || static_cast<std::int64_t>(x) >= state->width || static_cast<std::int64_t>(y) < 0 ||
            static_cast<std::int64_t>(y) >= state->height)
            fail(std::string(what) + " outside the bitmap");
        return state->data + y * static_cast<Address>(state->bytes_per_row) + x * static_cast<Address>(state->bits_per_pixel / 8);
    };
    method(br, "getPixel:atX:y:", [rep_of, sample_at](CpuState& s, GuestMemory& m) {
        const auto state = rep_of(s.x[0]);
        const auto at = sample_at(s.x[0], s.x[3], s.x[4], "-[NSBitmapImageRep getPixel:atX:y:]");
        const auto width = static_cast<Address>(state->bits_per_sample / 8);
        for (std::int64_t i = 0; i < state->samples_per_pixel; ++i)
            m.write64(s.x[2] + 8 * i, width == 1 ? m.read8(at + i) : m.read8(at + 2 * i) | (m.read8(at + 2 * i + 1) << 8));
    }, "v@:^Qqq");
    method(br, "setPixel:atX:y:", [rep_of, sample_at](CpuState& s, GuestMemory& m) {
        const auto state = rep_of(s.x[0]);
        const auto at = sample_at(s.x[0], s.x[3], s.x[4], "-[NSBitmapImageRep setPixel:atX:y:]");
        const auto width = static_cast<Address>(state->bits_per_sample / 8);
        for (std::int64_t i = 0; i < state->samples_per_pixel; ++i) {
            const auto value = m.read64(s.x[2] + 8 * i);
            if (width == 1) m.write8(at + i, static_cast<std::uint8_t>(value));
            else {
                m.write8(at + 2 * i, static_cast<std::uint8_t>(value));
                m.write8(at + 2 * i + 1, static_cast<std::uint8_t>(value >> 8));
            }
        }
    }, "v@:^Qqq");
    method(br, "numberOfPlanes", [rep_of](CpuState& s, GuestMemory&) {
        const auto state = rep_of(s.x[0]);
        s.x[0] = state->planar ? static_cast<Address>(state->samples_per_pixel) : 1;
    }, "q@:");
    const auto rep_from_data = [this](Address self, Address data) -> Address {
        const auto decoded = decode_image(data_bytes(data));
        if (!decoded) { release(self); return 0; }
        setup_bitmap(self, decoded->pixels.width, decoded->pixels.height);
        const auto state = objc.host_state<BitmapState>(self);
        memory().write_bytes(state->data, decoded->pixels.rgba);
        return self;
    };
    method(br, "initWithData:", [rep_from_data](CpuState& s, GuestMemory&) { s.x[0] = rep_from_data(s.x[0], s.x[2]); });
    class_method(br, "imageRepWithData:", [this, rep_from_data](CpuState& s, GuestMemory&) {
        s.x[0] = autorelease(rep_from_data(objc.allocate(bitmap_rep), s.x[2]));
    });
    const auto encode_rep = [this, rep_of](Address self, std::uint64_t type) -> Address {
        if (type > static_cast<std::uint64_t>(ImageFileType::png)) unsupported("NSBitmapImageFileType " + std::to_string(type));
        const auto pixels = bitmap_pixels(rep_of(self));
        const auto bytes = pixels ? encode_image(*pixels, static_cast<ImageFileType>(type)) : std::nullopt;
        return bytes ? make_data(*bytes) : 0;
    };
    method(br, "representationUsingType:properties:", [encode_rep](CpuState& s, GuestMemory&) { s.x[0] = encode_rep(s.x[0], s.x[2]); }, "@@:Q@");
    method(br, "TIFFRepresentation", [encode_rep](CpuState& s, GuestMemory&) { s.x[0] = encode_rep(s.x[0], 0); });
    // NSCopying: a rep of the receiver's class owning a copy of the pixels.
    method(br, "copyWithZone:", [this, rep_of](CpuState& s, GuestMemory& m) {
        const auto from = rep_of(s.x[0]);
        const auto copy = objc.allocate(objc.object_class(s.x[0]));
        auto to = std::make_shared<BitmapState>(*from);
        to->color_space_name = retain(to->color_space_name);
        to->data = 0;
        to->owns = false;
        if (from->data) {
            const auto bytes = static_cast<std::size_t>(from->bytes_per_row * from->height);
            std::vector<std::uint8_t> pixels(bytes);
            m.read_bytes(from->data, pixels);
            to->data = process.allocate_guest(bytes);
            to->owns = true;
            m.write_bytes(to->data, pixels);
        }
        static_cast<void>(objc.host_state<BitmapState>(copy, [&] { return to; }));
        object_copied(s.x[0], copy);
        s.x[0] = copy;
    }, "@@:^v");
    method(br, "dealloc", [this](CpuState& s, GuestMemory&) {
        object_deallocated(s.x[0]);
        if (const auto state = objc.find_host_state<BitmapState>(s.x[0])) {
            if (state->owns && state->data) process.release_guest(std::exchange(state->data, 0));
            release(std::exchange(state->color_space_name, 0));
        }
        static_cast<void>(objc.send_super(s.x[0], bitmap_rep, sel("dealloc")));
    }, "v@:");

    // NSBezierPath is Quartz 2D's (darwin/coregraphics/appkit_drawing.cpp).

    // ---- NSColor ----
    const auto co = color;
    const auto color_of = [this](Address value) { return objc.host_state<ColorState>(value); };
    for (const auto* name : {"colorWithRed:green:blue:alpha:", "colorWithSRGBRed:green:blue:alpha:", "colorWithDeviceRed:green:blue:alpha:",
             "colorWithCalibratedRed:green:blue:alpha:"})
        class_method(co, name, [this](CpuState& s, GuestMemory&) { s.x[0] = color_object(d(s, 0), d(s, 1), d(s, 2), d(s, 3)); }, "@@:dddd");
    for (const auto* name : {"colorWithWhite:alpha:", "colorWithGenericGamma22White:alpha:", "colorWithDeviceWhite:alpha:",
             "colorWithCalibratedWhite:alpha:"})
        class_method(co, name, [this](CpuState& s, GuestMemory&) {
            s.x[0] = color_object(d(s, 0), d(s, 0), d(s, 0), d(s, 1));
            objc.host_state<ColorState>(s.x[0])->white = true;
        }, "@@:dd");
    // Shared named colours (shared_color). windowBackgroundColor is a dynamic
    // system colour (controls/appearance.cpp).
    for (const auto& [name, rgba] : std::initializer_list<std::pair<const char*, std::array<double, 4>>>{
             {"blackColor", {0, 0, 0, 1}}, {"whiteColor", {1, 1, 1, 1}}, {"clearColor", {0, 0, 0, 0}},
             {"redColor", {1, 0, 0, 1}}, {"greenColor", {0, 1, 0, 1}}, {"blueColor", {0, 0, 1, 1}},
             {"grayColor", {0.5, 0.5, 0.5, 1}}, {"lightGrayColor", {2.0 / 3, 2.0 / 3, 2.0 / 3, 1}},
             {"darkGrayColor", {1.0 / 3, 1.0 / 3, 1.0 / 3, 1}}, {"yellowColor", {1, 1, 0, 1}}, {"cyanColor", {0, 1, 1, 1}},
             {"magentaColor", {1, 0, 1, 1}}, {"orangeColor", {1, 0.5, 0, 1}}, {"brownColor", {0.6, 0.4, 0.2, 1}},
             {"purpleColor", {0.5, 0, 0.5, 1}},
             {"controlAccentColor", {0, 0.48, 1, 1}}, {"selectedTextBackgroundColor", {0.7, 0.84, 1, 1}},
             // The Touch Bar scrubber's background: its dark tone, without the texture.
             {"scrubberTexturedBackgroundColor", {0.18, 0.18, 0.18, 1}}}) {
        const auto value = rgba;
        const std::string key = name;
        class_method(co, name, [this, value, key](CpuState& s, GuestMemory&) {
            s.x[0] = shared_color(key, [&] { return color_object(value[0], value[1], value[2], value[3]); });
        });
    }
    // Components in a colour space by its model (NSColorSpaceModel gray 0,
    // RGB 1, CMYK 2), the last component being alpha when present.
    class_method(co, "colorWithColorSpace:components:count:", [this](CpuState& s, GuestMemory& m) {
        const auto space = s.x[2], components = s.x[3];
        const auto count = static_cast<std::int64_t>(s.x[4]);
        if (!space || !components || count <= 0) fail("+[NSColor colorWithColorSpace:components:count:] without components");
        const auto model = static_cast<std::int64_t>(send(space, "colorSpaceModel").x0);
        std::vector<double> c(static_cast<std::size_t>(count));
        for (std::int64_t i = 0; i < count; ++i) c[static_cast<std::size_t>(i)] = std::bit_cast<double>(m.read64(components + 8 * i));
        const auto alpha = [&](std::int64_t index) { return count > index ? c[static_cast<std::size_t>(index)] : 1.0; };
        switch (model) {
        case 0:
            s.x[0] = color_object(c[0], c[0], c[0], alpha(1));
            objc.host_state<ColorState>(s.x[0])->white = true;
            break;
        case 1:
            if (count < 3) fail("+[NSColor colorWithColorSpace:components:count:] needs three RGB components");
            s.x[0] = color_object(c[0], c[1], c[2], alpha(3));
            break;
        case 2: {
            if (count < 4) fail("+[NSColor colorWithColorSpace:components:count:] needs four CMYK components");
            const double k = 1 - c[3];
            s.x[0] = color_object((1 - c[0]) * k, (1 - c[1]) * k, (1 - c[2]) * k, alpha(4));
            break;
        }
        default: unsupported("+[NSColor colorWithColorSpace:components:count:] in colour space model " + std::to_string(model));
        }
    }, "@@:@^dq");
    // NSBlueControlTint: the accent colour is blue.
    class_method(co, "currentControlTint", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "Q@:");
    method(co, "colorWithAlphaComponent:", [this, color_of](CpuState& s, GuestMemory&) {
        const auto state = color_of(s.x[0]);
        s.x[0] = color_object(state->red, state->green, state->blue, d(s, 0));
    }, "@@:d");
    method(co, "redComponent", [color_of](CpuState& s, GuestMemory&) { set_d(s, 0, color_of(s.x[0])->red); }, "d@:");
    method(co, "greenComponent", [color_of](CpuState& s, GuestMemory&) { set_d(s, 0, color_of(s.x[0])->green); }, "d@:");
    method(co, "blueComponent", [color_of](CpuState& s, GuestMemory&) { set_d(s, 0, color_of(s.x[0])->blue); }, "d@:");
    method(co, "alphaComponent", [color_of](CpuState& s, GuestMemory&) { set_d(s, 0, color_of(s.x[0])->alpha); }, "d@:");
    method(co, "whiteComponent", [color_of](CpuState& s, GuestMemory&) {
        const auto state = color_of(s.x[0]);
        set_d(s, 0, (state->red + state->green + state->blue) / 3);
    }, "d@:");
    method(co, "numberOfComponents", [](CpuState& s, GuestMemory&) { s.x[0] = 4; }, "q@:");
    method(co, "getRed:green:blue:alpha:", [color_of](CpuState& s, GuestMemory& m) {
        const auto state = color_of(s.x[0]);
        const std::array<double, 4> values{state->red, state->green, state->blue, state->alpha};
        for (unsigned index = 0; index < 4; ++index)
            if (s.x[2 + index]) m.write64(s.x[2 + index], std::bit_cast<std::uint64_t>(values[index]));
    }, "v@:^d^d^d^d");
    // Colors are kept as sRGB components; every RGB color space here is
    // treated as sRGB, so conversion keeps the components.
    for (const auto* name : {"colorUsingColorSpace:", "colorUsingColorSpaceName:", "colorUsingType:"})
        method(co, name, [this](CpuState& s, GuestMemory&) { s.x[0] = autorelease(retain(s.x[0])); });
    method(co, "colorSpaceName", [this](CpuState& s, GuestMemory&) { s.x[0] = auto_string("NSCalibratedRGBColorSpace"); });
    method(co, "colorSpace", [this](CpuState& s, GuestMemory&) { s.x[0] = send(color_space, "sRGBColorSpace").x0; });
    // -set, -setFill, -setStroke and -CGColor are Quartz 2D's
    // (darwin/coregraphics/appkit_drawing.cpp).
    // Colors are immutable values: copies are the color itself; equality
    // compares components (and whether the color is a gray).
    for (const auto* name : {"copy", "copyWithZone:"})
        method(co, name, [this](CpuState& s, GuestMemory&) { s.x[0] = retain(s.x[0]); });
    method(co, "isEqual:", [this, color_of](CpuState& s, GuestMemory&) {
        if (s.x[0] == s.x[2]) { s.x[0] = 1; return; }
        if (!s.x[2] || !objc.find_host_state<ColorState>(s.x[2])) { s.x[0] = 0; return; }
        const auto a = color_of(s.x[0]), b = color_of(s.x[2]);
        s.x[0] = a->red == b->red && a->green == b->green && a->blue == b->blue && a->alpha == b->alpha && a->white == b->white;
    }, "B@:@");
    method(co, "hash", [color_of](CpuState& s, GuestMemory&) {
        const auto c = color_of(s.x[0]);
        const auto channel = [](double v) { return static_cast<Address>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
        s.x[0] = (channel(c->red) << 24) | (channel(c->green) << 16) | (channel(c->blue) << 8) | channel(c->alpha);
    }, "Q@:");

    // ---- NSColorSpace ----
    const auto cs = color_space;
    for (const auto& [selector, name] : std::initializer_list<std::pair<const char*, const char*>>{
             {"deviceRGBColorSpace", "kCGColorSpaceSRGB"}, {"sRGBColorSpace", "kCGColorSpaceSRGB"},
             {"genericRGBColorSpace", "kCGColorSpaceGenericRGB"}, {"extendedSRGBColorSpace", "kCGColorSpaceExtendedSRGB"},
             {"displayP3ColorSpace", "kCGColorSpaceDisplayP3"}}) {
        const std::string key = name;
        class_method(cs, selector, [this, key](CpuState& s, GuestMemory&) {
            std::lock_guard lock(mutex);
            if (const auto found = color_spaces.find(key); found != color_spaces.end()) { s.x[0] = found->second; return; }
            const auto value = objc.allocate(color_space);
            if (const auto record = process.objects().find(value)) record->immortal = true;
            objc.host_state<ColorSpaceState>(value)->name = key;
            color_spaces.emplace(key, value);
            s.x[0] = value;
        });
    }
    method(cs, "colorSpaceModel", [this](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(objc.host_state<ColorSpaceState>(s.x[0])->model); }, "q@:");
    method(cs, "numberOfColorComponents", [](CpuState& s, GuestMemory&) { s.x[0] = 3; }, "q@:");
    // The named RGB spaces carry an authored matrix/TRC profile (Qt's Cocoa
    // screens read it); unnamed spaces (Qt support's gray/CMYK singletons) none.
    method(cs, "ICCProfileData", [this](CpuState& s, GuestMemory&) {
        const auto name = objc.host_state<ColorSpaceState>(s.x[0])->name;
        if (name.empty()) { s.x[0] = 0; return; }
        // Primaries as XYZ rows (red, green, blue): the spaces' chromaticities
        // with a D65 white, Bradford-adapted to the D50 PCS.
        static constexpr std::array<std::array<double, 3>, 3> srgb{{
            {0.4360185, 0.2224751, 0.0139238}, {0.3850857, 0.7169029, 0.0970818}, {0.1430696, 0.0606177, 0.7140658}}};
        static constexpr std::array<std::array<double, 3>, 3> p3{{
            {0.5150915, 0.2411775, -0.0010497}, {0.2919528, 0.6922433, 0.0418815}, {0.1571295, 0.0665749, 0.7842396}}};
        const bool display_p3 = name == "kCGColorSpaceDisplayP3";
        const auto profile = rgb_icc_profile(display_p3 ? "Display P3" : name == "kCGColorSpaceGenericRGB" ? "Generic RGB" : "sRGB IEC61966-2.1",
                                             display_p3 ? p3 : srgb);
        s.x[0] = make_data(profile);
    });
    method(cs, "localizedName", [this](CpuState& s, GuestMemory&) {
        const auto name = objc.host_state<ColorSpaceState>(s.x[0])->name;
        s.x[0] = auto_string(name == "kCGColorSpaceDisplayP3" ? "Display P3" : name == "kCGColorSpaceGenericRGB" ? "Generic RGB" : "sRGB IEC61966-2.1");
    });
    method(cs, "CGColorSpace", [this](CpuState& s, GuestMemory&) {
        // CGColorSpaceCreateWithName (CoreGraphics provider); the NSColorSpace
        // singletons keep the +1 reference for their lifetime.
        const auto state = objc.host_state<ColorSpaceState>(s.x[0]);
        std::lock_guard lock(mutex);
        if (const auto found = cg_color_spaces.find(state->name); found != cg_color_spaces.end()) { s.x[0] = found->second; return; }
        const auto name = string(state->name);
        const auto space = guest_call(core_graphics_library, "_CGColorSpaceCreateWithName", {name});
        release(name);
        cg_color_spaces.emplace(state->name, space);
        s.x[0] = space;
    }, "^v@:");

    // ---- NSAppearance ----
    const auto ap = appearance;
    class_method(ap, "appearanceNamed:", [this](CpuState& s, GuestMemory&) { s.x[0] = appearance_named(text(s.x[2])); });
    for (const auto* name : {"currentAppearance", "currentDrawingAppearance"})
        class_method(ap, name, [this](CpuState& s, GuestMemory&) {
            if (current_appearance_override) { s.x[0] = current_appearance_override; return; }
            const auto chosen = app ? app_state()->appearance : 0;
            s.x[0] = chosen ? chosen : appearance_named(system_appearance_name());
        });
    // +setCurrentAppearance: (deprecated; wxWidgets sets it around drawing):
    // the appearance +currentAppearance answers until it is set to nil. Main
    // thread drawing only, so one value stands for the thread's.
    class_method(ap, "setCurrentAppearance:", [this](CpuState& s, GuestMemory&) {
        const auto previous = std::exchange(current_appearance_override, retain(s.x[2]));
        release(previous);
    }, "v@:@");
    method(ap, "performAsCurrentDrawingAppearance:", [this](CpuState& s, GuestMemory&) {
        const auto previous = std::exchange(current_appearance_override, retain(s.x[0]));
        struct Restore {
            AppKit& kit; Address previous;
            ~Restore() { kit.release(std::exchange(kit.current_appearance_override, previous)); }
        } restore{*this, previous};
        if (s.x[2]) static_cast<void>(invoke_block(s.x[2], {}));
    }, "v@:@?");
    method(ap, "name", [this](CpuState& s, GuestMemory&) { s.x[0] = objc.host_state<AppearanceState>(s.x[0])->name; });
    method(ap, "allowsVibrancy", [this](CpuState& s, GuestMemory&) {
        return_bool(s, text(objc.host_state<AppearanceState>(s.x[0])->name).find("Vibrant") != std::string::npos);
    }, "B@:");
    method(ap, "bestMatchFromAppearancesWithNames:", [this](CpuState& s, GuestMemory&) {
        const auto own = text(objc.host_state<AppearanceState>(s.x[0])->name);
        const bool dark = own.find("Dark") != std::string::npos;
        const auto names = items(s.x[2]);
        for (const auto value : names) if (text(value) == own) { s.x[0] = value; return; }
        for (const auto value : names) {
            const auto candidate = text(value);
            if ((candidate.find("Dark") != std::string::npos) == dark) { s.x[0] = value; return; }
        }
        s.x[0] = 0;
    });

    // ---- NSColorSampler ----
    // The color under the pointer when sampling starts (host::ui; the host
    // has no system loupe); the handler runs before -showSamplerWithSelectionHandler:
    // returns, with nil when the screen cannot be read.
    method(color_sampler, "showSamplerWithSelectionHandler:", [this](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("NSColorSampler needs a selection handler");
        Address picked{};
        if (const auto pixel = host::ui::pixel_under_pointer())
            picked = color_object((*pixel)[0] / 255.0, (*pixel)[1] / 255.0, (*pixel)[2] / 255.0, 1);
        static_cast<void>(invoke_block(s.x[2], {picked}));
    }, "v@:@?");

    // ---- NSWorkspace ----
    const auto ws_class = workspace;
    class_method(ws_class, "sharedWorkspace", [this](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        if (!shared_workspace) {
            shared_workspace = objc.allocate(workspace);
            if (const auto record = process.objects().find(shared_workspace)) record->immortal = true;
        }
        s.x[0] = shared_workspace;
    });
    const auto shell_open = [this](std::string_view guest_path, const char* verb, std::string_view parameters) {
        return static_cast<bool>(host::ui::shell_open(host_path(guest_path), verb, parameters));
    };
    method(ws_class, "openURL:", [this, shell_open](CpuState& s, GuestMemory&) {
        // Foundation's NSURL represents file URLs only.
        return_bool(s, s.x[2] && shell_open(cf.url_path(s.x[2]), "open", ""));
    }, "B@:@");
    method(ws_class, "openFile:", [this, shell_open](CpuState& s, GuestMemory&) { return_bool(s, s.x[2] && shell_open(text(s.x[2]), "open", "")); }, "B@:@");
    method(ws_class, "selectFile:inFileViewerRootedAtPath:", [this](CpuState& s, GuestMemory&) {
        const auto target = s.x[2] ? host_path(text(s.x[2])) : host_path(text(s.x[3]));
        return_bool(s, static_cast<bool>(host::ui::reveal_file(target)));
    }, "B@:@@");
    // The workspace's own notification center (not the application's default
    // center, as on macOS). Workspace notifications (launches, sleep, volumes,
    // display options) are never posted to it: the host reports none of them.
    method(ws_class, "notificationCenter", [this](CpuState& s, GuestMemory&) {
        Address made{};
        {
            std::lock_guard lock(mutex);
            if (workspace_center) { s.x[0] = workspace_center; return; }
        }
        made = send(send(objc.find_class("NSNotificationCenter"), "alloc").x0, "init").x0;
        {
            std::lock_guard lock(mutex);
            if (!workspace_center) {
                workspace_center = made;
                if (const auto record = process.objects().find(made)) record->immortal = true;
                made = 0;
            }
            s.x[0] = workspace_center;
        }
        if (made) release(made);   // another thread made it first (released outside the lock)
    });
    // The running applications Alchemy knows are this one: it is frontmost and
    // owns the menu bar while it is active; otherwise a Windows program is in
    // front, which is no NSRunningApplication (nil).
    for (const auto* selector : {"frontmostApplication", "menuBarOwningApplication"})
        method(ws_class, selector, [this](CpuState& s, GuestMemory&) {
            Address application{};
            { std::lock_guard lock(mutex); application = app; }
            const bool active = application && (send(application, "isActive").x0 & 0xff) != 0;
            const auto running = objc.find_class("NSRunningApplication");
            s.x[0] = active && running ? send(running, "currentApplication").x0 : 0;
        });

    // ---- NSDocumentController ----
    const auto dc_class = document_controller;
    // +sharedDocumentController and the document architecture: documents.cpp.
    method(dc_class, "noteNewRecentDocumentURL:", [this](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;
        const auto path = cf.url_path(s.x[2]);
        {
            std::lock_guard lock(mutex);
            for (auto it = recent_documents.begin(); it != recent_documents.end(); ++it)
                if (cf.url_path(*it) == path) { release(*it); recent_documents.erase(it); break; }
            recent_documents.insert(recent_documents.begin(), retain(s.x[2]));
            while (recent_documents.size() > 10) { release(recent_documents.back()); recent_documents.pop_back(); }
        }
        // The host's recent-items list (Start menu, jump lists).
        host::ui::add_recent_document(host_path(path));
    }, "v@:@");
    method(dc_class, "recentDocumentURLs", [this](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        s.x[0] = auto_array(recent_documents);
    });
    method(dc_class, "clearRecentDocuments:", [this](CpuState&, GuestMemory&) {
        std::vector<Address> old;
        { std::lock_guard lock(mutex); old.swap(recent_documents); }
        for (const auto value : old) release(value);
    }, "v@:@");
    method(dc_class, "maximumRecentDocumentCount", [](CpuState& s, GuestMemory&) { s.x[0] = 10; }, "Q@:");

    // ---- NSScreen ----
    const auto sc = screen;
    const auto info_of = [this](Address value) {
        const auto state = objc.host_state<ScreenState>(value);
        if (const auto server = window_server_or_null())
            for (const auto& info : server->screens()) if (info.id == state->info.id) state->info = info;
        return state->info;
    };
    class_method(sc, "screens", [this](CpuState& s, GuestMemory&) {
        std::vector<Address> values;
        if (const auto server = window_server_or_null())
            for (const auto& info : server->screens()) if (const auto value = screen_object(info.id)) values.push_back(value);
        s.x[0] = auto_array(values);
    });
    for (const auto* name : {"mainScreen", "deepestScreen"})
        class_method(sc, name, [this](CpuState& s, GuestMemory&) { s.x[0] = main_screen(); });
    class_method(sc, "screensHaveSeparateSpaces", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    method(sc, "frame", [info_of](CpuState& s, GuestMemory&) { return_rect(s, info_of(s.x[0]).frame); }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    method(sc, "visibleFrame", [info_of](CpuState& s, GuestMemory&) { return_rect(s, info_of(s.x[0]).visible_frame); },
           "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    method(sc, "backingScaleFactor", [info_of](CpuState& s, GuestMemory&) { set_d(s, 0, info_of(s.x[0]).backing_scale); }, "d@:");
    method(sc, "maximumFramesPerSecond", [info_of](CpuState& s, GuestMemory&) {
        const auto rate = info_of(s.x[0]).refresh_rate;
        s.x[0] = static_cast<Address>(rate > 0 ? std::lround(rate) : 60);
    }, "q@:");
    method(sc, "minimumRefreshInterval", [info_of](CpuState& s, GuestMemory&) {
        const auto rate = info_of(s.x[0]).refresh_rate;
        set_d(s, 0, 1 / (rate > 0 ? rate : 60));
    }, "d@:");
    method(sc, "maximumRefreshInterval", [info_of](CpuState& s, GuestMemory&) {
        const auto rate = info_of(s.x[0]).refresh_rate;
        set_d(s, 0, 1 / (rate > 0 ? rate : 60));
    }, "d@:");
    method(sc, "displayUpdateGranularity", [](CpuState& s, GuestMemory&) { set_d(s, 0, 0); }, "d@:");
    method(sc, "localizedName", [this, info_of](CpuState& s, GuestMemory&) { s.x[0] = auto_string(info_of(s.x[0]).name); });
    method(sc, "depth", [](CpuState& s, GuestMemory&) { s.x[0] = 0x208; }, "i@:");   // NSWindowDepthTwentyfourBitRGB
    method(sc, "colorSpace", [this](CpuState& s, GuestMemory&) { s.x[0] = send(color_space, "sRGBColorSpace").x0; });
    method(sc, "deviceDescription", [this, info_of](CpuState& s, GuestMemory&) {
        const auto info = info_of(s.x[0]);
        const std::array<std::pair<std::string_view, Address>, 4> entries{{
            {"NSScreenNumber", cf.integer(info.id, 3 /* kCFNumberSInt32Type */)},
            {"NSDeviceIsScreen", string("YES")},
            {"NSDeviceBitsPerSample", cf.integer(8)},
            {"NSDeviceColorSpaceName", string("NSCalibratedRGBColorSpace")}}};
        std::vector<std::pair<Address, Address>> values;
        for (const auto& [key, value] : entries) values.emplace_back(string(key), value);
        const auto dictionary = cf.dictionary(values);
        for (const auto& [key, value] : values) { process.objects().release(key); process.objects().release(value); }
        s.x[0] = autorelease(dictionary);
    });
    const auto screen_scale = [info_of](CpuState& s, bool to_backing) {
        const auto scale = info_of(s.x[0]).backing_scale;
        const auto factor = to_backing ? scale : 1 / scale;
        const auto r = rect_arg(s);
        return_rect(s, {{r.origin.x * factor, r.origin.y * factor}, {r.size.width * factor, r.size.height * factor}});
    };
    method(sc, "convertRectToBacking:", [screen_scale](CpuState& s, GuestMemory&) { screen_scale(s, true); },
           "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(sc, "convertRectFromBacking:", [screen_scale](CpuState& s, GuestMemory&) { screen_scale(s, false); },
           "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    // NSEdgeInsets is an HFA of four doubles; Windows screens have no notch.
    method(sc, "safeAreaInsets", [](CpuState& s, GuestMemory&) { for (unsigned index = 0; index < 4; ++index) set_d(s, index, 0); },
           "{NSEdgeInsets=dddd}@:");
}

} // namespace alchemy::gui::appkit
