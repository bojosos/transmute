// AVFoundation capture and AVAudioEngine's input node through the strict-mode provider fixture,
// over a fake camera backend and a fake HAL with one microphone: no real camera or microphone is
// opened. Device discovery and formats, authorization as the (fake) host decides it, sessions
// delivering NV12 pictures as CMSampleBuffers to a delegate queue (scaled, mirrored, in the pixel
// format asked for), audio data outputs, the engine's input node feeding the mixer or a tap, and
// the lower-level input paths: AUHAL input (input callback and AudioUnitRender on bus 1),
// AudioQueue input queues, and IOProcs on the input device.
#include "alchemy/gui/tests/fill_fixture.hpp"
#include "alchemy/darwin/media/capture_core.hpp"
#include "alchemy/darwin/media/hal_core.hpp"
#include "alchemy/darwin/media/hal_host.hpp"
#include "alchemy/darwin/media/media_support.hpp"
#include "alchemy/darwin/media/media_container.hpp"
#include "fake_aac.hpp"
#include "fake_video.hpp"
#include "alchemy/darwin/media/aac_encoder.hpp"
#include "alchemy/darwin/kit/support/support_kit.hpp"
#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/host/time.hpp"

#include <algorithm>
#include <filesystem>
#include "alchemy/path.hpp"
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using namespace fill_test;
namespace media = alchemy::gui::completion_kit::media;
namespace hal = media::hal;
namespace capture = media::capture;

#define FRAMEWORK(name) "/System/Library/Frameworks/" name ".framework/Versions/A/" name
constexpr std::string_view cm = FRAMEWORK("CoreMedia"), cv = FRAMEWORK("CoreVideo"), av = FRAMEWORK("AVFoundation"), au = FRAMEWORK("AudioUnit"),
    at = FRAMEWORK("AudioToolbox"), ca = FRAMEWORK("CoreAudio");
#undef FRAMEWORK
constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";

constexpr std::uint32_t four(const char (&code)[5]) {
    return (std::uint32_t(std::uint8_t(code[0])) << 24) | (std::uint32_t(std::uint8_t(code[1])) << 16) |
           (std::uint32_t(std::uint8_t(code[2])) << 8) | std::uint32_t(std::uint8_t(code[3]));
}

bool wait_for(const std::function<bool()>& done, double seconds = 3) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// ---- the fake HAL: one output device the test plays, one mono microphone at 48 kHz ----

struct FakeOutput final : hal::OutputStream {
    explicit FakeOutput(hal::Render render) : render(std::move(render)) {}
    ~FakeOutput() override {
        std::lock_guard lock(mutex());
        if (current() == this) current() = nullptr;
    }
    bool start() override { running = true; return true; }
    void stop() override { running = false; }
    hal::Render render;
    std::atomic<bool> running{};
    static std::mutex& mutex() { static std::mutex value; return value; }
    static FakeOutput*& current() { static FakeOutput* value{}; return value; }
    // Takes `frames` stereo frames, paced so the device's I/O thread renders ahead.
    static std::vector<float> play(std::uint32_t frames) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::lock_guard lock(mutex());
        std::vector<float> out(static_cast<std::size_t>(frames) * 2);
        if (current() && current()->running) current()->render(out.data(), frames, 0);
        return out;
    }
};

