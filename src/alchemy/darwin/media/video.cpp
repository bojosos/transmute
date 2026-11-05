#include "alchemy/darwin/media/media.hpp"

#include <algorithm>
#include <array>

// VideoToolbox, AVFoundation and the CoreVideo pixel-buffer gaps.
//
// - VideoToolbox: Alchemy has no video encoder. Compression session creation
//   fails with kVTCouldNotFindVideoEncoderErr; since no such session can exist,
//   its calls answer kVTInvalidSessionErr. Decompression sessions (AV1, VP9,
//   VP8) are vt_decompression.cpp's and pixel transfer sessions
//   pixel_transfer.cpp's. Hardware decoding is reported unsupported; no
//   supplemental decoder is available.
// - AVFoundation: the capture string constants (device types, media types,
//   notification names). The capture classes are av_capture.cpp's (cameras
//   over host::camera, microphones over the CoreAudio HAL);
//   AVSampleBufferDisplayLayer is av_sample_buffer.cpp's.
// - CoreVideo: the dictionary keys, host time (mach_absolute_time's), the ITU-T
//   H.273 code points of the colour attachment values, whether a pixel buffer
//   matches attributes, and the Metal texture caches, real objects holding their
//   device. Pixel buffers and pools are core_video.cpp's.
namespace alchemy::gui::fill {
namespace {

constexpr std::int32_t vt_parameter = -12902, vt_invalid_session = -12903, vt_pixel_transfer_not_supported = -12905,
    vt_could_not_find_encoder = -12908;
constexpr std::int32_t cv_invalid_argument = -6661, cv_unsupported = -6663;

void impossible(std::uint64_t object, std::string_view what) {
    if (object) throw std::runtime_error("Alchemy: " + std::string(what) + " received an object that cannot exist under Alchemy");
}

class Video final : public Provider {
public:
    using Provider::Provider;
    void install_video_toolbox();
    void install_av_foundation();
    void install_core_video();
    void install_code_points();
    void install_metal_texture_cache();
};

struct TextureCache final : ObjectPayload { Address device{}; };
struct MetalTexture final : ObjectPayload { Address texture{}, image{}; };

// CVMetalTextureCache: a cache object that holds its Metal device. A texture
// from an image buffer (a CVPixelBuffer) is an MTLTexture over the buffer's
// IOSurface plane (-[MTLDevice newTextureWithDescriptor:iosurface:plane:]),
// so it sees the buffer's pixels; a buffer without an IOSurface is copied into
// a new texture when the texture is made. CVMetalTexture objects hold their
// texture and image; nothing is cached between calls, so flushing has nothing
// to drop.
void Video::install_metal_texture_cache() {
    const auto native = [this](const char* name) {
        const auto found = objc.find_class(name);
        return found ? found : objc.register_native_class(name, objc.find_class("NSObject"), 8);
    };
    const auto cache_class = native("__AlchemyCVMetalTextureCache");
    const auto texture_class = native("__AlchemyCVMetalTexture");
    if (!has(core_video, "_CVMetalTextureCacheCreate")) {
        static_cast<void>(objc.add_native_method(cache_class, "dealloc", [self = shared_from_this(), this, cache_class](CpuState& s, GuestMemory&) {
            if (const auto value = objc.find_host_state<TextureCache>(s.x[0])) release(std::exchange(value->device, 0));
            static_cast<void>(objc.send_super(s.x[0], cache_class, objc.selector("dealloc")));
        }, false, "v@:"));
        static_cast<void>(objc.add_native_method(texture_class, "dealloc", [self = shared_from_this(), this, texture_class](CpuState& s, GuestMemory&) {
            if (const auto value = objc.find_host_state<MetalTexture>(s.x[0])) {
                release(std::exchange(value->texture, 0));
                release(std::exchange(value->image, 0));
            }
            static_cast<void>(objc.send_super(s.x[0], texture_class, objc.selector("dealloc")));
        }, false, "v@:"));
    }
    // CVMetalTextureCacheCreate(allocator, cacheAttributes, id<MTLDevice>, textureAttributes, CVMetalTextureCacheRef *out)
    raw(core_video, "_CVMetalTextureCacheCreate", [this, cache_class](CpuState& s, GuestMemory& m) {
        if (!s.x[4]) { s.x[0] = status(cv_invalid_argument); return; }
        m.write64(s.x[4], 0);
        if (!s.x[2]) { s.x[0] = status(cv_invalid_argument); return; }
        const auto cache = objc.allocate(cache_class);
        objc.host_state<TextureCache>(cache)->device = retain(s.x[2]);
        m.write64(s.x[4], cache);
        s.x[0] = 0;
    });
    raw(core_video, "_CVMetalTextureCacheFlush", [this](CpuState& s, GuestMemory&) {
        if (s.x[0] && !objc.find_host_state<TextureCache>(s.x[0])) throw std::runtime_error("Alchemy: CVMetalTextureCacheFlush on an object that is not a texture cache");
    });
    // CVMetalTextureCacheCreateTextureFromImage(allocator, cache, CVImageBufferRef image, attributes, MTLPixelFormat,
    //     width, height, planeIndex, [stack] CVMetalTextureRef *out)
    raw(core_video, "_CVMetalTextureCacheCreateTextureFromImage", [this, texture_class](CpuState& s, GuestMemory& m) {
        const auto out = m.read64(s.sp);
        if (out) m.write64(out, 0);
        const auto cache = s.x[1] ? objc.find_host_state<TextureCache>(s.x[1]) : nullptr;
        const auto image = s.x[2];
        if (!cache || !image || !out) { s.x[0] = status(cv_invalid_argument); return; }
        if (call(core_foundation, "_CFGetTypeID", {image}) != call(core_video, "_CVPixelBufferGetTypeID", {})) {
            s.x[0] = status(cv_invalid_argument);
            return;
        }
        const auto format = s.x[4], width = s.x[5], height = s.x[6], plane = s.x[7];
        const auto descriptor_class = objc.find_class("MTLTextureDescriptor");
        if (!descriptor_class || !width || !height) { s.x[0] = status(cv_invalid_argument); return; }
        CallRegs make{};
        make.x[2] = format;
        make.x[3] = width;
        make.x[4] = height;
        make.x[5] = 0;
        const auto descriptor = objc.send(descriptor_class, "texture2DDescriptorWithPixelFormat:width:height:mipmapped:", make).x0;
        if (!descriptor) { s.x[0] = status(cv_invalid_argument); return; }
        CallRegs usage{};
        usage.x[2] = 1 | 2 | 4;   // MTLTextureUsageShaderRead | ShaderWrite | RenderTarget
        static_cast<void>(objc.send(descriptor, "setUsage:", usage));
        Address texture{};
        if (const auto surface = call(core_video, "_CVPixelBufferGetIOSurface", {image})) {
            CallRegs r{};
            r.x[2] = descriptor;
            r.x[3] = surface;
            r.x[4] = plane;
            texture = objc.send(cache->device, "newTextureWithDescriptor:iosurface:plane:", r).x0;
        } else {
            // No IOSurface: a texture with a copy of the pixels.
            CallRegs r{};
            r.x[2] = descriptor;
            texture = objc.send(cache->device, "newTextureWithDescriptor:", r).x0;
            const bool planar = call(core_video, "_CVPixelBufferIsPlanar", {image}) != 0;
            const auto base = planar ? call(core_video, "_CVPixelBufferGetBaseAddressOfPlane", {image, plane}) : call(core_video, "_CVPixelBufferGetBaseAddress", {image});
            const auto row = planar ? call(core_video, "_CVPixelBufferGetBytesPerRowOfPlane", {image, plane}) : call(core_video, "_CVPixelBufferGetBytesPerRow", {image});
            if (texture && base) {
                // -replaceRegion:mipmapLevel:withBytes:bytesPerRow: (MTLRegion {origin xyz, size whd} by value: 48 bytes, by reference)
                const auto region = process.allocate_guest(48, 8);
                const std::array<std::uint64_t, 6> values{0, 0, 0, width, height, 1};
                for (std::size_t i = 0; i < values.size(); ++i) m.write64(region + 8 * i, values[i]);
                CallRegs replace{};
                replace.x[2] = region;
                replace.x[3] = 0;
                replace.x[4] = base;
                replace.x[5] = row;
                static_cast<void>(objc.send(texture, "replaceRegion:mipmapLevel:withBytes:bytesPerRow:", replace));
                process.release_guest(region);
            }
        }
        if (!texture) { s.x[0] = status(cv_invalid_argument); return; }
        const auto made = objc.allocate(texture_class);
        const auto state = objc.host_state<MetalTexture>(made);
        state->texture = texture;   // newTexture... is +1
        state->image = retain(image);
        m.write64(out, made);
        s.x[0] = 0;
    });
    raw(core_video, "_CVMetalTextureGetTexture", [this](CpuState& s, GuestMemory&) {
        const auto state = s.x[0] ? objc.find_host_state<MetalTexture>(s.x[0]) : nullptr;
        s.x[0] = state ? state->texture : 0;
    });
    constant(core_video, "_CVMetalTextureIsFlipped", 0);
    // CVMetalTextureGetCleanTexCoords(texture, lowerLeft[2], lowerRight[2], upperRight[2], upperLeft[2]): the whole texture.
    raw(core_video, "_CVMetalTextureGetCleanTexCoords", [](CpuState& s, GuestMemory& m) {
        const std::array<std::array<float, 2>, 4> corners{{{0, 1}, {1, 1}, {1, 0}, {0, 0}}};
        for (unsigned i = 0; i < 4; ++i)
            if (s.x[1 + i]) {
                m.write32(s.x[1 + i], std::bit_cast<std::uint32_t>(corners[i][0]));
                m.write32(s.x[1 + i] + 4, std::bit_cast<std::uint32_t>(corners[i][1]));
            }
    });
}

// CV{ColorPrimaries,TransferFunction,YCbCrMatrix}GetIntegerCodePointForString and
// ...GetStringForIntegerCodePoint: the ITU-T H.273 numbers of the attachment values. An unknown string
// is 2 (unspecified); an unknown number has no string (NULL). The strings are the exported constants.
void Video::install_code_points() {
    struct Point { int code; const char* symbol; const char* value; };
    using Table = std::vector<Point>;
    static const Table primaries{{1, "_kCVImageBufferColorPrimaries_ITU_R_709_2", "ITU_R_709_2"},
                                 {5, "_kCVImageBufferColorPrimaries_EBU_3213", "EBU_3213"},
                                 {6, "_kCVImageBufferColorPrimaries_SMPTE_C", "SMPTE_C"},
                                 {9, "_kCVImageBufferColorPrimaries_ITU_R_2020", "ITU_R_2020"},
                                 {11, "_kCVImageBufferColorPrimaries_DCI_P3", "DCI_P3"},
                                 {12, "_kCVImageBufferColorPrimaries_P3_D65", "P3_D65"},
                                 {22, "_kCVImageBufferColorPrimaries_P22", "P22"}};
    static const Table transfer{{1, "_kCVImageBufferTransferFunction_ITU_R_709_2", "ITU_R_709_2"},
                                {7, "_kCVImageBufferTransferFunction_SMPTE_240M_1995", "SMPTE_240M_1995"},
                                {8, "_kCVImageBufferTransferFunction_Linear", "Linear"},
                                {13, "_kCVImageBufferTransferFunction_sRGB", "IEC_sRGB"},
                                {14, "_kCVImageBufferTransferFunction_ITU_R_2020", "ITU_R_2020"},
                                {16, "_kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ", "SMPTE_ST_2084_PQ"},
                                {17, "_kCVImageBufferTransferFunction_SMPTE_ST_428_1", "SMPTE_ST_428_1"},
                                {18, "_kCVImageBufferTransferFunction_ITU_R_2100_HLG", "ITU_R_2100_HLG"}};
    static const Table matrix{{1, "_kCVImageBufferYCbCrMatrix_ITU_R_709_2", "ITU_R_709_2"},
                              {6, "_kCVImageBufferYCbCrMatrix_ITU_R_601_4", "ITU_R_601_4"},
                              {7, "_kCVImageBufferYCbCrMatrix_SMPTE_240M_1995", "SMPTE_240M_1995"},
                              {9, "_kCVImageBufferYCbCrMatrix_ITU_R_2020", "ITU_R_2020"}};
    for (const auto& entry : std::array<std::pair<const char*, const Table*>, 3>{{
             {"ColorPrimaries", &primaries}, {"TransferFunction", &transfer}, {"YCbCrMatrix", &matrix}}}) {
        const std::string kind = entry.first;
        const Table* table = entry.second;
        raw(core_video, "_CV" + kind + "GetIntegerCodePointForString", [this, table](CpuState& s, GuestMemory&) {
            const auto text = s.x[0] ? cf.utf8(s.x[0]) : std::string();
            const auto found = std::find_if(table->begin(), table->end(), [&](const Point& p) { return text == p.value; });
            s.x[0] = static_cast<std::uint64_t>(found != table->end() ? found->code : 2);
        });
        raw(core_video, "_CV" + kind + "GetStringForIntegerCodePoint", [this, table](CpuState& s, GuestMemory& m) {
            const auto code = static_cast<int>(static_cast<std::int32_t>(s.x[0]));
            const auto found = std::find_if(table->begin(), table->end(), [&](const Point& p) { return code == p.code; });
            s.x[0] = found != table->end() ? m.read64(symbol(core_video, found->symbol)) : 0;
        });
    }
}

void Video::install_video_toolbox() {
    // VTCompressionSessionCreate(allocator, width, height, codec, encoderSpecification, sourceAttributes,
    //     compressedDataAllocator, outputCallback, [stack] refcon, compressionSessionOut)
    failure(video_toolbox, "_VTCompressionSessionCreate", vt_could_not_find_encoder, {9});
    // Pixel transfer sessions and the VTSession property calls are pixel_transfer.cpp's, decompression
    // sessions vt_decompression.cpp's.
    for (const auto* name : {"_VTCompressionSessionCompleteFrames", "_VTCompressionSessionEncodeFrame"})
        raw(video_toolbox, name, [](CpuState& s, GuestMemory&) { s.x[0] = status(s.x[0] ? vt_invalid_session : vt_parameter); });
    raw(video_toolbox, "_VTCompressionSessionInvalidate", [](CpuState& s, GuestMemory&) { impossible(s.x[0], "VTCompressionSessionInvalidate"); });
    constant(video_toolbox, "_VTIsHardwareDecodeSupported", 0);
    // The encoder list is empty (VTCopyVideoEncoderList(options, CFArrayRef *out)), so no encoder has
    // properties to report, and stereo MV-HEVC neither decodes nor encodes.
    raw(video_toolbox, "_VTCopyVideoEncoderList", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) { s.x[0] = status(vt_parameter); return; }
        m.write64(s.x[1], cf.array(std::vector<Address>{}));
        s.x[0] = 0;
    });
    // VTCopySupportedPropertyDictionaryForEncoder(width, height, codec, encoderSpecification, CFStringRef *encoderIDOut,
    //     CFDictionaryRef *supportedPropertiesOut)
    raw(video_toolbox, "_VTCopySupportedPropertyDictionaryForEncoder", [](CpuState& s, GuestMemory& m) {
        if (s.x[4]) m.write64(s.x[4], 0);
        if (s.x[5]) m.write64(s.x[5], 0);
        s.x[0] = status(vt_could_not_find_encoder);
    });
    constant(video_toolbox, "_VTIsStereoMVHEVCDecodeSupported", 0);
    constant(video_toolbox, "_VTIsStereoMVHEVCEncodeSupported", 0);
    for (const auto* key : {"CodecType", "EncoderID", "CodecName", "EncoderName", "DisplayName", "GPURegistryID", "InstanceLimit",
                            "IsHardwareAccelerated", "PerformanceRating", "QualityRating", "SupportedSelectionProperties", "SupportsFrameReordering"})
        string(video_toolbox, "_kVTVideoEncoderList_" + std::string(key), key);
    raw(video_toolbox, "_VTRegisterSupplementalVideoDecoderIfAvailable", [](CpuState&, GuestMemory&) {});   // none is available
    for (const auto& [symbol, value] : std::array<std::pair<const char*, const char*>, 16>{{
             {"_kVTCompressionPropertyKey_AllowFrameReordering", "AllowFrameReordering"},
             {"_kVTCompressionPropertyKey_AverageBitRate", "AverageBitRate"},
             {"_kVTCompressionPropertyKey_DataRateLimits", "DataRateLimits"},
             {"_kVTCompressionPropertyKey_ExpectedFrameRate", "ExpectedFrameRate"},
             {"_kVTCompressionPropertyKey_MaxFrameDelayCount", "MaxFrameDelayCount"},
             {"_kVTCompressionPropertyKey_MaxKeyFrameInterval", "MaxKeyFrameInterval"},
             {"_kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration", "MaxKeyFrameIntervalDuration"},
             {"_kVTCompressionPropertyKey_ProfileLevel", "ProfileLevel"},
             {"_kVTCompressionPropertyKey_RealTime", "RealTime"},
             {"_kVTEncodeFrameOptionKey_ForceKeyFrame", "ForceKeyFrame"},
             {"_kVTProfileLevel_H264_Baseline_AutoLevel", "H264_Baseline_AutoLevel"},
             {"_kVTProfileLevel_H264_High_AutoLevel", "H264_High_AutoLevel"},
             {"_kVTProfileLevel_H264_Main_AutoLevel", "H264_Main_AutoLevel"},
             {"_kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder", "EnableHardwareAcceleratedVideoDecoder"},
             {"_kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder", "RequireHardwareAcceleratedVideoDecoder"},
             {"_kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder", "RequireHardwareAcceleratedVideoEncoder"}}})
        string(video_toolbox, symbol, value);
}

