#include "alchemy/darwin/colorsync/profiles.hpp"
#include "alchemy/darwin/coregraphics/icc_profiles.hpp"
#include "alchemy/gui/value_provider.hpp"
#include "alchemy/host/crypto.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// ColorSync profiles (alchemy-fx): ColorSyncProfileRef objects over ICC
// profile bytes. Profiles come from data, files, or the names of the
// profiles CoreGraphics generates for its named color spaces
// (coregraphics/icc_profiles.hpp; the kColorSync*Profile names are those
// color spaces' names). Reading is real: the data, the 128-byte header, tags
// by signature, the description ('desc' or 'mluc'), the profile ID (MD5 of
// the profile with the header's flags, rendering intent and ID zeroed, as
// ICC.1 specifies, when the header carries none), and a structural check
// (header size and signature, tag table within the profile). Transforms
// (ColorSyncTransformCreate) need a color management module and stop
// explicitly; no display profile is read from the host, so
// ColorSyncProfileCreateWithDisplayID answers NULL, as for an unknown display.
namespace alchemy::colorsync {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view library = "/System/Library/Frameworks/ColorSync.framework/Versions/A/ColorSync";
constexpr std::string_view core_foundation = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr std::string_view error_domain = "com.apple.ColorSync";   // kColorSyncErrorDomain (value unverified)
constexpr std::size_t header_size = 128;

std::uint32_t be32(const std::vector<std::uint8_t>& b, std::size_t at) {
    return at + 4 <= b.size() ? (std::uint32_t{b[at]} << 24) | (std::uint32_t{b[at + 1]} << 16) | (std::uint32_t{b[at + 2]} << 8) | b[at + 3] : 0;
}
std::uint32_t signature(const std::string& text) {
    std::uint32_t value{};
    for (std::size_t i = 0; i < 4; ++i) value = (value << 8) | static_cast<std::uint8_t>(i < text.size() ? text[i] : ' ');
    return value;
}
// The structural problems of a profile ("" when none).
std::string problem_of(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < header_size + 4) return "the data is shorter than an ICC header and tag count";
    if (be32(bytes, 36) != signature("acsp")) return "the header has no 'acsp' signature";
    if (be32(bytes, 0) > bytes.size()) return "the header's size is larger than the data";
    const auto count = be32(bytes, header_size);
    if (header_size + 4 + std::uint64_t{count} * 12 > bytes.size()) return "the tag table runs past the data";
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto entry = header_size + 4 + std::size_t{i} * 12;
        if (std::uint64_t{be32(bytes, entry + 4)} + be32(bytes, entry + 8) > bytes.size()) return "a tag runs past the data";
    }
    return {};
}
std::optional<std::vector<std::uint8_t>> tag_of(const std::vector<std::uint8_t>& bytes, std::uint32_t wanted) {
    if (!problem_of(bytes).empty()) return std::nullopt;
    const auto count = be32(bytes, header_size);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto entry = header_size + 4 + std::size_t{i} * 12;
        if (be32(bytes, entry) != wanted) continue;
        const auto offset = be32(bytes, entry + 4), size = be32(bytes, entry + 8);
        return std::vector<std::uint8_t>(bytes.begin() + offset, bytes.begin() + offset + size);
    }
    return std::nullopt;
}
// The profile description: a 'desc' (ICC v2) or 'mluc' (v4, first record) tag.
std::optional<std::u16string> description_of(const std::vector<std::uint8_t>& bytes) {
    const auto tag = tag_of(bytes, signature("desc"));
    if (!tag || tag->size() < 12) return std::nullopt;
    const auto type = be32(*tag, 0);
    if (type == signature("desc")) {
        const auto count = be32(*tag, 8);
        std::u16string text;
        for (std::size_t i = 0; i < count && 12 + i < tag->size(); ++i)
            if ((*tag)[12 + i]) text.push_back(static_cast<char16_t>((*tag)[12 + i]));
        return text;
    }
    if (type == signature("mluc") && tag->size() >= 28 && be32(*tag, 8) > 0) {
        const auto length = be32(*tag, 20), offset = be32(*tag, 24);
        std::u16string text;
        for (std::size_t i = 0; i + 1 < length && offset + i + 1 < tag->size(); i += 2)
            text.push_back(static_cast<char16_t>(((*tag)[offset + i] << 8) | (*tag)[offset + i + 1]));
        return text;
    }
    return std::nullopt;
}

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    Address profile_class{};
    void install();

    Address make_data(const std::vector<std::uint8_t>& bytes) {
        const auto block = process.allocate_guest(std::max<std::size_t>(bytes.size(), 1));
        memory().write_bytes(block, bytes);
        const auto data = call(core_foundation, "_CFDataCreate", {0, block, bytes.size()});
        process.release_guest(block);
        return data;   // +1
    }
    std::vector<std::uint8_t> bytes_of(Address data) {
        const auto length = call(core_foundation, "_CFDataGetLength", {data});
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::min<Address>(length, 1u << 30)));
        if (!bytes.empty()) memory().read_bytes(call(core_foundation, "_CFDataGetBytePtr", {data}), bytes);
        return bytes;
    }
    std::vector<std::uint8_t> profile_bytes(Address profile) {
        if (!profile) throw std::runtime_error("Alchemy: a ColorSync function needs a ColorSyncProfileRef");
        const auto data = stored(profile, "data");
        return data ? bytes_of(data) : std::vector<std::uint8_t>{};
    }
    // A profile (+1) of `bytes`, or 0 with *error_out (+1) set.
    Address make_profile(const std::vector<std::uint8_t>& bytes, Address url, Address error_out) {
        if (const auto problem = problem_of(bytes); !problem.empty()) {
            if (error_out) memory().write64(error_out, retain(bridge->error(error_domain, -1, "Not an ICC profile: " + problem)));
            return 0;
        }
        const auto profile = create(profile_class);
        const auto data = make_data(bytes);
        store(profile, "data", data);
        release(data);
        if (url) store(profile, "url", url);
        return profile;
    }
};

