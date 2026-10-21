#include "alchemy/darwin/media/media.hpp"

#include <algorithm>
#include <array>

// VideoToolbox, AVFoundation and the CoreVideo pixel-buffer gaps.
//
// - VideoToolbox: Alchemy has no video decoder, encoder or pixel-transfer
//   implementation. Session creation fails with kVTCouldNotFindVideoDecoderErr,
//   kVTCouldNotFindVideoEncoderErr or kVTPixelTransferNotSupportedErr; since no
//   session can exist, session calls answer kVTInvalidSessionErr. Hardware
//   decoding is reported unsupported; no supplemental decoder is available.
// - AVFoundation: there are no capture devices. Device lists (and discovery
//   sessions) are empty, default devices nil, authorization "restricted" and
//   access requests are answered NO; capture inputs fail with
//   AVErrorDeviceNotConnected. Methods of devices, formats and frame-rate
//   ranges, which cannot exist, stop with an explicit message. Capture
//   sessions are real (configuration nesting, preset, outputs, running state)
//   but can have no input, so no frame is ever delivered; video data outputs
//   keep their delegate, queue and settings. AVSampleBufferDisplayLayer and
//   the still-image output are class shells.
// - CoreVideo: pixel buffers cannot be created under Alchemy
//   (CVPixelBufferCreate answers kCVReturnUnsupported), so pools and IOSurface,
//   planar-byte or wrapped-byte buffers fail the same way, and accessors of the
//   buffers that cannot exist answer the documented values for NULL. Metal
//   texture caches are real objects holding their device; since no image
//   buffer exists, no texture is ever made from one. CVBufferRetain/Release
//   retain and release any CF object; host time is mach_absolute_time's.
namespace alchemy::gui::fill {
namespace {

constexpr std::int32_t vt_parameter = -12902, vt_invalid_session = -12903, vt_pixel_transfer_not_supported = -12905,
    vt_could_not_find_decoder = -12906, vt_could_not_find_encoder = -12908;
constexpr std::int32_t cv_invalid_argument = -6661, cv_unsupported = -6663;

void impossible(std::uint64_t object, std::string_view what) {
    if (object) throw std::runtime_error("Alchemy: " + std::string(what) + " received an object that cannot exist under Alchemy");
}

struct CaptureSession final : ObjectPayload {
    bool running{};
    std::uint32_t configuring{};
    Address preset{};                  // retained
    std::vector<Address> outputs;      // retained
};
struct VideoDataOutput final : ObjectPayload {
    Address delegate{}, queue{}, settings{};
    bool discards_late{true};
};

class Video final : public Provider {
public:
    using Provider::Provider;
    void install_video_toolbox();
    void install_av_foundation();
    void install_core_video();
    void install_metal_texture_cache();
};

struct TextureCache final : ObjectPayload { Address device{}; };

// CVMetalTextureCache: a real cache object that holds its Metal device. Since
// no CVImageBuffer can exist under Alchemy, no texture is ever created from
// one and the cache stays empty (flushing has nothing to drop).
void Video::install_metal_texture_cache() {
    const auto cache_class = objc.find_class("__AlchemyCVMetalTextureCache") ? objc.find_class("__AlchemyCVMetalTextureCache")
        : objc.register_native_class("__AlchemyCVMetalTextureCache", objc.find_class("NSObject"), 8);
    if (!has(core_video, "_CVMetalTextureCacheCreate")) {
        static_cast<void>(objc.add_native_method(cache_class, "dealloc", [self = shared_from_this(), this, cache_class](CpuState& s, GuestMemory&) {
            if (const auto value = objc.find_host_state<TextureCache>(s.x[0])) release(std::exchange(value->device, 0));
            static_cast<void>(objc.send_super(s.x[0], cache_class, objc.selector("dealloc")));
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
    // CVMetalTextureCacheCreateTextureFromImage(allocator, cache, CVImageBufferRef image, attributes, pixelFormat,
    //     width, height, planeIndex, [stack] CVMetalTextureRef *out): no image buffer can exist.
    raw(core_video, "_CVMetalTextureCacheCreateTextureFromImage", [](CpuState& s, GuestMemory& m) {
        const auto out = m.read64(s.sp);
        if (out) m.write64(out, 0);
        impossible(s.x[2], "CVMetalTextureCacheCreateTextureFromImage");
        s.x[0] = status(cv_invalid_argument);
    });
    // No CVMetalTexture can exist: NULL answers nil.
    raw(core_video, "_CVMetalTextureGetTexture", [](CpuState& s, GuestMemory&) { impossible(s.x[0], "CVMetalTextureGetTexture"); s.x[0] = 0; });
}

void Video::install_video_toolbox() {
    failure(video_toolbox, "_VTDecompressionSessionCreate", vt_could_not_find_decoder, {5});
    // VTCompressionSessionCreate(allocator, width, height, codec, encoderSpecification, sourceAttributes,
    //     compressedDataAllocator, outputCallback, [stack] refcon, compressionSessionOut)
    failure(video_toolbox, "_VTCompressionSessionCreate", vt_could_not_find_encoder, {9});
    failure(video_toolbox, "_VTPixelTransferSessionCreate", vt_pixel_transfer_not_supported, {1});
    for (const auto* name : {"_VTCompressionSessionCompleteFrames", "_VTCompressionSessionEncodeFrame", "_VTDecompressionSessionDecodeFrame",
                             "_VTDecompressionSessionDecodeFrameWithOutputHandler",
                             "_VTDecompressionSessionWaitForAsynchronousFrames", "_VTPixelTransferSessionTransferImage",
                             "_VTSessionCopyProperty", "_VTSessionSetProperty"})
        raw(video_toolbox, name, [](CpuState& s, GuestMemory&) { s.x[0] = status(s.x[0] ? vt_invalid_session : vt_parameter); });
    raw(video_toolbox, "_VTSessionCopySupportedPropertyDictionary", [](CpuState& s, GuestMemory& m) {
        if (s.x[1]) m.write64(s.x[1], 0);
        s.x[0] = status(s.x[0] ? vt_invalid_session : vt_parameter);
    });
    for (const auto* name : {"_VTCompressionSessionInvalidate", "_VTDecompressionSessionInvalidate", "_VTPixelTransferSessionInvalidate"}) {
        const std::string what(name + 1);
        raw(video_toolbox, name, [what](CpuState& s, GuestMemory&) { impossible(s.x[0], what); });
    }
    constant(video_toolbox, "_VTIsHardwareDecodeSupported", 0);
    // No decompression session can exist, so none accepts a format description.
    raw(video_toolbox, "_VTDecompressionSessionCanAcceptFormatDescription", [](CpuState& s, GuestMemory&) {
        impossible(s.x[0], "VTDecompressionSessionCanAcceptFormatDescription");
        s.x[0] = 0;
    });
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
    // No capture devices exist.
    const auto device = cls(av_foundation, "AVCaptureDevice", "NSObject");
    method(device, "devices", [this](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(cf.array({})); }, true);
    method(device, "devicesWithMediaType:", [this](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(cf.array({})); }, true);
    method(device, "defaultDeviceWithMediaType:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, true);
    method(device, "deviceWithUniqueID:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, true);
    // AVAuthorizationStatusRestricted: access cannot be granted.
    method(device, "authorizationStatusForMediaType:", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, true, "q@:@");
    method(device, "requestAccessForMediaType:completionHandler:", [this](CpuState& s, GuestMemory&) {
        if (s.x[3]) static_cast<void>(bridge->invoke_block(s.x[3], {0}));
    }, true, "v@:@@?");
    const auto input = cls(av_foundation, "AVCaptureInput", "NSObject");
    const auto device_input = cls(av_foundation, "AVCaptureDeviceInput", "AVCaptureInput");
    static_cast<void>(input);
    const auto not_connected = [this](GuestMemory& m, Address out) {
        if (out) m.write64(out, bridge->autorelease(bridge->error("AVFoundationErrorDomain", -11814, "No capture device is connected")));
    };
    method(device_input, "deviceInputWithDevice:error:", [not_connected](CpuState& s, GuestMemory& m) { not_connected(m, s.x[3]); s.x[0] = 0; }, true);
    method(device_input, "initWithDevice:error:", [this, not_connected](CpuState& s, GuestMemory& m) {
        not_connected(m, s.x[3]);
        bridge->release(s.x[0]);
        s.x[0] = 0;
    });
    // Methods of objects that cannot exist (no device, so no format or frame-rate range either).
    const auto impossible_method = [this](Address target, const char* class_name, const char* selector, const char* types) {
        const std::string what = std::string("-[") + class_name + " " + selector + "]";
        method(target, selector, [what](CpuState&, GuestMemory&) {
            throw std::runtime_error("unsupported: Alchemy: " + what + " (no capture device exists, so no instance can)");
        }, false, types);
    };
    for (const auto& [selector, types] : std::initializer_list<std::pair<const char*, const char*>>{
             {"activeFormat", "@@:"}, {"formats", "@@:"}, {"activeVideoMinFrameDuration", "{?=qiIq}@:"}, {"activeVideoMaxFrameDuration", "{?=qiIq}@:"},
             {"lockForConfiguration:", "B@:^@"}, {"unlockForConfiguration", "v@:"}, {"setActiveFormat:", "v@:@"},
             {"setActiveVideoMinFrameDuration:", "v@:{?=qiIq}"}, {"setActiveVideoMaxFrameDuration:", "v@:{?=qiIq}"},
             {"uniqueID", "@@:"}, {"localizedName", "@@:"}, {"hasMediaType:", "B@:@"}, {"isConnected", "B@:"}})
        impossible_method(device, "AVCaptureDevice", selector, types);
    const auto device_format = cls(av_foundation, "AVCaptureDeviceFormat", "NSObject");
    for (const auto& [selector, types] : std::initializer_list<std::pair<const char*, const char*>>{
             {"formatDescription", "^{opaqueCMFormatDescription=}@:"}, {"videoSupportedFrameRateRanges", "@@:"}, {"mediaType", "@@:"}})
        impossible_method(device_format, "AVCaptureDeviceFormat", selector, types);
    const auto frame_rate_range = cls(av_foundation, "AVFrameRateRange", "NSObject");
    for (const auto& [selector, types] : std::initializer_list<std::pair<const char*, const char*>>{
             {"minFrameRate", "d@:"}, {"maxFrameRate", "d@:"}, {"minFrameDuration", "{?=qiIq}@:"}, {"maxFrameDuration", "{?=qiIq}@:"}})
        impossible_method(frame_rate_range, "AVFrameRateRange", selector, types);
    // Discovery sessions find no device.
    const auto discovery = cls(av_foundation, "AVCaptureDeviceDiscoverySession", "NSObject");
    method(discovery, "discoverySessionWithDeviceTypes:mediaType:position:", [this](CpuState& s, GuestMemory&) {
        const auto made = objc.send(objc.send(s.x[0], objc.selector("alloc"), {}).x0, objc.selector("init"), {}).x0;
        s.x[0] = bridge->autorelease(made);
    }, true, "@@:@@q");
    method(discovery, "devices", [this](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(cf.array({})); });
    method(discovery, "supportedMultiCamDeviceSets", [this](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(cf.array({})); });

    // Sessions: real configuration state; no input can be added (none can exist).
    const auto session = cls(av_foundation, "AVCaptureSession", "NSObject");
    static_cast<void>(cls(av_foundation, "AVCaptureOutput", "NSObject"));
    static_cast<void>(cls(av_foundation, "AVCaptureStillImageOutput", "AVCaptureOutput"));
    const auto data_output = cls(av_foundation, "AVCaptureVideoDataOutput", "AVCaptureOutput");
    const auto output_class = objc.find_class("AVCaptureOutput");
    const auto session_state = [this](Address self) { return objc.host_state<CaptureSession>(self); };
    const auto kind_of = [this](Address object, Address target) {
        CallRegs registers{};
        registers.x[2] = target;
        return (objc.send(object, objc.selector("isKindOfClass:"), registers).x0 & 0xff) != 0;
    };
    method(session, "canAddInput:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:@");
    method(session, "addInput:", [](CpuState& s, GuestMemory&) {
        throw std::runtime_error(s.x[2] ? "unsupported: Alchemy: -[AVCaptureSession addInput:] with an input (no capture device exists, so no input can)"
                                        : "Alchemy: -[AVCaptureSession addInput:] with nil (NSInvalidArgumentException)");
    }, false, "v@:@");
    method(session, "inputs", [this](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(cf.array({})); });
    method(session, "canAddOutput:", [session_state, output_class, kind_of](CpuState& s, GuestMemory&) {
        const auto& outputs = session_state(s.x[0])->outputs;
        s.x[0] = s.x[2] && kind_of(s.x[2], output_class) && std::find(outputs.begin(), outputs.end(), s.x[2]) == outputs.end() ? 1 : 0;
    }, false, "B@:@");
    method(session, "addOutput:", [this, session_state, output_class, kind_of](CpuState& s, GuestMemory&) {
        auto state = session_state(s.x[0]);
        if (!s.x[2] || !kind_of(s.x[2], output_class) || std::find(state->outputs.begin(), state->outputs.end(), s.x[2]) != state->outputs.end())
            throw std::runtime_error("Alchemy: -[AVCaptureSession addOutput:] cannot add this output (NSInvalidArgumentException)");
        state->outputs.push_back(retain(s.x[2]));
    }, false, "v@:@");
    method(session, "removeOutput:", [this, session_state](CpuState& s, GuestMemory&) {
        auto state = session_state(s.x[0]);
        const auto found = std::find(state->outputs.begin(), state->outputs.end(), s.x[2]);
        if (found == state->outputs.end()) return;
        state->outputs.erase(found);
        release(s.x[2]);
    }, false, "v@:@");
    method(session, "outputs", [this, session_state](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(cf.array(session_state(s.x[0])->outputs)); });
    method(session, "beginConfiguration", [session_state](CpuState& s, GuestMemory&) { ++session_state(s.x[0])->configuring; }, false, "v@:");
    method(session, "commitConfiguration", [session_state](CpuState& s, GuestMemory&) {
        auto state = session_state(s.x[0]);
        if (state->configuring == 0) throw std::runtime_error("Alchemy: -[AVCaptureSession commitConfiguration] without beginConfiguration (NSGenericException)");
        --state->configuring;
    }, false, "v@:");
    method(session, "sessionPreset", [this, session_state](CpuState& s, GuestMemory&) {
        auto state = session_state(s.x[0]);
        s.x[0] = state->preset ? state->preset : cf.constant_string("AVCaptureSessionPresetHigh");
    });
    method(session, "canSetSessionPreset:", [](CpuState& s, GuestMemory&) { s.x[0] = s.x[2] ? 1 : 0; }, false, "B@:@");   // no input limits a preset
    method(session, "setSessionPreset:", [this, session_state](CpuState& s, GuestMemory&) {
        if (!s.x[2]) throw std::runtime_error("Alchemy: -[AVCaptureSession setSessionPreset:] with nil (NSInvalidArgumentException)");
        auto state = session_state(s.x[0]);
        const auto old = std::exchange(state->preset, cf.copy(s.x[2]));
        release(old);
    }, false, "v@:@");
    // Running with no input: the session runs and delivers nothing.
    method(session, "startRunning", [session_state](CpuState& s, GuestMemory&) { session_state(s.x[0])->running = true; }, false, "v@:");
    method(session, "stopRunning", [session_state](CpuState& s, GuestMemory&) { session_state(s.x[0])->running = false; }, false, "v@:");
    method(session, "isRunning", [session_state](CpuState& s, GuestMemory&) { s.x[0] = session_state(s.x[0])->running ? 1 : 0; }, false, "B@:");
    method(session, "isInterrupted", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:");
    method(session, "dealloc", [this, session](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<CaptureSession>(s.x[0])) {
            for (const auto output : state->outputs) release(output);
            state->outputs.clear();
            release(std::exchange(state->preset, 0));
        }
        static_cast<void>(objc.send_super(s.x[0], session, objc.selector("dealloc")));
    }, false, "v@:");
    // Video data outputs: delegate (not retained), callback queue (retained) and settings.
    const auto output_state = [this](Address self) { return objc.host_state<VideoDataOutput>(self); };
    method(data_output, "setSampleBufferDelegate:queue:", [this, output_state](CpuState& s, GuestMemory&) {
        if (s.x[2] && !s.x[3]) throw std::runtime_error("Alchemy: -[AVCaptureVideoDataOutput setSampleBufferDelegate:queue:] needs a queue (NSInvalidArgumentException)");
        auto state = output_state(s.x[0]);
        if (s.x[3]) static_cast<void>(call(libsystem, "_dispatch_retain", {s.x[3]}));
        if (state->queue) static_cast<void>(call(libsystem, "_dispatch_release", {state->queue}));
        state->delegate = s.x[2];
        state->queue = s.x[2] ? s.x[3] : 0;
        if (!s.x[2] && s.x[3]) static_cast<void>(call(libsystem, "_dispatch_release", {s.x[3]}));
    }, false, "v@:@@");
    method(data_output, "sampleBufferDelegate", [output_state](CpuState& s, GuestMemory&) { s.x[0] = output_state(s.x[0])->delegate; });
    method(data_output, "sampleBufferCallbackQueue", [output_state](CpuState& s, GuestMemory&) { s.x[0] = output_state(s.x[0])->queue; });
    method(data_output, "videoSettings", [output_state](CpuState& s, GuestMemory&) { s.x[0] = output_state(s.x[0])->settings; });
    method(data_output, "setVideoSettings:", [this, output_state](CpuState& s, GuestMemory&) {
        auto state = output_state(s.x[0]);
        const auto old = std::exchange(state->settings, s.x[2] ? cf.copy(s.x[2]) : 0);
        release(old);
    }, false, "v@:@");
    method(data_output, "alwaysDiscardsLateVideoFrames", [output_state](CpuState& s, GuestMemory&) { s.x[0] = output_state(s.x[0])->discards_late ? 1 : 0; }, false, "B@:");
    method(data_output, "setAlwaysDiscardsLateVideoFrames:", [output_state](CpuState& s, GuestMemory&) { output_state(s.x[0])->discards_late = (s.x[2] & 0xff) != 0; }, false, "v@:B");
    method(data_output, "dealloc", [this, data_output](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<VideoDataOutput>(s.x[0])) {
            if (state->queue) static_cast<void>(call(libsystem, "_dispatch_release", {std::exchange(state->queue, 0)}));
            release(std::exchange(state->settings, 0));
        }
        static_cast<void>(objc.send_super(s.x[0], data_output, objc.selector("dealloc")));
    }, false, "v@:");
    static_cast<void>(cls(av_foundation, "AVSampleBufferDisplayLayer", "CALayer"));
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
    constant(core_video, "_CVPixelBufferGetTypeID", static_cast<Address>(TypeId::cv_pixel_buffer));
    // Creation fails like CVPixelBufferCreate.
    failure(core_video, "_CVPixelBufferCreateWithIOSurface", cv_unsupported, {3});
    // CVPixelBufferCreateWithBytes(allocator, width, height, format, base, bytesPerRow, releaseCallback,
    //     releaseRefCon, [stack] attributes, pixelBufferOut): the callback is not called (nothing was made).
    failure(core_video, "_CVPixelBufferCreateWithBytes", cv_unsupported, {9});
    // Retain/release of any CF object (NULL-safe); a pixel buffer itself cannot exist.
    for (const auto* name : {"_CVBufferRetain", "_CVPixelBufferRetain"})
        raw(core_video, name, [this](CpuState& s, GuestMemory&) { s.x[0] = retain(s.x[0]); });
    raw(core_video, "_CVBufferRelease", [this](CpuState& s, GuestMemory&) { release(s.x[0]); });
    raw(core_video, "_CVPixelBufferPoolRelease", [](CpuState& s, GuestMemory&) { impossible(s.x[0], "CVPixelBufferPoolRelease"); });
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
    install_metal_texture_cache();
    // CVPixelBufferCreateWithPlanarBytes(15 arguments): pixelBufferOut is the 7th stack argument.
    failure(core_video, "_CVPixelBufferCreateWithPlanarBytes", cv_unsupported, {14});
    failure(core_video, "_CVPixelBufferPoolCreate", cv_unsupported, {3});
    failure(core_video, "_CVPixelBufferPoolCreatePixelBuffer", cv_invalid_argument, {2});
    failure(core_video, "_CVPixelBufferPoolCreatePixelBufferWithAuxAttributes", cv_invalid_argument, {3});
    raw(core_video, "_CVPixelBufferPoolFlush", [](CpuState& s, GuestMemory&) { impossible(s.x[0], "CVPixelBufferPoolFlush"); });
    // Accessors of pixel buffers (none can exist): the documented values for NULL.
    for (const auto* name : {"_CVPixelBufferGetBaseAddress", "_CVPixelBufferGetBaseAddressOfPlane", "_CVPixelBufferGetBytesPerRow",
                             "_CVPixelBufferGetBytesPerRowOfPlane", "_CVPixelBufferGetDataSize", "_CVPixelBufferGetHeight",
                             "_CVPixelBufferGetHeightOfPlane", "_CVPixelBufferGetPixelFormatType", "_CVPixelBufferGetPlaneCount",
                             "_CVPixelBufferGetWidth", "_CVPixelBufferGetWidthOfPlane", "_CVPixelBufferIsPlanar", "_CVBufferGetAttachment"}) {
        const std::string what(name + 1);
        raw(core_video, name, [what](CpuState& s, GuestMemory&) { impossible(s.x[0], what); s.x[0] = 0; });
    }
    for (const auto* name : {"_CVPixelBufferLockBaseAddress", "_CVPixelBufferUnlockBaseAddress"}) {
        const std::string what(name + 1);
        raw(core_video, name, [what](CpuState& s, GuestMemory&) { impossible(s.x[0], what); s.x[0] = status(cv_invalid_argument); });
    }
    raw(core_video, "_CVBufferSetAttachment", [](CpuState& s, GuestMemory&) { impossible(s.x[0], "CVBufferSetAttachment"); });
    // CGRect / CGSize results (zero for NULL).
    raw(core_video, "_CVImageBufferGetCleanRect", [](CpuState& s, GuestMemory&) {
        impossible(s.x[0], "CVImageBufferGetCleanRect");
        for (unsigned i = 0; i < 4; ++i) return_real(s, 0, i);
    });
    for (const auto* name : {"_CVImageBufferGetDisplaySize", "_CVImageBufferGetEncodedSize"}) {
        const std::string what(name + 1);
        raw(core_video, name, [what](CpuState& s, GuestMemory&) { impossible(s.x[0], what); return_real(s, 0, 0); return_real(s, 0, 1); });
    }
}

} // namespace

void install_video_toolbox(InstallContext& context) { std::make_shared<Video>(context)->install_video_toolbox(); }
void install_av_foundation(InstallContext& context) { std::make_shared<Video>(context)->install_av_foundation(); }
void install_core_video_gaps(InstallContext& context) { std::make_shared<Video>(context)->install_core_video(); }

} // namespace alchemy::gui::fill
