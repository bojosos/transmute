#include "alchemy/darwin/coregraphics/icc_profiles.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>

// ICC.1:2022 (v4.4) display-class ('mntr') RGB and gray profiles, written from
// the published parameters of each standard:
// - sRGB: IEC 61966-2-1:1999 (BT.709 primaries, D65, the piecewise sRGB curve).
// - Display P3: SMPTE EG 432-1:2010 primaries with the D65 white point and the
//   sRGB curve (the Display P3 definition); DCI-P3: SMPTE RP 431-2:2011 (same
//   primaries, DCI white x 0.314 y 0.351, gamma 2.6).
// - ITU-R BT.709-6 and BT.2020-2 primaries, D65, decoded with the inverse of
//   the BT.709 OETF (V = 1.099 L^0.45 - 0.099 above L 0.018, else 4.5 L).
//   BT.2020 states 1.0993 / 0.0181 for 12-bit; the 10-bit (BT.709) constants
//   are used. UNSURE: Apple's ITU-R 709/2020 spaces may decode with BT.1886
//   (gamma 2.4) instead; nothing public says which.
// - Adobe RGB (1998) Color Image Encoding, version 2005-05: primaries, D65,
//   gamma 563/256.
// - ROMM RGB: ISO 22028-2:2013 (D50, gamma 1.8 with a linear segment of slope
//   16 below 1/512).
// - ACEScg: SMPTE ST 2065-1 / Academy S-2014-004 (AP1 primaries, ACES white
//   x 0.32168 y 0.33767, linear).
// - Gray: gamma 2.2 (kCGColorSpaceGenericGrayGamma2_2, by its name) or linear.
// Apple's own spaces with no public definition share a standard profile, the
// same way Alchemy draws them (color.cpp), and are flagged UNSURE below:
// Generic RGB (sRGB), Generic RGB Linear (linear sRGB), Generic Gray and
// Extended Gray (gamma 2.2). Extended-range spaces share the profile of
// their standard-range space (the encoding is the same curve, extended).
// Each profile: the header (D50 PCS illuminant), desc and cprt (mluc), wtpt
// (D50, as v4 requires for display profiles), chad (Bradford adaptation from
// the standard's white to D50), the D50-adapted colorants rXYZ/gXYZ/bXYZ and
// one parametric curve shared by the TRC tags (kTRC for gray). The profile ID
// is zero (not computed), which ICC.1 allows.
namespace alchemy::gui::quartz2d {
namespace {

struct Curve { std::uint16_t function; std::array<double, 5> parameters; }; // ICC 'para': g, a, b, c, d
struct Xy { double x, y; };
struct Family {
    std::string_view description;
    bool gray;
    Xy red, green, blue, white;
    Curve curve;
    std::array<std::string_view, 4> names; // names[0] owns the profile (named_icc_profile_owner)
};

constexpr Curve srgb_curve{3, {2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045}};
constexpr Curve linear_curve{0, {1.0}};
constexpr Curve gamma_2_2{0, {2.2}};
constexpr Curve bt709_curve{3, {1 / 0.45, 1 / 1.099, 0.099 / 1.099, 1 / 4.5, 0.081}};
constexpr Xy d65{0.3127, 0.3290}, d50_white{0.3457, 0.3585};
constexpr Xy bt709_red{0.640, 0.330}, bt709_green{0.300, 0.600}, bt709_blue{0.150, 0.060};
constexpr Xy p3_red{0.680, 0.320}, p3_green{0.265, 0.690}, p3_blue{0.150, 0.060};

const Family families[] = {
    // UNSURE: Generic RGB is Apple's own profile; Alchemy draws it as sRGB.
    {"sRGB IEC61966-2.1", false, bt709_red, bt709_green, bt709_blue, d65, srgb_curve,
     {"kCGColorSpaceSRGB", "kCGColorSpaceExtendedSRGB", "kCGColorSpaceGenericRGB"}},
    // UNSURE: Generic RGB Linear, as above (linear sRGB).
    {"Linear sRGB", false, bt709_red, bt709_green, bt709_blue, d65, linear_curve,
     {"kCGColorSpaceLinearSRGB", "kCGColorSpaceExtendedLinearSRGB", "kCGColorSpaceGenericRGBLinear"}},
    {"Display P3", false, p3_red, p3_green, p3_blue, d65, srgb_curve,
     {"kCGColorSpaceDisplayP3", "kCGColorSpaceExtendedDisplayP3"}},
    {"DCI-P3", false, p3_red, p3_green, p3_blue, {0.314, 0.351}, {0, {2.6}}, {"kCGColorSpaceDCIP3"}},
    {"Adobe RGB (1998)", false, {0.640, 0.330}, {0.210, 0.710}, {0.150, 0.060}, d65, {0, {563.0 / 256}},
     {"kCGColorSpaceAdobeRGB1998"}},
    {"ITU-R BT.709", false, bt709_red, bt709_green, bt709_blue, d65, bt709_curve, {"kCGColorSpaceITUR_709"}},
    {"ITU-R BT.2020", false, {0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}, d65, bt709_curve, {"kCGColorSpaceITUR_2020"}},
    {"ROMM RGB", false, {0.7347, 0.2653}, {0.1596, 0.8404}, {0.0366, 0.0001}, d50_white,
     {3, {1.8, 1, 0, 1.0 / 16, 16.0 / 512}}, {"kCGColorSpaceROMMRGB"}},
    {"ACEScg", false, {0.713, 0.293}, {0.165, 0.830}, {0.128, 0.044}, {0.32168, 0.33767}, linear_curve,
     {"kCGColorSpaceACESCGLinear"}},
    // UNSURE: Generic Gray and Extended Gray are taken as gamma 2.2.
    {"Gray Gamma 2.2", true, {}, {}, {}, d65, gamma_2_2,
     {"kCGColorSpaceGenericGrayGamma2_2", "kCGColorSpaceGenericGray", "kCGColorSpaceExtendedGray"}},
    {"Linear Gray", true, {}, {}, {}, d65, linear_curve, {"kCGColorSpaceLinearGray", "kCGColorSpaceExtendedLinearGray"}},
};

using Vector = std::array<double, 3>;
using Matrix = std::array<Vector, 3>;
constexpr Vector d50{0.9642, 1.0, 0.8249}; // ICC PCS illuminant

Vector to_xyz(Xy value) { return {value.x / value.y, 1.0, (1 - value.x - value.y) / value.y}; }
Vector multiply(const Matrix& m, const Vector& v) {
    return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
}
Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix result{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            for (int k = 0; k < 3; ++k) result[row][column] += a[row][k] * b[k][column];
    return result;
}
Matrix invert(const Matrix& m) {
    const auto det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                     m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    Matrix r{};
    r[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
    r[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
    r[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
    r[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
    r[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
    r[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
    r[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
    r[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
    r[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
    return r;
}
// Bradford chromatic adaptation from `white` to D50 (ICC.1 Annex E).
Matrix bradford_to_d50(const Vector& white) {
    constexpr Matrix bradford{{{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}}};
    const auto source = multiply(bradford, white), target = multiply(bradford, d50);
    Matrix scale{};
    for (int i = 0; i < 3; ++i) scale[i][i] = target[i] / source[i];
    return multiply(invert(bradford), multiply(scale, bradford));
}

class Writer {
public:
    void u8(std::uint8_t v) { bytes.push_back(v); }
    void u16(std::uint16_t v) { u8(static_cast<std::uint8_t>(v >> 8)); u8(static_cast<std::uint8_t>(v)); }
    void u32(std::uint32_t v) { u16(static_cast<std::uint16_t>(v >> 16)); u16(static_cast<std::uint16_t>(v)); }
    void signature(std::string_view four) { for (const auto c : four) u8(static_cast<std::uint8_t>(c)); }
    void fixed(double v) { u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(v * 65536)))); }
    void pad() { while (bytes.size() % 4) u8(0); }
    std::vector<std::uint8_t> bytes;
};

std::vector<std::uint8_t> mluc(std::string_view text) {
    Writer w;
    w.signature("mluc"); w.u32(0); w.u32(1); w.u32(12);
    w.signature("en"); w.signature("US"); w.u32(static_cast<std::uint32_t>(text.size() * 2)); w.u32(28);
    for (const auto c : text) w.u16(static_cast<unsigned char>(c));
    return w.bytes;
}
std::vector<std::uint8_t> xyz(const Vector& value) {
    Writer w;
    w.signature("XYZ "); w.u32(0);
    for (const auto component : value) w.fixed(component);
    return w.bytes;
}
std::vector<std::uint8_t> parametric(const Curve& curve) {
    constexpr std::array<std::size_t, 5> counts{1, 3, 4, 5, 7};
    Writer w;
    w.signature("para"); w.u32(0); w.u16(curve.function); w.u16(0);
    for (std::size_t index = 0; index < counts.at(curve.function); ++index) w.fixed(curve.parameters.at(index));
    return w.bytes;
}

std::vector<std::uint8_t> build(const Family& family) {
    // Tag signature and data; tags with equal data share one copy (TRCs).
    std::vector<std::pair<std::string_view, std::vector<std::uint8_t>>> tags;
    tags.emplace_back("desc", mluc(family.description));
    tags.emplace_back("cprt", mluc("No copyright, use freely (Alchemy project-authored profile)"));
    tags.emplace_back("wtpt", xyz(d50));
    const auto curve = parametric(family.curve);
    if (family.gray) {
        tags.emplace_back("kTRC", curve);
    } else {
        const auto white = to_xyz(family.white);
        const Matrix primaries{{{to_xyz(family.red)[0], to_xyz(family.green)[0], to_xyz(family.blue)[0]},
                                {1.0, 1.0, 1.0},
                                {to_xyz(family.red)[2], to_xyz(family.green)[2], to_xyz(family.blue)[2]}}};
        const auto scale = multiply(invert(primaries), white);
        const auto adapt = bradford_to_d50(white);
        Writer chad;
        chad.signature("sf32"); chad.u32(0);
        for (const auto& row : adapt) for (const auto value : row) chad.fixed(value);
        tags.emplace_back("chad", chad.bytes);
        constexpr std::array<std::string_view, 3> colorants{"rXYZ", "gXYZ", "bXYZ"};
        for (int channel = 0; channel < 3; ++channel) {
            const Vector column{primaries[0][channel] * scale[channel], primaries[1][channel] * scale[channel],
                                primaries[2][channel] * scale[channel]};
            tags.emplace_back(colorants[channel], xyz(multiply(adapt, column)));
        }
        for (const auto* trc : {"rTRC", "gTRC", "bTRC"}) tags.emplace_back(trc, curve);
    }

    Writer body; // tag data, offsets relative to the profile start
    const auto table_size = 4 + 12 * tags.size();
    std::vector<std::pair<std::uint32_t, std::uint32_t>> placed;
    std::map<std::vector<std::uint8_t>, std::uint32_t> shared;
    for (const auto& [signature, data] : tags) {
        const auto found = shared.find(data);
        if (found != shared.end()) { placed.emplace_back(found->second, static_cast<std::uint32_t>(data.size())); continue; }
        const auto offset = static_cast<std::uint32_t>(128 + table_size + body.bytes.size());
        body.bytes.insert(body.bytes.end(), data.begin(), data.end());
        body.pad();
        shared.emplace(data, offset);
        placed.emplace_back(offset, static_cast<std::uint32_t>(data.size()));
    }
    const auto total = static_cast<std::uint32_t>(128 + table_size + body.bytes.size());

    Writer w;
    w.u32(total);
    w.u32(0);                            // preferred CMM: none
    w.u32(0x04400000);                   // version 4.4
    w.signature("mntr");
    w.signature(family.gray ? "GRAY" : "RGB ");
    w.signature("XYZ ");
    constexpr std::array<std::uint16_t, 6> date{2026, 1, 1, 0, 0, 0}; // creation date (fixed: identical bytes)
    for (const auto field : date) w.u16(field);
    w.signature("acsp");
    w.u32(0);                            // primary platform: none
    w.u32(0);                            // flags
    w.u32(0); w.u32(0);                  // device manufacturer, model
    w.u32(0); w.u32(0);                  // device attributes
    w.u32(0);                            // rendering intent: perceptual
    for (const auto component : d50) w.fixed(component);
    w.u32(0);                            // creator: none
    for (int i = 0; i < 16 + 28; ++i) w.u8(0); // profile ID (not computed), reserved
    w.u32(static_cast<std::uint32_t>(tags.size()));
    for (std::size_t index = 0; index < tags.size(); ++index) {
        w.signature(tags[index].first);
        w.u32(placed[index].first);
        w.u32(placed[index].second);
    }
    w.bytes.insert(w.bytes.end(), body.bytes.begin(), body.bytes.end());
    return w.bytes;
}

struct Profiles {
    std::map<std::string_view, const std::vector<std::uint8_t>*> by_name;
    std::vector<std::pair<std::vector<std::uint8_t>, std::string_view>> built; // profile, owning name
    Profiles() {
        built.reserve(std::size(families));
        for (const auto& family : families) built.emplace_back(build(family), family.names[0]);
        for (std::size_t index = 0; index < std::size(families); ++index)
            for (const auto name : families[index].names)
                if (!name.empty()) by_name.emplace(name, &built[index].first);
    }
};
const Profiles& profiles() {
    static const Profiles value;
    return value;
}

} // namespace

const std::vector<std::uint8_t>* named_icc_profile(std::string_view name) {
    const auto& all = profiles();
    const auto found = all.by_name.find(name);
    return found == all.by_name.end() ? nullptr : found->second;
}

std::string_view named_icc_profile_owner(std::span<const std::uint8_t> bytes) {
    for (const auto& [profile, owner] : profiles().built)
        if (std::ranges::equal(profile, bytes)) return owner;
    return {};
}
} // namespace alchemy::gui::quartz2d