void Provider::install() {
    profile_class = cls(library, "__AlchemyColorSyncProfile", "NSObject");
    cleanup_on_dealloc(profile_class);
    for (const auto& [symbol, name] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_kColorSyncSRGBProfile", "kCGColorSpaceSRGB"}, {"_kColorSyncGenericRGBProfile", "kCGColorSpaceGenericRGB"},
             {"_kColorSyncGenericGrayProfile", "kCGColorSpaceGenericGray"},
             {"_kColorSyncGenericGrayGamma22Profile", "kCGColorSpaceGenericGrayGamma2_2"},
             {"_kColorSyncDisplayP3Profile", "kCGColorSpaceDisplayP3"}, {"_kColorSyncDCIP3Profile", "kCGColorSpaceDCIP3"},
             {"_kColorSyncAdobeRGB1998Profile", "kCGColorSpaceAdobeRGB1998"}, {"_kColorSyncITUR709Profile", "kCGColorSpaceITUR_709"},
             {"_kColorSyncITUR2020Profile", "kCGColorSpaceITUR_2020"}, {"_kColorSyncROMMRGBProfile", "kCGColorSpaceROMMRGB"},
             {"_kColorSyncACESCGLinearProfile", "kCGColorSpaceACESCGLinear"}})
        string(library, symbol, name);
    string(library, "_kColorSyncErrorDomain", error_domain);

    raw(library, "_ColorSyncProfileCreate", [this](CpuState& s, GuestMemory&) {
        // (CFDataRef, CFErrorRef*)
        if (s.x[1]) memory().write64(s.x[1], 0);
        s.x[0] = s.x[0] ? make_profile(bytes_of(s.x[0]), 0, s.x[1]) : 0;
    });
    raw(library, "_ColorSyncProfileCreateWithURL", [this](CpuState& s, GuestMemory&) {
        if (s.x[1]) memory().write64(s.x[1], 0);
        CallRegs registers;
        registers.x[2] = s.x[0];
        const auto data = s.x[0] ? objc.send(objc.find_class("NSData"), "dataWithContentsOfURL:", registers).x0 : 0;
        if (!data) {
            if (s.x[1]) memory().write64(s.x[1], retain(bridge->error(error_domain, -1, "The profile file could not be read")));
            s.x[0] = 0;
            return;
        }
        s.x[0] = make_profile(bytes_of(data), s.x[0], s.x[1]);
    });
    raw(library, "_ColorSyncProfileCreateWithName", [this](CpuState& s, GuestMemory&) {
        const auto* bytes = s.x[0] ? gui::quartz2d::named_icc_profile(cf.utf8(s.x[0])) : nullptr;
        s.x[0] = bytes ? make_profile(*bytes, 0, 0) : 0;
    });
    // No display profile is read from the host: NULL, as for an unknown display.
    raw(library, "_ColorSyncProfileCreateWithDisplayID", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    raw(library, "_ColorSyncProfileCopyData", [this](CpuState& s, GuestMemory&) {
        if (s.x[1]) memory().write64(s.x[1], 0);
        s.x[0] = make_data(profile_bytes(s.x[0]));
    });
    raw(library, "_ColorSyncProfileCopyHeader", [this](CpuState& s, GuestMemory&) {
        auto bytes = profile_bytes(s.x[0]);
        bytes.resize(std::min(bytes.size(), header_size));
        s.x[0] = make_data(bytes);
    });
    raw(library, "_ColorSyncProfileCopyTag", [this](CpuState& s, GuestMemory&) {
        const auto tag = s.x[1] ? tag_of(profile_bytes(s.x[0]), signature(cf.utf8(s.x[1]))) : std::nullopt;
        s.x[0] = tag ? make_data(*tag) : 0;
    });
    raw(library, "_ColorSyncProfileContainsTag", [this](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[1] && tag_of(profile_bytes(s.x[0]), signature(cf.utf8(s.x[1]))) ? 1 : 0;
    });
    raw(library, "_ColorSyncProfileCopyDescriptionString", [this](CpuState& s, GuestMemory&) {
        const auto text = description_of(profile_bytes(s.x[0]));
        if (!text) { s.x[0] = 0; return; }
        std::string utf8;
        for (const auto unit : *text) {
            if (unit < 0x80) utf8.push_back(static_cast<char>(unit));
            else if (unit < 0x800) { utf8.push_back(static_cast<char>(0xc0 | (unit >> 6))); utf8.push_back(static_cast<char>(0x80 | (unit & 0x3f))); }
            else { utf8.push_back(static_cast<char>(0xe0 | (unit >> 12))); utf8.push_back(static_cast<char>(0x80 | ((unit >> 6) & 0x3f))); utf8.push_back(static_cast<char>(0x80 | (unit & 0x3f))); }
        }
        s.x[0] = cf.string(utf8);
    });
    raw(library, "_ColorSyncProfileGetURL", [this](CpuState& s, GuestMemory&) {
        if (s.x[1]) memory().write64(s.x[1], 0);
        s.x[0] = s.x[0] ? stored(s.x[0], "url") : 0;
    });
    // ColorSyncMD5 (16 bytes, returned in x0 / x1): the header's profile ID,
    // else the MD5 of the profile with flags, rendering intent and ID zeroed.
    raw(library, "_ColorSyncProfileGetMD5", [this](CpuState& s, GuestMemory&) {
        auto bytes = profile_bytes(s.x[0]);
        std::array<std::uint8_t, 16> digest{};
        if (bytes.size() >= 100) std::copy(bytes.begin() + 84, bytes.begin() + 100, digest.begin());
        if (std::all_of(digest.begin(), digest.end(), [](std::uint8_t b) { return b == 0; }) && bytes.size() >= header_size) {
            for (const auto& [from, to] : std::initializer_list<std::pair<std::size_t, std::size_t>>{{44, 48}, {64, 68}, {84, 100}})
                std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(from), bytes.begin() + static_cast<std::ptrdiff_t>(to), 0);
            const auto hashed = host::crypto::hash(host::crypto::Digest::md5, std::as_bytes(std::span(bytes)));
            if (!hashed || hashed->size() != 16) throw std::runtime_error("Alchemy: ColorSyncProfileGetMD5 is unsupported (the host has no MD5)");
            std::copy(hashed->begin(), hashed->end(), digest.begin());
        }
        std::uint64_t low{}, high{};
        for (int i = 7; i >= 0; --i) { low = (low << 8) | digest[static_cast<std::size_t>(i)]; high = (high << 8) | digest[static_cast<std::size_t>(i) + 8]; }
        s.x[0] = low;
        s.x[1] = high;
    });
    raw(library, "_ColorSyncProfileVerify", [this](CpuState& s, GuestMemory&) {
        // (profile, CFErrorRef* errors, CFErrorRef* warnings)
        const auto problem = problem_of(profile_bytes(s.x[0]));
        if (s.x[2]) memory().write64(s.x[2], 0);
        if (s.x[1]) memory().write64(s.x[1], problem.empty() ? 0 : retain(bridge->error(error_domain, -1, problem)));
        s.x[0] = problem.empty() ? 1 : 0;
    });
    raw(library, "_ColorSyncProfileGetTypeID", [this](CpuState& s, GuestMemory&) {
        const auto sample = create(profile_class);
        s.x[0] = call(core_foundation, "_CFGetTypeID", {sample});
        release(sample);
    });
    raw(library, "_ColorSyncTransformCreate", [](CpuState&, GuestMemory&) {
        throw std::runtime_error("Alchemy: ColorSyncTransformCreate is unsupported (no color management module converts between ICC profiles)");
    });
}

} // namespace

void install_colorsync_profiles(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::colorsync
