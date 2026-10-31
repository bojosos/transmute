#include "alchemy/darwin/media/media_support.hpp"
#include "alchemy/darwin/media/capture_core.hpp"
#include "alchemy/darwin/media/cmtime.hpp"
#include "alchemy/darwin/media/hal_core.hpp"
#include "alchemy/darwin/media/media_codecs.hpp"
#include "alchemy/darwin/media/pixel_convert.hpp"
#include "alchemy/darwin/media/video_frames.hpp"

#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/gui/runloop.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// AVFoundation capture over the host's cameras (capture_core.hpp: host::camera in alchemy-fx) and
// the CoreAudio HAL's input devices (hal_open_input: the host's microphones):
// - AVCaptureDevice: +devicesWithMediaType:, +devices, +defaultDeviceWithMediaType: (the first
//   camera; the default input device), +deviceWithUniqueID:, +defaultDeviceWithDeviceType:
//   mediaType:position: and AVCaptureDeviceDiscoverySession list the cameras (device type External,
//   or BuiltInWideAngleCamera when the host reports a front or back position) and the microphones
//   (Microphone, also matched by BuiltInMicrophone). The lists are read once per process (no
//   hot-plug). Each camera's formats are its sizes (420v format descriptions) with one frame-rate
//   range each; a microphone has one float32 format at its rate and channels. activeFormat
//   (default: the format nearest 1280x720) and the active frame durations are kept as set, under
//   lockForConfiguration:. Focus, exposure, white balance, torch and flash are not supported (the
//   documented isXSupported answers say so).
// - Authorization (+authorizationStatusForMediaType:) is what the host decides: its privacy
//   settings deny (Denied) or allow (Authorized) the camera or microphone, or say nothing
//   (NotDetermined). +requestAccessForMediaType:completionHandler: shows no prompt: when the
//   settings say nothing, the host decides by opening the default device once (not started), and
//   the answer is kept for the process; with no device the status stays NotDetermined and the
//   handler gets NO. The handler runs on a capture thread, as documented ("arbitrary dispatch
//   queue"). A device input for a denied device fails with AVErrorApplicationIsNotAuthorizedToUseDevice.
// - AVCaptureSession: inputs, outputs, presets, configuration nesting, running state and the
//   DidStartRunning / DidStopRunning / RuntimeError notifications. -startRunning opens each output's
//   device: the camera at the active format (or the preset's size; Low 192x144, Medium 480x360,
//   High 1280x720, the fixed sizes as named) and rate (the active minimum frame duration, else up
//   to 30 per second), the microphone at its own rate. A device that does not open (refused by the
//   privacy settings, gone) posts AVCaptureSessionRuntimeErrorNotification.
// - AVCaptureVideoDataOutput hands the delegate one CMSampleBuffer per picture on its queue: an
//   IOSurface-backed CVPixelBuffer in videoSettings' kCVPixelBufferPixelFormatTypeKey (420v by
//   default; any format the decoder path writes: availableVideoCVPixelFormatTypes) scaled to its
//   Width/Height keys when given, mirrored when the connection says so, stamped with the host time
//   it was taken. With alwaysDiscardsLateVideoFrames (the default) a picture arriving while the
//   delegate still has the previous one is dropped. AVCaptureAudioDataOutput hands over each
//   captured packet as float32 interleaved linear PCM at the device's rate (audioSettings are kept
//   but not applied).
// - Cloaked runs list no cameras and capture silence from the microphones (capture_core.hpp,
//   hal_host.cpp). Photo outputs are media_gaps.cpp's; still-image outputs are a class shell.
namespace alchemy::gui::completion_kit::media {
namespace {

constexpr std::string_view av_foundation = "/System/Library/Frameworks/AVFoundation.framework/Versions/A/AVFoundation";
constexpr std::string_view core_media = "/System/Library/Frameworks/CoreMedia.framework/Versions/A/CoreMedia";
constexpr std::string_view core_video = "/System/Library/Frameworks/CoreVideo.framework/Versions/A/CoreVideo";
constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";

constexpr std::uint32_t four(const char (&code)[5]) {
    return (std::uint32_t(std::uint8_t(code[0])) << 24) | (std::uint32_t(std::uint8_t(code[1])) << 16) |
           (std::uint32_t(std::uint8_t(code[2])) << 8) | std::uint32_t(std::uint8_t(code[3]));
}

// AVAuthorizationStatus
constexpr std::int64_t not_determined = 0, denied = 2, authorized = 3;
// AVError codes
constexpr std::int64_t error_unknown = -11800, error_not_connected = -11814, error_not_authorized = -11852;

enum class Media { video, audio };

std::function<std::unique_ptr<capture::Backend>()>& backend_factory() {
    static std::function<std::unique_ptr<capture::Backend>()> factory;
    return factory;
}

struct DeviceState final : ObjectPayload {
    Media media{};
    std::string id, name;
    std::int64_t position{};
    std::vector<capture::Format> formats;   // video
    std::uint32_t hal_id{}, channels{};     // audio
    double rate{};
    std::vector<Address> format_objects;    // retained AVCaptureDeviceFormat
    Address active{};                       // one of format_objects
    CMTime min_duration{}, max_duration{};  // invalid: the format's own
    std::uint32_t locks{};
};
struct FormatState final : ObjectPayload {
    Media media{};
    capture::Format format;
    std::uint32_t channels{};
    double rate{};
    Address description{};                  // retained CMFormatDescription
    Address range{};                        // retained AVFrameRateRange (video)
};
struct RangeState final : ObjectPayload { double min_rate{}, max_rate{}; };
struct DiscoveryState final : ObjectPayload { std::vector<Address> devices; };   // retained
struct InputState final : ObjectPayload {
    Address device{};                       // retained
    Address port{};                         // retained AVCaptureInputPort
};
struct PortState final : ObjectPayload { Address input{}; bool enabled{true}; };   // the input is not retained
struct ConnectionState final : ObjectPayload {
    Address output{};                       // not retained (the output holds the connection)
    bool enabled{true}, mirrored{}, automatic_mirroring{true};
    std::int64_t orientation{1};
};
struct OutputState final : ObjectPayload {
    Media media{};
    Address delegate{}, queue{}, settings{};   // the queue and settings retained
    bool discards_late{true};
    Address connection{};                   // retained AVCaptureConnection
    Address session{};                      // not retained
    bool alive{true};
    std::atomic<int> in_flight{};
};
struct SessionState final : ObjectPayload {
    bool running{};
    std::uint32_t configuring{};
    Address preset{};                       // retained
    std::vector<Address> inputs, outputs;   // retained
    std::vector<std::unique_ptr<capture::VideoStream>> video;
    std::vector<std::unique_ptr<hal::InputStream>> audio;
};

// One picture (NV12) or one audio packet (float32 interleaved) on its way to a delegate.
struct Captured {
    std::vector<std::uint8_t> picture;
    std::vector<float> samples;
    std::uint32_t width{}, height{}, frames{}, channels{};
    double rate{};
    std::uint64_t time_ns{};
};

// The picture `p` (NV12, w x h) as a planar 4:2:0 frame of `ow` x `oh`, nearest-neighbour scaled
// and mirrored left to right when asked.
codecs::VideoFrame planar(const Captured& p, std::uint32_t ow, std::uint32_t oh, bool mirror) {
    codecs::VideoFrame f;
    f.width = ow;
    f.height = oh;
    const auto cw = (ow + 1) / 2, chh = (oh + 1) / 2;
    f.planes[0] = {std::vector<std::uint8_t>(static_cast<std::size_t>(ow) * oh), ow, oh, ow};
    f.planes[1] = {std::vector<std::uint8_t>(static_cast<std::size_t>(cw) * chh), cw, chh, cw};
    f.planes[2] = f.planes[1];
    const auto w = p.width, h = p.height;
    const auto* src = p.picture.data();
    for (std::uint32_t y = 0; y < oh; ++y) {
        const auto sy = static_cast<std::size_t>(y) * h / oh;
        for (std::uint32_t x = 0; x < ow; ++x) {
            const auto sx = static_cast<std::size_t>(mirror ? ow - 1 - x : x) * w / ow;
            f.planes[0].data[static_cast<std::size_t>(y) * ow + x] = src[sy * w + sx];
        }
    }
    const auto sw = w / 2, sh = h / 2;
    for (std::uint32_t y = 0; y < chh; ++y) {
        const auto sy = std::min<std::size_t>(static_cast<std::size_t>(y) * sh / chh, sh ? sh - 1 : 0);
        for (std::uint32_t x = 0; x < cw; ++x) {
            const auto sx = std::min<std::size_t>(static_cast<std::size_t>(mirror ? cw - 1 - x : x) * sw / cw, sw ? sw - 1 : 0);
            const auto at = static_cast<std::size_t>(w) * h + sy * w + 2 * sx;
            f.planes[1].data[static_cast<std::size_t>(y) * cw + x] = src[at];
            f.planes[2].data[static_cast<std::size_t>(y) * cw + x] = src[at + 1];
        }
    }
    return f;
}

// AVCaptureSessionPreset sizes (0 x 0: the device's active format).
std::pair<std::uint32_t, std::uint32_t> preset_size(const std::string& preset) {
    static const std::map<std::string, std::pair<std::uint32_t, std::uint32_t>> sizes{
        {"AVCaptureSessionPresetLow", {192, 144}}, {"AVCaptureSessionPresetMedium", {480, 360}}, {"AVCaptureSessionPresetHigh", {1280, 720}},
        {"AVCaptureSessionPreset320x240", {320, 240}}, {"AVCaptureSessionPreset352x288", {352, 288}},
        {"AVCaptureSessionPreset640x480", {640, 480}}, {"AVCaptureSessionPreset960x540", {960, 540}},
        {"AVCaptureSessionPreset1280x720", {1280, 720}}, {"AVCaptureSessionPreset1920x1080", {1920, 1080}},
        {"AVCaptureSessionPreset3840x2160", {3840, 2160}}, {"AVCaptureSessionPresetiFrame960x540", {960, 540}},
        {"AVCaptureSessionPresetiFrame1280x720", {1280, 720}}};
    const auto found = sizes.find(preset);
    return found == sizes.end() ? std::pair<std::uint32_t, std::uint32_t>{0, 0} : found->second;
}

class Capture : public std::enable_shared_from_this<Capture> {
public:
    explicit Capture(Support& support) : support_(support), bridge_(security::bridge()) {}
    void install();

private:
    capture::Backend& backend() {
        std::lock_guard lock(mutex_);
        if (!backend_) backend_ = backend_factory() ? backend_factory()() : capture::make_host_backend();
        return *backend_;
    }
    const std::vector<Address>& devices();   // retained, cameras then microphones
    std::vector<Address> devices_of(std::optional<Media> media);
    Address default_device(Media media);
    Address make_format(DeviceState& d, const capture::Format* video);
    std::int64_t status(Media media);
    Media media_of(Address type, const char* what);
    std::shared_ptr<CompletionThread> thread();
    void write_time(Address at, const CMTime& t) {
        auto& m = support_.memory();
        m.write64(at, static_cast<std::uint64_t>(t.value));
        m.write32(at + 8, static_cast<std::uint32_t>(t.timescale));
        m.write32(at + 12, t.flags);
        m.write64(at + 16, static_cast<std::uint64_t>(t.epoch));
    }
    CMTime read_time(Address at) {
        auto& m = support_.memory();
        return {static_cast<std::int64_t>(m.read64(at)), static_cast<std::int32_t>(m.read32(at + 8)), m.read32(at + 12),
                static_cast<std::int64_t>(m.read64(at + 16))};
    }
    Address input_for(SessionState& session, Media media);   // the session's first input of `media`
    void start(Address session_object);
    void stop(Address session_object);
    void post_notification(std::string_view name, Address session, Address error = 0);
    void deliver(const std::shared_ptr<OutputState>& output, Address output_object, Captured captured);
    Address video_sample(const Captured& c, Address output_object, const std::shared_ptr<OutputState>& output);
    Address audio_sample(const Captured& c);

