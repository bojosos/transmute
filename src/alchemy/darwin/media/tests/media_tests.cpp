// media provider tests: the Qt WebEngine media imports resolve; CoreMedia
// data types are real (CMTime, CMBlockBuffer, CMFormatDescription with H.264
// SPS parsing, CMSampleBuffer); codecs and capture devices are absent with the
// documented error codes; with the host's audio and MIDI devices hidden
// (ALCHEMY_AUDIO_DEVICES/ALCHEMY_MIDI_DEVICES=none) the device lists are empty;
// MIDI clients, packet lists and caption-appearance defaults are real.
#include "alchemy/gui/tests/fill_fixture.hpp"
#include "alchemy/darwin/media/media.hpp"

#include <array>
#include <chrono>
#include <thread>
#include <vector>
#include <cmath>
#include <span>

namespace {
using namespace fill_test;

#define FRAMEWORK(name) "/System/Library/Frameworks/" name ".framework/Versions/A/" name
constexpr std::string_view cm = FRAMEWORK("CoreMedia"), vt = FRAMEWORK("VideoToolbox"), av = FRAMEWORK("AVFoundation"),
    cv = FRAMEWORK("CoreVideo"), at = FRAMEWORK("AudioToolbox"), au = FRAMEWORK("AudioUnit"), ca = FRAMEWORK("CoreAudio"),
    midi = FRAMEWORK("CoreMIDI"), ma = FRAMEWORK("MediaAccessibility"), ic = FRAMEWORK("ImageCaptureCore"),
    gc = FRAMEWORK("GameController");
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
    // The output timing: as stored, then moved by an output presentation time (PTS 7/1 shown at 10/1).
    const auto output = f.block(72), needed = f.block(8);
    require(f.status(cm, "_CMSampleBufferGetOutputSampleTimingInfoArray", {sbuf, 1, output, needed}) == 0 && f.engine.read64(needed) == 1 &&
            f.engine.read64(output + 24) == 7, "output timing as stored");
    const auto shown = f.block(24);
    f.engine.write64(shown, 10); f.engine.write32(shown + 8, 1); f.engine.write32(shown + 12, 1);
    require(f.status(cm, "_CMSampleBufferSetOutputPresentationTimeStamp", {sbuf, shown}) == 0, "an output presentation time");
    require(f.status(cm, "_CMSampleBufferGetOutputSampleTimingInfoArray", {sbuf, 1, output, needed}) == 0 && f.engine.read64(output + 24) == 10 &&
            f.engine.read32(output + 32) == 1, "output timing at the output presentation time");
    require(f.status(cm, "_CMSampleBufferGetOutputSampleTimingInfoArray", {sbuf, 0, output, needed}) == -12737, "an array too small");
    require(f.string_constant(cm, "_kCMFormatDescriptionExtension_VerbatimSampleDescription") == "VerbatimSampleDescription", "VerbatimSampleDescription");
}

