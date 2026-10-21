#include "alchemy/darwin/kit/support/support_kit.hpp"

#include "alchemy/path.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

// CoreText: kCTFontLanguagesAttribute and CTFontManagerCreateFontDescriptorFromData
// (real: the data goes to a host temp file named by its hash, which
// DirectWrite loads).
// Qt 6.5's bind-time surface first needed them (moved from
// darwin/identities/qt_imports.cpp); installed by the support kit
// (kit/support/install.cpp) at the old position, only when missing.
namespace alchemy::gui::support_kit {

namespace {

constexpr std::string_view core_foundation = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr std::string_view core_text = "/System/Library/Frameworks/CoreText.framework/Versions/A/CoreText";

} // namespace

void install_coretext_font_data(Support& support) {
    auto* s = &support;
    if (!support.process.resolve(core_text, "_kCTFontLanguagesAttribute"))
        support.string_constant(core_text, "_kCTFontLanguagesAttribute", "NSCTFontLanguagesAttribute");
    // ---- CoreText: font descriptors from font data (real) ----
    support.fill_function(core_text, "_CTFontManagerCreateFontDescriptorFromData", [s](CpuState& c, GuestMemory& m) {
        if (!c.x[0]) { c.x[0] = 0; return; }
        const auto length = s->call(core_foundation, "_CFDataGetLength", {c.x[0]});
        const auto bytes = s->call(core_foundation, "_CFDataGetBytePtr", {c.x[0]});
        if (!length || !bytes) { c.x[0] = 0; return; }
        std::vector<std::uint8_t> data(static_cast<std::size_t>(length));
        m.read_bytes(bytes, data);
        // DirectWrite loads fonts from files: keep the data in a host temp
        // file named by its hash for the life of the process.
        std::uint64_t hash = 1469598103934665603ULL;
        for (const auto byte : data) hash = (hash ^ byte) * 1099511628211ULL;
        const auto directory = std::filesystem::temp_directory_path() / L"alchemy-fonts";
        std::filesystem::create_directories(directory);
        char name[40];
        std::snprintf(name, sizeof name, "%016llx.font", static_cast<unsigned long long>(hash));
        const auto path = directory / name;
        if (!std::filesystem::exists(path)) {
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }
        const auto guest = guest_path_string(path);   // the /Volumes/<drive> view of the host path
        const auto url = s->cf.file_url(guest, false);
        const auto descriptors = s->call(core_text, "_CTFontManagerCreateFontDescriptorsFromURL", {url});
        s->release(url);
        Address result{};
        if (descriptors && s->cf.length(descriptors) > 0) result = s->retain(s->cf.element(descriptors, 0));
        s->release(descriptors);
        c.x[0] = result;
    });
}

} // namespace alchemy::gui::support_kit
