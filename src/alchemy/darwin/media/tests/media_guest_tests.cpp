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

#include <algorithm>
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
    require(f.status(ca, "_AudioDeviceStop", {device, proc_id}) == 0, "AudioDeviceStop");
    require(f.status(ca, "_AudioDeviceGetCurrentTime", {device, data}) == hal::error::not_running, "stopped");
    require(f.status(ca, "_AudioDeviceDestroyIOProcID", {device, proc_id}) == 0, "IOProc destroyed");
    require(f.status(ca, "_AudioDeviceDestroyIOProcID", {device, proc_id}) == hal::error::illegal_operation, "destroyed twice");
    set(hal::four("dOut"), hal::scope_global);
    require(f.status(ca, "_AudioObjectAddPropertyListener", {1, address, 0x1000, 0}) == 0, "listener");
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
    require(static_cast<std::uint32_t>(f.status(at, "_AudioConverterNew", {aac, out_format, converter_out})) == 0x666D743F, "no AAC decoder");
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
    // Capture: discovery finds nothing; a session runs with an output and no input.
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
    require(f.send(layer, "contents") != first_contents, "the layer shows later pictures");
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

} // namespace

int main() {
    alchemy::gui::completion_kit::media::set_midi_backend_factory([] { return std::make_unique<FakeMidi>(); });
    alchemy::gui::completion_kit::media::set_hal_backend_factory([] { return std::make_unique<FakeHal>(); });
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
        {"video_toolbox", video_toolbox},
        {"av_video", av_video},
        {"av_player", av_player},
        {"sample_buffer_renderers", sample_buffer_renderers},
        {"au_listeners", au_listeners},
        {"av_foundation", av_foundation},
        {"avaudio", avaudio},
    });
}
