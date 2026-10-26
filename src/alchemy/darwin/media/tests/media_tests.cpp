// media provider tests: the Qt WebEngine media imports resolve; CoreMedia
// data types are real (CMTime, CMBlockBuffer, CMFormatDescription with H.264
// SPS parsing, CMSampleBuffer); codecs and capture devices are absent with the
// documented error codes; with the host's audio and MIDI devices hidden
// (ALCHEMY_AUDIO_DEVICES/ALCHEMY_MIDI_DEVICES=none) the device lists are empty;
// MIDI clients, packet lists and caption-appearance defaults are real.
#include "alchemy/gui/tests/fill_fixture.hpp"
#include "alchemy/darwin/media/media.hpp"

#include <cmath>

namespace {
using namespace fill_test;

#define FRAMEWORK(name) "/System/Library/Frameworks/" name ".framework/Versions/A/" name
constexpr std::string_view cm = FRAMEWORK("CoreMedia"), vt = FRAMEWORK("VideoToolbox"), av = FRAMEWORK("AVFoundation"),
    cv = FRAMEWORK("CoreVideo"), at = FRAMEWORK("AudioToolbox"), au = FRAMEWORK("AudioUnit"), ca = FRAMEWORK("CoreAudio"),
    midi = FRAMEWORK("CoreMIDI"), ma = FRAMEWORK("MediaAccessibility"), ic = FRAMEWORK("ImageCaptureCore");
#undef FRAMEWORK
constexpr std::string_view sys = "/usr/lib/libSystem.B.dylib";

std::uint32_t fourcc(const char (&text)[5]) {
    return (std::uint32_t(std::uint8_t(text[0])) << 24) | (std::uint32_t(std::uint8_t(text[1])) << 16) |
        (std::uint32_t(std::uint8_t(text[2])) << 8) | std::uint8_t(text[3]);
}

// Exp-Golomb writer for synthetic H.264 SPS NAL units.
struct BitWriter {
    std::vector<std::uint8_t> bytes{0x67};   // nal_unit_type 7
    unsigned used{8};
    void bit(unsigned value) {
        if (used == 8) { bytes.push_back(0); used = 0; }
        if (value) bytes.back() |= static_cast<std::uint8_t>(0x80U >> used);
        ++used;
    }
    void bits(std::uint32_t value, unsigned count) { while (count--) bit((value >> count) & 1U); }
    void ue(std::uint32_t value) {
        const auto coded = value + 1;
        unsigned length = 0;
        while ((coded >> length) > 1) ++length;
        bits(0, length);
        bits(coded, length + 1);
    }
};
std::vector<std::uint8_t> sps(std::uint32_t width_mbs, std::uint32_t height_units, std::uint32_t crop_bottom) {
    BitWriter w;
    w.bits(66, 8); w.bits(0xc0, 8); w.bits(31, 8);   // baseline, constraint flags, level 3.1
    w.ue(0);           // seq_parameter_set_id
    w.ue(0);           // log2_max_frame_num_minus4
    w.ue(2);           // pic_order_cnt_type
    w.ue(1);           // max_num_ref_frames
    w.bit(0);          // gaps_in_frame_num_value_allowed_flag
    w.ue(width_mbs - 1);
    w.ue(height_units - 1);
    w.bit(1);          // frame_mbs_only_flag
    w.bit(1);          // direct_8x8_inference_flag
    w.bit(crop_bottom ? 1 : 0);
    if (crop_bottom) { w.ue(0); w.ue(0); w.ue(0); w.ue(crop_bottom); }
    w.bit(0);          // vui_parameters_present_flag
    w.bit(1);          // rbsp stop bit
    return w.bytes;
}

void imports(Fixture& f) { imports_resolve(f, {cm, vt, av, cv, at, au, ca, midi, ma, ic}); }

void core_media(Fixture& f) {
    // CMTime
    alchemy::CallRegs make{};
    make.x[0] = 3; make.x[1] = 2;
    const auto time = f.block(24);
    make.indirect_result = time;
    static_cast<void>(f.regs(cm, "_CMTimeMake", make));
    require(f.engine.read64(time) == 3 && f.engine.read32(time + 8) == 2 && f.engine.read32(time + 12) == 1, "CMTimeMake");
    alchemy::CallRegs seconds{};
    seconds.x[0] = time;
    require(f.regs(cm, "_CMTimeGetSeconds", seconds).d(0) == 1.5, "CMTimeGetSeconds");
    seconds.x[0] = f.symbol(cm, "_kCMTimeInvalid");
    require(std::isnan(f.regs(cm, "_CMTimeGetSeconds", seconds).d(0)), "invalid time is NaN");
    // CMBlockBuffer
    const auto out = f.block(8);
    alchemy::CallRegs create{};
    create.x[2] = 16; create.x[6] = 16; create.x[7] = 1;   // blockLength, dataLength, kCMBlockBufferAssureMemoryNowFlag
    create.push_stack(out);
    require(static_cast<std::int32_t>(f.regs(cm, "_CMBlockBufferCreateWithMemoryBlock", create).x0) == 0, "CMBlockBufferCreateWithMemoryBlock");
    const auto buffer = f.engine.read64(out);
    require(f.call(cm, "_CMBlockBufferGetDataLength", {buffer}) == 16, "data length");
    require(f.status(cm, "_CMBlockBufferReplaceDataBytes", {f.text("abcd"), buffer, 2, 4}) == 0, "ReplaceDataBytes");
    const auto pointer = f.block(8), at_offset = f.block(8), total = f.block(8);
    require(f.status(cm, "_CMBlockBufferGetDataPointer", {buffer, 2, at_offset, total, pointer}) == 0 && f.engine.read64(at_offset) == 14 &&
            f.engine.read64(total) == 16, "GetDataPointer");
    require(f.read_text(f.engine.read64(pointer), 4) == "abcd", "data bytes");
    require(f.status(cm, "_CMBlockBufferGetDataPointer", {buffer, 16, 0, 0, pointer}) == -12703, "offset out of range");
    require(f.status(cm, "_CMBlockBufferReplaceDataBytes", {f.text("abcd"), buffer, 14, 4}) == -12704, "replace past the end");
    require(f.call(cm, "_CMBlockBufferIsRangeContiguous", {buffer, 0, 0}) == 1, "contiguous");
    const auto contiguous = f.block(8);
    require(f.status(cm, "_CMBlockBufferCreateContiguous", {0, buffer, 0, 0, 2, 4, 0, contiguous}) == 0, "CreateContiguous");
    require(f.call(cm, "_CMBlockBufferGetDataLength", {f.engine.read64(contiguous)}) == 4, "contiguous copy length");
    // CMFormatDescription
    const auto description = f.block(8);
    require(f.status(cm, "_CMVideoFormatDescriptionCreate", {0, fourcc("avc1"), 640, 480, 0, description}) == 0, "VideoFormatDescriptionCreate");
    const auto desc = f.engine.read64(description);
    require(f.call(cm, "_CMVideoFormatDescriptionGetDimensions", {desc}) == (640ULL | (480ULL << 32)), "dimensions");
    require(f.call(cm, "_CMFormatDescriptionGetMediaSubType", {desc}) == fourcc("avc1"), "subtype");
    alchemy::CallRegs aperture{};
    aperture.x[0] = desc;
    const auto rect = f.regs(cm, "_CMVideoFormatDescriptionGetCleanAperture", aperture);
    require(rect.d(2) == 640 && rect.d(3) == 480, "clean aperture");
    // H.264 parameter sets: 1280x720, and 1920x1080 through cropping.
    require(alchemy::gui::fill::h264_sps_dimensions(sps(80, 45, 0)) == std::pair<std::int32_t, std::int32_t>{1280, 720}, "SPS 720p");
    require(alchemy::gui::fill::h264_sps_dimensions(sps(120, 68, 4)) == std::pair<std::int32_t, std::int32_t>{1920, 1080}, "SPS 1080p cropped");
    require(!alchemy::gui::fill::h264_sps_dimensions(std::vector<std::uint8_t>{0x68, 0xce, 0x38, 0x80}), "PPS is not an SPS");
    const auto set0 = sps(80, 45, 0);
    const std::vector<std::uint8_t> set1{0x68, 0xce, 0x38, 0x80};
    const auto p0 = f.block(set0.size()), p1 = f.block(set1.size());
    f.engine.write_bytes(p0, set0);
    f.engine.write_bytes(p1, set1);
    const auto pointers = f.block(16), sizes = f.block(16);
    f.engine.write64(pointers, p0); f.engine.write64(pointers + 8, p1);
    f.engine.write64(sizes, set0.size()); f.engine.write64(sizes + 8, set1.size());
    require(f.status(cm, "_CMVideoFormatDescriptionCreateFromH264ParameterSets", {0, 2, pointers, sizes, 4, description}) == 0, "from parameter sets");
    const auto h264 = f.engine.read64(description);
    require(f.call(cm, "_CMVideoFormatDescriptionGetDimensions", {h264}) == (1280ULL | (720ULL << 32)), "H.264 dimensions");
    const auto set = f.block(8), set_size = f.block(8), count = f.block(8), header = f.block(4);
    require(f.status(cm, "_CMVideoFormatDescriptionGetH264ParameterSetAtIndex", {h264, 1, set, set_size, count, header}) == 0 &&
            f.engine.read64(set_size) == 4 && f.engine.read64(count) == 2 && f.engine.read32(header) == 4 &&
            f.memory().read8(f.engine.read64(set)) == 0x68, "GetH264ParameterSetAtIndex");
    require(f.status(cm, "_CMVideoFormatDescriptionCreateFromH264ParameterSets", {0, 2, pointers, sizes, 3, description}) == -12710, "NAL header length 3");
    require(f.status(cm, "_CMVideoFormatDescriptionCreateForImageBuffer", {0, 0, description}) == -12710, "no image buffers");
    // CMSampleBuffer
    const auto timing = f.block(72);
    f.engine.write64(timing + 24, 7); f.engine.write32(timing + 32, 1); f.engine.write32(timing + 36, 1);   // PTS 7/1
    const auto sample_sizes = f.block(8);
    f.engine.write64(sample_sizes, 16);
    const auto sample = f.block(8);
    alchemy::CallRegs ready{};
    ready.x[1] = buffer; ready.x[2] = desc; ready.x[3] = 1; ready.x[4] = 1; ready.x[5] = timing; ready.x[6] = 1; ready.x[7] = sample_sizes;
    ready.push_stack(sample);
    require(static_cast<std::int32_t>(f.regs(cm, "_CMSampleBufferCreateReady", ready).x0) == 0, "CMSampleBufferCreateReady");
    const auto sbuf = f.engine.read64(sample);
    require(f.call(cm, "_CMSampleBufferGetDataBuffer", {sbuf}) == buffer && f.call(cm, "_CMSampleBufferGetFormatDescription", {sbuf}) == desc, "sample parts");
    require(f.call(cm, "_CMSampleBufferGetImageBuffer", {sbuf}) == 0, "no image buffer");
    alchemy::CallRegs pts{};
    pts.x[0] = sbuf;
    const auto pts_out = f.block(24);
    pts.indirect_result = pts_out;
    static_cast<void>(f.regs(cm, "_CMSampleBufferGetPresentationTimeStamp", pts));
    require(f.engine.read64(pts_out) == 7 && f.engine.read32(pts_out + 8) == 1, "presentation time stamp");
    const auto attachments = f.call(cm, "_CMSampleBufferGetSampleAttachmentsArray", {sbuf, 1});
    require(attachments && f.cf.length(attachments) == 1, "sample attachments");
    require(f.call(cm, "_CMSampleBufferGetSampleAttachmentsArray", {sbuf, 0}) == attachments, "attachments kept");
    require(f.string_constant(cm, "_kCMSampleAttachmentKey_NotSync") == "NotSync", "attachment key");
}

void video(Fixture& f) {
    const auto out = f.block(8, 0xff);
    require(f.status(vt, "_VTDecompressionSessionCreate", {0, 1, 0, 0, 0, out}) == -12906 && f.engine.read64(out) == 0, "no decoder");
    alchemy::CallRegs compress{};
    compress.x[1] = 640; compress.x[2] = 480; compress.x[3] = fourcc("avc1");
    const auto session = f.block(8, 0xff);
    compress.push_stack(Address{0});
    compress.push_stack(session);
    require(static_cast<std::int32_t>(f.regs(vt, "_VTCompressionSessionCreate", compress).x0) == -12908 && f.engine.read64(session) == 0, "no encoder");
    require(f.status(vt, "_VTPixelTransferSessionCreate", {0, out}) == 0 && f.engine.read64(out) != 0, "a pixel transfer session");
    require(f.status(vt, "_VTSessionSetProperty", {0, 0, 0}) == -12902, "session calls");
    require(f.call(vt, "_VTIsHardwareDecodeSupported", {fourcc("avc1")}) == 0, "no hardware decode");
    // AVFoundation
    require(f.string_constant(av, "_AVMediaTypeVideo") == "vide", "AVMediaTypeVideo");
    for (const auto* type : {"AVCaptureDeviceTypeExternal", "AVCaptureDeviceTypeContinuityCamera", "AVCaptureDeviceTypeMicrophone"})
        require(f.string_constant(av, std::string("_") + type) == type, std::string("macOS 14 capture device type ") + type);
    const auto device = f.cls("AVCaptureDevice");
    require(f.cf.length(f.send(device, "devicesWithMediaType:", {f.engine.read64(f.symbol(av, "_AVMediaTypeVideo"))})) == 0, "no capture devices");
    require(f.send(device, "defaultDeviceWithMediaType:", {0}) == 0, "no default device");
    require(f.send(device, "authorizationStatusForMediaType:", {0}) == 1, "restricted");
    const auto error = f.block(8);
    require(f.send(f.cls("AVCaptureDeviceInput"), "deviceInputWithDevice:error:", {0, error}) == 0, "no input");
    require(f.call("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "_CFErrorGetCode", {f.engine.read64(error)}) ==
            static_cast<Address>(-11814), "AVErrorDeviceNotConnected");
    // CoreVideo
    require(f.status(cv, "_CVPixelBufferPoolCreate", {0, 0, 0, out}) == -6682 && f.engine.read64(out) == 0, "a pool needs its buffers' size");
    require(f.call(cv, "_CVPixelBufferGetWidth", {0}) == 0, "NULL width");
    require(f.status(cv, "_CVPixelBufferLockBaseAddress", {0, 0}) == -6661, "NULL lock");
}

void audio(Fixture& f) {
    const auto address = f.block(12), size = f.block(4), value = f.block(4, 0xff);
    f.engine.write32(address, fourcc("dev#"));
    require(f.status(ca, "_AudioObjectGetPropertyDataSize", {1, address, 0, 0, size}) == 0 && f.engine.read32(size) == 0, "no devices");
    f.engine.write32(address, fourcc("dOut"));
    f.engine.write32(size, 4);
    require(f.status(ca, "_AudioObjectGetPropertyData", {1, address, 0, 0, size, value}) == 0 && f.engine.read32(value) == 0, "no default output");
    require(f.call(ca, "_AudioObjectHasProperty", {1, address}) == 1, "HasProperty");
    require(static_cast<std::uint32_t>(f.call(ca, "_AudioObjectGetPropertyDataSize", {5, address, 0, 0, size})) == fourcc("!obj"), "bad object");
    f.engine.write32(address, fourcc("zzzz"));
    require(static_cast<std::uint32_t>(f.call(ca, "_AudioObjectGetPropertyDataSize", {1, address, 0, 0, size})) == fourcc("who?"), "unknown property");
    const auto timebase = f.block(8);
    static_cast<void>(f.call(sys, "_mach_timebase_info", {timebase}));
    const auto numer = f.engine.read32(timebase), denom = f.engine.read32(timebase + 4);
    require(f.call(ca, "_AudioConvertHostTimeToNanos", {1000000}) == 1000000ULL * numer / denom, "host time to nanoseconds");
    require(static_cast<std::uint32_t>(f.call(at, "_AudioConverterNew", {f.block(40), f.block(40), f.block(8)})) == fourcc("fmt?"), "no converters");
    require(f.status(at, "_AudioQueueNewInput", {f.block(40), 0, 0, 0, 0, 0, f.block(8)}) == -66680, "no input device");
    require(f.status(au, "_AudioUnitReset", {0, 0, 0}) == static_cast<std::int32_t>(0x80008001U), "no audio units");
    // CoreMIDI
    // (ALCHEMY_MIDI_DEVICES=none: the Windows MIDI devices are not listed.)
    require(f.status(midi, "_MIDIClientCreate", {0, 0, 0, f.block(4)}) == 0, "MIDI client");
    require(f.call(midi, "_MIDIGetNumberOfSources") == 0 && f.call(midi, "_MIDIGetNumberOfDestinations") == 0, "no endpoints");
    const auto list = f.block(64);
    const auto first = f.call(midi, "_MIDIPacketListInit", {list});
    require(first == list + 4, "MIDIPacketListInit");
    const auto on = f.block(3), off = f.block(3);
    f.engine.write_bytes(on, std::vector<std::uint8_t>{0x90, 60, 100});
    f.engine.write_bytes(off, std::vector<std::uint8_t>{0x80, 60, 0});
    const auto packet = f.call(midi, "_MIDIPacketListAdd", {list, 64, first, 5, 3, on});
    require(packet == first && f.engine.read32(list) == 1, "first packet");
    require(f.call(midi, "_MIDIPacketListAdd", {list, 64, packet, 5, 3, off}) == packet && alchemy::gui::fill::read16(f.engine, packet + 8) == 6, "same time merges");
    const auto second = f.call(midi, "_MIDIPacketListAdd", {list, 64, packet, 9, 3, on});
    require(second == ((packet + 16 + 3) & ~Address{3}) && f.engine.read32(list) == 2 && f.engine.read64(second) == 9, "new packet, 4-byte aligned");
    require(f.call(midi, "_MIDIPacketListAdd", {list, 40, second, 12, 3, on}) == 0, "list full");
}

void accessibility(Fixture& f) {
    const auto behavior = f.block(8, 0xff);
    alchemy::CallRegs registers{};
    registers.x[0] = 0; registers.x[1] = behavior;
    require(f.regs(ma, "_MACaptionAppearanceGetForegroundOpacity", registers).d(0) == 1.0 && f.engine.read64(behavior) == 0, "foreground opacity");
    require(f.regs(ma, "_MACaptionAppearanceGetWindowOpacity", registers).d(0) == 0.0, "no window");
    require(f.call(ma, "_MACaptionAppearanceGetTextEdgeStyle", {0, behavior}) == 1, "no edge style");
    require(f.call(ma, "_MACaptionAppearanceCopyForegroundColor", {0, behavior}) != 0, "foreground color");
    require(f.call(ma, "_MACaptionAppearanceCopyFontDescriptorForStyle", {0, behavior, 0}) != 0, "font descriptor");
    require(f.string_constant(ic, "_ICOverwrite") == "overwrite", "ICOverwrite");
    require(f.cls("ICCameraDevice") != 0, "ImageCaptureCore classes");
}

} // namespace

int main() {
    // The host's MIDI and audio devices stay out of these tests (deterministic lists).
    _putenv_s("ALCHEMY_MIDI_DEVICES", "none");
    _putenv_s("ALCHEMY_AUDIO_DEVICES", "none");
    return run("media", {{"imports", imports}, {"core media", core_media}, {"video", video}, {"audio", audio}, {"accessibility", accessibility}});
}