void video(Fixture& f) {
    // H.264 has no decoder (AV1, VP9 and VP8 do: media_guest_tests' video_toolbox).
    const auto out = f.block(8, 0xff), format_out = f.block(8);
    require(f.status(cm, "_CMVideoFormatDescriptionCreate", {0, fourcc("avc1"), 640, 480, 0, format_out}) == 0, "an H.264 description");
    require(f.status(vt, "_VTDecompressionSessionCreate", {0, f.engine.read64(format_out), 0, 0, 0, out}) == -12906 && f.engine.read64(out) == 0,
            "no H.264 decoder");
    // H.264 and HEVC encode where the host has an encoder (media_guest_tests' vt_compression); JPEG never does.
    alchemy::CallRegs compress{};
    compress.x[1] = 640; compress.x[2] = 480; compress.x[3] = fourcc("jpeg");
    const auto session = f.block(8, 0xff);
    compress.push_stack(Address{0});
    compress.push_stack(session);
    require(static_cast<std::int32_t>(f.regs(vt, "_VTCompressionSessionCreate", compress).x0) == -12908 && f.engine.read64(session) == 0, "no JPEG encoder");
    require(f.status(vt, "_VTPixelTransferSessionCreate", {0, out}) == 0 && f.engine.read64(out) != 0, "a pixel transfer session");
    require(f.status(vt, "_VTSessionSetProperty", {0, 0, 0}) == -12902, "session calls");
    require(f.call(vt, "_VTIsHardwareDecodeSupported", {fourcc("avc1")}) == 0, "no hardware decode");
    // CVImageBufferCreateColorSpaceFromAttachments: the CoreGraphics space of the primaries and transfer function.
    {
        const auto cg = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";
        const auto space_of = [&](const char* primaries, const char* transfer) -> std::string {
            const std::array<std::pair<Address, Address>, 2> entries{{{f.cf.string("CVImageBufferColorPrimaries"), f.cf.string(primaries)},
                                                                      {f.cf.string("CVImageBufferTransferFunction"), f.cf.string(transfer)}}};
            const auto space = f.call(cv, "_CVImageBufferCreateColorSpaceFromAttachments", {f.cf.dictionary(entries)});
            if (!space) return "";
            const auto name = f.call(cg, "_CGColorSpaceCopyName", {space});
            return name ? f.cf.utf8(name) : "(unnamed)";
        };
        require(space_of("ITU_R_709_2", "ITU_R_709_2") == "kCGColorSpaceITUR_709" && space_of("ITU_R_709_2", "IEC_sRGB") == "kCGColorSpaceSRGB" &&
                space_of("ITU_R_709_2", "Linear") == "kCGColorSpaceLinearSRGB" && space_of("P3_D65", "IEC_sRGB") == "kCGColorSpaceDisplayP3" &&
                space_of("ITU_R_2020", "ITU_R_2020") == "kCGColorSpaceITUR_2020", "the color spaces of BT.709, sRGB, Display P3 and BT.2020");
        require(space_of("ITU_R_2020", "SMPTE_ST_2084_PQ").empty() && space_of("SMPTE_C", "ITU_R_709_2").empty(), "none for PQ or SMPTE C");
        require(f.call(cv, "_CVImageBufferCreateColorSpaceFromAttachments", {0}) == 0, "none without attachments");
    }
    // AVFoundation
    require(f.string_constant(av, "_AVMediaTypeVideo") == "vide", "AVMediaTypeVideo");
    for (const auto* type : {"AVCaptureDeviceTypeExternal", "AVCaptureDeviceTypeContinuityCamera", "AVCaptureDeviceTypeMicrophone"})
        require(f.string_constant(av, std::string("_") + type) == type, std::string("macOS 14 capture device type ") + type);
    // Screen capture through AVFoundation is not available: no screen input is made.
    require(f.send(f.send(f.cls("AVCaptureScreenInput"), "alloc"), "initWithDisplayID:", {1}) == 0, "no AVCaptureScreenInput");
    const auto device = f.cls("AVCaptureDevice");
    require(f.cf.length(f.send(device, "devicesWithMediaType:", {f.engine.read64(f.symbol(av, "_AVMediaTypeVideo"))})) == 0, "no capture devices");
    require(f.send(device, "defaultDeviceWithMediaType:", {0}) == 0, "no default device");
    const auto error = f.block(8);
    require(f.send(f.cls("AVCaptureDeviceInput"), "deviceInputWithDevice:error:", {0, error}) == 0, "no input");
    require(f.call("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "_CFErrorGetCode", {f.engine.read64(error)}) ==
            static_cast<Address>(-11814), "AVErrorDeviceNotConnected");
    // CoreVideo
    require(f.status(cv, "_CVPixelBufferPoolCreate", {0, 0, 0, out}) == -6682 && f.engine.read64(out) == 0, "a pool needs its buffers' size");
    require(f.call(cv, "_CVPixelBufferGetWidth", {0}) == 0, "NULL width");
    require(f.status(cv, "_CVPixelBufferLockBaseAddress", {0, 0}) == -6661, "NULL lock");
}

// CMTimeRange, CMTimebase, CMSync, the queues and the sample-buffer timing and size getters.
void core_media_timing(Fixture& f) {
    const auto copy_bytes = [&](Address to, Address from, std::size_t n) {
        std::vector<std::uint8_t> bytes(n);
        f.engine.read_bytes(from, bytes);
        f.engine.write_bytes(to, bytes);
    };
    const auto time = [&](std::int64_t value, std::int32_t timescale) {
        const auto at = f.block(24);
        f.engine.write64(at, static_cast<std::uint64_t>(value));
        f.engine.write32(at + 8, static_cast<std::uint32_t>(timescale));
        f.engine.write32(at + 12, 1);
        return at;
    };
    const auto range = [&](std::int64_t start, std::int64_t duration, std::int32_t timescale) {
        const auto at = f.block(48);
        copy_bytes(at, time(start, timescale), 24);
        copy_bytes(at + 24, time(duration, timescale), 24);
        return at;
    };
    const auto seconds_of = [&](Address t) {
        alchemy::CallRegs r{};
        r.x[0] = t;
        return f.regs(cm, "_CMTimeGetSeconds", r).d(0);
    };
    const auto x8 = [&](const char* name, std::initializer_list<Address> arguments, std::size_t size = 24) {
        alchemy::CallRegs r{};
        std::size_t i = 0;
        for (const auto a : arguments) r.x[i++] = a;
        r.indirect_result = f.block(size);
        static_cast<void>(f.regs(cm, name, r));
        return r.indirect_result;
    };
    // Ranges: [1, 4) and [3, 7) at timescale 1.
    const auto a = range(1, 3, 1), b = range(3, 4, 1);
    const auto u = x8("_CMTimeRangeGetUnion", {a, b}, 48), i = x8("_CMTimeRangeGetIntersection", {a, b}, 48);
    require(seconds_of(u) == 1 && seconds_of(u + 24) == 6, "union [1, 7)");
    require(seconds_of(i) == 3 && seconds_of(i + 24) == 1, "intersection [3, 4)");
    require(f.call(cm, "_CMTimeRangeContainsTime", {a, time(1, 1)}) == 1 && f.call(cm, "_CMTimeRangeContainsTime", {a, time(4, 1)}) == 0, "ContainsTime (end excluded)");
    require(f.call(cm, "_CMTimeRangeContainsTimeRange", {u, b}) == 1 && f.call(cm, "_CMTimeRangeContainsTimeRange", {a, b}) == 0, "ContainsTimeRange");
    require(seconds_of(x8("_CMTimeClampToRange", {time(9, 1), a})) == 4, "clamped to the end");
    require(seconds_of(x8("_CMTimeMapTimeFromRangeToRange", {time(2, 1), a, range(10, 6, 1)})) == 12, "mapped: 10 + (2 - 1) * 6 / 3");
    require(seconds_of(x8("_CMTimeMultiplyByRatio", {time(10, 1), 3, 4})) == 7.5 || seconds_of(x8("_CMTimeMultiplyByRatio", {time(10, 1), 3, 4})) == 8, "ratio");
    require(seconds_of(x8("_CMTimeMaximum", {time(1, 2), time(2, 3)})) == 2.0 / 3, "maximum");
    const auto dictionary = f.call(cm, "_CMTimeCopyAsDictionary", {time(5, 7), 0});
    require(seconds_of(x8("_CMTimeMakeFromDictionary", {dictionary})) == 5.0 / 7, "dictionary round trip");
    // A timebase on the host clock: stopped, then running at 2x from 10 s.
    const auto out = f.block(8);
    const auto clock = f.call(cm, "_CMClockGetHostTimeClock");
    require(clock && f.status(cm, "_CMTimebaseCreateWithSourceClock", {0, clock, out}) == 0, "CMTimebaseCreateWithSourceClock");
    const auto timebase = f.engine.read64(out);
    require(f.status(cm, "_CMTimebaseSetTime", {timebase, time(10, 1)}) == 0 && seconds_of(x8("_CMTimebaseGetTime", {timebase})) == 10, "stopped at 10 s");
    alchemy::CallRegs rate{};
    rate.x[0] = timebase;
    rate.set_d(0, 2.0);
    require(static_cast<std::int32_t>(f.regs(cm, "_CMTimebaseSetRate", rate).x0) == 0, "SetRate 2");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto later = seconds_of(x8("_CMTimebaseGetTime", {timebase}));
    require(later > 10.15 && later < 11.5, "running at 2x: " + std::to_string(later));
    alchemy::CallRegs get_rate{};
    get_rate.x[0] = timebase;
    require(f.regs(cm, "_CMTimebaseGetRate", get_rate).d(0) == 2.0 && f.regs(cm, "_CMTimebaseGetEffectiveRate", get_rate).d(0) == 2.0, "rates");
    alchemy::CallRegs relative{};
    relative.x[0] = timebase;
    relative.x[1] = clock;
    require(f.regs(cm, "_CMSyncGetRelativeRate", relative).d(0) == 2.0, "CMSyncGetRelativeRate");
    require(f.call(cm, "_CMTimebaseCopySourceClock", {timebase}) == clock, "the source clock");
    // CMSimpleQueue: capacity 2.
    require(f.status(cm, "_CMSimpleQueueCreate", {0, 2, out}) == 0, "CMSimpleQueueCreate");
    const auto simple = f.engine.read64(out);
    require(f.status(cm, "_CMSimpleQueueEnqueue", {simple, 11}) == 0 && f.status(cm, "_CMSimpleQueueEnqueue", {simple, 22}) == 0 &&
                f.status(cm, "_CMSimpleQueueEnqueue", {simple, 33}) == -12773,
            "full at its capacity");
    require(f.call(cm, "_CMSimpleQueueDequeue", {simple}) == 11 && f.call(cm, "_CMSimpleQueueGetCount", {simple}) == 1, "FIFO");
    // Sample buffers: three samples, one timing entry (1/30 s from 2 s), one size (100 bytes).
    const auto timing = f.block(72);
    copy_bytes(timing, time(1, 30), 24);
    copy_bytes(timing + 24, time(60, 30), 24);
    const auto sizes = f.block(8);
    f.engine.write64(sizes, 100);
    const auto make_sample = [&](std::int64_t pts) {
        copy_bytes(timing + 24, time(pts, 30), 24);
        alchemy::CallRegs r{};
        r.x = {0, 0, 0, 3, 1, timing, 1, sizes};
        const auto sample_out = f.block(8);
        r.push_stack(sample_out);
        require(static_cast<std::int32_t>(f.regs(cm, "_CMSampleBufferCreateReady", r).x0) == 0, "CMSampleBufferCreateReady");
        return f.engine.read64(sample_out);
    };
    const auto sample = make_sample(60);
    require(f.call(cm, "_CMSampleBufferGetTotalSampleSize", {sample}) == 300 && f.call(cm, "_CMSampleBufferGetSampleSize", {sample, 2}) == 100, "sizes");
    const auto info = f.block(72);
    require(f.status(cm, "_CMSampleBufferGetSampleTimingInfo", {sample, 2, info}) == 0 && seconds_of(info + 24) == 62.0 / 30, "sample 2's time");
    require(seconds_of(x8("_CMSampleBufferGetOutputDuration", {sample})) == 0.1, "output duration");
    require(f.call(cm, "_CMSampleBufferIsValid", {sample}) == 1 && f.status(cm, "_CMSampleBufferInvalidate", {sample}) == 0 &&
                f.call(cm, "_CMSampleBufferIsValid", {sample}) == 0,
            "invalidated");
    // CMBufferQueue sorted by output presentation time.
    alchemy::CallRegs callbacks{};
    const auto sorted = f.regs(cm, "_CMBufferQueueGetCallbacksForSampleBuffersSortedByOutputPTS", callbacks).x0;
    require(sorted && f.status(cm, "_CMBufferQueueCreate", {0, 0, sorted, out}) == 0, "CMBufferQueueCreate");
    const auto queue = f.engine.read64(out);
    const auto late = make_sample(90), early = make_sample(30);
    require(f.status(cm, "_CMBufferQueueEnqueue", {queue, late}) == 0 && f.status(cm, "_CMBufferQueueEnqueue", {queue, early}) == 0, "enqueued");
    require(f.call(cm, "_CMBufferQueueGetBufferCount", {queue}) == 2 && f.call(cm, "_CMBufferQueueGetHead", {queue}) == early, "sorted by time");
    require(seconds_of(x8("_CMBufferQueueGetDuration", {queue})) == 0.2, "queued duration");
    require(f.call(cm, "_CMBufferQueueDequeueAndRetain", {queue}) == early && f.status(cm, "_CMBufferQueueMarkEndOfData", {queue}) == 0 &&
                f.status(cm, "_CMBufferQueueEnqueue", {queue, early}) == -12763,
            "end of data");
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

// The CMAttachmentBearer calls on a sample buffer and a CVPixelBuffer; the VideoToolbox
// quality-metric keys; CoreAudio's ducking and process taps, which answer as unsupported;
// CATapDescription and AVCapturePhotoSettings as value classes.
void bearers_and_taps(Fixture& f) {
    const auto sample_out = f.block(8);
    alchemy::CallRegs make{};
    make.push_stack(sample_out);
    require(f.regs(cm, "_CMSampleBufferCreateReady", make).x0 == 0, "an empty sample buffer");
    const auto sample = f.engine.read64(sample_out);
    const auto key = f.cf.string("AlchemyKey"), other = f.cf.string("AlchemyOther");
    const auto value = f.cf.string("value"), mode_out = f.block(4);
    f.call(cm, "_CMSetAttachment", {sample, key, value, 1});
    f.call(cm, "_CMSetAttachment", {sample, other, value, 0});
    require(f.call(cm, "_CMGetAttachment", {sample, key, mode_out}) == value && f.engine.read32(mode_out) == 1, "CMGetAttachment and its mode");
    require(f.cf.dictionary_entries(f.call(cm, "_CMCopyDictionaryOfAttachments", {0, sample, 1})).size() == 1 &&
            f.cf.dictionary_entries(f.call(cm, "_CMCopyDictionaryOfAttachments", {0, sample, 0})).size() == 1, "one attachment of each mode");
    // Only the propagating one reaches a pixel buffer, through CoreVideo.
    const auto image_out = f.block(8);
    require(f.status(cv, "_CVPixelBufferCreate", {0, 16, 16, fourcc("BGRA"), 0, image_out}) == 0, "a pixel buffer");
    const auto image = f.engine.read64(image_out);
    f.call(cm, "_CMPropagateAttachments", {sample, image});
    require(f.call(cv, "_CVBufferGetAttachment", {image, key, 0}) == value && f.call(cm, "_CMGetAttachment", {image, other, 0}) == 0,
            "CMPropagateAttachments copies the propagating attachments");
    f.call(cm, "_CMRemoveAttachment", {image, key});
    require(f.call(cm, "_CMGetAttachment", {image, key, 0}) == 0, "CMRemoveAttachment on a CVBuffer");
    f.call(cm, "_CMRemoveAllAttachments", {sample});
    require(f.cf.dictionary_entries(f.call(cm, "_CMCopyDictionaryOfAttachments", {0, sample, 1})).empty(), "CMRemoveAllAttachments");

    require(f.string_constant(vt, "_kVTSampleAttachmentKey_QualityMetrics") == "QualityMetrics" &&
            f.string_constant(vt, "_kVTSampleAttachmentQualityMetricsKey_LumaMeanSquaredError") == "LumaMeanSquaredError", "quality-metric keys");

    const auto tap_id = f.block(4);
    f.engine.write32(tap_id, 77);
    require(static_cast<std::uint32_t>(f.status(ca, "_AudioDeviceDuck", {1})) == fourcc("unop"), "no ducking");
    require(static_cast<std::uint32_t>(f.status(ca, "_AudioHardwareCreateProcessTap", {0, tap_id})) == fourcc("unop") && f.engine.read32(tap_id) == 0,
            "no process tap");
    require(static_cast<std::uint32_t>(f.status(ca, "_AudioHardwareDestroyProcessTap", {1})) == fourcc("!obj"), "no tap to destroy");
    const std::array<std::uint64_t, 1> pids{f.cf.integer(42)};
    const auto processes = f.cf.array(pids);
    const auto tap = f.send(f.send(f.cls("CATapDescription"), "alloc"), "initMonoGlobalTapButExcludeProcesses:", {processes});
    require(tap && f.send(tap, "processes") == processes && (f.send(tap, "isMono") & 0xff) && (f.send(tap, "isExclusive") & 0xff) &&
            (f.send(tap, "isMixdown") & 0xff) && f.send(tap, "UUID") != 0, "a mono global tap description");
    const auto device_tap = f.send(f.send(f.cls("CATapDescription"), "alloc"), "initWithProcesses:andDeviceUID:withStream:", {processes, f.cf.string("dev"), 2});
    require(f.cf.number_value(f.send(device_tap, "stream")) == 2.0 && f.cf.utf8(f.send(device_tap, "deviceUID")) == "dev" &&
            !(f.send(device_tap, "isMixdown") & 0xff) && f.send(device_tap, "UUID") != f.send(tap, "UUID"), "a device stream tap description");

    const auto first = f.send(f.cls("AVCapturePhotoSettings"), "photoSettings");
    const auto format = f.cf.dictionary(std::span<const std::pair<std::uint64_t, std::uint64_t>>{});
    const auto second = f.send(f.cls("AVCapturePhotoSettings"), "photoSettingsWithFormat:", {format});
    require(first && second && f.send(first, "uniqueID") != f.send(second, "uniqueID") && f.send(second, "format") == format &&
            f.send(first, "photoQualityPrioritization") == 2, "photo settings");
    const auto copy = f.send(f.cls("AVCapturePhotoSettings"), "photoSettingsFromPhotoSettings:", {second});
    require(f.send(copy, "format") == format && f.send(copy, "uniqueID") != f.send(second, "uniqueID"), "photo settings copy");
    if (const auto output_class = f.cls("AVCapturePhotoOutput"))
        require(f.cf.length(f.send(f.send(f.send(output_class, "alloc"), "init"), "availablePhotoCodecTypes")) == 0, "an unconnected photo output");
}

// GameController as Godot binds it: the input element names, GCColor, button and axis elements an app sets, and
// no controllers.
void game_controller(Fixture& f) {
    for (const char* name : {"ButtonA", "ButtonB", "ButtonX", "ButtonY", "ButtonMenu", "ButtonOptions", "DirectionPad", "DualShockTouchpadButton",
                             "DualShockTouchpadOne", "DualShockTouchpadTwo", "LeftShoulder", "LeftThumbstick", "LeftThumbstickButton", "LeftTrigger",
                             "RightShoulder", "RightThumbstick", "RightThumbstickButton", "RightTrigger", "XboxPaddleOne", "XboxPaddleTwo",
                             "XboxPaddleThree", "XboxPaddleFour"})
        require(f.string_constant(gc, std::string("_GCInput") + name) == name, std::string("GCInput") + name);
    require(f.string_constant(gc, "_GCHapticsLocalityLeftHandle") == "LeftHandle" &&
            f.string_constant(gc, "_GCControllerDidConnectNotification") == "GCControllerDidConnectNotification", "haptics and notification names");
    alchemy::CallRegs rgb{};
    rgb.set_s(0, 1.0f);
    rgb.set_s(1, 0.5f);
    rgb.set_s(2, 0.25f);
    const auto color = f.objc.send(f.send(f.cls("GCColor"), "alloc"), f.objc.selector("initWithRed:green:blue:"), rgb).x0;
    require(color && f.objc.send(color, f.objc.selector("red"), {}).s(0) == 1.0f && f.objc.send(color, f.objc.selector("green"), {}).s(0) == 0.5f &&
            f.objc.send(color, f.objc.selector("blue"), {}).s(0) == 0.25f && f.send(color, "copy") == color, "GCColor");
    const auto button = f.send(f.send(f.cls("GCControllerButtonInput"), "alloc"), "init");
    require((f.send(button, "isPressed") & 0xff) == 0, "a button at rest");
    alchemy::CallRegs value{};
    value.set_s(0, 0.75f);
    static_cast<void>(f.objc.send(button, f.objc.selector("setValue:"), value));
    require((f.send(button, "isPressed") & 0xff) == 1 && f.objc.send(button, f.objc.selector("value"), {}).s(0) == 0.75f && f.send(button, "collection") == 0,
            "a pressed button keeps its value");
    const auto axis = f.send(f.send(f.cls("GCControllerAxisInput"), "alloc"), "init");
    value.set_s(0, -0.5f);
    static_cast<void>(f.objc.send(axis, f.objc.selector("setValue:"), value));
    require(f.objc.send(axis, f.objc.selector("value"), {}).s(0) == -0.5f && f.send(axis, "valueChangedHandler") == 0, "an axis keeps its value");
    require(f.cf.array_values(f.send(f.cls("GCController"), "controllers")).empty() && f.send(f.cls("GCController"), "current") == 0, "no controllers");
}

} // namespace

int main() {
    // The host's MIDI and audio devices stay out of these tests (deterministic lists).
    _putenv_s("ALCHEMY_MIDI_DEVICES", "none");
    _putenv_s("ALCHEMY_AUDIO_DEVICES", "none");
    _putenv_s("ALCHEMY_CAMERA", "none");
    return run("media", {{"imports", imports}, {"core media", core_media}, {"core media timing", core_media_timing}, {"video", video}, {"audio", audio}, {"accessibility", accessibility},
                         {"bearers and taps", bearers_and_taps}, {"game controller", game_controller}});
}
