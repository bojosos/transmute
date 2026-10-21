#include "alchemy/darwin/media/media.hpp"

#include <array>

// MediaAccessibility and ImageCaptureCore.
//
// - Caption appearance: no user caption style is stored under Alchemy, so
//   every query answers the system default style (white proportional sans
//   serif text, opaque black background, no window, no edge effect, normal
//   size) with behaviour kMACaptionAppearanceBehaviorUseValue.
// - ImageCaptureCore: no cameras or scanners exist; the classes are shells.
namespace alchemy::gui::fill {
namespace {

class Misc final : public Provider {
public:
    using Provider::Provider;

    Address color(double red, double green, double blue, double alpha) {
        CallRegs registers{};
        registers.set_d(0, red);
        registers.set_d(1, green);
        registers.set_d(2, blue);
        registers.set_d(3, alpha);
        return call_regs(core_graphics, "_CGColorCreateGenericRGB", registers).x0;
    }
    void install_media_accessibility() {
        string(media_accessibility, "_kMACaptionAppearanceSettingsChangedNotification",
               "com.apple.MediaAccessibility.captionAppearanceSettingsChanged");
        const auto use_value = [](GuestMemory& m, Address behavior) { if (behavior) m.write64(behavior, 0); };
        raw(media_accessibility, "_MACaptionAppearanceCopyForegroundColor", [this, use_value](CpuState& s, GuestMemory& m) {
            use_value(m, s.x[1]); s.x[0] = color(1, 1, 1, 1);
        });
        raw(media_accessibility, "_MACaptionAppearanceCopyBackgroundColor", [this, use_value](CpuState& s, GuestMemory& m) {
            use_value(m, s.x[1]); s.x[0] = color(0, 0, 0, 1);
        });
        raw(media_accessibility, "_MACaptionAppearanceCopyWindowColor", [this, use_value](CpuState& s, GuestMemory& m) {
            use_value(m, s.x[1]); s.x[0] = color(0, 0, 0, 1);
        });
        for (const auto& [name, value] : std::array<std::pair<const char*, double>, 5>{{
                 {"_MACaptionAppearanceGetForegroundOpacity", 1.0}, {"_MACaptionAppearanceGetBackgroundOpacity", 1.0},
                 {"_MACaptionAppearanceGetWindowOpacity", 0.0}, {"_MACaptionAppearanceGetRelativeCharacterSize", 1.0},
                 {"_MACaptionAppearanceGetWindowRoundedCornerRadius", 0.0}}}) {
            const auto result = value;
            raw(media_accessibility, name, [use_value, result](CpuState& s, GuestMemory& m) { use_value(m, s.x[1]); return_real(s, result); });
        }
        // kMACaptionAppearanceTextEdgeStyleNone
        raw(media_accessibility, "_MACaptionAppearanceGetTextEdgeStyle", [use_value](CpuState& s, GuestMemory& m) { use_value(m, s.x[1]); s.x[0] = 1; });
        // MACaptionAppearanceCopyFontDescriptorForStyle(domain, behavior*, fontStyle)
        raw(media_accessibility, "_MACaptionAppearanceCopyFontDescriptorForStyle", [this, use_value](CpuState& s, GuestMemory& m) {
            use_value(m, s.x[1]);
            static constexpr std::array<const char*, 8> families{"Helvetica", "Courier", "Times", "Menlo", "Helvetica", "Chalkboard",
                                                                 "Snell Roundhand", "Copperplate"};
            const auto style = static_cast<std::size_t>(s.x[2]);
            const auto name = cf.string(families[style < families.size() ? style : 0]);
            CallRegs registers{};
            registers.x[0] = name;
            registers.set_d(0, 0.0);
            s.x[0] = call_regs(core_text, "_CTFontDescriptorCreateWithNameAndSize", registers).x0;
            release(name);
        });
    }
    void install_image_capture() {
        for (const auto& [symbol, value] : std::array<std::pair<const char*, const char*>, 5>{{
                 {"_ICDownloadsDirectoryURL", "downloadsDirectoryURL"}, {"_ICOverwrite", "overwrite"},
                 {"_ICSaveAsFilename", "saveAsFilename"}, {"_ICSavedFilename", "savedFilename"},
                 {"_ICTransportTypeMassStorage", "ICTransportTypeMassStorage"}}})
            string(image_capture_core, symbol, value);
        static_cast<void>(cls(image_capture_core, "ICDevice", "NSObject"));
        static_cast<void>(cls(image_capture_core, "ICCameraDevice", "ICDevice"));
        static_cast<void>(cls(image_capture_core, "ICCameraItem", "NSObject"));
        static_cast<void>(cls(image_capture_core, "ICCameraFile", "ICCameraItem"));
        static_cast<void>(cls(image_capture_core, "ICDeviceBrowser", "NSObject"));
    }
};

} // namespace

void install_media_accessibility(InstallContext& context) { std::make_shared<Misc>(context)->install_media_accessibility(); }
void install_image_capture(InstallContext& context) { std::make_shared<Misc>(context)->install_image_capture(); }

} // namespace alchemy::gui::fill
