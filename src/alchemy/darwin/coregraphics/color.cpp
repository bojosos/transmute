#include "alchemy/darwin/coregraphics/icc_profiles.hpp"
#include "alchemy/darwin/coregraphics/q2d_internal.hpp"

#include <algorithm>

// CGColorSpace and CGColor. They replace the headless CGColorSpace shell of
// darwin_frameworks.cpp (same host class _AlchemyCGColorSpace, same names).
// Colors are converted to straight sRGB for drawing: RGB spaces are treated
// as sRGB except linear spaces (gamma encoded) and Display P3 / DCI-P3
// (converted through linear XYZ); gray is equal RGB; CMYK uses the naive
// complement; Lab converts from D50. No ICC color management is applied: a
// space made from one of the named spaces' own profiles (icc_profiles.cpp)
// converts as that named space, any other profile by its model.
namespace alchemy::gui::quartz2d {
namespace {

struct NamedSpace {
    std::string_view name;
    ColorModel model;
    bool linear, extended, wide;
};
constexpr NamedSpace named_spaces[] = {
    {"kCGColorSpaceSRGB", ColorModel::rgb, false, false, false},
    {"kCGColorSpaceExtendedSRGB", ColorModel::rgb, false, true, false},
    {"kCGColorSpaceLinearSRGB", ColorModel::rgb, true, false, false},
    {"kCGColorSpaceExtendedLinearSRGB", ColorModel::rgb, true, true, false},
    {"kCGColorSpaceGenericRGB", ColorModel::rgb, false, false, false},
    {"kCGColorSpaceGenericRGBLinear", ColorModel::rgb, true, false, false},
    {"kCGColorSpaceDisplayP3", ColorModel::rgb, false, false, true},
    {"kCGColorSpaceExtendedDisplayP3", ColorModel::rgb, false, true, true},
    {"kCGColorSpaceDCIP3", ColorModel::rgb, false, false, true},
    {"kCGColorSpaceAdobeRGB1998", ColorModel::rgb, false, false, true},
    {"kCGColorSpaceITUR_709", ColorModel::rgb, false, false, false},
    {"kCGColorSpaceITUR_2020", ColorModel::rgb, false, false, true},
    {"kCGColorSpaceROMMRGB", ColorModel::rgb, false, false, true},
    {"kCGColorSpaceACESCGLinear", ColorModel::rgb, true, false, true},
    {"kCGColorSpaceGenericGray", ColorModel::monochrome, false, false, false},
    {"kCGColorSpaceGenericGrayGamma2_2", ColorModel::monochrome, false, false, false},
    {"kCGColorSpaceLinearGray", ColorModel::monochrome, true, false, false},
    {"kCGColorSpaceExtendedGray", ColorModel::monochrome, false, true, false},
    {"kCGColorSpaceExtendedLinearGray", ColorModel::monochrome, true, true, false},
    {"kCGColorSpaceGenericCMYK", ColorModel::cmyk, false, false, false},
    {"kCGColorSpaceGenericLab", ColorModel::lab, false, false, false},
    {"kCGColorSpaceGenericXYZ", ColorModel::xyz, true, false, false},
};
const NamedSpace* find_named(std::string_view name) {
    for (const auto& entry : named_spaces) if (entry.name == name) return &entry;
    return nullptr;
}

double encode_srgb(double linear) {
    const auto magnitude = std::abs(linear);
    const auto value = magnitude <= 0.0031308 ? 12.92 * magnitude : 1.055 * std::pow(magnitude, 1 / 2.4) - 0.055;
    return linear < 0 ? -value : value;
}
double decode_srgb(double encoded) {
    const auto magnitude = std::abs(encoded);
    const auto value = magnitude <= 0.04045 ? magnitude / 12.92 : std::pow((magnitude + 0.055) / 1.055, 2.4);
    return encoded < 0 ? -value : value;
}
Rgba from_linear_xyz(double x, double y, double z, double alpha) {
    // XYZ (D65) -> linear sRGB.
    const auto r = 3.2404542 * x - 1.5371385 * y - 0.4985314 * z;
    const auto g = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z;
    const auto b = 0.0556434 * x - 0.2040259 * y + 1.0572252 * z;
    return {encode_srgb(r), encode_srgb(g), encode_srgb(b), alpha};
}

} // namespace

unsigned ColorSpaceInfo::components() const {
    switch (model) {
    case ColorModel::monochrome: return 1;
    case ColorModel::rgb: case ColorModel::lab: case ColorModel::xyz: return 3;
    case ColorModel::cmyk: return 4;
    case ColorModel::indexed: return 1;
    case ColorModel::pattern: return 0;
    default: return 3;
    }
}

Address Quartz::make_color_space(ColorModel model, std::string name) {
    auto info = std::make_shared<ColorSpaceInfo>();
    info->model = model;
    if (const auto* named = find_named(name)) {
        info->linear = named->linear;
        info->extended = named->extended;
        info->wide = named->wide;
    }
    info->name = std::move(name);
    return create(color_space_class, info, [this, info](Address) { release(std::exchange(info->base, 0)); });
}
Address Quartz::device_rgb() {
    std::lock_guard lock(mutex);
    if (!shared_device_rgb) shared_device_rgb = make_color_space(ColorModel::rgb, "");
    return shared_device_rgb;
}
Address Quartz::device_gray() {
    std::lock_guard lock(mutex);
    if (!shared_device_gray) shared_device_gray = make_color_space(ColorModel::monochrome, "");
    return shared_device_gray;
}
Address Quartz::srgb() {
    std::lock_guard lock(mutex);
    if (!shared_srgb) shared_srgb = make_color_space(ColorModel::rgb, "kCGColorSpaceSRGB");
    return shared_srgb;
}

Rgba Quartz::to_rgba(Address space, std::span<const double> c, double alpha) {
    const auto info = space ? space_info(space) : nullptr;
    const auto model = info ? info->model : ColorModel::rgb;
    const auto at = [&](std::size_t index) { return index < c.size() ? c[index] : 0.0; };
    const auto clamp = [&](double value) { return info && info->extended ? value : std::clamp(value, 0.0, 1.0); };
    Rgba result{0, 0, 0, std::clamp(alpha, 0.0, 1.0)};
    switch (model) {
    case ColorModel::monochrome: {
        auto gray = at(0);
        if (info && info->linear) gray = encode_srgb(gray);
        result.r = result.g = result.b = clamp(gray);
        break;
    }
    case ColorModel::rgb: {
        auto r = at(0), g = at(1), b = at(2);
        if (info && info->linear) { r = encode_srgb(r); g = encode_srgb(g); b = encode_srgb(b); }
        const auto* named = !info ? nullptr : info->name.empty() ? &info->profile : &info->name;
        if (named && (*named == "kCGColorSpaceDisplayP3" || *named == "kCGColorSpaceExtendedDisplayP3" || *named == "kCGColorSpaceDCIP3")) {
            // Display P3 (sRGB transfer, P3 primaries, D65) -> XYZ -> sRGB.
            const auto lr = decode_srgb(r), lg = decode_srgb(g), lb = decode_srgb(b);
            const auto x = 0.4865709 * lr + 0.2656677 * lg + 0.1982173 * lb;
            const auto y = 0.2289746 * lr + 0.6917385 * lg + 0.0792869 * lb;
            const auto z = 0.0000000 * lr + 0.0451134 * lg + 1.0439444 * lb;
            const auto converted = from_linear_xyz(x, y, z, result.a);
            r = converted.r; g = converted.g; b = converted.b;
        }
        result.r = clamp(r); result.g = clamp(g); result.b = clamp(b);
        break;
    }
    case ColorModel::cmyk:
        result.r = std::clamp((1 - at(0)) * (1 - at(3)), 0.0, 1.0);
        result.g = std::clamp((1 - at(1)) * (1 - at(3)), 0.0, 1.0);
        result.b = std::clamp((1 - at(2)) * (1 - at(3)), 0.0, 1.0);
        break;
    case ColorModel::lab: {
        // CIE Lab (D50) -> XYZ (D50) -> Bradford to D65 -> sRGB.
        const auto fy = (at(0) + 16) / 116, fx = fy + at(1) / 500, fz = fy - at(2) / 200;
        const auto inverse = [](double t) { return t > 6.0 / 29 ? t * t * t : 3 * (6.0 / 29) * (6.0 / 29) * (t - 4.0 / 29); };
        const auto x50 = 0.96422 * inverse(fx), y50 = inverse(fy), z50 = 0.82521 * inverse(fz);
        const auto x = 0.9555766 * x50 - 0.0230393 * y50 + 0.0631636 * z50;
        const auto y = -0.0282895 * x50 + 1.0099416 * y50 + 0.0210077 * z50;
        const auto z = 0.0122982 * x50 - 0.0204830 * y50 + 1.3299098 * z50;
        const auto converted = from_linear_xyz(x, y, z, result.a);
        result.r = std::clamp(converted.r, 0.0, 1.0); result.g = std::clamp(converted.g, 0.0, 1.0); result.b = std::clamp(converted.b, 0.0, 1.0);
        break;
    }
    case ColorModel::xyz: {
        const auto converted = from_linear_xyz(at(0), at(1), at(2), result.a);
        result.r = std::clamp(converted.r, 0.0, 1.0); result.g = std::clamp(converted.g, 0.0, 1.0); result.b = std::clamp(converted.b, 0.0, 1.0);
        break;
    }
    case ColorModel::indexed: {
        const auto base = info->base;
        const auto base_info = base ? space_info(base) : nullptr;
        const auto count = base_info ? base_info->components() : 3;
        const auto index = static_cast<std::size_t>(std::clamp(at(0), 0.0, static_cast<double>(info->last_index)));
        std::vector<double> values(count);
        for (unsigned component = 0; component < count; ++component) {
            const auto offset = index * count + component;
            values[component] = offset < info->table.size() ? info->table[offset] / 255.0 : 0;
        }
        return to_rgba(base, values, alpha);
    }
    case ColorModel::pattern:
        // The tint of an uncolored pattern is in the base space.
        if (info->base) return to_rgba(info->base, c, alpha);
        return {0, 0, 0, result.a};
    default:
        unsupported("color space model " + std::to_string(static_cast<int>(model)));
    }
    return result;
}

Address Quartz::make_color(Address space, std::vector<double> components, Address pattern) {
    auto info = std::make_shared<ColorInfo>();
    info->space = retain(space);
    info->components = std::move(components);
    info->pattern = retain(pattern);
    const auto block = allocate(info->components.size() * 8);
    info->guest_components = block.address;
    info->guest_heap = block.heap;
    for (std::size_t index = 0; index < info->components.size(); ++index)
        write_double(block.address + index * 8, info->components[index]);
    return create(color_class, info, [this, info](Address) {
        release(std::exchange(info->space, 0));
        release(std::exchange(info->pattern, 0));
        free({std::exchange(info->guest_components, 0), info->guest_heap});
    });
}

Rgba Quartz::color_rgba(Address color) {
    const auto info = require<ColorInfo>(color, "CGColor");
    const auto count = info->components.empty() ? 0 : info->components.size() - 1;
    return to_rgba(info->space, std::span(info->components.data(), count), info->alpha());
}

void Quartz::install_colors() {
    const auto cg = core_graphics_library;
    color_space_class = declare_class("_AlchemyCGColorSpace");
    color_class = declare_class("_AlchemyCGColor");
    pattern_class = declare_class("_AlchemyCGPattern");
    for (const auto& entry : named_spaces) string_constant(cg, "_" + std::string(entry.name), entry.name);
    for (const auto* name : {"kCGColorWhite", "kCGColorBlack", "kCGColorClear"}) string_constant(cg, "_" + std::string(name), name);

    const auto space = [this](Address object) { return space_info(object); };
    function(cg, "_CGColorSpaceCreateDeviceRGB", [this](CpuState& s, GuestMemory&) { s.x[0] = retain(device_rgb()); });
    function(cg, "_CGColorSpaceCreateDeviceGray", [this](CpuState& s, GuestMemory&) { s.x[0] = retain(device_gray()); });
    function(cg, "_CGColorSpaceCreateDeviceCMYK", [this](CpuState& s, GuestMemory&) { s.x[0] = make_color_space(ColorModel::cmyk, ""); });
    function(cg, "_CGColorSpaceCreateWithName", [this](CpuState& s, GuestMemory&) {
        if (!s.x[0] || cf.kind(s.x[0]) != ObjectKind::string) { s.x[0] = 0; return; }
        const auto name = cf.utf8(s.x[0]);
        const auto* named = find_named(name);
        if (!named) { s.x[0] = 0; return; }
        if (name == "kCGColorSpaceSRGB") { s.x[0] = retain(srgb()); return; }
        s.x[0] = make_color_space(named->model, name);
    });
    // Calibrated / ICC-based spaces are not color managed: their model decides.
    function(cg, "_CGColorSpaceCreateCalibratedRGB", [this](CpuState& s, GuestMemory&) { s.x[0] = make_color_space(ColorModel::rgb, "kCGColorSpaceGenericRGB"); });
    function(cg, "_CGColorSpaceCreateCalibratedGray", [this](CpuState& s, GuestMemory&) { s.x[0] = make_color_space(ColorModel::monochrome, "kCGColorSpaceGenericGray"); });
    function(cg, "_CGColorSpaceCreateLab", [this](CpuState& s, GuestMemory&) { s.x[0] = make_color_space(ColorModel::lab, "kCGColorSpaceGenericLab"); });
    const auto from_icc = [this](std::vector<std::uint8_t> bytes) -> Address {
        // ICC header: data colour space signature at offset 16.
        if (bytes.size() < 128) return 0;
        const std::string signature(bytes.begin() + 16, bytes.begin() + 20);
        ColorModel model{};
        if (signature == "RGB ") model = ColorModel::rgb;
        else if (signature == "GRAY") model = ColorModel::monochrome;
        else if (signature == "CMYK") model = ColorModel::cmyk;
        else if (signature == "Lab ") model = ColorModel::lab;
        else return 0;
        const auto value = make_color_space(model, "");
        const auto info = space_info(value);
        if (const auto owner = named_icc_profile_owner(bytes); !owner.empty()) {
            const auto* named = find_named(owner);
            info->profile = std::string(owner);
            info->linear = named->linear;
            info->wide = named->wide;
        }
        info->icc = std::move(bytes);
        return value;
    };
    for (const auto* name : {"_CGColorSpaceCreateWithICCData", "_CGColorSpaceCreateWithICCProfile"})
        function(cg, name, [this, from_icc](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? from_icc(data_bytes(s.x[0])) : 0; });
    function(cg, "_CGColorSpaceCreateICCBased", [this](CpuState& s, GuestMemory&) {
        // (nComponents, range, profile data provider, alternate)
        const auto count = s.x[0];
        if (s.x[3]) { s.x[0] = retain(s.x[3]); return; }
        s.x[0] = make_color_space(count == 1 ? ColorModel::monochrome : count == 4 ? ColorModel::cmyk : ColorModel::rgb, "");
    });
    function(cg, "_CGColorSpaceCreatePattern", [this, space](CpuState& s, GuestMemory&) {
        const auto value = make_color_space(ColorModel::pattern, "");
        if (s.x[0]) { static_cast<void>(space(s.x[0])); space_info(value)->base = retain(s.x[0]); }
        s.x[0] = value;
    });
    function(cg, "_CGColorSpaceCreateIndexed", [this, space](CpuState& s, GuestMemory& m) {
        // (baseSpace, lastIndex, colorTable)
        if (!s.x[0] || !s.x[2] || s.x[1] > 255) { s.x[0] = 0; return; }
        const auto base = space(s.x[0]);
        const auto count = (s.x[1] + 1) * base->components();
        std::vector<std::uint8_t> table(static_cast<std::size_t>(count));
        m.read_bytes(s.x[2], table);
        const auto value = make_color_space(ColorModel::indexed, "");
        const auto info = space_info(value);
        info->base = retain(s.x[0]);
        info->table = std::move(table);
        info->last_index = static_cast<std::uint32_t>(s.x[1]);
        s.x[0] = value;
    });
    function(cg, "_CGColorSpaceRetain", [this, space](CpuState& s, GuestMemory&) {
        if (s.x[0]) { static_cast<void>(space(s.x[0])); s.x[0] = retain(s.x[0]); }
    });
    function(cg, "_CGColorSpaceRelease", [this, space](CpuState& s, GuestMemory&) {
        if (s.x[0]) { static_cast<void>(space(s.x[0])); release(s.x[0]); }
    });
    function(cg, "_CGColorSpaceCopyName", [this, space](CpuState& s, GuestMemory&) {
        const auto name = s.x[0] ? space(s.x[0])->name : std::string();
        s.x[0] = name.empty() ? 0 : cf.string(name);
    });
    function(cg, "_CGColorSpaceGetName", [this, space](CpuState& s, GuestMemory&) {
        const auto name = s.x[0] ? space(s.x[0])->name : std::string();
        s.x[0] = name.empty() ? 0 : cf.constant_string(name);
    });
    function(cg, "_CGColorSpaceGetModel", [space](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] ? static_cast<std::uint32_t>(static_cast<std::int32_t>(space(s.x[0])->model)) : static_cast<std::uint32_t>(-1);
    });
    function(cg, "_CGColorSpaceGetNumberOfComponents", [space](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? space(s.x[0])->components() : 0; });
    function(cg, "_CGColorSpaceGetBaseColorSpace", [space](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? space(s.x[0])->base : 0; });
    function(cg, "_CGColorSpaceGetColorTableCount", [space](CpuState& s, GuestMemory&) {
        const auto info = s.x[0] ? space(s.x[0]) : nullptr;
        s.x[0] = info && info->model == ColorModel::indexed ? info->last_index + 1 : 0;
    });
    function(cg, "_CGColorSpaceGetColorTable", [space](CpuState& s, GuestMemory& m) {
        const auto info = s.x[0] ? space(s.x[0]) : nullptr;
        if (info && s.x[1] && !info->table.empty()) m.write_bytes(s.x[1], info->table);
    });
    function(cg, "_CGColorSpaceIsWideGamutRGB", [space](CpuState& s, GuestMemory&) { return_bool(s, s.x[0] && space(s.x[0])->wide); });
    function(cg, "_CGColorSpaceUsesExtendedRange", [space](CpuState& s, GuestMemory&) { return_bool(s, s.x[0] && space(s.x[0])->extended); });
    function(cg, "_CGColorSpaceSupportsOutput", [space](CpuState& s, GuestMemory&) {
        const auto model = s.x[0] ? space(s.x[0])->model : ColorModel::unknown;
        return_bool(s, model == ColorModel::rgb || model == ColorModel::monochrome || model == ColorModel::cmyk);
    });
    // A space made from ICC data answers those bytes; a named RGB or gray space
    // its generated profile (a new immutable CFData of the cached bytes); CMYK,
    // Lab, XYZ and device spaces have none (NULL).
    function(cg, "_CGColorSpaceCopyICCData", [this, space](CpuState& s, GuestMemory&) {
        const auto info = s.x[0] ? space(s.x[0]) : nullptr;
        const auto* profile = !info ? nullptr : !info->icc.empty() ? &info->icc : named_icc_profile(info->name);
        s.x[0] = profile ? data_object(*profile) : 0;
    });
    function(cg, "_CGColorSpaceGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });

    // ---- CGColor ----
    const auto color = [this](Address object) { return require<ColorInfo>(object, "CGColor"); };
    function(cg, "_CGColorCreate", [this, space](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) { s.x[0] = 0; return; }
        const auto info = space(s.x[0]);
        if (info->model == ColorModel::pattern) { s.x[0] = 0; return; }
        std::vector<double> components(info->components() + 1);
        for (std::size_t index = 0; index < components.size(); ++index)
            components[index] = std::bit_cast<double>(m.read64(s.x[1] + index * 8));
        s.x[0] = make_color(s.x[0], std::move(components));
    });
    function(cg, "_CGColorCreateGenericRGB", [this](CpuState& s, GuestMemory&) {
        s.x[0] = make_color(srgb(), {d(s, 0), d(s, 1), d(s, 2), d(s, 3)});
    });
    function(cg, "_CGColorCreateSRGB", [this](CpuState& s, GuestMemory&) { s.x[0] = make_color(srgb(), {d(s, 0), d(s, 1), d(s, 2), d(s, 3)}); });
    for (const auto* name : {"_CGColorCreateGenericGray", "_CGColorCreateGenericGrayGamma2_2"})
        function(cg, name, [this](CpuState& s, GuestMemory&) { s.x[0] = make_color(device_gray(), {d(s, 0), d(s, 1)}); });
    function(cg, "_CGColorCreateGenericCMYK", [this](CpuState& s, GuestMemory&) {
        const auto cmyk = make_color_space(ColorModel::cmyk, "kCGColorSpaceGenericCMYK");
        s.x[0] = make_color(cmyk, {d(s, 0), d(s, 1), d(s, 2), d(s, 3), d(s, 4)});
        release(cmyk);
    });
    function(cg, "_CGColorCreateWithPattern", [this, space](CpuState& s, GuestMemory& m) {
        // (space, pattern, components)
        if (!s.x[0] || !s.x[1]) { s.x[0] = 0; return; }
        const auto info = space(s.x[0]);
        if (info->model != ColorModel::pattern) { s.x[0] = 0; return; }
        static_cast<void>(require<PatternInfo>(s.x[1], "CGPattern"));
        const auto base_count = info->base ? space(info->base)->components() : 0;
        std::vector<double> components(base_count + 1, 1.0);
        if (s.x[2]) for (std::size_t index = 0; index < components.size(); ++index) components[index] = std::bit_cast<double>(m.read64(s.x[2] + index * 8));
        s.x[0] = make_color(s.x[0], std::move(components), s.x[1]);
    });
    function(cg, "_CGColorGetConstantColor", [this](CpuState& s, GuestMemory&) {
        const auto name = s.x[0] ? cf.utf8(s.x[0]) : std::string();
        auto& constants = constant_colors;
        std::lock_guard lock(mutex);
        if (const auto found = constants.find(name); found != constants.end()) { s.x[0] = found->second; return; }
        std::vector<double> components;
        if (name == "kCGColorWhite") components = {1, 1};
        else if (name == "kCGColorBlack") components = {0, 1};
        else if (name == "kCGColorClear") components = {0, 0};
        else { s.x[0] = 0; return; }
        const auto value = make_color(device_gray(), components);
        if (const auto record = process.objects().find(value)) record->immortal = true;
        constants.emplace(name, value);
        s.x[0] = value;
    });
    function(cg, "_CGColorCreateCopy", [this, color](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        const auto info = color(s.x[0]);
        s.x[0] = make_color(info->space, info->components, info->pattern);
    });
    function(cg, "_CGColorCreateCopyWithAlpha", [this, color](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        const auto info = color(s.x[0]);
        auto components = info->components;
        if (!components.empty()) components.back() = d(s, 0);
        s.x[0] = make_color(info->space, std::move(components), info->pattern);
    });
    function(cg, "_CGColorCreateCopyByMatchingToColorSpace", [this, space](CpuState& s, GuestMemory&) {
        // (space, intent, color, options)
        if (!s.x[0] || !s.x[2]) { s.x[0] = 0; return; }
        const auto target = space(s.x[0]);
        const auto rgba = color_rgba(s.x[2]);
        std::vector<double> components;
        switch (target->model) {
        case ColorModel::rgb: components = {rgba.r, rgba.g, rgba.b, rgba.a}; break;
        case ColorModel::monochrome: components = {0.2126 * rgba.r + 0.7152 * rgba.g + 0.0722 * rgba.b, rgba.a}; break;
        case ColorModel::cmyk: {
            const auto k = 1 - std::max({rgba.r, rgba.g, rgba.b});
            const auto scale = k < 1 ? 1 / (1 - k) : 0;
            components = {(1 - rgba.r - k) * scale, (1 - rgba.g - k) * scale, (1 - rgba.b - k) * scale, k, rgba.a};
            break;
        }
        default: s.x[0] = 0; return;
        }
        s.x[0] = make_color(s.x[0], std::move(components));
    });
    function(cg, "_CGColorEqualToColor", [this](CpuState& s, GuestMemory&) {
        if (s.x[0] == s.x[1]) { return_bool(s, true); return; }
        const auto a = find<ColorInfo>(s.x[0]), b = find<ColorInfo>(s.x[1]);
        if (!a || !b) { return_bool(s, false); return; }
        const auto sa = a->space ? space_info(a->space) : nullptr, sb = b->space ? space_info(b->space) : nullptr;
        const bool same_space = a->space == b->space || (sa && sb && sa->model == sb->model && sa->name == sb->name);
        return_bool(s, same_space && a->components == b->components && a->pattern == b->pattern);
    });
    function(cg, "_CGColorGetAlpha", [color](CpuState& s, GuestMemory&) { set_d(s, 0, s.x[0] ? color(s.x[0])->alpha() : 0); });
    function(cg, "_CGColorGetColorSpace", [color](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? color(s.x[0])->space : 0; });
    function(cg, "_CGColorGetComponents", [color](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? color(s.x[0])->guest_components : 0; });
    function(cg, "_CGColorGetNumberOfComponents", [color](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? color(s.x[0])->components.size() : 0; });
    function(cg, "_CGColorGetPattern", [color](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? color(s.x[0])->pattern : 0; });
    function(cg, "_CGColorRetain", [this](CpuState& s, GuestMemory&) { s.x[0] = retain(s.x[0]); });
    function(cg, "_CGColorRelease", [this](CpuState& s, GuestMemory&) { release(s.x[0]); });
    function(cg, "_CGColorGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });
}

} // namespace alchemy::gui::quartz2d
