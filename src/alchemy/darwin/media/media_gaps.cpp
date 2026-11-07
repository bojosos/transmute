#include "alchemy/gui/value_provider.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <limits>
#include <map>
#include <string>

// Media and graphics gaps of Chrome's framework:
// - VideoToolbox / CoreVideo / CoreMedia / AVFoundation dictionary keys and
//   values (Apple's header values: VideoToolbox keys are the key name without
//   its prefix, as the media provider's existing keys).
// - CoreVideo: buffer attachments are real over the media provider's
//   CVBufferGetAttachment; pixel-format descriptions are real for the common
//   formats; no compressed pixel format is available.
// - CoreMedia: host-time conversion is real (the CMAttachmentBearer calls,
//   multi-block CMBlockBuffers and HEVC parameter sets are coremedia.cpp's).
// - AVFoundation: AVCapturePhotoSettings is a real value class; an
//   AVCapturePhotoOutput is never connected to a device (no capture device
//   exists), so it offers no codec or pixel format and capturing raises, as
//   documented for an output with no active video connection. (The
//   sample-buffer renderers are av_sample_buffer.cpp's.)
// - CoreAudio: aggregate devices and process taps cannot be created and
//   devices cannot be ducked (kAudioHardwareUnsupportedOperationError);
//   CATapDescription is a real value class. AudioToolbox: audio files cannot
//   be written through callbacks.
// - CoreMIDI 2.0 event lists are real; there are no MIDI endpoints, so input
//   ports bind to the provider's block-based ports (which never receive) and
//   sending fails with kMIDIObjectNotFound.
// - GameController: notification names, haptic localities, the input element
//   names (GCInputButtonA and the rest), the pad classes, GCColor (a real RGB
//   value) and the button and axis element classes (values an app can set; no
//   controller is ever connected, so the system makes none).
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
constexpr std::int32_t audio_bad_object = static_cast<std::int32_t>(fourcc("!obj"));
constexpr std::int32_t cg_invalid_connection = 1002;

struct PixelFormat {
    std::uint32_t type;
    const char* name;               // kCVPixelFormatName: the kCVPixelFormatType_ suffix
    std::uint32_t bits_per_block;   // 0 for planar formats (see planes)
    bool alpha, ycbcr, rgb, gray;
    const char* range;              // Y'CbCr only
    std::array<std::uint32_t, 3> planes;   // bits per block of each plane
    std::uint32_t chroma_h, chroma_v;      // the chroma planes' subsampling (planar)
    bool cg;                               // kCVPixelFormatCGBitmapContextCompatibility / CGImageCompatibility
};

