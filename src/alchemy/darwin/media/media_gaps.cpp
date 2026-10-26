#include "alchemy/gui/value_provider.hpp"

#include <array>
#include <limits>

// Media and graphics gaps of Chrome's framework:
// - VideoToolbox / CoreVideo / CoreMedia / AVFoundation dictionary keys and
//   values (Apple's header values: VideoToolbox keys are the key name without
//   its prefix, as the media provider's existing keys).
// - CoreVideo: buffer attachments are real over the media provider's
//   CVBufferGetAttachment; pixel-format descriptions are real for the common
//   formats; no compressed pixel format is available.
// - CoreMedia: host-time conversion is real; CMSetAttachment is real for
//   CVBuffers (multi-block CMBlockBuffers and HEVC parameter sets are
//   coremedia.cpp's).
// - AVFoundation: the capture-photo and sample-buffer renderer classes exist
//   as plain classes (no device or renderer can be created).
// - CoreAudio: aggregate devices cannot be created
//   (kAudioHardwareUnsupportedOperationError); CATapDescription is a plain
//   class. AudioToolbox: audio files cannot be written through callbacks.
// - CoreMIDI 2.0 event lists are real; there are no MIDI endpoints, so input
//   ports bind to the provider's block-based ports (which never receive) and
//   sending fails with kMIDIObjectNotFound.
// - GameController: notification names, haptic localities and the pad
//   classes (no controller is ever connected).
// - CoreGraphics: no display is mirrored; CGRegion is a real rectangle value;
//   windows cannot be excluded from capture (kCGErrorInvalidConnection).
// - CoreText: CTFontCreateForStringWithLanguage is CTFontCreateForString (the
//   language is not used to pick a fallback); family matching over a
//   collection is real; fonts have no variation axes (NULL).
// - (CAMetalDisplayLink, the CADynamicRange keys and IOSurfaceSetPurgeable
//   are darwin/quartzcore's.)
namespace alchemy::gui::values {
namespace {

using fill::av_foundation;
using fill::audio_toolbox;
using fill::core_audio;
using fill::core_foundation;
using fill::core_graphics;
using fill::core_media;
using fill::core_text;
using fill::core_video;
using fill::fourcc;
using fill::game_controller;
using fill::iosurface;
using fill::quartz_core;
using fill::status;
using fill::video_toolbox;

constexpr std::int32_t fd_invalid_parameter = -12710;
constexpr std::int32_t audio_unsupported_operation = static_cast<std::int32_t>(fourcc("unop"));
constexpr std::int32_t audio_bad_device = static_cast<std::int32_t>(fourcc("!dev"));
constexpr std::int32_t cg_invalid_connection = 1002;

struct PixelFormat {
    std::uint32_t type;
    std::uint32_t bits_per_block;   // 0 for planar formats (see planes)
    bool alpha, ycbcr, rgb;
    const char* range;              // nullptr for RGB formats
    std::array<std::uint32_t, 3> planes;   // bits per block of each plane
};

// <CoreVideo/CVPixelBuffer.h> kCVPixelFormatType_* values.
constexpr std::array<PixelFormat, 11> pixel_formats{{
    {0x00000020, 32, true, false, true, nullptr, {}},                  // 32ARGB
    {fourcc("BGRA"), 32, true, false, true, nullptr, {}},              // 32BGRA
    {fourcc("RGBA"), 32, true, false, true, nullptr, {}},              // 32RGBA
    {fourcc("l10r"), 32, true, false, true, nullptr, {}},              // ARGB2101010LEPacked
    {fourcc("2vuy"), 16, false, true, false, "VideoRange", {}},        // 422YpCbCr8
    {fourcc("420v"), 0, false, true, false, "VideoRange", {8, 16, 0}},  // 420YpCbCr8BiPlanarVideoRange
    {fourcc("420f"), 0, false, true, false, "FullRange", {8, 16, 0}},   // 420YpCbCr8BiPlanarFullRange
    {fourcc("x420"), 0, false, true, false, "VideoRange", {16, 32, 0}}, // 420YpCbCr10BiPlanarVideoRange
    {fourcc("xf20"), 0, false, true, false, "FullRange", {16, 32, 0}},  // 420YpCbCr10BiPlanarFullRange
    {fourcc("y420"), 0, false, true, false, "VideoRange", {8, 8, 8}},   // 420YpCbCr8Planar
    {fourcc("f420"), 0, false, true, false, "FullRange", {8, 8, 8}},    // 420YpCbCr8PlanarFullRange
}};

struct Region final : ObjectPayload { std::array<double, 4> rect{}; };

} // namespace

void Provider::install_media() {
    // ---------------------------------------------------------------- VideoToolbox
    for (const auto* name : {"BaseLayerFrameRateFraction", "ColorPrimaries", "ContentLightLevelInfo", "EncoderID",
                             "HDRMetadataInsertionMode", "MasteringDisplayColorVolume", "OutputBitDepth",
                             "PixelTransferProperties", "ReferenceBufferCount", "TransferFunction",
                             "UsingHardwareAcceleratedVideoEncoder", "YCbCrMatrix", "CalculateMeanSquaredError"})
        string(video_toolbox, std::string("_kVTCompressionPropertyKey_") + name, name);
    string(video_toolbox, "_kVTEncodeFrameOptionKey_BaseFrameQP", "BaseFrameQP");
    string(video_toolbox, "_kVTHDRMetadataInsertionMode_Auto", "Auto");
    string(video_toolbox, "_kVTHDRMetadataInsertionMode_None", "None");
    string(video_toolbox, "_kVTPixelTransferPropertyKey_ScalingMode", "ScalingMode");
    string(video_toolbox, "_kVTScalingMode_CropSourceToCleanAperture", "CropSourceToCleanAperture");
    string(video_toolbox, "_kVTScalingMode_Normal", "Normal");
    for (const auto* name : {"HEVC_Main_AutoLevel", "HEVC_Main10_AutoLevel", "HEVC_Main42210_AutoLevel"})
        string(video_toolbox, std::string("_kVTProfileLevel_") + name, name);
    string(video_toolbox, "_kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder", "EnableHardwareAcceleratedVideoEncoder");
    string(video_toolbox, "_kVTVideoEncoderSpecification_EnableLowLatencyRateControl", "EnableLowLatencyRateControl");

    // ---------------------------------------------------------------- CoreVideo
    string(core_video, "_kCVImageBufferCleanApertureKey", "CVCleanAperture");
    string(core_video, "_kCVImageBufferCleanApertureWidthKey", "Width");
    string(core_video, "_kCVImageBufferCleanApertureHeightKey", "Height");
    string(core_video, "_kCVImageBufferCleanApertureHorizontalOffsetKey", "HorizontalOffset");
    string(core_video, "_kCVImageBufferCleanApertureVerticalOffsetKey", "VerticalOffset");
    string(core_video, "_kCVImageBufferColorPrimaries_DCI_P3", "DCI_P3");
    string(core_video, "_kCVImageBufferColorPrimaries_P3_D65", "P3_D65");
    string(core_video, "_kCVImageBufferAmbientViewingEnvironmentKey", "AmbientViewingEnvironment");
    string(core_video, "_kCVPixelFormatConstant", "PixelFormat");
    string(core_video, "_kCVPixelFormatBitsPerBlock", "BitsPerBlock");
    string(core_video, "_kCVPixelFormatContainsAlpha", "ContainsAlpha");
    string(core_video, "_kCVPixelFormatContainsYCbCr", "ContainsYCbCr");
    string(core_video, "_kCVPixelFormatContainsRGB", "ContainsRGB");
    string(core_video, "_kCVPixelFormatPlanes", "Planes");
    string(core_video, "_kCVPixelFormatComponentRange", "ComponentRange");
    string(core_video, "_kCVPixelFormatComponentRange_VideoRange", "VideoRange");
    string(core_video, "_kCVPixelFormatComponentRange_FullRange", "FullRange");
    string(core_video, "_kCVPixelFormatComponentRange_WideRange", "WideRange");
    // CFTypeRef CVBufferCopyAttachment(buffer, key, CVAttachmentMode *mode): +1.
    raw(core_video, "_CVBufferCopyAttachment", [this](CpuState& s, GuestMemory&) {
        s.x[0] = retain(call(core_video, "_CVBufferGetAttachment", {s.x[0], s.x[1], s.x[2]}));
    });
    raw(core_video, "_CVBufferHasAttachment", [this](CpuState& s, GuestMemory&) {
        s.x[0] = call(core_video, "_CVBufferGetAttachment", {s.x[0], s.x[1], 0}) != 0;
    });
    constant(core_video, "_CVIsCompressedPixelFormatAvailable", 0);   // no compressed pixel formats
    raw(core_video, "_CVPixelFormatDescriptionCreateWithPixelFormatType", [this](CpuState& s, GuestMemory&) {
        const auto type = static_cast<std::uint32_t>(s.x[1]);
        const PixelFormat* format = nullptr;
        for (const auto& entry : pixel_formats) if (entry.type == type) format = &entry;
        if (!format) { s.x[0] = 0; return; }
        std::vector<std::pair<std::string, Address>> entries;
        entries.emplace_back("PixelFormat", cf.integer(type, 3));
        entries.emplace_back("ContainsAlpha", cf.boolean(format->alpha));
        entries.emplace_back("ContainsYCbCr", cf.boolean(format->ycbcr));
        entries.emplace_back("ContainsRGB", cf.boolean(format->rgb));
        if (format->range) entries.emplace_back("ComponentRange", cf.string(format->range));
        if (format->bits_per_block) {
            entries.emplace_back("BitsPerBlock", cf.integer(format->bits_per_block, 3));
        } else {
            std::vector<Address> planes;
            for (const auto bits : format->planes) {
                if (!bits) continue;
                const auto key = cf.string("BitsPerBlock");
                const auto number = cf.integer(bits, 3);
                const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> plane{{{key, number}}};
                planes.push_back(cf.dictionary(plane));
                release(key);
                release(number);
            }
            entries.emplace_back("Planes", cf.array(planes));
            for (const auto plane : planes) release(plane);
        }
        std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
        for (const auto& [key, value] : entries) pairs.emplace_back(cf.string(key), value);
        s.x[0] = cf.dictionary(pairs);
        for (const auto& [key, value] : pairs) { release(key); release(value); }
    });

    // ---------------------------------------------------------------- CoreMedia
    string(core_media, "_kCMSampleBufferAttachmentKey_ResetDecoderBeforeDecoding", "ResetDecoderBeforeDecoding");
    // UInt64 CMClockConvertHostTimeToSystemUnits(CMTime hostTime): the 24-byte
    // CMTime {value, timescale, flags, epoch} arrives by reference.
    raw(core_media, "_CMClockConvertHostTimeToSystemUnits", [this](CpuState& s, GuestMemory& m) {
        const auto value = static_cast<std::int64_t>(m.read64(s.x[0]));
        const auto timescale = static_cast<std::int32_t>(m.read32(s.x[0] + 8));
        const auto flags = m.read32(s.x[0] + 12);
        if (!(flags & 1) || timescale <= 0) { s.x[0] = 0; return; }   // not kCMTimeFlags_Valid
        const auto info = process.allocate_guest(8);
        static_cast<void>(call(fill::libsystem, "_mach_timebase_info", {info}));
        const auto numer = m.read32(info), denom = m.read32(info + 4);
        process.release_guest(info);
        const long double nanoseconds = static_cast<long double>(value) * 1e9L / timescale;
        s.x[0] = static_cast<std::uint64_t>(nanoseconds * (denom ? denom : 1) / (numer ? numer : 1));
    });
    // void CMSetAttachment(CMAttachmentBearerRef target, key, value, mode)
    raw(core_media, "_CMSetAttachment", [this](CpuState& s, GuestMemory&) {
        const auto pixel_buffer_type = call(core_video, "_CVPixelBufferGetTypeID", {});
        if (s.x[0] && call(core_foundation, "_CFGetTypeID", {s.x[0]}) == pixel_buffer_type) {
            static_cast<void>(call(core_video, "_CVBufferSetAttachment", {s.x[0], s.x[1], s.x[2], s.x[3]}));
            return;
        }
        throw std::runtime_error("Alchemy: CMSetAttachment is unsupported for this attachment bearer (only CVBuffers)");
    });

    // ---------------------------------------------------------------- AVFoundation
    string(av_foundation, "_AVVideoCodecKey", "AVVideoCodecKey");
    string(av_foundation, "_AVVideoCodecTypeJPEG", "jpeg");
    if (objc.find_class("AVCaptureOutput")) static_cast<void>(cls(av_foundation, "AVCapturePhotoOutput", "AVCaptureOutput"));
    static_cast<void>(cls(av_foundation, "AVCapturePhotoSettings", "NSObject"));   // the sample-buffer renderers are av_sample_buffer.cpp's

    // ---------------------------------------------------------------- CoreAudio, AudioToolbox
    failure(core_audio, "_AudioHardwareCreateAggregateDevice", audio_unsupported_operation);
    failure(core_audio, "_AudioHardwareDestroyAggregateDevice", audio_bad_device);
    static_cast<void>(cls(core_audio, "CATapDescription", "NSObject"));

    // ---------------------------------------------------------------- GameController
    string(game_controller, "_GCControllerDidConnectNotification", "GCControllerDidConnectNotification");
    string(game_controller, "_GCControllerDidDisconnectNotification", "GCControllerDidDisconnectNotification");
    for (const auto* name : {"Default", "All", "Handles", "LeftHandle", "RightHandle", "Triggers", "LeftTrigger", "RightTrigger"})
        string(game_controller, std::string("_GCHapticsLocality") + name, name);   // (unverified: value = locality name)
    float_constant(game_controller, "_GCHapticDurationInfinite", std::numeric_limits<float>::infinity());   // (unverified)
    if (!objc.find_class("GCPhysicalInputProfile")) static_cast<void>(cls(game_controller, "GCPhysicalInputProfile", "NSObject"));
    if (!objc.find_class("GCExtendedGamepad")) static_cast<void>(cls(game_controller, "GCExtendedGamepad", "GCPhysicalInputProfile"));
    for (const auto* name : {"GCDualSenseGamepad", "GCDualShockGamepad", "GCXboxGamepad"})
        static_cast<void>(cls(game_controller, name, "GCExtendedGamepad"));

    // ---------------------------------------------------------------- CoreGraphics
    string(core_graphics, "_kCGColorSpaceExtendedLinearITUR_2020", "kCGColorSpaceExtendedLinearITUR_2020");
    constant(core_graphics, "_CGDisplayIsInMirrorSet", 0);   // no display is mirrored
    const auto region_class = cls(core_graphics, "__CGRegion", "NSObject");
    // CGRegionRef CGRegionCreateWithRect(CGRect) (rect in d0-d3).
    raw(core_graphics, "_CGRegionCreateWithRect", [this, region_class](CpuState& s, GuestMemory&) {
        const auto object = objc.allocate(region_class);
        objc.host_state<Region>(object)->rect = {fill::real(s, 0), fill::real(s, 1), fill::real(s, 2), fill::real(s, 3)};
        s.x[0] = object;
    });
    failure(core_graphics, "_CGSSetWindowCaptureExcludeShape", cg_invalid_connection);

    // ---------------------------------------------------------------- CoreText
    raw(core_text, "_CTFontCreateForStringWithLanguage", [this](CpuState& s, GuestMemory&) {
        s.x[0] = call(core_text, "_CTFontCreateForString", {s.x[0], s.x[1], s.x[2], s.x[3]});
    });
    // CFArrayRef CTFontCollectionCreateMatchingFontDescriptorsForFamily(collection, familyName, options):
    // NULL when no descriptor matches.
    raw(core_text, "_CTFontCollectionCreateMatchingFontDescriptorsForFamily", [this](CpuState& s, GuestMemory&) {
        if (!s.x[0] || !s.x[1]) { s.x[0] = 0; return; }
        const auto all = call(core_text, "_CTFontCollectionCreateMatchingFontDescriptors", {s.x[0]});
        if (!all) { s.x[0] = 0; return; }
        const auto family = cf.utf8(s.x[1]);
        const auto key = bridge->constant(core_text, "_kCTFontFamilyNameAttribute");
        std::vector<Address> matching;
        for (const auto descriptor : cf.array_values(all)) {
            const auto name = call(core_text, "_CTFontDescriptorCopyAttribute", {descriptor, key});
            if (name && bridge->is_string(name) && cf.utf8(name) == family) matching.push_back(descriptor);
            release(name);
        }
        s.x[0] = matching.empty() ? 0 : cf.array(matching);
        release(all);
    });
    raw(core_text, "_CTFontCopyVariationAxesInternal", [this](CpuState& s, GuestMemory&) {
        s.x[0] = call(core_text, "_CTFontCopyVariationAxes", {s.x[0]});
    });
    string(core_text, "_kCTFontVariationAxesAttribute", "NSCTFontVariationAxesAttribute");   // (unverified)
}

} // namespace alchemy::gui::values
