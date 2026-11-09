// QLab media group (G5) exports through the strict-mode provider fixture: the
// guest calling convention of the CoreMedia CMTime/sample-buffer additions,
// CoreVideo, the Accelerate kernels, CoreMIDI over a fake device backend,
// the CoreAudio HAL over a fake device backend, AudioToolbox (MusicSequence,
// AudioFile/ExtAudioFile, AU listeners) and the AVFoundation classes.
#include "alchemy/gui/tests/fill_fixture.hpp"
#include "alchemy/darwin/media/media_support.hpp"
#include "alchemy/darwin/media/audio_file_core.hpp"
#include "alchemy/darwin/media/midi_core.hpp"
#include "alchemy/darwin/media/hal_core.hpp"

#include "alchemy/path.hpp"
#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/host/crypto.hpp"
#include "alchemy/darwin/media/media_container.hpp"
#include "alchemy/darwin/media/media_codecs.hpp"
#include "fake_aac.hpp"
#include "fake_video.hpp"
#include "alchemy/darwin/media/video_config.hpp"
#include "alchemy/host/media_codec.hpp"
#include "alchemy/host/speech.hpp"

#include <algorithm>
#include <functional>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <span>
#include <tuple>
#include <sstream>
#include <map>
#include <mutex>

namespace {
using namespace fill_test;

#define FRAMEWORK(name) "/System/Library/Frameworks/" name ".framework/Versions/A/" name
constexpr std::string_view cm = FRAMEWORK("CoreMedia"), cv = FRAMEWORK("CoreVideo"), midi_lib = FRAMEWORK("CoreMIDI"),
    acc = FRAMEWORK("Accelerate"), vt = FRAMEWORK("VideoToolbox"), av = FRAMEWORK("AVFoundation"),
    ca = FRAMEWORK("CoreAudio"), at = FRAMEWORK("AudioToolbox"), au = FRAMEWORK("AudioUnit");
#undef FRAMEWORK

namespace midi = alchemy::gui::completion_kit::media::midi;

// The output stream of the fake device: the test plays it by calling FakeStream::play.
struct FakeStream final : alchemy::gui::completion_kit::media::hal::OutputStream {
    explicit FakeStream(alchemy::gui::completion_kit::media::hal::Render render) : render(std::move(render)) {}
    ~FakeStream() override {
        std::lock_guard lock(mutex());
        if (current() == this) current() = nullptr;
    }
    bool start() override { running = true; return true; }
    void stop() override { running = false; }
    alchemy::gui::completion_kit::media::hal::Render render;
    std::atomic<bool> running{};
    static std::mutex& mutex() { static std::mutex value; return value; }
    static FakeStream*& current() { static FakeStream* value{}; return value; }
    // Takes `frames` stereo frames from the device, once the device is running.
    static std::vector<float> play(std::uint32_t frames) {
        std::lock_guard lock(mutex());
        std::vector<float> out(static_cast<std::size_t>(frames) * 2);
        if (current() && current()->running) current()->render(out.data(), frames, 0);
        return out;
    }
    // Takes `frames` once the device's I/O thread has had time to render ahead (play itself is
    // instant: back to back, it would drain the FIFO faster than the I/O thread fills it).
    static std::vector<float> play_paced(std::uint32_t frames) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        return play(frames);
    }
    // Plays until a frame is not silent (or 3 s pass); returns that buffer.
    static std::vector<float> play_until_sound(std::uint32_t frames) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        for (;;) {
            auto out = play(frames);
            if (std::any_of(out.begin(), out.end(), [](float v) { return v != 0; }) || std::chrono::steady_clock::now() > deadline) return out;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
};

struct FakeHal final : alchemy::gui::completion_kit::media::hal::Backend {
    alchemy::gui::completion_kit::media::hal::Snapshot snapshot() override {
        alchemy::gui::completion_kit::media::hal::Snapshot result;
        result.devices.push_back({"{render}", "Line Out", "", false, 2, 48000, 3, 2});
        result.default_output = "{render}";
        return result;
    }
    std::unique_ptr<alchemy::gui::completion_kit::media::hal::OutputStream> open_output(
        const alchemy::gui::completion_kit::media::hal::Device&, alchemy::gui::completion_kit::media::hal::Render render) override {
        auto stream = std::make_unique<FakeStream>(std::move(render));
        std::lock_guard lock(FakeStream::mutex());
        FakeStream::current() = stream.get();
        return stream;
    }
};

struct FakeMidi final : midi::Backend {
    std::vector<midi::PortInfo> inputs() override { return {{"Port A", "Maker", "drv", false}}; }
    std::vector<midi::PortInfo> outputs() override { return {{"Port A", "Maker", "drv", false}}; }
    bool open_input(std::size_t, Input) override { return true; }
    void close_input(std::size_t) override {}
    bool send(std::size_t, std::span<const std::uint8_t> message) override {
        std::lock_guard lock(mutex());
        sent().emplace_back(message.begin(), message.end());
        return true;
    }
    static std::mutex& mutex() { static std::mutex value; return value; }
    static std::vector<std::vector<std::uint8_t>>& sent() { static std::vector<std::vector<std::uint8_t>> value; return value; }
};

void write_time(Fixture& f, Address at, std::int64_t value, std::int32_t timescale, std::uint32_t flags = 1) {
    f.engine.write64(at, static_cast<std::uint64_t>(value));
    f.engine.write32(at + 8, static_cast<std::uint32_t>(timescale));
    f.engine.write32(at + 12, flags);
    f.engine.write64(at + 16, 0);
}

void core_media(Fixture& f) {
    const auto a = f.block(24), b = f.block(24), out = f.block(48);
    write_time(f, a, 1, 3);
    write_time(f, b, 1, 2);
    alchemy::CallRegs r{};
    r.x[0] = a; r.x[1] = b; r.indirect_result = out;
    static_cast<void>(f.regs(cm, "_CMTimeAdd", r));
    require(f.engine.read64(out) == 5 && f.engine.read32(out + 8) == 6 && f.engine.read32(out + 12) == 1, "CMTimeAdd");
    require(static_cast<std::int32_t>(f.regs(cm, "_CMTimeCompare", r).x0) == -1, "CMTimeCompare");
    alchemy::CallRegs seconds{};
    seconds.set_d(0, 2.5);
    seconds.x[0] = 600;
    seconds.indirect_result = out;
    static_cast<void>(f.regs(cm, "_CMTimeMakeWithSeconds", seconds));
    require(f.engine.read64(out) == 1500 && f.engine.read32(out + 8) == 600, "CMTimeMakeWithSeconds");
    alchemy::CallRegs scaled{};
    scaled.x[0] = a;
    scaled.set_d(0, 3.0);
    scaled.indirect_result = out;
    static_cast<void>(f.regs(cm, "_CMTimeMultiplyByFloat64", scaled));
    require(f.engine.read64(out) == 3 && f.engine.read32(out + 8) == 3, "CMTimeMultiplyByFloat64");
    const auto zero = f.symbol(cm, "_kCMTimeZero"), infinity = f.symbol(cm, "_kCMTimePositiveInfinity");
    require(f.engine.read32(zero + 8) == 1 && f.engine.read32(zero + 12) == 1 && f.engine.read32(infinity + 12) == 5, "time constants");
    const auto description = f.call(cm, "_CMTimeCopyDescription", {0, b});
    require(f.cf.utf8(description) == "{1/2 = 0.500}", "CMTimeCopyDescription");
    alchemy::CallRegs range{};
    range.x[0] = a; range.x[1] = b; range.indirect_result = out;
    static_cast<void>(f.regs(cm, "_CMTimeRangeMake", range));
    const auto end = f.block(24);
    alchemy::CallRegs get_end{};
    get_end.x[0] = out; get_end.indirect_result = end;
    static_cast<void>(f.regs(cm, "_CMTimeRangeGetEnd", get_end));
    require(f.engine.read64(end) == 5 && f.engine.read32(end + 8) == 6, "CMTimeRangeGetEnd");

    // An interleaved stereo float audio description and a 4-frame sample buffer.
    const auto asbd = f.block(40);
    f.engine.write64(asbd, std::bit_cast<std::uint64_t>(48000.0));
    f.engine.write32(asbd + 8, 0x6c70636d);   // 'lpcm'
    f.engine.write32(asbd + 12, 9);           // float | packed
    f.engine.write32(asbd + 16, 8); f.engine.write32(asbd + 20, 1); f.engine.write32(asbd + 24, 8);
    f.engine.write32(asbd + 28, 2); f.engine.write32(asbd + 32, 32);
    const auto format_out = f.block(8);
    require(f.status(cm, "_CMAudioFormatDescriptionCreate", {0, asbd, 0, 0, 0, 0, 0, format_out}) == 0, "CMAudioFormatDescriptionCreate");
    const auto format = f.engine.read64(format_out);
    const auto stream = f.call(cm, "_CMAudioFormatDescriptionGetStreamBasicDescription", {format});
    require(stream && f.engine.read32(stream + 28) == 2, "stream description");
    const auto block_out = f.block(8);
    alchemy::CallRegs block{};
    block.x[2] = 32; block.x[6] = 32; block.x[7] = 1;
    block.push_stack(block_out);
    require(f.regs(cm, "_CMBlockBufferCreateWithMemoryBlock", block).x0 == 0, "block buffer");
    const auto timing = f.block(72);
    write_time(f, timing, 1, 48000);
    write_time(f, timing + 24, 0, 48000);
    write_time(f, timing + 48, 0, 0, 0);
    const auto sample_out = f.block(8);
    alchemy::CallRegs create{};
    create.x[1] = f.engine.read64(block_out); create.x[2] = format; create.x[3] = 4; create.x[4] = 1; create.x[5] = timing;
    create.push_stack(sample_out);
    require(f.regs(cm, "_CMSampleBufferCreateReady", create).x0 == 0, "sample buffer");
    const auto sample = f.engine.read64(sample_out);
    require(f.call(cm, "_CMSampleBufferGetNumSamples", {sample}) == 4 && f.call(cm, "_CMSampleBufferIsValid", {sample}) == 1, "num samples");
    alchemy::CallRegs duration{};
    duration.x[0] = sample; duration.indirect_result = out;
    static_cast<void>(f.regs(cm, "_CMSampleBufferGetDuration", duration));
    require(f.engine.read64(out) == 4 && f.engine.read32(out + 8) == 48000, "sample buffer duration");
    const auto needed = f.block(8), list = f.block(64), retained = f.block(8);
    require(f.status(cm, "_CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer", {sample, needed, list, 64, 0, 0, 0, retained}) == 0, "audio buffer list");
    require(f.engine.read64(needed) == 24 && f.engine.read32(list) == 1 && f.engine.read32(list + 8) == 2 && f.engine.read32(list + 12) == 32 &&
            f.engine.read64(retained) == f.engine.read64(block_out), "interleaved list");
}

void core_video(Fixture& f) {
    require(f.call(cv, "_CVGetCurrentHostTime") > 0, "host time");
    alchemy::CallRegs none{};
    require(f.regs(cv, "_CVGetHostClockFrequency", none).d(0) == 1e9, "host clock frequency");
    const auto key = f.engine.read64(f.symbol(cv, "_kCVImageBufferPixelAspectRatioKey"));
    require(f.cf.utf8(key) == "CVPixelAspectRatio", "pixel aspect ratio key");
    require(f.call(cv, "_CVBufferRetain", {0}) == 0, "retain NULL");
    const auto out = f.block(8);
    require(f.status(cv, "_CVMetalTextureCacheCreate", {0, 0, 0, 0, out}) == -6661, "texture cache needs a device");
    // A 32BGRA buffer in malloc'd memory: CF type, geometry, attachments.
    require(f.status(cv, "_CVPixelBufferCreate", {0, 30, 20, 0x42475241, 0, out}) == 0, "CVPixelBufferCreate");
    const auto bgra = f.engine.read64(out);
    const auto cf_library = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
    require(f.call(cf_library, "_CFGetTypeID", {bgra}) == f.call(cv, "_CVPixelBufferGetTypeID"), "a CVPixelBuffer CF type");
    require(f.call(cv, "_CVPixelBufferGetWidth", {bgra}) == 30 && f.call(cv, "_CVPixelBufferGetHeight", {bgra}) == 20 &&
            f.call(cv, "_CVPixelBufferGetBytesPerRow", {bgra}) % 64 == 0 && f.call(cv, "_CVPixelBufferGetBytesPerRow", {bgra}) >= 120 &&
            f.call(cv, "_CVPixelBufferGetBaseAddress", {bgra}) != 0 && f.call(cv, "_CVPixelBufferIsPlanar", {bgra}) == 0, "geometry");
    require(f.status(cv, "_CVPixelBufferLockBaseAddress", {bgra, 0}) == 0 && f.status(cv, "_CVPixelBufferUnlockBaseAddress", {bgra, 0}) == 0, "lock");
    const auto attachment_key = f.cf.string("CVImageBufferYCbCrMatrix"), attachment_value = f.cf.string("ITU_R_709_2");
    f.call(cv, "_CVBufferSetAttachment", {bgra, attachment_key, attachment_value, 1});
    const auto mode = f.block(4);
    require(f.call(cv, "_CVBufferGetAttachment", {bgra, f.cf.string("CVImageBufferYCbCrMatrix"), mode}) == attachment_value && f.engine.read32(mode) == 1,
            "attachment");
    // A planar 420v buffer backed by an IOSurface.
    const auto iosurface_key = f.cf.string("IOSurfaceProperties"), empty = f.cf.dictionary({});
    const std::array<std::pair<Address, Address>, 1> surface_attributes{{{iosurface_key, empty}}};
    require(f.status(cv, "_CVPixelBufferCreate", {0, 64, 48, 0x34323076, f.cf.dictionary(surface_attributes), out}) == 0, "420v");
    const auto nv12 = f.engine.read64(out);
    require(f.call(cv, "_CVPixelBufferGetIOSurface", {nv12}) != 0 && f.call(cv, "_CVPixelBufferGetPlaneCount", {nv12}) == 2 &&
            f.call(cv, "_CVPixelBufferGetWidthOfPlane", {nv12, 1}) == 32 && f.call(cv, "_CVPixelBufferGetHeightOfPlane", {nv12, 1}) == 24 &&
            f.call(cv, "_CVPixelBufferGetBaseAddressOfPlane", {nv12, 1}) > f.call(cv, "_CVPixelBufferGetBaseAddressOfPlane", {nv12, 0}),
            "420v planes on an IOSurface");
    f.call(cv, "_CVBufferPropagateAttachments", {bgra, nv12});
    require(f.call(cv, "_CVBufferGetAttachment", {nv12, attachment_key, 0}) == attachment_value, "propagated attachment");
    // Wrapped bytes: the release callback runs when the buffer goes.
    static bool released = false;
    const auto callback = f.process.install_function("_alchemy_test_pixels_released", [](transmute::CpuState& s, transmute::GuestMemory&) {
        released = s.x[0] == 0x99;
    }, "/usr/lib/alchemy_test_io.dylib");
    const auto pixels = f.block(16 * 16 * 4);
    alchemy::CallRegs bytes{};
    bytes.x[1] = 16; bytes.x[2] = 16; bytes.x[3] = 0x42475241; bytes.x[4] = pixels; bytes.x[5] = 64; bytes.x[6] = callback; bytes.x[7] = 0x99;
    bytes.push_stack(Address{0});
    bytes.push_stack(out);
    require(static_cast<std::int32_t>(f.regs(cv, "_CVPixelBufferCreateWithBytes", bytes).x0) == 0 &&
            f.call(cv, "_CVPixelBufferGetBaseAddress", {f.engine.read64(out)}) == pixels, "wrapped bytes");
    f.call(cv, "_CVPixelBufferRelease", {f.engine.read64(out)});
    require(released, "the release callback ran");
    require(f.status(cv, "_CVPixelBufferCreate", {0, 16, 16, 0x12345678, 0, out}) == -6680, "unknown pixel format");
    // A pool: buffers of its attributes, with an allocation threshold.
    const std::array<std::pair<Address, Address>, 3> pool_buffers{{
        {f.cf.string("Width"), f.cf.integer(8)}, {f.cf.string("Height"), f.cf.integer(8)}, {f.cf.string("PixelFormatType"), f.cf.integer(0x42475241)}}};
    require(f.status(cv, "_CVPixelBufferPoolCreate", {0, 0, f.cf.dictionary(pool_buffers), out}) == 0, "CVPixelBufferPoolCreate");
    const auto pool = f.engine.read64(out);
    const std::array<std::pair<Address, Address>, 1> aux_entries{{{f.cf.string("AllocationThreshold"), f.cf.integer(1)}}};
    const auto aux = f.cf.dictionary(aux_entries);
    require(f.status(cv, "_CVPixelBufferPoolCreatePixelBufferWithAuxAttributes", {0, pool, aux, out}) == 0 &&
            f.call(cv, "_CVPixelBufferGetWidth", {f.engine.read64(out)}) == 8, "a pool buffer");
    const auto first = f.engine.read64(out);
    require(f.status(cv, "_CVPixelBufferPoolCreatePixelBufferWithAuxAttributes", {0, pool, aux, out}) == -6689, "allocation threshold");
    f.call(cv, "_CVPixelBufferRelease", {first});
    require(f.status(cv, "_CVPixelBufferPoolCreatePixelBufferWithAuxAttributes", {0, pool, aux, out}) == 0, "below the threshold again");
    // VTPixelTransferSession: an orange 32BGRA buffer through 420v and back at half size.
    const auto rows = f.call(cv, "_CVPixelBufferGetBytesPerRow", {bgra}), base = f.call(cv, "_CVPixelBufferGetBaseAddress", {bgra});
    for (Address y = 0; y < 20; ++y)
        for (Address x = 0; x < 30; ++x) f.engine.write32(base + y * rows + x * 4, 0xFF0080FF);   // bytes FF 80 00 FF: B G R A
    const auto session_out = f.block(8);
    require(f.status(vt, "_VTPixelTransferSessionCreate", {0, session_out}) == 0, "VTPixelTransferSessionCreate");
    const auto session = f.engine.read64(session_out);
    require(f.status(cv, "_CVPixelBufferCreate", {0, 30, 20, 0x34323076, 0, out}) == 0, "a 420v destination");
    const auto yuv = f.engine.read64(out);
    require(f.status(vt, "_VTPixelTransferSessionTransferImage", {session, bgra, yuv}) == 0, "BGRA to 420v");
    require(f.status(cv, "_CVPixelBufferCreate", {0, 15, 10, 0x42475241, 0, out}) == 0, "a small BGRA destination");
    const auto small = f.engine.read64(out);
    require(f.status(vt, "_VTPixelTransferSessionTransferImage", {session, yuv, small}) == 0, "420v to BGRA, scaled");
    const auto pixel = f.engine.read32(f.call(cv, "_CVPixelBufferGetBaseAddress", {small}) + f.call(cv, "_CVPixelBufferGetBytesPerRow", {small}) * 5 + 4 * 7);
    const auto near = [](std::uint32_t value, int want) { return std::abs(static_cast<int>(value & 0xFF) - want) <= 3; };
    require(near(pixel, 0xFF) && near(pixel >> 8, 0x80) && near(pixel >> 16, 0x00) && (pixel >> 24) == 0xFF,
            "the color survives a Y'CbCr round trip: " + std::to_string(pixel));
    require(f.status(vt, "_VTPixelTransferSessionTransferImage", {session, bgra, 0}) == -12902, "no destination");
    // VTPixelRotationSession: the orange picture with a green top-left pixel, turned clockwise into 20x30.
    f.engine.write32(base, 0xFF00FF00);
    const auto close = [](std::uint32_t a, std::uint32_t b) {
        for (int i = 0; i < 32; i += 8)
            if (std::abs(static_cast<int>((a >> i) & 0xFF) - static_cast<int>((b >> i) & 0xFF)) > 1) return false;
        return true;
    };
    require(f.status(vt, "_VTPixelRotationSessionCreate", {0, session_out}) == 0, "VTPixelRotationSessionCreate");
    const auto rotation = f.engine.read64(session_out);
    require(f.call(cf_library, "_CFGetTypeID", {rotation}) == f.call(vt, "_VTPixelRotationSessionGetTypeID"), "a rotation session type");
    require(f.status(vt, "_VTSessionSetProperty", {rotation, f.engine.read64(f.symbol(vt, "_kVTPixelRotationPropertyKey_Rotation")),
                                                  f.engine.read64(f.symbol(vt, "_kVTRotation_CW90"))}) == 0, "rotate clockwise");
    require(f.status(cv, "_CVPixelBufferCreate", {0, 20, 30, 0x42475241, 0, out}) == 0, "a 20x30 destination");
    const auto turned = f.engine.read64(out);
    require(f.status(vt, "_VTPixelRotationSessionRotateImage", {rotation, bgra, small}) == -12902, "the destination must have the rotated size");
    require(f.status(vt, "_VTPixelRotationSessionRotateImage", {rotation, bgra, turned}) == 0, "rotated");
    const auto turned_rows = f.call(cv, "_CVPixelBufferGetBytesPerRow", {turned}), turned_base = f.call(cv, "_CVPixelBufferGetBaseAddress", {turned});
    require(close(f.engine.read32(turned_base + 19 * 4), 0xFF00FF00) && close(f.engine.read32(turned_base), 0xFF0080FF) &&
            close(f.engine.read32(turned_base + 29 * turned_rows + 19 * 4), 0xFF0080FF), "the top-left pixel is now top-right");
    require(f.status(vt, "_VTSessionSetProperty", {rotation, f.engine.read64(f.symbol(vt, "_kVTPixelRotationPropertyKey_FlipVerticalOrientation")),
                                                  f.engine.read64(f.symbol(cf_library, "_kCFBooleanTrue"))}) == 0 &&
            f.status(vt, "_VTPixelRotationSessionRotateImage", {rotation, bgra, turned}) == 0 &&
            close(f.engine.read32(turned_base + 29 * turned_rows + 19 * 4), 0xFF00FF00), "then flipped vertically: bottom-right");
    f.call(vt, "_VTPixelRotationSessionInvalidate", {rotation});
    require(f.status(vt, "_VTPixelRotationSessionRotateImage", {rotation, bgra, turned}) == -12903, "invalidated");
    // VTCreateCGImageFromCVPixelBuffer: the 20x30 picture as a premultiplied CGImage.
    const auto cg_library = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";
    require(f.status(vt, "_VTCreateCGImageFromCVPixelBuffer", {turned, 0, out}) == 0 && f.engine.read64(out) != 0, "VTCreateCGImageFromCVPixelBuffer");
    const auto cg_image = f.engine.read64(out);
    require(f.call(cg_library, "_CGImageGetWidth", {cg_image}) == 20 && f.call(cg_library, "_CGImageGetHeight", {cg_image}) == 30 &&
            f.call(cg_library, "_CGImageGetAlphaInfo", {cg_image}) == 2, "its size, premultiplied alpha");
    require(f.status(vt, "_VTCreateCGImageFromCVPixelBuffer", {0, 0, out}) == -12902 && f.engine.read64(out) == 0, "no buffer");
    // A host without video encoders: an empty list, no encoder properties; no MV-HEVC.
    fake_video::install(true);
    require(f.status(vt, "_VTCopyVideoEncoderList", {0, out}) == 0 && f.call(cf_library, "_CFArrayGetCount", {f.engine.read64(out)}) == 0,
            "an empty encoder list");
    require(f.status(vt, "_VTCopySupportedPropertyDictionaryForEncoder", {64, 64, 0x61766331, 0, out, session_out}) == -12908, "no encoder");
    fake_video::install();
    require((f.call(vt, "_VTIsStereoMVHEVCDecodeSupported") & 0xff) == 0, "no MV-HEVC decoding");
    // The H.273 code points of the colour attachment values.
    const auto bt2020 = f.engine.read64(f.symbol(cv, "_kCVImageBufferColorPrimaries_ITU_R_2020"));
    require(f.call(cv, "_CVColorPrimariesGetIntegerCodePointForString", {bt2020}) == 9 &&
            f.call(cv, "_CVColorPrimariesGetStringForIntegerCodePoint", {9}) == bt2020 &&
            f.call(cv, "_CVColorPrimariesGetStringForIntegerCodePoint", {99}) == 0 &&
            f.call(cv, "_CVColorPrimariesGetIntegerCodePointForString", {f.cf.string("nonsense")}) == 2, "colour primaries code points");
    require(f.call(cv, "_CVTransferFunctionGetIntegerCodePointForString", {f.engine.read64(f.symbol(cv, "_kCVImageBufferTransferFunction_sRGB"))}) == 13 &&
            f.cf.utf8(f.call(cv, "_CVYCbCrMatrixGetStringForIntegerCodePoint", {6})) == "ITU_R_601_4", "transfer function and matrix code points");
    // CVPixelBufferIsCompatibleWithAttributes: the 30x20 BGRA buffer against the pool's 8x8, and its own size
    // with either of two formats.
    const std::array<Address, 2> formats{f.cf.integer(0x34323076), f.cf.integer(0x42475241)};
    const std::array<std::pair<Address, Address>, 3> own{{
        {f.cf.string("Width"), f.cf.integer(30)}, {f.cf.string("Height"), f.cf.integer(20)},
        {f.cf.string("PixelFormatType"), f.cf.array(std::vector<Address>(formats.begin(), formats.end()))}}};
    require((f.call(cv, "_CVPixelBufferIsCompatibleWithAttributes", {bgra, f.cf.dictionary(pool_buffers)}) & 0xff) == 0 &&
            (f.call(cv, "_CVPixelBufferIsCompatibleWithAttributes", {bgra, f.cf.dictionary(own)}) & 0xff) == 1, "CVPixelBufferIsCompatibleWithAttributes");
    // Pixel format descriptions: every format CVPixelBufferCreate makes, a registered one, and the four-character codes.
    const auto lookup = [&](Address dictionary, const char* key) { return f.call(cf_library, "_CFDictionaryGetValue", {dictionary, f.cf.string(key)}); };
    const auto all = f.call(cv, "_CVPixelFormatDescriptionArrayCreateWithAllPixelFormatTypes", {0});
    require(f.call(cf_library, "_CFArrayGetCount", {all}) == 34, "all pixel format types: " + std::to_string(f.call(cf_library, "_CFArrayGetCount", {all})));
    const auto biplanar = f.call(cv, "_CVPixelFormatDescriptionCreateWithPixelFormatType", {0, 0x34323076});
    const auto planes_of = lookup(biplanar, "Planes");
    require(biplanar && f.cf.utf8(lookup(biplanar, "Name")) == "420YpCbCr8BiPlanarVideoRange" && f.call(cf_library, "_CFArrayGetCount", {planes_of}) == 2 &&
            f.cf.number_value(lookup(f.call(cf_library, "_CFArrayGetValueAtIndex", {planes_of, 1}), "HorizontalSubsampling")).value_or(0) == 2,
            "the 420v description: name, two planes, chroma subsampled by 2");
    require(f.call(cv, "_CVPixelFormatDescriptionCreateWithPixelFormatType", {0, 0x7a7a7a7a}) == 0, "no description of an unknown format");
    const std::array<std::pair<Address, Address>, 1> custom_entries{{{f.cf.string("BitsPerBlock"), f.cf.integer(24)}}};
    const auto custom = f.cf.dictionary(custom_entries);
    f.call(cv, "_CVPixelFormatDescriptionRegisterDescriptionWithPixelFormatType", {custom, 0x7a7a7a7a});
    require(f.call(cv, "_CVPixelFormatDescriptionCreateWithPixelFormatType", {0, 0x7a7a7a7a}) == custom &&
            f.call(cf_library, "_CFArrayGetCount", {f.call(cv, "_CVPixelFormatDescriptionArrayCreateWithAllPixelFormatTypes", {0})}) == 35,
            "a registered description");
    require(f.cf.utf8(f.call(cv, "_CVPixelFormatTypeCopyFourCharCodeString", {0x42475241})) == "BGRA" &&
            f.cf.utf8(f.call(cv, "_CVPixelFormatTypeCopyFourCharCodeString", {0x20})) == "0x00000020", "CVPixelFormatTypeCopyFourCharCodeString");
    // CVPixelBufferCopyCreationAttributes: the size and format, and the attributes given.
    const auto made_with = f.call(cv, "_CVPixelBufferCopyCreationAttributes", {nv12});
    require(f.cf.number_value(lookup(made_with, "Width")).value_or(0) == 64 && f.cf.number_value(lookup(made_with, "PixelFormatType")).value_or(0) == 0x34323076 &&
            lookup(made_with, "IOSurfaceProperties") != 0, "CVPixelBufferCopyCreationAttributes");
    require(f.cf.number_value(lookup(f.call(cv, "_CVPixelBufferCopyCreationAttributes", {bgra}), "Height")).value_or(0) == 20, "without attributes");
    // An image buffer as a format description and a sample buffer.
    const auto description_out = f.block(8);
    require(f.status(cm, "_CMVideoFormatDescriptionCreateForImageBuffer", {0, small, description_out}) == 0, "format description for an image buffer");
    const auto description = f.engine.read64(description_out);
    require(f.call(cm, "_CMVideoFormatDescriptionGetDimensions", {description}) == (15ULL | (10ULL << 32)) &&
            f.call(cm, "_CMFormatDescriptionGetMediaSubType", {description}) == 0x42475241, "its size and pixel format");
    const auto timing = f.block(72);
    const auto sample_out = f.block(8);
    require(f.status(cm, "_CMSampleBufferCreateForImageBuffer", {0, small, 1, 0, 0, description, timing, sample_out}) == 0 &&
            f.call(cm, "_CMSampleBufferGetImageBuffer", {f.engine.read64(sample_out)}) == small, "a sample buffer over the image buffer");
    // HEVC: a 1920x1088 SPS with a 4-line bottom conformance window is 1920x1080.
    std::vector<std::uint8_t> sps{0x42, 0x01, 0x01, 0x01, 0x60, 0x01, 0x02, 0x03, 0x90, 0x01, 0x02, 0x03, 0x04, 0x05, 0x5d};
    {
        std::vector<bool> bits;
        const auto ue = [&](std::uint32_t value) {
            const auto code = value + 1;
            int length = 0;
            while ((code >> length) > 1) ++length;
            for (int i = 0; i < length; ++i) bits.push_back(false);
            for (int i = length; i >= 0; --i) bits.push_back(((code >> i) & 1) != 0);
        };
        ue(0); ue(1); ue(1920); ue(1088);
        bits.push_back(true);   // conformance_window_flag
        ue(0); ue(0); ue(0); ue(4);   // conf_win offsets in chroma units: 4 x 2 lines
        bits.push_back(true);   // stop bit
        while (bits.size() % 8) bits.push_back(false);
        for (std::size_t i = 0; i < bits.size(); i += 8) {
            std::uint8_t byte = 0;
            for (std::size_t j = 0; j < 8; ++j) byte = static_cast<std::uint8_t>((byte << 1) | (bits[i + j] ? 1 : 0));
            sps.push_back(byte);
        }
    }
    const auto sps_bytes = f.block(sps.size()), sets = f.block(8), sizes = f.block(8);
    f.engine.write_bytes(sps_bytes, sps);
    f.engine.write64(sets, sps_bytes);
    f.engine.write64(sizes, sps.size());
    require(f.status(cm, "_CMVideoFormatDescriptionCreateFromHEVCParameterSets", {0, 1, sets, sizes, 4, 0, description_out}) == 0 &&
            f.call(cm, "_CMVideoFormatDescriptionGetDimensions", {f.engine.read64(description_out)}) == (1920ULL | (1080ULL << 32)), "HEVC SPS size");
    const auto count = f.block(8), header = f.block(4);
    require(f.status(cm, "_CMVideoFormatDescriptionGetHEVCParameterSetAtIndex", {f.engine.read64(description_out), 0, 0, 0, count, header}) == 0 &&
            f.engine.read64(count) == 1 && f.engine.read32(header) == 4, "HEVC parameter set count");
    require(static_cast<std::int32_t>(f.call(vt, "_VTDecompressionSessionDecodeFrameWithOutputHandler", {0, 0, 0, 0, 0})) == -12902, "no session");
    require(f.string_constant(av, "_AVMetadataCommonKeyTitle") == "title" && f.string_constant(av, "_AVSampleRateKey") == "AVSampleRateKey", "AV keys");
}

std::vector<float> floats(Fixture& f, Address at, std::size_t count) {
    std::vector<float> values(count);
    for (std::size_t i = 0; i < count; ++i) values[i] = std::bit_cast<float>(f.engine.read32(at + i * 4));
    return values;
}
Address float_block(Fixture& f, std::initializer_list<float> values) {
    const auto at = f.block(values.size() * 4 + 4);
    std::size_t i = 0;
    for (const auto value : values) f.engine.write32(at + 4 * i++, std::bit_cast<std::uint32_t>(value));
    return at;
}

// vDSP kernels against reference loops; BLAS/LAPACK on small systems.
void accelerate(Fixture& f) {
    const auto a = float_block(f, {1, -4, 3, 2}), b = float_block(f, {-2, 1, 5, 0.5f}), out = f.block(64);
    static_cast<void>(f.call(acc, "_vDSP_maxv", {a, 1, out, 4}));
    require(floats(f, out, 1)[0] == 3, "maxv");
    static_cast<void>(f.call(acc, "_vDSP_rmsqv", {a, 1, out, 4}));
    require(std::fabs(floats(f, out, 1)[0] - std::sqrt(30.0f / 4)) < 1e-6, "rmsqv");
    static_cast<void>(f.call(acc, "_vDSP_vmaxmg", {a, 1, b, 1, out, 1, 4}));
    require(floats(f, out, 4) == std::vector<float>({2, 4, 5, 2}), "vmaxmg");
    const auto start = float_block(f, {1}), step = float_block(f, {0.5f});
    static_cast<void>(f.call(acc, "_vDSP_vramp", {start, step, out, 1, 4}));
    require(floats(f, out, 4) == std::vector<float>({1, 1.5f, 2, 2.5f}), "vramp");
    static_cast<void>(f.call(acc, "_vDSP_vclr", {out, 2, 2}));
    require(floats(f, out, 4) == std::vector<float>({0, 1.5f, 0, 2.5f}), "vclr with stride");
    const auto table = float_block(f, {0, 10, 20, 30}), index = float_block(f, {0.5f, 2.25f, 1});
    static_cast<void>(f.call(acc, "_vDSP_vlint", {table, index, 1, out, 1, 3, 4}));
    require(floats(f, out, 3) == std::vector<float>({5, 22.5f, 10}), "vlint");
    const auto c = float_block(f, {1, 1, 1, 1}), d = float_block(f, {2, 2, 2, 2});
    alchemy::CallRegs mmsb{};
    mmsb.x = {a, 1, b, 1, c, 1, d, 1};
    mmsb.push_stack(out);
    mmsb.push_stack(std::int64_t{1});
    mmsb.push_stack(std::uint64_t{4});
    static_cast<void>(f.regs(acc, "_vDSP_vmmsb", mmsb));
    require(floats(f, out, 4) == std::vector<float>({-4, -6, 13, -1}), "vmmsb = A*B - C*D");

    // cblas_dgemm, row-major: C(2x2) = 1 * A(2x3) * B(3x2) + 0 * C.
    const auto doubles = [&](std::initializer_list<double> values) {
        const auto at = f.block(values.size() * 8);
        std::size_t i = 0;
        for (const auto value : values) f.engine.write64(at + 8 * i++, std::bit_cast<std::uint64_t>(value));
        return at;
    };
    const auto read = [&](Address at, std::size_t i) { return std::bit_cast<double>(f.engine.read64(at + 8 * i)); };
    const auto ma = doubles({1, 2, 3, 4, 5, 6}), mb = doubles({7, 8, 9, 10, 11, 12}), mc = doubles({0, 0, 0, 0});
    alchemy::CallRegs gemm{};
    gemm.x = {101, 111, 111, 2, 2, 3, ma, 3};
    gemm.set_d(0, 1.0);
    gemm.set_d(1, 0.0);
    gemm.push_stack(mb);
    gemm.push_stack(std::int32_t{2});
    gemm.push_stack(mc);
    gemm.push_stack(std::int32_t{2});
    static_cast<void>(f.regs(acc, "_cblas_dgemm", gemm));
    require(read(mc, 0) == 58 && read(mc, 1) == 64 && read(mc, 2) == 139 && read(mc, 3) == 154, "dgemm");
    // dgesv_: [[2, 1], [1, 3]] x = [3, 5] -> x = [0.8, 1.4] (column-major).
    const auto ints = [&](std::initializer_list<std::int32_t> values) {
        const auto at = f.block(values.size() * 4);
        std::size_t i = 0;
        for (const auto value : values) f.engine.write32(at + 4 * i++, static_cast<std::uint32_t>(value));
        return at;
    };
    const auto n = ints({2}), nrhs = ints({1}), lda = ints({2}), ipiv = ints({0, 0}), ldb = ints({2}), info = ints({-99});
    const auto matrix = doubles({2, 1, 1, 3}), rhs = doubles({3, 5});
    static_cast<void>(f.call(acc, "_dgesv_", {n, nrhs, matrix, lda, ipiv, rhs, ldb, info}));
    require(f.engine.read32(info) == 0 && std::fabs(read(rhs, 0) - 0.8) < 1e-12 && std::fabs(read(rhs, 1) - 1.4) < 1e-12, "dgesv");
    const auto singular = doubles({1, 2, 2, 4}), rhs2 = doubles({1, 1});
    static_cast<void>(f.call(acc, "_dgesv_", {n, nrhs, singular, lda, ipiv, rhs2, ldb, info}));
    require(f.engine.read32(info) == 2, "dgesv reports the zero pivot");
}

void core_midi(Fixture& f) {
    const auto client_out = f.block(4), port_out = f.block(4), name = f.cf.string("QLab");
    require(f.status(midi_lib, "_MIDIClientCreate", {name, 0, 0, client_out}) == 0, "MIDIClientCreate");
    const auto client = f.engine.read32(client_out);
    require(f.call(midi_lib, "_MIDIGetNumberOfDevices") == 1 && f.call(midi_lib, "_MIDIGetNumberOfSources") == 1 &&
            f.call(midi_lib, "_MIDIGetNumberOfDestinations") == 1 && f.call(midi_lib, "_MIDIGetNumberOfExternalDevices") == 0, "enumeration");
    const auto device = f.call(midi_lib, "_MIDIGetDevice", {0});
    const auto entity = f.call(midi_lib, "_MIDIDeviceGetEntity", {device, 0});
    require(f.call(midi_lib, "_MIDIDeviceGetNumberOfEntities", {device}) == 1 && f.call(midi_lib, "_MIDIEntityGetNumberOfSources", {entity}) == 1, "entity");
    const auto destination = f.call(midi_lib, "_MIDIEntityGetDestination", {entity, 0});
    require(destination == f.call(midi_lib, "_MIDIGetDestination", {0}), "entity destination is listed");
    const auto string_out = f.block(8);
    const auto name_key = f.engine.read64(f.symbol(midi_lib, "_kMIDIPropertyName"));
    require(f.status(midi_lib, "_MIDIObjectGetStringProperty", {destination, name_key, string_out}) == 0 &&
            f.cf.utf8(f.engine.read64(string_out)) == "Port A", "endpoint name");
    const auto manufacturer = f.engine.read64(f.symbol(midi_lib, "_kMIDIPropertyManufacturer"));
    require(f.status(midi_lib, "_MIDIObjectGetStringProperty", {destination, manufacturer, string_out}) == 0 &&
            f.cf.utf8(f.engine.read64(string_out)) == "Maker", "inherited manufacturer");
    const auto offline = f.engine.read64(f.symbol(midi_lib, "_kMIDIPropertyOffline")), number_out = f.block(4, 0xff);
    require(f.status(midi_lib, "_MIDIObjectGetIntegerProperty", {destination, offline, number_out}) == 0 && f.engine.read32(number_out) == 0, "online");
    const auto private_key = f.engine.read64(f.symbol(midi_lib, "_kMIDIPropertyPrivate"));
    require(f.status(midi_lib, "_MIDIObjectGetIntegerProperty", {destination, private_key, number_out}) == -10835, "unset property");
    const auto entity_out = f.block(4);
    require(f.status(midi_lib, "_MIDIEndpointGetEntity", {destination, entity_out}) == 0 && f.engine.read32(entity_out) == entity, "endpoint entity");
    const auto properties = f.block(8);
    require(f.status(midi_lib, "_MIDIObjectGetProperties", {device, properties, 1}) == 0, "MIDIObjectGetProperties");
    const auto entities_key = f.cf.string("entities");
    require(f.cf.dictionary_value(f.engine.read64(properties), entities_key) != 0, "deep properties list the entities");

    // Output: a note to the hardware destination reaches the backend.
    require(f.status(midi_lib, "_MIDIOutputPortCreate", {client, name, port_out}) == 0, "output port");
    const auto list = f.block(256);
    const auto packet = f.call(midi_lib, "_MIDIPacketListInit", {list});
    const auto bytes = f.block(3);
    f.memory().write8(bytes, 0x90); f.memory().write8(bytes + 1, 64); f.memory().write8(bytes + 2, 100);
    require(f.call(midi_lib, "_MIDIPacketListAdd", {list, 256, packet, 0, 3, bytes}) != 0, "packet added");
    require(f.status(midi_lib, "_MIDISend", {f.engine.read32(port_out), destination, list}) == 0, "MIDISend");
    {
        std::lock_guard lock(FakeMidi::mutex());
        require(FakeMidi::sent().size() == 1 && FakeMidi::sent()[0] == std::vector<std::uint8_t>({0x90, 64, 100}), "backend got the note");
    }
    // A virtual source with a chosen unique ID; SysEx requests complete synchronously.
    const auto source_out = f.block(4);
    require(f.status(midi_lib, "_MIDISourceCreate", {client, name, source_out}) == 0 && f.call(midi_lib, "_MIDIGetNumberOfSources") == 2, "virtual source");
    const auto unique_key = f.engine.read64(f.symbol(midi_lib, "_kMIDIPropertyUniqueID"));
    require(f.status(midi_lib, "_MIDIObjectSetIntegerProperty", {f.engine.read32(source_out), unique_key, 31337}) == 0, "set unique ID");
    const auto found = f.block(4), type = f.block(4);
    require(f.status(midi_lib, "_MIDIObjectFindByUniqueID", {31337, found, type}) == 0 && f.engine.read32(found) == f.engine.read32(source_out) &&
            f.engine.read32(type) == 2, "find by unique ID");
    require(f.status(midi_lib, "_MIDIReceived", {f.engine.read32(source_out), list}) == 0, "MIDIReceived with no listeners");
    const auto request = f.block(40), sysex = f.block(4);
    f.memory().write8(sysex, 0xf0); f.memory().write8(sysex + 1, 0x7e); f.memory().write8(sysex + 2, 0x01); f.memory().write8(sysex + 3, 0xf7);
    f.engine.write32(request, static_cast<std::uint32_t>(destination));
    f.engine.write64(request + 8, sysex);
    f.engine.write32(request + 16, 4);
    require(f.status(midi_lib, "_MIDISendSysex", {request}) == 0 && f.memory().read8(request + 20) == 1 && f.engine.read32(request + 16) == 0, "SysEx request completes");
    {
        std::lock_guard lock(FakeMidi::mutex());
        require(FakeMidi::sent().size() == 2 && FakeMidi::sent()[1].size() == 4, "SysEx reached the backend");
    }
    require(f.status(midi_lib, "_MIDIFlushOutput", {0}) == 0, "flush all");

    // MIDI 2.0 event lists: sent as MIDI 1.0 to the hardware, delivered in each receiver's protocol.
    const auto events = f.block(512), words = f.block(16);
    const auto add = [&](Address list, Address current, std::initializer_list<std::uint32_t> values) {
        std::uint32_t i = 0;
        for (const auto value : values) f.engine.write32(words + 4ULL * i++, value);
        return f.call(midi_lib, "_MIDIEventListAdd", {list, 512, current, 0, values.size(), words});
    };
    auto current = f.call(midi_lib, "_MIDIEventListInit", {events, 2});   // kMIDIProtocol_2_0
    require(current == events + 8 && add(events, current, {0x40903c00, 0xffff0000}) == current, "MIDIEventListInit / Add");
    require(f.status(midi_lib, "_MIDISendEventList", {f.engine.read32(port_out), destination, events}) == 0, "MIDISendEventList");
    {
        std::lock_guard lock(FakeMidi::mutex());
        require(FakeMidi::sent().size() == 3 && FakeMidi::sent()[2] == std::vector<std::uint8_t>({0x90, 60, 127}), "a MIDI 2.0 note on as MIDI 1.0");
    }
    static std::atomic<int> received{0};
    static std::atomic<std::uint32_t> protocol{0}, first{0}, second{0};
    const auto b = alchemy::gui::security::bridge();
    const auto receive = b->make_block([](transmute::CpuState& s, transmute::GuestMemory& m) {
        protocol = m.read32(s.x[1]);
        if (m.read32(s.x[1] + 4) && m.read32(s.x[1] + 16) >= 2) {
            first = m.read32(s.x[1] + 20);
            second = m.read32(s.x[1] + 24);
        }
        ++received;
    });
    const auto virtual_out = f.block(4);
    require(f.status(midi_lib, "_MIDIDestinationCreateWithProtocol", {client, name, 2, virtual_out, receive}) == 0, "MIDIDestinationCreateWithProtocol");
    f.memory().write8(bytes, 0x91); f.memory().write8(bytes + 1, 64); f.memory().write8(bytes + 2, 100);
    const auto again = f.call(midi_lib, "_MIDIPacketListInit", {list});
    require(f.call(midi_lib, "_MIDIPacketListAdd", {list, 256, again, 0, 3, bytes}) != 0, "a MIDI 1.0 note on");
    require(f.status(midi_lib, "_MIDISend", {f.engine.read32(port_out), f.engine.read32(virtual_out), list}) == 0, "MIDISend to the virtual destination");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (received == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    require(received == 1 && protocol == 2 && first == 0x40914000 && second == 0xc9240000U, "delivered as a MIDI 2.0 event list");
    // An input port with the 1.0 protocol hears a virtual source's event list.
    const auto source_again = f.block(4), input_out = f.block(4);
    require(f.status(midi_lib, "_MIDISourceCreateWithProtocol", {client, name, 1, source_again}) == 0, "MIDISourceCreateWithProtocol");
    require(f.status(midi_lib, "_MIDIInputPortCreateWithProtocol", {client, name, 1, input_out, receive}) == 0 &&
            f.status(midi_lib, "_MIDIPortConnectSource", {f.engine.read32(input_out), f.engine.read32(source_again), 0}) == 0, "an input port");
    current = f.call(midi_lib, "_MIDIEventListInit", {events, 1});
    require(add(events, current, {0x20b30740}) != 0 && f.status(midi_lib, "_MIDIReceivedEventList", {f.engine.read32(source_again), events}) == 0,
            "MIDIReceivedEventList");
    while (received == 1 && std::chrono::steady_clock::now() < deadline + std::chrono::seconds(3)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    require(received == 2 && protocol == 1, "delivered as a MIDI 1.0 event list");
    require(f.status(midi_lib, "_MIDIPortDispose", {f.engine.read32(input_out)}) == 0 &&
            f.status(midi_lib, "_MIDIEndpointDispose", {f.engine.read32(source_again)}) == 0 &&
            f.status(midi_lib, "_MIDIEndpointDispose", {f.engine.read32(virtual_out)}) == 0, "event-list endpoints disposed");
    require(f.status(midi_lib, "_MIDIEndpointDispose", {f.engine.read32(source_out)}) == 0 && f.call(midi_lib, "_MIDIGetNumberOfSources") == 1, "dispose");
    require(f.status(midi_lib, "_MIDIPortDispose", {f.engine.read32(port_out)}) == 0 && f.status(midi_lib, "_MIDIClientDispose", {client}) == 0, "dispose port and client");
}

void core_audio(Fixture& f) {
    namespace hal = alchemy::gui::completion_kit::media::hal;
    const auto address = f.block(12), size = f.block(4), data = f.block(64);
    const auto set = [&](std::uint32_t selector, std::uint32_t scope) {
        f.engine.write32(address, selector);
        f.engine.write32(address + 4, scope);
        f.engine.write32(address + 8, 0);
    };
    set(hal::four("dev#"), hal::scope_global);
    require(f.status(ca, "_AudioObjectGetPropertyDataSize", {1, address, 0, 0, size}) == 0 && f.engine.read32(size) == 4, "one device");
    f.engine.write32(size, 64);
    require(f.status(ca, "_AudioObjectGetPropertyData", {1, address, 0, 0, size, data}) == 0 && f.engine.read32(size) == 4, "device list");
    const auto device = f.engine.read32(data);
    set(hal::four("dOut"), hal::scope_global);
    f.engine.write32(size, 4);
    require(f.status(ca, "_AudioObjectGetPropertyData", {1, address, 0, 0, size, data}) == 0 && f.engine.read32(data) == device, "default output");
    set(hal::four("lnam"), hal::scope_global);
    f.engine.write32(size, 8);
    require(f.status(ca, "_AudioObjectGetPropertyData", {device, address, 0, 0, size, data}) == 0 && f.cf.utf8(f.engine.read64(data)) == "Line Out", "device name");
    // kAudioHardwarePropertyTranslateUIDToDevice with a CFString qualifier.
    set(hal::four("uidd"), hal::scope_global);
    const auto uid = f.block(8);
    f.engine.write64(uid, f.cf.string("{render}"));
    f.engine.write32(size, 4);
    require(f.status(ca, "_AudioObjectGetPropertyData", {1, address, 8, uid, size, data}) == 0 && f.engine.read32(data) == device, "UID to device");
    set(hal::four("nsrt"), hal::scope_global);
    f.engine.write32(size, 8);
    require(f.status(ca, "_AudioObjectGetPropertyData", {device, address, 0, 0, size, data}) == 0 &&
            std::bit_cast<double>(f.engine.read64(data)) == 48000.0, "nominal sample rate");
    set(hal::four("dev#"), hal::scope_global);
    f.engine.write32(size, 2);
    require(f.status(ca, "_AudioObjectGetPropertyData", {1, address, 0, 0, size, data}) == 0 && f.engine.read32(size) == 0, "array truncates to whole elements");
    set(hal::four("nsrt"), hal::scope_global);
    f.engine.write32(size, 4);
    require(f.status(ca, "_AudioObjectGetPropertyData", {device, address, 0, 0, size, data}) == hal::error::bad_size, "short scalar");
    // An IOProc (a host function standing in for guest code) writes 0.25 into every sample of
    // its output buffer; the device plays it.
    const auto io_proc = f.process.install_function("_alchemy_test_io_proc", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        const auto list = s.x[4];
        const std::uint64_t buffer = m.read64(list + 16), bytes = m.read32(list + 12);
        for (std::uint32_t i = 0; i < bytes / 4; ++i) m.write32(buffer + 4ULL * i, std::bit_cast<std::uint32_t>(0.25f));
        require(m.read32(s.x[5] + 56) == 7 && s.x[6] == 0x77, "IOProc time stamp flags and client data");
        s.x[0] = 0;
    }, "/usr/lib/alchemy_test_io.dylib");
    const auto proc_out = f.block(8);
    require(f.status(ca, "_AudioDeviceCreateIOProcID", {77, io_proc, 0, proc_out}) == hal::error::bad_device, "not a device");
    require(f.status(ca, "_AudioDeviceGetCurrentTime", {device, data}) == hal::error::not_running, "not running");
    require(f.status(ca, "_AudioDeviceCreateIOProcID", {device, io_proc, 0x77, proc_out}) == 0 && f.engine.read64(proc_out) != 0, "IOProc created");
    const auto proc_id = f.engine.read64(proc_out);
    require(f.status(ca, "_AudioDeviceStart", {device, proc_id}) == 0, "AudioDeviceStart");
    const auto played = FakeStream::play_until_sound(256);
    require(played[0] == 0.25f && played[1] == 0.25f, "the IOProc's samples play");
    require(f.status(ca, "_AudioDeviceGetCurrentTime", {device, data}) == 0 && std::bit_cast<double>(f.engine.read64(data)) >= 256 &&
            f.engine.read32(data + 56) == 7, "current time while running");
    // AudioDeviceTranslateTime: a sample time to host time, and that host time back.
    const auto in_time = f.block(64), out_time = f.block(64);
    f.engine.write64(in_time, std::bit_cast<std::uint64_t>(1000.0));
    f.engine.write32(in_time + 56, 1);   // kAudioTimeStampSampleTimeValid
    require(f.status(ca, "_AudioDeviceTranslateTime", {device, in_time, out_time}) == 0 && f.engine.read32(out_time + 56) == 7 &&
            std::bit_cast<double>(f.engine.read64(out_time)) == 1000.0, "sample time to host time");
    f.engine.write64(in_time + 8, f.engine.read64(out_time + 8));
    f.engine.write32(in_time + 56, 2);   // kAudioTimeStampHostTimeValid
    require(f.status(ca, "_AudioDeviceTranslateTime", {device, in_time, out_time}) == 0 &&
            std::fabs(std::bit_cast<double>(f.engine.read64(out_time)) - 1000.0) < 4800.0,
            "host time back to sample time (the clock is read again: within 0.1 s)");
    f.engine.write64(in_time, std::bit_cast<std::uint64_t>(0.0));
    f.engine.write32(in_time + 56, 1);
    require(f.status(ca, "_AudioDeviceGetNearestStartTime", {device, in_time, 0}) == 0 && std::bit_cast<double>(f.engine.read64(in_time)) >= 256,
            "the nearest start time is not in the past");
    require(f.status(ca, "_AudioDeviceStop", {device, proc_id}) == 0, "AudioDeviceStop");
    require(f.status(ca, "_AudioDeviceGetCurrentTime", {device, data}) == hal::error::not_running, "stopped");
    require(f.status(ca, "_AudioDeviceTranslateTime", {device, in_time, out_time}) == hal::error::not_running, "no translation when stopped");
    require(f.status(ca, "_AudioDeviceDestroyIOProcID", {device, proc_id}) == 0, "IOProc destroyed");
    require(f.status(ca, "_AudioDeviceDestroyIOProcID", {device, proc_id}) == hal::error::illegal_operation, "destroyed twice");
    set(hal::four("dOut"), hal::scope_global);
    require(f.status(ca, "_AudioObjectAddPropertyListener", {1, address, 0x1000, 0}) == 0, "listener");

    // The older property calls: the same properties by direction and channel.
    f.engine.write32(size, 4);
    require(f.status(ca, "_AudioHardwareGetProperty", {hal::four("dOut"), size, data}) == 0 && f.engine.read32(data) == device, "AudioHardwareGetProperty");
    const auto writable = f.block(8);
    f.engine.write64(writable, 0xff);
    require(f.status(ca, "_AudioDeviceGetPropertyInfo", {device, 0, 0, hal::four("fsiz"), size, writable}) == 0 && f.engine.read32(size) == 4 &&
            f.memory().read8(writable) == 1, "AudioDeviceGetPropertyInfo: the buffer size is writable");
    f.engine.write32(size, 4);
    require(f.status(ca, "_AudioDeviceGetProperty", {device, 0, 0, hal::four("fsiz"), size, data}) == 0, "AudioDeviceGetProperty");
    const auto frames = f.engine.read32(data);
    require(f.status(ca, "_AudioDeviceSetProperty", {device, 0, 0, 0, hal::four("fsiz"), 4, data}) == 0, "AudioDeviceSetProperty");
    set(hal::four("fsiz"), hal::scope_global);
    f.engine.write32(size, 4);
    require(f.status(ca, "_AudioObjectGetPropertyData", {device, address, 0, 0, size, data}) == 0 && f.engine.read32(data) == frames, "the same buffer size");
    f.engine.write32(size, 64);
    require(f.status(ca, "_AudioDeviceGetProperty", {device, 0, 0, hal::four("name"), size, data}) == 0 && f.engine.read32(size) > 1 &&
            f.memory().read8(data) != 0, "the device's C string name");
    f.engine.write32(size, 64);
    require(f.status(ca, "_AudioDeviceGetProperty", {device, 0, 0, hal::four("stm#"), size, data}) == 0 && f.engine.read32(size) == 4, "its stream");
    const auto stream = f.engine.read32(data);
    f.engine.write32(size, 4);
    require(f.status(ca, "_AudioStreamGetProperty", {stream, 0, hal::four("sdir"), size, data}) == 0 && f.engine.read32(data) == 0, "AudioStreamGetProperty");
    require(f.status(ca, "_AudioStreamGetPropertyInfo", {stream, 0, hal::four("sdir"), size, 0}) == 0 && f.engine.read32(size) == 4, "AudioStreamGetPropertyInfo");
    require(f.status(ca, "_AudioDeviceAddPropertyListener", {device, 0, 0, hal::four("nsrt"), 0x1000, 0}) == 0 &&
            f.status(ca, "_AudioDeviceAddPropertyListener", {77, 0, 0, hal::four("nsrt"), 0x1000, 0}) == hal::error::bad_object, "older listeners");
    require(f.status(ca, "_AudioHardwareUnload", {}) == 0, "AudioHardwareUnload");
    require(f.call(ca, "_AudioConvertHostTimeToNanos", {12345}) == 12345, "host time is nanoseconds");
}

std::vector<std::uint8_t> fixture_bytes(const char* name) {
    const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
    std::ifstream in(path, std::ios::binary);
    require(in.good(), "fixture missing: " + path.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The default output unit: a mono 16-bit 24 kHz client, resampled and copied to both channels
// of the 48 kHz device.
void output_units(Fixture& f) {
    const auto description = f.block(20), component_out = f.block(8), unit_out = f.block(8), size = f.block(4), value = f.block(64);
    f.engine.write32(description, 0x61756F75);        // 'auou'
    f.engine.write32(description + 4, 0x64656620);    // 'def '
    const auto component = f.call(au, "_AudioComponentFindNext", {0, description});
    require(component != 0 && f.call(au, "_AudioComponentFindNext", {component, description}) == 0, "one default output component");
    f.engine.write32(description + 4, 0);
    require(f.call(au, "_AudioComponentCount", {description}) == 3, "three output units");
    require(f.status(au, "_AudioComponentCopyName", {component, component_out}) == 0 && f.cf.utf8(f.engine.read64(component_out)) == "Apple: DefaultOutputUnit",
            "component name");
    // AudioComponentInstantiate: the handler gets an instance (and noErr), or NULL and the status for a component that is none.
    static std::vector<std::pair<Address, std::int32_t>> instantiated;
    instantiated.clear();
    const auto handler_fn = f.process.install_function("_alchemy_test_au_instantiated", [](transmute::CpuState& s, transmute::GuestMemory&) {
        instantiated.emplace_back(s.x[1], static_cast<std::int32_t>(s.x[2]));   // (block, AudioComponentInstance, OSStatus)
    }, "/usr/lib/alchemy_test_io.dylib");
    const auto handler = f.block(32), handler_descriptor = f.block(16);
    f.engine.write64(handler_descriptor + 8, 32);
    f.engine.write64(handler, f.process.resolve("/usr/lib/libSystem.B.dylib", "__NSConcreteGlobalBlock")->address);
    f.engine.write32(handler + 8, 1U << 28);   // BLOCK_IS_GLOBAL
    f.engine.write64(handler + 16, handler_fn);
    f.engine.write64(handler + 24, handler_descriptor);
    f.call(au, "_AudioComponentInstantiate", {component, 0, handler});
    f.call(au, "_AudioComponentInstantiate", {0, 0, handler});
    require(instantiated.size() == 2 && instantiated[0].first && instantiated[0].second == 0 && !instantiated[1].first && instantiated[1].second == -50,
            "AudioComponentInstantiate: an instance, then paramErr for no component");
    require(f.status(au, "_AudioComponentInstanceDispose", {instantiated[0].first}) == 0, "the instantiated unit disposed");
    require(f.status(au, "_AudioComponentInstanceNew", {component, unit_out}) == 0, "AudioComponentInstanceNew");
    const auto unit = f.engine.read64(unit_out);
    f.engine.write32(size, 4);
    require(f.status(au, "_AudioUnitGetProperty", {unit, 2006, 1, 1, value, size}) == 0 && f.engine.read32(value) == 0, "no input I/O");
    require(f.status(au, "_AudioUnitSetProperty", {unit, 2003, 1, 1, value, 4}) == -10865, "the default output has no EnableIO");
    // kAudioUnitProperty_StreamFormat, input scope: 24 kHz mono signed 16-bit packed.
    f.engine.write64(value, std::bit_cast<std::uint64_t>(24000.0));
    f.engine.write32(value + 8, 0x6C70636D);   // 'lpcm'
    f.engine.write32(value + 12, 4 | 8);       // signed integer, packed
    f.engine.write32(value + 16, 2);
    f.engine.write32(value + 20, 1);
    f.engine.write32(value + 24, 2);
    f.engine.write32(value + 28, 1);
    f.engine.write32(value + 32, 16);
    f.engine.write32(value + 36, 0);
    require(f.status(au, "_AudioUnitSetProperty", {unit, 8, 1, 0, value, 40}) == 0, "client stream format");
    // The render callback writes 8192 (0.25) into every sample.
    const auto callback = f.process.install_function("_alchemy_test_render", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        const std::uint64_t frames = static_cast<std::uint32_t>(s.x[4]), list = s.x[5];
        require(m.read32(list) == 1 && m.read32(list + 12) == frames * 2, "one 16-bit mono buffer");
        const auto buffer = m.read64(list + 16);
        for (std::uint32_t i = 0; i < frames; ++i) {
            m.write8(buffer + 2ULL * i, 0x00);
            m.write8(buffer + 2ULL * i + 1, 0x20);
        }
        s.x[0] = 0;
    }, "/usr/lib/alchemy_test_io.dylib");
    f.engine.write64(value, callback);
    f.engine.write64(value + 8, 0);
    require(f.status(au, "_AudioUnitSetProperty", {unit, 23, 0, 0, value, 16}) == 0, "render callback");
    require(f.status(au, "_AudioOutputUnitStart", {unit}) == -10867, "uninitialized");
    require(f.status(au, "_AudioUnitInitialize", {unit}) == 0 && f.status(au, "_AudioOutputUnitStart", {unit}) == 0, "started");
    f.engine.write32(size, 4);
    require(f.status(au, "_AudioUnitGetProperty", {unit, 2001, 0, 0, value, size}) == 0 && f.engine.read32(value) == 1, "IsRunning");
    // Past the resampler's onset, a constant input plays as the same constant on both channels.
    static_cast<void>(FakeStream::play_until_sound(256));
    std::vector<float> steady;
    for (int i = 0; i < 8; ++i) steady = FakeStream::play_paced(256);
    require(std::fabs(steady[200] - 0.25f) < 0.01f && std::fabs(steady[201] - 0.25f) < 0.01f, "mono 24 kHz plays on both channels at 48 kHz");
    // kHALOutputParam_Volume halves it.
    alchemy::CallRegs volume{};
    volume.x[0] = unit; volume.x[1] = 14;
    volume.set_s(0, 0.5f);
    require(f.regs(au, "_AudioUnitSetParameter", volume).x0 == 0, "volume");
    for (int i = 0; i < 8; ++i) steady = FakeStream::play_paced(256);
    require(std::fabs(steady[200] - 0.125f) < 0.01f, "volume applied");
    require(f.status(au, "_AudioOutputUnitStop", {unit}) == 0 && f.status(au, "_AudioUnitUninitialize", {unit}) == 0, "stopped");
    require(f.status(au, "_AudioComponentInstanceDispose", {unit}) == 0, "disposed");
    // AUHAL: enabling input makes initialization fail (no input device).
    f.engine.write32(description + 4, 0x6168616C);   // 'ahal'
    const auto hal_component = f.call(au, "_AudioComponentFindNext", {0, description});
    require(hal_component != 0 && f.status(au, "_AudioComponentInstanceNew", {hal_component, unit_out}) == 0, "AUHAL");
    const auto hal_unit = f.engine.read64(unit_out);
    f.engine.write32(value, 1);
    require(f.status(au, "_AudioUnitSetProperty", {hal_unit, 2003, 1, 1, value, 4}) == 0 &&
            f.status(au, "_AudioUnitInitialize", {hal_unit}) == -10875, "no input device");
    require(f.status(au, "_AudioUnitRender", {hal_unit, 0, 0, 1, 64, value}) == -10876, "no input to pull");
    require(f.status(au, "_AudioComponentInstanceDispose", {hal_unit}) == 0, "AUHAL disposed");

    // AUGraph: a default output node fed by a node input callback plays; a DLS synth node has no component here, so
    // the graph does not open with it; two output units do not connect; no unit is a music device.
    const auto graph_out = f.block(8), node_out = f.block(4), output_node_out = f.block(4), flag = f.block(4), unit_info = f.block(8);
    require(f.status(at, "_NewAUGraph", {graph_out}) == 0, "NewAUGraph");
    const auto graph = f.engine.read64(graph_out);
    f.engine.write32(description, 0x61756F75);        // 'auou'
    f.engine.write32(description + 4, 0x64656620);    // 'def '
    f.engine.write32(description + 8, 0x6170706C);    // 'appl'
    require(f.status(at, "_AUGraphAddNode", {graph, description, output_node_out}) == 0, "an output node");
    const auto output_node = f.engine.read32(output_node_out);
    const auto synth = f.block(20);
    f.engine.write32(synth, 0x61756D75); f.engine.write32(synth + 4, 0x646C7320); f.engine.write32(synth + 8, 0x6170706C);   // 'aumu' 'dls ' 'appl'
    require(f.status(at, "_AUGraphAddNode", {graph, synth, node_out}) == 0, "a DLS synth node (kept until open)");
    const auto synth_node = f.engine.read32(node_out);
    require(f.status(at, "_AUGraphOpen", {graph}) == -10864 && f.status(at, "_AUGraphIsOpen", {graph, flag}) == 0 && (f.engine.read32(flag) & 0xff) == 0,
            "no DLS synth: kAUGraphErr_InvalidAudioUnit, still closed");
    require(f.status(at, "_AUGraphRemoveNode", {graph, synth_node}) == 0 && f.status(at, "_AUGraphGetNodeCount", {graph, flag}) == 0 &&
            f.engine.read32(flag) == 1, "the synth node removed");
    require(f.status(at, "_AUGraphOpen", {graph}) == 0, "AUGraphOpen");
    require(f.status(at, "_AUGraphNodeInfo", {graph, output_node, 0, unit_info}) == 0 && f.engine.read64(unit_info) != 0, "the output node's unit");
    const auto graph_unit = f.engine.read64(unit_info);
    require(static_cast<std::uint32_t>(f.status(at, "_MusicDeviceMIDIEvent", {graph_unit, 0x90, 60, 100, 0})) == 0x80008002U,
            "an output unit is not a music device (badComponentSelector)");
    // The callback writes 0.25 into every float sample of every buffer.
    const auto graph_render = f.process.install_function("_alchemy_test_graph_render", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        const auto list = s.x[5];
        for (std::uint32_t b = 0; b < m.read32(list); ++b) {
            const auto bytes = m.read32(list + 8 + 16ULL * b + 4);
            const auto data = m.read64(list + 8 + 16ULL * b + 8);
            for (std::uint32_t i = 0; i + 4 <= bytes; i += 4) m.write32(data + i, std::bit_cast<std::uint32_t>(0.25f));
        }
        s.x[0] = 0;
    }, "/usr/lib/alchemy_test_io.dylib");
    f.engine.write64(value, graph_render);
    f.engine.write64(value + 8, 0);
    require(f.status(at, "_AUGraphSetNodeInputCallback", {graph, output_node, 0, value}) == 0, "a node input callback");
    require(f.status(at, "_AUGraphStart", {graph}) == -10863, "not initialized: kAUGraphErr_CannotDoInCurrentContext");
    require(f.status(at, "_AUGraphInitialize", {graph}) == 0 && f.status(at, "_AUGraphStart", {graph}) == 0 &&
            f.status(at, "_AUGraphIsRunning", {graph, flag}) == 0 && (f.engine.read32(flag) & 0xff) == 1, "the graph runs");
    static_cast<void>(FakeStream::play_until_sound(256));
    std::vector<float> graph_steady;
    for (int i = 0; i < 8; ++i) graph_steady = FakeStream::play_paced(256);
    require(std::fabs(graph_steady[200] - 0.25f) < 0.01f, "the graph's output plays its callback");
    require(f.status(at, "_AUGraphStop", {graph}) == 0 && f.status(at, "_AUGraphUninitialize", {graph}) == 0, "stopped");
    // A second output node connected into the first: output units take no connection, so initializing fails.
    require(f.status(at, "_AUGraphAddNode", {graph, description, node_out}) == 0, "a second output node");
    require(f.status(at, "_AUGraphConnectNodeInput", {graph, f.engine.read32(node_out), 0, output_node, 0}) == 0 &&
            f.status(at, "_AUGraphInitialize", {graph}) == -10879, "the connection is refused at initialize");
    require(f.status(at, "_AUGraphConnectNodeInput", {graph, output_node, 0, output_node, 0}) == -10861, "no node connects to itself");
    require(f.status(at, "_AUGraphConnectNodeInput", {graph, 99, 0, output_node, 0}) == -10860, "an unknown node");
    require(f.status(at, "_DisposeAUGraph", {graph}) == 0 && f.status(at, "_AUGraphOpen", {graph}) == -50, "disposed");
}

// AudioConverter: 16-bit stereo 44.1 kHz to float mono 48 kHz through the input procedure.
void audio_converter(Fixture& f) {
    const auto asbd = [&](double rate, std::uint32_t flags, std::uint32_t bytes, std::uint32_t channels, std::uint32_t bits) {
        const auto at = f.block(40);
        f.engine.write64(at, std::bit_cast<std::uint64_t>(rate));
        f.engine.write32(at + 8, 0x6C70636D);   // 'lpcm'
        f.engine.write32(at + 12, flags);
        f.engine.write32(at + 16, bytes * channels);
        f.engine.write32(at + 20, 1);
        f.engine.write32(at + 24, bytes * channels);
        f.engine.write32(at + 28, channels);
        f.engine.write32(at + 32, bits);
        return at;
    };
    const auto in = asbd(44100, 4 | 8, 2, 2, 16), out_format = asbd(48000, 1 | 8, 4, 1, 32);
    const auto converter_out = f.block(8);
    require(f.status(at, "_AudioConverterNew", {in, out_format, converter_out}) == 0, "AudioConverterNew");
    const auto converter = f.engine.read64(converter_out);
    // The input: 4410 frames of constant 16384 (0.5) in both channels, then the end.
    static Address source{};
    static std::uint32_t remaining{};
    source = f.block(4410 * 4);
    for (std::uint32_t i = 0; i < 4410 * 2; ++i) {
        f.memory().write8(source + 2ULL * i, 0x00);
        f.memory().write8(source + 2ULL * i + 1, 0x40);
    }
    remaining = 4410;
    const auto input = f.process.install_function("_alchemy_test_converter_input", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        const auto wanted = m.read32(s.x[1]);
        const auto give = std::min(wanted, remaining);
        const auto offset = (4410 - remaining) * 4ULL;
        m.write32(s.x[1], give);
        m.write32(s.x[2] + 8 + 4, give * 4);
        m.write64(s.x[2] + 8 + 8, give ? source + offset : 0);
        remaining -= give;
        s.x[0] = 0;
    }, "/usr/lib/alchemy_test_io.dylib");
    const auto list = f.block(24), data = f.block(4800 * 4), packets = f.block(4);
    f.engine.write32(list, 1);
    f.engine.write32(list + 8, 1);
    f.engine.write32(list + 12, 4800 * 4);
    f.engine.write64(list + 16, data);
    f.engine.write32(packets, 4800);
    require(f.status(at, "_AudioConverterFillComplexBuffer", {converter, input, 0, packets, list, 0}) == 0, "FillComplexBuffer");
    const auto made = f.engine.read32(packets);
    require(made >= 4790 && made <= 4800, "about 4800 frames out of 4410 in: " + std::to_string(made));
    require(std::fabs(std::bit_cast<float>(f.engine.read32(data + 4 * 2400)) - 0.5f) < 0.01f, "stereo 0.5 becomes mono 0.5");
    const auto size = f.block(4), value = f.block(40);
    f.engine.write32(size, 40);
    require(f.status(at, "_AudioConverterGetProperty", {converter, 0x61636F64 /* 'acod' */, size, value}) == 0 &&
            std::bit_cast<double>(f.engine.read64(value)) == 48000.0, "current output description");
    require(f.status(at, "_AudioConverterDispose", {converter}) == 0, "AudioConverterDispose");
    const auto aac = asbd(44100, 0, 0, 2, 0);
    f.engine.write32(aac + 8, 0x61616320);   // 'aac '
    // AAC decodes through the host's decoder where there is one; without one the converter refuses with 'fmt?'.
    const auto aac_status = static_cast<std::uint32_t>(f.status(at, "_AudioConverterNew", {aac, out_format, converter_out}));
    if (alchemy::gui::completion_kit::media::codecs::audio_decoder_available(alchemy::gui::completion_kit::media::container::fourcc("aac "))) {
        require(aac_status == 0, "an AAC decoding converter");
        require(f.status(at, "_AudioConverterDispose", {f.engine.read64(converter_out)}) == 0, "AudioConverterDispose (AAC)");
    } else {
        require(aac_status == 0x666D743F, "no AAC decoder");
    }
    // AudioFormatGetProperty: FormatInfo completes a description; layouts count their channels.
    const auto partial = asbd(48000, 4 | 8, 0, 2, 24);
    f.engine.write32(partial + 16, 0);
    f.engine.write32(partial + 24, 0);
    f.engine.write32(size, 40);
    require(f.status(at, "_AudioFormatGetProperty", {0x666D7469 /* 'fmti' */, 40, partial, size, value}) == 0 &&
            f.engine.read32(value + 24) == 6 && f.engine.read32(value + 20) == 1, "FormatInfo: 24-bit stereo frames are 6 bytes");
    const auto layout_tag = f.block(4);
    f.engine.write32(layout_tag, (121U << 16) | 6);   // kAudioChannelLayoutTag_MPEG_5_1_A
    f.engine.write32(size, 0);
    require(f.status(at, "_AudioFormatGetPropertyInfo", {0x636D706C /* 'cmpl' */, 4, layout_tag, size}) == 0 && f.engine.read32(size) == 12 + 6 * 20,
            "ChannelLayoutForTag size");
    const auto layout = f.block(12 + 6 * 20);
    require(f.status(at, "_AudioFormatGetProperty", {0x636D706C, 4, layout_tag, size, layout}) == 0 && f.engine.read32(layout + 8) == 6 &&
            f.engine.read32(layout + 12 + 3 * 20) == 4, "5.1 A has the LFE fourth");
    f.engine.write32(size, 4);
    require(f.status(at, "_AudioFormatGetProperty", {0x6E63686D /* 'nchm' */, 12 + 6 * 20, layout, size, value}) == 0 && f.engine.read32(value) == 6,
            "NumberOfChannelsForLayout");
}

// AudioQueue output: float stereo 48 kHz buffers play on the fake device and come back to the callback.
void audio_queue(Fixture& f) {
    const auto format = f.block(40);
    f.engine.write64(format, std::bit_cast<std::uint64_t>(48000.0));
    f.engine.write32(format + 8, 0x6C70636D);
    f.engine.write32(format + 12, 1 | 8);
    f.engine.write32(format + 16, 8);
    f.engine.write32(format + 20, 1);
    f.engine.write32(format + 24, 8);
    f.engine.write32(format + 28, 2);
    f.engine.write32(format + 32, 32);
    static std::atomic<int> returned{0};
    const auto callback = f.process.install_function("_alchemy_test_queue_callback", [](transmute::CpuState&, transmute::GuestMemory&) { ++returned; },
                                                     "/usr/lib/alchemy_test_io.dylib");
    const auto queue_out = f.block(8), buffer_out = f.block(8);
    require(f.status(at, "_AudioQueueNewOutput", {format, callback, 0, 0, 0, 0, queue_out}) == 0, "AudioQueueNewOutput");
    const auto queue = f.engine.read64(queue_out);
    require(f.status(at, "_AudioQueueAllocateBuffer", {queue, 512 * 8, buffer_out}) == 0, "AudioQueueAllocateBuffer");
    const auto buffer = f.engine.read64(buffer_out);
    const auto data = f.engine.read64(buffer + 8);
    for (std::uint32_t i = 0; i < 512 * 2; ++i) f.engine.write32(data + 4ULL * i, std::bit_cast<std::uint32_t>(0.75f));
    f.engine.write32(buffer + 16, 512 * 8);
    require(f.status(at, "_AudioQueueEnqueueBuffer", {queue, buffer, 0, 0}) == 0, "AudioQueueEnqueueBuffer");
    require(f.status(at, "_AudioQueueStart", {queue, 0}) == 0, "AudioQueueStart");
    const auto played = FakeStream::play_until_sound(256);
    require(played[0] == 0.75f && played[1] == 0.75f, "the queued buffer plays");
    for (int i = 0; i < 4 && returned == 0; ++i) static_cast<void>(FakeStream::play_paced(256));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (returned == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    require(returned == 1, "the played buffer comes back to the callback");
    require(f.status(at, "_AudioQueueStop", {queue, 1}) == 0 && f.status(at, "_AudioQueueDispose", {queue, 1}) == 0, "stopped and disposed");

    // Offline rendering: 1024 frames of 0.5 rendered as 16-bit stereo, 512 at a time; the buffer
    // comes back to the callback inside the call that finishes it, and then there is silence.
    returned = 0;
    require(f.status(at, "_AudioQueueNewOutput", {format, callback, 0, 0, 0, 0, queue_out}) == 0, "an offline queue");
    const auto offline = f.engine.read64(queue_out);
    const auto out_buffer = f.block(8);
    require(f.status(at, "_AudioQueueAllocateBuffer", {offline, 1024 * 8, buffer_out}) == 0 &&
            f.status(at, "_AudioQueueAllocateBuffer", {offline, 512 * 4, out_buffer}) == 0, "source and render buffers");
    const auto source = f.engine.read64(buffer_out), rendered = f.engine.read64(out_buffer);
    for (std::uint32_t i = 0; i < 1024 * 2; ++i) f.engine.write32(f.engine.read64(source + 8) + 4ULL * i, std::bit_cast<std::uint32_t>(0.5f));
    f.engine.write32(source + 16, 1024 * 8);
    const auto stamp = f.block(64);
    require(f.status(at, "_AudioQueueOfflineRender", {offline, stamp, rendered, 512}) == -66626, "not offline yet");
    const auto s16 = f.block(40);
    f.engine.write64(s16, std::bit_cast<std::uint64_t>(48000.0));
    f.engine.write32(s16 + 8, 0x6C70636D);
    f.engine.write32(s16 + 12, 4 | 8);   // signed integer, packed
    f.engine.write32(s16 + 16, 4); f.engine.write32(s16 + 20, 1); f.engine.write32(s16 + 24, 4);
    f.engine.write32(s16 + 28, 2); f.engine.write32(s16 + 32, 16);
    require(f.status(at, "_AudioQueueSetOfflineRenderFormat", {offline, s16, 0}) == 0, "AudioQueueSetOfflineRenderFormat");
    require(f.status(at, "_AudioQueueEnqueueBuffer", {offline, source, 0, 0}) == 0 && f.status(at, "_AudioQueueStart", {offline, 0}) == 0,
            "enqueued and started (no device)");
    require(f.status(at, "_AudioQueueOfflineRender", {offline, stamp, rendered, 0}) == 0 && f.engine.read32(rendered + 16) == 0, "priming");
    const auto rendered_data = f.engine.read64(rendered + 8);
    const auto sample = [&](std::uint32_t index) { return static_cast<std::int16_t>(f.memory().read8(rendered_data + 2ULL * index) | (f.memory().read8(rendered_data + 2ULL * index + 1) << 8)); };
    require(f.status(at, "_AudioQueueOfflineRender", {offline, stamp, rendered, 512}) == 0 && f.engine.read32(rendered + 16) == 512 * 4 &&
            std::abs(sample(0) - 16384) <= 1 && std::abs(sample(1023) - 16384) <= 1 && returned == 0, "the first 512 frames");
    require(f.status(at, "_AudioQueueOfflineRender", {offline, stamp, rendered, 512}) == 0 && std::abs(sample(1023) - 16384) <= 1 && returned == 1,
            "the last 512 frames; the buffer is back");
    require(f.status(at, "_AudioQueueOfflineRender", {offline, stamp, rendered, 512}) == 0 && sample(0) == 0 && sample(1023) == 0, "then silence");
    require(f.status(at, "_AudioQueueSetOfflineRenderFormat", {offline, 0, 0}) == -66678, "not while running");
    require(f.status(at, "_AudioQueueStop", {offline, 1}) == 0 && f.status(at, "_AudioQueueDispose", {offline, 1}) == 0, "offline queue disposed");
}

// OpenAL over the fake device: a constant mono buffer at the listener plays centered (equal power).
void openal(Fixture& f) {
    constexpr std::string_view al = "/System/Library/Frameworks/OpenAL.framework/Versions/A/OpenAL";
    const auto device = f.call(al, "_alcOpenDevice", {0});
    require(device != 0, "alcOpenDevice");
    const auto context = f.call(al, "_alcCreateContext", {device, 0});
    require(context != 0 && f.call(al, "_alcMakeContextCurrent", {context}) == 1, "a current context");
    const auto names = f.block(8);
    f.call(al, "_alGenBuffers", {1, names});
    f.call(al, "_alGenSources", {1, names + 4});
    const auto buffer = f.engine.read32(names), source = f.engine.read32(names + 4);
    const auto data = f.block(48000 * 2);
    for (std::uint32_t i = 0; i < 48000; ++i) {
        f.memory().write8(data + 2ULL * i, 0x00);
        f.memory().write8(data + 2ULL * i + 1, 0x40);   // 16384: 0.5
    }
    f.call(al, "_alBufferData", {buffer, 0x1101 /* AL_FORMAT_MONO16 */, data, 48000 * 2, 48000});
    require(f.call(al, "_alGetError") == 0, "alBufferData");
    f.call(al, "_alSourcei", {source, 0x1009 /* AL_BUFFER */, buffer});
    f.call(al, "_alSourcei", {source, 0x1007 /* AL_LOOPING */, 1});
    f.call(al, "_alSourcePlay", {source});
    const auto state = f.block(4);
    f.call(al, "_alGetSourcei", {source, 0x1010, state});
    require(f.engine.read32(state) == 0x1012, "AL_PLAYING");
    const auto played = FakeStream::play_until_sound(256);
    require(std::fabs(played[100] - 0.5f * 0.70710678f) < 0.01f && std::fabs(played[101] - played[100]) < 0.001f, "centered, equal power");
    // Moved to the right of the listener (+x): the right channel is louder.
    alchemy::CallRegs position{};
    position.x[0] = source; position.x[1] = 0x1004;   // AL_POSITION
    position.set_s(0, 1.0f); position.set_s(1, 0.0f); position.set_s(2, 0.0f);
    static_cast<void>(f.regs(al, "_alSource3f", position));
    std::vector<float> after;
    for (int i = 0; i < 6; ++i) after = FakeStream::play_paced(256);
    require(after[201] > after[200] * 4, "panned right");
    f.call(al, "_alSourceStop", {source});
    f.call(al, "_alGetSourcei", {source, 0x1010, state});
    require(f.engine.read32(state) == 0x1014, "AL_STOPPED");
    f.call(al, "_alDeleteSources", {1, names + 4});
    f.call(al, "_alDeleteBuffers", {1, names});
    require(f.call(al, "_alGetError") == 0, "no AL error");
    f.call(al, "_alcMakeContextCurrent", {0});
    f.call(al, "_alcDestroyContext", {context});
    require(f.call(al, "_alcCloseDevice", {device}) == 1, "alcCloseDevice");
}

// A 16-bit WAVE file of `frames` frames of `channels` channels, sample(frame, channel).
template <class Sample>
std::filesystem::path write_wave(const std::filesystem::path& path, std::uint32_t rate, std::uint16_t channels, std::uint32_t frames, Sample sample) {
    std::vector<std::uint8_t> file;
    const auto le = [&](std::uint32_t value, int bytes) { for (int i = 0; i < bytes; ++i) file.push_back(static_cast<std::uint8_t>(value >> (8 * i))); };
    const auto tag = [&](const char* text) { file.insert(file.end(), text, text + 4); };
    const std::uint32_t bytes = frames * channels * 2;
    tag("RIFF"); le(36 + bytes, 4); tag("WAVE");
    tag("fmt "); le(16, 4); le(1, 2); le(channels, 2); le(rate, 4); le(rate * channels * 2, 4); le(channels * 2U, 2); le(16, 2);
    tag("data"); le(bytes, 4);
    for (std::uint32_t i = 0; i < frames; ++i)
        for (std::uint16_t c = 0; c < channels; ++c) le(static_cast<std::uint16_t>(sample(i, c)), 2);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    return path;
}

// AVFAudio: AVAudioTime, AVAudioPCMBuffer, AVAudioFile, AVAudioEngine with a player node and a
// tap on the main mixer, and AVAudioPlayer, over the fake device.
void avaudio(Fixture& f) {
    const auto b = alchemy::gui::security::bridge();
    const auto real = [](const alchemy::CallResult& r) { return r.d(0); };
    // AVAudioTime: a sample time extrapolated from an anchor with both times.
    alchemy::CallRegs sample_time{};
    sample_time.x[2] = 1000;
    sample_time.set_d(0, 48000);
    const auto time = f.objc.send(f.cls("AVAudioTime"), f.objc.selector("timeWithSampleTime:atRate:"), sample_time).x0;
    require(time && f.send(time, "sampleTime") == 1000 && (f.send(time, "isHostTimeValid") & 0xff) == 0, "AVAudioTime sample time");
    alchemy::CallRegs anchor_time{};
    anchor_time.x[2] = 1'000'000'000;
    anchor_time.x[3] = 0;
    anchor_time.set_d(0, 48000);
    const auto anchor = f.objc.send(f.cls("AVAudioTime"), f.objc.selector("timeWithHostTime:sampleTime:atRate:"), anchor_time).x0;
    const auto full = f.send(time, "extrapolateTimeFromAnchor:", {anchor});
    require(full && f.send(full, "hostTime") == 1'000'000'000 + 1000 * 1'000'000'000ULL / 48000, "extrapolated host time");

    // AVAudioPCMBuffer: deinterleaved float32 stereo at 48 kHz.
    alchemy::CallRegs standard{};
    standard.x[2] = 2;
    standard.set_d(0, 48000);
    const auto format = f.objc.send(f.send(f.cls("AVAudioFormat"), "alloc"), f.objc.selector("initStandardFormatWithSampleRate:channels:"), standard).x0;
    require(format != 0, "a standard format");
    const auto buffer = f.send(f.send(f.cls("AVAudioPCMBuffer"), "alloc"), "initWithPCMFormat:frameCapacity:", {format, 4800});
    require(buffer && f.send(buffer, "frameCapacity") == 4800 && f.send(buffer, "frameLength") == 0 && f.send(buffer, "stride") == 1, "AVAudioPCMBuffer");
    require(f.send(buffer, "int16ChannelData") == 0, "no int16 data in a float buffer");
    const auto channels = f.send(buffer, "floatChannelData");
    const auto left = f.engine.read64(channels), right = f.engine.read64(channels + 8);
    for (std::uint32_t i = 0; i < 4800; ++i) {
        f.engine.write32(left + 4ULL * i, std::bit_cast<std::uint32_t>(0.5f));
        f.engine.write32(right + 4ULL * i, std::bit_cast<std::uint32_t>(-0.25f));
    }
    f.send(buffer, "setFrameLength:", {4800});
    const auto list = f.send(buffer, "audioBufferList");
    require(f.engine.read32(list) == 2 && f.engine.read32(list + 12) == 4800 * 4 && f.engine.read64(list + 32) == right, "the buffer list");

    // AVAudioEngine: player -> main mixer -> output, a tap on the mixer.
    const auto engine = f.send(f.send(f.cls("AVAudioEngine"), "alloc"), "init");
    const auto player = f.send(f.send(f.cls("AVAudioPlayerNode"), "alloc"), "init");
    f.send(engine, "attachNode:", {player});
    const auto mixer = f.send(engine, "mainMixerNode");
    require(mixer && f.send(mixer, "engine") == engine, "mainMixerNode");
    f.send(engine, "connect:to:format:", {player, mixer, format});
    require(f.send(player, "outputFormatForBus:", {0}) == format, "the connection format");
    const auto hardware = f.send(f.send(engine, "outputNode"), "outputFormatForBus:", {0});
    require(real(f.objc.send(hardware, f.objc.selector("sampleRate"))) == 48000, "the output node: the device's rate");
    static std::atomic<int> completed{0}, tapped{0};
    const auto done = b->make_block([](transmute::CpuState&, transmute::GuestMemory&) { ++completed; });
    const auto tap = b->make_block([](transmute::CpuState& s, transmute::GuestMemory&) {
        if (s.x[1] && s.x[2]) ++tapped;   // (AVAudioPCMBuffer *, AVAudioTime *)
    });
    alchemy::CallRegs tap_args{};
    tap_args.x = {0, 0, 0, 1024, 0, tap, 0, 0};
    static_cast<void>(f.objc.send(mixer, f.objc.selector("installTapOnBus:bufferSize:format:block:"), tap_args));
    const auto error = f.block(8);
    require((f.send(engine, "startAndReturnError:", {error}) & 0xff) == 1 && (f.send(engine, "isRunning") & 0xff) == 1, "the engine starts");
    f.send(player, "scheduleBuffer:completionHandler:", {buffer, done});
    f.send(player, "play");
    require((f.send(player, "isPlaying") & 0xff) == 1, "the player plays");
    const auto played = FakeStream::play_until_sound(256);
    const auto first = static_cast<std::size_t>(std::find_if(played.begin(), played.end(), [](float v) { return v != 0; }) - played.begin()) & ~std::size_t{1};
    require(first + 1 < played.size() && played[first] == 0.5f && played[first + 1] == -0.25f, "the buffer plays through the mixer");
    for (int i = 0; i < 24; ++i) static_cast<void>(FakeStream::play_paced(256));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((completed == 0 || tapped == 0) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    require(completed == 1, "the completion handler runs once the buffer is consumed");
    require(tapped >= 1, "the tap delivers buffers");
    require(f.send(player, "lastRenderTime") != 0, "lastRenderTime while running");
    // Volume applies in the mixer; a looping buffer keeps playing.
    alchemy::CallRegs half{};
    half.set_s(0, 0.5f);
    static_cast<void>(f.objc.send(player, f.objc.selector("setVolume:"), half));
    alchemy::CallRegs looping{};
    looping.x = {0, 0, buffer, 0, 1 /* AVAudioPlayerNodeBufferLoops */, 0, 0, 0};
    static_cast<void>(f.objc.send(player, f.objc.selector("scheduleBuffer:atTime:options:completionHandler:"), looping));
    std::vector<float> quieter;
    for (int i = 0; i < 40; ++i) quieter = FakeStream::play_paced(256);
    require(quieter[0] == 0.25f && quieter[1] == -0.125f, "volume 0.5, still looping");
    f.send(player, "stop");
    require((f.send(player, "isPlaying") & 0xff) == 0, "stopped");
    f.send(mixer, "removeTapOnBus:", {0});
    f.send(engine, "stop");
    require((f.send(engine, "isRunning") & 0xff) == 0, "the engine stops");
    // The input node has no device behind it: an engine fed by it does not start.
    const auto input = f.send(engine, "inputNode");
    require(input && f.send(input, "outputFormatForBus:", {0}) == 0, "the input node has no format");

    // AVAudioFile: a mono 16-bit file read as float32.
    const auto directory = std::filesystem::temp_directory_path() / "alchemy_media_avaudio";
    std::filesystem::create_directories(directory);
    const auto path = write_wave(directory / "ramp.wav", 44100, 1, 1000, [](std::uint32_t i, std::uint16_t) { return static_cast<std::int16_t>(i * 32); });
    const auto url = f.cf.file_url(alchemy::guest_path_string(path), false);
    const auto file = f.send(f.send(f.cls("AVAudioFile"), "alloc"), "initForReading:error:", {url, error});
    require(file && static_cast<std::int64_t>(f.send(file, "length")) == 1000, "AVAudioFile length");
    const auto processing = f.send(file, "processingFormat");
    require(f.send(processing, "channelCount") == 1 && f.send(processing, "commonFormat") == 1, "float32 processing format");
    const auto file_buffer = f.send(f.send(f.cls("AVAudioPCMBuffer"), "alloc"), "initWithPCMFormat:frameCapacity:", {processing, 512});
    require((f.send(file, "readIntoBuffer:error:", {file_buffer, error}) & 0xff) == 1 && f.send(file_buffer, "frameLength") == 512, "read 512 frames");
    const auto data = f.engine.read64(f.send(file_buffer, "floatChannelData"));
    require(std::bit_cast<float>(f.engine.read32(data + 400)) == 3200.0f / 32768, "the samples, converted");
    require(f.send(file, "framePosition") == 512, "framePosition");
    require((f.send(file, "readIntoBuffer:error:", {file_buffer, error}) & 0xff) == 1 && f.send(file_buffer, "frameLength") == 488, "the rest");
    require((f.send(file, "readIntoBuffer:error:", {file_buffer, error}) & 0xff) == 1 && f.send(file_buffer, "frameLength") == 0, "end of file");
    f.engine.write64(error, 0);
    require(f.send(f.send(f.cls("AVAudioFile"), "alloc"), "initForWriting:settings:error:", {url, 0, error}) == 0 && f.engine.read64(error) != 0,
            "writing needs settings");
    // Writing a CAF: 16-bit stereo at 48 kHz from the float buffer above, read back.
    {
        std::vector<std::pair<Address, Address>> entries{
            {f.cf.string("AVSampleRateKey"), f.cf.integer(48000, 3)}, {f.cf.string("AVNumberOfChannelsKey"), f.cf.integer(2, 3)},
            {f.cf.string("AVLinearPCMBitDepthKey"), f.cf.integer(16, 3)}, {f.cf.string("AVLinearPCMIsFloatKey"), f.cf.boolean(false)}};
        const auto out_url = f.cf.file_url(alchemy::guest_path_string(directory / "written.caf"), false);
        const auto writer = f.send(f.send(f.cls("AVAudioFile"), "alloc"), "initForWriting:settings:error:", {out_url, f.cf.dictionary(entries), error});
        require(writer != 0 && f.send(f.send(writer, "fileFormat"), "channelCount") == 2, "AVAudioFile for writing");
        require((f.send(writer, "writeFromBuffer:error:", {buffer, error}) & 0xff) == 1 && f.send(writer, "length") == 4800, "writeFromBuffer:");
        f.send(writer, "close");
        const auto reader = f.send(f.send(f.cls("AVAudioFile"), "alloc"), "initForReading:error:", {out_url, error});
        require(reader && f.send(reader, "length") == 4800, "the written file reads back");
        const auto back = f.send(f.send(f.cls("AVAudioPCMBuffer"), "alloc"), "initWithPCMFormat:frameCapacity:", {f.send(reader, "processingFormat"), 4800});
        require((f.send(reader, "readIntoBuffer:error:", {back, error}) & 0xff) == 1 && f.send(back, "frameLength") == 4800, "read 4800 frames");
        const auto planes = f.send(back, "floatChannelData");
        require(std::bit_cast<float>(f.engine.read32(f.engine.read64(planes) + 400)) == 0.5f &&
                std::bit_cast<float>(f.engine.read32(f.engine.read64(planes + 8) + 400)) == -0.25f, "the written samples");
        f.send(back, "release");
        f.send(reader, "release");
        f.send(writer, "release");
    }

    // AVAudioPlayer: a 1 s 44.1 kHz tone plays resampled to 48 kHz; currentTime advances; -stop.
    const auto tone_path = write_wave(directory / "tone.wav", 44100, 1, 44100, [](std::uint32_t, std::uint16_t) { return static_cast<std::int16_t>(8192); });
    const auto tone_url = f.cf.file_url(alchemy::guest_path_string(tone_path), false);
    f.engine.write64(error, 0);
    const auto audio_player = f.send(f.send(f.cls("AVAudioPlayer"), "alloc"), "initWithContentsOfURL:error:", {tone_url, error});
    require(audio_player && f.send(audio_player, "numberOfChannels") == 1, "AVAudioPlayer");
    require(std::fabs(real(f.objc.send(audio_player, f.objc.selector("duration"))) - 1.0) < 1e-9, "duration");
    require((f.send(audio_player, "play") & 0xff) == 1, "play");
    const auto heard = FakeStream::play_until_sound(256);
    require(std::any_of(heard.begin(), heard.end(), [](float v) { return v > 0; }), "the player sounds");
    require(real(f.objc.send(audio_player, f.objc.selector("currentTime"))) > 0, "currentTime advances");
    f.send(audio_player, "stop");
    require((f.send(audio_player, "isPlaying") & 0xff) == 0 && real(f.objc.send(audio_player, f.objc.selector("currentTime"))) > 0,
            "stopped, keeping currentTime");
    // The 1000-frame ramp plays to its end and stops by itself.
    const auto short_player = f.send(f.send(f.cls("AVAudioPlayer"), "alloc"), "initWithContentsOfURL:error:", {url, error});
    require(short_player && std::fabs(real(f.objc.send(short_player, f.objc.selector("duration"))) - 1000.0 / 44100) < 1e-9, "a short player");
    require((f.send(short_player, "play") & 0xff) == 1, "play the short file");
    for (int i = 0; i < 40 && (f.send(short_player, "isPlaying") & 0xff) == 1; ++i) static_cast<void>(FakeStream::play_paced(256));
    require((f.send(short_player, "isPlaying") & 0xff) == 0, "it finishes");
    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto from_data = f.send(f.send(f.cls("AVAudioPlayer"), "alloc"), "initWithData:error:", {b->data(bytes), error});
    require(from_data && std::fabs(real(f.objc.send(from_data, f.objc.selector("duration"))) - 1000.0 / 44100) < 1e-9, "AVAudioPlayer from data");
    f.send(audio_player, "release");
    f.send(short_player, "release");
    f.send(from_data, "release");
    f.send(file_buffer, "release");
    f.send(file, "release");
    f.send(engine, "release");
    f.send(player, "release");
    f.send(buffer, "release");
    f.send(format, "release");
    in.close();
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// AVAudioConverter (PCM layouts and channels, sample rates through the input block, MP3 packets in
// an AVAudioCompressedBuffer), AVAudioSourceNode and AVAudioSinkNode, and manual rendering (offline,
// the realtime block, the input node's block).
void avaudio_conversion(Fixture& f) {
    const auto b = alchemy::gui::security::bridge();
    const auto format_of = [&](std::uint64_t common, double rate, std::uint32_t channels, bool interleaved) {
        alchemy::CallRegs r{};
        r.x[2] = common; r.x[3] = channels; r.x[4] = interleaved;
        r.set_d(0, rate);
        return f.objc.send(f.send(f.cls("AVAudioFormat"), "alloc"), f.objc.selector("initWithCommonFormat:sampleRate:channels:interleaved:"), r).x0;
    };
    const auto pcm_buffer = [&](Address format, std::uint32_t capacity) {
        return f.send(f.send(f.cls("AVAudioPCMBuffer"), "alloc"), "initWithPCMFormat:frameCapacity:", {format, capacity});
    };
    const auto float_at = [&](Address channel_data, std::uint32_t channel, std::uint32_t frame) {
        return std::bit_cast<float>(f.engine.read32(f.engine.read64(channel_data + 8ULL * channel) + 4ULL * frame));
    };

    // Float32 stereo (0.5, -0.25) into int16 mono: the first channel, or both averaged with downmix.
    const auto stereo = format_of(1, 48000, 2, false), mono16 = format_of(3, 48000, 1, true);
    const auto source = pcm_buffer(stereo, 256), target = pcm_buffer(mono16, 256);
    for (std::uint32_t i = 0; i < 256; ++i) {
        f.engine.write32(f.engine.read64(f.send(source, "floatChannelData")) + 4ULL * i, std::bit_cast<std::uint32_t>(0.5f));
        f.engine.write32(f.engine.read64(f.send(source, "floatChannelData") + 8) + 4ULL * i, std::bit_cast<std::uint32_t>(-0.25f));
    }
    f.send(source, "setFrameLength:", {256});
    const auto converter = f.send(f.send(f.cls("AVAudioConverter"), "alloc"), "initFromFormat:toFormat:", {stereo, mono16});
    require(converter && f.send(converter, "outputFormat") == mono16, "an AVAudioConverter");
    const auto error = f.block(8);
    require((f.send(converter, "convertToBuffer:fromBuffer:error:", {target, source, error}) & 0xff) == 1 && f.send(target, "frameLength") == 256,
            "PCM to PCM");
    const auto first_sample = [&] { return static_cast<std::int16_t>(f.engine.read32(f.engine.read64(f.send(target, "int16ChannelData"))) & 0xffff); };
    require(std::abs(first_sample() - 16384) <= 1, "the first channel: " + std::to_string(first_sample()));
    f.send(converter, "setDownmix:", {1});
    f.send(converter, "convertToBuffer:fromBuffer:error:", {target, source, error});
    require(std::abs(first_sample() - 4096) <= 1, "downmixed: " + std::to_string(first_sample()));

    // 44.1 kHz to 48 kHz through the input block: 4410 frames of 0.5 become about 4800.
    const auto in441 = format_of(1, 44100, 1, false), out48 = format_of(1, 48000, 1, false);
    const auto resampling = f.send(f.send(f.cls("AVAudioConverter"), "alloc"), "initFromFormat:toFormat:", {in441, out48});
    const auto input = pcm_buffer(in441, 4410);
    for (std::uint32_t i = 0; i < 4410; ++i) f.engine.write32(f.engine.read64(f.send(input, "floatChannelData")) + 4ULL * i, std::bit_cast<std::uint32_t>(0.5f));
    f.send(input, "setFrameLength:", {4410});
    static std::atomic<int> given{0};
    given = 0;
    const auto feed = b->make_block([input](transmute::CpuState& s, transmute::GuestMemory& m) {
        // AVAudioBuffer * (^)(AVAudioPacketCount, AVAudioConverterInputStatus *)
        if (given++ == 0) { m.write64(s.x[2], 0); s.x[0] = input; return; }
        m.write64(s.x[2], 2);   // EndOfStream
        s.x[0] = 0;
    });
    const auto output = pcm_buffer(out48, 1024);
    std::uint64_t total = 0;
    float middle = 0;
    for (int round = 0; round < 16; ++round) {
        const auto status = f.send(resampling, "convertToBuffer:error:withInputFromBlock:", {output, error, feed});
        require(status <= 2, "the conversion succeeds: " + std::to_string(status));
        const auto frames = f.send(output, "frameLength");
        if (total < 2400 && total + frames > 2400) middle = float_at(f.send(output, "floatChannelData"), 0, static_cast<std::uint32_t>(2400 - total));
        total += frames;
        if (status == 2) break;
    }
    require(total >= 4798 && total <= 4802 && std::fabs(middle - 0.5f) < 0.01f, "resampled: " + std::to_string(total) + " frames, " + std::to_string(middle));

    // MP3 packets from the file through an AVAudioCompressedBuffer, decoded like the reference.
    const std::string vectors = std::string(ALCHEMY_THIRD_PARTY) + "/minimp3/vectors/";
    const auto directory = std::filesystem::temp_directory_path() / "alchemy_qlab_media_converter";
    std::filesystem::create_directories(directory);
    const auto path = directory / "hecommon.mp3";
    std::filesystem::copy_file(vectors + "l3-hecommon.bit", path, std::filesystem::copy_options::overwrite_existing);
    std::vector<std::int16_t> reference;
    {
        std::ifstream in(vectors + "l3-hecommon.pcm", std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
        reference.resize(bytes.size() / 2);
        std::memcpy(reference.data(), bytes.data(), reference.size() * 2);
    }
    const auto out = f.block(8), size = f.block(4), asbd = f.block(40);
    require(f.status(at, "_AudioFileOpenURL", {f.cf.file_url(alchemy::guest_path_string(path), false), 1, 0, out}) == 0, "the MP3 file opens");
    const auto file = f.engine.read64(out);
    f.engine.write32(size, 40);
    require(f.status(at, "_AudioFileGetProperty", {file, 0x64666d74, size, asbd}) == 0, "its data format");
    const auto mp3 = f.send(f.send(f.cls("AVAudioFormat"), "alloc"), "initWithStreamDescription:", {asbd});
    const auto channels = f.engine.read32(asbd + 28);
    const auto rate = std::bit_cast<double>(f.engine.read64(asbd));
    const auto pcm = format_of(1, rate, channels, true);
    const auto decoding = f.send(f.send(f.cls("AVAudioConverter"), "alloc"), "initFromFormat:toFormat:", {mp3, pcm});
    require(decoding != 0, "an MP3 decoding converter");
    const auto packets = f.send(f.send(f.cls("AVAudioCompressedBuffer"), "alloc"), "initWithFormat:packetCapacity:maximumPacketSize:", {mp3, 8, 1500});
    require(packets && f.send(packets, "packetDescriptions") != 0 && f.send(packets, "byteCapacity") == 8 * 1500, "an AVAudioCompressedBuffer");
    const auto room = f.block(4), count = f.block(4);
    f.engine.write32(room, 8 * 1500);
    f.engine.write32(count, 8);
    require(f.status(at, "_AudioFileReadPacketData", {file, 0, room, f.send(packets, "packetDescriptions"), 0, count, f.send(packets, "data")}) == 0,
            "eight packets");
    f.send(packets, "setPacketCount:", {f.engine.read32(count)});
    f.send(packets, "setByteLength:", {f.engine.read32(room)});
    static std::atomic<int> packet_calls{0};
    packet_calls = 0;
    const auto feed_packets = b->make_block([packets](transmute::CpuState& s, transmute::GuestMemory& m) {
        if (packet_calls++ == 0) { m.write64(s.x[2], 0); s.x[0] = packets; return; }
        m.write64(s.x[2], 2);
        s.x[0] = 0;
    });
    const auto decoded = pcm_buffer(pcm, 8 * 1152);
    require(f.send(decoding, "convertToBuffer:error:withInputFromBlock:", {decoded, error, feed_packets}) == 0 &&
            f.send(decoded, "frameLength") == 8 * 1152, "eight MPEG frames decode");
    const auto samples = f.engine.read64(f.send(decoded, "floatChannelData"));
    bool same = true;
    for (std::size_t i = 0; i < 8 * 1152 * channels && i < reference.size(); ++i)
        same = same && std::fabs(std::bit_cast<float>(f.engine.read32(samples + 4 * i)) * 32768.0f - reference[i]) <= 2.0f;
    require(same, "the decoded frames match the reference");
    static_cast<void>(f.call(at, "_AudioFileClose", {file}));

    // Offline manual rendering: a mono source node of 0.25 into the main mixer, an input block of
    // 0.5 through the input node, and a sink node fed by a second source.
    const auto engine = f.send(f.send(f.cls("AVAudioEngine"), "alloc"), "init");
    const auto stereo48 = format_of(1, 48000, 2, false), mono48 = format_of(1, 48000, 1, false);
    alchemy::CallRegs manual{};
    manual.x = {0, 0, 0, stereo48, 4096, error, 0, 0};   // offline
    require((f.objc.send(engine, f.objc.selector("enableManualRenderingMode:format:maximumFrameCount:error:"), manual).x0 & 0xff) == 1 &&
            (f.send(engine, "isInManualRenderingMode") & 0xff) == 1 && f.send(engine, "manualRenderingMaximumFrameCount") == 4096, "manual rendering");
    const auto constant = [&](float value) {
        // OSStatus (^)(BOOL *isSilence, const AudioTimeStamp *, AVAudioFrameCount, AudioBufferList *)
        return b->make_block([value](transmute::CpuState& s, transmute::GuestMemory& m) {
            const auto frames = static_cast<std::uint32_t>(s.x[3]);
            const auto list = s.x[4];
            for (std::uint32_t i = 0; i < m.read32(list); ++i) {
                const auto data = m.read64(list + 8 + 16ULL * i + 8);
                for (std::uint32_t frame = 0; frame < frames; ++frame) m.write32(data + 4ULL * frame, std::bit_cast<std::uint32_t>(value));
            }
            s.x[0] = 0;
        });
    };
    const auto tone = f.send(f.send(f.cls("AVAudioSourceNode"), "alloc"), "initWithFormat:renderBlock:", {mono48, constant(0.25f)});
    const auto feeder = f.send(f.send(f.cls("AVAudioSourceNode"), "alloc"), "initWithRenderBlock:", {constant(0.125f)});
    static std::atomic<std::uint64_t> sunk{0};
    static std::atomic<std::uint32_t> sunk_bits{0};
    sunk = 0;
    const auto sink = f.send(f.send(f.cls("AVAudioSinkNode"), "alloc"), "initWithReceiverBlock:", {b->make_block([](transmute::CpuState& s, transmute::GuestMemory& m) {
        // OSStatus (^)(const AudioTimeStamp *, AVAudioFrameCount, const AudioBufferList *)
        sunk += s.x[2];
        sunk_bits = m.read32(m.read64(s.x[3] + 16));
        s.x[0] = 0;
    })});
    require(tone && feeder && sink, "source and sink nodes");
    const auto mixer = f.send(engine, "mainMixerNode");
    for (const auto node : {tone, feeder, sink}) f.send(engine, "attachNode:", {node});
    f.send(engine, "connect:to:format:", {tone, mixer, 0});
    f.send(engine, "connect:to:format:", {feeder, sink, mono48});
    const auto input_node = f.send(engine, "inputNode");
    const auto input_data = f.block(4 * 4096), input_list = f.block(24);
    for (std::uint32_t i = 0; i < 4096; ++i) f.engine.write32(input_data + 4ULL * i, std::bit_cast<std::uint32_t>(0.5f));
    f.engine.write32(input_list, 1);
    f.engine.write32(input_list + 8, 1);
    f.engine.write32(input_list + 12, 4 * 4096);
    f.engine.write64(input_list + 16, input_data);
    const auto give_input = b->make_block([input_list](transmute::CpuState& s, transmute::GuestMemory&) { s.x[0] = input_list; });
    require((f.send(input_node, "setManualRenderingInputPCMFormat:inputBlock:", {mono48, give_input}) & 0xff) == 1, "the manual input");
    f.send(engine, "connect:to:format:", {input_node, mixer, mono48});
    require((f.send(engine, "startAndReturnError:", {error}) & 0xff) == 1, "the engine starts with no device");
    const auto rendered = pcm_buffer(stereo48, 4096);
    alchemy::CallRegs offline{};
    offline.x = {0, 0, 1024, rendered, error, 0, 0, 0};
    const auto status = f.objc.send(engine, f.objc.selector("renderOffline:toBuffer:error:"), offline).x0;
    const auto data = f.send(rendered, "floatChannelData");
    require(status == 0 && f.send(rendered, "frameLength") == 1024 && std::fabs(float_at(data, 0, 100) - 0.75f) < 1e-5f &&
            std::fabs(float_at(data, 1, 1000) - 0.75f) < 1e-5f, "the source and the input mix: " + std::to_string(float_at(data, 0, 100)));
    require(sunk == 1024 && std::bit_cast<float>(sunk_bits.load()) == 0.125f, "the sink receives its input");
    require(f.send(engine, "manualRenderingSampleTime") == 1024, "the manual sample time");
    // The realtime-style block renders into an AudioBufferList of the manual format.
    const auto block = f.send(engine, "manualRenderingBlock");
    const auto os_status = f.block(4);
    f.send(rendered, "setFrameLength:", {512});
    require(block && b->invoke_block(block, {512, f.send(rendered, "mutableAudioBufferList"), os_status}) == 0 && f.engine.read32(os_status) == 0 &&
            std::fabs(float_at(data, 1, 10) - 0.75f) < 1e-5f, "manualRenderingBlock");
    f.send(engine, "stop");
    f.send(engine, "disableManualRenderingMode");
    require((f.send(engine, "isInManualRenderingMode") & 0xff) == 0, "manual rendering off");
}

// AVAudioUnit effects in an offline manual render: a constant mono source through an EQ's global
// gain, a dry delay, a reverb, a distortion, varispeed and time-pitch into the main mixer.
void avaudio_effects(Fixture& f) {
    const auto b = alchemy::gui::security::bridge();
    const auto format_of = [&](double rate, std::uint32_t channels) {
        alchemy::CallRegs r{};
        r.x[2] = 1; r.x[3] = channels; r.x[4] = 0;
        r.set_d(0, rate);
        return f.objc.send(f.send(f.cls("AVAudioFormat"), "alloc"), f.objc.selector("initWithCommonFormat:sampleRate:channels:interleaved:"), r).x0;
    };
    const auto mono = format_of(48000, 1), stereo = format_of(48000, 2);
    static std::atomic<std::uint64_t> asked{0};
    const auto constant = b->make_block([](transmute::CpuState& s, transmute::GuestMemory& m) {
        const auto frames = static_cast<std::uint32_t>(s.x[3]);
        asked += frames;
        const auto data = m.read64(s.x[4] + 16);
        for (std::uint32_t i = 0; i < frames; ++i) m.write32(data + 4ULL * i, std::bit_cast<std::uint32_t>(0.25f));
        s.x[0] = 0;
    });
    const auto error = f.block(8);
    // Renders `frames` through `effect` (source -> effect -> mixer); the first left sample, and how
    // many frames the source was asked for.
    const auto run = [&](Address effect, std::uint32_t frames, std::uint32_t at) {
        const auto engine = f.send(f.send(f.cls("AVAudioEngine"), "alloc"), "init");
        alchemy::CallRegs manual{};
        manual.x = {0, 0, 0, stereo, 4096, error, 0, 0};
        require((f.objc.send(engine, f.objc.selector("enableManualRenderingMode:format:maximumFrameCount:error:"), manual).x0 & 0xff) == 1, "manual rendering");
        const auto source = f.send(f.send(f.cls("AVAudioSourceNode"), "alloc"), "initWithFormat:renderBlock:", {mono, constant});
        const auto mixer = f.send(engine, "mainMixerNode");
        f.send(engine, "attachNode:", {source});
        f.send(engine, "attachNode:", {effect});
        f.send(engine, "connect:to:format:", {source, effect, mono});
        f.send(engine, "connect:to:format:", {effect, mixer, mono});
        require((f.send(engine, "startAndReturnError:", {error}) & 0xff) == 1, "the engine starts");
        const auto buffer = f.send(f.send(f.cls("AVAudioPCMBuffer"), "alloc"), "initWithPCMFormat:frameCapacity:", {stereo, 4096});
        asked = 0;
        alchemy::CallRegs offline{};
        offline.x = {0, 0, frames, buffer, error, 0, 0, 0};
        require(f.objc.send(engine, f.objc.selector("renderOffline:toBuffer:error:"), offline).x0 == 0, "an offline render");
        const float value = std::bit_cast<float>(f.engine.read32(f.engine.read64(f.send(buffer, "floatChannelData")) + 4ULL * at));
        f.send(engine, "stop");
        return std::pair<float, std::uint64_t>{value, asked.load()};
    };
    const auto make = [&](const char* name) { return f.send(f.send(f.cls(name), "alloc"), "init"); };
    const auto set_float = [&](Address object, const char* selector, float value) {
        alchemy::CallRegs r{};
        r.set_s(0, value);
        static_cast<void>(f.objc.send(object, f.objc.selector(selector), r));
    };
    // EQ: +6.0206 dB of global gain doubles 0.25; a 0 dB parametric band changes nothing.
    const auto eq = f.send(f.send(f.cls("AVAudioUnitEQ"), "alloc"), "initWithNumberOfBands:", {2});
    require(eq && f.cf.length(f.send(eq, "bands")) == 2, "an EQ with two bands");
    const auto band = f.cf.element(f.send(eq, "bands"), 0);
    f.send(band, "setFilterType:", {0});
    require(f.send(band, "filterType") == 0, "the band's filter type");
    set_float(eq, "setGlobalGain:", 6.0206f);
    const auto [doubled, eq_asked] = run(eq, 1024, 500);
    require(std::fabs(doubled - 0.5f) < 1e-3f && eq_asked == 1024, "EQ global gain: " + std::to_string(doubled));
    // Delay all dry: unchanged.
    const auto delay = make("AVAudioUnitDelay");
    set_float(delay, "setWetDryMix:", 0);
    require(std::fabs(run(delay, 1024, 500).first - 0.25f) < 1e-6f, "a dry delay");
    // Distortion: a soft clipper keeps a positive input positive and below 1.
    const auto distortion = make("AVAudioUnitDistortion");
    f.send(distortion, "loadFactoryPreset:", {3});
    const auto clipped = run(distortion, 1024, 500).first;
    require(clipped > 0.25f && clipped < 1.0f, "distortion: " + std::to_string(clipped));
    // Reverb: a wet signal builds up from the constant input.
    const auto reverb = make("AVAudioUnitReverb");
    f.send(reverb, "loadFactoryPreset:", {4});
    const auto wet = run(reverb, 4096, 4000).first;
    require(std::isfinite(wet) && wet != 0.0f, "reverb: " + std::to_string(wet));
    // Varispeed at 2: the source is asked for about twice the frames, and the level holds.
    const auto varispeed = make("AVAudioUnitVarispeed");
    set_float(varispeed, "setRate:", 2.0f);
    const auto [fast, fast_asked] = run(varispeed, 1024, 900);
    require(fast_asked >= 2000 && fast_asked <= 2200 && std::fabs(fast - 0.25f) < 0.01f,
            "varispeed: " + std::to_string(fast_asked) + " frames, " + std::to_string(fast));
    // Time-pitch an octave up at the same tempo: about as many input frames as output ones, the level held.
    const auto time_pitch = make("AVAudioUnitTimePitch");
    set_float(time_pitch, "setPitch:", 1200.0f);
    const auto [shifted, shifted_asked] = run(time_pitch, 4096, 3000);
    require(shifted_asked >= 4096 && shifted_asked <= 4096 * 2 + 8192 && std::fabs(shifted - 0.25f) < 0.02f,
            "time-pitch: " + std::to_string(shifted_asked) + " frames, " + std::to_string(shifted));
    alchemy::CallRegs bypass{};
    bypass.x[2] = 1;
    static_cast<void>(f.objc.send(time_pitch, f.objc.selector("setBypass:"), bypass));
    require((f.send(time_pitch, "bypass") & 0xff) == 1, "bypass");
}

// AVSpeechSynthesizer over the host's voices: the voice list matches host::speech's; with a voice,
// writeUtterance hands over speech then an empty buffer, and speakUtterance plays it on the fake
// device with didStart, willSpeakRange and didFinish, and a stop cancels; without one, an
// utterance is cancelled.
void speech(Fixture& f) {
    constexpr std::string_view run_loop = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
    const auto mode = f.engine.read64(f.symbol(run_loop, "_kCFRunLoopDefaultMode"));
    const auto run = [&](double seconds) {
        alchemy::CallRegs r;
        r.x[0] = mode;
        r.set_d(0, seconds);
        static_cast<void>(f.regs(run_loop, "_CFRunLoopRunInMode", r));
    };
    static std::atomic<int> started{0}, finished{0}, cancelled{0}, words{0};
    started = finished = cancelled = words = 0;
    const auto delegate_class = f.objc.register_native_class("AlchemySpeechTestDelegate", f.cls("NSObject"), 8);
    const auto count = [&](const char* selector, std::atomic<int>& counter) {
        static_cast<void>(f.objc.add_native_method(delegate_class, selector, [&counter](transmute::CpuState&, transmute::GuestMemory&) { ++counter; }, false, "v@:@@"));
    };
    count("speechSynthesizer:didStartSpeechUtterance:", started);
    count("speechSynthesizer:didFinishSpeechUtterance:", finished);
    count("speechSynthesizer:didCancelSpeechUtterance:", cancelled);
    static_cast<void>(f.objc.add_native_method(delegate_class, "speechSynthesizer:willSpeakRangeOfSpeechString:utterance:",
                                               [](transmute::CpuState&, transmute::GuestMemory&) { ++words; }, false, "v@:@{_NSRange=QQ}@"));
    const auto delegate = f.send(f.send(delegate_class, "alloc"), "init");
    const auto host_voices = alchemy::host::speech::voices();
    const auto listed = f.send(f.cls("AVSpeechSynthesisVoice"), "speechVoices");
    const auto voice_count = host_voices ? host_voices->size() : 0;
    require(f.cf.length(listed) == voice_count, "the voices are the host's: " + std::to_string(voice_count));
    const auto synthesizer = f.send(f.send(f.cls("AVSpeechSynthesizer"), "alloc"), "init");
    f.send(synthesizer, "setDelegate:", {delegate});
    const auto utterance = [&](const char* text) { return f.send(f.cls("AVSpeechUtterance"), "speechUtteranceWithString:", {f.cf.string(text)}); };
    const auto wait = [&](const std::atomic<int>& counter, double seconds, bool play) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        std::vector<float> heard;
        while (counter == 0 && std::chrono::steady_clock::now() < deadline) {
            if (play) {
                const auto out = FakeStream::play_paced(1024);
                heard.insert(heard.end(), out.begin(), out.end());
            }
            run(0.01);
        }
        return heard;
    };
    if (voice_count == 0) {
        f.send(synthesizer, "speakUtterance:", {utterance("hello")});
        static_cast<void>(wait(cancelled, 5, false));
        require(cancelled == 1 && started == 0, "without voices an utterance is cancelled");
        return;
    }
    const auto first = f.cf.element(listed, 0);
    require(!f.cf.utf8(f.send(first, "identifier")).empty() && !f.cf.utf8(f.send(first, "name")).empty(), "a voice's identifier and name");
    require(f.send(f.cls("AVSpeechSynthesisVoice"), "voiceWithIdentifier:", {f.send(first, "identifier")}) != 0, "voiceWithIdentifier:");
    // writeUtterance: buffers of speech, then an empty one.
    static std::atomic<std::uint64_t> written{0};
    static std::atomic<int> ended{0};
    static std::atomic<bool> voiced{false};
    written = 0; ended = 0; voiced = false;
    const auto b = alchemy::gui::security::bridge();
    // (AVAudioBuffer *): count the frames; any sample above the noise floor is speech.
    const auto collect = b->make_block([&f](transmute::CpuState& s, transmute::GuestMemory& m) {
        const auto buffer = s.x[1];
        const auto frames = f.send(buffer, "frameLength");
        if (frames == 0) { ++ended; return; }
        written += frames;
        const auto data = m.read64(f.send(buffer, "floatChannelData"));
        for (std::uint64_t i = 0; i < frames; ++i)
            if (std::fabs(std::bit_cast<float>(m.read32(data + 4 * i))) > 0.01f) { voiced = true; break; }
    });
    f.send(synthesizer, "writeUtterance:toBufferCallback:", {utterance("hello world"), collect});
    static_cast<void>(wait(ended, 15, false));
    require(ended == 1 && written > 2205 && voiced, "writeUtterance: " + std::to_string(written.load()) + " frames");
    // Speaking: didStart, word ranges, didFinish, and sound on the device.
    f.send(synthesizer, "speakUtterance:", {utterance("hello world")});
    const auto heard = wait(finished, 20, true);
    require(started == 1 && finished == 1 && words >= 1, "the delegate hears start, " + std::to_string(words.load()) + " words and finish");
    require(std::any_of(heard.begin(), heard.end(), [](float v) { return std::fabs(v) > 0.01f; }), "speech is heard on the device");
    // A stop at once cancels the utterance and drops the queue.
    f.send(synthesizer, "speakUtterance:", {utterance("this is a much longer sentence that is still being spoken when it is stopped")});
    f.send(synthesizer, "speakUtterance:", {utterance("never spoken")});
    for (int i = 0; i < 600 && started < 2; ++i) { static_cast<void>(FakeStream::play_paced(256)); run(0.01); }
    require((f.send(synthesizer, "stopSpeakingAtBoundary:", {0}) & 0xff) == 1, "stopSpeakingAtBoundary:");
    for (int i = 0; i < 50 && cancelled == 0; ++i) run(0.01);
    require(cancelled == 1 && (f.send(synthesizer, "isSpeaking") & 0xff) == 0 && started == 2, "a stop cancels and drops the queue");
}

// A SoundFont 2 file of one preset (bank 0, program 0) playing one looped 440 Hz sine sample
// (root key 69), written from the 2.04 specification's chunk layout.
std::vector<std::uint8_t> sine_sound_font() {
    std::vector<std::uint8_t> out;
    const auto u16 = [](std::vector<std::uint8_t>& b, unsigned v) { b.push_back(static_cast<std::uint8_t>(v)); b.push_back(static_cast<std::uint8_t>(v >> 8)); };
    const auto u32 = [&](std::vector<std::uint8_t>& b, std::uint32_t v) { u16(b, v & 0xffff); u16(b, v >> 16); };
    const auto name = [](std::vector<std::uint8_t>& b, const char* text) {
        std::array<std::uint8_t, 20> n{};
        std::memcpy(n.data(), text, std::strlen(text));
        b.insert(b.end(), n.begin(), n.end());
    };
    const auto chunk = [&](const char* id, const std::vector<std::uint8_t>& body) {
        std::vector<std::uint8_t> c(id, id + 4);
        u32(c, static_cast<std::uint32_t>(body.size()));
        c.insert(c.end(), body.begin(), body.end());
        if (body.size() & 1) c.push_back(0);
        return c;
    };
    const auto list = [&](const char* type, const std::vector<std::vector<std::uint8_t>>& parts) {
        std::vector<std::uint8_t> body(type, type + 4);
        for (const auto& p : parts) body.insert(body.end(), p.begin(), p.end());
        return chunk("LIST", body);
    };
    constexpr std::uint32_t frames = 44100;
    std::vector<std::uint8_t> smpl;
    for (std::uint32_t i = 0; i < frames + 46; ++i)
        u16(smpl, static_cast<std::uint16_t>(static_cast<std::int16_t>(i < frames ? std::lround(16000 * std::sin(2 * 3.141592653589793 * 440 * i / 44100.0)) : 0)));
    std::vector<std::uint8_t> phdr, pbag, pmod(10), pgen, inst, ibag, imod(10), igen, shdr, ifil;
    name(phdr, "Sine"); u16(phdr, 0); u16(phdr, 0); u16(phdr, 0); u32(phdr, 0); u32(phdr, 0); u32(phdr, 0);
    name(phdr, "EOP"); u16(phdr, 0); u16(phdr, 0); u16(phdr, 1); u32(phdr, 0); u32(phdr, 0); u32(phdr, 0);
    u16(pbag, 0); u16(pbag, 0); u16(pbag, 1); u16(pbag, 0);
    u16(pgen, 41); u16(pgen, 0); u16(pgen, 0); u16(pgen, 0);           // instrument 0, terminal
    name(inst, "SineInst"); u16(inst, 0); name(inst, "EOI"); u16(inst, 1);
    u16(ibag, 0); u16(ibag, 0); u16(ibag, 2); u16(ibag, 0);
    u16(igen, 54); u16(igen, 1); u16(igen, 53); u16(igen, 0); u16(igen, 0); u16(igen, 0);   // loop, sample 0, terminal
    name(shdr, "Sine"); u32(shdr, 0); u32(shdr, frames); u32(shdr, 4410); u32(shdr, frames - 4410); u32(shdr, 44100);
    shdr.push_back(69); shdr.push_back(0); u16(shdr, 0); u16(shdr, 1);
    name(shdr, "EOS"); for (int i = 0; i < 26; ++i) shdr.push_back(0);
    u16(ifil, 2); u16(ifil, 4);
    const auto riff = std::vector<std::vector<std::uint8_t>>{
        list("INFO", {chunk("ifil", ifil)}), list("sdta", {chunk("smpl", smpl)}),
        list("pdta", {chunk("phdr", phdr), chunk("pbag", pbag), chunk("pmod", pmod), chunk("pgen", pgen), chunk("inst", inst),
                      chunk("ibag", ibag), chunk("imod", imod), chunk("igen", igen), chunk("shdr", shdr)})};
    std::vector<std::uint8_t> body{'s', 'f', 'b', 'k'};
    for (const auto& p : riff) body.insert(body.end(), p.begin(), p.end());
    return chunk("RIFF", body);
}

// AVAudioUnitSampler playing a SoundFont preset by note (pitch by zero crossings, release to
// silence, a missing preset failing), and AVAudioEnvironmentNode placing a mono source.
void sampler_environment(Fixture& f) {
    const auto b = alchemy::gui::security::bridge();
    const auto format_of = [&](double rate, std::uint32_t channels) {
        alchemy::CallRegs r{};
        r.x[2] = 1; r.x[3] = channels; r.x[4] = 0;
        r.set_d(0, rate);
        return f.objc.send(f.send(f.cls("AVAudioFormat"), "alloc"), f.objc.selector("initWithCommonFormat:sampleRate:channels:interleaved:"), r).x0;
    };
    const auto stereo = format_of(48000, 2), mono = format_of(48000, 1);
    const auto error = f.block(8);
    const auto make_engine = [&]() {
        const auto engine = f.send(f.send(f.cls("AVAudioEngine"), "alloc"), "init");
        alchemy::CallRegs manual{};
        manual.x = {0, 0, 0, stereo, 4096, error, 0, 0};
        require((f.objc.send(engine, f.objc.selector("enableManualRenderingMode:format:maximumFrameCount:error:"), manual).x0 & 0xff) == 1, "manual rendering");
        return engine;
    };
    const auto render = [&](Address engine, std::uint32_t frames) {
        const auto buffer = f.send(f.send(f.cls("AVAudioPCMBuffer"), "alloc"), "initWithPCMFormat:frameCapacity:", {stereo, 4096});
        alchemy::CallRegs offline{};
        offline.x = {0, 0, frames, buffer, error, 0, 0, 0};
        require(f.objc.send(engine, f.objc.selector("renderOffline:toBuffer:error:"), offline).x0 == 0, "an offline render");
        const auto data = f.send(buffer, "floatChannelData");
        std::array<std::vector<float>, 2> out;
        for (unsigned c = 0; c < 2; ++c)
            for (std::uint32_t i = 0; i < frames; ++i) out[c].push_back(std::bit_cast<float>(f.engine.read32(f.engine.read64(data + 8 * c) + 4ULL * i)));
        return out;
    };
    const auto crossings = [](const std::vector<float>& v) {
        int n = 0;
        for (std::size_t i = 1; i < v.size(); ++i) n += (v[i - 1] < 0) != (v[i] < 0);
        return n;
    };
    // The sound font as a file.
    const auto directory = std::filesystem::temp_directory_path() / "alchemy_qlab_media_sampler";
    std::filesystem::create_directories(directory);
    const auto path = directory / "sine.sf2";
    {
        const auto bytes = sine_sound_font();
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const auto url = f.cf.file_url(alchemy::guest_path_string(path), false);
    const auto engine = make_engine();
    const auto sampler = f.send(f.send(f.cls("AVAudioUnitSampler"), "alloc"), "init");
    f.send(engine, "attachNode:", {sampler});
    f.send(engine, "connect:to:format:", {sampler, f.send(engine, "mainMixerNode"), stereo});
    require(f.send(sampler, "loadSoundBankInstrumentAtURL:program:bankMSB:bankLSB:error:", {url, 0, 0x79, 0, error}) == 1, "the sound font's preset loads");
    require(f.send(sampler, "loadSoundBankInstrumentAtURL:program:bankMSB:bankLSB:error:", {url, 5, 0x79, 0, error}) == 0 && f.engine.read64(error) != 0,
            "a missing preset fails with an error");
    require(f.send(sampler, "loadSoundBankInstrumentAtURL:program:bankMSB:bankLSB:error:", {url, 0, 0x79, 0, error}) == 1, "reloaded");
    require((f.send(engine, "startAndReturnError:", {error}) & 0xff) == 1, "the engine starts");
    f.send(sampler, "startNote:withVelocity:onChannel:", {69, 127, 0});
    static_cast<void>(render(engine, 1024));   // the attack
    const auto a4 = render(engine, 4096);
    const auto a4_crossings = crossings(a4[0]);
    // 440 Hz at 48 kHz: about 75 crossings in 4096 frames; an octave up about 150.
    require(a4_crossings >= 70 && a4_crossings <= 80, "A4 at 440 Hz: " + std::to_string(a4_crossings) + " crossings");
    f.send(sampler, "stopNote:onChannel:", {69, 0});
    f.send(sampler, "startNote:withVelocity:onChannel:", {81, 127, 0});
    static_cast<void>(render(engine, 4096));   // A4's release
    const auto a5 = render(engine, 4096);
    require(std::abs(crossings(a5[0]) - 150) <= 8, "A5 an octave up: " + std::to_string(crossings(a5[0])) + " crossings");
    f.send(sampler, "sendMIDIEvent:data1:data2:", {0x80, 81, 0});
    static_cast<void>(render(engine, 4096));
    const auto quiet = render(engine, 1024);
    require(std::all_of(quiet[0].begin(), quiet[0].end(), [](float v) { return std::fabs(v) < 1e-4f; }), "released notes fall silent");
    f.send(engine, "stop");

    // AVAudioEnvironmentNode: a mono source of 0.25 at (10, 0, 0) with the inverse model (reference 1,
    // rolloff 1): 1/10 of it, all to the right.
    const auto spatial = make_engine();
    static std::atomic<int> unused{0};
    const auto constant = b->make_block([](transmute::CpuState& s, transmute::GuestMemory& m) {
        const auto frames = static_cast<std::uint32_t>(s.x[3]);
        const auto data = m.read64(s.x[4] + 16);
        for (std::uint32_t i = 0; i < frames; ++i) m.write32(data + 4ULL * i, std::bit_cast<std::uint32_t>(0.25f));
        s.x[0] = 0;
    });
    static_cast<void>(unused);
    const auto source = f.send(f.send(f.cls("AVAudioSourceNode"), "alloc"), "initWithFormat:renderBlock:", {mono, constant});
    const auto environment = f.send(f.send(f.cls("AVAudioEnvironmentNode"), "alloc"), "init");
    f.send(spatial, "attachNode:", {source});
    f.send(spatial, "attachNode:", {environment});
    f.send(spatial, "connect:to:format:", {source, environment, mono});
    f.send(spatial, "connect:to:format:", {environment, f.send(spatial, "mainMixerNode"), stereo});
    const auto distance = f.send(environment, "distanceAttenuationParameters");
    f.send(distance, "setDistanceAttenuationModel:", {2});
    alchemy::CallRegs at{};
    at.set_s(0, 10.0f); at.set_s(1, 0.0f); at.set_s(2, 0.0f);
    static_cast<void>(f.objc.send(source, f.objc.selector("setPosition:"), at));
    require(f.objc.send(source, f.objc.selector("position")).s(0) == 10.0f, "the source's position");
    require((f.send(spatial, "startAndReturnError:", {error}) & 0xff) == 1, "the spatial engine starts");
    const auto placed = render(spatial, 1024);
    require(std::fabs(placed[0][500]) < 1e-4f && std::fabs(placed[1][500] - 0.025f) < 1e-4f,
            "to the right, a tenth: " + std::to_string(placed[0][500]) + " " + std::to_string(placed[1][500]));
    // Turning the listener to face the source (yaw 90) centres it: equal power, cos(pi/4) of 0.025 each.
    alchemy::CallRegs turn{};
    turn.set_s(0, 90.0f); turn.set_s(1, 0.0f); turn.set_s(2, 0.0f);
    static_cast<void>(f.objc.send(environment, f.objc.selector("setListenerAngularOrientation:"), turn));
    const auto centred = render(spatial, 1024);
    require(std::fabs(centred[0][500] - centred[1][500]) < 1e-4f && std::fabs(centred[0][500] - 0.025f * 0.70710678f) < 1e-4f,
            "facing it, centred: " + std::to_string(centred[0][500]) + " " + std::to_string(centred[1][500]));
}

// AVAudioSequencer: a Standard MIDI File (format 0, 480 ticks a quarter, 120 BPM) of A4 for a beat,
// a beat's rest, then A5 for a beat, played into the SoundFont sampler in an offline render.
void sequencer(Fixture& f) {
    const auto format_of = [&](double rate, std::uint32_t channels) {
        alchemy::CallRegs r{};
        r.x[2] = 1; r.x[3] = channels; r.x[4] = 0;
        r.set_d(0, rate);
        return f.objc.send(f.send(f.cls("AVAudioFormat"), "alloc"), f.objc.selector("initWithCommonFormat:sampleRate:channels:interleaved:"), r).x0;
    };
    const auto stereo = format_of(48000, 2);
    const auto error = f.block(8);
    const auto engine = f.send(f.send(f.cls("AVAudioEngine"), "alloc"), "init");
    alchemy::CallRegs manual{};
    manual.x = {0, 0, 0, stereo, 4096, error, 0, 0};
    require((f.objc.send(engine, f.objc.selector("enableManualRenderingMode:format:maximumFrameCount:error:"), manual).x0 & 0xff) == 1, "manual rendering");
    const auto directory = std::filesystem::temp_directory_path() / "alchemy_qlab_media_sequencer";
    std::filesystem::create_directories(directory);
    const auto path = directory / "sine.sf2";
    {
        const auto bytes = sine_sound_font();
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const auto sampler = f.send(f.send(f.cls("AVAudioUnitSampler"), "alloc"), "init");
    f.send(engine, "attachNode:", {sampler});
    f.send(engine, "connect:to:format:", {sampler, f.send(engine, "mainMixerNode"), stereo});
    require(f.send(sampler, "loadSoundBankInstrumentAtURL:program:bankMSB:bankLSB:error:", {f.cf.file_url(alchemy::guest_path_string(path), false), 0, 0x79, 0, error}) == 1,
            "the instrument loads");
    // The MIDI file.
    std::vector<std::uint8_t> track{0x00, 0xff, 0x51, 0x03, 0x07, 0xa1, 0x20,   // tempo 500000 us
                                    0x00, 0x90, 69, 100, 0x83, 0x60, 0x80, 69, 0,   // A4 for 480 ticks
                                    0x83, 0x60, 0x90, 81, 100, 0x83, 0x60, 0x80, 81, 0,   // a rest, A5 for 480 ticks
                                    0x83, 0x60, 0xff, 0x2f, 0x00};   // end of track at 1920
    std::vector<std::uint8_t> smf{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xe0, 'M', 'T', 'r', 'k', 0, 0, 0, static_cast<std::uint8_t>(track.size())};
    smf.insert(smf.end(), track.begin(), track.end());
    const auto buffer = f.block(smf.size());
    f.memory().write_bytes(buffer, smf);
    const auto data = f.send(f.cls("NSData"), "dataWithBytes:length:", {buffer, smf.size()});
    const auto sequencer = f.send(f.send(f.cls("AVAudioSequencer"), "alloc"), "initWithAudioEngine:", {engine});
    require(f.send(sequencer, "loadFromData:options:error:", {data, 0, error}) == 1, "the MIDI file loads");
    const auto tracks = f.send(sequencer, "tracks");
    require(f.cf.length(tracks) == 1, "one track");
    const auto first = f.cf.element(tracks, 0);
    require(f.objc.send(first, f.objc.selector("lengthInBeats")).d(0) == 4.0 && std::fabs(f.objc.send(first, f.objc.selector("lengthInSeconds")).d(0) - 2.0) < 1e-9,
            "four beats, two seconds");
    require((f.send(engine, "startAndReturnError:", {error}) & 0xff) == 1 && (f.send(sequencer, "startAndReturnError:", {error}) & 0xff) == 1, "playing");
    const auto render = [&](std::uint32_t frames) {
        const auto pcm = f.send(f.send(f.cls("AVAudioPCMBuffer"), "alloc"), "initWithPCMFormat:frameCapacity:", {stereo, 4096});
        std::vector<float> left;
        for (std::uint32_t done = 0; done < frames; done += 4000) {
            const auto count = std::min<std::uint32_t>(4000, frames - done);
            alchemy::CallRegs offline{};
            offline.x = {0, 0, count, pcm, error, 0, 0, 0};
            require(f.objc.send(engine, f.objc.selector("renderOffline:toBuffer:error:"), offline).x0 == 0, "an offline render");
            const auto channel = f.engine.read64(f.send(pcm, "floatChannelData"));
            for (std::uint32_t i = 0; i < count; ++i) left.push_back(std::bit_cast<float>(f.engine.read32(channel + 4ULL * i)));
        }
        return left;
    };
    const auto crossings = [](const std::vector<float>& v, std::size_t from, std::size_t count) {
        int n = 0;
        for (std::size_t i = from + 1; i < from + count; ++i) n += (v[i - 1] < 0) != (v[i] < 0);
        return n;
    };
    const auto loudest = [](const std::vector<float>& v, std::size_t from, std::size_t count) {
        float m = 0;
        for (std::size_t i = from; i < from + count; ++i) m = std::max(m, std::fabs(v[i]));
        return m;
    };
    // Two seconds at 48 kHz: A4 in [0, 0.5 s), silence in [0.5, 1), A5 in [1, 1.5).
    const auto out = render(96000);
    require(std::abs(crossings(out, 4800, 4800) - 88) <= 6, "A4 plays first: " + std::to_string(crossings(out, 4800, 4800)) + " crossings in 0.1 s");
    require(loudest(out, 33600, 9600) < 1e-4f, "the rest is silent: " + std::to_string(loudest(out, 33600, 9600)));
    require(std::abs(crossings(out, 52800, 4800) - 176) <= 8, "A5 plays third: " + std::to_string(crossings(out, 52800, 4800)) + " crossings in 0.1 s");
    require(std::fabs(f.objc.send(sequencer, f.objc.selector("currentPositionInSeconds")).d(0) - 2.0) < 0.01, "the clock ran two seconds");
    f.send(sequencer, "stop");
    require((f.send(sequencer, "isPlaying") & 0xff) == 0, "stopped");
    const auto written = f.send(sequencer, "dataWithSMPTEResolution:error:", {0, error});
    require(written && f.engine.read32(f.send(written, "bytes")) == 0x6468544d, "written back as a MIDI file");   // "MThd"
}

void music_sequence(Fixture& f) {
    const auto out = f.block(8);
    require(f.status(at, "_NewMusicSequence", {out}) == 0, "NewMusicSequence");
    const auto sequence = f.engine.read64(out);
    const auto bytes = fixture_bytes("two_tracks.mid");
    const auto data = alchemy::gui::security::bridge()->data(bytes);
    require(f.status(at, "_MusicSequenceFileLoadData", {sequence, data, 0, 0}) == 0, "MusicSequenceFileLoadData");
    const auto count = f.block(4);
    require(f.status(at, "_MusicSequenceGetTrackCount", {sequence, count}) == 0 && f.engine.read32(count) == 1, "track count");
    require(f.status(at, "_MusicSequenceGetIndTrack", {sequence, 0, out}) == 0, "GetIndTrack");
    const auto track = f.engine.read64(out);
    require(f.status(at, "_MusicSequenceGetIndTrack", {sequence, 1, out}) == -10859, "track index error");
    require(f.status(at, "_MusicSequenceGetTrackIndex", {sequence, track, count}) == 0 && f.engine.read32(count) == 0, "GetTrackIndex");
    require(f.status(at, "_MusicTrackGetSequence", {track, out}) == 0 && f.engine.read64(out) == sequence, "MusicTrackGetSequence");
    // Iterate: the six events with their types.
    require(f.status(at, "_NewMusicEventIterator", {track, out}) == 0, "iterator");
    const auto iterator = f.engine.read64(out);
    const auto has = f.block(1), time = f.block(8), type = f.block(4), payload = f.block(8), size = f.block(4);
    std::vector<std::uint32_t> types;
    for (;;) {
        require(f.status(at, "_MusicEventIteratorHasNextEvent", {iterator, has}) == 0, "HasNextEvent");
        if (!f.memory().read8(has)) break;
        require(f.status(at, "_MusicEventIteratorGetEventInfo", {iterator, time, type, payload, size}) == 0, "GetEventInfo");
        types.push_back(f.engine.read32(type));
        if (f.engine.read32(type) == 6 && types.size() == 2)
            require(f.memory().read8(f.engine.read64(payload) + 1) == 60 && f.engine.read32(size) == 8, "note payload");
        require(f.status(at, "_MusicEventIteratorNextEvent", {iterator}) == 0, "NextEvent");
    }
    require((types == std::vector<std::uint32_t>{5, 6, 6, 7, 7, 8}), "event types");
    require(f.status(at, "_MusicEventIteratorNextEvent", {iterator}) == -10857, "end of track");
    require(f.status(at, "_DisposeMusicEventIterator", {iterator}) == 0, "dispose iterator");
    // Tempo map and bar/beat conversion through the guest ABI.
    alchemy::CallRegs seconds{};
    seconds.x[0] = sequence; seconds.x[1] = time; seconds.set_d(0, 5.0);
    require(f.regs(at, "_MusicSequenceGetSecondsForBeats", seconds).x0 == 0 && std::bit_cast<double>(f.engine.read64(time)) == 3.0, "seconds for beats");
    const auto bar = f.block(12);
    alchemy::CallRegs to_bar{};
    to_bar.x[0] = sequence; to_bar.x[1] = 4; to_bar.x[2] = bar; to_bar.set_d(0, 4.5);
    require(f.regs(at, "_MusicSequenceBeatsToBarBeatTime", to_bar).x0 == 0 && f.engine.read32(bar) == 2 && f.memory().read8(bar + 4) == 2 &&
            f.memory().read8(bar + 6) == 2, "bar/beat");
    require(f.status(at, "_MusicSequenceBarBeatTimeToBeats", {sequence, bar, time}) == 0 && std::bit_cast<double>(f.engine.read64(time)) == 4.5, "bar/beat back");
    // Track properties: length, then mute.
    const auto length = f.block(4);
    f.engine.write32(length, 8);
    require(f.status(at, "_MusicTrackGetProperty", {track, 5, time, length}) == 0 && std::bit_cast<double>(f.engine.read64(time)) == 5.0, "track length");
    // Events added through the API keep time order; tempo events only go to the tempo track.
    const auto note = f.block(8);
    f.memory().write8(note, 0); f.memory().write8(note + 1, 72); f.memory().write8(note + 2, 80); f.memory().write8(note + 3, 0);
    f.engine.write32(note + 4, std::bit_cast<std::uint32_t>(0.5f));
    alchemy::CallRegs add{};
    add.x[0] = track; add.x[1] = note; add.set_d(0, 0.25);
    require(f.regs(at, "_MusicTrackNewMIDINoteEvent", add).x0 == 0, "add note");
    alchemy::CallRegs tempo{};
    tempo.x[0] = track; tempo.set_d(0, 1.0); tempo.set_d(1, 90.0);
    require(static_cast<std::int32_t>(f.regs(at, "_MusicTrackNewExtendedTempoEvent", tempo).x0) == -10853, "tempo events need the tempo track");
    // Writing a file.
    require(f.status(at, "_MusicSequenceFileCreateData", {sequence, 0x6d696469, 0, 480, out}) == 0 && f.engine.read64(out) != 0, "FileCreateData");
    // Editing through the guest ABI, on a second sequence loaded from the same file (seven events now in the first).
    {
        const auto edit_out = f.block(8);
        require(f.status(at, "_NewMusicSequence", {edit_out}) == 0, "a sequence to edit");
        const auto edited = f.engine.read64(edit_out);
        require(f.status(at, "_MusicSequenceFileLoadData", {edited, data, 0, 0}) == 0 && f.status(at, "_MusicSequenceGetIndTrack", {edited, 0, edit_out}) == 0,
                "loaded");
        const auto edit_track = f.engine.read64(edit_out);
        const auto event_count = [&](Address t) {
            require(f.status(at, "_NewMusicEventIterator", {t, edit_out}) == 0, "an iterator");
            const auto it = f.engine.read64(edit_out);
            std::uint32_t n = 0;
            for (;; ++n) {
                require(f.status(at, "_MusicEventIteratorHasCurrentEvent", {it, has}) == 0, "HasCurrentEvent");
                if (!f.memory().read8(has)) break;
                static_cast<void>(f.status(at, "_MusicEventIteratorNextEvent", {it}));
            }
            static_cast<void>(f.status(at, "_DisposeMusicEventIterator", {it}));
            return n;
        };
        // Cut [3, 4): the control and program changes at 3 go, the SysEx at 4 moves to 3.
        alchemy::CallRegs cut{};
        cut.x[0] = edit_track; cut.set_d(0, 3.0); cut.set_d(1, 4.0);
        require(f.regs(at, "_MusicTrackCut", cut).x0 == 0 && event_count(edit_track) == 4, "MusicTrackCut");
        // Copy [0, 1) of the first sequence's track to 10 (merge: nothing moves).
        alchemy::CallRegs merge{};
        merge.x[0] = track; merge.x[1] = edit_track; merge.set_d(0, 0.0); merge.set_d(1, 1.0); merge.set_d(2, 10.0);
        require(f.regs(at, "_MusicTrackMerge", merge).x0 == 0 && event_count(edit_track) == 8, "MusicTrackMerge copies four events");
        // The last event (at 10.25, the added note copied) moves to 0.5 through the iterator.
        require(f.status(at, "_NewMusicEventIterator", {edit_track, edit_out}) == 0, "iterator");
        const auto it = f.engine.read64(edit_out);
        alchemy::CallRegs seek{};
        seek.x[0] = it; seek.set_d(0, 10.2);
        require(f.regs(at, "_MusicEventIteratorSeek", seek).x0 == 0, "seek");
        alchemy::CallRegs move{};
        move.x[0] = it; move.set_d(0, 0.5);
        require(f.regs(at, "_MusicEventIteratorSetEventTime", move).x0 == 0 &&
                f.status(at, "_MusicEventIteratorGetEventInfo", {it, time, type, payload, size}) == 0 &&
                std::bit_cast<double>(f.engine.read64(time)) == 0.5 && f.engine.read32(type) == 6, "SetEventTime: the iterator stays on it");
        const auto channel = f.block(4);
        f.memory().write8(channel, 0xb0); f.memory().write8(channel + 1, 1); f.memory().write8(channel + 2, 64);
        require(f.status(at, "_MusicEventIteratorSetEventInfo", {it, 7, channel}) == 0 &&
                f.status(at, "_MusicEventIteratorGetEventInfo", {it, time, type, payload, size}) == 0 && f.engine.read32(type) == 7 &&
                f.engine.read32(size) == 4 && f.memory().read8(f.engine.read64(payload) + 2) == 64, "SetEventInfo: now a control change");
        require(f.status(at, "_MusicEventIteratorSetEventInfo", {it, 3, channel}) == -10853, "no tempo event outside the tempo track");
        require(f.status(at, "_DisposeMusicEventIterator", {it}) == 0, "iterator disposed");
        alchemy::CallRegs clear{};
        clear.x[0] = edit_track; clear.set_d(0, 10.0); clear.set_d(1, 20.0);
        require(f.regs(at, "_MusicTrackClear", clear).x0 == 0 && event_count(edit_track) == 5, "MusicTrackClear");
        require(f.status(at, "_MusicSequenceReverse", {edited}) == 0 && event_count(edit_track) == 5, "MusicSequenceReverse keeps the events");
        // Seconds: the tempo track becomes 60 BPM, so beats are seconds.
        require(f.status(at, "_MusicSequenceSetSequenceType", {edited, 0x73656373}) == 0 &&
                f.status(at, "_MusicSequenceGetSequenceType", {edited, count}) == 0 && f.engine.read32(count) == 0x73656373, "seconds type");
        alchemy::CallRegs in_seconds{};
        in_seconds.x[0] = edited; in_seconds.x[1] = time; in_seconds.set_d(0, 7.0);
        require(f.regs(at, "_MusicSequenceGetSecondsForBeats", in_seconds).x0 == 0 && std::bit_cast<double>(f.engine.read64(time)) == 7.0, "beats are seconds");
        require(f.status(at, "_MusicSequenceSetSequenceType", {edited, 0x73616d70}) == -10846, "samples need an AUGraph");
        // MusicSequenceFileCreate writes the file, which loads back.
        const auto directory = std::filesystem::temp_directory_path() / "alchemy_media_music_edit";
        std::filesystem::create_directories(directory);
        const auto url = f.cf.file_url(alchemy::guest_path_string(directory / "edited.mid"), false);
        require(f.status(at, "_MusicSequenceFileCreate", {edited, url, 0x6d696469, 1, 480}) == 0, "MusicSequenceFileCreate");
        require(f.status(at, "_MusicSequenceFileCreate", {edited, url, 0x6d696469, 0, 480}) == static_cast<std::int32_t>(0x70726d3f), "no erase flag: not replaced");
        require(f.status(at, "_MusicSequenceFileLoad", {edited, url, 0, 0}) == 0 && f.status(at, "_MusicSequenceGetIndTrack", {edited, 0, edit_out}) == 0 &&
                event_count(f.engine.read64(edit_out)) == 5, "the written file loads back");
        require(f.status(at, "_DisposeMusicSequence", {edited}) == 0, "disposed");
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    // The player plays the sequence to a MIDI destination.
    const auto player_out = f.block(8);
    require(f.status(at, "_NewMusicPlayer", {player_out}) == 0, "NewMusicPlayer");
    const auto player = f.engine.read64(player_out);
    require(f.status(at, "_MusicPlayerStart", {player}) == -10854, "no sequence");
    require(f.status(at, "_MusicPlayerSetSequence", {player, sequence}) == 0, "SetSequence");
    require(f.status(at, "_MusicPlayerStart", {player}) == -66720, "no MIDI destination and no AUGraph");
    const auto destination = f.call(midi_lib, "_MIDIGetDestination", {0});
    require(f.status(at, "_MusicSequenceSetMIDIEndpoint", {sequence, destination}) == 0, "SetMIDIEndpoint");
    require(f.status(at, "_MusicSequenceSetMIDIEndpoint", {sequence, 0x7fff0000}) == -10855, "not a destination");
    std::size_t before{};
    { std::lock_guard lock(FakeMidi::mutex()); before = FakeMidi::sent().size(); }
    require(f.status(at, "_MusicPlayerStart", {player}) == 0, "MusicPlayerStart");
    const auto host = f.block(8);
    alchemy::CallRegs host_for{};
    host_for.x[0] = player; host_for.x[1] = host; host_for.set_d(0, 2.0);
    require(f.regs(at, "_MusicPlayerGetHostTimeForBeats", host_for).x0 == 0 && f.engine.read64(host) > 0, "host time for beats");
    bool got_note = false;
    for (int i = 0; i < 100 && !got_note; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::lock_guard lock(FakeMidi::mutex());
        for (std::size_t k = before; k < FakeMidi::sent().size(); ++k)
            got_note = got_note || FakeMidi::sent()[k] == std::vector<std::uint8_t>({0x90, 60, 100});
    }
    require(got_note, "the first note reached the MIDI destination");
    require(f.status(at, "_MusicPlayerGetTime", {player, time}) == 0 && std::bit_cast<double>(f.engine.read64(time)) > 0, "time advances");
    require(f.status(at, "_MusicPlayerStop", {player}) == 0, "stop");
    require(f.status(at, "_MusicPlayerGetHostTimeForBeats", {player, host}) == -10852, "host time needs a playing player");
    require(f.status(at, "_DisposeMusicPlayer", {player}) == 0 && f.status(at, "_DisposeMusicSequence", {sequence}) == 0, "dispose");
}

// ExtAudioFile reads of a generated WAVE file (1000 frames of 16-bit stereo ramps).
void audio_files(Fixture& f) {
    const auto directory = std::filesystem::temp_directory_path() / "alchemy_qlab_media_tests";
    std::filesystem::create_directories(directory);
    const auto path = directory / "ramp.wav";
    {
        std::vector<std::uint8_t> data;
        for (int i = 0; i < 1000; ++i)
            for (const int v : {i * 32, -i * 32}) { data.push_back(static_cast<std::uint8_t>(v)); data.push_back(static_cast<std::uint8_t>(v >> 8)); }
        std::vector<std::uint8_t> file;
        const auto le = [&](std::uint32_t value, int bytes) { for (int i = 0; i < bytes; ++i) file.push_back(static_cast<std::uint8_t>(value >> (8 * i))); };
        const auto tag = [&](const char* text) { file.insert(file.end(), text, text + 4); };
        tag("RIFF"); le(36 + static_cast<std::uint32_t>(data.size()), 4); tag("WAVE");
        tag("fmt "); le(16, 4); le(1, 2); le(2, 2); le(44100, 4); le(44100 * 4, 4); le(4, 2); le(16, 2);
        tag("data"); le(static_cast<std::uint32_t>(data.size()), 4);
        file.insert(file.end(), data.begin(), data.end());
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    }
    const auto url = f.cf.file_url(alchemy::guest_path_string(path), false);
    const auto out = f.block(8);
    require(f.status(at, "_AudioFileOpenURL", {url, 1, 0, out}) == 0, "AudioFileOpenURL");
    const auto file = f.engine.read64(out);
    const auto size = f.block(4), value = f.block(64);
    f.engine.write32(size, 8);
    require(f.status(at, "_AudioFileGetProperty", {file, 0x70636e74, size, value}) == 0 && f.engine.read64(value) == 1000, "packet count");   // 'pcnt'
    f.engine.write32(size, 40);
    require(f.status(at, "_AudioFileGetProperty", {file, 0x64666d74, size, value}) == 0 && f.engine.read32(value + 28) == 2 &&
            f.engine.read32(value + 8) == 0x6c70636d, "data format");   // 'dfmt'
    f.engine.write32(size, 4);
    require(f.status(at, "_AudioFileGetProperty", {file, 0x64666d74, size, value}) == static_cast<std::int32_t>(0x2173697a), "'!siz'");
    // A canonical WAVE header is the one Alchemy writes: the file opens for writing too.
    const auto writable_out = f.block(8);
    require(f.status(at, "_AudioFileOpenURL", {url, 3, 0, writable_out}) == 0 && f.status(at, "_AudioFileClose", {f.engine.read64(writable_out)}) == 0,
            "a canonical WAVE opens for writing");
    require(f.status(at, "_ExtAudioFileWrapAudioFileID", {file, 0, out}) == 0, "ExtAudioFileWrapAudioFileID");
    const auto ext = f.engine.read64(out);
    f.engine.write32(size, 8);
    require(f.status(at, "_ExtAudioFileGetProperty", {ext, 0x2366726d, size, value}) == 0 && f.engine.read64(value) == 1000, "file length frames");   // '#frm'
    // Client format: float32, non-interleaved, same rate.
    const auto client = f.block(40);
    f.engine.write64(client, std::bit_cast<std::uint64_t>(44100.0));
    f.engine.write32(client + 8, 0x6c70636d);
    f.engine.write32(client + 12, 1 | 8 | 32);   // float, packed, non-interleaved
    f.engine.write32(client + 16, 4); f.engine.write32(client + 20, 1); f.engine.write32(client + 24, 4);
    f.engine.write32(client + 28, 2); f.engine.write32(client + 32, 32);
    require(f.status(at, "_ExtAudioFileSetProperty", {ext, 0x63666d74, 40, client}) == 0, "client format");   // 'cfmt'
    f.engine.write64(client, std::bit_cast<std::uint64_t>(48000.0));
    require(f.status(at, "_ExtAudioFileSetProperty", {ext, 0x63666d74, 40, client}) == static_cast<std::int32_t>(0x666d743f), "no sample-rate conversion");
    // Read 600 frames into two 512-frame buffers: 512 come back, then the rest.
    const auto left = f.block(512 * 4), right = f.block(512 * 4), list = f.block(8 + 32), frames = f.block(4);
    const auto setup = [&] {
        f.engine.write32(list, 2);
        f.engine.write32(list + 8, 1); f.engine.write32(list + 12, 512 * 4); f.engine.write64(list + 16, left);
        f.engine.write32(list + 24, 1); f.engine.write32(list + 28, 512 * 4); f.engine.write64(list + 32, right);
    };
    setup();
    f.engine.write32(frames, 600);
    require(f.status(at, "_ExtAudioFileRead", {ext, frames, list}) == 0 && f.engine.read32(frames) == 512 && f.engine.read32(list + 12) == 2048, "read limited by the buffers");
    const auto sample = [&](Address buffer, int index) { return std::bit_cast<float>(f.engine.read32(buffer + 4 * index)); };
    require(sample(left, 100) == 3200.0f / 32768 && sample(right, 100) == -3200.0f / 32768, "converted samples");
    require(f.status(at, "_ExtAudioFileSeek", {ext, 990}) == 0, "seek");
    setup();
    f.engine.write32(frames, 512);
    require(f.status(at, "_ExtAudioFileRead", {ext, frames, list}) == 0 && f.engine.read32(frames) == 10 && sample(left, 0) == 990.0f * 32 / 32768, "read to the end");
    setup();
    f.engine.write32(frames, 512);
    require(f.status(at, "_ExtAudioFileRead", {ext, frames, list}) == 0 && f.engine.read32(frames) == 0, "end of file: 0 frames");
    require(f.status(at, "_ExtAudioFileDispose", {ext}) == 0 && f.status(at, "_AudioFileClose", {file}) == 0, "dispose and close");
    require(f.status(at, "_ExtAudioFileOpenURL", {f.cf.file_url(alchemy::guest_path_string(directory / "missing.wav"), false), out}) == -43, "missing file");
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// A linear PCM AudioStreamBasicDescription at `at`.
void write_asbd(Fixture& f, Address at, double rate, std::uint32_t flags, std::uint32_t bytes_per_frame, std::uint32_t channels, std::uint32_t bits) {
    f.engine.write64(at, std::bit_cast<std::uint64_t>(rate));
    f.engine.write32(at + 8, 0x6c70636d);   // 'lpcm'
    f.engine.write32(at + 12, flags);
    f.engine.write32(at + 16, bytes_per_frame);
    f.engine.write32(at + 20, 1);
    f.engine.write32(at + 24, bytes_per_frame);
    f.engine.write32(at + 28, channels);
    f.engine.write32(at + 32, bits);
    f.engine.write32(at + 36, 0);
}

// Writing: AudioFileCreateWithURL + WritePackets (WAVE, AIFF), ExtAudioFileCreateWithURL + Write with
// a client format (CAF), the callback forms over memory, and the formats a container cannot hold.
void audio_file_writing(Fixture& f) {
    namespace af = alchemy::gui::completion_kit::media::audio_file;
    const auto directory = std::filesystem::temp_directory_path() / "alchemy_media_writing";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const auto url_of = [&](const char* name) { return f.cf.file_url(alchemy::guest_path_string(directory / name), false); };
    const auto asbd = f.block(40), out = f.block(8), count = f.block(4), size = f.block(4), value = f.block(64);
    // WAVE, 16-bit stereo: 100 frames of (i, -i).
    write_asbd(f, asbd, 44100, 4 | 8, 4, 2, 16);   // signed integer, packed
    require(f.status(at, "_AudioFileCreateWithURL", {url_of("out.wav"), 0x57415645, asbd, 1, out}) == 0, "AudioFileCreateWithURL WAVE");
    auto file = f.engine.read64(out);
    const auto frames = f.block(400);
    for (std::uint32_t i = 0; i < 100; ++i) {
        f.engine.write32(frames + 4ULL * i, (static_cast<std::uint32_t>(static_cast<std::uint16_t>(-static_cast<std::int16_t>(i * 100))) << 16) | (i * 100));
    }
    f.engine.write32(count, 100);
    require(f.status(at, "_AudioFileWritePackets", {file, 0, 400, 0, 0, count, frames}) == 0 && f.engine.read32(count) == 100, "AudioFileWritePackets");
    f.engine.write32(size, 8);
    require(f.status(at, "_AudioFileGetProperty", {file, 0x70636e74, size, value}) == 0 && f.engine.read64(value) == 100, "packet count while writing");
    require(f.status(at, "_AudioFileClose", {file}) == 0, "AudioFileClose");
    {
        std::ifstream in(directory / "out.wav", std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        require(bytes.size() == 44 + 400 && std::memcmp(bytes.data(), "RIFF", 4) == 0 && bytes[4] == 36 + 400 - 256 && bytes[5] == 1 &&
                bytes[40] == 0x90 && bytes[41] == 1 && bytes[48] == 100 && bytes[50] == 0x9c && bytes[51] == 0xff, "the WAVE bytes on disk");
    }
    require(f.status(at, "_AudioFileOpenURL", {url_of("out.wav"), 1, 0, out}) == 0, "the WAVE reads back");
    file = f.engine.read64(out);
    f.engine.write32(size, 8);
    require(f.status(at, "_AudioFileGetProperty", {file, 0x70636e74, size, value}) == 0 && f.engine.read64(value) == 100, "100 packets");
    require(f.status(at, "_AudioFileClose", {file}) == 0, "closed");
    require(f.status(at, "_AudioFileCreateWithURL", {url_of("out.wav"), 0x57415645, asbd, 0, out}) == static_cast<std::int32_t>(0x70726d3f),
            "an existing file is not replaced without kAudioFileFlags_EraseFile");
    // User data: a 'LIST' chunk set while writing and then replaced, a 'note' added and removed; the
    // header's 'fmt ' is listed but fixed. The file reads back with the LIST after its audio data.
    {
        constexpr std::uint32_t list_id = 0x4c495354, note_id = 0x6e6f7465, fmt_id = 0x666d7420, data_id = 0x64617461;
        require(f.status(at, "_AudioFileCreateWithURL", {url_of("user.wav"), 0x57415645, asbd, 1, out}) == 0, "a WAVE for user data");
        file = f.engine.read64(out);
        f.engine.write32(count, 100);
        require(f.status(at, "_AudioFileWritePackets", {file, 0, 400, 0, 0, count, frames}) == 0, "its audio");
        const auto user = f.block(16);
        f.memory().write_bytes(user, std::span(reinterpret_cast<const std::uint8_t*>("INFOtest122"), 11));
        require(f.status(at, "_AudioFileSetUserData", {file, list_id, 0, 9, user}) == 0 &&
                f.status(at, "_AudioFileSetUserData", {file, list_id, 0, 10, user}) == 0, "LIST set, then replaced");
        require(f.status(at, "_AudioFileSetUserData", {file, note_id, 0, 3, user}) == 0 && f.status(at, "_AudioFileCountUserData", {file, note_id, count}) == 0 &&
                f.engine.read32(count) == 1 && f.status(at, "_AudioFileRemoveUserData", {file, note_id, 0}) == 0 &&
                f.status(at, "_AudioFileCountUserData", {file, note_id, count}) == 0 && f.engine.read32(count) == 0, "a note added and removed");
        require(f.status(at, "_AudioFileCountUserData", {file, fmt_id, count}) == 0 && f.engine.read32(count) == 1 &&
                f.status(at, "_AudioFileSetUserData", {file, fmt_id, 0, 3, user}) == static_cast<std::int32_t>(0x6f703f3f) &&
                f.status(at, "_AudioFileCountUserData", {file, data_id, count}) == 0 && f.engine.read32(count) == 0, "the header's chunks are fixed");
        require(f.status(at, "_AudioFileClose", {file}) == 0, "closed with user data");
        std::ifstream in(directory / "user.wav", std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        require(bytes.size() == 44 + 400 + 8 + 10 && bytes[4] + (bytes[5] << 8) == 44 + 400 + 18 - 8 && std::memcmp(bytes.data() + 444, "LIST", 4) == 0 &&
                bytes[448] == 10 && std::memcmp(bytes.data() + 452, "INFOtest12", 10) == 0, "the LIST chunk follows the data on disk");
        require(f.status(at, "_AudioFileOpenURL", {url_of("user.wav"), 1, 0, out}) == 0, "it reads back");
        file = f.engine.read64(out);
        const auto got = f.block(64);
        f.engine.write32(size, 8);
        require(f.status(at, "_AudioFileGetProperty", {file, 0x70636e74, size, value}) == 0 && f.engine.read64(value) == 100, "its audio is intact");
        require(f.status(at, "_AudioFileCountUserData", {file, list_id, count}) == 0 && f.engine.read32(count) == 1 &&
                f.status(at, "_AudioFileGetUserDataSize", {file, list_id, 0, size}) == 0 && f.engine.read32(size) == 10, "LIST: one, 10 bytes");
        f.engine.write32(size, 64);
        require(f.status(at, "_AudioFileGetUserData", {file, list_id, 0, size, got}) == 0 && f.engine.read32(size) == 10 &&
                f.memory().read8(got + 9) == '2', "AudioFileGetUserData");
        f.engine.write32(size, 64);
        require(f.status(at, "_AudioFileGetUserDataAtOffset", {file, list_id, 0, 4, size, got}) == 0 && f.engine.read32(size) == 6 &&
                f.memory().read8(got) == 't', "AudioFileGetUserDataAtOffset");
        f.engine.write32(size, 4);
        require(f.status(at, "_AudioFileGetUserData", {file, list_id, 0, size, got}) == static_cast<std::int32_t>(0x2173697a), "too small ('!siz')");
        require(f.status(at, "_AudioFileGetUserDataSize", {file, list_id, 1, size}) == static_cast<std::int32_t>(0x63686b3f), "no second LIST ('chk?')");
        require(f.status(at, "_AudioFileSetUserData", {file, list_id, 0, 3, user}) == static_cast<std::int32_t>(0x70726d3f), "read-only ('prm?')");
        require(f.status(at, "_AudioFileClose", {file}) == 0, "closed");
    }
    // Global info: the types Alchemy writes and reads, their names, extensions and stream descriptions.
    {
        const auto specifier = f.block(16), info = f.block(1024);
        require(f.status(at, "_AudioFileGetGlobalInfoSize", {0x61667766, 0, 0, size}) == 0 && f.engine.read32(size) == 24, "six writable types (M4A and MPEG-4 of Apple Lossless)");
        f.engine.write32(size, 1024);
        require(f.status(at, "_AudioFileGetGlobalInfo", {0x61667266, 0, 0, size, info}) == 0 && f.engine.read32(size) >= 32 &&
                f.engine.read32(info) == 0x57415645, "readable types, WAVE first");
        f.engine.write32(specifier, 0x63616666);   // 'caff'
        f.engine.write32(size, 8);
        require(f.status(at, "_AudioFileGetGlobalInfo", {0x66746e6d, 4, specifier, size, info}) == 0 && f.cf.utf8(f.engine.read64(info)) == "CAF",
                "kAudioFileGlobalInfo_FileTypeName");
        f.engine.write32(specifier, 0x57415645);   // 'WAVE'
        f.engine.write32(size, 8);
        const auto cf_library = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
        require(f.status(at, "_AudioFileGetGlobalInfo", {0x66657874, 4, specifier, size, info}) == 0 &&
                f.call(cf_library, "_CFArrayGetCount", {f.engine.read64(info)}) == 2, "ExtensionsForType WAVE");
        f.engine.write64(specifier, f.cf.string("MP3"));
        f.engine.write32(size, 64);
        require(f.status(at, "_AudioFileGetGlobalInfo", {0x74657874, 8, specifier, size, info}) == 0 && f.engine.read32(size) == 4 &&
                f.engine.read32(info) == 0x4d504733, "TypesForExtension mp3 (any case)");
        f.engine.write32(specifier, 0x57415645);
        f.engine.write32(specifier + 4, 0x6c70636d);   // {WAVE, lpcm}
        f.engine.write32(size, 1024);
        require(f.status(at, "_AudioFileGetGlobalInfo", {0x73646964, 8, specifier, size, info}) == 0 && f.engine.read32(size) == 6 * 40 &&
                f.engine.read32(info + 8) == 0x6c70636d, "WAVE holds six sample layouts");
        f.engine.write32(specifier, 0x4d504733);
        f.engine.write32(specifier + 4, 0x2e6d7033);   // {MPG3, .mp3}
        f.engine.write32(size, 1024);
        require(f.status(at, "_AudioFileGetGlobalInfo", {0x73646964, 8, specifier, size, info}) == 0 && f.engine.read32(size) == 40 &&
                f.engine.read32(info + 20) == 1152, "MPG3 holds '.mp3'");
        f.engine.write32(specifier, 0x6d346166);
        f.engine.write32(specifier + 4, 0x616c6163);   // {m4af, alac}
        f.engine.write32(size, 1024);
        require(f.status(at, "_AudioFileGetGlobalInfo", {0x73646964, 8, specifier, size, info}) == 0 && f.engine.read32(size) == 40 &&
                f.engine.read32(info + 8) == 0x616c6163 && f.engine.read32(info + 20) == 4096, "M4A holds 'alac'");
        f.engine.write32(specifier, 0x58585858);
        require(f.status(at, "_AudioFileGetGlobalInfoSize", {0x66657874, 4, specifier, size}) == static_cast<std::int32_t>(0x7479703f), "unknown type");
        require(f.status(at, "_AudioFileGetGlobalInfoSize", {0x7a7a7a7a, 0, 0, size}) == static_cast<std::int32_t>(0x7074793f), "unknown property");
    }
    // AIFF holds big-endian integers only; AIFC also floats; MP3 is no container Alchemy writes.
    require(f.status(at, "_AudioFileCreateWithURL", {url_of("le.aif"), 0x41494646, asbd, 1, out}) == static_cast<std::int32_t>(0x666d743f),
            "AIFF refuses little-endian samples");
    require(f.status(at, "_AudioFileCreateWithURL", {url_of("x.mp3"), 0x4d504733, asbd, 1, out}) == static_cast<std::int32_t>(0x7479703f),
            "no MP3 writer");
    write_asbd(f, asbd, 48000, 1 | 2 | 8, 4, 1, 32);   // float, big-endian, packed
    require(f.status(at, "_AudioFileCreateWithURL", {url_of("f.aifc"), 0x41494643, asbd, 1, out}) == 0, "AIFC float32");
    file = f.engine.read64(out);
    f.engine.write32(count, 4);
    require(f.status(at, "_AudioFileWriteBytes", {file, 0, 0, count, frames}) == 0 && f.status(at, "_AudioFileClose", {file}) == 0, "AudioFileWriteBytes");
    require(f.status(at, "_AudioFileOpenURL", {url_of("f.aifc"), 1, 0, out}) == 0, "the AIFC reads back");
    file = f.engine.read64(out);
    f.engine.write32(size, 40);
    require(f.status(at, "_AudioFileGetProperty", {file, 0x64666d74, size, value}) == 0 && (f.engine.read32(value + 12) & 1) &&
                std::bit_cast<double>(f.engine.read64(value)) == 48000.0, "AIFC data format");
    require(f.status(at, "_AudioFileClose", {file}) == 0, "closed AIFC");

    // ExtAudioFile: a CAF of 16-bit mono written from float32 client frames, read back as float32.
    write_asbd(f, asbd, 22050, 4 | 8, 2, 1, 16);
    require(f.status(at, "_ExtAudioFileCreateWithURL", {url_of("out.caf"), 0x63616666, asbd, 0, 1, out}) == 0, "ExtAudioFileCreateWithURL CAF");
    auto ext = f.engine.read64(out);
    const auto client = f.block(40);
    write_asbd(f, client, 22050, 1 | 8, 4, 1, 32);   // float, packed
    require(f.status(at, "_ExtAudioFileSetProperty", {ext, 0x63666d74, 40, client}) == 0, "client format");
    const auto samples = f.block(64 * 4), list = f.block(24);
    for (int i = 0; i < 64; ++i) f.engine.write32(samples + 4ULL * i, std::bit_cast<std::uint32_t>(i % 2 ? 0.5f : -0.25f));
    f.engine.write32(list, 1);
    f.engine.write32(list + 8, 1);
    f.engine.write32(list + 12, 64 * 4);
    f.engine.write64(list + 16, samples);
    require(f.status(at, "_ExtAudioFileWrite", {ext, 64, list}) == 0 && f.status(at, "_ExtAudioFileWriteAsync", {ext, 0, 0}) == 0, "ExtAudioFileWrite");
    require(f.status(at, "_ExtAudioFileDispose", {ext}) == 0, "ExtAudioFileDispose");
    require(f.status(at, "_ExtAudioFileOpenURL", {url_of("out.caf"), out}) == 0, "the CAF reads back");
    ext = f.engine.read64(out);
    require(f.status(at, "_ExtAudioFileSetProperty", {ext, 0x63666d74, 40, client}) == 0, "client format for reading");
    f.engine.fill_bytes(samples, 0, 64 * 4);
    f.engine.write32(list + 12, 64 * 4);
    f.engine.write32(count, 64);
    require(f.status(at, "_ExtAudioFileRead", {ext, count, list}) == 0 && f.engine.read32(count) == 64, "64 frames");
    require(std::bit_cast<float>(f.engine.read32(samples)) == -0.25f && std::bit_cast<float>(f.engine.read32(samples + 4)) == 0.5f,
            "the samples, through 16 bits");
    require(f.status(at, "_ExtAudioFileDispose", {ext}) == 0, "disposed");

    // The callback forms over memory: InitializeWithCallbacks writes, OpenWithCallbacks reads.
    static std::vector<std::uint8_t> memory;
    memory.clear();
    const auto read_proc = f.process.install_function("_alchemy_test_af_read", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        const auto position = static_cast<std::size_t>(s.x[1]), wanted = static_cast<std::size_t>(s.x[2]);
        const auto n = position >= memory.size() ? 0 : std::min(wanted, memory.size() - position);
        if (n) m.write_bytes(s.x[3], std::span<const std::uint8_t>(memory.data() + position, n));
        m.write32(s.x[4], static_cast<std::uint32_t>(n));
        s.x[0] = 0;
    }, "/usr/lib/alchemy_test_io.dylib");
    const auto write_proc = f.process.install_function("_alchemy_test_af_write", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        const auto position = static_cast<std::size_t>(s.x[1]), n = static_cast<std::size_t>(s.x[2]);
        if (memory.size() < position + n) memory.resize(position + n);
        m.read_bytes(s.x[3], std::span<std::uint8_t>(memory.data() + position, n));
        m.write32(s.x[4], static_cast<std::uint32_t>(n));
        s.x[0] = 0;
    }, "/usr/lib/alchemy_test_io.dylib");
    const auto size_proc = f.process.install_function("_alchemy_test_af_size", [](transmute::CpuState& s, transmute::GuestMemory&) {
        s.x[0] = memory.size();
    }, "/usr/lib/alchemy_test_io.dylib");
    const auto set_size_proc = f.process.install_function("_alchemy_test_af_set_size", [](transmute::CpuState& s, transmute::GuestMemory&) {
        memory.resize(static_cast<std::size_t>(s.x[1]));
        s.x[0] = 0;
    }, "/usr/lib/alchemy_test_io.dylib");
    write_asbd(f, asbd, 8000, 4 | 8, 2, 1, 16);
    alchemy::CallRegs initialize{};
    initialize.x = {0x1234, read_proc, write_proc, size_proc, set_size_proc, 0x57415645, asbd, 0};
    initialize.push_stack(out);
    require(static_cast<std::int32_t>(f.regs(at, "_AudioFileInitializeWithCallbacks", initialize).x0) == 0, "AudioFileInitializeWithCallbacks");
    file = f.engine.read64(out);
    f.engine.write32(count, 50);
    require(f.status(at, "_AudioFileWritePackets", {file, 0, 100, 0, 0, count, frames}) == 0 && f.status(at, "_AudioFileClose", {file}) == 0,
            "written through the callbacks");
    af::Container container;
    std::string why;
    const auto parsed = af::parse([](std::uint64_t offset, std::span<std::uint8_t> to) -> std::size_t {
        if (offset >= memory.size()) return 0;
        const auto n = std::min<std::size_t>(to.size(), memory.size() - static_cast<std::size_t>(offset));
        std::memcpy(to.data(), memory.data() + offset, n);
        return n;
    }, memory.size(), container, why);
    require(parsed == 0 && container.data_bytes == 100 && container.format.rate == 8000 && memory.size() == 44 + 100, "the memory holds a WAVE: " + why);
    require(f.status(at, "_AudioFileOpenWithCallbacks", {0x1234, read_proc, 0, size_proc, 0, 0, out}) == 0, "AudioFileOpenWithCallbacks");
    file = f.engine.read64(out);
    f.engine.write32(size, 8);
    require(f.status(at, "_AudioFileGetProperty", {file, 0x70636e74, size, value}) == 0 && f.engine.read64(value) == 50, "50 packets through the callbacks");
    f.engine.write32(count, 40);
    require(f.status(at, "_AudioFileWriteBytes", {file, 0, 0, count, frames}) == static_cast<std::int32_t>(0x70726d3f), "opened read-only");
    require(f.status(at, "_AudioFileClose", {file}) == 0, "closed");
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// AU listeners keep registrations; with no audio unit instance, setting and notifying answer
// the AudioUnit layer's badComponentInstance.
void au_listeners(Fixture& f) {
    constexpr auto bad_instance = static_cast<std::int32_t>(0x80008001U);
    const auto out = f.block(8), parameter = f.block(24), event = f.block(32);
    alchemy::CallRegs create{};
    create.x[0] = 0x1000; create.x[1] = 7; create.x[4] = out;
    create.set_s(0, 0.1f);
    require(f.regs(at, "_AUListenerCreate", create).x0 == 0, "AUListenerCreate");
    const auto listener = f.engine.read64(out);
    f.engine.write64(parameter, 0x5000);   // an AudioUnit that does not exist
    f.engine.write32(parameter + 8, 3);
    require(f.status(at, "_AUListenerAddParameter", {listener, 0, parameter}) == 0, "AUListenerAddParameter");
    alchemy::CallRegs set{};
    set.x[0] = listener; set.x[2] = parameter;
    set.set_s(0, 0.5f);
    require(static_cast<std::int32_t>(f.regs(at, "_AUParameterSet", set).x0) == bad_instance, "AUParameterSet: no unit");
    require(f.status(at, "_AUParameterListenerNotify", {listener, 0, parameter}) == bad_instance, "notify: no unit");
    require(f.status(at, "_AUListenerRemoveParameter", {listener, 0, parameter}) == 0, "remove");
    alchemy::CallRegs events{};
    events.x[0] = 0x1000; events.x[4] = out;
    require(f.regs(at, "_AUEventListenerCreate", events).x0 == 0, "AUEventListenerCreate");
    const auto event_listener = f.engine.read64(out);
    f.engine.write32(event, 0);
    f.engine.write64(event + 8, 0x5000);
    require(f.status(at, "_AUEventListenerAddEventType", {event_listener, 0, event}) == 0, "parameter event");
    require(f.status(at, "_AUEventListenerRemoveEventType", {event_listener, 0, event}) == 0, "remove event");
    f.engine.write32(event, 3);
    require(f.status(at, "_AUEventListenerAddEventType", {event_listener, 0, event}) == bad_instance, "property events need a unit");
    require(f.status(at, "_AUListenerDispose", {listener}) == 0 && f.status(at, "_AUListenerDispose", {event_listener}) == 0, "dispose");
    require(f.status(at, "_AUListenerDispose", {listener}) == -50, "dispose twice");
    require(f.status(au, "_AudioComponentCopyName", {0, out}) == -50 && f.status(au, "_AudioComponentGetDescription", {0, out}) == -50, "no components");
    require(f.status(au, "_AudioUnitAddRenderNotify", {0x5000, 0x1000, 0}) == bad_instance, "no units");

    // A real parameter: the default output's kHALOutputParam_Volume (0-1, linear).
    const auto description = f.block(20), size = f.block(4), info = f.block(112);
    f.engine.write32(description, 0x61756F75);        // 'auou'
    f.engine.write32(description + 4, 0x64656620);    // 'def '
    require(f.status(au, "_AudioComponentInstanceNew", {f.call(au, "_AudioComponentFindNext", {0, description}), out}) == 0, "an output unit");
    const auto unit = f.engine.read64(out);
    f.engine.write32(size, 104);
    require(f.status(au, "_AudioUnitGetProperty", {unit, 4, 0, 14, info, size}) == 0 && f.engine.read32(size) == 104 &&
            std::bit_cast<float>(f.engine.read32(info + 88)) == 1.0f && f.memory().read8(info) == 'V', "kAudioUnitProperty_ParameterInfo");
    f.engine.write64(parameter, unit);
    f.engine.write32(parameter + 8, 14);
    f.engine.write32(parameter + 12, 0);
    f.engine.write32(parameter + 16, 0);
    alchemy::CallRegs linear{};
    linear.x[0] = parameter;
    linear.set_s(0, 0.25f);
    require(f.regs(at, "_AUParameterValueToLinear", linear).s(0) == 0.25f, "AUParameterValueToLinear");
    linear.set_s(0, 2.0f);
    require(f.regs(at, "_AUParameterValueToLinear", linear).s(0) == 1.0f, "clamped to the range");
    linear.set_s(0, 0.75f);
    require(f.regs(at, "_AUParameterValueFromLinear", linear).s(0) == 0.75f, "AUParameterValueFromLinear");
    const auto text = f.block(32);
    const auto format = [&](double value, std::uint32_t digits) {
        alchemy::CallRegs r{};
        r.x[0] = parameter; r.x[1] = text; r.x[2] = digits;
        r.set_d(0, value);
        static_cast<void>(f.regs(at, "_AUParameterFormatValue", r));
        std::string out;
        for (Address a = text; f.memory().read8(a); ++a) out.push_back(static_cast<char>(f.memory().read8(a)));
        return out;
    };
    require(format(3.14159, 4) == "3.142" && format(123.456, 4) == "123.5" && format(0.5, 3) == "0.50", "AUParameterFormatValue: " + format(3.14159, 4));
    // A dispatch-queue listener hears AUParameterSet on its queue, with the value.
    static std::atomic<int> heard{0};
    static std::atomic<std::uint32_t> heard_value{0};
    const auto b = alchemy::gui::security::bridge();
    const auto block = b->make_block([](transmute::CpuState& s, transmute::GuestMemory& m) {
        if (s.x[1] == 0x77 && m.read32(s.x[2] + 8) == 14) {
            heard_value = static_cast<std::uint32_t>(s.v[0][0]);
            ++heard;
        }
    });
    const auto queue = f.call("/usr/lib/libSystem.B.dylib", "_dispatch_get_global_queue", {0, 0});
    alchemy::CallRegs make{};
    make.x[0] = out; make.x[1] = queue; make.x[2] = block;
    require(f.regs(at, "_AUListenerCreateWithDispatchQueue", make).x0 == 0, "AUListenerCreateWithDispatchQueue");
    const auto queued = f.engine.read64(out);
    require(f.status(at, "_AUListenerAddParameter", {queued, 0x77, parameter}) == 0, "its parameter");
    alchemy::CallRegs set_volume{};
    set_volume.x[0] = 0; set_volume.x[2] = parameter;
    set_volume.set_s(0, 0.5f);
    require(f.regs(at, "_AUParameterSet", set_volume).x0 == 0, "AUParameterSet on the unit");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (heard == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    require(heard == 1 && std::bit_cast<float>(heard_value.load()) == 0.5f, "the block hears 0.5 on its queue");
    // An event listener hears a gesture begin through AUEventListenerNotify.
    static std::atomic<std::uint32_t> gesture{99};
    const auto event_proc = f.process.install_function("_alchemy_test_au_event", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        gesture = m.read32(s.x[2]);
    }, "/usr/lib/alchemy_test_io.dylib");
    alchemy::CallRegs event_create{};
    event_create.x[0] = event_proc; event_create.x[4] = out;
    require(f.regs(at, "_AUEventListenerCreate", event_create).x0 == 0, "an event listener");
    const auto gestures = f.engine.read64(out);
    f.engine.write32(event, 1);   // kAudioUnitEvent_BeginParameterChangeGesture
    std::vector<std::uint8_t> parameter_bytes(24);
    f.memory().read_bytes(parameter, parameter_bytes);
    f.memory().write_bytes(event + 8, parameter_bytes);
    require(f.status(at, "_AUEventListenerAddEventType", {gestures, 0, event}) == 0 &&
            f.status(at, "_AUEventListenerNotify", {0, 0, event}) == 0 && gesture == 1, "AUEventListenerNotify: a gesture begins");
    require(f.status(at, "_AUListenerDispose", {queued}) == 0 && f.status(at, "_AUListenerDispose", {gestures}) == 0 &&
            f.status(au, "_AudioComponentInstanceDispose", {unit}) == 0, "listeners and unit disposed");
}

// AVFoundation: an asset over a generated WAVE file, a reader vending float samples, value
// classes, and the classes whose instances cannot exist.
void av_foundation(Fixture& f) {
    const auto directory = std::filesystem::temp_directory_path() / "alchemy_qlab_media_av";
    std::filesystem::create_directories(directory);
    const auto path = directory / "tone.wav";
    {
        std::vector<std::uint8_t> file;
        const auto le = [&](std::uint32_t value, int bytes) { for (int i = 0; i < bytes; ++i) file.push_back(static_cast<std::uint8_t>(value >> (8 * i))); };
        const auto tag = [&](const char* text) { file.insert(file.end(), text, text + 4); };
        const std::uint32_t frames = 10000;
        tag("RIFF"); le(36 + frames * 2, 4); tag("WAVE");
        tag("fmt "); le(16, 4); le(1, 2); le(1, 2); le(8000, 4); le(16000, 4); le(2, 2); le(16, 2);
        tag("data"); le(frames * 2, 4);
        for (std::uint32_t i = 0; i < frames; ++i) le(static_cast<std::uint16_t>(static_cast<std::int16_t>(i % 2 ? 8192 : -8192)), 2);
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    }
    const auto url = f.cf.file_url(alchemy::guest_path_string(path), false);
    const auto asset = f.send(f.cls("AVURLAsset"), "URLAssetWithURL:options:", {url, 0});
    require(asset != 0, "AVURLAsset");
    const auto tracks = f.send(asset, "tracksWithMediaType:", {f.cf.string("soun")});
    require(f.cf.array_values(tracks).size() == 1, "one audio track");
    require(f.cf.array_values(f.send(asset, "tracksWithMediaType:", {f.cf.string("vide")})).empty(), "no video track");
    const auto track = f.cf.array_values(tracks)[0];
    alchemy::CallRegs none{};
    const auto time = f.block(48);
    none.indirect_result = time;
    static_cast<void>(f.objc.send(asset, f.objc.selector("duration"), none));
    require(f.engine.read64(time) == 10000 && f.engine.read32(time + 8) == 8000, "duration 10000/8000");
    static_cast<void>(f.objc.send(track, f.objc.selector("timeRange"), none));
    require(f.engine.read64(time + 24) == 10000 && f.engine.read64(time) == 0, "track time range");
    require(f.cf.array_values(f.send(asset, "commonMetadata")).empty() && (f.send(asset, "hasProtectedContent") & 0xff) == 0, "no metadata, unprotected");
    const auto descriptions = f.cf.array_values(f.send(track, "formatDescriptions"));
    require(descriptions.size() == 1 && f.memory().read32(f.call(cm, "_CMAudioFormatDescriptionGetStreamBasicDescription", {descriptions[0]}) + 28) == 1, "track format");
    // A reader with float32 output settings.
    const auto error = f.block(8);
    const auto reader = f.send(f.cls("AVAssetReader"), "assetReaderWithAsset:error:", {asset, error});
    require(reader != 0, "AVAssetReader");
    std::vector<std::pair<Address, Address>> entries{
        {f.cf.string("AVFormatIDKey"), f.cf.integer(0x6c70636d, 3)}, {f.cf.string("AVLinearPCMBitDepthKey"), f.cf.integer(32, 3)},
        {f.cf.string("AVLinearPCMIsFloatKey"), f.cf.boolean(true)}, {f.cf.string("AVLinearPCMIsNonInterleaved"), f.cf.boolean(false)}};
    const auto settings = f.cf.dictionary(entries);
    const auto output = f.send(f.cls("AVAssetReaderTrackOutput"), "assetReaderTrackOutputWithTrack:outputSettings:", {track, settings});
    require((f.send(reader, "canAddOutput:", {output}) & 0xff) == 1, "canAddOutput:");
    static_cast<void>(f.send(reader, "addOutput:", {output}));
    // Read from 0.5 s for 0.5 s: 4000 frames.
    const auto range = f.block(48);
    f.engine.write64(range, 1); f.engine.write32(range + 8, 2); f.engine.write32(range + 12, 1); f.engine.write64(range + 16, 0);
    f.engine.write64(range + 24, 1); f.engine.write32(range + 32, 2); f.engine.write32(range + 36, 1); f.engine.write64(range + 40, 0);
    static_cast<void>(f.send(reader, "setTimeRange:", {range}));
    require((f.send(reader, "startReading") & 0xff) == 1 && f.send(reader, "status") == 1, "startReading");
    std::uint64_t frames = 0;
    bool first = true;
    while (const auto sample = f.send(output, "copyNextSampleBuffer")) {
        frames += f.call(cm, "_CMSampleBufferGetNumSamples", {sample});
        if (first) {
            const auto list = f.block(64), retained = f.block(8);
            require(f.status(cm, "_CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer", {sample, 0, list, 64, 0, 0, 0, retained}) == 0, "buffer list");
            const auto data = f.engine.read64(list + 16);
            require(std::bit_cast<float>(f.engine.read32(data)) == -0.25f && std::bit_cast<float>(f.engine.read32(data + 4)) == 0.25f, "float samples from frame 4000");
            alchemy::CallRegs pts{};
            pts.x[0] = sample;
            pts.indirect_result = time;
            static_cast<void>(f.regs(cm, "_CMSampleBufferGetPresentationTimeStamp", pts));
            require(f.engine.read64(time) == 4000 && f.engine.read32(time + 8) == 8000, "presentation time");
            first = false;
        }
        static_cast<void>(f.call("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "_CFRelease", {sample}));
    }
    require(frames == 4000 && f.send(reader, "status") == 2, "4000 frames, then completed");
    // Media Alchemy cannot read: no tracks, failed status, no reader, no images.
    const auto video_url = f.cf.file_url(alchemy::guest_path_string(directory / "missing.mov"), false);
    const auto video = f.send(f.cls("AVURLAsset"), "URLAssetWithURL:options:", {video_url, 0});
    require(f.cf.array_values(f.send(video, "tracks")).empty() && f.send(video, "statusOfValueForKey:error:", {f.cf.string("tracks"), error}) == 3, "unreadable media");
    require(f.send(f.cls("AVAssetReader"), "assetReaderWithAsset:error:", {video, error}) == 0 && f.engine.read64(error) != 0, "no reader for unreadable media");
    const auto generator = f.send(f.cls("AVAssetImageGenerator"), "assetImageGeneratorWithAsset:", {asset});
    require(f.send(generator, "copyCGImageAtTime:actualTime:error:", {time, 0, error}) == 0, "no images");
    // Value classes.
    alchemy::CallRegs standard{};
    standard.set_d(0, 48000.0);
    standard.x[2] = 2;
    const auto format = f.objc.send(f.send(f.cls("AVAudioFormat"), "alloc"), f.objc.selector("initStandardFormatWithSampleRate:channels:"), standard).x0;
    require(format && f.send(format, "channelCount") == 2 && (f.send(format, "isStandard") & 0xff) == 1 && f.send(format, "commonFormat") == 1, "standard format");
    const auto asbd = f.send(format, "streamDescription");
    require(f.engine.read32(asbd + 12) == (1 | 8 | 32) && f.engine.read32(asbd + 24) == 4, "deinterleaved float32 description");
    standard.x[2] = 6;
    require(f.objc.send(f.send(f.cls("AVAudioFormat"), "alloc"), f.objc.selector("initStandardFormatWithSampleRate:channels:"), standard).x0 == 0, "six channels need a layout");
    const auto layout = f.send(f.cls("AVAudioChannelLayout"), "layoutWithLayoutTag:", {(121U << 16) | 6});   // 5.1 (A)
    require(layout && f.send(layout, "channelCount") == 6, "channel layout");
    alchemy::CallRegs with_layout{};
    with_layout.x[2] = 1; with_layout.x[3] = 0; with_layout.x[4] = layout;
    with_layout.set_d(0, 48000.0);
    const auto surround = f.objc.send(f.send(f.cls("AVAudioFormat"), "alloc"), f.objc.selector("initWithCommonFormat:sampleRate:interleaved:channelLayout:"), with_layout).x0;
    require(surround && f.send(surround, "channelCount") == 6 && f.send(surround, "channelLayout") == layout, "format with a layout");
    const auto bus = f.send(f.send(f.cls("AUAudioUnitBus"), "alloc"), "initWithFormat:error:", {format, error});
    require(bus && (f.send(bus, "setFormat:error:", {surround, error}) & 0xff) == 1 && f.send(bus, "format") == surround, "AUAudioUnitBus");
    // No audio component exists.
    const auto manager = f.send(f.cls("AVAudioUnitComponentManager"), "sharedAudioUnitComponentManager");
    const auto description = f.block(20);
    require(manager && f.cf.array_values(f.send(manager, "componentsMatchingDescription:", {description})).empty(), "no components");
    require(f.send(f.send(f.cls("AUAudioUnit"), "alloc"), "initWithComponentDescription:error:", {description, error}) == 0 && f.engine.read64(error) != 0, "no AUAudioUnit");
    // Capture (no cameras here): discovery finds nothing; a session runs with an output and no input.
    const auto discovery = f.send(f.cls("AVCaptureDeviceDiscoverySession"), "discoverySessionWithDeviceTypes:mediaType:position:", {0, 0, 0});
    require(discovery && f.cf.array_values(f.send(discovery, "devices")).empty(), "no capture devices");
    const auto session = f.send(f.send(f.cls("AVCaptureSession"), "alloc"), "init");
    const auto data_output = f.send(f.send(f.cls("AVCaptureVideoDataOutput"), "alloc"), "init");
    static_cast<void>(f.send(session, "beginConfiguration"));
    require((f.send(session, "canAddOutput:", {data_output}) & 0xff) == 1, "canAddOutput:");
    static_cast<void>(f.send(session, "addOutput:", {data_output}));
    static_cast<void>(f.send(session, "setSessionPreset:", {f.cf.string("AVCaptureSessionPresetHigh")}));
    static_cast<void>(f.send(session, "commitConfiguration"));
    static_cast<void>(f.send(session, "startRunning"));
    require((f.send(session, "isRunning") & 0xff) == 1 && f.cf.array_values(f.send(session, "outputs")).size() == 1, "session runs");
    static_cast<void>(f.send(session, "stopRunning"));
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// vImage Y'CbCr <-> ARGB against hand-computed BT.601 / BT.709 video-range values.
void vimage(Fixture& f) {
    const auto range = f.block(32);
    const std::array<std::uint32_t, 8> video{16, 128, 235, 240, 235, 16, 240, 16};
    for (std::size_t i = 0; i < 8; ++i) f.engine.write32(range + 4 * i, video[i]);
    const auto matrix = [&](const char* name) { return f.engine.read64(f.symbol(acc, name)); };
    require(std::bit_cast<float>(f.engine.read32(matrix("_kvImage_YpCbCrToARGBMatrix_ITU_R_601_4") + 4)) == 1.402f, "601 Cr_R");
    const auto to601 = f.block(128), to709 = f.block(128), from601 = f.block(128);
    require(f.call(acc, "_vImageConvert_YpCbCrToARGB_GenerateConversion", {matrix("_kvImage_YpCbCrToARGBMatrix_ITU_R_601_4"), range, to601, 0, 0, 0}) == 0, "601 conversion");
    require(f.call(acc, "_vImageConvert_YpCbCrToARGB_GenerateConversion", {matrix("_kvImage_YpCbCrToARGBMatrix_ITU_R_709_2"), range, to709, 0, 0, 0}) == 0, "709 conversion");
    require(f.call(acc, "_vImageConvert_ARGBToYpCbCr_GenerateConversion", {matrix("_kvImage_ARGBToYpCbCrMatrix_ITU_R_601_4"), range, from601, 0, 0, 0}) == 0, "601 inverse");
    const auto buffer = [&](Address data, std::uint64_t height, std::uint64_t width, std::uint64_t row_bytes) {
        const auto at = f.block(32);
        f.engine.write64(at, data); f.engine.write64(at + 8, height); f.engine.write64(at + 16, width); f.engine.write64(at + 24, row_bytes);
        return at;
    };
    const auto bytes = [&](std::initializer_list<std::uint8_t> values) {
        const auto at = f.block(values.size());
        f.engine.write_bytes(at, std::vector<std::uint8_t>(values));
        return at;
    };
    const auto read = [&](Address at, std::size_t count) {
        std::vector<std::uint8_t> out(count);
        f.engine.read_bytes(at, out);
        return out;
    };
    // 4:2:2 BT.601 red (Cb 90, Y' 81, Cr 240): R = 0.2968 + 1.402 * 0.5 -> 254; G, B round to 0.
    const auto red422 = bytes({90, 81, 240, 81});
    const auto out = f.block(16);
    require(f.call(acc, "_vImageConvert_422CbYpCrYp8ToARGB8888", {buffer(red422, 1, 2, 4), buffer(out, 1, 2, 8), to601, 0, 255, 0}) == 0, "422 to ARGB");
    require((read(out, 8) == std::vector<std::uint8_t>{255, 254, 0, 0, 255, 254, 0, 0}), "601 red");
    const auto bgra = bytes({3, 2, 1, 0});
    require(f.call(acc, "_vImageConvert_422CbYpCrYp8ToARGB8888", {buffer(red422, 1, 2, 4), buffer(out, 1, 2, 8), to601, bgra, 7, 0}) == 0 &&
            (read(out, 4) == std::vector<std::uint8_t>{0, 0, 254, 7}), "permute to BGRA with alpha 7");
    // BT.709 red (Cb 102, Y' 63, Cr 240): R clips to 255, G = 0.0023 -> 1, B -> 0.
    const auto red709 = bytes({102, 63, 240, 63});
    require(f.call(acc, "_vImageConvert_422CbYpCrYp8ToARGB8888", {buffer(red709, 1, 2, 4), buffer(out, 1, 2, 8), to709, 0, 255, 0}) == 0 &&
            (read(out, 4) == std::vector<std::uint8_t>{255, 255, 1, 0}), "709 red");
    // 4:2:0 biplanar, neutral chroma: Y' 235 white, 16 black, 81 grey 76.
    const auto luma = bytes({235, 16, 81, 81}), chroma = bytes({128, 128});
    require(f.call(acc, "_vImageConvert_420Yp8_CbCr8ToARGB8888", {buffer(luma, 2, 2, 2), buffer(chroma, 1, 1, 2), buffer(out, 2, 2, 8), to601, 0, 255, 0}) == 0, "420 to ARGB");
    require((read(out, 16) == std::vector<std::uint8_t>{255, 255, 255, 255, 255, 0, 0, 0, 255, 76, 76, 76, 255, 76, 76, 76}), "420 greys");
    require(static_cast<std::int64_t>(f.call(acc, "_vImageConvert_420Yp8_CbCr8ToARGB8888", {buffer(luma, 2, 2, 2), buffer(chroma, 1, 1, 2), buffer(out, 4, 4, 16), to601, 0, 255, 0})) == -21766,
            "destination larger than the source");
    // ARGB to 4:2:2: red then white. Y'0 = 0.299 * 219 + 16 -> 81, Y'1 235, Cb = 128 - 0.0844 * 224 -> 109, Cr = 128 + 0.25 * 224 = 184.
    const auto argb = bytes({255, 255, 0, 0, 255, 255, 255, 255});
    require(f.call(acc, "_vImageConvert_ARGB8888To422CbYpCrYp8", {buffer(argb, 1, 2, 8), buffer(out, 1, 2, 4), from601, 0, 0}) == 0 &&
            (read(out, 4) == std::vector<std::uint8_t>{109, 81, 184, 235}), "ARGB to 422");
    require(static_cast<std::int64_t>(f.call(acc, "_vImageConvert_422CbYpCrYp8ToARGB8888", {buffer(red422, 1, 2, 4), buffer(out, 1, 2, 8), from601, 0, 255, 0})) == -21773,
            "the wrong conversion kind");
    const std::array<std::uint32_t, 8> bad{16, 128, 16, 240, 235, 16, 240, 16};
    for (std::size_t i = 0; i < 8; ++i) f.engine.write32(range + 4 * i, bad[i]);
    require(static_cast<std::int64_t>(f.call(acc, "_vImageConvert_YpCbCrToARGB_GenerateConversion", {matrix("_kvImage_YpCbCrToARGBMatrix_ITU_R_601_4"), range, to601, 0, 0, 0})) == -21773, "invalid range");
    require(f.string_constant(cv, "_kCVImageBufferFieldDetailKey") == "CVFieldDetail" && f.call(vt, "_VTDecompressionSessionCanAcceptFormatDescription", {0, 0}) == 0,
            "field keys; no decompression session");
}

// Compressed audio through the guest-facing APIs: minimp3's layer III conformance vector
// (third_party/minimp3/vectors) as an .mp3 file. AudioFile describes and reads its packets,
// ExtAudioFile and AudioConverter decode them, compared with the vector's reference output.
void compressed_audio(Fixture& f) {
    const auto directory = std::filesystem::temp_directory_path() / "alchemy_qlab_media_compressed";
    std::filesystem::create_directories(directory);
    const std::string vectors = std::string(ALCHEMY_THIRD_PARTY) + "/minimp3/vectors/";
    const auto path = directory / "hecommon.mp3";
    std::filesystem::copy_file(vectors + "l3-hecommon.bit", path, std::filesystem::copy_options::overwrite_existing);
    std::vector<std::int16_t> reference;
    {
        std::ifstream in(vectors + "l3-hecommon.pcm", std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
        reference.resize(bytes.size() / 2);
        std::memcpy(reference.data(), bytes.data(), reference.size() * 2);
    }
    const auto url = f.cf.file_url(alchemy::guest_path_string(path), false);
    const auto out = f.block(8), size = f.block(4), value = f.block(64);
    require(f.status(at, "_AudioFileOpenURL", {url, 1, 0, out}) == 0, "AudioFileOpenURL on an MP3 file");
    const auto file = f.engine.read64(out);
    f.engine.write32(size, 4);
    require(f.status(at, "_AudioFileGetProperty", {file, 0x66666d74, size, value}) == 0 && f.engine.read32(value) == 0x4d504733, "file type 'MPG3'");
    f.engine.write32(size, 40);
    require(f.status(at, "_AudioFileGetProperty", {file, 0x64666d74, size, value}) == 0 && f.engine.read32(value + 8) == 0x2e6d7033 &&
            f.engine.read32(value + 20) == 1152, "data format '.mp3', 1152 frames per packet");
    const auto rate = std::bit_cast<double>(f.engine.read64(value));
    const auto channels = f.engine.read32(value + 28);
    f.engine.write32(size, 8);
    require(f.status(at, "_AudioFileGetProperty", {file, 0x70636e74, size, value}) == 0, "packet count");   // 'pcnt'
    const auto packet_count = f.engine.read64(value);
    // The reference may be up to two frames shorter than the stream (minimp3's own test allows it).
    require(packet_count * 1152 * channels >= reference.size() && packet_count * 1152 * channels <= reference.size() + 2 * 1152 * channels,
            "every frame is a packet: " + std::to_string(packet_count));
    f.engine.write32(size, 16);
    require(f.status(at, "_AudioFileGetProperty", {file, 0x706e666f, size, value}) == 0 && f.engine.read64(value) == packet_count * 1152 &&
            f.engine.read32(value + 8) == 0, "packet table: no priming");   // 'pnfo'
    require(f.status(at, "_AudioFileReadBytes", {file, 0, 0, size, value}) == static_cast<std::int32_t>(0x6f703f3f), "ReadBytes is for PCM");
    // The packets with their descriptions: one MPEG frame each.
    const auto room = f.block(4), count = f.block(4), descriptions = f.block(16 * 64), packets = f.block(64 * 1500);
    f.engine.write32(room, 64 * 1500);
    f.engine.write32(count, static_cast<std::uint32_t>(packet_count));
    require(f.status(at, "_AudioFileReadPacketData", {file, 0, room, descriptions, 0, count, packets}) == 0 &&
            f.engine.read32(count) == packet_count, "ReadPacketData returns every packet");
    const auto first_size = f.engine.read32(descriptions + 12);
    require(f.engine.read64(descriptions) == 0 && f.engine.read64(descriptions + 16) == first_size && f.memory().read8(packets) == 0xff,
            "packet descriptions are contiguous in the buffer");

    // ExtAudioFile: float32 interleaved at the file's rate, compared with the reference.
    require(f.status(at, "_ExtAudioFileWrapAudioFileID", {file, 0, out}) == 0, "ExtAudioFileWrapAudioFileID on an MP3 file");
    const auto ext = f.engine.read64(out);
    const auto client = f.block(40);
    f.engine.write64(client, std::bit_cast<std::uint64_t>(rate));
    f.engine.write32(client + 8, 0x6c70636d);
    f.engine.write32(client + 12, 1 | 8);
    f.engine.write32(client + 16, 4 * channels); f.engine.write32(client + 20, 1); f.engine.write32(client + 24, 4 * channels);
    f.engine.write32(client + 28, channels); f.engine.write32(client + 32, 32);
    require(f.status(at, "_ExtAudioFileSetProperty", {ext, 0x63666d74, 40, client}) == 0, "float client format");
    const auto total = static_cast<std::uint32_t>(packet_count * 1152);
    const auto data = f.block(4ULL * total * channels), list = f.block(24), frames = f.block(4);
    f.engine.write32(list, 1);
    f.engine.write32(list + 8, channels); f.engine.write32(list + 12, 4 * total * channels); f.engine.write64(list + 16, data);
    f.engine.write32(frames, total);
    require(f.status(at, "_ExtAudioFileRead", {ext, frames, list}) == 0 && f.engine.read32(frames) == total, "ExtAudioFileRead decodes the file");
    const auto close_to_reference = [&](Address samples, std::size_t n, std::size_t from) {
        for (std::size_t i = 0; i < n; ++i)
            if (std::fabs(std::bit_cast<float>(f.engine.read32(samples + 4 * i)) * 32768.0f - reference[from + i]) > 2.0f) return false;
        return true;
    };
    require(close_to_reference(data, reference.size(), 0), "decoded MP3 matches the reference output");
    require(f.status(at, "_ExtAudioFileSeek", {ext, 10000}) == 0, "seek");
    f.engine.write32(frames, 500);
    f.engine.write32(list + 12, 4 * 500 * channels);
    require(f.status(at, "_ExtAudioFileRead", {ext, frames, list}) == 0 && f.engine.read32(frames) == 500 &&
            close_to_reference(data, 500ULL * channels, 10000ULL * channels), "a read after a seek is sample-accurate");
    require(f.status(at, "_ExtAudioFileDispose", {ext}) == 0, "ExtAudioFileDispose");

    // AudioConverter: the packets in (with descriptions), float out.
    const auto mp3 = f.block(40), pcm = f.block(40), converter_out = f.block(8);
    f.engine.write64(mp3, std::bit_cast<std::uint64_t>(rate));
    f.engine.write32(mp3 + 8, 0x2e6d7033);
    f.engine.write32(mp3 + 20, 1152);
    f.engine.write32(mp3 + 28, channels);
    for (std::uint32_t i = 0; i < 10; ++i) f.engine.write32(pcm + 8 + 4 * i, f.engine.read32(client + 8 + 4 * i));
    f.engine.write64(pcm, std::bit_cast<std::uint64_t>(rate));
    require(f.status(at, "_AudioConverterNew", {mp3, pcm, converter_out}) == 0, "AudioConverterNew from MP3");
    const auto converter = f.engine.read64(converter_out);
    static Address feed_packets{}, feed_descriptions{}, feed_one{};
    static std::uint32_t feed_next{}, feed_total{};
    feed_packets = packets;
    feed_descriptions = descriptions;
    feed_one = f.block(16);
    feed_next = 0;
    feed_total = static_cast<std::uint32_t>(packet_count);
    // One packet per call, from the buffer ReadPacketData filled, with its own description.
    const auto input = f.process.install_function("_alchemy_test_mp3_input", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        if (feed_next >= feed_total) {
            m.write32(s.x[1], 0);
            s.x[0] = 0;
            return;
        }
        const auto entry = feed_descriptions + 16ULL * feed_next++;
        const auto bytes = m.read32(entry + 12);
        m.write32(s.x[1], 1);
        m.write32(s.x[2] + 8 + 4, bytes);
        m.write64(s.x[2] + 8 + 8, feed_packets + m.read64(entry));
        m.write64(feed_one, 0);
        m.write32(feed_one + 8, 0);
        m.write32(feed_one + 12, bytes);
        if (s.x[3]) m.write64(s.x[3], feed_one);
        s.x[0] = 0;
    }, "/usr/lib/alchemy_test_io.dylib");
    f.engine.write32(frames, total);
    f.engine.write32(list + 12, 4 * total * channels);
    require(f.status(at, "_AudioConverterFillComplexBuffer", {converter, input, 0, frames, list, 0}) == 0 && f.engine.read32(frames) == total,
            "FillComplexBuffer decodes every packet: " + std::to_string(f.engine.read32(frames)));
    require(close_to_reference(data, reference.size(), 0), "AudioConverter output matches the reference output");
    require(f.status(at, "_AudioConverterDispose", {converter}) == 0 && f.status(at, "_AudioFileClose", {file}) == 0, "dispose and close");

    // AVAudioFile: an MP3 opens, with its length in frames.
    const auto av_file = f.send(f.send(f.cls("AVAudioFile"), "alloc"), "initForReading:error:", {url, 0});
    require(av_file && static_cast<std::uint32_t>(f.send(av_file, "length")) == total, "AVAudioFile reads MP3 lengths");
    f.send(av_file, "release");
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// Apple Lossless files (stereo 16-bit, encoded here by the ALAC encoder, in CAF and M4A) through AudioFile,
// ExtAudioFile, AudioConverter, AVAudioFile and AVAssetReader, compared sample for sample.
void alac_files(Fixture& f) {
    namespace codecs = alchemy::gui::completion_kit::media::codecs;
    const auto directory = std::filesystem::temp_directory_path() / "alchemy_qlab_media_alac";
    std::filesystem::create_directories(directory);
    constexpr std::uint32_t frames = 4096 * 2 + 1000, channels = 2;
    std::vector<std::int32_t> signal(std::size_t(frames) * channels);
    for (std::uint32_t i = 0; i < frames; ++i)
        for (std::uint32_t c = 0; c < channels; ++c)
            signal[std::size_t(i) * channels + c] = std::int32_t(std::lround(20000.0 * std::sin(0.01 * (c + 1) * i))) << 16;
    std::string why;
    auto encoder = codecs::make_audio_encoder(alchemy::gui::completion_kit::media::container::fourcc("alac"), 44100, channels, 16, 0, why);
    require(encoder != nullptr, "an ALAC encoder: " + why);
    std::vector<std::vector<std::uint8_t>> packets;
    for (std::size_t at = 0; at < signal.size(); at += 4096 * channels) {
        std::vector<std::uint8_t> packet;
        require(encoder->encode(std::span(signal).subspan(at, std::min<std::size_t>(4096 * channels, signal.size() - at)), packet, why), "encodes: " + why);
        packets.push_back(std::move(packet));
    }
    const auto cookie = encoder->cookie();
    const auto be = [](std::vector<std::uint8_t>& out, std::uint64_t value, int bytes) { for (int i = bytes - 1; i >= 0; --i) out.push_back(std::uint8_t(value >> (8 * i))); };
    // CAF: desc, kuki, pakt (BER sizes), data.
    std::vector<std::uint8_t> caf{'c', 'a', 'f', 'f', 0, 1, 0, 0};
    const auto chunk = [&](const char* type, const std::vector<std::uint8_t>& body) { caf.insert(caf.end(), type, type + 4); be(caf, body.size(), 8); caf.insert(caf.end(), body.begin(), body.end()); };
    std::vector<std::uint8_t> desc;
    be(desc, std::bit_cast<std::uint64_t>(44100.0), 8); desc.insert(desc.end(), {'a', 'l', 'a', 'c'}); be(desc, 1, 4); be(desc, 0, 4); be(desc, 4096, 4); be(desc, channels, 4); be(desc, 0, 4);
    chunk("desc", desc);
    chunk("kuki", cookie);
    std::vector<std::uint8_t> pakt, data(4, 0);
    be(pakt, packets.size(), 8); be(pakt, frames, 8); be(pakt, 0, 4); be(pakt, packets.size() * 4096 - frames, 4);
    for (const auto& p : packets) {
        std::vector<std::uint8_t> ber{std::uint8_t(p.size() & 0x7f)};
        for (auto v = p.size() >> 7; v; v >>= 7) ber.insert(ber.begin(), std::uint8_t(0x80 | (v & 0x7f)));
        pakt.insert(pakt.end(), ber.begin(), ber.end());
        data.insert(data.end(), p.begin(), p.end());
    }
    chunk("pakt", pakt);
    chunk("data", data);
    const auto caf_path = directory / "tone.caf";
    std::ofstream(caf_path, std::ios::binary).write(reinterpret_cast<const char*>(caf.data()), std::streamsize(caf.size()));
    const auto expected = [&](std::size_t sample) { return float(signal[sample] >> 16) / 32768.0f; };

    // AudioFile: the packets as the file holds them.
    const auto url = f.cf.file_url(alchemy::guest_path_string(caf_path), false);
    const auto out = f.block(8), size = f.block(4), value = f.block(64);
    require(f.status(at, "_AudioFileOpenURL", {url, 1, 0, out}) == 0, "AudioFileOpenURL on an ALAC CAF");
    const auto file = f.engine.read64(out);
    f.engine.write32(size, 4);
    require(f.status(at, "_AudioFileGetProperty", {file, 0x66666d74, size, value}) == 0 && f.engine.read32(value) == 0x63616666, "file type 'caff'");
    f.engine.write32(size, 40);
    require(f.status(at, "_AudioFileGetProperty", {file, 0x64666d74, size, value}) == 0 && f.engine.read32(value + 8) == 0x616c6163 &&
            f.engine.read32(value + 12) == 1 && f.engine.read32(value + 20) == 4096 && f.engine.read32(value + 28) == channels, "data format 'alac', 16-bit source");
    f.engine.write32(size, 64);
    require(f.status(at, "_AudioFileGetProperty", {file, 0x6d676963, size, value}) == 0 && f.engine.read32(size) == cookie.size() &&
            f.engine.read32(value) == 0x00100000, "the magic cookie");   // frameLength 4096, big endian
    f.engine.write32(size, 8);
    require(f.status(at, "_AudioFileGetProperty", {file, 0x70636e74, size, value}) == 0 && f.engine.read64(value) == packets.size(), "three packets");
    f.engine.write32(size, 16);
    require(f.status(at, "_AudioFileGetProperty", {file, 0x706e666f, size, value}) == 0 && f.engine.read64(value) == frames && f.engine.read32(value + 8) == 0 &&
            f.engine.read32(value + 12) == packets.size() * 4096 - frames, "packet table: valid frames and remainder");
    const auto room = f.block(4), count = f.block(4), descriptions = f.block(16 * 3), packet_bytes = f.block(64 * 1024);
    f.engine.write32(room, 64 * 1024);
    f.engine.write32(count, 3);
    require(f.status(at, "_AudioFileReadPacketData", {file, 0, room, descriptions, 0, count, packet_bytes}) == 0 && f.engine.read32(count) == 3 &&
            f.engine.read32(descriptions + 12) == packets[0].size() && f.engine.read32(descriptions + 16 + 12) == packets[1].size(), "ReadPacketData");

    // ExtAudioFile: float32 interleaved, exactly the source samples.
    require(f.status(at, "_ExtAudioFileWrapAudioFileID", {file, 0, out}) == 0, "ExtAudioFileWrapAudioFileID");
    const auto ext = f.engine.read64(out);
    const auto client = f.block(40);
    f.engine.write64(client, std::bit_cast<std::uint64_t>(44100.0));
    f.engine.write32(client + 8, 0x6c70636d); f.engine.write32(client + 12, 1 | 8);
    f.engine.write32(client + 16, 4 * channels); f.engine.write32(client + 20, 1); f.engine.write32(client + 24, 4 * channels);
    f.engine.write32(client + 28, channels); f.engine.write32(client + 32, 32);
    require(f.status(at, "_ExtAudioFileSetProperty", {ext, 0x63666d74, 40, client}) == 0, "float client format");
    const auto samples = f.block(4ULL * frames * channels), list = f.block(24), got = f.block(4);
    f.engine.write32(list, 1); f.engine.write32(list + 8, channels); f.engine.write32(list + 12, 4 * frames * channels); f.engine.write64(list + 16, samples);
    f.engine.write32(got, frames + 100);
    require(f.status(at, "_ExtAudioFileRead", {ext, got, list}) == 0 && f.engine.read32(got) == frames, "ExtAudioFileRead decodes every frame");
    const auto exact = [&](Address at_samples, std::size_t n, std::size_t from) {
        for (std::size_t i = 0; i < n; ++i)
            if (std::bit_cast<float>(f.engine.read32(at_samples + 4 * i)) != expected(from + i)) return false;
        return true;
    };
    require(exact(samples, std::size_t(frames) * channels, 0), "ExtAudioFile: the source samples");
    require(f.status(at, "_ExtAudioFileSeek", {ext, 5000}) == 0, "seek");
    f.engine.write32(got, 300);
    require(f.status(at, "_ExtAudioFileRead", {ext, got, list}) == 0 && f.engine.read32(got) == 300 && exact(samples, 300 * channels, 5000 * channels), "a read after a seek");
    require(f.status(at, "_ExtAudioFileDispose", {ext}) == 0, "ExtAudioFileDispose");

    // AudioConverter: the packets in with the cookie set, float out.
    const auto alac_format = f.block(40), pcm = f.block(40), converter_out = f.block(8);
    f.engine.write64(alac_format, std::bit_cast<std::uint64_t>(44100.0));
    f.engine.write32(alac_format + 8, 0x616c6163); f.engine.write32(alac_format + 12, 1); f.engine.write32(alac_format + 20, 4096); f.engine.write32(alac_format + 28, channels);
    for (std::uint32_t i = 0; i < 40; i += 4) f.engine.write32(pcm + i, f.engine.read32(client + i));
    require(f.status(at, "_AudioConverterNew", {alac_format, pcm, converter_out}) == 0, "AudioConverterNew from ALAC");
    const auto converter = f.engine.read64(converter_out);
    const auto cookie_at = f.block(cookie.size());
    f.memory().write_bytes(cookie_at, cookie);
    require(f.status(at, "_AudioConverterSetProperty", {converter, 0x646d6763, static_cast<std::uint64_t>(cookie.size()), cookie_at}) == 0, "the decompression cookie");
    static Address alac_packets{}, alac_descriptions{}, alac_one{};
    static std::uint32_t alac_next{};
    alac_packets = packet_bytes;
    alac_descriptions = descriptions;
    alac_one = f.block(16);
    alac_next = 0;
    const auto input = f.process.install_function("_alchemy_test_alac_input", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        if (alac_next >= 3) { m.write32(s.x[1], 0); s.x[0] = 0; return; }
        const auto entry = alac_descriptions + 16ULL * alac_next++;
        const auto bytes = m.read32(entry + 12);
        m.write32(s.x[1], 1);
        m.write32(s.x[2] + 8 + 4, bytes);
        m.write64(s.x[2] + 8 + 8, alac_packets + m.read64(entry));
        m.write64(alac_one, 0); m.write32(alac_one + 8, 0); m.write32(alac_one + 12, bytes);
        if (s.x[3]) m.write64(s.x[3], alac_one);
        s.x[0] = 0;
    }, "/usr/lib/alchemy_test_io.dylib");
    f.engine.write32(got, frames);
    f.engine.write32(list + 12, 4 * frames * channels);
    require(f.status(at, "_AudioConverterFillComplexBuffer", {converter, input, 0, got, list, 0}) == 0 && f.engine.read32(got) >= frames &&
            exact(samples, std::size_t(frames) * channels, 0), "AudioConverter decodes ALAC packets: " + std::to_string(f.engine.read32(got)));
    require(f.status(at, "_AudioConverterDispose", {converter}) == 0 && f.status(at, "_AudioFileClose", {file}) == 0, "dispose and close");

    // AVAudioFile and AVAssetReader on the same file.
    const auto error = f.block(8);
    const auto av_file = f.send(f.send(f.cls("AVAudioFile"), "alloc"), "initForReading:error:", {url, error});
    require(av_file && static_cast<std::uint32_t>(f.send(av_file, "length")) == frames, "AVAudioFile reads ALAC lengths");
    const auto processing = f.send(av_file, "processingFormat");
    const auto buffer = f.send(f.send(f.cls("AVAudioPCMBuffer"), "alloc"), "initWithPCMFormat:frameCapacity:", {processing, 1000});
    require((f.send(av_file, "readIntoBuffer:error:", {buffer, error}) & 0xff) == 1 && f.send(buffer, "frameLength") == 1000, "AVAudioFile reads 1000 frames");
    const auto planes = f.send(buffer, "floatChannelData");
    const auto right = f.engine.read64(planes + 8);
    require(std::bit_cast<float>(f.engine.read32(right + 4 * 777)) == expected(777 * channels + 1), "AVAudioFile: the right channel");
    f.send(av_file, "release");
    const auto asset = f.send(f.cls("AVURLAsset"), "URLAssetWithURL:options:", {url, 0});
    const auto tracks = f.cf.array_values(f.send(asset, "tracksWithMediaType:", {f.cf.string("soun")}));
    require(tracks.size() == 1 && (f.send(asset, "isReadable") & 0xff) == 1, "AVURLAsset: one readable ALAC track");
    const auto reader = f.send(f.cls("AVAssetReader"), "assetReaderWithAsset:error:", {asset, error});
    const auto output = f.send(f.cls("AVAssetReaderAudioMixOutput"), "assetReaderAudioMixOutputWithAudioTracks:audioSettings:", {f.cf.array(std::vector<Address>{tracks[0]}), 0});
    static_cast<void>(f.send(reader, "addOutput:", {output}));
    require((f.send(reader, "startReading") & 0xff) == 1, "AVAssetReader starts");
    std::size_t read_frames = 0;
    bool same = true;
    while (const auto sample = f.send(output, "copyNextSampleBuffer")) {
        const auto block_list = f.block(64), retained = f.block(8);
        require(f.status(cm, "_CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer", {sample, 0, block_list, 64, 0, 0, 0, retained}) == 0, "buffer list");
        const auto bytes = f.engine.read32(block_list + 12);
        same = same && exact(f.engine.read64(block_list + 16), bytes / 4, read_frames * channels);
        read_frames += bytes / 4 / channels;
        static_cast<void>(f.call("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "_CFRelease", {f.engine.read64(retained)}));
        static_cast<void>(f.call("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "_CFRelease", {sample}));
    }
    require(read_frames == frames && same, "AVAssetReader: every frame, exactly: " + std::to_string(read_frames));
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// Writing Apple Lossless: AudioConverter encodes 16-bit PCM into packets; AudioFile writes them with the cookie into a
// CAF and an M4A; ExtAudioFile writes 24-bit frames into an M4A in uneven pieces; AVAudioFile writes a CAF. Every
// file reads back exactly through ExtAudioFile.
void alac_writing(Fixture& f) {
    const auto directory = std::filesystem::temp_directory_path() / "alchemy_qlab_media_alac_writing";
    std::filesystem::create_directories(directory);
    constexpr std::uint32_t frames = 4096 * 2 + 1000, channels = 2;
    const auto pcm = f.block(4ULL * frames * channels);
    std::vector<std::int16_t> source(std::size_t(frames) * channels);
    std::vector<std::uint8_t> source_bytes(source.size() * 2);
    for (std::uint32_t i = 0; i < frames; ++i)
        for (std::uint32_t c = 0; c < channels; ++c)
            source[std::size_t(i) * channels + c] = static_cast<std::int16_t>(std::lround(20000.0 * std::sin(0.013 * (c + 1) * i)));
    std::memcpy(source_bytes.data(), source.data(), source_bytes.size());
    f.memory().write_bytes(pcm, source_bytes);
    const auto write_asbd = [&](Address at_, double rate, std::uint32_t id, std::uint32_t flags, std::uint32_t bytes_per_packet, std::uint32_t per_packet,
                                std::uint32_t bytes_per_frame, std::uint32_t bits) {
        f.engine.write64(at_, std::bit_cast<std::uint64_t>(rate));
        f.engine.write32(at_ + 8, id); f.engine.write32(at_ + 12, flags); f.engine.write32(at_ + 16, bytes_per_packet); f.engine.write32(at_ + 20, per_packet);
        f.engine.write32(at_ + 24, bytes_per_frame); f.engine.write32(at_ + 28, channels); f.engine.write32(at_ + 32, bits); f.engine.write32(at_ + 36, 0);
    };
    const auto int16 = f.block(40), alac = f.block(40), out = f.block(8), size = f.block(4);
    write_asbd(int16, 44100, 0x6c70636d, 4 | 8, 4, 1, 4, 16);   // signed, packed
    write_asbd(alac, 44100, 0x616c6163, 0, 0, 0, 0, 0);          // depth, frames per packet: the converter's choice

    // AudioConverter: 16-bit PCM in through the input procedure, packets out with descriptions.
    require(f.status(at, "_AudioConverterNew", {int16, alac, out}) == 0, "AudioConverterNew to Apple Lossless");
    const auto converter = f.engine.read64(out);
    const auto described = f.block(40);
    f.engine.write32(size, 40);
    require(f.status(at, "_AudioConverterGetProperty", {converter, 0x61636f64, size, described}) == 0 && f.engine.read32(described + 12) == 1 &&
            f.engine.read32(described + 20) == 4096, "the output: 16-bit source, 4096 frames per packet");
    f.engine.write32(size, 4);
    const auto max_packet = f.block(4);
    require(f.status(at, "_AudioConverterGetProperty", {converter, 0x786f7073, size, max_packet}) == 0 && f.engine.read32(max_packet) > 0, "the largest packet");
    static Address pcm_at{};
    static std::uint32_t pcm_next{}, pcm_frames{};
    pcm_at = pcm;
    pcm_next = 0;
    pcm_frames = frames;
    const auto input = f.process.install_function("_alchemy_test_alac_pcm_input", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        const auto n = std::min<std::uint32_t>(m.read32(s.x[1]), std::min<std::uint32_t>(1500, pcm_frames - pcm_next));   // uneven pieces
        m.write32(s.x[1], n);
        m.write32(s.x[2] + 8 + 4, n * 4);
        m.write64(s.x[2] + 8 + 8, pcm_at + 4ULL * pcm_next);
        pcm_next += n;
        s.x[0] = 0;
    }, "/usr/lib/alchemy_test_io.dylib");
    const auto room = 3 * f.engine.read32(max_packet);
    const auto packets = f.block(room), descriptions = f.block(16 * 3), list = f.block(24), count = f.block(4);
    f.engine.write32(list, 1); f.engine.write32(list + 8, channels); f.engine.write32(list + 12, room); f.engine.write64(list + 16, packets);
    f.engine.write32(count, 3);
    require(f.status(at, "_AudioConverterFillComplexBuffer", {converter, input, 0, count, list, descriptions}) == 0 && f.engine.read32(count) == 3,
            "three packets: " + std::to_string(f.engine.read32(count)));
    const auto bytes = f.engine.read32(list + 12);
    require(f.engine.read32(descriptions + 32 + 8) == 1000 && f.engine.read64(descriptions + 32) + f.engine.read32(descriptions + 32 + 12) == bytes,
            "the last packet is 1000 frames; the descriptions cover the bytes");
    f.engine.write32(count, 1);
    f.engine.write32(list + 12, room);
    require(f.status(at, "_AudioConverterFillComplexBuffer", {converter, input, 0, count, list, f.block(16)}) == 0 && f.engine.read32(count) == 0,
            "nothing after the end");
    f.engine.write32(list + 12, bytes);
    const auto cookie = f.block(64);
    f.engine.write32(size, 64);
    require(f.status(at, "_AudioConverterGetProperty", {converter, 0x636d6763, size, cookie}) == 0 && f.engine.read32(size) == 24, "the compression cookie");
    const auto cookie_size = f.engine.read32(size);
    require(f.status(at, "_AudioConverterDispose", {converter}) == 0, "dispose");

    // Reads a file back through ExtAudioFile as 16-bit integers: the source, exactly.
    const auto read_back = [&](const std::filesystem::path& path, const std::string& what) {
        const auto url = f.cf.file_url(alchemy::guest_path_string(path), false);
        require(f.status(at, "_ExtAudioFileOpenURL", {url, out}) == 0, what + ": opens");
        const auto ext = f.engine.read64(out);
        require(f.status(at, "_ExtAudioFileSetProperty", {ext, 0x63666d74, 40, int16}) == 0, what + ": a 16-bit client format");
        const auto data = f.block(4ULL * (frames + 100)), back = f.block(24), got = f.block(4);
        f.engine.write32(back, 1); f.engine.write32(back + 8, channels); f.engine.write32(back + 12, 4 * (frames + 100)); f.engine.write64(back + 16, data);
        f.engine.write32(got, frames + 100);
        require(f.status(at, "_ExtAudioFileRead", {ext, got, back}) == 0 && f.engine.read32(got) == frames, what + ": every frame: " + std::to_string(f.engine.read32(got)));
        std::vector<std::uint8_t> got_bytes(source_bytes.size());
        f.memory().read_bytes(data, got_bytes);
        require(got_bytes == source_bytes, what + ": the source samples, exactly");
        require(f.status(at, "_ExtAudioFileDispose", {ext}) == 0, what + ": dispose");
    };

    // AudioFile: the converter's packets, the cookie and the packet table into a CAF and an M4A.
    for (const auto& [type, name] : {std::pair{0x63616666u, "packets.caf"}, std::pair{0x6d346166u, "packets.m4a"}}) {
        const auto path = directory / name;
        const auto url = f.cf.file_url(alchemy::guest_path_string(path), false);
        write_asbd(alac, 44100, 0x616c6163, 1, 0, 4096, 0, 0);
        require(f.status(at, "_AudioFileCreateWithURL", {url, type, alac, 1, out}) == 0, std::string(name) + ": AudioFileCreateWithURL");
        const auto file = f.engine.read64(out);
        require(f.status(at, "_AudioFileSetProperty", {file, 0x6d676963, cookie_size, cookie}) == 0, std::string(name) + ": the magic cookie");
        f.engine.write32(count, 3);
        require(f.status(at, "_AudioFileWritePackets", {file, 0, bytes, descriptions, 1, count, packets}) == static_cast<std::int32_t>(0x70636b3f),
                std::string(name) + ": packets go in at the next packet only");
        require(f.status(at, "_AudioFileWritePackets", {file, 0, bytes, descriptions, 0, count, packets}) == 0 && f.engine.read32(count) == 3,
                std::string(name) + ": WritePackets");
        f.engine.write32(size, 8);
        require(f.status(at, "_AudioFileGetProperty", {file, 0x70636e74, size, out}) == 0 && f.engine.read64(out) == 3, std::string(name) + ": three packets");
        require(f.status(at, "_AudioFileClose", {file}) == 0, std::string(name) + ": close");
        read_back(path, name);
    }

    // ExtAudioFile: 16-bit frames in uneven pieces into an M4A, encoded as they come.
    {
        const auto path = directory / "ext.m4a";
        const auto url = f.cf.file_url(alchemy::guest_path_string(path), false);
        write_asbd(alac, 44100, 0x616c6163, 1, 0, 4096, 0, 0);
        require(f.status(at, "_ExtAudioFileCreateWithURL", {url, 0x6d346166, alac, 0, 1, out}) == 0, "ExtAudioFileCreateWithURL (M4A, ALAC)");
        const auto ext = f.engine.read64(out);
        require(f.status(at, "_ExtAudioFileSetProperty", {ext, 0x63666d74, 40, int16}) == 0, "a 16-bit client format");
        const auto piece = f.block(24);
        for (std::uint32_t at_frame = 0; at_frame < frames;) {
            const auto n = std::min<std::uint32_t>(frames - at_frame, 3333);
            f.engine.write32(piece, 1); f.engine.write32(piece + 8, channels); f.engine.write32(piece + 12, n * 4); f.engine.write64(piece + 16, pcm + 4ULL * at_frame);
            require(f.status(at, "_ExtAudioFileWrite", {ext, n, piece}) == 0, "ExtAudioFileWrite");
            at_frame += n;
        }
        f.engine.write32(size, 8);
        require(f.status(at, "_ExtAudioFileGetProperty", {ext, 0x2366726d, size, out}) == 0 && f.engine.read64(out) == frames, "FileLengthFrames while writing");
        require(f.status(at, "_ExtAudioFileDispose", {ext}) == 0, "ExtAudioFileDispose writes the last packet and the movie");
        read_back(path, "ExtAudioFile M4A");
    }

    // AVAudioFile: Apple Lossless settings into a CAF, from float buffers.
    {
        const auto path = directory / "avaudiofile.caf";
        const auto url = f.cf.file_url(alchemy::guest_path_string(path), false);
        const auto settings = f.cf.dictionary(std::vector<std::pair<Address, Address>>{
            {f.cf.string("AVFormatIDKey"), f.cf.integer(0x616c6163, 3)}, {f.cf.string("AVSampleRateKey"), f.cf.real(44100)},
            {f.cf.string("AVNumberOfChannelsKey"), f.cf.integer(channels, 3)}, {f.cf.string("AVEncoderBitDepthHintKey"), f.cf.integer(16, 3)}});
        const auto error = f.block(8);
        const auto file = f.send(f.send(f.cls("AVAudioFile"), "alloc"), "initForWriting:settings:error:", {url, settings, error});
        require(file != 0, "AVAudioFile writes Apple Lossless");
        require(f.engine.read32(f.send(f.send(file, "fileFormat"), "streamDescription") + 8) == 0x616c6163, "its file format is Apple Lossless");
        const auto buffer = f.send(f.send(f.cls("AVAudioPCMBuffer"), "alloc"), "initWithPCMFormat:frameCapacity:", {f.send(file, "processingFormat"), frames});
        const auto planes = f.send(buffer, "floatChannelData");
        for (std::uint32_t c = 0; c < channels; ++c) {
            const auto plane = f.engine.read64(planes + 8ULL * c);
            for (std::uint32_t i = 0; i < frames; ++i)
                f.engine.write32(plane + 4ULL * i, std::bit_cast<std::uint32_t>(static_cast<float>(source[std::size_t(i) * channels + c]) / 32768.0f));
        }
        f.send(buffer, "setFrameLength:", {frames});
        require((f.send(file, "writeFromBuffer:error:", {buffer, error}) & 0xff) == 1, "writeFromBuffer:error:");
        f.send(file, "release");
        read_back(path, "AVAudioFile CAF");
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// AAC through the fake encoder (fake_aac.hpp: 2048 frames of delay, 8-byte packets): AudioConverter's packets, delay,
// cookie and bit rates; ExtAudioFile and AVAudioFile writing M4A and CAF files, read back by the container parser
// (Alchemy decodes no AAC).
void aac_encoding(Fixture& f) {
    namespace mc = alchemy::gui::completion_kit::media::container;
    const auto directory = std::filesystem::temp_directory_path() / "alchemy_qlab_media_aac";
    std::filesystem::create_directories(directory);
    constexpr std::uint32_t frames = 10000, channels = 2, packets_expected = (fake_aac::priming + frames + 1023) / 1024;
    std::vector<std::uint8_t> source(std::size_t(frames) * channels * 2);
    for (std::uint32_t i = 0; i < frames * channels; ++i) {
        const auto v = static_cast<std::int16_t>(std::lround(12000.0 * std::sin(0.02 * i)));
        source[2 * i] = static_cast<std::uint8_t>(v);
        source[2 * i + 1] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(v) >> 8);
    }
    const auto pcm = f.block(source.size());
    f.memory().write_bytes(pcm, source);
    const auto int16 = f.block(40), aac_format = f.block(40), out = f.block(8), size = f.block(4), value = f.block(256);
    const auto write_asbd = [&](Address at_, double rate, std::uint32_t id, std::uint32_t flags, std::uint32_t bytes_per_packet, std::uint32_t per_packet,
                                std::uint32_t bytes_per_frame, std::uint32_t count, std::uint32_t bits) {
        f.engine.write64(at_, std::bit_cast<std::uint64_t>(rate));
        f.engine.write32(at_ + 8, id); f.engine.write32(at_ + 12, flags); f.engine.write32(at_ + 16, bytes_per_packet); f.engine.write32(at_ + 20, per_packet);
        f.engine.write32(at_ + 24, bytes_per_frame); f.engine.write32(at_ + 28, count); f.engine.write32(at_ + 32, bits); f.engine.write32(at_ + 36, 0);
    };
    write_asbd(int16, 48000, 0x6c70636d, 4 | 8, 4, 1, 4, channels, 16);
    write_asbd(aac_format, 0, 0x61616320, 0, 0, 0, 0, 0, 0);   // rate and channels: the input's

    // AudioConverter.
    require(f.status(at, "_AudioConverterNew", {int16, aac_format, out}) == 0, "AudioConverterNew to AAC");
    const auto converter = f.engine.read64(out);
    f.engine.write32(size, 40);
    require(f.status(at, "_AudioConverterGetProperty", {converter, 0x61636f64, size, value}) == 0 && std::bit_cast<double>(f.engine.read64(value)) == 48000 &&
            f.engine.read32(value + 12) == 2 && f.engine.read32(value + 20) == 1024 && f.engine.read32(value + 28) == channels, "AAC-LC, 1024 frames a packet");
    f.engine.write32(size, 256);
    require(f.status(at, "_AudioConverterGetProperty", {converter, 0x61657372, size, value}) == 0 && f.engine.read32(size) == 32 &&
            std::bit_cast<double>(f.engine.read64(value + 16)) == 48000, "the encode sample rates");
    f.engine.write32(size, 256);
    require(f.status(at, "_AudioConverterGetProperty", {converter, 0x61656272, size, value}) == 0 && f.engine.read32(size) == 48, "three encode bit rates");
    f.engine.write32(value, 100000);
    require(f.status(at, "_AudioConverterSetProperty", {converter, 0x62726174, 4, value}) == 0, "set the bit rate");
    f.engine.write32(size, 4);
    require(f.status(at, "_AudioConverterGetProperty", {converter, 0x62726174, size, value}) == 0 && f.engine.read32(value) == 96000, "the nearest bit rate");
    f.engine.write32(size, 4);
    require(f.status(at, "_AudioConverterGetProperty", {converter, 0x786f7073, size, value}) == 0 && f.engine.read32(value) == 1536, "the largest packet");
    static Address aac_pcm{};
    static std::uint32_t aac_next{};
    aac_pcm = pcm;
    aac_next = 0;
    const auto input = f.process.install_function("_alchemy_test_aac_input", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        const auto n = std::min<std::uint32_t>(m.read32(s.x[1]), std::min<std::uint32_t>(777, frames - aac_next));
        m.write32(s.x[1], n);
        m.write32(s.x[2] + 8 + 4, n * 4);
        m.write64(s.x[2] + 8 + 8, aac_pcm + 4ULL * aac_next);
        aac_next += n;
        s.x[0] = 0;
    }, "/usr/lib/alchemy_test_io.dylib");
    const auto buffer = f.block(64 * 1536), descriptions = f.block(16 * 64), list = f.block(24), count = f.block(4);
    f.engine.write32(list, 1); f.engine.write32(list + 8, channels); f.engine.write32(list + 12, 64 * 1536); f.engine.write64(list + 16, buffer);
    f.engine.write32(count, 64);
    require(f.status(at, "_AudioConverterFillComplexBuffer", {converter, input, 0, count, list, descriptions}) == 0 && f.engine.read32(count) == packets_expected,
            "a packet per 1024 frames of delay and input: " + std::to_string(f.engine.read32(count)));
    require(f.engine.read32(descriptions + 12) == 8 && f.engine.read32(buffer) == 0 && f.engine.read32(buffer + 4) == 0x21434141 &&
            f.engine.read64(descriptions + 16 * 11) == 11 * 8 && f.engine.read32(buffer + 11 * 8) == 11, "the packets in order, described");
    f.engine.write32(size, 8);
    require(f.status(at, "_AudioConverterGetProperty", {converter, 0x7072696d, size, value}) == 0 && f.engine.read32(value) == fake_aac::priming, "the delay leads");
    f.engine.write32(size, 256);
    require(f.status(at, "_AudioConverterGetProperty", {converter, 0x636d6763, size, value}) == 0 && f.memory().read8(value) == 0x03, "the ES descriptor cookie");
    std::vector<std::uint8_t> cookie(f.engine.read32(size));
    f.memory().read_bytes(value, cookie);
    const std::uint8_t asc[] = {0x11, 0x90};   // AAC-LC, 48 kHz, two channels
    require(std::search(cookie.begin(), cookie.end(), std::begin(asc), std::end(asc)) != cookie.end(), "the AudioSpecificConfig of 48 kHz stereo in the cookie");
    require(f.status(at, "_AudioConverterDispose", {converter}) == 0, "dispose");
    write_asbd(aac_format, 22050, 0x61616320, 0, 0, 0, 0, channels, 0);
    require(f.status(at, "_AudioConverterNew", {int16, aac_format, out}) == static_cast<std::int32_t>(0x666d743f), "no AAC encoder takes 22050 Hz");

    // Files, parsed back.
    const auto parse = [&](const std::filesystem::path& path, mc::Movie& movie) {
        std::ifstream in(path, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
        std::string why;
        const mc::ReadAt read = [&bytes](std::uint64_t at_, std::span<std::uint8_t> o) -> std::size_t {
            if (at_ >= bytes.size()) return 0;
            const auto n = std::min<std::size_t>(o.size(), bytes.size() - static_cast<std::size_t>(at_));
            std::memcpy(o.data(), bytes.data() + at_, n);
            return n;
        };
        require(mc::parse(read, bytes.size(), movie, why) && !movie.tracks.empty(), path.filename().string() + " parses: " + why);
    };
    for (const auto& [type, name] : {std::pair{0x6d346166u, "ext.m4a"}, std::pair{0x63616666u, "ext.caf"}}) {
        const auto path = directory / name;
        write_asbd(aac_format, 48000, 0x61616320, 2, 0, 1024, 0, channels, 0);
        require(f.status(at, "_ExtAudioFileCreateWithURL", {f.cf.file_url(alchemy::guest_path_string(path), false), type, aac_format, 0, 1, out}) == 0,
                std::string(name) + ": ExtAudioFileCreateWithURL");
        const auto ext = f.engine.read64(out);
        require(f.status(at, "_ExtAudioFileSetProperty", {ext, 0x63666d74, 40, int16}) == 0, std::string(name) + ": a 16-bit client");
        const auto piece = f.block(24);
        for (std::uint32_t at_frame = 0; at_frame < frames;) {
            const auto n = std::min<std::uint32_t>(frames - at_frame, 3333);
            f.engine.write32(piece, 1); f.engine.write32(piece + 8, channels); f.engine.write32(piece + 12, n * 4); f.engine.write64(piece + 16, pcm + 4ULL * at_frame);
            require(f.status(at, "_ExtAudioFileWrite", {ext, n, piece}) == 0, std::string(name) + ": ExtAudioFileWrite");
            at_frame += n;
        }
        require(f.status(at, "_ExtAudioFileDispose", {ext}) == 0, std::string(name) + ": dispose");
        mc::Movie movie;
        parse(path, movie);
        const auto& t = movie.tracks[0];
        require(t.samples.size() == packets_expected && t.priming == fake_aac::priming && !t.config.empty() &&
                    (type == 0x63616666u ? t.codec == mc::fourcc("aac ") && t.duration == frames : t.codec == mc::fourcc("mp4a")),
                std::string(name) + ": the packets, the delay as priming, the ES descriptor: " + std::to_string(t.samples.size()) + " " + std::to_string(t.priming));
    }

    // AVAudioFile: AAC settings at 192 kb/s into an M4A; a rate no encoder takes is refused.
    const auto error = f.block(8);
    const auto settings = [&](double rate) {
        return f.cf.dictionary(std::vector<std::pair<Address, Address>>{
            {f.cf.string("AVFormatIDKey"), f.cf.integer(0x61616320, 3)}, {f.cf.string("AVSampleRateKey"), f.cf.real(rate)},
            {f.cf.string("AVNumberOfChannelsKey"), f.cf.integer(channels, 3)}, {f.cf.string("AVEncoderBitRateKey"), f.cf.integer(192000, 3)}});
    };
    const auto av_path = directory / "avaudiofile.m4a";
    const auto av_url = f.cf.file_url(alchemy::guest_path_string(av_path), false);
    require(f.send(f.send(f.cls("AVAudioFile"), "alloc"), "initForWriting:settings:error:", {av_url, settings(22050), error}) == 0, "22050 Hz AAC is refused");
    const auto file = f.send(f.send(f.cls("AVAudioFile"), "alloc"), "initForWriting:settings:error:", {av_url, settings(48000), error});
    require(file != 0 && f.engine.read32(f.send(f.send(file, "fileFormat"), "streamDescription") + 8) == 0x61616320, "AVAudioFile writes AAC");
    const auto pcm_buffer = f.send(f.send(f.cls("AVAudioPCMBuffer"), "alloc"), "initWithPCMFormat:frameCapacity:", {f.send(file, "processingFormat"), frames});
    f.send(pcm_buffer, "setFrameLength:", {frames});
    require((f.send(file, "writeFromBuffer:error:", {pcm_buffer, error}) & 0xff) == 1, "writeFromBuffer:error:");
    f.send(file, "release");
    mc::Movie movie;
    parse(av_path, movie);
    const auto& config = movie.tracks[0].config;
    const std::uint8_t bits[] = {0x00, 0x02, 0xee, 0x00};   // 192000, big endian
    require(movie.tracks[0].samples.size() == packets_expected && std::search(config.begin(), config.end(), std::begin(bits), std::end(bits)) != config.end(),
            "the M4A's ES descriptor carries 192 kb/s");
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// AAC both ways through the host's own encoder and decoder (skipped where either is missing): M4A and CAF files written
// by ExtAudioFile read back through ExtAudioFile, AVAudioFile and AVAssetReader with every frame and in place (a 440 Hz
// tone: any offset ruins the match), and their packets through AudioConverter with the codec's delay leading.
void aac_decoding(Fixture& f) {
    namespace aac = alchemy::gui::completion_kit::media::aac;
    namespace codecs = alchemy::gui::completion_kit::media::codecs;
    aac::set_backend({});   // the host's encoder
    struct Restore { ~Restore() { fake_aac::install(); } } restore;
    if (!aac::available() || !codecs::audio_decoder_available(alchemy::gui::completion_kit::media::container::fourcc("mp4a"))) {
        std::cout << "[qlab media] no AAC encoder or decoder on this host: aac_decoding skipped" << std::endl;
        return;
    }
    const auto directory = std::filesystem::temp_directory_path() / "alchemy_qlab_media_aac_decoding";
    std::filesystem::create_directories(directory);
    constexpr std::uint32_t frames = 48000, channels = 2;
    std::vector<float> source(std::size_t(frames) * channels);
    for (std::uint32_t i = 0; i < frames; ++i)
        for (std::uint32_t c = 0; c < channels; ++c) source[std::size_t(i) * channels + c] = 0.4f * std::sin(2 * 3.14159265f * 440.0f * float(i) / 48000.0f + float(c));
    std::vector<std::uint8_t> source_bytes(source.size() * 4);
    std::memcpy(source_bytes.data(), source.data(), source_bytes.size());
    const auto pcm = f.block(source_bytes.size());
    f.memory().write_bytes(pcm, source_bytes);
    const auto float32 = f.block(40), aac_format = f.block(40), out = f.block(8), size = f.block(8);
    const auto write_asbd = [&](Address at_, std::uint32_t id, std::uint32_t flags, std::uint32_t bytes_per_packet, std::uint32_t per_packet,
                                std::uint32_t bytes_per_frame, std::uint32_t bits) {
        f.engine.write64(at_, std::bit_cast<std::uint64_t>(48000.0));
        f.engine.write32(at_ + 8, id); f.engine.write32(at_ + 12, flags); f.engine.write32(at_ + 16, bytes_per_packet); f.engine.write32(at_ + 20, per_packet);
        f.engine.write32(at_ + 24, bytes_per_frame); f.engine.write32(at_ + 28, channels); f.engine.write32(at_ + 32, bits); f.engine.write32(at_ + 36, 0);
    };
    write_asbd(float32, 0x6c70636d, 1 | 8, 8, 1, 8, 32);
    write_asbd(aac_format, 0x61616320, 2, 0, 1024, 0, 0);
    // The SNR of `got` (interleaved floats from frame 0) against the source over frames [2048, 46000).
    const auto snr = [&](const std::vector<float>& got) {
        double signal = 0, noise = 0;
        for (std::size_t i = 2048 * channels; i < 46000 * channels && i < got.size(); ++i) {
            signal += double(source[i]) * source[i];
            noise += (double(got[i]) - source[i]) * (double(got[i]) - source[i]);
        }
        return 10 * std::log10(signal / std::max(noise, 1e-12));
    };
    const auto read_floats = [&](Address samples, std::size_t count) {
        std::vector<float> v(count);
        std::vector<std::uint8_t> bytes(count * 4);
        f.memory().read_bytes(samples, bytes);
        std::memcpy(v.data(), bytes.data(), bytes.size());
        return v;
    };
    for (const auto& [type, name] : {std::pair{0x6d346166u, "tone.m4a"}, std::pair{0x63616666u, "tone.caf"}}) {
        const auto path = directory / name;
        const auto url = f.cf.file_url(alchemy::guest_path_string(path), false);
        require(f.status(at, "_ExtAudioFileCreateWithURL", {url, type, aac_format, 0, 1, out}) == 0, std::string(name) + ": created");
        auto ext = f.engine.read64(out);
        require(f.status(at, "_ExtAudioFileSetProperty", {ext, 0x63666d74, 40, float32}) == 0, std::string(name) + ": a float client");
        const auto list = f.block(24);
        f.engine.write32(list, 1); f.engine.write32(list + 8, channels); f.engine.write32(list + 12, frames * 8); f.engine.write64(list + 16, pcm);
        require(f.status(at, "_ExtAudioFileWrite", {ext, frames, list}) == 0 && f.status(at, "_ExtAudioFileDispose", {ext}) == 0, std::string(name) + ": written");
        // ExtAudioFile: every frame, in place.
        require(f.status(at, "_ExtAudioFileOpenURL", {url, out}) == 0, std::string(name) + ": opens for reading");
        ext = f.engine.read64(out);
        require(f.status(at, "_ExtAudioFileSetProperty", {ext, 0x63666d74, 40, float32}) == 0, std::string(name) + ": a float client for reading");
        f.engine.write32(size, 8);
        const auto length_status = f.status(at, "_ExtAudioFileGetProperty", {ext, 0x2366726d, size, out});
        require(length_status == 0 && f.engine.read64(out) == frames,
                std::string(name) + ": FileLengthFrames (status " + std::to_string(length_status) + ") " + std::to_string(f.engine.read64(out)));
        const auto back = f.block(std::size_t(frames + 100) * 8), back_list = f.block(24), got = f.block(4);
        f.engine.write32(back_list, 1); f.engine.write32(back_list + 8, channels); f.engine.write32(back_list + 12, (frames + 100) * 8); f.engine.write64(back_list + 16, back);
        f.engine.write32(got, frames + 100);
        require(f.status(at, "_ExtAudioFileRead", {ext, got, back_list}) == 0 && f.engine.read32(got) == frames, std::string(name) + ": every frame read");
        const auto quality = snr(read_floats(back, std::size_t(frames) * channels));
        require(quality > 20, std::string(name) + ": in place, SNR " + std::to_string(quality));
        // A seek lands on the same frames.
        require(f.status(at, "_ExtAudioFileSeek", {ext, 30000}) == 0, std::string(name) + ": seek");
        f.engine.write32(got, 1000);
        f.engine.write32(back_list + 12, 1000 * 8);
        require(f.status(at, "_ExtAudioFileRead", {ext, got, back_list}) == 0 && f.engine.read32(got) == 1000, std::string(name) + ": a read after a seek");
        const auto after = read_floats(back, 1000 * channels);
        double e = 0, s = 0;
        for (std::size_t i = 0; i < after.size(); ++i) {
            e += (double(after[i]) - source[30000 * channels + i]) * (double(after[i]) - source[30000 * channels + i]);
            s += double(source[30000 * channels + i]) * source[30000 * channels + i];
        }
        require(10 * std::log10(s / std::max(e, 1e-12)) > 20, std::string(name) + ": the frames after the seek are in place");
        require(f.status(at, "_ExtAudioFileDispose", {ext}) == 0, std::string(name) + ": dispose");
        // AVAudioFile and AVAssetReader: the same length.
        const auto error = f.block(8);
        const auto av_file = f.send(f.send(f.cls("AVAudioFile"), "alloc"), "initForReading:error:", {url, error});
        require(av_file && static_cast<std::uint32_t>(f.send(av_file, "length")) == frames, std::string(name) + ": AVAudioFile length");
        f.send(av_file, "release");
        const auto asset = f.send(f.cls("AVURLAsset"), "URLAssetWithURL:options:", {url, 0});
        const auto tracks = f.cf.array_values(f.send(asset, "tracksWithMediaType:", {f.cf.string("soun")}));
        require(tracks.size() == 1 && (f.send(asset, "isReadable") & 0xff) == 1, std::string(name) + ": a readable AAC track");
        const auto reader = f.send(f.cls("AVAssetReader"), "assetReaderWithAsset:error:", {asset, error});
        const auto output = f.send(f.cls("AVAssetReaderAudioMixOutput"), "assetReaderAudioMixOutputWithAudioTracks:audioSettings:", {f.cf.array(std::vector<Address>{tracks[0]}), 0});
        static_cast<void>(f.send(reader, "addOutput:", {output}));
        require((f.send(reader, "startReading") & 0xff) == 1, std::string(name) + ": AVAssetReader starts");
        std::uint64_t read_frames = 0;
        while (const auto sample = f.send(output, "copyNextSampleBuffer")) {
            read_frames += f.call(cm, "_CMSampleBufferGetNumSamples", {sample});
            static_cast<void>(f.call("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "_CFRelease", {sample}));
        }
        require(read_frames == frames, std::string(name) + ": AVAssetReader reads every frame: " + std::to_string(read_frames));
    }

    // AudioConverter: the M4A's packets with its cookie; the codec's delay leads the output.
    const auto url = f.cf.file_url(alchemy::guest_path_string(directory / "tone.m4a"), false);
    require(f.status(at, "_AudioFileOpenURL", {url, 1, 0, out}) == 0, "AudioFileOpenURL on the AAC M4A");
    const auto file = f.engine.read64(out);
    const auto cookie = f.block(256);
    f.engine.write32(size, 256);
    require(f.status(at, "_AudioFileGetProperty", {file, 0x6d676963, size, cookie}) == 0, "its magic cookie");
    const auto cookie_size = f.engine.read32(size);
    f.engine.write32(size, 16);
    require(f.status(at, "_AudioFileGetProperty", {file, 0x706e666f, size, out}) == 0, "its packet table info");
    const auto priming = f.engine.read32(out + 8);
    f.engine.write32(size, 8);
    require(f.status(at, "_AudioFileGetProperty", {file, 0x70636e74, size, out}) == 0, "its packet count");
    const auto packet_count = static_cast<std::uint32_t>(f.engine.read64(out));
    const auto room = f.block(4), count = f.block(4), descriptions = f.block(16ULL * packet_count), packets = f.block(768ULL * channels * packet_count);
    f.engine.write32(room, 768 * channels * packet_count);
    f.engine.write32(count, packet_count);
    require(f.status(at, "_AudioFileReadPacketData", {file, 0, room, descriptions, 0, count, packets}) == 0 && f.engine.read32(count) == packet_count, "the packets");
    require(f.status(at, "_AudioConverterNew", {aac_format, float32, out}) == 0, "AudioConverterNew from AAC");
    const auto converter = f.engine.read64(out);
    require(f.status(at, "_AudioConverterSetProperty", {converter, 0x646d6763, cookie_size, cookie}) == 0, "the decompression cookie");
    static Address aac_packets{}, aac_descriptions{}, aac_one{};
    static std::uint32_t aac_next{}, aac_total{};
    aac_packets = packets;
    aac_descriptions = descriptions;
    aac_one = f.block(16);
    aac_next = 0;
    aac_total = packet_count;
    const auto input = f.process.install_function("_alchemy_test_aac_packets", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        if (aac_next >= aac_total) { m.write32(s.x[1], 0); s.x[0] = 0; return; }
        const auto entry = aac_descriptions + 16ULL * aac_next++;
        const auto bytes = m.read32(entry + 12);
        m.write32(s.x[1], 1);
        m.write32(s.x[2] + 8 + 4, bytes);
        m.write64(s.x[2] + 8 + 8, aac_packets + m.read64(entry));
        m.write64(aac_one, 0); m.write32(aac_one + 8, 0); m.write32(aac_one + 12, bytes);
        if (s.x[3]) m.write64(s.x[3], aac_one);
        s.x[0] = 0;
    }, "/usr/lib/alchemy_test_io.dylib");
    const std::uint32_t total = packet_count * 1024;
    const auto decoded = f.block(std::size_t(total) * 8), list = f.block(24), got = f.block(4);
    f.engine.write32(list, 1); f.engine.write32(list + 8, channels); f.engine.write32(list + 12, total * 8); f.engine.write64(list + 16, decoded);
    f.engine.write32(got, total);
    require(f.status(at, "_AudioConverterFillComplexBuffer", {converter, input, 0, got, list, 0}) == 0 && f.engine.read32(got) == total,
            "a block of 1024 frames a packet: " + std::to_string(f.engine.read32(got)));
    const auto all = read_floats(decoded, std::size_t(total) * channels);
    const std::vector<float> trimmed(all.begin() + std::ptrdiff_t(priming) * channels, all.end());
    const auto quality = snr(trimmed);
    require(quality > 20, "AudioConverter: the source after the priming (" + std::to_string(priming) + "), SNR " + std::to_string(quality));
    require(f.status(at, "_AudioConverterDispose", {converter}) == 0 && f.status(at, "_AudioFileClose", {file}) == 0, "dispose and close");
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// AudioFileStream: minimp3's MP3 vector fed in pieces gives the packets AudioFile reads from the file; a seek
// to a parsed packet is exact; a WAVE stream gives its frames without descriptions.
void audio_file_stream(Fixture& f) {
    const std::string vectors = std::string(ALCHEMY_THIRD_PARTY) + "/minimp3/vectors/";
    std::ifstream in(vectors + "l3-hecommon.bit", std::ios::binary);
    const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), {});
    require(file.size() > 10000, "the MP3 vector");
    static std::vector<std::uint32_t> properties, sizes;
    static std::vector<std::uint8_t> packets;
    static std::uint32_t packet_count{}, flags_seen{}, undescribed{};
    static Address client_seen{};
    const auto reset = [] { properties.clear(); sizes.clear(); packets.clear(); packet_count = flags_seen = undescribed = 0; client_seen = 0; };
    const auto listener = f.process.install_function("_alchemy_test_afs_listener", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        client_seen = s.x[0];
        properties.push_back(static_cast<std::uint32_t>(s.x[2]));
        flags_seen |= m.read32(s.x[3]);
    }, "/usr/lib/alchemy_test_io.dylib");
    const auto packets_proc = f.process.install_function("_alchemy_test_afs_packets", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        const auto bytes = static_cast<std::uint32_t>(s.x[1]), n = static_cast<std::uint32_t>(s.x[2]);
        std::vector<std::uint8_t> data(bytes);
        m.read_bytes(s.x[3], data);
        if (!s.x[4]) {
            ++undescribed;
            packets.insert(packets.end(), data.begin(), data.end());
        }
        for (std::uint32_t i = 0; s.x[4] && i < n; ++i) {
            const auto start = m.read64(s.x[4] + 16ULL * i);
            const auto size = m.read32(s.x[4] + 16ULL * i + 12);
            packets.insert(packets.end(), data.begin() + static_cast<std::ptrdiff_t>(start), data.begin() + static_cast<std::ptrdiff_t>(start + size));
            sizes.push_back(size);
        }
        packet_count += n;
    }, "/usr/lib/alchemy_test_io.dylib");
    const auto out = f.block(8), size = f.block(4), value = f.block(64), chunk = f.block(1000);
    const auto parse = [&](Address stream, const std::vector<std::uint8_t>& bytes, std::size_t from, std::uint32_t flags) {
        for (std::size_t offset = from; offset < bytes.size(); offset += 1000) {
            const auto n = std::min<std::size_t>(1000, bytes.size() - offset);
            f.memory().write_bytes(chunk, std::span(bytes.data() + offset, n));
            require(f.status(at, "_AudioFileStreamParseBytes", {stream, n, chunk, offset == from ? flags : 0}) == 0, "AudioFileStreamParseBytes");
        }
    };
    reset();
    require(f.status(at, "_AudioFileStreamOpen", {0x1234, listener, 0, 0, out}) == -50, "a packets procedure is required");
    require(f.status(at, "_AudioFileStreamOpen", {0x1234, listener, packets_proc, 0x4d504733, out}) == 0, "AudioFileStreamOpen");
    const auto stream = f.engine.read64(out);
    f.engine.write32(size, 4);
    require(f.status(at, "_AudioFileStreamGetProperty", {stream, 0x72656479, size, value}) == 0 && f.engine.read32(value) == 0, "not ready yet");
    parse(stream, file, 0, 0);
    require(!properties.empty() && properties.back() == 0x72656479 && flags_seen == 1 && client_seen == 0x1234,
            "the listener hears the properties, then ReadyToProducePackets");
    f.engine.write32(size, 40);
    require(f.status(at, "_AudioFileStreamGetProperty", {stream, 0x64666d74, size, value}) == 0 && f.engine.read32(value + 8) == 0x2e6d7033 &&
            f.engine.read32(value + 20) == 1152 && f.engine.read32(size) == 40, "data format '.mp3'");
    require(f.status(at, "_AudioFileStreamGetPropertyInfo", {stream, 0x64666d74, size, value}) == 0 && f.engine.read32(size) == 40 &&
            f.memory().read8(value) == 0, "GetPropertyInfo: 40 bytes, not writable");
    f.engine.write32(size, 8);
    require(f.status(at, "_AudioFileStreamGetProperty", {stream, 0x646f6666, size, value}) == 0, "data offset");
    const auto data_offset = f.engine.read64(value);

    // The same packets as AudioFile's (a cut-off last frame aside).
    const auto directory = std::filesystem::temp_directory_path() / "alchemy_qlab_media_stream";
    std::filesystem::create_directories(directory);
    const auto path = directory / "hecommon.mp3";
    std::filesystem::copy_file(vectors + "l3-hecommon.bit", path, std::filesystem::copy_options::overwrite_existing);
    require(f.status(at, "_AudioFileOpenURL", {f.cf.file_url(alchemy::guest_path_string(path), false), 1, 0, out}) == 0, "AudioFileOpenURL");
    const auto audio_file = f.engine.read64(out);
    f.engine.write32(size, 8);
    require(f.status(at, "_AudioFileGetProperty", {audio_file, 0x70636e74, size, value}) == 0, "AudioFile packet count");
    const auto file_packets = static_cast<std::uint32_t>(f.engine.read64(value));
    f.engine.write32(size, 8);
    require(f.status(at, "_AudioFileGetProperty", {audio_file, 0x646f6666, size, value}) == 0 && f.engine.read64(value) == data_offset,
            "the same data offset as AudioFile");
    const auto room = f.block(4), count = f.block(4), descriptions = f.block(16ULL * file_packets), file_bytes = f.block(file.size());
    f.engine.write32(room, static_cast<std::uint32_t>(file.size()));
    f.engine.write32(count, file_packets);
    require(f.status(at, "_AudioFileReadPacketData", {audio_file, 0, room, descriptions, 0, count, file_bytes}) == 0, "ReadPacketData");
    require(packet_count + 1 >= file_packets && packet_count <= file_packets && sizes.size() == packet_count,
            "every whole frame is a packet: " + std::to_string(packet_count) + " of " + std::to_string(file_packets));
    std::vector<std::uint8_t> expected(packets.size());
    f.memory().read_bytes(file_bytes, expected);
    require(expected == packets, "the packets are AudioFile's");
    require(f.status(at, "_AudioFileClose", {audio_file}) == 0, "AudioFileClose");

    // Seeking to a parsed packet is exact; parsing from there gives that packet first.
    std::uint64_t tenth = 0;
    for (int i = 0; i < 10; ++i) tenth += sizes[static_cast<std::size_t>(i)];
    const auto flags = f.block(4);
    f.engine.write32(flags, 0);
    require(f.status(at, "_AudioFileStreamSeek", {stream, 10, out, flags}) == 0 && f.engine.read64(out) == tenth && f.engine.read32(flags) == 0,
            "seek to packet 10");
    const auto first_after = std::vector<std::uint8_t>(packets.begin() + static_cast<std::ptrdiff_t>(tenth),
                                                       packets.begin() + static_cast<std::ptrdiff_t>(tenth + sizes[10]));
    reset();
    parse(stream, file, static_cast<std::size_t>(data_offset + tenth), 1);   // kAudioFileStreamParseFlag_Discontinuity
    require(packets.size() >= first_after.size() && std::equal(first_after.begin(), first_after.end(), packets.begin()), "packet 10 after the seek");

    // A streaming player: those packets into an AudioQueue of the stream's '.mp3' format, decoded and played.
    static std::atomic<int> queue_returned{0};
    queue_returned = 0;
    const auto queue_callback = f.process.install_function("_alchemy_test_afs_queue_callback",
                                                           [](transmute::CpuState&, transmute::GuestMemory&) { ++queue_returned; },
                                                           "/usr/lib/alchemy_test_io.dylib");
    const auto stream_format = f.block(40), queue_out = f.block(8), buffer_out = f.block(8);
    f.engine.write32(size, 40);
    require(f.status(at, "_AudioFileStreamGetProperty", {stream, 0x64666d74, size, stream_format}) == 0, "the stream's format");
    require(f.status(at, "_AudioQueueNewOutput", {stream_format, queue_callback, 0, 0, 0, 0, queue_out}) == 0, "AudioQueueNewOutput for '.mp3'");
    const auto queue = f.engine.read64(queue_out);
    const auto queued_packets = static_cast<std::uint32_t>(std::min<std::size_t>(40, sizes.size()));
    std::uint32_t total = 0;
    for (std::uint32_t i = 0; i < queued_packets; ++i) total += sizes[i];
    require(f.status(at, "_AudioQueueAllocateBufferWithPacketDescriptions", {queue, total, queued_packets, buffer_out}) == 0, "a buffer with descriptions");
    const auto queue_buffer = f.engine.read64(buffer_out);
    f.memory().write_bytes(f.engine.read64(queue_buffer + 8), std::span(packets.data(), total));
    f.engine.write32(queue_buffer + 16, total);
    const auto queue_descriptions = f.engine.read64(queue_buffer + 40);
    for (std::uint32_t i = 0, offset = 0; i < queued_packets; offset += sizes[i++]) {
        f.engine.write64(queue_descriptions + 16ULL * i, offset);
        f.engine.write32(queue_descriptions + 16ULL * i + 8, 0);
        f.engine.write32(queue_descriptions + 16ULL * i + 12, sizes[i]);
    }
    f.engine.write32(queue_buffer + 48, queued_packets);
    require(f.status(at, "_AudioQueueEnqueueBuffer", {queue, queue_buffer, 0, 0}) == 0, "the MP3 packets enqueued");
    require(f.status(at, "_AudioQueueStart", {queue, 0}) == 0, "AudioQueueStart");
    const auto heard = FakeStream::play_until_sound(512);
    require(std::any_of(heard.begin(), heard.end(), [](float v) { return std::fabs(v) > 1e-4f; }), "the decoded MP3 plays");
    const auto queue_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (queue_returned == 0 && std::chrono::steady_clock::now() < queue_deadline) {
        static_cast<void>(FakeStream::play_paced(2048));
    }
    require(queue_returned == 1, "the played buffer comes back");
    require(f.status(at, "_AudioQueueStop", {queue, 1}) == 0 && f.status(at, "_AudioQueueDispose", {queue, 1}) == 0, "queue stopped and disposed");
    f.engine.write32(stream_format + 8, 0x61616320);   // 'aac '
    // AAC plays through the host's decoder where there is one; without one the queue refuses it with 'fmt?'.
    const auto aac_queue = f.status(at, "_AudioQueueNewOutput", {stream_format, queue_callback, 0, 0, 0, 0, queue_out});
    if (alchemy::gui::completion_kit::media::codecs::audio_decoder_available(alchemy::gui::completion_kit::media::container::fourcc("aac "))) {
        require(aac_queue == 0, "an AAC queue");
        require(f.status(at, "_AudioQueueDispose", {f.engine.read64(queue_out), 1}) == 0, "the AAC queue disposed");
    } else {
        require(aac_queue == static_cast<std::int32_t>(0x666d743f), "AAC has no decoder ('fmt?')");
    }
    require(f.status(at, "_AudioFileStreamSetProperty", {stream, 0x64666d74, 40, value}) == static_cast<std::int32_t>(0x6e6f7065),
            "properties cannot be set ('nope')");
    require(f.status(at, "_AudioFileStreamClose", {stream}) == 0, "AudioFileStreamClose");

    // A WAVE stream: frames, no descriptions.
    const auto wave_path = write_wave(directory / "ramp.wav", 22050, 2, 3000, [](std::uint32_t i, std::uint16_t c) { return static_cast<std::int16_t>(i * 3 + c); });
    std::ifstream wave_in(wave_path, std::ios::binary);
    const std::vector<std::uint8_t> wave((std::istreambuf_iterator<char>(wave_in)), {});
    reset();
    require(f.status(at, "_AudioFileStreamOpen", {0, listener, packets_proc, 0, out}) == 0, "AudioFileStreamOpen for WAVE");
    const auto wave_stream = f.engine.read64(out);
    parse(wave_stream, wave, 0, 0);
    require(packet_count == 3000 && undescribed > 0 && sizes.empty() && packets == std::vector<std::uint8_t>(wave.begin() + 44, wave.end()),
            "WAVE frames: " + std::to_string(packet_count));
    f.engine.write32(size, 4);
    require(f.status(at, "_AudioFileStreamGetProperty", {wave_stream, 0x66666d74, size, value}) == 0 && f.engine.read32(value) == 0x57415645, "'WAVE'");
    require(f.status(at, "_AudioFileStreamClose", {wave_stream}) == 0, "close the WAVE stream");
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// VTDecompressionSession: libvpx's VP9 test vector (through our WebM parser) and libaom's AV1 test
// vectors decoded into CVPixelBuffers, compared with the projects' per-frame MD5s; H.264 has no decoder.
struct DecodedImage { Address image; std::int64_t pts; std::uint32_t flags; std::int32_t status; };
Fixture* vt_fixture{};
constexpr std::string_view vt_cf = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr std::string_view vt_cg = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";
std::vector<DecodedImage>& vt_images() { static std::vector<DecodedImage> value; return value; }

std::string md5_hex(const std::vector<std::uint8_t>& data) {
    const auto digest = alchemy::host::crypto::hash(alchemy::host::crypto::Digest::md5, std::as_bytes(std::span(data)));
    require(static_cast<bool>(digest), "MD5");
    std::string out;
    for (const auto b : *digest) { const char* d = "0123456789abcdef"; out += d[b >> 4]; out += d[b & 15]; }
    return out;
}

// The picture's planes as the projects' tools hash them (I420: luma, then Cb, then Cr rows; 16-bit
// little-endian samples, low-aligned, above 8 bits) from a 420v/420f or x420 buffer.
std::vector<std::uint8_t> i420_of(Fixture& f, Address image) {
    require(f.call(cv, "_CVPixelBufferLockBaseAddress", {image, 1}) == 0, "lock");
    const auto format = static_cast<std::uint32_t>(f.call(cv, "_CVPixelBufferGetPixelFormatType", {image}));
    const bool ten = format == 0x78343230;   // 'x420'
    const auto width = f.call(cv, "_CVPixelBufferGetWidth", {image}), height = f.call(cv, "_CVPixelBufferGetHeight", {image});
    std::vector<std::uint8_t> out;
    const auto bytes = ten ? 2 : 1;
    const auto luma = f.call(cv, "_CVPixelBufferGetBaseAddressOfPlane", {image, 0}), luma_row = f.call(cv, "_CVPixelBufferGetBytesPerRowOfPlane", {image, 0});
    const auto put = [&](std::uint32_t value) {
        if (ten) { value >>= 6; out.push_back(static_cast<std::uint8_t>(value)); out.push_back(static_cast<std::uint8_t>(value >> 8)); }
        else out.push_back(static_cast<std::uint8_t>(value));
    };
    const auto read = [&](Address at) { return ten ? std::uint32_t(f.memory().read8(at) | (f.memory().read8(at + 1) << 8)) : std::uint32_t(f.memory().read8(at)); };
    for (Address y = 0; y < height; ++y)
        for (Address x = 0; x < width; ++x) put(read(luma + y * luma_row + x * bytes));
    const auto chroma = f.call(cv, "_CVPixelBufferGetBaseAddressOfPlane", {image, 1}), chroma_row = f.call(cv, "_CVPixelBufferGetBytesPerRowOfPlane", {image, 1});
    const auto cw = (width + 1) / 2, ch = (height + 1) / 2;
    for (int component = 0; component < 2; ++component)
        for (Address y = 0; y < ch; ++y)
            for (Address x = 0; x < cw; ++x) put(read(chroma + y * chroma_row + (2 * x + component) * bytes));
    f.call(cv, "_CVPixelBufferUnlockBaseAddress", {image, 1});
    return out;
}

void video_toolbox(Fixture& f) {
    namespace mc = alchemy::gui::completion_kit::media::container;
    vt_fixture = &f;
    const auto callback = f.process.install_function("_alchemy_test_vt_output", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        // (refcon, sourceFrameRefCon, status, infoFlags, imageBuffer, CMTime *pts, CMTime *duration)
        if (s.x[4]) static_cast<void>(vt_fixture->call(vt_cf, "_CFRetain", {s.x[4]}));
        vt_images().push_back({s.x[4], static_cast<std::int64_t>(m.read64(s.x[5])), static_cast<std::uint32_t>(s.x[3]),
                               static_cast<std::int32_t>(s.x[2])});
    }, "/usr/lib/alchemy_test_io.dylib");
    const auto record = f.block(16);
    f.engine.write64(record, callback);
    f.engine.write64(record + 8, 0x1234);
    // Decodes `packets` (bytes, pts in 1/1000 s) of `codec` at width x height; the images that came out.
    const auto decode = [&](std::uint32_t codec, std::int32_t width, std::int32_t height, const std::vector<std::pair<std::vector<std::uint8_t>, std::int64_t>>& packets,
                            Address attributes) {
        const auto format_out = f.block(8), session_out = f.block(8);
        require(f.status(cm, "_CMVideoFormatDescriptionCreate", {0, codec, static_cast<Address>(width), static_cast<Address>(height), 0, format_out}) == 0,
                "CMVideoFormatDescriptionCreate");
        const auto format = f.engine.read64(format_out);
        require(f.status(vt, "_VTDecompressionSessionCreate", {0, format, 0, attributes, record, session_out}) == 0, "VTDecompressionSessionCreate");
        const auto session = f.engine.read64(session_out);
        require(f.call(vt, "_VTDecompressionSessionCanAcceptFormatDescription", {session, format}) == 1, "the session accepts its format");
        vt_images().clear();
        for (const auto& [bytes, pts] : packets) {
            const auto block_out = f.block(8), sample_out = f.block(8), timing = f.block(72);
            alchemy::CallRegs block{};
            block.x[2] = bytes.size(); block.x[6] = bytes.size(); block.x[7] = 1;
            block.push_stack(block_out);
            require(f.regs(cm, "_CMBlockBufferCreateWithMemoryBlock", block).x0 == 0, "block buffer");
            const auto data = f.block(bytes.size());
            f.memory().write_bytes(data, bytes);
            require(f.status(cm, "_CMBlockBufferReplaceDataBytes", {data, f.engine.read64(block_out), 0, bytes.size()}) == 0, "fill the block");
            write_time(f, timing, 33, 1000);
            write_time(f, timing + 24, pts, 1000);
            write_time(f, timing + 48, 0, 0, 0);
            alchemy::CallRegs create{};
            create.x[1] = f.engine.read64(block_out); create.x[2] = format; create.x[3] = 1; create.x[4] = 1; create.x[5] = timing;
            create.push_stack(sample_out);
            require(f.regs(cm, "_CMSampleBufferCreateReady", create).x0 == 0, "sample buffer");
            const auto info = f.block(4);
            require(f.status(vt, "_VTDecompressionSessionDecodeFrame", {session, f.engine.read64(sample_out), 0, 0, info}) == 0, "DecodeFrame");
            f.call(vt_cf, "_CFRelease", {f.engine.read64(sample_out)});
            f.call(vt_cf, "_CFRelease", {f.engine.read64(block_out)});
        }
        require(f.status(vt, "_VTDecompressionSessionFinishDelayedFrames", {session}) == 0 &&
                f.status(vt, "_VTDecompressionSessionWaitForAsynchronousFrames", {session}) == 0, "finish");
        f.call(vt, "_VTDecompressionSessionInvalidate", {session});
        require(f.status(vt, "_VTDecompressionSessionDecodeFrame", {session, 0, 0, 0, 0}) == -12903, "an invalidated session");
        f.call(vt_cf, "_CFRelease", {session});
        f.call(vt_cf, "_CFRelease", {format});
        std::vector<DecodedImage> images;
        for (const auto& image : vt_images()) if (image.image) images.push_back(image);
        return images;
    };
    const std::string third_party = ALCHEMY_THIRD_PARTY;
    const auto load = [](const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        require(in.good(), "cannot open " + path);
        return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), {});
    };

    // VP9 in WebM: one image per frame, each matching the vector's per-frame MD5.
    {
        const auto file = load(third_party + "/libvpx/test-vectors/vp90-2-02-size-08x08.webm");
        mc::Movie movie;
        std::string why;
        const mc::ReadAt read = [&file](std::uint64_t offset, std::span<std::uint8_t> out) -> std::size_t {
            if (offset >= file.size()) return 0;
            const auto n = std::min<std::size_t>(out.size(), file.size() - offset);
            std::memcpy(out.data(), file.data() + offset, n);
            return n;
        };
        require(mc::parse(read, file.size(), movie, why), "the WebM parses: " + why);
        const auto& track = movie.tracks.at(0);
        std::vector<std::pair<std::vector<std::uint8_t>, std::int64_t>> packets;
        for (const auto& sample : track.samples) packets.emplace_back(mc::read_sample(read, track, sample), sample.pts);
        const auto images = decode(0x76703039 /* 'vp09' */, 8, 8, packets, 0);
        std::istringstream lines(std::string(reinterpret_cast<const char*>(load(third_party + "/libvpx/test-vectors/vp90-2-02-size-08x08.webm.md5").data()),
                                             load(third_party + "/libvpx/test-vectors/vp90-2-02-size-08x08.webm.md5").size()));
        std::vector<std::string> expected;
        for (std::string line; std::getline(lines, line);) if (line.size() >= 32) expected.push_back(line.substr(0, 32));
        require(images.size() == expected.size(), "VP9: one image per frame: " + std::to_string(images.size()));
        for (std::size_t i = 0; i < images.size(); ++i) {
            require(f.call(cv, "_CVPixelBufferGetPixelFormatType", {images[i].image}) == 0x34323076, "VP9 pictures are 420v");   // '420v'
            require(md5_hex(i420_of(f, images[i].image)) == expected[i], "VP9 frame " + std::to_string(i + 1) + " MD5");
            require(images[i].pts == packets[i].second, "VP9 presentation times come back");
            f.call(vt_cf, "_CFRelease", {images[i].image});
        }
    }
    // AV1, 8 and 10 bits: libaom's test vectors against their per-frame MD5s.
    const auto ivf_packets = [](const std::vector<std::uint8_t>& file) {
        std::vector<std::pair<std::vector<std::uint8_t>, std::int64_t>> packets;
        for (std::size_t at = 32; at + 12 <= file.size();) {
            const std::uint32_t size = file[at] | (file[at + 1] << 8) | (file[at + 2] << 16) | (std::uint32_t(file[at + 3]) << 24);
            packets.emplace_back(std::vector<std::uint8_t>(file.begin() + std::ptrdiff_t(at + 12), file.begin() + std::ptrdiff_t(at + 12 + size)),
                                 static_cast<std::int64_t>(packets.size()));
            at += 12 + size;
        }
        return packets;
    };
    for (const auto& [name, ten] : {std::pair{"av1-1-b8-01-size-16x16.ivf", false}, std::pair{"av1-1-b10-00-quantizer-00.ivf", true}}) {
        const auto file = load(third_party + "/libaom/test-vectors/" + name);
        const auto width = static_cast<std::int32_t>(file[12] | (file[13] << 8)), height = static_cast<std::int32_t>(file[14] | (file[15] << 8));
        const auto images = decode(0x61763031 /* 'av01' */, width, height, ivf_packets(file), 0);
        const auto listing = load(third_party + "/libaom/test-vectors/" + name + ".md5");
        std::istringstream lines(std::string(reinterpret_cast<const char*>(listing.data()), listing.size()));
        std::vector<std::string> expected;
        for (std::string line; std::getline(lines, line);) if (line.size() >= 32) expected.push_back(line.substr(0, 32));
        require(!images.empty() && images.size() == expected.size(), std::string(name) + ": one image per frame: " + std::to_string(images.size()));
        for (std::size_t i = 0; i < images.size(); ++i) {
            require(f.call(cv, "_CVPixelBufferGetPixelFormatType", {images[i].image}) == (ten ? 0x78343230U : 0x34323076U), std::string(name) + " pixel format");
            require(md5_hex(i420_of(f, images[i].image)) == expected[i], std::string(name) + ": frame " + std::to_string(i + 1) + " MD5");
            f.call(vt_cf, "_CFRelease", {images[i].image});
        }
    }
    // A BGRA destination: the pictures come out in it.
    {
        const auto file = load(third_party + "/libaom/test-vectors/av1-1-b8-01-size-16x16.ivf");
        const auto packets = ivf_packets(file);
        const auto key = f.cf.string("PixelFormatType"), value = f.cf.integer(0x42475241);   // 'BGRA'
        const std::array<std::pair<Address, Address>, 1> entries{{{key, value}}};
        const auto attributes = f.cf.dictionary(entries);
        const auto images = decode(0x61763031, 16, 16, {packets.front()}, attributes);
        require(images.size() == 1 && f.call(cv, "_CVPixelBufferGetPixelFormatType", {images[0].image}) == 0x42475241 &&
                f.call(cv, "_CVPixelBufferGetWidth", {images[0].image}) == 16, "a 16x16 BGRA picture");
        f.call(vt_cf, "_CFRelease", {images[0].image});
    }
    // H.264 has no decoder (until the patent question is decided).
    {
        const auto format_out = f.block(8), session_out = f.block(8);
        require(f.status(cm, "_CMVideoFormatDescriptionCreate", {0, 0x61766331 /* 'avc1' */, 64, 64, 0, format_out}) == 0, "an H.264 description");
        require(f.status(vt, "_VTDecompressionSessionCreate", {0, f.engine.read64(format_out), 0, 0, record, session_out}) == -12906 &&
                f.engine.read64(session_out) == 0, "kVTCouldNotFindVideoDecoderErr for H.264");
    }
}

// VTCompressionSession over the fake encoders (fake_video.hpp): the encoder list and specification, the properties
// as the encoder's settings, frames in any buffer format (the fake gives back each picture's mean Y'CbCr), the
// output callback and handler with each frame's refcon and times, keyframes forced and NotSync marks, reordered
// frames with decode times, the format description's parameter sets and avcC/hvcC atom, CompleteFrames, the pool,
// Invalidate and the refusals.
struct Encoded {
    Address refcon{}, sample{}, output_refcon{};
    std::int32_t status{};
    std::uint32_t flags{};
};
std::vector<Encoded>& vt_encoded() { static std::vector<Encoded> value; return value; }

void vt_compression(Fixture& f) {
    namespace vc = alchemy::gui::completion_kit::media::video_config;
    fake_video::install();
    vt_fixture = &f;
    const auto cfl = std::string(vt_cf);
    const auto callback = f.process.install_function("_alchemy_test_vt_encoded", [](transmute::CpuState& s, transmute::GuestMemory&) {
        // (outputCallbackRefCon, sourceFrameRefCon, status, infoFlags, sampleBuffer)
        if (s.x[4]) static_cast<void>(vt_fixture->call(vt_cf, "_CFRetain", {s.x[4]}));
        vt_encoded().push_back({s.x[1], s.x[4], s.x[0], static_cast<std::int32_t>(s.x[2]), static_cast<std::uint32_t>(s.x[3])});
    }, "/usr/lib/alchemy_test_io.dylib");
    const auto out = f.block(8), session_out = f.block(8);
    // The encoder list: Alchemy's IDs.
    require(f.status(vt, "_VTCopyVideoEncoderList", {0, out}) == 0, "VTCopyVideoEncoderList");
    const auto list = f.cf.array_values(f.engine.read64(out));
    require(list.size() == 3, "three encoders listed");
    const auto entry = [&](Address dictionary, const char* key) { return f.cf.dictionary_value(dictionary, f.cf.string(key)); };
    require(f.cf.utf8(entry(list[0], "EncoderID")) == "com.alchemy.mediafoundation.h264.0" && f.cf.number_value(entry(list[0], "CodecType")) == 0x61766331 &&
            f.cf.utf8(entry(list[0], "CodecName")) == "H.264" && entry(list[1], "IsHardwareAccelerated") == f.cf.boolean(true) &&
            f.cf.utf8(entry(list[2], "EncoderID")) == "com.alchemy.mediafoundation.hevc.0", "the encoders' IDs, codecs and kinds");
    const auto create = [&](std::uint32_t width, std::uint32_t height, std::uint32_t codec, Address specification, Address attributes, Address refcon,
                            bool with_callback = true) {
        alchemy::CallRegs r{};
        r.x[1] = width; r.x[2] = height; r.x[3] = codec; r.x[4] = specification; r.x[5] = attributes; r.x[7] = with_callback ? callback : 0;
        r.push_stack(refcon);
        r.push_stack(session_out);
        f.engine.write64(session_out, 0xdead);
        return static_cast<std::int32_t>(f.regs(vt, "_VTCompressionSessionCreate", r).x0);
    };
    const auto spec = [&](const char* key, Address value) {
        const std::array<std::pair<Address, Address>, 1> entries{{{f.cf.string(key), value}}};
        return f.cf.dictionary(entries);
    };
    // Refusals at creation.
    require(create(64, 48, 0x6a706567 /* 'jpeg' */, 0, 0, 0) == -12908 && f.engine.read64(session_out) == 0, "no JPEG encoder");
    require(create(63, 48, 0x61766331, 0, 0, 0) == -12908, "odd sizes have no encoder");
    require(create(64, 48, 0x68766331, spec("RequireHardwareAcceleratedVideoEncoder", f.cf.boolean(true)), 0, 0) == -12908, "no hardware HEVC encoder");
    require(create(64, 48, 0x61766331, spec("EncoderID", f.cf.string("com.apple.videotoolbox.videoencoder.h264")), 0, 0) == -12908,
            "Apple's encoder IDs are not Alchemy's");
    require(create(64, 48, 0x61766331, spec("RequiredEncoderGPURegistryID", f.cf.integer(1)), 0, 0) == -12908, "no encoder on a named GPU");
    require(f.status(vt, "_VTCopySupportedPropertyDictionaryForEncoder", {64, 48, 0x61766331, spec("RequireHardwareAcceleratedVideoEncoder", f.cf.boolean(true)),
            out, session_out}) == 0 && f.cf.utf8(f.engine.read64(out)) == "com.alchemy.mediafoundation.h264.1" &&
            f.cf.dictionary_value(f.engine.read64(session_out), f.cf.string("AverageBitRate")) != 0, "the hardware encoder's ID and supported keys");

    // H.264 at 64x48 on the hardware encoder, by its ID.
    require(create(64, 48, 0x61766331, spec("EncoderID", f.cf.string("com.alchemy.mediafoundation.h264.1")), 0, 0x5150) == 0, "VTCompressionSessionCreate");
    auto session = f.engine.read64(session_out);
    require(f.call(cfl, "_CFGetTypeID", {session}) == f.call(vt, "_VTCompressionSessionGetTypeID"), "its type");
    const auto set = [&](const char* key, Address value) { return f.status(vt, "_VTSessionSetProperty", {session, f.cf.string(key), value}); };
    require(set("AverageBitRate", f.cf.integer(500000)) == 0 && set("ExpectedFrameRate", f.cf.integer(25)) == 0 &&
            set("MaxKeyFrameInterval", f.cf.integer(10)) == 0 && set("ProfileLevel", f.cf.string("H264_Main_3_1")) == 0 &&
            set("RealTime", f.cf.boolean(true)) == 0 && set("AllowFrameReordering", f.cf.boolean(false)) == 0, "the properties");
    const std::array<Address, 2> limits{f.cf.integer(50000), f.cf.integer(1)};   // 50 kB per second: 400 kb/s
    require(set("DataRateLimits", f.cf.array(limits)) == 0, "DataRateLimits");
    require(set("Quality", f.cf.real(0.5)) == -12900 && set("MaxH264SliceBytes", f.cf.integer(1000)) == -12900, "keys not honoured: not supported");
    require(set("ProfileLevel", f.cf.string("HEVC_Main_AutoLevel")) == -12902 && set("AverageBitRate", f.cf.string("fast")) == -12902,
            "values refused");
    require(set("UsingHardwareAcceleratedVideoEncoder", f.cf.boolean(false)) == -12901, "a read-only key");
    require(f.status(vt, "_VTSessionCopyProperty", {session, f.cf.string("UsingHardwareAcceleratedVideoEncoder"), 0, out}) == 0 &&
            f.engine.read64(out) == f.cf.boolean(true), "the hardware encoder is used");
    // The pool: the session's size, 420v.
    const auto pool = f.call(vt, "_VTCompressionSessionGetPixelBufferPool", {session});
    require(pool != 0 && f.status(cv, "_CVPixelBufferPoolCreatePixelBuffer", {0, pool, out}) == 0, "a pool buffer");
    const auto pooled = f.engine.read64(out);
    require(f.call(cv, "_CVPixelBufferGetWidth", {pooled}) == 64 && f.call(cv, "_CVPixelBufferGetHeight", {pooled}) == 48 &&
            f.call(cv, "_CVPixelBufferGetPixelFormatType", {pooled}) == 0x34323076, "64x48 420v");
    // Frames: a BGRA buffer twice the size (scaled), mid grey then white; the pool's 420v buffer filled directly.
    const auto bgra_out = f.block(8);
    require(f.status(cv, "_CVPixelBufferCreate", {0, 128, 96, 0x42475241, 0, bgra_out}) == 0, "a BGRA buffer");
    const auto bgra = f.engine.read64(bgra_out);
    const auto fill_bgra = [&](std::uint8_t v) {
        f.call(cv, "_CVPixelBufferLockBaseAddress", {bgra, 0});
        const auto base = f.call(cv, "_CVPixelBufferGetBaseAddress", {bgra}), row = f.call(cv, "_CVPixelBufferGetBytesPerRow", {bgra});
        for (Address y = 0; y < 96; ++y) f.memory().write_bytes(base + y * row, std::vector<std::uint8_t>(128 * 4, v));
        f.call(cv, "_CVPixelBufferUnlockBaseAddress", {bgra, 0});
    };
    const auto fill_420v = [&](std::uint8_t y, std::uint8_t cb, std::uint8_t cr) {
        f.call(cv, "_CVPixelBufferLockBaseAddress", {pooled, 0});
        const auto luma = f.call(cv, "_CVPixelBufferGetBaseAddressOfPlane", {pooled, 0}), luma_row = f.call(cv, "_CVPixelBufferGetBytesPerRowOfPlane", {pooled, 0});
        const auto chroma = f.call(cv, "_CVPixelBufferGetBaseAddressOfPlane", {pooled, 1}), chroma_row = f.call(cv, "_CVPixelBufferGetBytesPerRowOfPlane", {pooled, 1});
        for (Address r = 0; r < 48; ++r) f.memory().write_bytes(luma + r * luma_row, std::vector<std::uint8_t>(64, y));
        std::vector<std::uint8_t> pairs;
        for (int i = 0; i < 32; ++i) { pairs.push_back(cb); pairs.push_back(cr); }
        for (Address r = 0; r < 24; ++r) f.memory().write_bytes(chroma + r * chroma_row, pairs);
        f.call(cv, "_CVPixelBufferUnlockBaseAddress", {pooled, 0});
    };
    const auto pts = f.block(24), duration = f.block(24), info = f.block(4);
    const auto times = [&](std::int64_t frame) {
        f.engine.write64(pts, static_cast<Address>(frame * 40)); f.engine.write32(pts + 8, 1000); f.engine.write32(pts + 12, 1); f.engine.write64(pts + 16, 0);
        f.engine.write64(duration, 40); f.engine.write32(duration + 8, 1000); f.engine.write32(duration + 12, 1); f.engine.write64(duration + 16, 0);
    };
    const auto force_key = spec("ForceKeyFrame", f.cf.boolean(true));
    vt_encoded().clear();
    for (std::int64_t i = 0; i < 12; ++i) {
        times(i);
        Address image = bgra;
        if (i < 4) fill_bgra(i < 2 ? 0x80 : 0xff);
        else { fill_420v(static_cast<std::uint8_t>(100 + i), 90, 160); image = pooled; }
        require(f.status(vt, "_VTCompressionSessionEncodeFrame", {session, image, pts, duration, i == 6 ? force_key : 0, 0x1000 + static_cast<Address>(i), info}) == 0,
                "VTCompressionSessionEncodeFrame " + std::to_string(i));
        require((f.engine.read32(info) & 1) == 0, "never asynchronous");
    }
    require(f.status(vt, "_VTCompressionSessionCompleteFrames", {session, pts}) == 0, "CompleteFrames");
    require(fake_video::last.bit_rate == 400000 && fake_video::last.frame_rate == 25 && fake_video::last.keyframe_interval == 10 &&
            fake_video::last.profile == fake_video::enc::Profile::h264_main && fake_video::last.level == 31 && fake_video::last.real_time &&
            !fake_video::last.frame_reordering && fake_video::last.require_hardware == false && fake_video::last.encoder_id == "com.alchemy.mediafoundation.h264.1",
            "the encoder's settings: the bit rate under the data rate limit, the rate, interval, profile and level");
    require(set("AverageBitRate", f.cf.integer(1000000)) == -12901, "the running encoder is not retuned");
    require(vt_encoded().size() == 12, "a sample per frame: " + std::to_string(vt_encoded().size()));
    Address first_format{};
    for (std::size_t i = 0; i < vt_encoded().size(); ++i) {
        const auto& e = vt_encoded()[i];
        require(e.status == 0 && e.flags == 0 && e.sample && e.refcon == 0x1000 + i && e.output_refcon == 0x5150, "frame " + std::to_string(i) + " came out in order");
        alchemy::CallRegs r{};
        r.x[0] = e.sample;
        const auto time = f.block(24);
        r.indirect_result = time;
        f.regs(cm, "_CMSampleBufferGetPresentationTimeStamp", r);
        require(f.engine.read64(time) == i * 40 && f.engine.read32(time + 8) == 1000, "its presentation time");
        f.regs(cm, "_CMSampleBufferGetDuration", r);
        require(f.engine.read64(time) == 40, "its duration");
        // The picture the encoder got: the fake's payload after the NAL header (number, mean Y, Cb, Cr).
        const auto block = f.call(cm, "_CMSampleBufferGetDataBuffer", {e.sample});
        const auto length = f.call(cm, "_CMBlockBufferGetDataLength", {block});
        const auto bytes = f.block(length);
        require(length == 12 && f.status(cm, "_CMBlockBufferCopyDataBytes", {block, 0, length, bytes}) == 0, "the sample's NAL unit");
        const auto y = f.memory().read8(bytes + 9), cb = f.memory().read8(bytes + 10), cr = f.memory().read8(bytes + 11);
        if (i < 2) require(y >= 124 && y <= 128 && cb == 128 && cr == 128, "mid grey as video-range Y'CbCr: " + std::to_string(y));
        else if (i < 4) require(y >= 233 && y <= 236 && cb == 128 && cr == 128, "white as video-range Y'CbCr: " + std::to_string(y));
        else require(y == 100 + i && cb == 90 && cr == 160, "the 420v planes as they are");
        const bool key = i == 0 || i == 6;
        require((f.memory().read8(bytes + 4) & 0x1f) == (key ? 5 : 1), "keyframes where they were forced (and the first)");
        const auto attachments = f.call(cm, "_CMSampleBufferGetSampleAttachmentsArray", {e.sample, 0});
        const auto first = attachments ? f.cf.element(attachments, 0) : 0;
        const auto not_sync = first ? f.cf.dictionary_value(first, f.cf.string("NotSync")) : 0;
        require(key ? not_sync == 0 : not_sync == f.cf.boolean(true), "NotSync on the others");
        const auto format = f.call(cm, "_CMSampleBufferGetFormatDescription", {e.sample});
        if (!first_format) first_format = format;
        require(format == first_format, "one format description");
    }
    // The format description: H.264 64x48 from the encoder's parameter sets, with its avcC atom.
    require(f.call(cm, "_CMFormatDescriptionGetMediaSubType", {first_format}) == 0x61766331 &&
            f.call(cm, "_CMVideoFormatDescriptionGetDimensions", {first_format}) == (64 | (Address{48} << 32)), "avc1 64x48");
    const auto sets_count = f.block(8), header = f.block(4);
    require(f.status(cm, "_CMVideoFormatDescriptionGetH264ParameterSetAtIndex", {first_format, 0, 0, 0, sets_count, header}) == 0 &&
            f.engine.read64(sets_count) == 2 && f.engine.read32(header) == 4, "two parameter sets, 4-byte lengths");
    const auto atoms = f.call(cm, "_CMFormatDescriptionGetExtension", {first_format, f.cf.string("SampleDescriptionExtensionAtoms")});
    const auto avcc = atoms ? f.cf.dictionary_value(atoms, f.cf.string("avcC")) : 0;
    require(avcc != 0, "the avcC atom");
    const auto avcc_bytes = f.call(cfl, "_CFDataGetLength", {avcc});
    std::vector<std::uint8_t> record(avcc_bytes);
    f.memory().read_bytes(f.call(cfl, "_CFDataGetBytePtr", {avcc}), record);
    const auto read = vc::read_avc_configuration(record);
    require(read && read->sets.size() == 2 && read->nal_length_size == 4 && record[1] == 77 && record[3] == 31, "avcC: Main 3.1, SPS and PPS");
    // A CMVideoFormatDescription made from that atom has the same parameter sets.
    const std::array<std::pair<Address, Address>, 1> atom_entries{{{f.cf.string("avcC"), avcc}}};
    const std::array<std::pair<Address, Address>, 1> extension_entries{{{f.cf.string("SampleDescriptionExtensionAtoms"), f.cf.dictionary(atom_entries)}}};
    require(f.status(cm, "_CMVideoFormatDescriptionCreate", {0, 0x61766331, 64, 48, f.cf.dictionary(extension_entries), out}) == 0 &&
            f.status(cm, "_CMVideoFormatDescriptionGetH264ParameterSetAtIndex", {f.engine.read64(out), 1, 0, 0, sets_count, header}) == 0 &&
            f.engine.read64(sets_count) == 2, "the atom's parameter sets");
    for (const auto& e : vt_encoded()) f.call(cfl, "_CFRelease", {e.sample});
    f.call(vt, "_VTCompressionSessionInvalidate", {session});
    times(20);
    require(f.status(vt, "_VTCompressionSessionEncodeFrame", {session, bgra, pts, duration, 0, 0, info}) == -12903, "invalidated");
    f.call(cfl, "_CFRelease", {session});

    // HEVC with reordered frames, through output handlers; a picture that never comes out is reported as dropped.
    require(create(64, 48, 0x68766331, 0, 0, 0, false) == 0, "an HEVC session without a callback");
    session = f.engine.read64(session_out);
    require(f.status(vt, "_VTSessionSetProperty", {session, f.cf.string("ProfileLevel"), f.cf.string("HEVC_Main_AutoLevel")}) == 0 &&
            f.status(vt, "_VTCompressionSessionPrepareToEncodeFrames", {session}) == 0 && fake_video::last.frame_reordering &&
            fake_video::last.profile == fake_video::enc::Profile::hevc_main, "prepared: reordering allowed by default");
    times(0);
    require(f.status(vt, "_VTCompressionSessionEncodeFrame", {session, bgra, pts, duration, 0, 0, info}) == -12902, "no callback: handlers only");
    static std::vector<std::tuple<std::int32_t, std::uint32_t, Address>> handled;
    handled.clear();
    const auto handler_fn = f.process.install_function("_alchemy_test_vt_handled", [](transmute::CpuState& s, transmute::GuestMemory&) {
        // block(OSStatus, VTEncodeInfoFlags, CMSampleBufferRef)
        if (s.x[3]) static_cast<void>(vt_fixture->call(vt_cf, "_CFRetain", {s.x[3]}));
        handled.emplace_back(static_cast<std::int32_t>(s.x[1]), static_cast<std::uint32_t>(s.x[2]), s.x[3]);
    }, "/usr/lib/alchemy_test_io.dylib");
    // A compiler-emitted (global) block whose invoke is that function.
    const auto handler = f.block(32), descriptor = f.block(16);
    f.engine.write64(descriptor + 8, 32);
    f.engine.write64(handler, f.process.resolve("/usr/lib/libSystem.B.dylib", "__NSConcreteGlobalBlock")->address);
    f.engine.write32(handler + 8, 1U << 28);   // BLOCK_IS_GLOBAL
    f.engine.write64(handler + 16, handler_fn);
    f.engine.write64(handler + 24, descriptor);
    for (std::int64_t i = 0; i < 5; ++i) {
        times(i);
        require(f.status(vt, "_VTCompressionSessionEncodeFrameWithOutputHandler", {session, bgra, pts, duration, 0, info, handler}) == 0, "HEVC frame");
    }
    require(handled.size() == 5, "frames 0, 2, 1, 4, 3 so far: " + std::to_string(handled.size()));
    require(f.status(vt, "_VTCompressionSessionCompleteFrames", {session, pts}) == 0, "CompleteFrames");
    std::vector<std::int64_t> order, decode_order;
    for (const auto& [result, flags, sample] : handled) {
        require(result == 0 && flags == 0 && sample, "handled");
        alchemy::CallRegs r{};
        r.x[0] = sample;
        const auto time = f.block(24);
        r.indirect_result = time;
        f.regs(cm, "_CMSampleBufferGetPresentationTimeStamp", r);
        order.push_back(static_cast<std::int64_t>(f.engine.read64(time)));
        f.regs(cm, "_CMSampleBufferGetDecodeTimeStamp", r);
        decode_order.push_back(static_cast<std::int64_t>(f.engine.read64(time)));
    }
    require(order == std::vector<std::int64_t>{0, 80, 40, 160, 120}, "decode order: the reordered pairs");
    require(std::is_sorted(decode_order.begin(), decode_order.end()) && decode_order[1] <= order[1], "decode times in order, before the presentation times");
    const auto hevc_format = f.call(cm, "_CMSampleBufferGetFormatDescription", {std::get<2>(handled[0])});
    const auto hevc_atoms = f.call(cm, "_CMFormatDescriptionGetExtension", {hevc_format, f.cf.string("SampleDescriptionExtensionAtoms")});
    const auto hvcc = hevc_atoms ? f.cf.dictionary_value(hevc_atoms, f.cf.string("hvcC")) : 0;
    require(hvcc != 0 && f.call(cm, "_CMVideoFormatDescriptionGetDimensions", {hevc_format}) == (64 | (Address{48} << 32)), "hvc1 64x48 with its hvcC atom");
    std::vector<std::uint8_t> hevc_record(f.call(cfl, "_CFDataGetLength", {hvcc}));
    f.memory().read_bytes(f.call(cfl, "_CFDataGetBytePtr", {hvcc}), hevc_record);
    const auto hevc_read = vc::read_hevc_configuration(hevc_record);
    require(hevc_read && hevc_read->sets.size() == 3 && hevc_record[12] == 93 && (hevc_record[1] & 0x1f) == 1, "hvcC: Main, level 3.1, VPS, SPS, PPS");
    for (const auto& h : handled) f.call(cfl, "_CFRelease", {std::get<2>(h)});
    f.call(vt, "_VTCompressionSessionInvalidate", {session});
    f.call(cfl, "_CFRelease", {session});

    // AllowTemporalCompression off: every frame a keyframe.
    // Software encoders only (EnableHardwareAcceleratedVideoEncoder false): the encoder opened is a software one.
    require(create(64, 48, 0x61766331, spec("EnableHardwareAcceleratedVideoEncoder", f.cf.boolean(false)), 0, 0) == 0 &&
            f.status(vt, "_VTCompressionSessionPrepareToEncodeFrames", {f.engine.read64(session_out)}) == 0 && fake_video::last.require_software &&
            f.status(vt, "_VTSessionCopyProperty", {f.engine.read64(session_out), f.cf.string("UsingHardwareAcceleratedVideoEncoder"), 0, out}) == 0 &&
            f.engine.read64(out) == f.cf.boolean(false), "a software encoder when hardware ones are not enabled");
    f.call(vt, "_VTCompressionSessionInvalidate", {f.engine.read64(session_out)});
    f.call(cfl, "_CFRelease", {f.engine.read64(session_out)});
    require(create(64, 48, 0x61766331, 0, 0, 0) == 0, "an H.264 session");
    session = f.engine.read64(session_out);
    require(f.status(vt, "_VTSessionSetProperty", {session, f.cf.string("AllowTemporalCompression"), f.cf.boolean(false)}) == 0, "no temporal compression");
    vt_encoded().clear();
    for (std::int64_t i = 0; i < 3; ++i) {
        times(i);
        require(f.status(vt, "_VTCompressionSessionEncodeFrame", {session, bgra, pts, duration, 0, 0, info}) == 0, "a frame");
    }
    for (const auto& e : vt_encoded()) {
        const auto attachments = f.call(cm, "_CMSampleBufferGetSampleAttachmentsArray", {e.sample, 0});
        require(!f.cf.dictionary_value(f.cf.element(attachments, 0), f.cf.string("NotSync")), "every frame a keyframe");
        f.call(cfl, "_CFRelease", {e.sample});
    }
    times(1);
    require(f.status(vt, "_VTCompressionSessionEncodeFrame", {session, bgra, pts, duration, 0, 0, info}) == -12902, "presentation times increase");
    f.call(vt, "_VTCompressionSessionInvalidate", {session});
    f.call(cfl, "_CFRelease", {session});

    // Multi-pass is not available.
    require(f.status(vt, "_VTMultiPassStorageCreate", {0, 0, f.block(48), 0, out}) == -12907 && f.engine.read64(out) == 0, "no multi-pass storage");
}

// VTCompressionSession over the host's own encoders (skipped where there is none): 30 frames of a moving gradient
// at 160x120, H.264 and HEVC, decoded back by the host's decoders (host/media_codec.hpp) from the session's samples
// and format description: every frame, close to what went in.
void vt_compression_host(Fixture& f) {
    namespace mc = alchemy::host::media_codec;
    namespace enc = alchemy::gui::completion_kit::media::video_encoding;
    enc::set_backend({});
    struct Restore { ~Restore() { fake_video::install(); } } restore;
    vt_fixture = &f;
    const auto cfl = std::string(vt_cf);
    const auto out = f.block(8), session_out = f.block(8), info = f.block(4), pts = f.block(24), duration = f.block(24);
    const auto callback = f.process.install_function("_alchemy_test_vt_encoded_host", [](transmute::CpuState& s, transmute::GuestMemory&) {
        if (s.x[4]) static_cast<void>(vt_fixture->call(vt_cf, "_CFRetain", {s.x[4]}));
        vt_encoded().push_back({s.x[1], s.x[4], s.x[0], static_cast<std::int32_t>(s.x[2]), static_cast<std::uint32_t>(s.x[3])});
    }, "/usr/lib/alchemy_test_io.dylib");
    // Whether the host's own encoder and decoder of `codec` work here (a listed codec may not: a server image's): a few
    // pictures encoded and one decoded back straight through the host layer; the reason when not.
    const auto usable = [](mc::Codec codec, std::string& why) {
        mc::VideoSettings settings;
        settings.codec = codec;
        settings.width = 160;
        settings.height = 120;
        const auto encoder = mc::open_video(settings);
        if (!encoder) { why = std::string("no encoder opens (") + std::string(alchemy::host::name(encoder.error())) + ")"; return false; }
        std::vector<std::uint8_t> luma(160 * 120, 100), chroma(160 * 60, 128);
        std::vector<mc::Sample> samples;
        bool ok = true;
        for (int i = 0; i < 3 && ok; ++i) ok = static_cast<bool>(mc::encode(*encoder, {160, 120, luma.data(), 160, chroma.data(), 160, i * 333333LL, 333333}, false, samples));
        ok = ok && static_cast<bool>(mc::drain(*encoder, samples));
        const auto sets = mc::parameter_sets(*encoder);
        mc::close(*encoder);
        if (!ok || samples.empty() || sets.empty()) { why = "the encoder gives no samples"; return false; }
        const auto decoder = mc::open_video_decoder({codec, sets, 160, 120, {}});
        if (!decoder) { why = std::string("no decoder opens (") + std::string(alchemy::host::name(decoder.error())) + ")"; return false; }
        std::vector<mc::DecodedPicture> pictures;
        for (const auto& s : samples) ok = ok && static_cast<bool>(mc::decode(*decoder, s, pictures));
        ok = ok && static_cast<bool>(mc::drain(*decoder, pictures));
        mc::close(*decoder);
        if (!ok || pictures.empty()) { why = "the decoder gives no pictures"; return false; }
        return true;
    };
    // Each codec on the encoder VideoToolbox picks (hardware first where there is one) and on the software encoders
    // only (kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder false: what a host without a GPU has).
    for (const auto& [codec, software] : {std::pair{0x61766331u, false}, std::pair{0x61766331u, true}, std::pair{0x68766331u, false},
                                          std::pair{0x68766331u, true}}) {
        const std::string name = std::string(codec == 0x61766331u ? "H.264" : "HEVC") + (software ? " (software)" : "");
        const auto mc_codec = codec == 0x61766331u ? mc::Codec::h264 : mc::Codec::hevc;
        const auto decoders = mc::decoders(mc_codec);
        std::string why;
        if (decoders && !decoders->empty() && !usable(mc_codec, why)) {
            std::cout << "[qlab media] the host's " << name << " codecs do not work here (" << why << "): skipped" << std::endl;
            continue;
        }
        alchemy::CallRegs r{};
        r.x[1] = 160; r.x[2] = 120; r.x[3] = codec; r.x[7] = callback;
        if (software) {
            const std::array<std::pair<Address, Address>, 1> entries{{{f.cf.string("EnableHardwareAcceleratedVideoEncoder"), f.cf.boolean(false)}}};
            r.x[4] = f.cf.dictionary(entries);
        }
        r.push_stack(Address{0});
        r.push_stack(session_out);
        const auto created = static_cast<std::int32_t>(f.regs(vt, "_VTCompressionSessionCreate", r).x0);
        if (created == -12908 || !decoders || decoders->empty()) {
            std::cout << "[qlab media] no " << name << " encoder or decoder on this host: skipped" << std::endl;
            continue;
        }
        require(created == 0, std::string(name) + ": VTCompressionSessionCreate");
        const auto session = f.engine.read64(session_out);
        require(f.status(vt, "_VTSessionSetProperty", {session, f.cf.string("AverageBitRate"), f.cf.integer(2000000)}) == 0, "a generous bit rate");
        const auto buffer_out = f.block(8);
        require(f.status(cv, "_CVPixelBufferCreate", {0, 160, 120, 0x42475241, 0, buffer_out}) == 0, "BGRA");
        const auto image = f.engine.read64(buffer_out);
        std::vector<std::vector<std::uint8_t>> grey;   // each frame's luma as the encoder got it (BT.601 video range of R = G = B)
        vt_encoded().clear();
        for (int i = 0; i < 30; ++i) {
            f.call(cv, "_CVPixelBufferLockBaseAddress", {image, 0});
            const auto base = f.call(cv, "_CVPixelBufferGetBaseAddress", {image}), row = f.call(cv, "_CVPixelBufferGetBytesPerRow", {image});
            std::vector<std::uint8_t> luma(160 * 120);
            for (Address y = 0; y < 120; ++y) {
                std::vector<std::uint8_t> line(160 * 4);
                for (Address x = 0; x < 160; ++x) {
                    const auto v = static_cast<std::uint8_t>((x + y + i * 3) & 0xff);
                    line[x * 4] = line[x * 4 + 1] = line[x * 4 + 2] = v;
                    line[x * 4 + 3] = 255;
                    luma[y * 160 + x] = static_cast<std::uint8_t>(std::lround(16 + v * 219.0 / 255));
                }
                f.memory().write_bytes(base + y * row, line);
            }
            f.call(cv, "_CVPixelBufferUnlockBaseAddress", {image, 0});
            grey.push_back(std::move(luma));
            f.engine.write64(pts, static_cast<Address>(i)); f.engine.write32(pts + 8, 30); f.engine.write32(pts + 12, 1); f.engine.write64(pts + 16, 0);
            f.engine.write64(duration, 1); f.engine.write32(duration + 8, 30); f.engine.write32(duration + 12, 1); f.engine.write64(duration + 16, 0);
            require(f.status(vt, "_VTCompressionSessionEncodeFrame", {session, image, pts, duration, 0, static_cast<Address>(i), info}) == 0, std::string(name) + " encodes");
        }
        require(f.status(vt, "_VTCompressionSessionCompleteFrames", {session, pts}) == 0, std::string(name) + ": CompleteFrames");
        require(vt_encoded().size() == 30, std::string(name) + ": 30 samples, " + std::to_string(vt_encoded().size()));
        // The parameter sets from the format description, the samples' bytes and times to the host's decoder.
        const auto format = f.call(cm, "_CMSampleBufferGetFormatDescription", {vt_encoded()[0].sample});
        const auto count = f.block(8), header = f.block(4), pointer = f.block(8), size = f.block(8);
        const char* getter = codec == 0x61766331u ? "_CMVideoFormatDescriptionGetH264ParameterSetAtIndex" : "_CMVideoFormatDescriptionGetHEVCParameterSetAtIndex";
        require(f.status(cm, getter, {format, 0, pointer, size, count, header}) == 0 && f.engine.read32(header) == 4, std::string(name) + ": the parameter sets");
        std::vector<std::vector<std::uint8_t>> sets;
        for (Address k = 0; k < f.engine.read64(count); ++k) {
            require(f.status(cm, getter, {format, k, pointer, size, 0, 0}) == 0, "a parameter set");
            std::vector<std::uint8_t> set(f.engine.read64(size));
            f.memory().read_bytes(f.engine.read64(pointer), set);
            sets.push_back(std::move(set));
        }
        const auto decoder = mc::open_video_decoder({mc_codec, sets, 160, 120, {}});
        require(static_cast<bool>(decoder), std::string(name) + ": the host's decoder opens on the session's parameter sets");
        std::vector<mc::DecodedPicture> pictures;
        for (const auto& e : vt_encoded()) {
            require(e.status == 0 && e.sample, name + ": a sample for frame " + std::to_string(e.refcon) + " (status " + std::to_string(e.status) + ", flags " +
                    std::to_string(e.flags) + ")");
            const auto block = f.call(cm, "_CMSampleBufferGetDataBuffer", {e.sample});
            const auto length = f.call(cm, "_CMBlockBufferGetDataLength", {block});
            const auto bytes = f.block(length);
            require(f.status(cm, "_CMBlockBufferCopyDataBytes", {block, 0, length, bytes}) == 0, "its bytes");
            mc::Sample sample;
            sample.data.resize(length);
            f.memory().read_bytes(bytes, sample.data);
            alchemy::CallRegs t{};
            t.x[0] = e.sample;
            const auto time = f.block(24);
            t.indirect_result = time;
            f.regs(cm, "_CMSampleBufferGetPresentationTimeStamp", t);
            sample.pts = static_cast<std::int64_t>(f.engine.read64(time)) * 10000000 / 30;
            sample.duration = 10000000 / 30;
            const auto attachments = f.call(cm, "_CMSampleBufferGetSampleAttachmentsArray", {e.sample, 0});
            sample.keyframe = !f.cf.dictionary_value(f.cf.element(attachments, 0), f.cf.string("NotSync"));
            require(static_cast<bool>(mc::decode(*decoder, sample, pictures)), std::string(name) + " decodes");
            f.call(cfl, "_CFRelease", {e.sample});
        }
        require(static_cast<bool>(mc::drain(*decoder, pictures)) && pictures.size() == 30, std::string(name) + ": 30 pictures, " + std::to_string(pictures.size()));
        double worst = 1e9;
        for (std::size_t i = 0; i < pictures.size(); ++i) {
            const auto& p = pictures[i];
            require(p.width == 160 && p.height == 120 && p.pts == static_cast<std::int64_t>(i) * 10000000 / 30, std::string(name) + ": picture in order");
            double error = 0;
            for (std::uint32_t y = 0; y < 120; ++y)
                for (std::uint32_t x = 0; x < 160; ++x) {
                    const double d = double(p.luma[y * p.luma_stride + x]) - grey[i][y * 160 + x];
                    error += d * d;
                }
            worst = std::min(worst, 10 * std::log10(255.0 * 255.0 / std::max(error / (160.0 * 120.0), 1e-9)));
        }
        require(worst > 30, std::string(name) + ": every picture close to its frame, worst PSNR " + std::to_string(worst));
        std::cout << "[qlab media] " << name << " through VTCompressionSession and back: worst PSNR " << worst << " dB" << std::endl;
        mc::close(*decoder);
        f.call(vt, "_VTCompressionSessionInvalidate", {session});
        f.call(cfl, "_CFRelease", {session});
    }
}

// AVFoundation over container files: libvpx's VP9 WebM vector as an AVURLAsset (its video track),
// read as stored samples and as decoded 420v pictures (against the vector's per-frame MD5s), and
// an image from AVAssetImageGenerator; an MP3 file's audio track read as its stored packets.
void av_video(Fixture& f) {
    const auto directory = std::filesystem::temp_directory_path() / "alchemy_qlab_media_av_video";
    std::filesystem::create_directories(directory);
    const std::string third_party = ALCHEMY_THIRD_PARTY;
    const auto webm = directory / "size-08x08.webm";
    std::filesystem::copy_file(third_party + "/libvpx/test-vectors/vp90-2-02-size-08x08.webm", webm, std::filesystem::copy_options::overwrite_existing);
    std::vector<std::string> expected;
    {
        std::ifstream in(third_party + "/libvpx/test-vectors/vp90-2-02-size-08x08.webm.md5");
        for (std::string line; std::getline(in, line);) if (line.size() >= 32) expected.push_back(line.substr(0, 32));
    }
    const auto url = f.cf.file_url(alchemy::guest_path_string(webm), false);
    const auto asset = f.send(f.cls("AVURLAsset"), "URLAssetWithURL:options:", {url, 0});
    const auto tracks = f.send(asset, "tracks");
    require(f.cf.length(tracks) == 1, "the WebM asset has one track");
    const auto track = f.cf.element(tracks, 0);
    require(f.cf.utf8(f.send(track, "mediaType")) == "vide" && (f.send(track, "isDecodable") & 0xff) == 1, "a decodable video track");
    require(f.cf.length(f.send(asset, "tracksWithMediaType:", {f.cf.string("vide")})) == 1 &&
            f.cf.length(f.send(asset, "tracksWithMediaType:", {f.cf.string("soun")})) == 0, "tracksWithMediaType:");
    const auto size = f.objc.send(track, f.objc.selector("naturalSize"), {});
    require(std::bit_cast<double>(size.v[0][0]) == 8.0 && std::bit_cast<double>(size.v[1][0]) == 8.0, "naturalSize 8x8");
    const auto description = f.cf.element(f.send(track, "formatDescriptions"), 0);
    require(f.call(cm, "_CMFormatDescriptionGetMediaSubType", {description}) == 0x76703039, "the description is VP9");   // 'vp09'
    // Stored samples: every frame, each a CMSampleBuffer with data.
    const auto read = [&](Address settings) {
        const auto error = f.block(8);
        f.engine.write64(error, 0);
        const auto reader = f.send(f.cls("AVAssetReader"), "assetReaderWithAsset:error:", {asset, error});
        require(reader != 0, "an AVAssetReader of the WebM asset");
        const auto output = f.send(f.cls("AVAssetReaderTrackOutput"), "assetReaderTrackOutputWithTrack:outputSettings:", {track, settings});
        f.send(reader, "addOutput:", {output});
        require((f.send(reader, "startReading") & 0xff) == 1, "startReading");
        std::vector<Address> buffers;
        while (const auto buffer = f.send(output, "copyNextSampleBuffer")) buffers.push_back(buffer);
        require(f.send(reader, "status") == 2, "the reader completes");
        return buffers;
    };
    const auto stored = read(0);
    require(stored.size() == expected.size(), "a stored sample per frame: " + std::to_string(stored.size()));
    for (const auto buffer : stored) {
        const auto block = f.call(cm, "_CMSampleBufferGetDataBuffer", {buffer});
        require(block && f.call(cm, "_CMBlockBufferGetDataLength", {block}) > 0 && f.call(cm, "_CMSampleBufferGetFormatDescription", {buffer}) == description,
                "stored samples carry their bytes and the track's description");
        f.call(vt_cf, "_CFRelease", {buffer});
    }
    // Decoded pictures in 420v, per-frame MD5s as the vector lists them.
    const auto key = f.cf.string("PixelFormatType"), value = f.cf.integer(0x34323076);   // '420v'
    const std::array<std::pair<Address, Address>, 1> entries{{{key, value}}};
    const auto pictures = read(f.cf.dictionary(entries));
    require(pictures.size() == expected.size(), "a picture per frame: " + std::to_string(pictures.size()));
    for (std::size_t i = 0; i < pictures.size(); ++i) {
        const auto image = f.call(cm, "_CMSampleBufferGetImageBuffer", {pictures[i]});
        require(image && md5_hex(i420_of(f, image)) == expected[i], "decoded frame " + std::to_string(i + 1) + " MD5");
        f.call(vt_cf, "_CFRelease", {pictures[i]});
    }
    // An image of the first frame.
    const auto generator = f.send(f.cls("AVAssetImageGenerator"), "assetImageGeneratorWithAsset:", {asset});
    const auto when = f.block(24), actual = f.block(24), error = f.block(8);
    write_time(f, when, 0, 1000);
    f.engine.write64(error, 0);
    const auto image = f.send(generator, "copyCGImageAtTime:actualTime:error:", {when, actual, error});
    require(image && f.call(vt_cg, "_CGImageGetWidth", {image}) == 8 && f.call(vt_cg, "_CGImageGetHeight", {image}) == 8 &&
            f.engine.read64(error) == 0, "AVAssetImageGenerator makes an 8x8 image");
    f.call(vt_cg, "_CGImageRelease", {image});

    // An MP3 asset: one audio track, whose stored packets keep their MPEG description.
    const auto mp3 = directory / "hecommon.mp3";
    std::filesystem::copy_file(third_party + "/minimp3/vectors/l3-hecommon.bit", mp3, std::filesystem::copy_options::overwrite_existing);
    const auto audio_asset = f.send(f.cls("AVURLAsset"), "URLAssetWithURL:options:", {f.cf.file_url(alchemy::guest_path_string(mp3), false), 0});
    const auto audio_tracks = f.send(audio_asset, "tracksWithMediaType:", {f.cf.string("soun")});
    require(f.cf.length(audio_tracks) == 1, "the MP3 asset has an audio track");
    const auto reader = f.send(f.cls("AVAssetReader"), "assetReaderWithAsset:error:", {audio_asset, 0});
    const auto output = f.send(f.cls("AVAssetReaderTrackOutput"), "assetReaderTrackOutputWithTrack:outputSettings:", {f.cf.element(audio_tracks, 0), 0});
    f.send(reader, "addOutput:", {output});
    require((f.send(reader, "startReading") & 0xff) == 1, "startReading the MP3");
    const auto packet = f.send(output, "copyNextSampleBuffer");
    const auto asbd = packet ? f.call(cm, "_CMAudioFormatDescriptionGetStreamBasicDescription", {f.call(cm, "_CMSampleBufferGetFormatDescription", {packet})}) : 0;
    require(asbd && f.engine.read32(asbd + 8) == 0x2e6d7033, "stored MP3 packets are '.mp3'");
    f.call(vt_cf, "_CFRelease", {packet});
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// AVPlayer: libvpx's VP9 WebM vector loads (status ready, size), shows its pictures on an
// AVPlayerLayer and through an AVPlayerItemVideoOutput, plays to its end and pauses there, and
// seeks; an MP3 item's audio reaches the output device.
// AudioServices: a 0.1 s WAVE of 0.5 plays on the fake device; when it ends, the procedure added for it
// and a play's block complete on the main queue; a sound disposed while it plays does not complete;
// the alert completes at once; the per-sound properties; a sound over 30 s is refused.
void system_sounds(Fixture& f) {
    const auto directory = std::filesystem::temp_directory_path() / "alchemy_media_system_sounds";
    std::filesystem::create_directories(directory);
    const auto path = write_wave(directory / "ding.wav", 48000, 2, 4800, [](std::uint32_t, std::uint16_t) { return static_cast<std::int16_t>(16384); });
    const auto mode = f.engine.read64(f.symbol(vt_cf, "_kCFRunLoopDefaultMode"));
    const auto run = [&](double seconds) {
        alchemy::CallRegs r;
        r.x[0] = mode;
        r.set_d(0, seconds);
        static_cast<void>(f.regs(vt_cf, "_CFRunLoopRunInMode", r));
    };
    static std::atomic<int> completions{0}, blocks{0};
    static std::uint64_t completed_id{}, completed_client{};
    const auto proc = f.process.install_function("_alchemy_test_sound_done", [](transmute::CpuState& s, transmute::GuestMemory&) {
        completed_id = s.x[0] & 0xffffffff;
        completed_client = s.x[1];
        ++completions;
    }, "/usr/lib/alchemy_test_io.dylib");
    const auto b = alchemy::gui::security::bridge();
    const auto done = b->make_block([](transmute::CpuState&, transmute::GuestMemory&) { ++blocks; });
    const auto pump = [&](const std::function<bool()>& until) {
        for (int i = 0; i < 200 && !until(); ++i) {
            static_cast<void>(FakeStream::play_paced(1024));
            run(0.005);
        }
        return until();
    };
    const auto out = f.block(8);
    require(f.status(at, "_AudioServicesCreateSystemSoundID", {f.cf.file_url(alchemy::guest_path_string(path), false), out}) == 0,
            "AudioServicesCreateSystemSoundID");
    const auto sound = f.engine.read32(out);
    require(f.status(at, "_AudioServicesAddSystemSoundCompletion", {sound, f.call(vt_cf, "_CFRunLoopGetMain"), 0, proc, 0x42}) == 0,
            "AudioServicesAddSystemSoundCompletion");
    f.call(at, "_AudioServicesPlaySystemSound", {sound});
    const auto heard = FakeStream::play_until_sound(256);
    require(std::fabs(heard[0] - 0.5f) < 0.01f && std::fabs(heard[1] - 0.5f) < 0.01f, "the sound plays: " + std::to_string(heard[0]));
    require(pump([] { return completions == 1; }) && completed_id == sound && completed_client == 0x42, "its completion procedure ran");
    f.call(at, "_AudioServicesPlaySystemSoundWithCompletion", {sound, done});
    require(pump([] { return blocks == 1 && completions == 2; }), "the block and the procedure complete a second play");
    // Properties: kAudioServicesPropertyIsUISound per sound.
    const auto specifier = f.block(4), size = f.block(4), value = f.block(4);
    f.engine.write32(specifier, sound);
    f.engine.write32(size, 4);
    require(f.status(at, "_AudioServicesGetProperty", {0x69737569, 4, specifier, size, value}) == 0 && f.engine.read32(value) == 1, "a UI sound");
    f.engine.write32(value, 0);
    require(f.status(at, "_AudioServicesSetProperty", {0x69737569, 4, specifier, 4, value}) == 0 &&
            f.status(at, "_AudioServicesGetProperty", {0x69737569, 4, specifier, size, value}) == 0 && f.engine.read32(value) == 0, "set to not a UI sound");
    require(f.status(at, "_AudioServicesGetPropertyInfo", {0x69737569, 0, 0, size, 0}) == static_cast<std::int32_t>(0x7370633f) &&
            f.status(at, "_AudioServicesGetPropertyInfo", {0x7a7a7a7a, 4, specifier, size, 0}) == static_cast<std::int32_t>(0x7074793f),
            "specifier and property errors");
    // Disposed while it plays: it stops and does not complete.
    f.call(at, "_AudioServicesPlaySystemSound", {sound});
    static_cast<void>(FakeStream::play_until_sound(256));
    require(f.status(at, "_AudioServicesDisposeSystemSoundID", {sound}) == 0, "disposed while playing");
    static_cast<void>(pump([] { return false; }));
    require(completions == 2, "no completion for the disposed play");
    require(f.status(at, "_AudioServicesDisposeSystemSoundID", {sound}) == -1500, "disposed twice");
    // kSystemSoundID_UserPreferredAlert (no AppKit here: nothing sounds) completes at once.
    f.call(at, "_AudioServicesPlayAlertSoundWithCompletion", {0x1000, done});
    require(pump([] { return blocks == 2; }), "the alert completes");
    // Over 30 seconds is not a system sound.
    const auto long_path = write_wave(directory / "long.wav", 8000, 1, 8000 * 31, [](std::uint32_t, std::uint16_t) { return static_cast<std::int16_t>(0); });
    require(f.status(at, "_AudioServicesCreateSystemSoundID", {f.cf.file_url(alchemy::guest_path_string(long_path), false), out}) == -1502,
            "kAudioServicesSystemSoundExceededMaximumDurationError");
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// AVQueuePlayer: two 0.15 s items played in turn (an end notification each, then no item); inserting and removing;
// AVPlayerLooper keeping copies of its template queued and counting loops; forwardPlaybackEndTime.
void queue_player(Fixture& f) {
    const auto directory = std::filesystem::temp_directory_path() / "alchemy_qlab_media_queue";
    std::filesystem::create_directories(directory);
    const auto path = write_wave(directory / "short.wav", 48000, 1, 7200, [](std::uint32_t, std::uint16_t) { return static_cast<std::int16_t>(4096); });
    const auto url = f.cf.file_url(alchemy::guest_path_string(path), false);
    const auto mode = f.engine.read64(f.symbol(vt_cf, "_kCFRunLoopDefaultMode"));
    const auto run = [&](double seconds) {
        alchemy::CallRegs r;
        r.x[0] = mode;
        r.set_d(0, seconds);
        static_cast<void>(f.regs(vt_cf, "_CFRunLoopRunInMode", r));
    };
    static std::atomic<int> ended{0};
    const auto observer_class = f.objc.register_native_class("AlchemyQueueTestObserver", f.cls("NSObject"), 8);
    static_cast<void>(f.objc.add_native_method(observer_class, "itemEnded:", [](transmute::CpuState&, transmute::GuestMemory&) { ++ended; }, false, "v@:@"));
    const auto observer = f.send(f.send(observer_class, "alloc"), "init");
    const auto center = f.send(f.cls("NSNotificationCenter"), "defaultCenter");
    f.send(center, "addObserver:selector:name:object:", {observer, f.objc.selector("itemEnded:"), f.cf.string("AVPlayerItemDidPlayToEndTimeNotification"), 0});
    const auto item = [&] { return f.send(f.cls("AVPlayerItem"), "playerItemWithURL:", {url}); };
    const auto a = item(), b = item(), c = item();
    const auto queue = f.send(f.cls("AVQueuePlayer"), "queuePlayerWithItems:", {f.cf.array(std::vector<Address>{a, b})});
    require(queue && f.send(queue, "currentItem") == a && f.cf.length(f.send(queue, "items")) == 2 && f.send(queue, "actionAtItemEnd") == 0, "AVQueuePlayer");
    require((f.send(queue, "canInsertItem:afterItem:", {c, a}) & 0xff) == 1 && (f.send(queue, "canInsertItem:afterItem:", {a, 0}) & 0xff) == 0, "canInsertItem:afterItem:");
    f.send(queue, "insertItem:afterItem:", {c, a});
    require(f.cf.array_values(f.send(queue, "items")) == std::vector<Address>{a, c, b}, "inserted after the current item");
    f.send(queue, "removeItem:", {c});
    require(f.cf.length(f.send(queue, "items")) == 2, "removed");
    f.send(queue, "play");
    for (int i = 0; i < 100 && (ended < 2 || f.send(queue, "currentItem")); ++i) run(0.03);
    require(ended == 2 && f.send(queue, "currentItem") == 0 && f.cf.length(f.send(queue, "items")) == 0, "both items played, then no item: " + std::to_string(ended.load()));
    require(f.objc.send(queue, f.objc.selector("rate"), {}).s(0) == 0, "stopped after the last item");
    // AVPlayerLooper on a fresh queue player.
    const auto looping = f.send(f.send(f.cls("AVQueuePlayer"), "alloc"), "init");
    const auto looper = f.send(f.cls("AVPlayerLooper"), "playerLooperWithPlayer:templateItem:", {looping, item()});
    require(looper && f.send(looper, "status") == 1 && f.cf.length(f.send(looper, "loopingPlayerItems")) == 3, "three copies queued");
    f.send(looping, "play");
    for (int i = 0; i < 100 && f.send(looper, "loopCount") < 2; ++i) run(0.03);
    require(f.send(looper, "loopCount") >= 2 && f.cf.length(f.send(looper, "loopingPlayerItems")) == 3 && f.send(looping, "currentItem") != 0, "looping");
    f.send(looper, "disableLooping");
    f.send(looping, "pause");
    // forwardPlaybackEndTime
    const auto end = f.block(24);
    f.engine.write64(end, 3);
    f.engine.write32(end + 8, 100);
    f.engine.write32(end + 12, 1);
    const auto d = item();
    f.send(d, "setForwardPlaybackEndTime:", {end});
    alchemy::CallRegs r{};
    r.indirect_result = f.block(24);
    static_cast<void>(f.objc.send(d, f.objc.selector("forwardPlaybackEndTime"), r));
    require(f.engine.read64(r.indirect_result) == 18000 && f.engine.read32(r.indirect_result + 8) == 600000, "forwardPlaybackEndTime 0.03 s");
    f.send(center, "removeObserver:", {observer});
}

void av_player(Fixture& f) {
    const auto directory = std::filesystem::temp_directory_path() / "alchemy_qlab_media_av_player";
    std::filesystem::create_directories(directory);
    const std::string third_party = ALCHEMY_THIRD_PARTY;
    const auto webm = directory / "size-08x08.webm";
    std::filesystem::copy_file(third_party + "/libvpx/test-vectors/vp90-2-02-size-08x08.webm", webm, std::filesystem::copy_options::overwrite_existing);
    const auto mode = f.engine.read64(f.symbol(vt_cf, "_kCFRunLoopDefaultMode"));
    const auto run = [&](double seconds) {
        alchemy::CallRegs r;
        r.x[0] = mode;
        r.set_d(0, seconds);
        r.x[1] = 0;
        static_cast<void>(f.regs(vt_cf, "_CFRunLoopRunInMode", r));
    };
    const auto time_of = [&](Address object, const char* selector) {
        const auto at = f.block(24);
        alchemy::CallRegs r;
        r.indirect_result = at;
        static_cast<void>(f.objc.send(object, f.objc.selector(selector), r));
        const auto timescale = static_cast<std::int32_t>(f.engine.read32(at + 8));
        return timescale > 0 ? static_cast<double>(static_cast<std::int64_t>(f.engine.read64(at))) / timescale : -1.0;
    };
    const auto rate_of = [&](Address player) { return f.objc.send(player, f.objc.selector("rate"), {}).s(0); };

    const auto player = f.send(f.cls("AVPlayer"), "playerWithURL:", {f.cf.file_url(alchemy::guest_path_string(webm), false)});
    const auto item = f.send(player, "currentItem");
    require(item && f.send(item, "status") == 0, "the item loads asynchronously");
    const auto layer = f.send(f.cls("AVPlayerLayer"), "playerLayerWithPlayer:", {player});
    const auto output = f.send(f.send(f.cls("AVPlayerItemVideoOutput"), "alloc"), "initWithPixelBufferAttributes:", {0});
    f.send(item, "addOutput:", {output});
    for (int i = 0; i < 40 && f.send(item, "status") == 0; ++i) run(0.05);
    require(f.send(item, "status") == 1, "the item becomes ready to play");
    const auto size = f.objc.send(item, f.objc.selector("presentationSize"), {});
    require(size.d(0) == 8.0 && size.d(1) == 8.0, "presentationSize 8x8");
    const auto duration = time_of(item, "duration");
    require(duration > 0.1 && duration < 2.0, "the item's duration: " + std::to_string(duration));
    require((f.send(layer, "isReadyForDisplay") & 0xff) == 1 && f.send(layer, "contents") != 0, "the first picture is on the layer");
    const auto first_contents = f.send(layer, "contents");
    const auto when = f.block(24);
    write_time(f, when, 0, 1000);
    require((f.send(output, "hasNewPixelBufferForItemTime:", {when}) & 0xff) == 1, "the output has a picture");
    const auto image = f.send(output, "copyPixelBufferForItemTime:itemTimeForDisplay:", {when, 0});
    require(image && f.call(cv, "_CVPixelBufferGetWidth", {image}) == 8 && (f.send(output, "hasNewPixelBufferForItemTime:", {when}) & 0xff) == 0,
            "copyPixelBufferForItemTime hands it out once");
    f.call(vt_cf, "_CFRelease", {image});
    // Play to the end: the rate returns to 0 at the item's duration, with the last picture shown.
    f.send(player, "play");
    require(rate_of(player) == 1.0f && f.send(player, "timeControlStatus") == 2, "playing");
    for (int i = 0; i < 60 && rate_of(player) != 0.0f; ++i) run(0.05);
    require(rate_of(player) == 0.0f && std::fabs(time_of(player, "currentTime") - duration) < 1e-3, "the player pauses at the item's end");
    // A later picture is the one shown (the output sees a new one, presented after time 0). Not the layer's contents
    // pointer: the first picture's surface is freed once replaced, so a later surface can reuse its address.
    static_cast<void>(first_contents);
    const auto display = f.block(24);
    require(f.send(layer, "contents") != 0 && (f.send(output, "hasNewPixelBufferForItemTime:", {when}) & 0xff) == 1, "a new picture after playing");
    const auto last = f.send(output, "copyPixelBufferForItemTime:itemTimeForDisplay:", {when, display});
    const auto display_scale = static_cast<std::int32_t>(f.engine.read32(display + 8));
    require(last && display_scale > 0 && static_cast<std::int64_t>(f.engine.read64(display)) > 0, "the layer shows later pictures");
    f.call(vt_cf, "_CFRelease", {last});
    // Seeking back.
    write_time(f, when, 100, 1000);
    f.send(player, "seekToTime:", {when});
    require(std::fabs(time_of(player, "currentTime") - 0.1) < 1e-3, "seekToTime:");

    // An MP3 item's audio plays on the output device.
    const auto mp3 = directory / "hecommon.mp3";
    std::filesystem::copy_file(third_party + "/minimp3/vectors/l3-hecommon.bit", mp3, std::filesystem::copy_options::overwrite_existing);
    const auto audio_player = f.send(f.cls("AVPlayer"), "playerWithURL:", {f.cf.file_url(alchemy::guest_path_string(mp3), false)});
    const auto audio_item = f.send(audio_player, "currentItem");
    for (int i = 0; i < 40 && f.send(audio_item, "status") == 0; ++i) run(0.05);
    require(f.send(audio_item, "status") == 1, "the MP3 item is ready");
    f.send(audio_player, "play");
    run(0.05);
    const auto sound = FakeStream::play_until_sound(512);
    require(std::any_of(sound.begin(), sound.end(), [](float v) { return v != 0; }), "the MP3 is heard");
    f.send(audio_player, "pause");
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// The sample-buffer renderers the way Chromium's CoreAnimation layer tree drives them: a 420v
// IOSurface CVPixelBuffer in a CMSampleBufferCreateForImageBuffer buffer marked DisplayImmediately,
// enqueued on an AVSampleBufferDisplayLayer, becomes its BGRA IOSurface contents; on a
// synchronizer, pictures wait for their time and linear PCM plays on the output device; a format
// with no decoder fails the layer.
void sample_buffer_renderers(Fixture& f) {
    const auto surface_lib = std::string_view("/System/Library/Frameworks/IOSurface.framework/Versions/A/IOSurface");
    const auto mode = f.engine.read64(f.symbol(vt_cf, "_kCFRunLoopDefaultMode"));
    const auto run = [&](double seconds) {
        alchemy::CallRegs r;
        r.x[0] = mode;
        r.set_d(0, seconds);
        static_cast<void>(f.regs(vt_cf, "_CFRunLoopRunInMode", r));
    };
    const auto true_value = f.engine.read64(f.symbol(vt_cf, "_kCFBooleanTrue"));
    // A 16x16 picture in `format` with IOSurface backing: 420v of one colour, or BGRA.
    const auto picture = [&](std::uint32_t format) {
        const auto format_key = f.cf.string("PixelFormatType"), surface_key = f.cf.string("IOSurfaceProperties");
        const auto value = f.cf.integer(format), empty = f.cf.dictionary(std::span<const std::pair<std::uint64_t, std::uint64_t>>{});
        const std::array<std::pair<std::uint64_t, std::uint64_t>, 2> entries{{{format_key, value}, {surface_key, empty}}};
        const auto out = f.block(8);
        require(f.status(cv, "_CVPixelBufferCreate", {0, 16, 16, format, f.cf.dictionary(entries), out}) == 0, "a 16x16 IOSurface picture");
        const auto image = f.engine.read64(out);
        f.call(cv, "_CVPixelBufferLockBaseAddress", {image, 0});
        if (format == 0x34323076) {   // '420v': Y' 81, Cb 90, Cr 240 (BT.601 red)
            const auto y = f.call(cv, "_CVPixelBufferGetBaseAddressOfPlane", {image, 0}), y_row = f.call(cv, "_CVPixelBufferGetBytesPerRowOfPlane", {image, 0});
            const auto c = f.call(cv, "_CVPixelBufferGetBaseAddressOfPlane", {image, 1}), c_row = f.call(cv, "_CVPixelBufferGetBytesPerRowOfPlane", {image, 1});
            for (Address row = 0; row < 16; ++row)
                for (Address x = 0; x < 16; ++x) f.memory().write8(y + row * y_row + x, 81);
            for (Address row = 0; row < 8; ++row)
                for (Address x = 0; x < 8; ++x) { f.memory().write8(c + row * c_row + 2 * x, 90); f.memory().write8(c + row * c_row + 2 * x + 1, 240); }
        } else {
            const auto base = f.call(cv, "_CVPixelBufferGetBaseAddress", {image}), row_bytes = f.call(cv, "_CVPixelBufferGetBytesPerRow", {image});
            for (Address row = 0; row < 16; ++row)
                for (Address x = 0; x < 16; ++x) f.engine.write32(base + row * row_bytes + 4 * x, 0xff00ff00);   // green
        }
        f.call(cv, "_CVPixelBufferUnlockBaseAddress", {image, 0});
        return image;
    };
    // A CMSampleBuffer of `image` at `seconds`, optionally marked to display immediately.
    const auto sample_of = [&](Address image, double seconds, bool immediately) {
        const auto format_out = f.block(8), sample_out = f.block(8), timing = f.block(72);
        require(f.status(cm, "_CMVideoFormatDescriptionCreateForImageBuffer", {0, image, format_out}) == 0, "a description of the picture");
        write_time(f, timing, 1, 30);
        write_time(f, timing + 24, static_cast<std::int64_t>(seconds * 1000), 1000);
        write_time(f, timing + 48, 0, 0, 0);
        require(f.status(cm, "_CMSampleBufferCreateForImageBuffer", {0, image, 1, 0, 0, f.engine.read64(format_out), timing, sample_out}) == 0,
                "CMSampleBufferCreateForImageBuffer");
        const auto sample = f.engine.read64(sample_out);
        if (immediately) {
            const auto attachments = f.call(cm, "_CMSampleBufferGetSampleAttachmentsArray", {sample, 1});
            f.call(vt_cf, "_CFDictionarySetValue", {f.cf.element(attachments, 0), f.cf.string("DisplayImmediately"), true_value});
        }
        return sample;
    };
    // The layer's contents as an IOSurface: its first pixel (B, G, R, A in memory).
    const auto first_pixel = [&](Address layer) -> std::uint32_t {
        const auto surface = f.send(layer, "contents");
        if (!surface) return 0;
        f.call(surface_lib, "_IOSurfaceLock", {surface, 1, 0});
        const auto value = f.engine.read32(f.call(surface_lib, "_IOSurfaceGetBaseAddress", {surface}));
        f.call(surface_lib, "_IOSurfaceUnlock", {surface, 1, 0});
        return value;
    };

    // Chromium's path: 420v, display immediately, no synchronizer.
    const auto layer = f.send(f.send(f.cls("AVSampleBufferDisplayLayer"), "alloc"), "init");
    require(layer && f.send(layer, "status") == 0 && (f.send(layer, "isReadyForMoreMediaData") & 0xff) == 1, "a new display layer wants pictures");
    f.send(layer, "enqueueSampleBuffer:", {sample_of(picture(0x34323076), 0, true)});
    const auto red = first_pixel(layer);
    require(f.send(layer, "status") == 1 && (red & 0xff) < 8 && ((red >> 8) & 0xff) < 8 && ((red >> 16) & 0xff) > 240,
            "the 420v picture shows as BGRA red: " + std::to_string(red));
    f.send(layer, "flushAndRemoveImage");
    require(f.send(layer, "contents") == 0, "flushAndRemoveImage clears the layer");

    // On a synchronizer: a picture for 0.2 s waits for the clock; LPCM audio plays from the clock's time.
    const auto sync = f.send(f.send(f.cls("AVSampleBufferRenderSynchronizer"), "alloc"), "init");
    const auto audio = f.send(f.send(f.cls("AVSampleBufferAudioRenderer"), "alloc"), "init");
    f.send(sync, "addRenderer:", {layer});
    f.send(sync, "addRenderer:", {audio});
    require(f.cf.length(f.send(sync, "renderers")) == 2, "two renderers");
    f.send(layer, "enqueueSampleBuffer:", {sample_of(picture(0x42475241), 0.2, false)});
    require(f.send(layer, "contents") == 0, "a picture for later waits while the clock stands");
    // A tenth of a second of 48 kHz stereo float at 0.5.
    const auto asbd = f.block(40);
    f.engine.write64(asbd, std::bit_cast<std::uint64_t>(48000.0));
    f.engine.write32(asbd + 8, 0x6c70636d);
    f.engine.write32(asbd + 12, 1 | 8);
    f.engine.write32(asbd + 16, 8); f.engine.write32(asbd + 20, 1); f.engine.write32(asbd + 24, 8);
    f.engine.write32(asbd + 28, 2); f.engine.write32(asbd + 32, 32);
    const auto format_out = f.block(8);
    require(f.status(cm, "_CMAudioFormatDescriptionCreate", {0, asbd, 0, 0, 0, 0, 0, format_out}) == 0, "an LPCM description");
    const auto block_out = f.block(8);
    alchemy::CallRegs block{};
    block.x[2] = 4800 * 8; block.x[6] = 4800 * 8; block.x[7] = 1;
    block.push_stack(block_out);
    require(f.regs(cm, "_CMBlockBufferCreateWithMemoryBlock", block).x0 == 0, "an audio block");
    const auto pcm = f.block(4800 * 8);
    for (int i = 0; i < 4800 * 2; ++i) f.engine.write32(pcm + 4ULL * i, std::bit_cast<std::uint32_t>(0.5f));
    require(f.status(cm, "_CMBlockBufferReplaceDataBytes", {pcm, f.engine.read64(block_out), 0, 4800 * 8}) == 0, "audio bytes");
    const auto timing = f.block(72), audio_out = f.block(8);
    write_time(f, timing, 1, 48000);
    write_time(f, timing + 24, 0, 48000);
    write_time(f, timing + 48, 0, 0, 0);
    alchemy::CallRegs ready{};
    ready.x[1] = f.engine.read64(block_out); ready.x[2] = f.engine.read64(format_out); ready.x[3] = 4800; ready.x[4] = 1; ready.x[5] = timing;
    ready.push_stack(audio_out);
    require(f.regs(cm, "_CMSampleBufferCreateReady", ready).x0 == 0, "an LPCM sample buffer");
    f.send(audio, "enqueueSampleBuffer:", {f.engine.read64(audio_out)});
    require(f.send(audio, "status") == 1, "the audio renderer takes linear PCM");
    const auto zero = f.block(24);
    write_time(f, zero, 0, 1000);
    alchemy::CallRegs start{};
    start.set_s(0, 1.0f);
    start.x[2] = zero;
    static_cast<void>(f.objc.send(sync, f.objc.selector("setRate:time:"), start));
    const auto sound = FakeStream::play_until_sound(512);
    require(std::any_of(sound.begin(), sound.end(), [](float v) { return std::fabs(v - 0.5f) < 0.05f; }), "the PCM is heard at the synchronizer's rate");
    for (int i = 0; i < 40 && !f.send(layer, "contents"); ++i) run(0.02);
    const auto green = first_pixel(layer);
    require(((green >> 8) & 0xff) == 0xff && (green & 0xff) == 0, "the picture shows at its time: " + std::to_string(green));
    const auto now = f.block(24);
    alchemy::CallRegs get{};
    get.indirect_result = now;
    static_cast<void>(f.objc.send(sync, f.objc.selector("currentTime"), get));
    require(static_cast<double>(static_cast<std::int64_t>(f.engine.read64(now))) / f.engine.read32(now + 8) >= 0.2, "the synchronizer's clock runs");
    alchemy::CallRegs stop{};
    stop.set_s(0, 0.0f);
    static_cast<void>(f.objc.send(sync, f.objc.selector("setRate:"), stop));

    // Samples with no decoder (H.264 here) fail the layer.
    const auto failing = f.send(f.send(f.cls("AVSampleBufferDisplayLayer"), "alloc"), "init");
    const auto avc = f.block(8), avc_block = f.block(8), avc_sample = f.block(8);
    require(f.status(cm, "_CMVideoFormatDescriptionCreate", {0, 0x61766331, 16, 16, 0, avc}) == 0, "an H.264 description");
    alchemy::CallRegs small{};
    small.x[2] = 16; small.x[6] = 16; small.x[7] = 1;
    small.push_stack(avc_block);
    require(f.regs(cm, "_CMBlockBufferCreateWithMemoryBlock", small).x0 == 0, "an H.264 block");
    alchemy::CallRegs make{};
    make.x[1] = f.engine.read64(avc_block); make.x[2] = f.engine.read64(avc); make.x[3] = 1;
    make.push_stack(avc_sample);
    require(f.regs(cm, "_CMSampleBufferCreateReady", make).x0 == 0, "an H.264 sample");
    f.send(failing, "enqueueSampleBuffer:", {f.engine.read64(avc_sample)});
    require(f.send(failing, "status") == 2 && f.send(failing, "error") != 0, "no H.264 decoder: the layer fails with an error");
}

// AVMutableComposition of two WAVE files (A: mono 48 kHz at 0.5; B: stereo 44.1 kHz at -0.25), edited with
// inserts, an empty range and a removal, read through AVAssetReader; AVMutableAudioMix volume ramps.
void compositions(Fixture& f) {
    const auto directory = std::filesystem::temp_directory_path() / "alchemy_qlab_media_composition";
    std::filesystem::create_directories(directory);
    const auto write_wave = [&](const std::filesystem::path& path, std::uint32_t rate, std::uint32_t channels, std::uint32_t frames, std::int16_t value) {
        std::vector<std::uint8_t> file;
        const auto le = [&](std::uint32_t v, int bytes) { for (int i = 0; i < bytes; ++i) file.push_back(static_cast<std::uint8_t>(v >> (8 * i))); };
        const auto tag = [&](const char* text) { file.insert(file.end(), text, text + 4); };
        const auto bytes = frames * channels * 2;
        tag("RIFF"); le(36 + bytes, 4); tag("WAVE");
        tag("fmt "); le(16, 4); le(1, 2); le(channels, 2); le(rate, 4); le(rate * channels * 2, 4); le(channels * 2, 2); le(16, 2);
        tag("data"); le(bytes, 4);
        for (std::uint32_t i = 0; i < frames * channels; ++i) le(static_cast<std::uint16_t>(value), 2);
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    };
    write_wave(directory / "a.wav", 48000, 1, 4800, 16384);
    write_wave(directory / "b.wav", 44100, 2, 4410, -8192);
    const auto open = [&](const char* name) {
        const auto url = f.cf.file_url(alchemy::guest_path_string(directory / name), false);
        const auto asset = f.send(f.cls("AVURLAsset"), "URLAssetWithURL:options:", {url, 0});
        const auto tracks = f.cf.array_values(f.send(asset, "tracksWithMediaType:", {f.cf.string("soun")}));
        require(tracks.size() == 1, std::string("the audio track of ") + name);
        return std::make_pair(url, tracks[0]);
    };
    const auto [url_a, track_a] = open("a.wav");
    const auto [url_b, track_b] = open("b.wav");
    const auto range = f.block(48), at = f.block(24), error = f.block(8), time = f.block(96);
    const auto set_range = [&](std::int64_t start_ms, std::int64_t duration_ms) {
        write_time(f, range, start_ms, 1000);
        write_time(f, range + 24, duration_ms, 1000);
    };
    const auto seconds = [&](Address t) { return static_cast<double>(static_cast<std::int64_t>(f.engine.read64(t))) / f.engine.read32(t + 8); };

    const auto composition = f.send(f.cls("AVMutableComposition"), "composition");
    require(composition != 0, "+[AVMutableComposition composition]");
    const auto track = f.send(composition, "addMutableTrackWithMediaType:preferredTrackID:", {f.cf.string("soun"), 0});
    require(track != 0 && f.send(track, "trackID") == 1, "a composition track with ID 1");
    set_range(0, 100);
    write_time(f, at, 0, 1000);
    require((f.send(track, "insertTimeRange:ofTrack:atTime:error:", {range, track_a, at, error}) & 0xff) == 1, "insert A at 0");
    write_time(f, at, 100, 1000);
    require((f.send(track, "insertTimeRange:ofTrack:atTime:error:", {range, track_b, at, error}) & 0xff) == 1, "insert B at 0.1 s");
    set_range(100, 50);
    static_cast<void>(f.send(composition, "insertEmptyTimeRange:", {range}));
    auto segments = f.cf.array_values(f.send(track, "segments"));
    require(segments.size() == 3 && (f.send(segments[1], "isEmpty") & 0xff) == 1 && (f.send(segments[2], "isEmpty") & 0xff) == 0, "A, empty, B");
    alchemy::CallRegs mapping{};
    mapping.indirect_result = time;
    static_cast<void>(f.objc.send(segments[2], f.objc.selector("timeMapping"), mapping));
    require(seconds(time) == 0 && seconds(time + 24) == 0.1 && seconds(time + 48) == 0.15 && seconds(time + 72) == 0.1, "B's mapping: source [0, 0.1) at 0.15 s");
    // Removing [0.05, 0.1) takes A's second half: A [0, 0.05), empty [0.05, 0.1), B [0.1, 0.2).
    set_range(50, 50);
    static_cast<void>(f.send(composition, "removeTimeRange:", {range}));
    segments = f.cf.array_values(f.send(track, "segments"));
    require(segments.size() == 3, "three segments after the removal");
    static_cast<void>(f.objc.send(segments[0], f.objc.selector("timeMapping"), mapping));
    require(seconds(time + 72) == 0.05 && seconds(time + 24) == 0.05, "A's first half");
    write_time(f, at, 120, 1000);
    const auto segment_b = f.send(track, "segmentForTrackTime:", {at});
    require(segment_b && f.send(segment_b, "sourceURL") == url_b && f.send(segment_b, "sourceTrackID") == 1, "the segment at 0.12 s is B's");
    static_cast<void>(f.objc.send(segment_b, f.objc.selector("timeMapping"), mapping));
    require(seconds(time + 48) == 0.1, "B starts at 0.1 s");
    alchemy::CallRegs none{};
    none.indirect_result = time;
    static_cast<void>(f.objc.send(composition, f.objc.selector("duration"), none));
    require(seconds(time) == 0.2, "the composition lasts 0.2 s");
    require(f.cf.array_values(f.send(composition, "tracksWithMediaType:", {f.cf.string("soun")})).size() == 1, "one audio track");

    // Reads every frame of an output as interleaved floats (channels per frame in `channels`).
    const auto read_all = [&](Address output, std::uint32_t& channels) {
        const auto reader = f.send(f.cls("AVAssetReader"), "assetReaderWithAsset:error:", {composition, error});
        require(reader != 0, "an AVAssetReader of the composition");
        static_cast<void>(f.send(reader, "addOutput:", {output}));
        require((f.send(reader, "startReading") & 0xff) == 1, "startReading");
        std::vector<float> samples;
        while (const auto sample = f.send(output, "copyNextSampleBuffer")) {
            const auto description = f.call(cm, "_CMSampleBufferGetFormatDescription", {sample});
            channels = f.engine.read32(f.call(cm, "_CMAudioFormatDescriptionGetStreamBasicDescription", {description}) + 28);
            const auto list = f.block(64), retained = f.block(8);
            require(f.status(cm, "_CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer", {sample, 0, list, 64, 0, 0, 0, retained}) == 0, "buffer list");
            const auto bytes = f.engine.read32(list + 12);
            const auto data = f.engine.read64(list + 16);
            for (std::uint32_t i = 0; i < bytes / 4; ++i) samples.push_back(std::bit_cast<float>(f.engine.read32(data + 4ULL * i)));
            static_cast<void>(f.call("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "_CFRelease", {f.engine.read64(retained)}));
            static_cast<void>(f.call("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "_CFRelease", {sample}));
        }
        require(f.send(reader, "status") == 2, "the reader completes");
        return samples;
    };
    const auto near = [](float value, float expected) { return std::fabs(value - expected) < 0.02f; };
    std::vector<std::pair<Address, Address>> entries{
        {f.cf.string("AVFormatIDKey"), f.cf.integer(0x6c70636d, 3)}, {f.cf.string("AVLinearPCMBitDepthKey"), f.cf.integer(32, 3)},
        {f.cf.string("AVLinearPCMIsFloatKey"), f.cf.boolean(true)}, {f.cf.string("AVLinearPCMIsNonInterleaved"), f.cf.boolean(false)}};
    std::uint32_t channels = 0;
    auto samples = read_all(f.send(f.cls("AVAssetReaderTrackOutput"), "assetReaderTrackOutputWithTrack:outputSettings:", {track, f.cf.dictionary(entries)}), channels);
    // At A's rate (48 kHz) with B's two channels: 0.2 s is 9600 frames.
    require(channels == 2 && samples.size() == 9600 * 2, "9600 stereo frames: " + std::to_string(samples.size()));
    const auto frame = [&](double t, std::uint32_t c) { return samples[static_cast<std::size_t>(t * 48000) * 2 + c]; };
    require(near(frame(0.025, 0), 0.5f) && near(frame(0.025, 1), 0.5f), "A (mono to both channels)");
    require(frame(0.075, 0) == 0 && frame(0.075, 1) == 0, "the empty segment is silent");
    require(near(frame(0.15, 0), -0.25f) && near(frame(0.15, 1), -0.25f), "B resampled to 48 kHz: " + std::to_string(frame(0.15, 0)));

    // An audio mix: a ramp from 1 to 0 over [0, 0.1), then 0.5 from 0.1 s.
    const auto parameters = f.send(f.cls("AVMutableAudioMixInputParameters"), "audioMixInputParametersWithTrack:", {track});
    require(parameters && f.send(parameters, "trackID") == 1, "input parameters for track 1");
    set_range(0, 100);
    alchemy::CallRegs ramp{};
    ramp.set_s(0, 1.0f);
    ramp.set_s(1, 0.0f);
    ramp.x[2] = range;
    static_cast<void>(f.objc.send(parameters, f.objc.selector("setVolumeRampFromStartVolume:toEndVolume:timeRange:"), ramp));
    write_time(f, at, 100, 1000);
    alchemy::CallRegs point{};
    point.set_s(0, 0.5f);
    point.x[2] = at;
    static_cast<void>(f.objc.send(parameters, f.objc.selector("setVolume:atTime:"), point));
    const auto start_volume = f.block(4), end_volume = f.block(4), ramp_range = f.block(48);
    write_time(f, at, 50, 1000);
    require((f.send(parameters, "getVolumeRampForTime:startVolume:endVolume:timeRange:", {at, start_volume, end_volume, ramp_range}) & 0xff) == 1 &&
                std::bit_cast<float>(f.engine.read32(start_volume)) == 1.0f && std::bit_cast<float>(f.engine.read32(end_volume)) == 0.0f &&
                seconds(ramp_range) == 0 && seconds(ramp_range + 24) == 0.1, "the ramp at 0.05 s");
    write_time(f, at, 150, 1000);
    require((f.send(parameters, "getVolumeRampForTime:startVolume:endVolume:timeRange:", {at, start_volume, end_volume, ramp_range}) & 0xff) == 1 &&
                std::bit_cast<float>(f.engine.read32(start_volume)) == 0.5f && seconds(ramp_range) == 0.1 && (f.engine.read32(ramp_range + 36) & 4) != 0,
            "the point at 0.1 s lasts");
    const auto mix = f.send(f.cls("AVMutableAudioMix"), "audioMix");
    static_cast<void>(f.send(mix, "setInputParameters:", {f.cf.array(std::vector<Address>{parameters})}));
    require(f.cf.array_values(f.send(mix, "inputParameters")).size() == 1, "the mix's parameters");
    const auto mix_output = f.send(f.cls("AVAssetReaderAudioMixOutput"), "assetReaderAudioMixOutputWithAudioTracks:audioSettings:",
                                   {f.cf.array(std::vector<Address>{track}), 0});
    static_cast<void>(f.send(mix_output, "setAudioMix:", {mix}));
    samples = read_all(mix_output, channels);
    require(samples.size() == 9600 * 2, "the mix reads 9600 frames");
    require(near(frame(0.025, 0), 0.375f), "A at 3/4 volume: " + std::to_string(frame(0.025, 0)));
    require(near(frame(0.15, 1), -0.125f), "B at half volume: " + std::to_string(frame(0.15, 1)));

    // The same parameters on a file asset's track (ID 1): A ramped.
    const auto file_reader = f.send(f.cls("AVAssetReader"), "assetReaderWithAsset:error:", {f.send(track_a, "asset"), error});
    const auto file_output = f.send(f.cls("AVAssetReaderAudioMixOutput"), "assetReaderAudioMixOutputWithAudioTracks:audioSettings:",
                                    {f.cf.array(std::vector<Address>{track_a}), 0});
    static_cast<void>(f.send(file_output, "setAudioMix:", {mix}));
    static_cast<void>(f.send(file_reader, "addOutput:", {file_output}));
    require((f.send(file_reader, "startReading") & 0xff) == 1, "a file reader with a mix");
    const auto first = f.send(file_output, "copyNextSampleBuffer");
    const auto list = f.block(64), retained = f.block(8);
    require(first && f.status(cm, "_CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer", {first, 0, list, 64, 0, 0, 0, retained}) == 0, "a mixed file buffer");
    require(near(std::bit_cast<float>(f.engine.read32(f.engine.read64(list + 16))), 0.5f), "the ramp starts at full volume");

    // A whole asset into a new composition: its audio track gets a composition track.
    set_range(0, 100);
    write_time(f, at, 0, 1000);
    const auto whole = f.send(f.cls("AVMutableComposition"), "composition");
    require((f.send(whole, "insertTimeRange:ofAsset:atTime:error:", {range, f.send(track_b, "asset"), at, error}) & 0xff) == 1, "insert B's asset");
    const auto whole_tracks = f.cf.array_values(f.send(whole, "tracks"));
    require(whole_tracks.size() == 1 && f.send(whole, "mutableTrackCompatibleWithTrack:", {track_a}) == whole_tracks[0], "a compatible audio track");
    static_cast<void>(f.objc.send(whole, f.objc.selector("duration"), none));
    require(seconds(time) == 0.1, "B's 0.1 s");

    // An asset with no readable media is refused.
    const auto missing = f.send(f.cls("AVURLAsset"), "URLAssetWithURL:options:", {f.cf.file_url(alchemy::guest_path_string(directory / "missing.wav"), false), 0});
    f.engine.write64(error, 0);
    require((f.send(composition, "insertTimeRange:ofAsset:atTime:error:", {range, missing, at, error}) & 0xff) == 0 && f.engine.read64(error) != 0,
            "no insert from unreadable media");
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

} // namespace

int main() {
    alchemy::gui::completion_kit::media::set_midi_backend_factory([] { return std::make_unique<FakeMidi>(); });
    alchemy::gui::completion_kit::media::set_hal_backend_factory([] { return std::make_unique<FakeHal>(); });
    fake_aac::install();   // AAC and video results do not depend on the host's encoders
    fake_video::install();
    _putenv_s("ALCHEMY_CAMERA", "none");   // the host's cameras stay out (media_capture_tests.cpp fakes them)
    return fill_test::run("qlab media", {
        {"core_media", core_media},
        {"core_video", core_video},
        {"accelerate", accelerate},
        {"vimage", vimage},
        {"core_midi", core_midi},
        {"core_audio", core_audio},
        {"output_units", output_units},
        {"audio_converter", audio_converter},
        {"audio_queue", audio_queue},
        {"openal", openal},
        {"music_sequence", music_sequence},
        {"audio_files", audio_files},
        {"audio_file_writing", audio_file_writing},
        {"compressed_audio", compressed_audio},
        {"alac_files", alac_files},
        {"alac_writing", alac_writing},
        {"aac_encoding", aac_encoding},
        {"aac_decoding", aac_decoding},
        {"audio_file_stream", audio_file_stream},
        {"video_toolbox", video_toolbox},
        {"vt_compression", vt_compression},
        {"vt_compression_host", vt_compression_host},
        {"av_video", av_video},
        {"av_player", av_player},
        {"queue_player", queue_player},
        {"system_sounds", system_sounds},
        {"sample_buffer_renderers", sample_buffer_renderers},
        {"au_listeners", au_listeners},
        {"av_foundation", av_foundation},
        {"compositions", compositions},
        {"avaudio", avaudio},
        {"avaudio_conversion", avaudio_conversion},
        {"avaudio_effects", avaudio_effects},
        {"speech", speech},
        {"sampler_environment", sampler_environment},
        {"sequencer", sequencer},
    });
}