// <CoreVideo/CVPixelBuffer.h> kCVPixelFormatType_* values: every format CVPixelBufferCreate makes
// (core_video.cpp).
constexpr std::array<PixelFormat, 34> pixel_formats{{
    {0x00000020, "32ARGB", 32, true, false, true, false, nullptr, {}, 0, 0, true},
    {fourcc("BGRA"), "32BGRA", 32, true, false, true, false, nullptr, {}, 0, 0, true},
    {fourcc("RGBA"), "32RGBA", 32, true, false, true, false, nullptr, {}, 0, 0, false},
    {fourcc("ABGR"), "32ABGR", 32, true, false, true, false, nullptr, {}, 0, 0, false},
    {0x00000018, "24RGB", 24, false, false, true, false, nullptr, {}, 0, 0, false},
    {fourcc("24BG"), "24BGR", 24, false, false, true, false, nullptr, {}, 0, 0, false},
    {0x00000010, "16BE555", 16, false, false, true, false, nullptr, {}, 0, 0, false},
    {fourcc("L555"), "16LE555", 16, false, false, true, false, nullptr, {}, 0, 0, false},
    {fourcc("L565"), "16LE565", 16, false, false, true, false, nullptr, {}, 0, 0, false},
    {fourcc("B565"), "16BE565", 16, false, false, true, false, nullptr, {}, 0, 0, false},
    {fourcc("l10r"), "ARGB2101010LEPacked", 32, true, false, true, false, nullptr, {}, 0, 0, false},
    {fourcc("RGhA"), "64RGBAHalf", 64, true, false, true, false, nullptr, {}, 0, 0, false},
    {fourcc("RGfA"), "128RGBAFloat", 128, true, false, true, false, nullptr, {}, 0, 0, false},
    {fourcc("2vuy"), "422YpCbCr8", 16, false, true, false, false, "VideoRange", {}, 0, 0, false},
    {fourcc("yuvs"), "422YpCbCr8_yuvs", 16, false, true, false, false, "VideoRange", {}, 0, 0, false},
    {fourcc("L008"), "OneComponent8", 8, false, false, false, true, nullptr, {}, 0, 0, false},
    {fourcc("L016"), "OneComponent16", 16, false, false, false, true, nullptr, {}, 0, 0, false},
    {fourcc("L00h"), "OneComponent16Half", 16, false, false, false, true, nullptr, {}, 0, 0, false},
    {fourcc("L00f"), "OneComponent32Float", 32, false, false, false, true, nullptr, {}, 0, 0, false},
    {fourcc("b16g"), "16Gray", 16, false, false, false, true, nullptr, {}, 0, 0, false},
    {fourcc("2C08"), "TwoComponent8", 16, false, false, false, false, nullptr, {}, 0, 0, false},
    {fourcc("2C16"), "TwoComponent16", 32, false, false, false, false, nullptr, {}, 0, 0, false},
    {fourcc("2C0h"), "TwoComponent16Half", 32, false, false, false, false, nullptr, {}, 0, 0, false},
    {fourcc("2C0f"), "TwoComponent32Float", 64, false, false, false, false, nullptr, {}, 0, 0, false},
    {fourcc("420v"), "420YpCbCr8BiPlanarVideoRange", 0, false, true, false, false, "VideoRange", {8, 16, 0}, 2, 2, false},
    {fourcc("420f"), "420YpCbCr8BiPlanarFullRange", 0, false, true, false, false, "FullRange", {8, 16, 0}, 2, 2, false},
    {fourcc("x420"), "420YpCbCr10BiPlanarVideoRange", 0, false, true, false, false, "VideoRange", {16, 32, 0}, 2, 2, false},
    {fourcc("xf20"), "420YpCbCr10BiPlanarFullRange", 0, false, true, false, false, "FullRange", {16, 32, 0}, 2, 2, false},
    {fourcc("y420"), "420YpCbCr8Planar", 0, false, true, false, false, "VideoRange", {8, 8, 8}, 2, 2, false},
    {fourcc("f420"), "420YpCbCr8PlanarFullRange", 0, false, true, false, false, "FullRange", {8, 8, 8}, 2, 2, false},
    {fourcc("422v"), "422YpCbCr8BiPlanarVideoRange", 0, false, true, false, false, "VideoRange", {8, 16, 0}, 2, 1, false},
    {fourcc("422f"), "422YpCbCr8BiPlanarFullRange", 0, false, true, false, false, "FullRange", {8, 16, 0}, 2, 1, false},
    {fourcc("444v"), "444YpCbCr8BiPlanarVideoRange", 0, false, true, false, false, "VideoRange", {8, 16, 0}, 1, 1, false},
    {fourcc("444f"), "444YpCbCr8BiPlanarFullRange", 0, false, true, false, false, "FullRange", {8, 16, 0}, 1, 1, false},
}};

struct Region final : ObjectPayload { std::array<double, 4> rect{}; };

// BOOL properties declared with an is- getter (`getter=isName`): -isName, -setName:.
void boolean_properties(Provider& p, Address target, std::initializer_list<std::pair<std::string_view, std::string_view>> names) {
    for (const auto& [getter, property] : names) {
        const std::string key(property);
        auto setter = "set" + key + ":";
        setter[3] = static_cast<char>(std::toupper(static_cast<unsigned char>(setter[3])));
        p.method(target, getter, [&p, key](CpuState& s, GuestMemory&) { s.x[0] = p.stored_integer(s.x[0], key) != 0; }, false, "B@:");
        p.method(target, setter, [&p, key](CpuState& s, GuestMemory&) { p.store_integer(s.x[0], key, (s.x[2] & 0xff) != 0); }, false, "v@:B");
    }
}