// Captures 10 ms of 0.25 every 10 ms while running.
struct FakeInput final : hal::InputStream {
    explicit FakeInput(hal::Capture capture) : capture(std::move(capture)) { ++live(); }
    ~FakeInput() override { stop(); --live(); }
    bool start() override {
        if (thread.joinable()) return true;
        running = true;
        thread = std::thread([this] {
            const std::vector<float> samples(480, 0.25f);
            while (running) {
                capture(samples.data(), 480, alchemy::host::time::monotonic_ns());
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        return true;
    }
    void stop() override {
        running = false;
        if (thread.joinable()) thread.join();
    }
    hal::Capture capture;
    std::atomic<bool> running{};
    std::thread thread;
    static std::atomic<int>& live() { static std::atomic<int> value{0}; return value; }
    static std::atomic<bool>& refuse() { static std::atomic<bool> value{false}; return value; }
};

struct FakeHal final : hal::Backend {
    hal::Snapshot snapshot() override {
        hal::Snapshot result;
        result.devices.push_back({"{render}", "Line Out", "", false, 2, 48000, 3, 2});
        result.devices.push_back({"{capture}", "Fake Microphone", "", true, 1, 48000, 4, 4});
        result.default_output = "{render}";
        result.default_input = "{capture}";
        return result;
    }
    std::unique_ptr<hal::OutputStream> open_output(const hal::Device&, hal::Render render) override {
        auto stream = std::make_unique<FakeOutput>(std::move(render));
        std::lock_guard lock(FakeOutput::mutex());
        FakeOutput::current() = stream.get();
        return stream;
    }
    std::unique_ptr<hal::InputStream> open_input(const hal::Device& device, hal::Capture capture) override {
        if (!device.input || FakeInput::refuse()) return nullptr;
        return std::make_unique<FakeInput>(std::move(capture));
    }
};

// ---- the fake camera: 1280x720 and 640x480, pictures whose luma is the column number ----

struct FakeCamera final : capture::VideoStream {
    FakeCamera(std::uint32_t width, std::uint32_t height, capture::Deliver deliver) : width(width), height(height), deliver(std::move(deliver)) {}
    ~FakeCamera() override { stop(); }
    bool start() override {
        running = true;
        thread = std::thread([this] {
            std::vector<std::uint8_t> picture(static_cast<std::size_t>(width) * height * 3 / 2, 0x80);
            for (std::uint32_t y = 0; y < height; ++y)
                for (std::uint32_t x = 0; x < width; ++x) picture[static_cast<std::size_t>(y) * width + x] = static_cast<std::uint8_t>(16 + x % 200);
            while (running) {
                deliver({width, height, alchemy::host::time::monotonic_ns(), picture.data(), picture.size()});
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
        return true;
    }
    void stop() override {
        running = false;
        if (thread.joinable()) thread.join();
    }
    void size(std::uint32_t& w, std::uint32_t& h) const override { w = width; h = height; }
    std::uint32_t width, height;
    capture::Deliver deliver;
    std::atomic<bool> running{};
    std::thread thread;
};

struct FakeCameras final : capture::Backend {
    static std::atomic<capture::Access>& camera() { static std::atomic<capture::Access> value{capture::Access::unknown}; return value; }
    static std::atomic<capture::Access>& microphone() { static std::atomic<capture::Access> value{capture::Access::unknown}; return value; }
    static std::atomic<int>& opened() { static std::atomic<int> value{0}; return value; }
    static std::atomic<std::uint32_t>& opened_width() { static std::atomic<std::uint32_t> value{0}; return value; }
    std::vector<capture::Camera> cameras() override {
        return {{"fake-camera-1", "Fake Camera", 0, {{1280, 720, 1, 30}, {640, 480, 1, 30}}}};
    }
    capture::Access camera_access() override { return camera(); }
    capture::Access microphone_access() override { return microphone(); }
    std::unique_ptr<capture::VideoStream> open(const std::string& id, std::uint32_t width, std::uint32_t, double, capture::Deliver deliver,
                                               capture::OpenError& error) override {
        if (camera() == capture::Access::denied) { error = capture::OpenError::denied; return nullptr; }
        if (id != "fake-camera-1") { error = capture::OpenError::not_found; return nullptr; }
        ++opened();
        opened_width() = width;
        const bool small = width && width <= 640;
        return std::make_unique<FakeCamera>(small ? 640 : 1280, small ? 480 : 720, std::move(deliver));
    }
};

Address media_type(Fixture& f, const char* symbol) { return f.engine.read64(f.symbol(av, symbol)); }

void devices(Fixture& f) {
    const auto device_class = f.cls("AVCaptureDevice");
    const auto video = media_type(f, "_AVMediaTypeVideo"), audio = media_type(f, "_AVMediaTypeAudio");
    const auto cameras = f.cf.array_values(f.send(device_class, "devicesWithMediaType:", {video}));
    const auto microphones = f.cf.array_values(f.send(device_class, "devicesWithMediaType:", {audio}));
    require(cameras.size() == 1 && microphones.size() == 1, "one camera, one microphone");
    const auto camera = cameras.front(), microphone = microphones.front();
    require(f.cf.utf8(f.send(camera, "uniqueID")) == "fake-camera-1" && f.cf.utf8(f.send(camera, "localizedName")) == "Fake Camera", "the camera's names");
    require(f.cf.utf8(f.send(microphone, "uniqueID")) == "{capture}" && f.cf.utf8(f.send(microphone, "localizedName")) == "Fake Microphone", "the microphone's");
    require(f.send(device_class, "defaultDeviceWithMediaType:", {video}) == camera && f.send(device_class, "defaultDeviceWithMediaType:", {audio}) == microphone,
            "default devices");
    require(f.send(device_class, "deviceWithUniqueID:", {f.cf.string("fake-camera-1")}) == camera, "deviceWithUniqueID:");
    require((f.send(camera, "hasMediaType:", {video}) & 0xff) == 1 && (f.send(camera, "hasMediaType:", {audio}) & 0xff) == 0, "hasMediaType:");
    require(f.cf.utf8(f.send(camera, "deviceType")) == "AVCaptureDeviceTypeExternal", "an external camera (no position)");
    // Formats: both sizes, 420v descriptions, 1 to 30 frames per second; the active one nearest 1280x720.
    const auto formats = f.cf.array_values(f.send(camera, "formats"));
    require(formats.size() == 2, "two formats");
    const auto dimensions = [&](Address format) { return f.call(cm, "_CMVideoFormatDescriptionGetDimensions", {f.send(format, "formatDescription")}); };
    require(dimensions(formats[0]) == (1280 | (720ULL << 32)) && dimensions(formats[1]) == (640 | (480ULL << 32)), "the formats' sizes");
    require(f.call(cm, "_CMFormatDescriptionGetMediaSubType", {f.send(formats[1], "formatDescription")}) == four("420v"), "420v");
    require(f.send(camera, "activeFormat") == formats[0], "the active format");
    const auto ranges = f.cf.array_values(f.send(formats[0], "videoSupportedFrameRateRanges"));
    require(ranges.size() == 1 && f.objc.send(ranges[0], f.objc.selector("maxFrameRate"), {}).d(0) == 30.0 &&
            f.objc.send(ranges[0], f.objc.selector("minFrameRate"), {}).d(0) == 1.0, "the frame-rate range");
    // Configuration needs the lock.
    const auto error = f.block(8);
    require((f.send(camera, "lockForConfiguration:", {error}) & 0xff) == 1, "lockForConfiguration:");
    f.send(camera, "setActiveFormat:", {formats[1]});
    f.send(camera, "unlockForConfiguration");
    require(f.send(camera, "activeFormat") == formats[1], "setActiveFormat:");
    // Discovery by device type, media type and position.
    const auto discover = [&](const char* type, Address media, std::int64_t position) {
        const auto types = f.cf.array(std::vector<Address>{f.cf.string(type)});
        const auto session = f.send(f.cls("AVCaptureDeviceDiscoverySession"), "discoverySessionWithDeviceTypes:mediaType:position:",
                                    {types, media, static_cast<Address>(position)});
        return f.cf.array_values(f.send(session, "devices"));
    };
    require(discover("AVCaptureDeviceTypeExternal", video, 0) == std::vector<Address>{camera}, "discovery: external cameras");
    require(discover("AVCaptureDeviceTypeBuiltInWideAngleCamera", video, 0).empty(), "discovery: no built-in camera");
    require(discover("AVCaptureDeviceTypeMicrophone", 0, 0) == std::vector<Address>{microphone}, "discovery: microphones");
    require(discover("AVCaptureDeviceTypeExternal", video, 2).empty(), "discovery: no front camera");
    require(f.send(device_class, "defaultDeviceWithDeviceType:mediaType:position:", {f.cf.string("AVCaptureDeviceTypeBuiltInMicrophone"), audio, 0}) == microphone,
            "defaultDeviceWithDeviceType: the microphone");
}

void authorization(Fixture& f) {
    const auto device_class = f.cls("AVCaptureDevice");
    const auto video = media_type(f, "_AVMediaTypeVideo"), audio = media_type(f, "_AVMediaTypeAudio");
    const auto b = alchemy::gui::security::bridge();
    static std::atomic<int> answers{0}, granted{0};
    const auto handler = b->make_block([](transmute::CpuState& s, transmute::GuestMemory&) {
        granted = static_cast<int>(s.x[1] & 0xff);
        ++answers;
    });
    // The settings say nothing: not determined until a request lets the host decide (the fake opens).
    FakeCameras::camera() = capture::Access::unknown;
    require(f.send(device_class, "authorizationStatusForMediaType:", {video}) == 0, "camera: not determined");
    const auto opened = FakeCameras::opened().load();
    f.send(device_class, "requestAccessForMediaType:completionHandler:", {video, handler});
    require(wait_for([] { return answers == 1; }) && granted == 1, "camera: granted");
    require(FakeCameras::opened() == opened + 1, "the host decided by opening the camera");
    require(f.send(device_class, "authorizationStatusForMediaType:", {video}) == 3, "camera: authorized");
    // The settings deny the microphone: denied, no input, and the request answers NO.
    FakeCameras::microphone() = capture::Access::denied;
    require(f.send(device_class, "authorizationStatusForMediaType:", {audio}) == 2, "microphone: denied");
    f.send(device_class, "requestAccessForMediaType:completionHandler:", {audio, handler});
    require(wait_for([] { return answers == 2; }) && granted == 0, "microphone: not granted");
    const auto microphone = f.send(device_class, "defaultDeviceWithMediaType:", {audio});
    const auto error = f.block(8);
    require(f.send(f.cls("AVCaptureDeviceInput"), "deviceInputWithDevice:error:", {microphone, error}) == 0 &&
                f.call("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "_CFErrorGetCode", {f.engine.read64(error)}) ==
                    static_cast<Address>(-11852),
            "AVErrorApplicationIsNotAuthorizedToUseDevice");
    FakeCameras::microphone() = capture::Access::allowed;
    require(f.send(device_class, "authorizationStatusForMediaType:", {audio}) == 3, "microphone: allowed by the settings");
}

// What the test delegate saw, read on the delegate queue.
struct Seen {
    std::atomic<int> video{0}, audio{0};
    std::atomic<std::uint32_t> width{0}, height{0}, format{0}, first_luma{0}, last_luma{0};
    std::atomic<std::int64_t> pts{0}, frames{0};
    std::atomic<float> sample{0};
    std::atomic<bool> iosurface{false};
};
Seen& seen() { static Seen value; return value; }

void session(Fixture& f) {
    const auto device_class = f.cls("AVCaptureDevice");
    const auto video = media_type(f, "_AVMediaTypeVideo"), audio = media_type(f, "_AVMediaTypeAudio");
    const auto camera = f.send(device_class, "defaultDeviceWithMediaType:", {video});
    const auto microphone = f.send(device_class, "defaultDeviceWithMediaType:", {audio});
    const auto error = f.block(8);
    const auto camera_input = f.send(f.cls("AVCaptureDeviceInput"), "deviceInputWithDevice:error:", {camera, error});
    const auto microphone_input = f.send(f.cls("AVCaptureDeviceInput"), "deviceInputWithDevice:error:", {microphone, error});
    require(camera_input && microphone_input && f.send(camera_input, "device") == camera, "device inputs");
    // The delegate reads each sample buffer on its queue.
    const auto delegate_class = f.objc.register_native_class("AlchemyCaptureTestDelegate", f.cls("NSObject"), 8);
    const auto video_output = f.send(f.send(f.cls("AVCaptureVideoDataOutput"), "alloc"), "init");
    static Address video_output_object{};
    video_output_object = video_output;
    static_cast<void>(f.objc.add_native_method(delegate_class, "captureOutput:didOutputSampleBuffer:fromConnection:", [](transmute::CpuState& s, transmute::GuestMemory&) {
        auto* sp = alchemy::gui::support_kit::support();
        const auto sample = s.x[3];
        auto& m = sp->memory();
        if (s.x[2] == video_output_object) {
            const auto image = sp->call(cm, "_CMSampleBufferGetImageBuffer", {sample});
            seen().width = static_cast<std::uint32_t>(sp->call(cv, "_CVPixelBufferGetWidth", {image}));
            seen().height = static_cast<std::uint32_t>(sp->call(cv, "_CVPixelBufferGetHeight", {image}));
            seen().format = static_cast<std::uint32_t>(sp->call(cv, "_CVPixelBufferGetPixelFormatType", {image}));
            seen().iosurface = sp->call(cv, "_CVPixelBufferGetIOSurface", {image}) != 0;
            static_cast<void>(sp->call(cv, "_CVPixelBufferLockBaseAddress", {image, 1}));
            // BGRA: the blue byte of the first and last pixels of the top row (grey pictures: blue follows luma).
            const auto pixels = sp->call(cv, "_CVPixelBufferGetBaseAddress", {image});
            seen().first_luma = m.read8(pixels);
            seen().last_luma = m.read8(pixels + 4ULL * (seen().width - 1));
            static_cast<void>(sp->call(cv, "_CVPixelBufferUnlockBaseAddress", {image, 1}));
            const auto out = sp->process.allocate_guest(24, 8);
            alchemy::CallRegs r{};
            r.x[0] = sample;
            r.indirect_result = out;
            static_cast<void>(sp->call(cm, "_CMSampleBufferGetPresentationTimeStamp", r));
            seen().pts = static_cast<std::int64_t>(m.read64(out));
            sp->process.release_guest(out);
            ++seen().video;
        } else {
            seen().frames = static_cast<std::int64_t>(sp->call(cm, "_CMSampleBufferGetNumSamples", {sample}));
            const auto block = sp->call(cm, "_CMSampleBufferGetDataBuffer", {sample});
            const auto value = sp->process.allocate_guest(4, 4);
            static_cast<void>(sp->call(cm, "_CMBlockBufferCopyDataBytes", {block, 0, 4, value}));
            seen().sample = std::bit_cast<float>(m.read32(value));
            sp->process.release_guest(value);
            ++seen().audio;
        }
    }, false, "v@:@^{opaqueCMSampleBuffer=}@"));
    const auto delegate = f.send(f.send(delegate_class, "alloc"), "init");
    const auto queue = f.call(libsystem, "_dispatch_queue_create", {f.text("capture test"), 0});
    const auto audio_output = f.send(f.send(f.cls("AVCaptureAudioDataOutput"), "alloc"), "init");
    f.send(video_output, "setSampleBufferDelegate:queue:", {delegate, queue});
    f.send(audio_output, "setSampleBufferDelegate:queue:", {delegate, queue});
    // BGRA at 320x240, mirrored.
    const auto settings = f.cf.dictionary(std::vector<std::pair<Address, Address>>{
        {f.cf.string("PixelFormatType"), f.cf.integer(four("BGRA"), 3)}, {f.cf.string("Width"), f.cf.integer(320, 3)}, {f.cf.string("Height"), f.cf.integer(240, 3)}});
    f.send(video_output, "setVideoSettings:", {settings});
    const auto session = f.send(f.send(f.cls("AVCaptureSession"), "alloc"), "init");
    f.send(session, "beginConfiguration");
    require((f.send(session, "canAddInput:", {camera_input}) & 0xff) == 1, "canAddInput:");
    f.send(session, "addInput:", {camera_input});
    require((f.send(session, "canAddInput:", {camera_input}) & 0xff) == 0, "not twice");
    f.send(session, "addInput:", {microphone_input});
    f.send(session, "addOutput:", {video_output});
    f.send(session, "addOutput:", {audio_output});
    f.send(session, "setSessionPreset:", {f.cf.string("AVCaptureSessionPreset640x480")});
    f.send(session, "commitConfiguration");
    const auto connection = f.send(video_output, "connectionWithMediaType:", {video});
    require(connection && f.cf.array_values(f.send(connection, "inputPorts")).size() == 1, "the video connection");
    f.send(connection, "setAutomaticallyAdjustsVideoMirroring:", {0});
    f.send(connection, "setVideoMirrored:", {1});
    const auto before = alchemy::host::time::monotonic_ns();
    f.send(session, "startRunning");
    require((f.send(session, "isRunning") & 0xff) == 1, "running");
    require(FakeCameras::opened_width() == 640, "the preset's size opened");
    require(wait_for([] { return seen().video >= 2 && seen().audio >= 2; }), "pictures and audio reach the delegate");
    require(seen().width == 320 && seen().height == 240 && seen().format == four("BGRA") && seen().iosurface, "320x240 BGRA, IOSurface-backed");
    // Mirrored: the left edge shows the picture's right edge (luma 16 + 638 % 200 = 54), brighter than column 0's 16.
    require(seen().first_luma > seen().last_luma, "mirrored: " + std::to_string(seen().first_luma.load()) + " / " + std::to_string(seen().last_luma.load()));
    require(seen().pts >= static_cast<std::int64_t>(before), "host-time stamps");
    require(seen().frames == 480 && std::fabs(seen().sample.load() - 0.25f) < 1e-6f, "audio: the microphone's packets");
    f.send(session, "stopRunning");
    require((f.send(session, "isRunning") & 0xff) == 0 && FakeInput::live() == 0, "stopped; the microphone closed");
    const auto after = seen().video.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(seen().video <= after + 1, "no pictures after stopRunning");
    f.send(session, "release");
}

void engine_input(Fixture& f) {
    const auto real = [](const alchemy::CallResult& r) { return r.d(0); };
    const auto engine = f.send(f.send(f.cls("AVAudioEngine"), "alloc"), "init");
    const auto input = f.send(engine, "inputNode");
    const auto format = f.send(input, "outputFormatForBus:", {0});
    require(format && f.send(format, "channelCount") == 1 && real(f.objc.send(format, f.objc.selector("sampleRate"), {})) == 48000, "the input device's format");
    f.send(engine, "connect:to:format:", {input, f.send(engine, "mainMixerNode"), format});
    const auto error = f.block(8);
    require((f.send(engine, "startAndReturnError:", {error}) & 0xff) == 1 && FakeInput::live() == 1, "the engine starts the microphone");
    std::vector<float> heard;
    for (int i = 0; i < 100 && std::none_of(heard.begin(), heard.end(), [](float v) { return std::fabs(v) > 0.1f; }); ++i) heard = FakeOutput::play(256);
    require(std::any_of(heard.begin(), heard.end(), [](float v) { return std::fabs(v) > 0.1f; }), "the microphone plays through the mixer");
    f.send(engine, "stop");
    require(FakeInput::live() == 0, "stop closes the microphone");
    // A tap on the input node alone runs the input.
    const auto b = alchemy::gui::security::bridge();
    static std::atomic<int> tapped{0};
    const auto tap = b->make_block([](transmute::CpuState& s, transmute::GuestMemory&) { if (s.x[1]) ++tapped; });
    f.send(engine, "disconnectNodeOutput:", {input});
    alchemy::CallRegs tap_args{};
    tap_args.x = {0, 0, 0, 1024, 0, tap, 0, 0};
    static_cast<void>(f.objc.send(input, f.objc.selector("installTapOnBus:bufferSize:format:block:"), tap_args));
    require((f.send(engine, "startAndReturnError:", {error}) & 0xff) == 1 && FakeInput::live() == 1, "a tapped input starts the microphone");
    for (int i = 0; i < 100 && tapped == 0; ++i) static_cast<void>(FakeOutput::play(256));
    require(tapped > 0, "the tap hears the microphone");
    f.send(engine, "stop");
    f.send(input, "removeTapOnBus:", {0});
    // The host refuses the microphone: the engine does not start.
    f.send(engine, "connect:to:format:", {input, f.send(engine, "mainMixerNode"), format});
    FakeInput::refuse() = true;
    f.engine.write64(error, 0);
    require((f.send(engine, "startAndReturnError:", {error}) & 0xff) == 0 && f.engine.read64(error) != 0, "refused: an error");
    FakeInput::refuse() = false;
}

// The default input device's ID (kAudioHardwarePropertyDefaultInputDevice).
std::uint32_t default_input(Fixture& f) {
    const auto address = f.block(12), size = f.block(4), value = f.block(4);
    f.engine.write32(address, four("dIn "));
    f.engine.write32(address + 4, four("glob"));
    f.engine.write32(size, 4);
    require(f.status(ca, "_AudioObjectGetPropertyData", {1, address, 0, 0, size, value}) == 0, "the default input device");
    return f.engine.read32(value);
}

// An ASBD: signed 16-bit packed interleaved linear PCM.
void int16_format(Fixture& f, Address at_, double rate, std::uint32_t channels) {
    f.engine.write64(at_, std::bit_cast<std::uint64_t>(rate));
    f.engine.write32(at_ + 8, four("lpcm"));
    f.engine.write32(at_ + 12, 4 | 8);
    f.engine.write32(at_ + 16, 2 * channels);
    f.engine.write32(at_ + 20, 1);
    f.engine.write32(at_ + 24, 2 * channels);
    f.engine.write32(at_ + 28, channels);
    f.engine.write32(at_ + 32, 16);
    f.engine.write32(at_ + 36, 0);
}

struct InputSeen {
    std::atomic<int> calls{0};
    std::atomic<std::uint32_t> frames{0}, bytes{0}, buffers{0}, outputs{99};
    std::atomic<std::int32_t> status{1}, sample{0};
    std::atomic<std::int64_t> sample_time{-1}, previous_time{-1};
    std::atomic<float> level{0};
    Address unit{}, list{}, data{};
};
InputSeen& input_seen() { static InputSeen value; return value; }

void auhal_input(Fixture& f) {
    auto& seen = input_seen();
    const auto description = f.block(20), unit_out = f.block(8), value = f.block(64), size = f.block(4), callback = f.block(16);
    f.engine.write32(description, four("auou"));
    f.engine.write32(description + 4, four("ahal"));
    f.engine.write32(description + 8, four("appl"));
    const auto component = f.call(au, "_AudioComponentFindNext", {0, description});
    require(component && f.status(au, "_AudioComponentInstanceNew", {component, unit_out}) == 0, "AUHAL");
    const auto unit = f.engine.read64(unit_out);
    seen.unit = unit;
    seen.list = f.block(24);
    seen.data = f.block(2 * 4096);
    // Input on, output off, the microphone as the current device.
    f.engine.write32(value, 1);
    require(f.status(au, "_AudioUnitSetProperty", {unit, 2003, 1, 1, value, 4}) == 0, "EnableIO input");
    f.engine.write32(value, 0);
    require(f.status(au, "_AudioUnitSetProperty", {unit, 2003, 2, 0, value, 4}) == 0, "EnableIO output off");
    const auto microphone = default_input(f);
    f.engine.write32(value, microphone);
    require(f.status(au, "_AudioUnitSetProperty", {unit, 2000, 0, 0, value, 4}) == 0, "CurrentDevice: the microphone");
    f.engine.write32(size, 4);
    require(f.status(au, "_AudioUnitGetProperty", {unit, 2006, 1, 1, value, size}) == 0 && f.engine.read32(value) == 1, "HasIO: input");
    // The device side of element 1 is the microphone's format; the client side 16-bit mono at its rate.
    f.engine.write32(size, 40);
    require(f.status(au, "_AudioUnitGetProperty", {unit, 8, 1, 1, value, size}) == 0 && std::bit_cast<double>(f.engine.read64(value)) == 48000.0 &&
                f.engine.read32(value + 28) == 1,
            "the input device's format");
    int16_format(f, value, 44100, 1);
    require(f.status(au, "_AudioUnitSetProperty", {unit, 8, 2, 1, value, 40}) == 0, "client format (44.1 kHz)");
    require(f.status(au, "_AudioUnitInitialize", {unit}) == -10868, "AUHAL does not convert the input's rate");
    int16_format(f, value, 48000, 1);
    require(f.status(au, "_AudioUnitSetProperty", {unit, 8, 2, 1, value, 40}) == 0, "client format (48 kHz)");
    // The input callback pulls bus 1 into its own buffer.
    const auto proc = f.process.install_function("_alchemy_test_auhal_input", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        auto& seen = input_seen();
        auto* sp = alchemy::gui::support_kit::support();
        const auto frames = static_cast<std::uint32_t>(s.x[4]);
        m.write32(seen.list, 1);
        m.write32(seen.list + 8, 1);
        m.write32(seen.list + 12, 2 * 4096);
        m.write64(seen.list + 16, seen.data);
        alchemy::CallRegs r{};
        r.x = {seen.unit, s.x[1], s.x[2], 1, frames, seen.list, 0, 0};
        seen.status = static_cast<std::int32_t>(sp->call(au, "_AudioUnitRender", r).x0);
        seen.frames = frames;
        seen.bytes = m.read32(seen.list + 12);
        seen.sample = static_cast<std::int16_t>(m.read32(seen.data + 2 * (frames / 2)) & 0xffff);
        seen.previous_time = seen.sample_time.load();
        seen.sample_time = static_cast<std::int64_t>(std::bit_cast<double>(m.read64(s.x[2])));
        ++seen.calls;
        s.x[0] = 0;
    }, std::string(au));
    f.engine.write64(callback, proc);
    f.engine.write64(callback + 8, 0);
    require(f.status(au, "_AudioUnitSetProperty", {unit, 2005, 0, 0, callback, 16}) == 0, "the input callback");
    require(f.status(au, "_AudioUnitInitialize", {unit}) == 0 && f.status(au, "_AudioOutputUnitStart", {unit}) == 0, "started");
    require(FakeInput::live() == 1, "the microphone opened");
    require(wait_for([&] { return seen.calls >= 3; }), "the input callback runs");
    require(seen.status == 0 && seen.frames == 512 && seen.bytes == 1024, "AudioUnitRender: 512 frames of 16-bit mono: " + std::to_string(seen.status.load()));
    require(seen.sample == 8192, "the microphone's 0.25 as 16-bit: " + std::to_string(seen.sample.load()));
    require(seen.sample_time - seen.previous_time == 512, "sample times count the device's frames");
    require(f.status(au, "_AudioOutputUnitStop", {unit}) == 0 && FakeInput::live() == 0, "stopped; the microphone closed");
    require(f.status(au, "_AudioComponentInstanceDispose", {unit}) == 0, "disposed");
}

void audio_queue_input(Fixture& f) {
    auto& seen = input_seen();
    seen.calls = 0;
    seen.sample_time = seen.previous_time = -1;
    const auto format = f.block(40), out = f.block(8);
    int16_format(f, format, 16000, 2);   // stereo at 16 kHz: mapped and resampled from the mono 48 kHz microphone
    const auto callback = f.process.install_function("_alchemy_test_queue_input", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        auto& seen = input_seen();
        auto* sp = alchemy::gui::support_kit::support();
        const auto buffer = s.x[2];
        seen.bytes = m.read32(buffer + 16);
        seen.frames = static_cast<std::uint32_t>(s.x[4]);
        const auto data = m.read64(buffer + 8);
        // The middle frame's right channel (the resampler has settled there).
        seen.sample = static_cast<std::int16_t>(m.read32(data + 4ULL * 800 + 2) & 0xffff);
        seen.previous_time = seen.sample_time.load();
        seen.sample_time = static_cast<std::int64_t>(std::bit_cast<double>(m.read64(s.x[3])));
        ++seen.calls;
        if (seen.bytes) static_cast<void>(sp->call(at, "_AudioQueueEnqueueBuffer", {s.x[1], buffer, 0, 0}));
    }, std::string(at));
    alchemy::CallRegs r{};
    r.x = {format, callback, 0, 0, 0, 0, out, 0};
    require(static_cast<std::int32_t>(f.regs(at, "_AudioQueueNewInput", r).x0) == 0, "AudioQueueNewInput");
    const auto queue = f.engine.read64(out);
    for (int i = 0; i < 3; ++i) {
        const auto buffer_out = f.block(8);
        require(f.status(at, "_AudioQueueAllocateBuffer", {queue, 6400, buffer_out}) == 0, "a buffer");
        require(f.status(at, "_AudioQueueEnqueueBuffer", {queue, f.engine.read64(buffer_out), 0, 0}) == 0, "enqueued empty");
    }
    require(f.status(at, "_AudioQueueStart", {queue, 0}) == 0 && FakeInput::live() == 1, "recording");
    require(wait_for([&] { return seen.calls >= 4; }, 5), "full buffers reach the callback");
    require(seen.bytes == 6400 && seen.frames == 1600, "100 ms of 16-bit stereo: " + std::to_string(seen.bytes.load()));
    require(std::abs(seen.sample - 8192) <= 2, "0.25, resampled, in both channels: " + std::to_string(seen.sample.load()));
    require(seen.sample_time - seen.previous_time == 1600, "start times in the queue's frames");
    require(f.status(at, "_AudioQueueStop", {queue, 1}) == 0 && FakeInput::live() == 0, "stopped; the microphone closed");
    require(f.status(at, "_AudioQueueDispose", {queue, 1}) == 0, "disposed");
}

void ioproc_input(Fixture& f) {
    auto& seen = input_seen();
    seen.calls = 0;
    const auto microphone = default_input(f);
    const auto proc = f.process.install_function("_alchemy_test_ioproc_input", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        auto& seen = input_seen();
        // (device, now, inputData, inputTime, outputData, outputTime, clientData)
        seen.buffers = m.read32(s.x[2]);
        seen.bytes = m.read32(s.x[2] + 12);
        seen.level = std::bit_cast<float>(m.read32(m.read64(s.x[2] + 16)));
        seen.outputs = m.read32(s.x[4]);
        ++seen.calls;
        s.x[0] = 0;
    }, std::string(ca));
    const auto id = f.block(8);
    require(f.status(ca, "_AudioDeviceCreateIOProcID", {microphone, proc, 0, id}) == 0, "an IOProc on the microphone");
    require(f.status(ca, "_AudioDeviceStart", {microphone, f.engine.read64(id)}) == 0 && FakeInput::live() == 1, "started");
    require(wait_for([&] { return seen.calls >= 3; }), "the IOProc runs");
    require(seen.buffers == 1 && seen.bytes == 512 * 4 && seen.outputs == 0 && seen.level == 0.25f, "one 512-frame float input buffer, no output");
    require(f.status(ca, "_AudioDeviceStop", {microphone, f.engine.read64(id)}) == 0 && FakeInput::live() == 0, "stopped; the microphone closed");
    require(f.status(ca, "_AudioDeviceDestroyIOProcID", {microphone, f.engine.read64(id)}) == 0, "destroyed");
}

// AVAudioRecorder: 0.5 s of the fake microphone (mono 0.25 at 48 kHz) into a 16-bit stereo 16 kHz WAVE file,
// with metering; AVAudioApplication's record permission.
void recorder(Fixture& f) {
    constexpr std::string_view run_loop = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
    const auto mode = f.engine.read64(f.symbol(run_loop, "_kCFRunLoopDefaultMode"));
    const auto run = [&](double seconds) {
        alchemy::CallRegs r;
        r.x[0] = mode;
        r.set_d(0, seconds);
        static_cast<void>(f.regs(run_loop, "_CFRunLoopRunInMode", r));
    };
    static std::atomic<int> finished{0};
    const auto delegate_class = f.objc.register_native_class("AlchemyRecorderTestDelegate", f.cls("NSObject"), 8);
    static_cast<void>(f.objc.add_native_method(delegate_class, "audioRecorderDidFinishRecording:successfully:",
                                               [](transmute::CpuState& s, transmute::GuestMemory&) { if (s.x[3] & 0xff) ++finished; }, false, "v@:@B"));
    const auto delegate = f.send(f.send(delegate_class, "alloc"), "init");
    const auto path = std::filesystem::temp_directory_path() / "alchemy_recorder_test.wav";
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    const auto url = f.cf.file_url(alchemy::guest_path_string(path), false);
    const auto settings = f.cf.dictionary(std::vector<std::pair<Address, Address>>{
        {f.cf.string("AVFormatIDKey"), f.cf.integer(four("lpcm"), 3)}, {f.cf.string("AVSampleRateKey"), f.cf.real(16000)},
        {f.cf.string("AVNumberOfChannelsKey"), f.cf.integer(2, 3)}, {f.cf.string("AVLinearPCMBitDepthKey"), f.cf.integer(16, 3)},
        {f.cf.string("AVLinearPCMIsFloatKey"), f.cf.boolean(false)}});
    const auto error = f.block(8);
    const auto recorder = f.send(f.send(f.cls("AVAudioRecorder"), "alloc"), "initWithURL:settings:error:", {url, settings, error});
    require(recorder && (f.send(recorder, "prepareToRecord") & 0xff) == 1, "AVAudioRecorder prepared");
    f.send(recorder, "setDelegate:", {delegate});
    f.send(recorder, "setMeteringEnabled:", {1});
    alchemy::CallRegs duration{};
    duration.set_d(0, 0.5);
    require((f.objc.send(recorder, f.objc.selector("recordForDuration:"), duration).x0 & 0xff) == 1 && (f.send(recorder, "isRecording") & 0xff) == 1 &&
                FakeInput::live() == 1,
            "recording");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    f.send(recorder, "updateMeters");
    alchemy::CallRegs channel{};
    channel.x[2] = 0;
    const auto peak = f.objc.send(recorder, f.objc.selector("peakPowerForChannel:"), channel).s(0);
    const auto average = f.objc.send(recorder, f.objc.selector("averagePowerForChannel:"), channel).s(0);
    require(std::fabs(peak + 12.04f) < 1.5f && std::fabs(average + 12.04f) < 0.5f, "meters: 0.25 is -12 dB (the peak allows the resampler's ringing): " + std::to_string(peak) + " / " + std::to_string(average));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (finished == 0 && std::chrono::steady_clock::now() < deadline) run(0.02);
    require(finished == 1 && (f.send(recorder, "isRecording") & 0xff) == 0 && FakeInput::live() == 0, "the duration ends the recording");
    // The file: 0.5 s of 16 kHz stereo at 0.25.
    const auto file = f.send(f.send(f.cls("AVAudioFile"), "alloc"), "initForReading:error:", {url, error});
    require(file && f.send(file, "length") == 8000, "8000 frames recorded: " + std::to_string(file ? f.send(file, "length") : 0));
    const auto processing = f.send(file, "processingFormat");
    require(f.send(processing, "channelCount") == 2, "stereo");
    const auto buffer = f.send(f.send(f.cls("AVAudioPCMBuffer"), "alloc"), "initWithPCMFormat:frameCapacity:", {processing, 8000});
    require((f.send(file, "readIntoBuffer:error:", {buffer, error}) & 0xff) == 1, "read back");
    const auto right = f.engine.read64(f.send(buffer, "floatChannelData") + 8);
    require(std::fabs(std::bit_cast<float>(f.engine.read32(right + 4 * 4000)) - 0.25f) < 0.001f, "the microphone's 0.25 in both channels");
    // AVAudioApplication: the microphone is allowed by the (fake) host settings.
    const auto application = f.send(f.cls("AVAudioApplication"), "sharedInstance");
    require(application && f.send(application, "recordPermission") == 0x67726E74, "record permission granted");
    std::filesystem::remove(path, ignored);
}

// A float32 stereo 48 kHz sample buffer of `frames` frames (left 0.5, right -0.25) at `pts` frames.
Address float_sample_buffer(Fixture& f, std::uint32_t frames, std::int64_t pts) {
    const auto asbd = f.block(40), out = f.block(8), time = f.block(24);
    f.engine.write64(asbd, std::bit_cast<std::uint64_t>(48000.0));
    f.engine.write32(asbd + 8, four("lpcm"));
    f.engine.write32(asbd + 12, 1 | 8);   // float, packed
    f.engine.write32(asbd + 16, 8);
    f.engine.write32(asbd + 20, 1);
    f.engine.write32(asbd + 24, 8);
    f.engine.write32(asbd + 28, 2);
    f.engine.write32(asbd + 32, 32);
    require(f.status(cm, "_CMAudioFormatDescriptionCreate", {0, asbd, 0, 0, 0, 0, 0, out}) == 0, "CMAudioFormatDescriptionCreate");
    const auto format = f.engine.read64(out);
    alchemy::CallRegs create{};
    create.x[2] = 8ULL * frames;
    create.x[6] = 8ULL * frames;
    create.x[7] = 1;
    create.push_stack(out);
    require(static_cast<std::int32_t>(f.regs(cm, "_CMBlockBufferCreateWithMemoryBlock", create).x0) == 0, "a block buffer");
    const auto block = f.engine.read64(out);
    const auto bytes = f.block(8ULL * frames);
    for (std::uint32_t i = 0; i < frames; ++i) {
        f.engine.write32(bytes + 8ULL * i, std::bit_cast<std::uint32_t>(0.5f));
        f.engine.write32(bytes + 8ULL * i + 4, std::bit_cast<std::uint32_t>(-0.25f));
    }
    require(f.status(cm, "_CMBlockBufferReplaceDataBytes", {bytes, block, 0, 8ULL * frames}) == 0, "the samples");
    f.engine.write64(time, static_cast<std::uint64_t>(pts));
    f.engine.write32(time + 8, 48000);
    f.engine.write32(time + 12, 1);
    require(f.status(cm, "_CMAudioSampleBufferCreateReadyWithPacketDescriptions", {0, block, format, frames, time, 0, out}) == 0, "an audio sample buffer");
    return f.engine.read64(out);
}

struct WriterSeen {
    std::atomic<int> finished{0}, calls{0};
    Address input{}, buffers[3]{};
};
WriterSeen& writer_seen() { static WriterSeen value; return value; }

// AVAssetWriter: float sample buffers into a 16-bit WAVE (settings) and into a CAF (nil settings, through
// requestMediaDataWhenReadyOnQueue:); video inputs in audio files and MP3 files are refused.
void asset_writer(Fixture& f) {
    auto& seen = writer_seen();
    const auto b = alchemy::gui::security::bridge();
    const auto error = f.block(8);
    const auto directory = std::filesystem::temp_directory_path();
    const auto wav = directory / "alchemy_writer_test.wav", caf = directory / "alchemy_writer_test.caf";
    std::error_code ignored;
    std::filesystem::remove(wav, ignored);
    std::filesystem::remove(caf, ignored);
    const auto wav_url = f.cf.file_url(alchemy::guest_path_string(wav), false), caf_url = f.cf.file_url(alchemy::guest_path_string(caf), false);
    const auto type = [&](const char* symbol) { return f.engine.read64(f.symbol(av, symbol)); };
    require(f.send(f.cls("AVAssetWriter"), "assetWriterWithURL:fileType:error:", {wav_url, type("_AVFileTypeMPEGLayer3"), error}) == 0 &&
                f.engine.read64(error) != 0,
            "no MP3 writer");
    const auto writer = f.send(f.cls("AVAssetWriter"), "assetWriterWithURL:fileType:error:", {wav_url, type("_AVFileTypeWAVE"), error});
    require(writer && f.send(writer, "status") == 0, "a WAVE writer");
    const auto video = f.send(f.cls("AVAssetWriterInput"), "assetWriterInputWithMediaType:outputSettings:", {f.cf.string("vide"), 0});
    require((f.send(writer, "canAddInput:", {video}) & 0xff) == 0, "no video input (no encoder)");
    const auto settings = f.cf.dictionary(std::vector<std::pair<Address, Address>>{
        {f.cf.string("AVFormatIDKey"), f.cf.integer(four("lpcm"), 3)}, {f.cf.string("AVSampleRateKey"), f.cf.real(48000)},
        {f.cf.string("AVNumberOfChannelsKey"), f.cf.integer(2, 3)}, {f.cf.string("AVLinearPCMBitDepthKey"), f.cf.integer(16, 3)},
        {f.cf.string("AVLinearPCMIsFloatKey"), f.cf.boolean(false)}});
    const auto input = f.send(f.cls("AVAssetWriterInput"), "assetWriterInputWithMediaType:outputSettings:", {f.cf.string("soun"), settings});
    require((f.send(writer, "canAddInput:", {input}) & 0xff) == 1, "canAddInput:");
    f.send(writer, "addInput:", {input});
    require((f.send(writer, "startWriting") & 0xff) == 1 && f.send(writer, "status") == 1, "writing");
    f.send(writer, "startSessionAtSourceTime:", {f.symbol(cm, "_kCMTimeZero")});
    for (int i = 0; i < 3; ++i) {
        require((f.send(input, "isReadyForMoreMediaData") & 0xff) == 1, "ready");
        require((f.send(input, "appendSampleBuffer:", {float_sample_buffer(f, 4800, 4800LL * i)}) & 0xff) == 1, "appended");
    }
    f.send(input, "markAsFinished");
    require((f.send(input, "isReadyForMoreMediaData") & 0xff) == 0, "finished inputs are not ready");
    const auto done = b->make_block([](transmute::CpuState&, transmute::GuestMemory&) { ++writer_seen().finished; });
    f.send(writer, "finishWritingWithCompletionHandler:", {done});
    require(wait_for([&] { return seen.finished == 1; }) && f.send(writer, "status") == 2, "completed");
    const auto read = [&](Address url, std::int64_t expected) {
        const auto file = f.send(f.send(f.cls("AVAudioFile"), "alloc"), "initForReading:error:", {url, error});
        require(file && static_cast<std::int64_t>(f.send(file, "length")) == expected, "frames written: " + std::to_string(file ? f.send(file, "length") : 0));
        const auto processing = f.send(file, "processingFormat");
        const auto buffer = f.send(f.send(f.cls("AVAudioPCMBuffer"), "alloc"), "initWithPCMFormat:frameCapacity:", {processing, static_cast<Address>(expected)});
        require((f.send(file, "readIntoBuffer:error:", {buffer, error}) & 0xff) == 1, "read back");
        const auto channels = f.send(buffer, "floatChannelData");
        const auto left = std::bit_cast<float>(f.engine.read32(f.engine.read64(channels) + 400)), right = std::bit_cast<float>(f.engine.read32(f.engine.read64(channels + 8) + 400));
        require(std::fabs(left - 0.5f) < 0.001f && std::fabs(right + 0.25f) < 0.001f, "the samples: " + std::to_string(left) + " " + std::to_string(right));
    };
    read(wav_url, 14400);
    // CAF in the buffers' own format (nil settings), fed through requestMediaDataWhenReadyOnQueue:.
    const auto caf_writer = f.send(f.cls("AVAssetWriter"), "assetWriterWithURL:fileType:error:", {caf_url, type("_AVFileTypeCoreAudioFormat"), error});
    const auto passthrough = f.send(f.cls("AVAssetWriterInput"), "assetWriterInputWithMediaType:outputSettings:", {f.cf.string("soun"), 0});
    f.send(caf_writer, "addInput:", {passthrough});
    require((f.send(caf_writer, "startWriting") & 0xff) == 1, "writing the CAF");
    f.send(caf_writer, "startSessionAtSourceTime:", {f.symbol(cm, "_kCMTimeZero")});
    seen.input = passthrough;
    for (int i = 0; i < 3; ++i) seen.buffers[i] = float_sample_buffer(f, 2400, 2400LL * i);
    const auto feed = b->make_block([](transmute::CpuState&, transmute::GuestMemory&) {
        auto& seen = writer_seen();
        auto* sp = alchemy::gui::support_kit::support();
        const auto call = seen.calls++;
        if (call < 3) static_cast<void>(sp->send_id(seen.input, "appendSampleBuffer:", {seen.buffers[call]}));
        else static_cast<void>(sp->send(seen.input, "markAsFinished"));
    });
    const auto queue = f.call(libsystem, "_dispatch_queue_create", {f.text("writer test"), 0});
    f.send(passthrough, "requestMediaDataWhenReadyOnQueue:usingBlock:", {queue, feed});
    require(wait_for([&] { return seen.calls >= 4; }), "the block ran until it finished the input: " + std::to_string(seen.calls.load()));
    require((f.send(caf_writer, "finishWriting") & 0xff) == 1, "finishWriting");
    read(caf_url, 7200);
    // Apple Lossless into an M4A file: settings other than linear PCM and ALAC are refused.
    const auto m4a = directory / "alchemy_writer_test.m4a";
    std::filesystem::remove(m4a, ignored);
    const auto m4a_url = f.cf.file_url(alchemy::guest_path_string(m4a), false);
    const auto alac_settings = f.cf.dictionary(std::vector<std::pair<Address, Address>>{
        {f.cf.string("AVFormatIDKey"), f.cf.integer(four("alac"), 3)}, {f.cf.string("AVSampleRateKey"), f.cf.real(48000)},
        {f.cf.string("AVNumberOfChannelsKey"), f.cf.integer(2, 3)}, {f.cf.string("AVEncoderBitDepthHintKey"), f.cf.integer(16, 3)}});
    const auto aac_settings = f.cf.dictionary(std::vector<std::pair<Address, Address>>{
        {f.cf.string("AVFormatIDKey"), f.cf.integer(four("aac "), 3)}, {f.cf.string("AVSampleRateKey"), f.cf.real(48000)},
        {f.cf.string("AVNumberOfChannelsKey"), f.cf.integer(2, 3)}});
    const auto m4a_writer = f.send(f.cls("AVAssetWriter"), "assetWriterWithURL:fileType:error:", {m4a_url, type("_AVFileTypeAppleM4A"), error});
    require(m4a_writer != 0, "an M4A writer");
    fake_aac::install(true);
    require((f.send(m4a_writer, "canApplyOutputSettings:forMediaType:", {alac_settings, f.cf.string("soun")}) & 0xff) == 1 &&
                (f.send(m4a_writer, "canApplyOutputSettings:forMediaType:", {aac_settings, f.cf.string("soun")}) & 0xff) == 0,
            "Apple Lossless settings apply; AAC needs an encoder");
    fake_aac::install();
    require((f.send(m4a_writer, "canApplyOutputSettings:forMediaType:", {aac_settings, f.cf.string("soun")}) & 0xff) == 1, "AAC applies with an encoder");
    const auto pcm_input = f.send(f.cls("AVAssetWriterInput"), "assetWriterInputWithMediaType:outputSettings:", {f.cf.string("soun"), settings});
    require((f.send(m4a_writer, "canAddInput:", {pcm_input}) & 0xff) == 0, "M4A files do not hold linear PCM");
    const auto alac_input = f.send(f.cls("AVAssetWriterInput"), "assetWriterInputWithMediaType:outputSettings:", {f.cf.string("soun"), alac_settings});
    require((f.send(m4a_writer, "canAddInput:", {alac_input}) & 0xff) == 1, "an Apple Lossless input");
    f.send(m4a_writer, "addInput:", {alac_input});
    require((f.send(m4a_writer, "startWriting") & 0xff) == 1, "writing the M4A");
    f.send(m4a_writer, "startSessionAtSourceTime:", {f.symbol(cm, "_kCMTimeZero")});
    for (int i = 0; i < 3; ++i) require((f.send(alac_input, "appendSampleBuffer:", {float_sample_buffer(f, 4800, 4800LL * i)}) & 0xff) == 1, "appended");
    f.send(alac_input, "markAsFinished");
    require((f.send(m4a_writer, "finishWriting") & 0xff) == 1, "finishWriting the M4A");
    read(m4a_url, 14400);
    // AAC into another M4A: its packets and the encoder's delay as the priming.
    const auto aac_file = directory / "alchemy_writer_test_aac.m4a";
    std::filesystem::remove(aac_file, ignored);
    const auto aac_writer = f.send(f.cls("AVAssetWriter"), "assetWriterWithURL:fileType:error:",
                                   {f.cf.file_url(alchemy::guest_path_string(aac_file), false), type("_AVFileTypeAppleM4A"), error});
    const auto aac_input = f.send(f.cls("AVAssetWriterInput"), "assetWriterInputWithMediaType:outputSettings:", {f.cf.string("soun"), aac_settings});
    require((f.send(aac_writer, "canAddInput:", {aac_input}) & 0xff) == 1, "an AAC input");
    f.send(aac_writer, "addInput:", {aac_input});
    require((f.send(aac_writer, "startWriting") & 0xff) == 1, "writing the AAC M4A");
    f.send(aac_writer, "startSessionAtSourceTime:", {f.symbol(cm, "_kCMTimeZero")});
    for (int i = 0; i < 3; ++i) require((f.send(aac_input, "appendSampleBuffer:", {float_sample_buffer(f, 4800, 4800LL * i)}) & 0xff) == 1, "appended");
    f.send(aac_input, "markAsFinished");
    require((f.send(aac_writer, "finishWriting") & 0xff) == 1, "finishWriting the AAC M4A");
    {
        std::ifstream in(aac_file, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
        namespace mc = alchemy::gui::completion_kit::media::container;
        mc::Movie movie;
        std::string why;
        const mc::ReadAt reader = [&bytes](std::uint64_t at_, std::span<std::uint8_t> o) -> std::size_t {
            if (at_ >= bytes.size()) return 0;
            const auto n = std::min<std::size_t>(o.size(), bytes.size() - static_cast<std::size_t>(at_));
            std::memcpy(o.data(), bytes.data() + at_, n);
            return n;
        };
        require(mc::parse(reader, bytes.size(), movie, why) && movie.tracks.size() == 1 && movie.tracks[0].codec == mc::fourcc("mp4a") &&
                    movie.tracks[0].priming == fake_aac::priming && movie.tracks[0].samples.size() == (fake_aac::priming + 14400 + 1023) / 1024,
                "the AAC M4A: " + why);
    }
    std::filesystem::remove(aac_file, ignored);
    std::filesystem::remove(wav, ignored);
    std::filesystem::remove(caf, ignored);
    std::filesystem::remove(m4a, ignored);
}

std::vector<Address>& passthrough_samples() { static std::vector<Address> value; return value; }
Fixture* movie_fixture = nullptr;

// The file's tracks, read back by the container parser.
alchemy::gui::completion_kit::media::container::Movie read_movie(const std::filesystem::path& file, std::vector<std::uint8_t>& bytes) {
    namespace mc = alchemy::gui::completion_kit::media::container;
    std::ifstream in(file, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in), {});
    mc::Movie movie;
    std::string why;
    const mc::ReadAt reader = [&bytes](std::uint64_t at, std::span<std::uint8_t> o) -> std::size_t {
        if (at >= bytes.size()) return 0;
        const auto n = std::min<std::size_t>(o.size(), bytes.size() - static_cast<std::size_t>(at));
        std::memcpy(o.data(), bytes.data() + at, n);
        return n;
    };
    require(mc::parse(reader, bytes.size(), movie, why), "the movie parses: " + why);
    return movie;
}

// AVAssetWriter movies over the fake encoders (fake_video.hpp, fake_aac.hpp): a QuickTime movie of H.264 frames from an
// AVAssetWriterInputPixelBufferAdaptor (reordered, every tenth a keyframe, a quarter turn) and AAC from float buffers,
// optimized for network use; an MPEG-4 file of the compressed samples a VTCompressionSession made, as they are; and the
// refusals (no encoder, video in an audio file, uncompressed frames without settings).
void asset_writer_movie(Fixture& f) {
    namespace mc = alchemy::gui::completion_kit::media::container;
    constexpr std::string_view vt = "/System/Library/Frameworks/VideoToolbox.framework/Versions/A/VideoToolbox";
    fake_video::install();
    fake_aac::install();
    movie_fixture = &f;
    const auto error = f.block(8);
    const auto directory = std::filesystem::temp_directory_path();
    const auto mov = directory / "alchemy_writer_test.mov", mp4 = directory / "alchemy_writer_test.mp4";
    std::error_code ignored;
    std::filesystem::remove(mov, ignored);
    std::filesystem::remove(mp4, ignored);
    const auto type = [&](const char* symbol) { return f.engine.read64(f.symbol(av, symbol)); };
    const auto key = [&](const char* symbol) { return f.engine.read64(f.symbol(av, symbol)); };
    const auto compression = f.cf.dictionary(std::vector<std::pair<Address, Address>>{
        {key("_AVVideoAverageBitRateKey"), f.cf.integer(500000, 3)}, {key("_AVVideoMaxKeyFrameIntervalKey"), f.cf.integer(10, 3)},
        {key("_AVVideoAllowFrameReorderingKey"), f.cf.boolean(true)}, {key("_AVVideoProfileLevelKey"), key("_AVVideoProfileLevelH264HighAutoLevel")}});
    const auto video_settings = f.cf.dictionary(std::vector<std::pair<Address, Address>>{
        {key("_AVVideoCodecKey"), key("_AVVideoCodecTypeH264")}, {key("_AVVideoWidthKey"), f.cf.integer(64, 3)},
        {key("_AVVideoHeightKey"), f.cf.integer(48, 3)}, {key("_AVVideoCompressionPropertiesKey"), compression}});
    const auto aac_settings = f.cf.dictionary(std::vector<std::pair<Address, Address>>{
        {f.cf.string("AVFormatIDKey"), f.cf.integer(four("aac "), 3)}, {f.cf.string("AVSampleRateKey"), f.cf.real(48000)},
        {f.cf.string("AVNumberOfChannelsKey"), f.cf.integer(2, 3)}});
    // Refusals: video in an audio file, a codec without an encoder, no encoders at all.
    const auto wav = f.send(f.cls("AVAssetWriter"), "assetWriterWithURL:fileType:error:",
                            {f.cf.file_url(alchemy::guest_path_string(directory / "alchemy_writer_refused.wav"), false), type("_AVFileTypeWAVE"), error});
    require((f.send(wav, "canApplyOutputSettings:forMediaType:", {video_settings, f.cf.string("vide")}) & 0xff) == 0, "no video in a WAVE file");
    const auto writer = f.send(f.cls("AVAssetWriter"), "assetWriterWithURL:fileType:error:",
                               {f.cf.file_url(alchemy::guest_path_string(mov), false), type("_AVFileTypeQuickTimeMovie"), error});
    require(writer && f.send(writer, "status") == 0, "a QuickTime movie writer");
    const auto jpeg = f.cf.dictionary(std::vector<std::pair<Address, Address>>{
        {key("_AVVideoCodecKey"), key("_AVVideoCodecTypeJPEG")}, {key("_AVVideoWidthKey"), f.cf.integer(64, 3)}, {key("_AVVideoHeightKey"), f.cf.integer(48, 3)}});
    require((f.send(writer, "canApplyOutputSettings:forMediaType:", {jpeg, f.cf.string("vide")}) & 0xff) == 0, "no JPEG encoder");
    fake_video::install(true);
    require((f.send(writer, "canApplyOutputSettings:forMediaType:", {video_settings, f.cf.string("vide")}) & 0xff) == 0, "no H.264 encoder: refused");
    fake_video::install();
    require((f.send(writer, "canApplyOutputSettings:forMediaType:", {video_settings, f.cf.string("vide")}) & 0xff) == 1, "H.264 settings apply");
    // The QuickTime movie.
    const auto video = f.send(f.cls("AVAssetWriterInput"), "assetWriterInputWithMediaType:outputSettings:", {f.cf.string("vide"), video_settings});
    const auto turn = f.block(48);
    for (const auto [i, v] : std::initializer_list<std::pair<int, double>>{{0, 0}, {1, 1}, {2, -1}, {3, 0}, {4, 48}, {5, 0}})
        f.engine.write64(turn + 8 * i, std::bit_cast<std::uint64_t>(v));
    f.send(video, "setTransform:", {turn});
    const auto adaptor = f.send(f.cls("AVAssetWriterInputPixelBufferAdaptor"), "assetWriterInputPixelBufferAdaptorWithAssetWriterInput:sourcePixelBufferAttributes:", {video, 0});
    require(adaptor && f.send(adaptor, "assetWriterInput") == video && f.send(adaptor, "pixelBufferPool") == 0, "an adaptor, no pool before writing");
    const auto audio = f.send(f.cls("AVAssetWriterInput"), "assetWriterInputWithMediaType:outputSettings:", {f.cf.string("soun"), aac_settings});
    require((f.send(writer, "canAddInput:", {video}) & 0xff) == 1 && (f.send(writer, "canAddInput:", {audio}) & 0xff) == 1, "both inputs fit");
    f.send(writer, "addInput:", {video});
    f.send(writer, "addInput:", {audio});
    f.send(writer, "setShouldOptimizeForNetworkUse:", {1});
    require((f.send(writer, "startWriting") & 0xff) == 1 && f.send(writer, "status") == 1, "writing the movie");
    const auto pool = f.send(adaptor, "pixelBufferPool");
    require(pool != 0, "the adaptor's pool");
    f.send(writer, "startSessionAtSourceTime:", {f.symbol(cm, "_kCMTimeZero")});
    const auto buffer_out = f.block(8), time = f.block(24);
    for (int i = 0; i < 20; ++i) {
        require(f.status(cv, "_CVPixelBufferPoolCreatePixelBuffer", {0, pool, buffer_out}) == 0, "a pixel buffer");
        f.engine.write64(time, static_cast<std::uint64_t>(i * 20));
        f.engine.write32(time + 8, 600);
        f.engine.write32(time + 12, 1);
        f.engine.write64(time + 16, 0);
        require((f.send(adaptor, "appendPixelBuffer:withPresentationTime:", {f.engine.read64(buffer_out), time}) & 0xff) == 1, "frame " + std::to_string(i));
        if (i < 7) require((f.send(audio, "appendSampleBuffer:", {float_sample_buffer(f, 4800, 4800LL * i)}) & 0xff) == 1, "audio " + std::to_string(i));
    }
    require(fake_video::last.frame_reordering && fake_video::last.bit_rate == 500000 && fake_video::last.keyframe_interval == 10 &&
                fake_video::last.profile == fake_video::enc::Profile::h264_high,
            "the compression properties reach the encoder (opened at the first frame)");
    f.send(video, "markAsFinished");
    f.send(audio, "markAsFinished");
    require((f.send(writer, "finishWriting") & 0xff) == 1 && f.send(writer, "status") == 2, "the movie completes");
    {
        std::vector<std::uint8_t> bytes;
        const auto movie = read_movie(mov, bytes);
        require(movie.brand == mc::fourcc("qt  ") && movie.tracks.size() == 2, "a QuickTime movie of two tracks");
        const auto& v = movie.tracks[0];
        const auto& a = movie.tracks[1];
        require(v.kind == mc::Track::Kind::video && v.codec == mc::fourcc("avc1") && v.width == 64 && v.height == 48 && !v.config.empty() &&
                    v.samples.size() == 20 && v.samples[0].keyframe && v.samples[10].keyframe == true,
                "the H.264 track: " + std::to_string(v.samples.size()) + " frames");
        bool reordered = false;
        for (const auto& s : v.samples) reordered = reordered || s.pts != s.dts;
        require(reordered, "reordered frames keep their presentation times");
        require(a.kind == mc::Track::Kind::audio && a.codec == mc::fourcc("mp4a") && a.priming == fake_aac::priming &&
                    a.samples.size() == (fake_aac::priming + 33600 + 1023) / 1024,
                "the AAC track: priming " + std::to_string(a.priming) + ", " + std::to_string(a.samples.size()) + " packets");
        // moov ahead of mdat (optimized for network use).
        const auto at = [&](const char* box) {
            const auto found = std::search(bytes.begin(), bytes.end(), box, box + 4);
            return found == bytes.end() ? bytes.size() : static_cast<std::size_t>(found - bytes.begin());
        };
        require(at("moov") < at("mdat"), "the moov comes first");
    }
    // MPEG-4 of compressed samples as they are: a VTCompressionSession's output through a passthrough input.
    const auto handler_fn = f.process.install_function("_alchemy_test_writer_vt_handled", [](transmute::CpuState& s, transmute::GuestMemory&) {
        // block(OSStatus, VTEncodeInfoFlags, CMSampleBufferRef)
        if (s.x[3] && !s.x[1]) {
            static_cast<void>(movie_fixture->call("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "_CFRetain", {s.x[3]}));
            passthrough_samples().push_back(s.x[3]);
        }
    }, "/usr/lib/alchemy_test_io.dylib");
    const auto handler = f.block(32), descriptor = f.block(16);
    f.engine.write64(descriptor + 8, 32);
    f.engine.write64(handler, f.process.resolve("/usr/lib/libSystem.B.dylib", "__NSConcreteGlobalBlock")->address);
    f.engine.write32(handler + 8, 1U << 28);   // BLOCK_IS_GLOBAL
    f.engine.write64(handler + 16, handler_fn);
    f.engine.write64(handler + 24, descriptor);
    const auto session_out = f.block(8);
    alchemy::CallRegs create{};
    create.x[1] = 64;
    create.x[2] = 48;
    create.x[3] = four("avc1");
    create.push_stack(0);
    create.push_stack(session_out);
    require(static_cast<std::int32_t>(f.regs(vt, "_VTCompressionSessionCreate", create).x0) == 0, "a compression session");
    const auto session = f.engine.read64(session_out);
    const auto session_pool = f.call(vt, "_VTCompressionSessionGetPixelBufferPool", {session});
    const auto duration = f.block(24);
    f.engine.write64(duration, 20);
    f.engine.write32(duration + 8, 600);
    f.engine.write32(duration + 12, 1);
    for (int i = 0; i < 6; ++i) {
        require(f.status(cv, "_CVPixelBufferPoolCreatePixelBuffer", {0, session_pool, buffer_out}) == 0, "a pixel buffer");
        f.engine.write64(time, static_cast<std::uint64_t>(i * 20));
        f.engine.write32(time + 8, 600);
        f.engine.write32(time + 12, 1);
        require(f.status(vt, "_VTCompressionSessionEncodeFrameWithOutputHandler", {session, f.engine.read64(buffer_out), time, duration, 0, 0, handler}) == 0, "encoded");
    }
    f.engine.write32(time + 12, 0);
    require(f.status(vt, "_VTCompressionSessionCompleteFrames", {session, time}) == 0, "CompleteFrames");
    require(passthrough_samples().size() == 6, "six compressed samples");
    const auto mp4_writer = f.send(f.cls("AVAssetWriter"), "assetWriterWithURL:fileType:error:",
                                   {f.cf.file_url(alchemy::guest_path_string(mp4), false), type("_AVFileTypeMPEG4"), error});
    const auto passthrough = f.send(f.cls("AVAssetWriterInput"), "assetWriterInputWithMediaType:outputSettings:", {f.cf.string("vide"), 0});
    require((f.send(mp4_writer, "canAddInput:", {passthrough}) & 0xff) == 1, "a passthrough video input");
    f.send(mp4_writer, "addInput:", {passthrough});
    require((f.send(mp4_writer, "startWriting") & 0xff) == 1, "writing the MPEG-4 file");
    f.send(mp4_writer, "startSessionAtSourceTime:", {f.symbol(cm, "_kCMTimeZero")});
    for (const auto sample : passthrough_samples()) require((f.send(passthrough, "appendSampleBuffer:", {sample}) & 0xff) == 1, "a compressed sample");
    f.send(passthrough, "markAsFinished");
    require((f.send(mp4_writer, "finishWriting") & 0xff) == 1, "the MPEG-4 file completes");
    {
        std::vector<std::uint8_t> bytes;
        const auto movie = read_movie(mp4, bytes);
        require(movie.brand == mc::fourcc("mp42") && movie.tracks.size() == 1 && movie.tracks[0].codec == mc::fourcc("avc1") &&
                    movie.tracks[0].samples.size() == 6 && movie.tracks[0].samples[0].keyframe && !movie.tracks[0].config.empty(),
                "the passthrough H.264 track");
    }
    // Uncompressed frames need settings.
    const auto refused = f.send(f.cls("AVAssetWriter"), "assetWriterWithURL:fileType:error:",
                                {f.cf.file_url(alchemy::guest_path_string(directory / "alchemy_writer_refused.mp4"), false), type("_AVFileTypeMPEG4"), error});
    const auto raw = f.send(f.cls("AVAssetWriterInput"), "assetWriterInputWithMediaType:outputSettings:", {f.cf.string("vide"), 0});
    f.send(refused, "addInput:", {raw});
    f.send(refused, "startWriting");
    f.send(refused, "startSessionAtSourceTime:", {f.symbol(cm, "_kCMTimeZero")});
    const auto adaptor_less = f.send(f.cls("AVAssetWriterInputPixelBufferAdaptor"), "assetWriterInputPixelBufferAdaptorWithAssetWriterInput:sourcePixelBufferAttributes:",
                                     {f.send(f.cls("AVAssetWriterInput"), "assetWriterInputWithMediaType:outputSettings:", {f.cf.string("vide"), 0}), 0});
    require(adaptor_less && f.send(adaptor_less, "pixelBufferPool") == 0, "no pool without settings");
    f.send(refused, "cancelWriting");
    require(f.send(refused, "status") == 4, "cancelled");
    for (const auto sample : passthrough_samples()) f.call("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "_CFRelease", {sample});
    passthrough_samples().clear();
    f.call(vt, "_VTCompressionSessionInvalidate", {session});
    // The host's own encoders (skipped where there is none): ten frames and a fifth of a second of AAC into a movie.
    alchemy::gui::completion_kit::media::video_encoding::set_backend({});
    alchemy::gui::completion_kit::media::aac::set_backend({});
    const auto host_encoders = alchemy::gui::completion_kit::media::video_encoding::encoders();
    const bool host_h264 = std::any_of(host_encoders.begin(), host_encoders.end(), [](const auto& e) {
        return e.codec == alchemy::gui::completion_kit::media::video_encoding::Codec::h264;
    });
    if (!host_h264 || !alchemy::gui::completion_kit::media::aac::available()) {
        std::cout << "[media capture] no host H.264 or AAC encoder: the real movie is skipped" << std::endl;
    } else {
        const auto real = f.send(f.cls("AVAssetWriter"), "assetWriterWithURL:fileType:error:",
                                 {f.cf.file_url(alchemy::guest_path_string(mov), false), type("_AVFileTypeQuickTimeMovie"), error});
        const auto real_video = f.send(f.cls("AVAssetWriterInput"), "assetWriterInputWithMediaType:outputSettings:", {f.cf.string("vide"), video_settings});
        const auto real_adaptor = f.send(f.cls("AVAssetWriterInputPixelBufferAdaptor"), "assetWriterInputPixelBufferAdaptorWithAssetWriterInput:sourcePixelBufferAttributes:",
                                         {real_video, 0});
        const auto real_audio = f.send(f.cls("AVAssetWriterInput"), "assetWriterInputWithMediaType:outputSettings:", {f.cf.string("soun"), aac_settings});
        f.send(real, "addInput:", {real_video});
        f.send(real, "addInput:", {real_audio});
        require((f.send(real, "startWriting") & 0xff) == 1, "writing with the host's encoders");
        f.send(real, "startSessionAtSourceTime:", {f.symbol(cm, "_kCMTimeZero")});
        const auto real_pool = f.send(real_adaptor, "pixelBufferPool");
        for (int i = 0; i < 10; ++i) {
            require(f.status(cv, "_CVPixelBufferPoolCreatePixelBuffer", {0, real_pool, buffer_out}) == 0, "a pixel buffer");
            f.engine.write64(time, static_cast<std::uint64_t>(i * 20));
            f.engine.write32(time + 8, 600);
            f.engine.write32(time + 12, 1);
            require((f.send(real_adaptor, "appendPixelBuffer:withPresentationTime:", {f.engine.read64(buffer_out), time}) & 0xff) == 1, "a real frame");
            if (i < 2) require((f.send(real_audio, "appendSampleBuffer:", {float_sample_buffer(f, 4800, 4800LL * i)}) & 0xff) == 1, "real audio");
        }
        f.send(real_video, "markAsFinished");
        f.send(real_audio, "markAsFinished");
        require((f.send(real, "finishWriting") & 0xff) == 1, "the real movie completes");
        std::vector<std::uint8_t> bytes;
        const auto movie = read_movie(mov, bytes);
        require(movie.tracks.size() == 2 && movie.tracks[0].codec == mc::fourcc("avc1") && movie.tracks[0].samples.size() == 10 &&
                    movie.tracks[0].samples[0].keyframe && movie.tracks[1].codec == mc::fourcc("mp4a") && movie.tracks[1].samples.size() >= 10,
                "the real movie: " + std::to_string(movie.tracks.size()) + " tracks");
        std::cout << "[media capture] real movie: " << movie.tracks[0].samples.size() << " H.264 frames, " << movie.tracks[1].samples.size()
                  << " AAC packets (priming " << movie.tracks[1].priming << "), " << bytes.size() << " bytes" << std::endl;
    }
    std::filesystem::remove(mov, ignored);
    std::filesystem::remove(mp4, ignored);
}

// Alchemy's own test devices (ALCHEMY_CAMERA=pattern, ALCHEMY_AUDIO_INPUT=tone) through the host backends: the
// colour-bar camera and the 440 Hz microphone, which replace the host's devices (none is opened).
void test_devices(Fixture&) {
    _putenv_s("ALCHEMY_CAMERA", "pattern");
    _putenv_s("ALCHEMY_AUDIO_INPUT", "tone");
    const auto cameras = capture::make_host_backend();
    const auto listed = cameras->cameras();
    require(listed.size() == 1 && listed[0].id == "alchemy-test-pattern" && listed[0].formats.size() == 2, "the test pattern is the only camera");
    require(cameras->camera_access() == capture::Access::allowed && cameras->microphone_access() == capture::Access::allowed, "test devices are allowed");
    std::mutex mutex;
    std::vector<std::uint8_t> picture;
    std::uint32_t width{}, height{};
    std::atomic<int> pictures{0};
    capture::OpenError error{};
    auto stream = cameras->open(listed[0].id, 640, 480, 30, [&](const capture::Picture& p) {
        std::lock_guard lock(mutex);
        if (picture.empty()) picture.assign(p.data, p.data + p.size);
        width = p.width;
        height = p.height;
        ++pictures;
    }, error);
    require(stream && stream->start(), "the pattern opens");
    require(wait_for([&] { return pictures >= 3; }), "pictures arrive");
    stream->stop();
    {
        std::lock_guard lock(mutex);
        require(width == 640 && height == 480 && picture.size() == 640 * 480 * 3 / 2, "640x480 NV12");
        require(picture[0] == 235 && picture[639] == 16, "white bar on the left, black on the right");
    }
    const auto backend = hal::make_host_backend();
    const auto snapshot = backend->snapshot();
    const auto tone = std::find_if(snapshot.devices.begin(), snapshot.devices.end(), [](const hal::Device& d) { return d.input; });
    require(tone != snapshot.devices.end() && tone->uid == "alchemy-test-tone" && snapshot.default_input == tone->uid &&
                std::count_if(snapshot.devices.begin(), snapshot.devices.end(), [](const hal::Device& d) { return d.input; }) == 1,
            "the test tone is the only microphone, and the default");
    std::vector<float> samples;
    auto input = backend->open_input(*tone, [&](const float* data, std::uint32_t frames, std::uint64_t) {
        std::lock_guard lock(mutex);
        samples.insert(samples.end(), data, data + frames);
    });
    require(input && input->start(), "the tone opens");
    require(wait_for([&] { std::lock_guard lock(mutex); return samples.size() >= 4800; }), "the tone arrives");
    input->stop();
    double squares = 0, peak = 0;
    for (const auto v : samples) { squares += double(v) * v; peak = std::max(peak, std::fabs(double(v))); }
    const auto rms = std::sqrt(squares / samples.size());
    require(std::fabs(peak - 0.25) < 0.001 && std::fabs(rms - 0.25 / std::sqrt(2.0)) < 0.002, "a sine at a quarter of full scale: " + std::to_string(rms));
    _putenv_s("ALCHEMY_CAMERA", "");
    _putenv_s("ALCHEMY_AUDIO_INPUT", "");
}

} // namespace

int main() {
    media::set_hal_backend_factory([] { return std::make_unique<FakeHal>(); });
    media::set_capture_backend_factory([] { return std::make_unique<FakeCameras>(); });
    return fill_test::run("media capture", {
        {"devices", devices},
        {"authorization", authorization},
        {"session", session},
        {"engine_input", engine_input},
        {"auhal_input", auhal_input},
        {"audio_queue_input", audio_queue_input},
        {"ioproc_input", ioproc_input},
        {"recorder", recorder},
        {"asset_writer", asset_writer},
        {"asset_writer_movie", asset_writer_movie},
        {"test_devices", test_devices},
    });
}