void Video::install_av_foundation() {
    // The device types of the macOS version Alchemy reports (14.0) are all here: a weak import of one that is
    // missing is NULL, and Chrome's capture service read through it (macOS 14's External, ContinuityCamera,
    // Microphone) and crashed on every start.
    for (const auto& [symbol, value] : std::array<std::pair<const char*, const char*>, 29>{{
             {"_AVCaptureDeviceTypeBuiltInWideAngleCamera", "AVCaptureDeviceTypeBuiltInWideAngleCamera"},
             {"_AVCaptureDeviceTypeExternalUnknown", "AVCaptureDeviceTypeExternalUnknown"},
             {"_AVCaptureDeviceTypeExternal", "AVCaptureDeviceTypeExternal"},
             {"_AVCaptureDeviceTypeContinuityCamera", "AVCaptureDeviceTypeContinuityCamera"},
             {"_AVCaptureDeviceTypeMicrophone", "AVCaptureDeviceTypeMicrophone"},
             {"_AVCaptureSessionPresetHigh", "AVCaptureSessionPresetHigh"},
             {"_AVFormatIDKey", "AVFormatIDKey"},
             {"_AVLinearPCMBitDepthKey", "AVLinearPCMBitDepthKey"},
             {"_AVLinearPCMIsBigEndianKey", "AVLinearPCMIsBigEndianKey"},
             {"_AVLinearPCMIsFloatKey", "AVLinearPCMIsFloatKey"},
             {"_AVLinearPCMIsNonInterleaved", "AVLinearPCMIsNonInterleaved"},
             {"_AVMetadataCommonKeyAlbumName", "albumName"},
             {"_AVMetadataCommonKeyArtist", "artist"},
             {"_AVMetadataCommonKeyCopyrights", "copyrights"},
             {"_AVMetadataCommonKeyPublisher", "publisher"},
             {"_AVMetadataCommonKeyTitle", "title"},
             {"_AVNumberOfChannelsKey", "AVNumberOfChannelsKey"},
             {"_AVSampleRateKey", "AVSampleRateKey"},
             {"_AVURLAssetPreferPreciseDurationAndTimingKey", "AVURLAssetPreferPreciseDurationAndTimingKey"},
             {"_AVCaptureDeviceWasConnectedNotification", "AVCaptureDeviceWasConnectedNotification"},
             {"_AVCaptureDeviceWasDisconnectedNotification", "AVCaptureDeviceWasDisconnectedNotification"},
             {"_AVCaptureSessionErrorKey", "AVCaptureSessionErrorKey"},
             {"_AVCaptureSessionRuntimeErrorNotification", "AVCaptureSessionRuntimeErrorNotification"},
             {"_AVLayerVideoGravityResize", "AVLayerVideoGravityResize"},
             {"_AVMediaTypeAudio", "soun"},
             {"_AVMediaTypeMuxed", "muxx"},
             {"_AVMediaTypeVideo", "vide"},
             {"_AVVideoScalingModeKey", "AVVideoScalingModeKey"},
             {"_AVVideoScalingModeResizeAspectFill", "AVVideoScalingModeResizeAspectFill"}}})
        string(av_foundation, symbol, value);
}