// CATapDescription: which processes a process tap would capture, and how. Every initializer
// gives the description a new UUID; no tap can be created from one (see
// AudioHardwareCreateProcessTap).
void install_tap_description(Provider& p) {
    const auto tap = p.cls(core_audio, "CATapDescription", "NSObject");
    p.object_properties(tap, {"name", "UUID", "processes", "deviceUID", "stream"});
    p.integer_properties(tap, {"muteBehavior"});   // CATapMuteBehavior, CATapUnmuted (0) by default
    boolean_properties(p, tap, {{"isPrivate", "privateTap"}, {"isMixdown", "mixdown"}, {"isMono", "mono"}, {"isExclusive", "exclusive"}});
    p.method(tap, "init", [&p, tap](CpuState& s, GuestMemory&) {
        const auto self = p.objc.send_super(s.x[0], tap, p.objc.selector("init")).x0;
        if (self) {
            const auto uuid = p.create(p.objc.find_class("NSUUID"));
            p.store(self, "UUID", uuid);
            p.release(uuid);
        }
        s.x[0] = self;
    });
    struct Shape { const char* selector; bool mono, mixdown, exclusive, device; };
    for (const auto& shape : std::array<Shape, 6>{{
             {"initStereoMixdownOfProcesses:", false, true, false, false},
             {"initStereoGlobalTapButExcludeProcesses:", false, true, true, false},
             {"initMonoMixdownOfProcesses:", true, true, false, false},
             {"initMonoGlobalTapButExcludeProcesses:", true, true, true, false},
             {"initWithProcesses:andDeviceUID:withStream:", false, false, false, true},
             {"initExcludingProcesses:andDeviceUID:withStream:", false, false, true, true},
         }}) {
        p.method(tap, shape.selector, [&p, shape](CpuState& s, GuestMemory&) {
            const auto self = p.objc.send(s.x[0], p.objc.selector("init")).x0;
            if (self) {
                p.store(self, "processes", s.x[2]);
                p.store_integer(self, "mono", shape.mono);
                p.store_integer(self, "mixdown", shape.mixdown);
                p.store_integer(self, "exclusive", shape.exclusive);
                if (shape.device) {
                    // (NSArray *processes, NSString *deviceUID, NSInteger stream); the property is an NSNumber.
                    p.store(self, "deviceUID", s.x[3]);
                    const auto stream = p.cf.integer(static_cast<std::int64_t>(s.x[4]), 9);
                    p.store(self, "stream", stream);
                    p.release(stream);
                }
            }
            s.x[0] = self;
        }, false, shape.device ? "@@:@@q" : "@@:@");
    }
}

