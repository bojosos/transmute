// AVFoundation capture and AVAudioEngine's input node through the strict-mode provider fixture,
// over a fake camera backend and a fake HAL with one microphone: no real camera or microphone is
// opened. Device discovery and formats, authorization as the (fake) host decides it, sessions
// delivering NV12 pictures as CMSampleBuffers to a delegate queue (scaled, mirrored, in the pixel
// format asked for), audio data outputs, and the engine's input node feeding the mixer or a tap.
#include "alchemy/gui/tests/fill_fixture.hpp"
#include "alchemy/darwin/media/capture_core.hpp"
#include "alchemy/darwin/media/hal_core.hpp"
#include "alchemy/darwin/media/media_support.hpp"
#include "alchemy/darwin/kit/support/support_kit.hpp"
#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/host/time.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using namespace fill_test;
namespace media = alchemy::gui::completion_kit::media;
namespace hal = media::hal;
namespace capture = media::capture;

#define FRAMEWORK(name) "/System/Library/Frameworks/" name ".framework/Versions/A/" name
constexpr std::string_view cm = FRAMEWORK("CoreMedia"), cv = FRAMEWORK("CoreVideo"), av = FRAMEWORK("AVFoundation");
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

} // namespace

int main() {
    media::set_hal_backend_factory([] { return std::make_unique<FakeHal>(); });
    media::set_capture_backend_factory([] { return std::make_unique<FakeCameras>(); });
    return fill_test::run("media capture", {
        {"devices", devices},
        {"authorization", authorization},
        {"session", session},
        {"engine_input", engine_input},
    });
}