void Video::install_core_video() {
    for (const auto& [symbol, value] : std::array<std::pair<const char*, const char*>, 37>{{
             {"_kCVImageBufferFieldCountKey", "CVFieldCount"},
             {"_kCVImageBufferFieldDetailKey", "CVFieldDetail"},
             {"_kCVImageBufferFieldDetailTemporalTopFirst", "TemporalTopFirst"},
             {"_kCVImageBufferFieldDetailTemporalBottomFirst", "TemporalBottomFirst"},
             {"_kCVImageBufferFieldDetailSpatialFirstLineEarly", "SpatialFirstLineEarly"},
             {"_kCVImageBufferFieldDetailSpatialFirstLineLate", "SpatialFirstLineLate"},
             {"_kCVImageBufferPixelAspectRatioKey", "CVPixelAspectRatio"},
             {"_kCVImageBufferPixelAspectRatioHorizontalSpacingKey", "HorizontalSpacing"},
             {"_kCVImageBufferPixelAspectRatioVerticalSpacingKey", "VerticalSpacing"},
             {"_kCVPixelBufferCGBitmapContextCompatibilityKey", "CGBitmapContextCompatibility"},
             {"_kCVImageBufferColorPrimariesKey", "CVImageBufferColorPrimaries"},
             {"_kCVImageBufferColorPrimaries_EBU_3213", "EBU_3213"},
             {"_kCVImageBufferColorPrimaries_ITU_R_2020", "ITU_R_2020"},
             {"_kCVImageBufferColorPrimaries_ITU_R_709_2", "ITU_R_709_2"},
             {"_kCVImageBufferColorPrimaries_SMPTE_C", "SMPTE_C"},
             {"_kCVImageBufferContentLightLevelInfoKey", "ContentLightLevelInfo"},
             {"_kCVImageBufferGammaLevelKey", "CVImageBufferGammaLevel"},
             {"_kCVImageBufferMasteringDisplayColorVolumeKey", "MasteringDisplayColorVolume"},
             {"_kCVImageBufferTransferFunctionKey", "CVImageBufferTransferFunction"},
             {"_kCVImageBufferTransferFunction_ITU_R_2020", "ITU_R_2020"},
             {"_kCVImageBufferTransferFunction_ITU_R_2100_HLG", "ITU_R_2100_HLG"},
             {"_kCVImageBufferTransferFunction_ITU_R_709_2", "ITU_R_709_2"},
             {"_kCVImageBufferTransferFunction_Linear", "Linear"},
             {"_kCVImageBufferTransferFunction_SMPTE_240M_1995", "SMPTE_240M_1995"},
             {"_kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ", "SMPTE_ST_2084_PQ"},
             {"_kCVImageBufferTransferFunction_SMPTE_ST_428_1", "SMPTE_ST_428_1"},
             {"_kCVImageBufferTransferFunction_UseGamma", "UseGamma"},
             {"_kCVImageBufferTransferFunction_sRGB", "IEC_sRGB"},
             {"_kCVImageBufferYCbCrMatrixKey", "CVImageBufferYCbCrMatrix"},
             {"_kCVImageBufferYCbCrMatrix_ITU_R_2020", "ITU_R_2020"},
             {"_kCVImageBufferYCbCrMatrix_ITU_R_601_4", "ITU_R_601_4"},
             {"_kCVImageBufferYCbCrMatrix_ITU_R_709_2", "ITU_R_709_2"},
             {"_kCVImageBufferYCbCrMatrix_SMPTE_240M_1995", "SMPTE_240M_1995"},
             {"_kCVPixelBufferPoolAllocationThresholdKey", "AllocationThreshold"},
             {"_kCVPixelBufferPoolMinimumBufferCountKey", "MinimumBufferCount"},
             {"_kCVPixelBufferBytesPerRowAlignmentKey", "BytesPerRowAlignment"},
             {"_kCVImageBufferChromaLocationTopFieldKey", "CVImageBufferChromaLocationTopField"}}})
        string(core_video, symbol, value);
    // Host time: mach_absolute_time and its frequency from mach_timebase_info.
    raw(core_video, "_CVGetCurrentHostTime", [this](CpuState& s, GuestMemory&) { s.x[0] = call(libsystem, "_mach_absolute_time", {}); });
    raw(core_video, "_CVGetHostClockFrequency", [this](CpuState& s, GuestMemory& m) {
        const auto info = process.allocate_guest(8);
        static_cast<void>(call(libsystem, "_mach_timebase_info", {info}));
        const auto numer = m.read32(info), denom = m.read32(info + 4);
        process.release_guest(info);
        return_real(s, numer && denom ? 1e9 * denom / numer : 1e9);
    });
    raw(core_video, "_CVGetHostClockMinimumTimeDelta", [](CpuState& s, GuestMemory&) { s.x[0] = 1; });
    string(core_video, "_kCVImageBufferColorPrimaries_P22", "P22");
    install_code_points();
    // CVPixelBufferIsCompatibleWithAttributes(CVPixelBufferRef, CFDictionaryRef): the buffer's width,
    // height and pixel format (one or any of an array) against those the attributes name.
    raw(core_video, "_CVPixelBufferIsCompatibleWithAttributes", [this](CpuState& s, GuestMemory&) {
        const auto buffer = s.x[0], attributes = s.x[1];
        if (!buffer || call(core_foundation, "_CFGetTypeID", {buffer}) != call(core_video, "_CVPixelBufferGetTypeID", {})) { s.x[0] = 0; return; }
        if (!attributes || cf.kind(attributes) != ObjectKind::dictionary) { s.x[0] = 1; return; }
        const auto number = [this](Address value) { return value ? cf.number_value(value) : std::nullopt; };
        bool compatible = true;
        for (const auto& [key, value] : cf.dictionary_entries(attributes)) {
            const auto name = cf.utf8(key);
            if (name == "Width" || name == "Height") {
                const auto want = number(value);
                const auto have = call(core_video, name == "Width" ? "_CVPixelBufferGetWidth" : "_CVPixelBufferGetHeight", {buffer});
                if (want && static_cast<std::uint64_t>(*want) != have) compatible = false;
            } else if (name == "PixelFormatType") {
                const auto have = static_cast<double>(static_cast<std::uint32_t>(call(core_video, "_CVPixelBufferGetPixelFormatType", {buffer})));
                std::vector<Address> formats = cf.kind(value) == ObjectKind::array ? cf.array_values(value) : std::vector<Address>{value};
                if (std::none_of(formats.begin(), formats.end(), [&](Address f) { return number(f) == have; })) compatible = false;
            }
        }
        s.x[0] = compatible ? 1 : 0;
    });
    install_metal_texture_cache();
}

} // namespace

void install_video_toolbox(InstallContext& context) { std::make_shared<Video>(context)->install_video_toolbox(); }
void install_av_foundation(InstallContext& context) { std::make_shared<Video>(context)->install_av_foundation(); }
void install_core_video_gaps(InstallContext& context) { std::make_shared<Video>(context)->install_core_video(); }

} // namespace alchemy::gui::fill