    Support& support_;
    std::shared_ptr<security::Bridge> bridge_;
    std::mutex mutex_;
    std::unique_ptr<capture::Backend> backend_;
    std::optional<std::vector<Address>> devices_;
    std::map<Media, std::int64_t> decided_;
    std::shared_ptr<CompletionThread> thread_;
    Address pixel_attributes_{};
    Address device_class_{}, format_class_{}, range_class_{}, discovery_class_{}, device_input_class_{}, port_class_{}, connection_class_{},
        video_output_class_{}, audio_output_class_{}, session_class_{};
};

std::shared_ptr<CompletionThread> Capture::thread() {
    std::lock_guard lock(mutex_);
    if (!thread_) thread_ = start_completion_thread(support_.process, "com.apple.avfoundation.capture");
    return thread_;
}

Address Capture::make_format(DeviceState& d, const capture::Format* video) {
    const auto object = support_.send_id(support_.send_id(format_class_, "alloc"), "init");
    const auto f = support_.objc.host_state<FormatState>(object);
    f->media = d.media;
    auto& m = support_.memory();
    const auto out = support_.process.allocate_guest(8, 8);
    m.write64(out, 0);
    if (video) {
        f->format = *video;
        // CMVideoFormatDescriptionCreate(allocator, codecType, width, height, extensions, out)
        static_cast<void>(support_.call(core_media, "_CMVideoFormatDescriptionCreate", {0, four("420v"), video->width, video->height, 0, out}));
        const auto range = support_.send_id(support_.send_id(range_class_, "alloc"), "init");
        const auto r = support_.objc.host_state<RangeState>(range);
        r->min_rate = video->min_rate > 0 ? video->min_rate : video->max_rate;
        r->max_rate = video->max_rate;
        f->range = range;
    } else {
        f->channels = d.channels;
        f->rate = d.rate;
        // AudioStreamBasicDescription: float32 interleaved linear PCM.
        const auto asbd = support_.process.allocate_guest(40, 8);
        m.write64(asbd, std::bit_cast<std::uint64_t>(d.rate));
        m.write32(asbd + 8, four("lpcm"));
        m.write32(asbd + 12, 9);   // kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked
        m.write32(asbd + 16, 4 * d.channels);
        m.write32(asbd + 20, 1);
        m.write32(asbd + 24, 4 * d.channels);
        m.write32(asbd + 28, d.channels);
        m.write32(asbd + 32, 32);
        m.write32(asbd + 36, 0);
        static_cast<void>(support_.call(core_media, "_CMAudioFormatDescriptionCreate", {0, asbd, 0, 0, 0, 0, 0, out}));
        support_.process.release_guest(asbd);
    }
    f->description = m.read64(out);
    support_.process.release_guest(out);
    return object;
}

const std::vector<Address>& Capture::devices() {
    {
        std::lock_guard lock(mutex_);
        if (devices_) return *devices_;
    }
    std::vector<Address> made;
    for (const auto& camera : backend().cameras()) {
        const auto object = support_.send_id(support_.send_id(device_class_, "alloc"), "init");
        const auto d = support_.objc.host_state<DeviceState>(object);
        d->media = Media::video;
        d->id = camera.id;
        d->name = camera.name;
        d->position = camera.position;
        d->formats = camera.formats;
        for (const auto& format : d->formats) d->format_objects.push_back(make_format(*d, &format));
        // The active format: the one nearest 1280x720.
        std::size_t best = 0;
        double best_cost = 1e300;
        for (std::size_t i = 0; i < d->formats.size(); ++i) {
            const double cost = std::fabs(static_cast<double>(d->formats[i].width) * d->formats[i].height - 1280.0 * 720.0);
            if (cost < best_cost) { best_cost = cost; best = i; }
        }
        if (!d->format_objects.empty()) d->active = d->format_objects[best];
        made.push_back(object);
    }
    for (const auto& input : hal_input_devices()) {
        const auto object = support_.send_id(support_.send_id(device_class_, "alloc"), "init");
        const auto d = support_.objc.host_state<DeviceState>(object);
        d->media = Media::audio;
        d->id = input.uid;
        d->name = input.name;
        d->hal_id = input.id;
        d->channels = input.channels;
        d->rate = input.rate;
        d->format_objects.push_back(make_format(*d, nullptr));
        d->active = d->format_objects.front();
        made.push_back(object);
    }
    std::lock_guard lock(mutex_);
    if (!devices_) devices_ = std::move(made);
    else for (const auto object : made) support_.release(object);
    return *devices_;
}

std::vector<Address> Capture::devices_of(std::optional<Media> media) {
    std::vector<Address> out;
    for (const auto object : devices())
        if (!media || support_.objc.find_host_state<DeviceState>(object)->media == *media) out.push_back(object);
    return out;
}

Address Capture::default_device(Media media) {
    const auto all = devices_of(media);
    if (all.empty()) return 0;
    if (media == Media::audio) {
        const auto wanted = hal_default_input();
        for (const auto object : all)
            if (support_.objc.find_host_state<DeviceState>(object)->hal_id == wanted) return object;
    }
    return all.front();
}

Media Capture::media_of(Address type, const char* what) {
    const auto text = type ? support_.cf.utf8(type) : std::string{};
    if (text == "vide") return Media::video;
    if (text == "soun") return Media::audio;
    throw std::runtime_error(std::string("Alchemy: ") + what + " with a media type other than video or audio (NSInvalidArgumentException)");
}

std::int64_t Capture::status(Media media) {
    const auto access = media == Media::video ? backend().camera_access() : backend().microphone_access();
    if (access == capture::Access::denied) return denied;
    if (access == capture::Access::allowed) return authorized;
    std::lock_guard lock(mutex_);
    const auto found = decided_.find(media);
    return found == decided_.end() ? not_determined : found->second;
}

Address Capture::input_for(SessionState& session, Media media) {
    for (const auto input : session.inputs) {
        const auto in = support_.objc.find_host_state<InputState>(input);
        if (in && in->device && support_.objc.find_host_state<DeviceState>(in->device)->media == media) return input;
    }
    return 0;
}

void Capture::post_notification(std::string_view name, Address session, Address error) {
    const auto center = support_.send_id(support_.objc.find_class("NSNotificationCenter"), "defaultCenter");
    const auto key = bridge_->constant(av_foundation, "_AVCaptureSessionErrorKey");
    Address info{};
    if (error && key) info = bridge_->dictionary({{key, error}});
    const auto name_object = bridge_->constant(av_foundation, std::string("_") + std::string(name));
    if (center && name_object) static_cast<void>(support_.send_id(center, "postNotificationName:object:userInfo:", {name_object, session, info}));
    support_.release(info);
}

Address Capture::video_sample(const Captured& c, Address output_object, const std::shared_ptr<OutputState>& output) {
    // The pixel format and size the output's settings ask for.
    std::uint32_t format = four("420v"), width = c.width, height = c.height;
    bool mirror = false;
    Address settings{}, connection{};
    {
        std::lock_guard lock(mutex_);
        settings = support_.retain(output->settings);
        connection = output->connection;
    }
    if (settings) {
        const auto value = [&](const char* key) -> std::optional<std::int64_t> {
            const auto k = support_.cf.string(key);
            const auto found = bridge_->lookup(settings, k);
            support_.release(k);
            return found ? bridge_->integer(found) : std::nullopt;
        };
        if (const auto f = value("PixelFormatType"); f && video_frames::writable(static_cast<std::uint32_t>(*f))) format = static_cast<std::uint32_t>(*f);
        const auto w = value("Width"), h = value("Height");
        if (w && h && *w > 1 && *h > 1) { width = static_cast<std::uint32_t>(*w) & ~1U; height = static_cast<std::uint32_t>(*h) & ~1U; }
        support_.release(settings);
    }
    if (const auto conn = connection ? support_.objc.find_host_state<ConnectionState>(connection) : nullptr) mirror = conn->mirrored;
    static_cast<void>(output_object);
    // IOSurface-backed pixel buffers, as capture hands out (CoreVideo's key exists once every provider is in).
    if (!pixel_attributes_)
        if (const auto key = bridge_->constant(core_video, "_kCVPixelBufferIOSurfacePropertiesKey")) {
            const auto empty = bridge_->dictionary({});
            pixel_attributes_ = bridge_->dictionary({{key, empty}});
            support_.release(empty);
        }
    const auto frame = planar(c, width, height, mirror);
    auto& m = support_.memory();
    const auto out = support_.process.allocate_guest(8 + 72, 8);
    m.write64(out, 0);
    // CVPixelBufferCreate(allocator, width, height, format, attributes, CVPixelBufferRef *out)
    const auto created = static_cast<std::int32_t>(support_.call(core_video, "_CVPixelBufferCreate", {0, width, height, format, pixel_attributes_, out}));
    const auto buffer = m.read64(out);
    if (created || !buffer) {
        support_.process.release_guest(out);
        return 0;
    }
    static_cast<void>(support_.call(core_video, "_CVPixelBufferLockBaseAddress", {buffer, 0}));
    const auto planes = support_.call(core_video, "_CVPixelBufferGetPlaneCount", {buffer});
    std::vector<Address> bases;
    std::vector<std::vector<std::uint8_t>> storage;
    std::vector<pixels::Plane> views;
    for (Address p = 0; p < std::max<Address>(planes, 1); ++p) {
        const auto base = planes ? support_.call(core_video, "_CVPixelBufferGetBaseAddressOfPlane", {buffer, p}) : support_.call(core_video, "_CVPixelBufferGetBaseAddress", {buffer});
        const auto row = planes ? support_.call(core_video, "_CVPixelBufferGetBytesPerRowOfPlane", {buffer, p}) : support_.call(core_video, "_CVPixelBufferGetBytesPerRow", {buffer});
        const auto rows = planes ? support_.call(core_video, "_CVPixelBufferGetHeightOfPlane", {buffer, p}) : support_.call(core_video, "_CVPixelBufferGetHeight", {buffer});
        bases.push_back(base);
        storage.emplace_back(static_cast<std::size_t>(row * rows));
        views.push_back({storage.back().data(), row});
    }
    const bool written = video_frames::write(frame, format, views);
    for (std::size_t p = 0; p < bases.size(); ++p)
        if (written && bases[p]) m.write_bytes(bases[p], storage[p]);
    static_cast<void>(support_.call(core_video, "_CVPixelBufferUnlockBaseAddress", {buffer, 0}));
    Address sample{};
    if (written) {
        m.write64(out, 0);
        static_cast<void>(support_.call(core_media, "_CMVideoFormatDescriptionCreateForImageBuffer", {0, buffer, out}));
        const auto description = m.read64(out);
        const auto timing = out + 8;
        write_time(timing, time_invalid);   // duration
        write_time(timing + 24, {static_cast<std::int64_t>(c.time_ns), 1'000'000'000, cmtime_flags::valid, 0});
        write_time(timing + 48, time_invalid);
        m.write64(out, 0);
        const auto result = static_cast<std::int32_t>(support_.call(core_media, "_CMSampleBufferCreateReadyWithImageBuffer", {0, buffer, description, timing, out}));
        sample = result ? 0 : m.read64(out);
        support_.release(description);
    }
    support_.release(buffer);
    support_.process.release_guest(out);
    return sample;
}

Address Capture::audio_sample(const Captured& c) {
    auto& m = support_.memory();
    const auto bytes = static_cast<std::size_t>(c.frames) * c.channels * 4;
    if (!bytes) return 0;
    const auto scratch = support_.process.allocate_guest(8 + 40 + 80 + bytes, 16);
    const auto out = scratch, asbd = scratch + 8, timing = scratch + 48, size = scratch + 120, data = scratch + 128;
    m.write64(asbd, std::bit_cast<std::uint64_t>(c.rate));
    m.write32(asbd + 8, four("lpcm"));
    m.write32(asbd + 12, 9);
    m.write32(asbd + 16, 4 * c.channels);
    m.write32(asbd + 20, 1);
    m.write32(asbd + 24, 4 * c.channels);
    m.write32(asbd + 28, c.channels);
    m.write32(asbd + 32, 32);
    m.write32(asbd + 36, 0);
    m.write64(out, 0);
    static_cast<void>(support_.call(core_media, "_CMAudioFormatDescriptionCreate", {0, asbd, 0, 0, 0, 0, 0, out}));
    const auto description = m.read64(out);
    // CMBlockBufferCreateWithMemoryBlock(allocator, NULL, length, blockAllocator, customSource, offset, dataLength, flags, &out)
    CallRegs create{};
    create.x[2] = bytes;
    create.x[6] = bytes;
    create.x[7] = 1;   // kCMBlockBufferAssureMemoryNowFlag
    create.push_stack(out);
    m.write64(out, 0);
    Address sample{};
    if (description && !static_cast<std::int32_t>(support_.call(core_media, "_CMBlockBufferCreateWithMemoryBlock", create).x0)) {
        const auto block = m.read64(out);
        m.write_bytes(data, std::span(reinterpret_cast<const std::uint8_t*>(c.samples.data()), bytes));
        static_cast<void>(support_.call(core_media, "_CMBlockBufferReplaceDataBytes", {data, block, 0, bytes}));
        write_time(timing, {1, static_cast<std::int32_t>(std::lround(c.rate)), cmtime_flags::valid, 0});
        write_time(timing + 24, {static_cast<std::int64_t>(c.time_ns), 1'000'000'000, cmtime_flags::valid, 0});
        write_time(timing + 48, time_invalid);
        m.write64(size, 4ULL * c.channels);
        // CMSampleBufferCreateReady(allocator, data, format, samples, timingEntries, timing, sizeEntries, sizes, &out)
        CallRegs ready{};
        ready.x[1] = block; ready.x[2] = description; ready.x[3] = c.frames; ready.x[4] = 1; ready.x[5] = timing; ready.x[6] = 1; ready.x[7] = size;
        ready.push_stack(out);
        m.write64(out, 0);
        if (!static_cast<std::int32_t>(support_.call(core_media, "_CMSampleBufferCreateReady", ready).x0)) sample = m.read64(out);
        support_.release(block);
    }
    support_.release(description);
    support_.process.release_guest(scratch);
    return sample;
}

// On a capture device's thread: hands the picture or packet to the completion thread, which makes
// the sample buffer and gives it to the delegate on its queue.
void Capture::deliver(const std::shared_ptr<OutputState>& output, Address output_object, Captured captured) {
    {
        std::lock_guard lock(mutex_);
        if (!output->alive || !output->delegate || !output->queue) return;
        const int limit = output->media == Media::video && output->discards_late ? 1 : 16;
        if (output->in_flight >= limit) return;   // late: dropped
        ++output->in_flight;
    }
    std::shared_ptr<CompletionThread> worker;
    {
        std::lock_guard lock(mutex_);
        worker = thread_;
    }
    if (!worker) { --output->in_flight; return; }
    const auto self = shared_from_this();
    worker->post([self, output, output_object, captured = std::move(captured)] {
        auto* sp = &self->support_;
        Address delegate{}, queue{}, connection{};
        {
            std::lock_guard lock(self->mutex_);
            if (output->alive) {
                delegate = output->delegate;
                queue = output->queue;
                connection = output->connection;
                static_cast<void>(sp->retain(output_object));
            }
        }
        if (!queue) { --output->in_flight; return; }
        static_cast<void>(sp->call(libsystem, "_dispatch_retain", {queue}));   // a dispatch object, not a CF one
        const auto sample = output->media == Media::video ? self->video_sample(captured, output_object, output) : self->audio_sample(captured);
        const auto b = self->bridge_;
        b->dispatch_async(queue, [self, sp, output, output_object, delegate, connection, sample] {
            const auto selector = sp->objc.selector("captureOutput:didOutputSampleBuffer:fromConnection:");
            bool current = false;
            {
                std::lock_guard lock(self->mutex_);
                current = output->alive && output->delegate == delegate;
            }
            if (current && sample && delegate && sp->objc.responds(delegate, selector)) {
                CallRegs r{};
                r.x[2] = output_object;
                r.x[3] = sample;
                r.x[4] = connection;
                static_cast<void>(sp->objc.send(delegate, selector, r));
            }
            sp->release(sample);
            --output->in_flight;
            sp->release(output_object);
        });
        static_cast<void>(sp->call(libsystem, "_dispatch_release", {queue}));
    });
}

void Capture::start(Address session_object) {
    const auto session = support_.objc.host_state<SessionState>(session_object);
    std::vector<Address> outputs;
    std::string preset;
    {
        std::lock_guard lock(mutex_);
        if (session->running) return;
        outputs = session->outputs;
        preset = session->preset ? support_.cf.utf8(session->preset) : "AVCaptureSessionPresetHigh";
    }
    if (!outputs.empty()) static_cast<void>(thread());
    const auto self = shared_from_this();
    std::vector<std::unique_ptr<capture::VideoStream>> video;
    std::vector<std::unique_ptr<hal::InputStream>> audio;
    Address failure{};
    for (const auto output_object : outputs) {
        const auto output = support_.objc.find_host_state<OutputState>(output_object);
        if (!output) continue;
        const auto input = input_for(*session, output->media);
        const auto in = input ? support_.objc.find_host_state<InputState>(input) : nullptr;
        const auto device = in ? support_.objc.find_host_state<DeviceState>(in->device) : nullptr;
        if (!device) continue;
        if (output->media == Media::video) {
            const auto active = device->active ? support_.objc.find_host_state<FormatState>(device->active) : nullptr;
            auto [width, height] = preset_size(preset);
            if (preset == "AVCaptureSessionPresetInputPriority" || preset == "AVCaptureSessionPresetPhoto" || preset == "AVCaptureSessionPresetHigh" || !width) {
                width = active ? active->format.width : 0;
                height = active ? active->format.height : 0;
            }
            double rate = active ? std::min(active->format.max_rate, 30.0) : 30.0;
            if (is_numeric(device->min_duration) && device->min_duration.value > 0) rate = 1.0 / seconds(device->min_duration);
            capture::OpenError error{};
            auto stream = backend().open(device->id, width, height, rate, [self, output, output_object](const capture::Picture& p) {
                Captured c;
                c.picture.assign(p.data, p.data + p.size);
                c.width = p.width;
                c.height = p.height;
                c.time_ns = p.time_ns;
                self->deliver(output, output_object, std::move(c));
            }, error);
            if (stream && stream->start()) video.push_back(std::move(stream));
            else if (!failure)
                failure = bridge_->error("AVFoundationErrorDomain", error == capture::OpenError::denied ? error_not_authorized
                                                                     : error == capture::OpenError::not_found ? error_not_connected : error_unknown,
                                         error == capture::OpenError::denied ? "The host's privacy settings refuse the camera" : "The camera did not start");
        } else {
            const std::uint32_t channels = device->channels;
            const double rate = device->rate;
            auto stream = hal_open_input(device->hal_id, [self, output, output_object, channels, rate](const float* samples, std::uint32_t frames, std::uint64_t time_ns) {
                Captured c;
                c.samples.assign(samples, samples + static_cast<std::size_t>(frames) * channels);
                c.frames = frames;
                c.channels = channels;
                c.rate = rate;
                c.time_ns = time_ns;
                self->deliver(output, output_object, std::move(c));
            });
            if (stream && stream->start()) audio.push_back(std::move(stream));
            else if (!failure)
                failure = bridge_->error("AVFoundationErrorDomain", backend().microphone_access() == capture::Access::denied ? error_not_authorized : error_unknown,
                                         "The microphone did not start");
        }
    }
    {
        std::lock_guard lock(mutex_);
        session->running = true;
        session->video = std::move(video);
        session->audio = std::move(audio);
    }
    if (failure) {
        post_notification("AVCaptureSessionRuntimeErrorNotification", session_object, failure);
        support_.release(failure);
    }
    post_notification("AVCaptureSessionDidStartRunningNotification", session_object);
}

void Capture::stop(Address session_object) {
    const auto session = support_.objc.find_host_state<SessionState>(session_object);
    if (!session) return;
    std::vector<std::unique_ptr<capture::VideoStream>> video;
    std::vector<std::unique_ptr<hal::InputStream>> audio;
    {
        std::lock_guard lock(mutex_);
        if (!session->running) return;
        session->running = false;
        video = std::move(session->video);
        audio = std::move(session->audio);
    }
    for (auto& stream : video) stream->stop();
    for (auto& stream : audio) stream->stop();
    video.clear();
    audio.clear();
    post_notification("AVCaptureSessionDidStopRunningNotification", session_object);
}

void Capture::install() {
    auto& support = support_;
    auto* sp = &support_;
    const auto self = shared_from_this();
    const auto ns_object = "NSObject";
    device_class_ = support.declare_class("AVCaptureDevice", ns_object, av_foundation);
    format_class_ = support.declare_class("AVCaptureDeviceFormat", ns_object, av_foundation);
    range_class_ = support.declare_class("AVFrameRateRange", ns_object, av_foundation);
    discovery_class_ = support.declare_class("AVCaptureDeviceDiscoverySession", ns_object, av_foundation);
    const auto input_class = support.declare_class("AVCaptureInput", ns_object, av_foundation);
    device_input_class_ = support.declare_class("AVCaptureDeviceInput", "AVCaptureInput", av_foundation);
    port_class_ = support.declare_class("AVCaptureInputPort", ns_object, av_foundation);
    connection_class_ = support.declare_class("AVCaptureConnection", ns_object, av_foundation);
    const auto output_class = support.declare_class("AVCaptureOutput", ns_object, av_foundation);
    static_cast<void>(support.declare_class("AVCaptureStillImageOutput", "AVCaptureOutput", av_foundation));
    video_output_class_ = support.declare_class("AVCaptureVideoDataOutput", "AVCaptureOutput", av_foundation);
    audio_output_class_ = support.declare_class("AVCaptureAudioDataOutput", "AVCaptureOutput", av_foundation);
    session_class_ = support.declare_class("AVCaptureSession", ns_object, av_foundation);
    for (const auto* name : {"AVCaptureSessionDidStartRunningNotification", "AVCaptureSessionDidStopRunningNotification",
                             "AVCaptureSessionWasInterruptedNotification", "AVCaptureSessionInterruptionEndedNotification",
                             "AVCaptureDeviceTypeBuiltInMicrophone", "AVCaptureSessionPresetLow", "AVCaptureSessionPresetMedium",
                             "AVCaptureSessionPresetPhoto", "AVCaptureSessionPresetInputPriority", "AVCaptureSessionPreset320x240",
                             "AVCaptureSessionPreset352x288", "AVCaptureSessionPreset640x480", "AVCaptureSessionPreset960x540",
                             "AVCaptureSessionPreset1280x720", "AVCaptureSessionPreset1920x1080", "AVCaptureSessionPreset3840x2160",
                             "AVCaptureSessionPresetiFrame960x540", "AVCaptureSessionPresetiFrame1280x720"})
        support.string_constant(av_foundation, std::string("_") + name, name);

    const auto text = [sp](const std::string& value) { return sp->send_id(sp->cf.string(value), "autorelease"); };
    const auto array = [this, sp](const std::vector<Address>& items) { return sp->send_id(bridge_->array(items), "autorelease"); };
    const auto device_of = [sp](Address object) {
        const auto d = sp->objc.find_host_state<DeviceState>(object);
        if (!d) unsupported("an AVCaptureDevice Alchemy did not make");
        return d;
    };

    // ---- AVCaptureDevice ------------------------------------------------------------------------
    support.method(device_class_, "devices", [self, array](CpuState& s, GuestMemory&) { s.x[0] = array(self->devices_of(std::nullopt)); }, "@@:", true);
    support.method(device_class_, "devicesWithMediaType:", [self, array](CpuState& s, GuestMemory&) {
        const auto type = s.x[2] ? self->support_.cf.utf8(s.x[2]) : std::string{};
        s.x[0] = array(type == "vide" ? self->devices_of(Media::video) : type == "soun" ? self->devices_of(Media::audio) : std::vector<Address>{});
    }, "@@:@", true);
    support.method(device_class_, "defaultDeviceWithMediaType:", [self](CpuState& s, GuestMemory&) {
        const auto type = s.x[2] ? self->support_.cf.utf8(s.x[2]) : std::string{};
        s.x[0] = type == "vide" ? self->default_device(Media::video) : type == "soun" ? self->default_device(Media::audio) : 0;
    }, "@@:@", true);
    support.method(device_class_, "deviceWithUniqueID:", [self, sp](CpuState& s, GuestMemory&) {
        const auto wanted = s.x[2] ? sp->cf.utf8(s.x[2]) : std::string{};
        s.x[0] = 0;
        for (const auto object : self->devices())
            if (sp->objc.find_host_state<DeviceState>(object)->id == wanted) { s.x[0] = object; break; }
    }, "@@:@", true);
    // The device types a device matches (its own first).
    const auto types_of = [](const DeviceState& d) -> std::vector<std::string> {
        if (d.media == Media::audio) return {"AVCaptureDeviceTypeMicrophone", "AVCaptureDeviceTypeBuiltInMicrophone"};
        if (d.position != 0) return {"AVCaptureDeviceTypeBuiltInWideAngleCamera"};
        return {"AVCaptureDeviceTypeExternal", "AVCaptureDeviceTypeExternalUnknown"};
    };
    // Devices of `types` (an NSArray of device types; 0: the single `type`), `media` and `position`.
    const auto matching = [self, sp, types_of](Address types, Address type_one, Address media, std::int64_t position) {
        std::vector<std::string> wanted;
        if (type_one) wanted.push_back(sp->cf.utf8(type_one));
        if (types) for (const auto t : sp->cf.array_values(types)) wanted.push_back(sp->cf.utf8(t));
        const auto type = media ? sp->cf.utf8(media) : std::string{};
        std::vector<Address> out;
        for (const auto object : self->devices()) {
            const auto d = sp->objc.find_host_state<DeviceState>(object);
            if (!type.empty() && type != (d->media == Media::video ? "vide" : "soun")) continue;
            if (position != 0 && d->position != position) continue;
            const auto own = types_of(*d);
            if (std::none_of(own.begin(), own.end(), [&](const std::string& t) { return std::find(wanted.begin(), wanted.end(), t) != wanted.end(); })) continue;
            out.push_back(object);
        }
        return out;
    };
    support.method(device_class_, "authorizationStatusForMediaType:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<std::uint64_t>(self->status(self->media_of(s.x[2], "+[AVCaptureDevice authorizationStatusForMediaType:]")));
    }, "q@:@", true);
    support.method(device_class_, "requestAccessForMediaType:completionHandler:", [self, sp](CpuState& s, GuestMemory&) {
        const auto media = self->media_of(s.x[2], "+[AVCaptureDevice requestAccessForMediaType:completionHandler:]");
        auto status = self->status(media);
        if (status == not_determined) {
            // The host decides: opening the default device once (not started) answers without a prompt.
            std::optional<std::int64_t> decided;
            if (media == Media::video) {
                if (const auto device = self->default_device(Media::video)) {
                    capture::OpenError error{};
                    auto stream = self->backend().open(sp->objc.find_host_state<DeviceState>(device)->id, 0, 0, 0, [](const capture::Picture&) {}, error);
                    if (stream) decided = authorized;
                    else if (error == capture::OpenError::denied) decided = denied;
                }
            } else if (const auto device = self->default_device(Media::audio)) {
                if (hal_open_input(sp->objc.find_host_state<DeviceState>(device)->hal_id, [](const float*, std::uint32_t, std::uint64_t) {})) decided = authorized;
            }
            if (decided) {
                std::lock_guard lock(self->mutex_);
                self->decided_[media] = *decided;
                status = *decided;
            }
        }
        if (s.x[3]) self->thread()->post_block(self->bridge_->block_copy(s.x[3]), {status == authorized ? 1U : 0U});
    }, "v@:@@?", true);
    support.method(device_class_, "uniqueID", [device_of, text](CpuState& s, GuestMemory&) { s.x[0] = text(device_of(s.x[0])->id); }, "@@:");
    support.method(device_class_, "modelID", [device_of, text](CpuState& s, GuestMemory&) { s.x[0] = text(device_of(s.x[0])->name); }, "@@:");
    support.method(device_class_, "localizedName", [device_of, text](CpuState& s, GuestMemory&) { s.x[0] = text(device_of(s.x[0])->name); }, "@@:");
    support.method(device_class_, "manufacturer", [text](CpuState& s, GuestMemory&) { s.x[0] = text(""); }, "@@:");
    support.method(device_class_, "deviceType", [device_of, types_of, sp](CpuState& s, GuestMemory&) {
        s.x[0] = sp->cf.constant_string(types_of(*device_of(s.x[0])).front());
    }, "@@:");
    support.method(device_class_, "position", [device_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<std::uint64_t>(device_of(s.x[0])->position); }, "q@:");
    // kIOAudioDeviceTransportTypeBuiltIn 'bltn', USB 'usb '
    support.method(device_class_, "transportType", [device_of](CpuState& s, GuestMemory&) {
        s.x[0] = device_of(s.x[0])->position != 0 ? four("bltn") : four("usb ");
    }, "i@:");
    support.method(device_class_, "hasMediaType:", [device_of, sp](CpuState& s, GuestMemory&) {
        const auto type = s.x[2] ? sp->cf.utf8(s.x[2]) : std::string{};
        s.x[0] = type == (device_of(s.x[0])->media == Media::video ? "vide" : "soun") ? 1 : 0;
    }, "B@:@");
    support.method(device_class_, "isConnected", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    for (const auto* selector : {"isSuspended", "isInUseByAnotherApplication", "hasFlash", "hasTorch", "isFlashAvailable", "isTorchAvailable",
                                 "isVideoHDREnabled", "isCenterStageActive", "isPortraitEffectActive", "isStudioLightActive",
                                 "isAdjustingFocus", "isAdjustingExposure", "isAdjustingWhiteBalance"})
        support.method(device_class_, selector, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    for (const auto* selector : {"isFocusModeSupported:", "isExposureModeSupported:", "isWhiteBalanceModeSupported:", "isTorchModeSupported:",
                                 "isFlashModeSupported:"})
        support.method(device_class_, selector, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:q");
    // AVCaptureFocusModeLocked, AVCaptureExposureModeLocked, AVCaptureWhiteBalanceModeLocked (0)
    for (const auto* selector : {"focusMode", "exposureMode", "whiteBalanceMode", "torchMode", "flashMode"})
        support.method(device_class_, selector, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "q@:");
    support.method(device_class_, "linkedDevices", [array](CpuState& s, GuestMemory&) { s.x[0] = array({}); }, "@@:");
    // The system video effects and microphone modes (macOS 12-14 Control Center features): none is available
    // or enabled. Both getter spellings are answered (the class properties' declared getters vary).
    for (const auto* selector : {"reactionEffectsEnabled", "reactionEffectGesturesEnabled", "isCenterStageEnabled", "centerStageEnabled",
                                 "isPortraitEffectEnabled", "portraitEffectEnabled", "isStudioLightEnabled", "studioLightEnabled"})
        support.method(device_class_, selector, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:", true);
    // AVCaptureCenterStageControlModeUser 0; AVCaptureMicrophoneModeStandard 0.
    for (const auto* selector : {"centerStageControlMode", "preferredMicrophoneMode", "activeMicrophoneMode"})
        support.method(device_class_, selector, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "q@:", true);
    for (const auto* selector : {"canPerformReactionEffects", "isPortraitEffectSupported", "isStudioLightSupported"})
        support.method(device_class_, selector, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    // The device's CoreMediaIO object: none (kCMIOObjectUnknown); CoreMediaIO has no devices here.
    support.method(device_class_, "connectionID", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "i@:");
    for (const auto* selector : {"isFocusPointOfInterestSupported", "isExposurePointOfInterestSupported"})
        support.method(device_class_, selector, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    // CGPoint (an HFA of two doubles in d0, d1): the centre, the documented default.
    for (const auto* selector : {"focusPointOfInterest", "exposurePointOfInterest"})
        support.method(device_class_, selector, [](CpuState& s, GuestMemory&) {
            s.v[0] = {std::bit_cast<std::uint64_t>(0.5), 0};
            s.v[1] = {std::bit_cast<std::uint64_t>(0.5), 0};
        }, "{CGPoint=dd}@:");
    support.method(device_class_, "reactionEffectsInProgress", [array](CpuState& s, GuestMemory&) { s.x[0] = array({}); }, "@@:");
    support.method(device_class_, "availableReactionTypes", [sp](CpuState& s, GuestMemory&) {
        s.x[0] = sp->send_id(sp->objc.find_class("NSSet"), "set");
    }, "@@:");
    support.method(device_class_, "formats", [device_of, array](CpuState& s, GuestMemory&) { s.x[0] = array(device_of(s.x[0])->format_objects); }, "@@:");
    support.method(device_class_, "activeFormat", [self, device_of](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        s.x[0] = device_of(s.x[0])->active;
    }, "@@:");
    const auto require_lock = [](const DeviceState& d, const char* what) {
        if (d.locks == 0) throw std::runtime_error(std::string("Alchemy: -[AVCaptureDevice ") + what + "] without lockForConfiguration: (NSGenericException)");
    };
    support.method(device_class_, "setActiveFormat:", [self, device_of, require_lock](CpuState& s, GuestMemory&) {
        const auto d = device_of(s.x[0]);
        require_lock(*d, "setActiveFormat:");
        if (std::find(d->format_objects.begin(), d->format_objects.end(), s.x[2]) == d->format_objects.end())
            throw std::runtime_error("Alchemy: -[AVCaptureDevice setActiveFormat:] with a format not in -formats (NSInvalidArgumentException)");
        std::lock_guard lock(self->mutex_);
        d->active = s.x[2];
        d->min_duration = d->max_duration = time_invalid;   // reset to the format's own, as documented
    }, "v@:@");
    // The active frame durations: as set, else the active format's (1 / its fastest, 1 / its slowest rate).
    const auto duration = [self, sp](Address device, bool minimum) {
        const auto d = sp->objc.find_host_state<DeviceState>(device);
        std::lock_guard lock(self->mutex_);
        const auto& set = minimum ? d->min_duration : d->max_duration;
        if (is_valid(set)) return set;
        const auto f = d->active ? sp->objc.find_host_state<FormatState>(d->active) : nullptr;
        if (!f || d->media != Media::video) return time_invalid;
        const auto rate = minimum ? std::min(f->format.max_rate, 30.0) : (f->format.min_rate > 0 ? f->format.min_rate : f->format.max_rate);
        return rate > 0 ? make_with_seconds(1.0 / rate, 1'000'000) : time_invalid;
    };
    support.method(device_class_, "activeVideoMinFrameDuration", [self, duration](CpuState& s, GuestMemory&) { self->write_time(s.x[8], duration(s.x[0], true)); }, "{?=qiIq}@:");
    support.method(device_class_, "activeVideoMaxFrameDuration", [self, duration](CpuState& s, GuestMemory&) { self->write_time(s.x[8], duration(s.x[0], false)); }, "{?=qiIq}@:");
    // -setActiveVideoMinFrameDuration:(CMTime) (a 24-byte struct: passed by reference in x2)
    for (const bool minimum : {true, false}) {
        const auto selector = minimum ? "setActiveVideoMinFrameDuration:" : "setActiveVideoMaxFrameDuration:";
        support.method(device_class_, selector, [self, device_of, require_lock, minimum, selector](CpuState& s, GuestMemory&) {
            const auto d = device_of(s.x[0]);
            require_lock(*d, selector);
            const auto value = self->read_time(s.x[2]);
            std::lock_guard lock(self->mutex_);
            (minimum ? d->min_duration : d->max_duration) = is_numeric(value) && value.value > 0 ? value : time_invalid;
        }, "v@:{?=qiIq}");
    }
    support.method(device_class_, "lockForConfiguration:", [device_of](CpuState& s, GuestMemory&) {
        ++device_of(s.x[0])->locks;
        s.x[0] = 1;
    }, "B@:^@");
    support.method(device_class_, "unlockForConfiguration", [device_of](CpuState& s, GuestMemory&) {
        auto d = device_of(s.x[0]);
        if (d->locks) --d->locks;
    }, "v@:");
    support.method(device_class_, "supportsAVCaptureSessionPreset:", [device_of, sp](CpuState& s, GuestMemory&) {
        const auto d = device_of(s.x[0]);
        const auto preset = s.x[2] ? sp->cf.utf8(s.x[2]) : std::string{};
        const auto [w, h] = preset_size(preset);
        if (d->media == Media::audio) { s.x[0] = 1; return; }
        s.x[0] = !w || std::any_of(d->formats.begin(), d->formats.end(), [&](const capture::Format& f) { return f.width >= w && f.height >= h; }) ? 1 : 0;
    }, "B@:@");

    // ---- AVCaptureDeviceFormat and AVFrameRateRange -------------------------------------------
    const auto format_of = [sp](Address object) {
        const auto f = sp->objc.find_host_state<FormatState>(object);
        if (!f) unsupported("an AVCaptureDeviceFormat Alchemy did not make");
        return f;
    };
    support.method(format_class_, "mediaType", [format_of, sp](CpuState& s, GuestMemory&) {
        s.x[0] = sp->cf.constant_string(format_of(s.x[0])->media == Media::video ? "vide" : "soun");
    }, "@@:");
    support.method(format_class_, "formatDescription", [format_of](CpuState& s, GuestMemory&) { s.x[0] = format_of(s.x[0])->description; }, "^{opaqueCMFormatDescription=}@:");
    support.method(format_class_, "videoSupportedFrameRateRanges", [format_of, array](CpuState& s, GuestMemory&) {
        const auto f = format_of(s.x[0]);
        s.x[0] = array(f->range ? std::vector<Address>{f->range} : std::vector<Address>{});
    }, "@@:");
    // CMVideoDimensions {int32 width, int32 height} in x0.
    support.method(format_class_, "highResolutionStillImageDimensions", [format_of](CpuState& s, GuestMemory&) {
        const auto f = format_of(s.x[0]);
        s.x[0] = f->format.width | (static_cast<std::uint64_t>(f->format.height) << 32);
    }, "{?=ii}@:");
    support.method(format_class_, "isVideoBinned", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    // No system video effect, stabilization, multi-camera or HDR mode is supported by any format.
    for (const auto* selector : {"isVideoHDRSupported", "isPortraitEffectSupported", "isCenterStageSupported", "isStudioLightSupported",
                                 "reactionEffectsSupported", "isBackgroundReplacementSupported", "isMultiCamSupported",
                                 "isHighestPhotoQualitySupported", "isGlobalToneMappingSupported", "isHighPhotoQualitySupported"})
        support.method(format_class_, selector, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    support.method(format_class_, "isVideoStabilizationModeSupported:", [](CpuState& s, GuestMemory&) { s.x[0] = s.x[2] == 0 ? 1 : 0; }, "B@:q");
    for (const auto* selector : {"videoFrameRateRangeForPortraitEffect", "videoFrameRateRangeForCenterStage", "videoFrameRateRangeForStudioLight",
                                 "videoFrameRateRangeForReactionEffectsInProgress", "videoFrameRateRangeForBackgroundReplacement"})
        support.method(format_class_, selector, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");
    // kAVCaptureColorSpace_sRGB 0.
    support.method(format_class_, "supportedColorSpaces", [array, sp](CpuState& s, GuestMemory&) {
        const auto zero = sp->cf.integer(0, 9);
        s.x[0] = array({zero});
        sp->release(zero);
    }, "@@:");
    support.method(format_class_, "videoMaxZoomFactor", [](CpuState& s, GuestMemory&) { s.v[0] = {std::bit_cast<std::uint64_t>(1.0), 0}; }, "d@:");
    support.method(format_class_, "videoFieldOfView", [](CpuState& s, GuestMemory&) { s.v[0] = {0, 0}; }, "f@:");
    support.method(format_class_, "dealloc", [this, sp](CpuState& s, GuestMemory&) {
        if (const auto f = sp->objc.find_host_state<FormatState>(s.x[0])) {
            sp->release(std::exchange(f->description, 0));
            sp->release(std::exchange(f->range, 0));
        }
        static_cast<void>(sp->objc.send_super(s.x[0], format_class_, sp->objc.selector("dealloc")));
    }, "v@:");
    const auto range_of = [sp](Address object) {
        const auto r = sp->objc.find_host_state<RangeState>(object);
        if (!r) unsupported("an AVFrameRateRange Alchemy did not make");
        return r;
    };
    support.method(range_class_, "minFrameRate", [range_of](CpuState& s, GuestMemory&) { s.v[0] = {std::bit_cast<std::uint64_t>(range_of(s.x[0])->min_rate), 0}; }, "d@:");
    support.method(range_class_, "maxFrameRate", [range_of](CpuState& s, GuestMemory&) { s.v[0] = {std::bit_cast<std::uint64_t>(range_of(s.x[0])->max_rate), 0}; }, "d@:");
    support.method(range_class_, "minFrameDuration", [self, range_of](CpuState& s, GuestMemory&) {
        const auto r = range_of(s.x[0]);
        self->write_time(s.x[8], r->max_rate > 0 ? make_with_seconds(1.0 / r->max_rate, 1'000'000) : time_invalid);
    }, "{?=qiIq}@:");
    support.method(range_class_, "maxFrameDuration", [self, range_of](CpuState& s, GuestMemory&) {
        const auto r = range_of(s.x[0]);
        self->write_time(s.x[8], r->min_rate > 0 ? make_with_seconds(1.0 / r->min_rate, 1'000'000) : time_invalid);
    }, "{?=qiIq}@:");

    // ---- AVCaptureDeviceDiscoverySession ----------------------------------------------------------
    support.method(discovery_class_, "discoverySessionWithDeviceTypes:mediaType:position:", [self, sp, matching](CpuState& s, GuestMemory&) {
        const auto object = sp->send_id(sp->send_id(self->discovery_class_, "alloc"), "init");
        auto found = matching(s.x[2], 0, s.x[3], static_cast<std::int64_t>(s.x[4]));
        for (const auto d : found) static_cast<void>(sp->retain(d));
        sp->objc.host_state<DiscoveryState>(object)->devices = std::move(found);
        s.x[0] = sp->send_id(object, "autorelease");
    }, "@@:@@q", true);
    support.method(discovery_class_, "devices", [sp, array](CpuState& s, GuestMemory&) { s.x[0] = array(sp->objc.host_state<DiscoveryState>(s.x[0])->devices); }, "@@:");
    support.method(discovery_class_, "supportedMultiCamDeviceSets", [array](CpuState& s, GuestMemory&) { s.x[0] = array({}); }, "@@:");
    support.method(discovery_class_, "dealloc", [this, sp](CpuState& s, GuestMemory&) {
        if (const auto d = sp->objc.find_host_state<DiscoveryState>(s.x[0]))
            for (const auto device : std::exchange(d->devices, {})) sp->release(device);
        static_cast<void>(sp->objc.send_super(s.x[0], discovery_class_, sp->objc.selector("dealloc")));
    }, "v@:");
    // +defaultDeviceWithDeviceType:mediaType:position: (the first match)
    support.method(device_class_, "defaultDeviceWithDeviceType:mediaType:position:", [sp, matching](CpuState& s, GuestMemory&) {
        const auto found = s.x[2] ? matching(0, s.x[2], s.x[3], static_cast<std::int64_t>(s.x[4])) : std::vector<Address>{};
        s.x[0] = found.empty() ? 0 : found.front();
    }, "@@:@@q", true);

    // ---- AVCaptureDeviceInput and AVCaptureInputPort ---------------------------------------------
    support.method(device_input_class_, "initWithDevice:error:", [self, sp](CpuState& s, GuestMemory&) {
        const auto d = s.x[2] ? sp->objc.find_host_state<DeviceState>(s.x[2]) : nullptr;
        const auto fail = [&](std::int64_t code, const std::string& why) {
            if (s.x[3]) self->bridge_->store_error(s.x[3], self->bridge_->error("AVFoundationErrorDomain", code, why), true);
            sp->release(s.x[0]);
            s.x[0] = 0;
        };
        if (!d) return fail(error_not_connected, "No capture device was given");
        if (self->status(d->media) == denied) return fail(error_not_authorized, "The host's privacy settings refuse " + d->name);
        const auto in = sp->objc.host_state<InputState>(s.x[0]);
        in->device = sp->retain(s.x[2]);
        in->port = sp->send_id(sp->send_id(self->port_class_, "alloc"), "init");
        sp->objc.host_state<PortState>(in->port)->input = s.x[0];
    }, "@@:@^@");
    support.method(device_input_class_, "deviceInputWithDevice:error:", [sp](CpuState& s, GuestMemory&) {
        const auto made = sp->send_id(sp->send_id(s.x[0], "alloc"), "initWithDevice:error:", {s.x[2], s.x[3]});
        s.x[0] = made ? sp->send_id(made, "autorelease") : 0;
    }, "@@:@^@", true);
    support.method(device_input_class_, "device", [sp](CpuState& s, GuestMemory&) { s.x[0] = sp->objc.host_state<InputState>(s.x[0])->device; }, "@@:");
    support.method(input_class, "ports", [sp, array](CpuState& s, GuestMemory&) {
        const auto in = sp->objc.find_host_state<InputState>(s.x[0]);
        s.x[0] = array(in && in->port ? std::vector<Address>{in->port} : std::vector<Address>{});
    }, "@@:");
    support.method(device_input_class_, "dealloc", [this, sp](CpuState& s, GuestMemory&) {
        if (const auto in = sp->objc.find_host_state<InputState>(s.x[0])) {
            sp->release(std::exchange(in->device, 0));
            sp->release(std::exchange(in->port, 0));
        }
        static_cast<void>(sp->objc.send_super(s.x[0], device_input_class_, sp->objc.selector("dealloc")));
    }, "v@:");
    const auto port_device = [sp](Address port) -> std::shared_ptr<DeviceState> {
        const auto p = sp->objc.find_host_state<PortState>(port);
        const auto in = p && p->input ? sp->objc.find_host_state<InputState>(p->input) : nullptr;
        return in && in->device ? sp->objc.find_host_state<DeviceState>(in->device) : nullptr;
    };
    support.method(port_class_, "input", [sp](CpuState& s, GuestMemory&) { s.x[0] = sp->objc.host_state<PortState>(s.x[0])->input; }, "@@:");
    support.method(port_class_, "mediaType", [sp, port_device](CpuState& s, GuestMemory&) {
        const auto d = port_device(s.x[0]);
        s.x[0] = d ? sp->cf.constant_string(d->media == Media::video ? "vide" : "soun") : 0;
    }, "@@:");
    support.method(port_class_, "formatDescription", [self, sp, port_device](CpuState& s, GuestMemory&) {
        const auto d = port_device(s.x[0]);
        std::lock_guard lock(self->mutex_);
        const auto f = d && d->active ? sp->objc.find_host_state<FormatState>(d->active) : nullptr;
        s.x[0] = f ? f->description : 0;
    }, "^{opaqueCMFormatDescription=}@:");
    support.method(port_class_, "isEnabled", [sp](CpuState& s, GuestMemory&) { s.x[0] = sp->objc.host_state<PortState>(s.x[0])->enabled ? 1 : 0; }, "B@:");
    support.method(port_class_, "setEnabled:", [sp](CpuState& s, GuestMemory&) { sp->objc.host_state<PortState>(s.x[0])->enabled = (s.x[2] & 0xff) != 0; }, "v@:B");

    // ---- AVCaptureOutput, AVCaptureConnection, the data outputs --------------------------------
    const auto output_of = [sp](Address object) {
        const auto o = sp->objc.find_host_state<OutputState>(object);
        if (!o) unsupported("an AVCaptureOutput Alchemy did not make");
        return o;
    };
    for (const auto& pair : {std::pair{video_output_class_, Media::video}, std::pair{audio_output_class_, Media::audio}}) {
        const Address cls = pair.first;
        const Media media = pair.second;
        support.method(cls, "init", [this, sp, cls, media](CpuState& s, GuestMemory&) {
            const auto object = sp->objc.send_super(s.x[0], cls, sp->objc.selector("init")).x0;
            if (object) {
                const auto o = sp->objc.host_state<OutputState>(object);
                o->media = media;
                o->connection = sp->send_id(sp->send_id(connection_class_, "alloc"), "init");
                sp->objc.host_state<ConnectionState>(o->connection)->output = object;
            }
            s.x[0] = object;
        }, "@@:");
        support.method(cls, "dealloc", [self, sp, cls](CpuState& s, GuestMemory&) {
            if (const auto o = sp->objc.find_host_state<OutputState>(s.x[0])) {
                Address queue{}, settings{}, connection{};
                {
                    std::lock_guard lock(self->mutex_);
                    o->alive = false;
                    queue = std::exchange(o->queue, 0);
                    settings = std::exchange(o->settings, 0);
                    connection = std::exchange(o->connection, 0);
                    o->delegate = 0;
                }
                if (queue) static_cast<void>(sp->call(libsystem, "_dispatch_release", {queue}));
                sp->release(settings);
                sp->release(connection);
            }
            static_cast<void>(sp->objc.send_super(s.x[0], cls, sp->objc.selector("dealloc")));
        }, "v@:");
        support.method(cls, "setSampleBufferDelegate:queue:", [self, sp, output_of, cls](CpuState& s, GuestMemory&) {
            if (s.x[2] && !s.x[3])
                throw std::runtime_error(std::string("Alchemy: -[") + (cls == self->video_output_class_ ? "AVCaptureVideoDataOutput" : "AVCaptureAudioDataOutput") +
                                         " setSampleBufferDelegate:queue:] needs a queue (NSInvalidArgumentException)");
            const auto o = output_of(s.x[0]);
            const auto queue = s.x[2] ? s.x[3] : 0;
            if (queue) static_cast<void>(sp->call(libsystem, "_dispatch_retain", {queue}));
            Address old{};
            {
                std::lock_guard lock(self->mutex_);
                o->delegate = s.x[2];
                old = std::exchange(o->queue, queue);
            }
            if (old) static_cast<void>(sp->call(libsystem, "_dispatch_release", {old}));
        }, "v@:@@");
        support.method(cls, "sampleBufferDelegate", [self, output_of](CpuState& s, GuestMemory&) {
            std::lock_guard lock(self->mutex_);
            s.x[0] = output_of(s.x[0])->delegate;
        }, "@@:");
        support.method(cls, "sampleBufferCallbackQueue", [self, output_of](CpuState& s, GuestMemory&) {
            std::lock_guard lock(self->mutex_);
            s.x[0] = output_of(s.x[0])->queue;
        }, "@@:");
        const auto settings_selector = media == Media::video ? "videoSettings" : "audioSettings";
        const auto set_selector = media == Media::video ? "setVideoSettings:" : "setAudioSettings:";
        support.method(cls, settings_selector, [self, output_of](CpuState& s, GuestMemory&) {
            std::lock_guard lock(self->mutex_);
            s.x[0] = output_of(s.x[0])->settings;
        }, "@@:");
        support.method(cls, set_selector, [self, sp, output_of](CpuState& s, GuestMemory&) {
            const auto copy = s.x[2] ? sp->cf.copy(s.x[2]) : 0;
            Address old{};
            {
                std::lock_guard lock(self->mutex_);
                old = std::exchange(output_of(s.x[0])->settings, copy);
            }
            sp->release(old);
        }, "v@:@");
        support.method(cls, "connections", [self, sp, output_of, array](CpuState& s, GuestMemory&) {
            const auto o = output_of(s.x[0]);
            const auto session = o->session ? sp->objc.find_host_state<SessionState>(o->session) : nullptr;
            s.x[0] = array(session && self->input_for(*session, o->media) ? std::vector<Address>{o->connection} : std::vector<Address>{});
        }, "@@:");
        support.method(cls, "connectionWithMediaType:", [self, sp, output_of](CpuState& s, GuestMemory&) {
            const auto o = output_of(s.x[0]);
            const auto type = s.x[2] ? sp->cf.utf8(s.x[2]) : std::string{};
            const auto session = o->session ? sp->objc.find_host_state<SessionState>(o->session) : nullptr;
            s.x[0] = session && type == (o->media == Media::video ? "vide" : "soun") && self->input_for(*session, o->media) ? o->connection : 0;
        }, "@@:@");
    }
    support.method(video_output_class_, "alwaysDiscardsLateVideoFrames", [output_of](CpuState& s, GuestMemory&) { s.x[0] = output_of(s.x[0])->discards_late ? 1 : 0; }, "B@:");
    support.method(video_output_class_, "setAlwaysDiscardsLateVideoFrames:", [output_of](CpuState& s, GuestMemory&) {
        output_of(s.x[0])->discards_late = (s.x[2] & 0xff) != 0;
    }, "v@:B");
    support.method(video_output_class_, "availableVideoCVPixelFormatTypes", [this, array](CpuState& s, GuestMemory&) {
        std::vector<Address> items;
        for (const auto format : {four("420v"), four("420f"), four("BGRA"), four("2vuy"), four("yuvs"), four("y420"), four("f420")})
            if (video_frames::writable(format)) items.push_back(bridge_->number(format));
        s.x[0] = array(items);
        for (const auto item : items) support_.release(item);
    }, "@@:");
    support.method(video_output_class_, "availableVideoCodecTypes", [array](CpuState& s, GuestMemory&) { s.x[0] = array({}); }, "@@:");

    const auto connection_of = [sp](Address object) {
        const auto c = sp->objc.find_host_state<ConnectionState>(object);
        if (!c) unsupported("an AVCaptureConnection Alchemy did not make");
        return c;
    };
    support.method(connection_class_, "output", [connection_of](CpuState& s, GuestMemory&) { s.x[0] = connection_of(s.x[0])->output; }, "@@:");
    support.method(connection_class_, "inputPorts", [self, sp, connection_of, array](CpuState& s, GuestMemory&) {
        const auto c = connection_of(s.x[0]);
        const auto o = c->output ? sp->objc.find_host_state<OutputState>(c->output) : nullptr;
        const auto session = o && o->session ? sp->objc.find_host_state<SessionState>(o->session) : nullptr;
        const auto input = session ? self->input_for(*session, o->media) : 0;
        const auto in = input ? sp->objc.find_host_state<InputState>(input) : nullptr;
        s.x[0] = array(in && in->port ? std::vector<Address>{in->port} : std::vector<Address>{});
    }, "@@:");
    support.method(connection_class_, "isEnabled", [connection_of](CpuState& s, GuestMemory&) { s.x[0] = connection_of(s.x[0])->enabled ? 1 : 0; }, "B@:");
    support.method(connection_class_, "setEnabled:", [connection_of](CpuState& s, GuestMemory&) { connection_of(s.x[0])->enabled = (s.x[2] & 0xff) != 0; }, "v@:B");
    support.method(connection_class_, "isActive", [connection_of](CpuState& s, GuestMemory&) { s.x[0] = connection_of(s.x[0])->enabled ? 1 : 0; }, "B@:");
    support.method(connection_class_, "isVideoMirroringSupported", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    support.method(connection_class_, "isVideoMirrored", [connection_of](CpuState& s, GuestMemory&) { s.x[0] = connection_of(s.x[0])->mirrored ? 1 : 0; }, "B@:");
    support.method(connection_class_, "setVideoMirrored:", [connection_of](CpuState& s, GuestMemory&) {
        const auto c = connection_of(s.x[0]);
        if (c->automatic_mirroring)
            throw std::runtime_error("Alchemy: -[AVCaptureConnection setVideoMirrored:] while automaticallyAdjustsVideoMirroring is YES (NSInvalidArgumentException)");
        c->mirrored = (s.x[2] & 0xff) != 0;
    }, "v@:B");
    support.method(connection_class_, "automaticallyAdjustsVideoMirroring", [connection_of](CpuState& s, GuestMemory&) { s.x[0] = connection_of(s.x[0])->automatic_mirroring ? 1 : 0; }, "B@:");
    support.method(connection_class_, "setAutomaticallyAdjustsVideoMirroring:", [connection_of](CpuState& s, GuestMemory&) {
        connection_of(s.x[0])->automatic_mirroring = (s.x[2] & 0xff) != 0;
    }, "v@:B");
    support.method(connection_class_, "isVideoOrientationSupported", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    // Frame durations belong to the device (activeVideoMin/MaxFrameDuration), as on macOS: the connection's
    // are not supported, and read as invalid.
    for (const auto* selector : {"isVideoMinFrameDurationSupported", "isVideoMaxFrameDurationSupported", "isVideoStabilizationSupported",
                                 "isVideoFieldModeSupported", "isCameraIntrinsicMatrixDeliverySupported", "isCameraIntrinsicMatrixDeliveryEnabled",
                                 "isVideoStabilizationEnabled", "enablesVideoStabilizationWhenAvailable"})
        support.method(connection_class_, selector, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    for (const auto* selector : {"videoMinFrameDuration", "videoMaxFrameDuration"})
        support.method(connection_class_, selector, [self](CpuState& s, GuestMemory&) { self->write_time(s.x[8], time_invalid); }, "{?=qiIq}@:");
    // AVCaptureVideoOrientationPortrait 1 (kept as set); stabilization off (AVCaptureVideoStabilizationModeOff 0).
    support.method(connection_class_, "videoOrientation", [connection_of](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<std::uint64_t>(connection_of(s.x[0])->orientation);
    }, "q@:");
    support.method(connection_class_, "setVideoOrientation:", [connection_of](CpuState& s, GuestMemory&) {
        connection_of(s.x[0])->orientation = static_cast<std::int64_t>(s.x[2]);
    }, "v@:q");
    for (const auto* selector : {"preferredVideoStabilizationMode", "activeVideoStabilizationMode"})
        support.method(connection_class_, selector, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "q@:");
    support.method(connection_class_, "setPreferredVideoStabilizationMode:", [](CpuState&, GuestMemory&) {}, "v@:q");
    // Rotation: only 0 degrees (the pictures are not rotated).
    support.method(connection_class_, "isVideoRotationAngleSupported:", [](CpuState& s, GuestMemory&) { s.x[0] = s.v[0][0] == 0 ? 1 : 0; }, "B@:d");
    support.method(connection_class_, "videoRotationAngle", [](CpuState& s, GuestMemory&) { s.v[0] = {0, 0}; }, "d@:");
    for (const auto* selector : {"videoScaleAndCropFactor", "videoMaxScaleAndCropFactor"})
        support.method(connection_class_, selector, [](CpuState& s, GuestMemory&) { s.v[0] = {std::bit_cast<std::uint64_t>(1.0), 0}; }, "d@:");
    support.method(connection_class_, "audioChannels", [array](CpuState& s, GuestMemory&) { s.x[0] = array({}); }, "@@:");

    // ---- AVCaptureSession ------------------------------------------------------------------------
    const auto session_of = [sp](Address object) { return sp->objc.host_state<SessionState>(object); };
    const auto kind_of = [sp](Address object, Address target) {
        CallRegs registers{};
        registers.x[2] = target;
        return (sp->objc.send(object, sp->objc.selector("isKindOfClass:"), registers).x0 & 0xff) != 0;
    };
    support.method(session_class_, "canAddInput:", [this, sp, session_of, kind_of](CpuState& s, GuestMemory&) {
        const auto session = session_of(s.x[0]);
        const auto in = s.x[2] && kind_of(s.x[2], device_input_class_) ? sp->objc.find_host_state<InputState>(s.x[2]) : nullptr;
        const auto d = in && in->device ? sp->objc.find_host_state<DeviceState>(in->device) : nullptr;
        // One input per media type, not twice.
        s.x[0] = d && std::find(session->inputs.begin(), session->inputs.end(), s.x[2]) == session->inputs.end() && !input_for(*session, d->media) ? 1 : 0;
    }, "B@:@");
    support.method(session_class_, "addInput:", [self, sp, session_of](CpuState& s, GuestMemory&) {
        if (!(sp->send(s.x[0], "canAddInput:", [&] { CallRegs r{}; r.x[2] = s.x[2]; return r; }()).x0 & 0xff))
            throw std::runtime_error("Alchemy: -[AVCaptureSession addInput:] cannot add this input (NSInvalidArgumentException)");
        const auto session = session_of(s.x[0]);
        std::lock_guard lock(self->mutex_);
        session->inputs.push_back(sp->retain(s.x[2]));
    }, "v@:@");
    support.method(session_class_, "removeInput:", [self, sp, session_of](CpuState& s, GuestMemory&) {
        const auto session = session_of(s.x[0]);
        {
            std::lock_guard lock(self->mutex_);
            const auto found = std::find(session->inputs.begin(), session->inputs.end(), s.x[2]);
            if (found == session->inputs.end()) return;
            session->inputs.erase(found);
        }
        sp->release(s.x[2]);
    }, "v@:@");
    support.method(session_class_, "inputs", [session_of, array](CpuState& s, GuestMemory&) { s.x[0] = array(session_of(s.x[0])->inputs); }, "@@:");
    support.method(session_class_, "canAddOutput:", [sp, session_of, kind_of, output_class](CpuState& s, GuestMemory&) {
        const auto session = session_of(s.x[0]);
        const auto o = s.x[2] && kind_of(s.x[2], output_class) ? sp->objc.find_host_state<OutputState>(s.x[2]) : nullptr;
        const bool known = s.x[2] && kind_of(s.x[2], output_class);
        s.x[0] = known && std::find(session->outputs.begin(), session->outputs.end(), s.x[2]) == session->outputs.end() && (!o || !o->session) ? 1 : 0;
    }, "B@:@");
    support.method(session_class_, "addOutput:", [self, sp, session_of](CpuState& s, GuestMemory&) {
        if (!(sp->send(s.x[0], "canAddOutput:", [&] { CallRegs r{}; r.x[2] = s.x[2]; return r; }()).x0 & 0xff))
            throw std::runtime_error("Alchemy: -[AVCaptureSession addOutput:] cannot add this output (NSInvalidArgumentException)");
        const auto session = session_of(s.x[0]);
        std::lock_guard lock(self->mutex_);
        session->outputs.push_back(sp->retain(s.x[2]));
        if (const auto o = sp->objc.find_host_state<OutputState>(s.x[2])) o->session = s.x[0];
    }, "v@:@");
    support.method(session_class_, "removeOutput:", [self, sp, session_of](CpuState& s, GuestMemory&) {
        const auto session = session_of(s.x[0]);
        {
            std::lock_guard lock(self->mutex_);
            const auto found = std::find(session->outputs.begin(), session->outputs.end(), s.x[2]);
            if (found == session->outputs.end()) return;
            session->outputs.erase(found);
            if (const auto o = sp->objc.find_host_state<OutputState>(s.x[2])) o->session = 0;
        }
        sp->release(s.x[2]);
    }, "v@:@");
    support.method(session_class_, "outputs", [session_of, array](CpuState& s, GuestMemory&) { s.x[0] = array(session_of(s.x[0])->outputs); }, "@@:");
    support.method(session_class_, "connections", [sp, session_of, array](CpuState& s, GuestMemory&) {
        std::vector<Address> items;
        for (const auto output : session_of(s.x[0])->outputs) {
            const auto list = sp->send_id(output, "connections");
            for (std::uint64_t i = 0, n = list ? sp->cf.length(list) : 0; i < n; ++i) items.push_back(sp->cf.element(list, i));
        }
        s.x[0] = array(items);
    }, "@@:");
    support.method(session_class_, "beginConfiguration", [session_of](CpuState& s, GuestMemory&) { ++session_of(s.x[0])->configuring; }, "v@:");
    support.method(session_class_, "commitConfiguration", [session_of](CpuState& s, GuestMemory&) {
        const auto session = session_of(s.x[0]);
        if (session->configuring == 0) throw std::runtime_error("Alchemy: -[AVCaptureSession commitConfiguration] without beginConfiguration (NSGenericException)");
        --session->configuring;
    }, "v@:");
    support.method(session_class_, "sessionPreset", [sp, session_of](CpuState& s, GuestMemory&) {
        const auto session = session_of(s.x[0]);
        s.x[0] = session->preset ? session->preset : sp->cf.constant_string("AVCaptureSessionPresetHigh");
    }, "@@:");
    support.method(session_class_, "canSetSessionPreset:", [sp, session_of](CpuState& s, GuestMemory&) {
        const auto session = session_of(s.x[0]);
        if (!s.x[2]) { s.x[0] = 0; return; }
        bool ok = true;
        for (const auto input : session->inputs)
            if (const auto device = sp->send_id(input, "device")) ok = ok && (sp->send(device, "supportsAVCaptureSessionPreset:", [&] { CallRegs r{}; r.x[2] = s.x[2]; return r; }()).x0 & 0xff);
        s.x[0] = ok ? 1 : 0;
    }, "B@:@");
    support.method(session_class_, "setSessionPreset:", [sp, session_of](CpuState& s, GuestMemory&) {
        if (!s.x[2]) throw std::runtime_error("Alchemy: -[AVCaptureSession setSessionPreset:] with nil (NSInvalidArgumentException)");
        const auto session = session_of(s.x[0]);
        sp->release(std::exchange(session->preset, sp->cf.copy(s.x[2])));
    }, "v@:@");
    support.method(session_class_, "startRunning", [self](CpuState& s, GuestMemory&) { self->start(s.x[0]); }, "v@:");
    support.method(session_class_, "stopRunning", [self](CpuState& s, GuestMemory&) { self->stop(s.x[0]); }, "v@:");
    support.method(session_class_, "isRunning", [self, session_of](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        s.x[0] = session_of(s.x[0])->running ? 1 : 0;
    }, "B@:");
    support.method(session_class_, "isInterrupted", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    support.method(session_class_, "dealloc", [this, self, sp](CpuState& s, GuestMemory&) {
        if (const auto session = sp->objc.find_host_state<SessionState>(s.x[0])) {
            self->stop(s.x[0]);
            for (const auto output : session->outputs)
                if (const auto o = sp->objc.find_host_state<OutputState>(output)) o->session = 0;
            for (const auto object : std::exchange(session->inputs, {})) sp->release(object);
            for (const auto object : std::exchange(session->outputs, {})) sp->release(object);
            sp->release(std::exchange(session->preset, 0));
        }
        static_cast<void>(sp->objc.send_super(s.x[0], session_class_, sp->objc.selector("dealloc")));
    }, "v@:");

}

} // namespace

void set_capture_backend_factory(std::function<std::unique_ptr<capture::Backend>()> factory) { backend_factory() = std::move(factory); }

void install_av_capture(Support& support) { std::make_shared<Capture>(support)->install(); }

} // namespace alchemy::gui::completion_kit::media