// AVCapturePhotoSettings (a value class with a unique ID per instance) and AVCapturePhotoOutput,
// which is never connected to a capture device.
void install_capture_photo(Provider& p) {
    const auto settings = p.cls(av_foundation, "AVCapturePhotoSettings", "NSObject");
    p.integer_properties(settings, {"uniqueID", "flashMode", "photoQualityPrioritization", "rawPhotoPixelFormatType"});
    p.object_properties(settings, {"format", "processedFileType", "previewPhotoFormat", "embeddedThumbnailPhotoFormat"});
    boolean_properties(p, settings, {{"isHighResolutionPhotoEnabled", "highResolutionPhotoEnabled"},
                                     {"isDepthDataDeliveryEnabled", "depthDataDeliveryEnabled"}});
    constexpr std::int64_t quality_balanced = 2;   // AVCapturePhotoQualityPrioritizationBalanced
    static std::atomic<std::int64_t> next_id{1};
    p.method(settings, "init", [&p, settings](CpuState& s, GuestMemory&) {
        const auto self = p.objc.send_super(s.x[0], settings, p.objc.selector("init")).x0;
        if (self) {
            p.store_integer(self, "uniqueID", next_id++);
            p.store_integer(self, "photoQualityPrioritization", quality_balanced);
        }
        s.x[0] = self;
    });
    // +photoSettings, +photoSettingsWithFormat:(NSDictionary *), +photoSettingsFromPhotoSettings:
    // (a copy with a new unique ID).
    const auto made = [&p, settings](Address format) {
        const auto value = p.create(settings);
        if (format) p.store(value, "format", format);
        return p.bridge->autorelease(value);
    };
    p.method(settings, "photoSettings", [made](CpuState& s, GuestMemory&) { s.x[0] = made(0); }, true);
    p.method(settings, "photoSettingsWithFormat:", [made](CpuState& s, GuestMemory&) { s.x[0] = made(s.x[2]); }, true, "@@:@");
    p.method(settings, "photoSettingsFromPhotoSettings:", [&p, made](CpuState& s, GuestMemory&) {
        const auto source = s.x[2];
        const auto value = made(source ? p.stored(source, "format") : 0);
        if (source) {
            for (const auto* key : {"flashMode", "photoQualityPrioritization", "rawPhotoPixelFormatType", "highResolutionPhotoEnabled", "depthDataDeliveryEnabled"})
                p.store_integer(value, key, p.stored_integer(source, key));
            for (const auto* key : {"processedFileType", "previewPhotoFormat", "embeddedThumbnailPhotoFormat"})
                p.store(value, key, p.stored(source, key));
        }
        s.x[0] = value;
    }, true, "@@:@");

    if (!p.objc.find_class("AVCaptureOutput")) return;
    const auto output = p.cls(av_foundation, "AVCapturePhotoOutput", "AVCaptureOutput");
    // Formats are offered once an output is connected to a device's input; it never is.
    for (const auto* name : {"availablePhotoCodecTypes", "availablePhotoPixelFormatTypes", "availablePhotoFileTypes",
                             "availableRawPhotoPixelFormatTypes", "supportedFlashModes"})
        p.method(output, name, [&p](CpuState& s, GuestMemory&) { s.x[0] = p.bridge->autorelease(p.cf.array({})); });
    p.integer_properties(output, {"maxPhotoQualityPrioritization"});
    boolean_properties(p, output, {{"isHighResolutionCaptureEnabled", "highResolutionCaptureEnabled"},
                                   {"isLivePhotoCaptureSupported", "livePhotoCaptureSupported"},
                                   {"isDepthDataDeliverySupported", "depthDataDeliverySupported"}});
    p.method(output, "capturePhotoWithSettings:delegate:", [](CpuState&, GuestMemory&) {
        throw std::runtime_error("Alchemy: NSInvalidArgumentException: -[AVCapturePhotoOutput capturePhotoWithSettings:delegate:] "
                                 "needs an active and enabled video connection, and no capture device exists");
    }, false, "v@:@@");
}

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
    // The encoder's per-frame quality metrics (CalculateMeanSquaredError); the encoders here
    // (vt_compression.cpp) do not report them, so no sample carries them. (unverified: value = key
    // name without its prefix)
    string(video_toolbox, "_kVTSampleAttachmentKey_QualityMetrics", "QualityMetrics");
    for (const auto* name : {"LumaMeanSquaredError", "ChromaBlueMeanSquaredError", "ChromaRedMeanSquaredError"})
        string(video_toolbox, std::string("_kVTSampleAttachmentQualityMetricsKey_") + name, name);

    // ---------------------------------------------------------------- CoreVideo
    string(core_video, "_kCVImageBufferCleanApertureKey", "CVCleanAperture");
    string(core_video, "_kCVImageBufferCleanApertureWidthKey", "Width");
    string(core_video, "_kCVImageBufferCleanApertureHeightKey", "Height");
    string(core_video, "_kCVImageBufferCleanApertureHorizontalOffsetKey", "HorizontalOffset");
    string(core_video, "_kCVImageBufferCleanApertureVerticalOffsetKey", "VerticalOffset");
    string(core_video, "_kCVImageBufferColorPrimaries_DCI_P3", "DCI_P3");
    string(core_video, "_kCVImageBufferColorPrimaries_P3_D65", "P3_D65");
    string(core_video, "_kCVImageBufferAmbientViewingEnvironmentKey", "AmbientViewingEnvironment");
    // The remaining buffer, image buffer, pixel buffer and Metal cache keys of the CoreVideo headers. The Apple Log values
    // are documented (kVTCompressionPropertyKey_LogTransferFunction); the others follow the naming convention.
    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_kCVBufferMovieTimeKey", "MovieTime"},   // (unverified: naming convention)
             {"_kCVBufferTimeValueKey", "TimeValue"},   // (unverified: naming convention)
             {"_kCVBufferTimeScaleKey", "TimeScale"},   // (unverified: naming convention)
             {"_kCVBufferPropagatedAttachmentsKey", "PropagatedAttachments"},   // (unverified: naming convention)
             {"_kCVBufferNonPropagatedAttachmentsKey", "NonPropagatedAttachments"},   // (unverified: naming convention)
             {"_kCVImageBufferAlphaChannelIsOpaque", "AlphaChannelIsOpaque"},   // (unverified: naming convention)
             {"_kCVImageBufferAlphaChannelModeKey", "AlphaChannelMode"},   // (unverified: naming convention)
             {"_kCVImageBufferAlphaChannelMode_PremultipliedAlpha", "PremultipliedAlpha"},   // (unverified: naming convention)
             {"_kCVImageBufferAlphaChannelMode_StraightAlpha", "StraightAlpha"},   // (unverified: naming convention)
             {"_kCVImageBufferCGColorSpaceKey", "CGColorSpace"},   // (unverified: naming convention)
             {"_kCVImageBufferChromaLocationBottomFieldKey", "CVImageBufferChromaLocationBottomField"},   // (unverified: naming convention)
             {"_kCVImageBufferChromaLocation_Left", "Left"},   // (unverified: naming convention)
             {"_kCVImageBufferChromaLocation_Center", "Center"},   // (unverified: naming convention)
             {"_kCVImageBufferChromaLocation_TopLeft", "TopLeft"},   // (unverified: naming convention)
             {"_kCVImageBufferChromaLocation_Top", "Top"},   // (unverified: naming convention)
             {"_kCVImageBufferChromaLocation_BottomLeft", "BottomLeft"},   // (unverified: naming convention)
             {"_kCVImageBufferChromaLocation_Bottom", "Bottom"},   // (unverified: naming convention)
             {"_kCVImageBufferChromaLocation_DV420", "DV 4:2:0"},   // (unverified: naming convention)
             {"_kCVImageBufferChromaSubsamplingKey", "CVImageBufferChromaSubsampling"},   // (unverified: naming convention)
             {"_kCVImageBufferChromaSubsampling_420", "420"},   // (unverified: naming convention)
             {"_kCVImageBufferChromaSubsampling_422", "422"},   // (unverified: naming convention)
             {"_kCVImageBufferChromaSubsampling_411", "411"},   // (unverified: naming convention)
             {"_kCVImageBufferDisplayDimensionsKey", "CVDisplayDimensions"},   // (unverified: naming convention)
             {"_kCVImageBufferDisplayWidthKey", "Width"},   // (unverified: naming convention)
             {"_kCVImageBufferDisplayHeightKey", "Height"},   // (unverified: naming convention)
             {"_kCVImageBufferDisplayMaskRectangleKey", "DisplayMaskRectangle"},   // (unverified: naming convention)
             {"_kCVImageBufferDisplayMaskRectangleStereoLeftKey", "DisplayMaskRectangleStereoLeft"},   // (unverified: naming convention)
             {"_kCVImageBufferDisplayMaskRectangleStereoRightKey", "DisplayMaskRectangleStereoRight"},   // (unverified: naming convention)
             {"_kCVImageBufferDisplayMaskRectangle_ReferenceRasterWidthKey", "ReferenceRasterWidth"},   // (unverified: naming convention)
             {"_kCVImageBufferDisplayMaskRectangle_ReferenceRasterHeightKey", "ReferenceRasterHeight"},   // (unverified: naming convention)
             {"_kCVImageBufferDisplayMaskRectangle_RectangleLeftKey", "RectangleLeft"},   // (unverified: naming convention)
             {"_kCVImageBufferDisplayMaskRectangle_RectangleWidthKey", "RectangleWidth"},   // (unverified: naming convention)
             {"_kCVImageBufferDisplayMaskRectangle_RectangleTopKey", "RectangleTop"},   // (unverified: naming convention)
             {"_kCVImageBufferDisplayMaskRectangle_RectangleHeightKey", "RectangleHeight"},   // (unverified: naming convention)
             {"_kCVImageBufferDisplayMaskRectangle_LeftEdgePointsKey", "LeftEdgePoints"},   // (unverified: naming convention)
             {"_kCVImageBufferDisplayMaskRectangle_RightEdgePointsKey", "RightEdgePoints"},   // (unverified: naming convention)
             {"_kCVImageBufferHorizontalDisparityAdjustmentKey", "HorizontalDisparityAdjustment"},   // (unverified: naming convention)
             {"_kCVImageBufferICCProfileKey", "CVImageBufferICCProfile"},   // (unverified: naming convention)
             {"_kCVImageBufferLogTransferFunctionKey", "LogTransferFunction"},   // (unverified: naming convention)
             {"_kCVImageBufferLogTransferFunction_AppleLog", "com.apple.rec2020.apple-log"},
             {"_kCVImageBufferLogTransferFunction_AppleLog2", "com.apple.apple-wide-gamut.apple-log"},
             {"_kCVImageBufferPostDecodeProcessingSequenceMetadataKey", "PostDecodeProcessingSequenceMetadata"},   // (unverified: naming convention)
             {"_kCVImageBufferPostDecodeProcessingFrameMetadataKey", "PostDecodeProcessingFrameMetadata"},   // (unverified: naming convention)
             {"_kCVImageBufferPreferredCleanApertureKey", "CVImageBufferPreferredCleanAperture"},   // (unverified: naming convention)
             {"_kCVImageBufferRegionOfInterestKey", "RegionOfInterest"},   // (unverified: naming convention)
             {"_kCVImageBufferSceneIlluminationKey", "SceneIllumination"},   // (unverified: naming convention)
             {"_kCVMetalBufferCacheMaximumBufferAgeKey", "MaximumBufferAge"},   // (unverified: naming convention)
             {"_kCVMetalTextureCacheMaximumTextureAgeKey", "MaximumTextureAge"},   // (unverified: naming convention)
             {"_kCVMetalTextureStorageMode", "MetalTextureStorageMode"},   // (unverified: naming convention)
             {"_kCVMetalTextureUsage", "MetalTextureUsage"},   // (unverified: naming convention)
             {"_kCVPixelBufferIOSurfaceOpenGLESFBOCompatibilityKey", "IOSurfaceOpenGLESFBOCompatibility"},   // (unverified: naming convention)
             {"_kCVPixelBufferIOSurfaceOpenGLESTextureCompatibilityKey", "IOSurfaceOpenGLESTextureCompatibility"},   // (unverified: naming convention)
             {"_kCVPixelBufferIOSurfacePurgeableKey", "IOSurfacePurgeable"},   // (unverified: naming convention)
             {"_kCVPixelBufferOpenGLESCompatibilityKey", "OpenGLESCompatibility"},   // (unverified: naming convention)
             {"_kCVPixelBufferOpenGLESTextureCacheCompatibilityKey", "OpenGLESTextureCacheCompatibility"},   // (unverified: naming convention)
             {"_kCVPixelBufferPoolFreeBufferNotification", "kCVPixelBufferPoolFreeBufferNotification"},   // (unverified: naming convention)
         })
        string(core_video, symbol, value);
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
    // The kCVPixelFormat* keys (their strings are the descriptions' keys below).
    for (const auto* key : {"Name", "BitsPerComponent", "BlackBlock", "BlockHeight", "BlockHorizontalAlignment", "BlockVerticalAlignment",
                            "BlockWidth", "CGBitmapContextCompatibility", "CGBitmapInfo", "CGImageCompatibility", "CodecType",
                            "ContainsGrayscale", "ContainsSenselArray", "FillExtendedPixelsCallback", "FourCC", "HorizontalSubsampling",
                            "OpenGLCompatibility", "OpenGLESCompatibility", "OpenGLFormat", "OpenGLInternalFormat", "OpenGLType",
                            "QDCompatibility", "VerticalSubsampling"})
        string(core_video, std::string("_kCVPixelFormat") + key, key);   // each (unverified: naming convention)
    // Descriptions registered with CVPixelFormatDescriptionRegisterDescriptionWithPixelFormatType (retained).
    auto registered = std::make_shared<std::map<std::uint32_t, Address>>();
    // A +1 description of `type` (NULL when it is not a format here).
    const auto describe = [this, registered](std::uint32_t type) -> Address {
        if (const auto found = registered->find(type); found != registered->end()) return retain(found->second);
        const PixelFormat* format = nullptr;
        for (const auto& entry : pixel_formats) if (entry.type == type) format = &entry;
        if (!format) return 0;
        std::vector<std::pair<std::string, Address>> entries;
        entries.emplace_back("PixelFormat", cf.integer(type, 3));
        entries.emplace_back("Name", cf.string(format->name));
        entries.emplace_back("ContainsAlpha", cf.boolean(format->alpha));
        entries.emplace_back("ContainsYCbCr", cf.boolean(format->ycbcr));
        entries.emplace_back("ContainsRGB", cf.boolean(format->rgb));
        entries.emplace_back("ContainsGrayscale", cf.boolean(format->gray));
        if (format->range) entries.emplace_back("ComponentRange", cf.string(format->range));
        if (format->cg) {
            entries.emplace_back("CGBitmapContextCompatibility", cf.boolean(true));
            entries.emplace_back("CGImageCompatibility", cf.boolean(true));
        }
        if (format->bits_per_block) {
            entries.emplace_back("BitsPerBlock", cf.integer(format->bits_per_block, 3));
        } else {
            std::vector<Address> planes;
            for (std::size_t i = 0; i < format->planes.size(); ++i) {
                const auto bits = format->planes[i];
                if (!bits) continue;
                std::vector<std::pair<std::uint64_t, std::uint64_t>> plane{{cf.string("BitsPerBlock"), cf.integer(bits, 3)}};
                if (i > 0) {   // the chroma planes' subsampling
                    plane.emplace_back(cf.string("HorizontalSubsampling"), cf.integer(format->chroma_h, 3));
                    plane.emplace_back(cf.string("VerticalSubsampling"), cf.integer(format->chroma_v, 3));
                }
                planes.push_back(cf.dictionary(plane));
                for (const auto& [k, v] : plane) { release(k); release(v); }
            }
            entries.emplace_back("Planes", cf.array(planes));
            for (const auto plane : planes) release(plane);
        }
        std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
        for (const auto& [key, value] : entries) pairs.emplace_back(cf.string(key), value);
        const auto description = cf.dictionary(pairs);
        for (const auto& [key, value] : pairs) { release(key); release(value); }
        return description;
    };
    // CVPixelFormatDescriptionCreateWithPixelFormatType(allocator, OSType): +1.
    raw(core_video, "_CVPixelFormatDescriptionCreateWithPixelFormatType", [describe](CpuState& s, GuestMemory&) {
        s.x[0] = describe(static_cast<std::uint32_t>(s.x[1]));
    });
    // CVPixelFormatDescriptionArrayCreateWithAllPixelFormatTypes(allocator): +1, the format types as CFNumbers.
    raw(core_video, "_CVPixelFormatDescriptionArrayCreateWithAllPixelFormatTypes", [this, registered](CpuState& s, GuestMemory&) {
        std::vector<Address> numbers;
        for (const auto& entry : pixel_formats) numbers.push_back(cf.integer(entry.type, 3));
        for (const auto& [type, description] : *registered)
            if (std::none_of(pixel_formats.begin(), pixel_formats.end(), [type](const PixelFormat& f) { return f.type == type; }))
                numbers.push_back(cf.integer(type, 3));
        s.x[0] = cf.array(numbers);
        for (const auto n : numbers) release(n);
    });
    // CVPixelFormatDescriptionRegisterDescriptionWithPixelFormatType(CFDictionaryRef, OSType): kept and described
    // from then on (a buffer of it still needs a layout CVPixelBufferCreate knows).
    raw(core_video, "_CVPixelFormatDescriptionRegisterDescriptionWithPixelFormatType", [this, registered](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        auto& slot = (*registered)[static_cast<std::uint32_t>(s.x[1])];
        const auto old = std::exchange(slot, retain(s.x[0]));
        if (old) release(old);
    });
    // CVPixelFormatTypeCopyFourCharCodeString(OSType): +1, the four characters, or "0x" and eight hex digits
    // for a code that is not printable (the numeric types such as kCVPixelFormatType_32ARGB).
    raw(core_video, "_CVPixelFormatTypeCopyFourCharCodeString", [this](CpuState& s, GuestMemory&) {
        const auto type = static_cast<std::uint32_t>(s.x[0]);
        std::string text;
        for (int shift = 24; shift >= 0; shift -= 8) text.push_back(static_cast<char>((type >> shift) & 0xff));
        if (!std::all_of(text.begin(), text.end(), [](char c) { return c >= 0x20 && c < 0x7f; })) {
            char hex[16];
            std::snprintf(hex, sizeof hex, "0x%08x", type);
            text = hex;
        }
        s.x[0] = cf.string(text);
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

    // ---------------------------------------------------------------- AVFoundation
    string(av_foundation, "_AVVideoCodecKey", "AVVideoCodecKey");
    string(av_foundation, "_AVVideoCodecTypeJPEG", "jpeg");
    install_capture_photo(*this);

    // ---------------------------------------------------------------- CoreAudio, AudioToolbox
    failure(core_audio, "_AudioHardwareCreateAggregateDevice", audio_unsupported_operation);
    failure(core_audio, "_AudioHardwareDestroyAggregateDevice", audio_bad_device);
    // OSStatus AudioDeviceDuck(AudioObjectID, Float32 duckedLevel, const AudioTimeStamp *startTime, Float32 rampDuration)
    failure(core_audio, "_AudioDeviceDuck", audio_unsupported_operation);
    // OSStatus AudioHardwareCreateProcessTap(CATapDescription *, AudioObjectID *outTapID): kAudioObjectUnknown.
    raw(core_audio, "_AudioHardwareCreateProcessTap", [](CpuState& s, GuestMemory& m) {
        if (s.x[1]) m.write32(s.x[1], 0);
        s.x[0] = status(audio_unsupported_operation);
    });
    failure(core_audio, "_AudioHardwareDestroyProcessTap", audio_bad_object);   // no tap exists
    install_tap_description(*this);

    // ---------------------------------------------------------------- GameController
    string(game_controller, "_GCControllerDidConnectNotification", "GCControllerDidConnectNotification");
    string(game_controller, "_GCControllerDidDisconnectNotification", "GCControllerDidDisconnectNotification");
    for (const auto* name : {"Default", "All", "Handles", "LeftHandle", "RightHandle", "Triggers", "LeftTrigger", "RightTrigger"})
        string(game_controller, std::string("_GCHapticsLocality") + name, name);   // (unverified: value = locality name)
    float_constant(game_controller, "_GCHapticDurationInfinite", std::numeric_limits<float>::infinity());   // (unverified)
    // The physical input element names (GCInputButtonName, GCInputDirectionPadName): the documentation gives
    // their names, not their values.
    for (const auto* name : {"ButtonA", "ButtonB", "ButtonX", "ButtonY", "ButtonHome", "ButtonMenu", "ButtonOptions", "ButtonShare",
                             "DirectionPad", "LeftThumbstick", "RightThumbstick", "LeftThumbstickButton", "RightThumbstickButton",
                             "LeftShoulder", "RightShoulder", "LeftTrigger", "RightTrigger", "LeftBumper", "RightBumper", "LeftPaddle",
                             "RightPaddle", "LeftSideButton", "RightSideButton", "XboxPaddleOne", "XboxPaddleTwo", "XboxPaddleThree",
                             "XboxPaddleFour", "DualShockTouchpadOne", "DualShockTouchpadTwo", "DualShockTouchpadButton", "Thumbstick",
                             "ThumbstickButton", "Trigger", "GripButton", "StylusTip", "StylusPrimaryButton", "StylusSecondaryButton",
                             "DirectionalDpad", "DirectionalCardinalDpad", "DirectionalCenterButton", "DirectionalTouchSurfaceButton",
                             "MicroGamepadDpad", "MicroGamepadButtonA", "MicroGamepadButtonX", "MicroGamepadButtonMenu"})
        string(game_controller, std::string("_GCInput") + name, name);   // (unverified: value = the name after "GCInput")
    // GCColor: an RGB value of float components (a DualShock or DualSense light).
    const auto color = cls(game_controller, "GCColor", "NSObject");
    real_properties(color, {"red", "green", "blue"}, true);
    method(color, "initWithRed:green:blue:", [this](CpuState& s, GuestMemory&) {
        const auto state = properties(s.x[0]);
        std::lock_guard lock(state->mutex);
        state->reals["red"] = fill::real32(s, 0);
        state->reals["green"] = fill::real32(s, 1);
        state->reals["blue"] = fill::real32(s, 2);
    }, false, "@@:fff");
    method(color, "copyWithZone:", [this](CpuState& s, GuestMemory&) { s.x[0] = objc.send(s.x[0], "retain").x0; }, false, "@@:^{_NSZone=}");   // immutable
    // The elements of a controller's input profile. None is connected, so the system makes none; one an app
    // creates keeps the value it is given (pressed above 0) and its handlers.
    if (!objc.find_class("GCControllerElement")) static_cast<void>(cls(game_controller, "GCControllerElement", "NSObject"));
    const auto element = objc.find_class("GCControllerElement");
    integer_properties(element, {"isAnalog", "isBoundToSystemGesture", "preferredSystemGestureState"});
    object_properties(element, {"localizedName", "unmappedLocalizedName", "sfSymbolsName", "unmappedSfSymbolsName"});
    method(element, "collection", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    method(element, "aliases", [this](CpuState& s, GuestMemory&) {
        const auto set = objc.find_class("NSSet");
        s.x[0] = set ? objc.send(set, "set").x0 : 0;
    });
    const auto button = cls(game_controller, "GCControllerButtonInput", "GCControllerElement");
    real_properties(button, {"value"}, true);
    integer_properties(button, {"isTouched"});
    object_properties(button, {"valueChangedHandler", "pressedChangedHandler", "touchedChangedHandler"});
    method(button, "isPressed", [this](CpuState& s, GuestMemory&) {
        const auto state = properties(s.x[0]);
        std::lock_guard lock(state->mutex);
        const auto found = state->reals.find("value");
        s.x[0] = found != state->reals.end() && found->second > 0 ? 1 : 0;
    }, false, "B@:");
    const auto axis = cls(game_controller, "GCControllerAxisInput", "GCControllerElement");
    real_properties(axis, {"value"}, true);
    object_properties(axis, {"valueChangedHandler"});
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
