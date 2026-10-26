#include "alchemy/darwin/media/audio_file.hpp"
#include "alchemy/darwin/media/hal_io.hpp"
#include "alchemy/darwin/media/resampler.hpp"

#include "alchemy/darwin/security/bridge.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// AVSampleBufferDisplayLayer, AVSampleBufferAudioRenderer and AVSampleBufferRenderSynchronizer
// (AVQueuedSampleBufferRendering): sample buffers in, pictures on a layer and sound on the output
// device out, on a synchronizer's clock.
// - The display layer takes CMSampleBuffers of image buffers (CVPixelBuffers), or of AV1, VP9 or
//   VP8 samples, which it decodes through a VTDecompressionSession. A picture marked
//   kCMSampleAttachmentKey_DisplayImmediately, or any picture of a layer no synchronizer
//   controls, is shown at once; the others when the synchronizer's time reaches them. Shown
//   pictures become the layer's contents as IOSurface-backed BGRA (others are converted with a
//   VTPixelTransferSession). Other compressed formats fail the layer
//   (AVQueuedSampleBufferRenderingStatusFailed, AVErrorDecoderNotFound).
// - The audio renderer takes linear PCM sample buffers and plays them on the default output device
//   while its synchronizer runs at rate 1, from the synchronizer's time (earlier audio is dropped,
//   gaps are silent), with its volume and mute. Compressed audio fails it.
// - The synchronizer's clock runs at its rate (setRate:, setRate:time:); periodic and boundary time
//   observers fire on their queues. requestMediaDataWhenReadyOnQueue:usingBlock: calls the block on
//   its queue while the renderer wants more (fewer than 8 pictures queued, under a second of audio).
// - CMTimebase is not implemented (no app imports it): -timebase, -controlTimebase and
//   -setControlTimebase: stop with an explicit message.
namespace alchemy::gui::completion_kit::media {
namespace {

constexpr std::string_view av_foundation = "/System/Library/Frameworks/AVFoundation.framework/Versions/A/AVFoundation";
constexpr std::string_view core_media = "/System/Library/Frameworks/CoreMedia.framework/Versions/A/CoreMedia";
constexpr std::string_view core_video = "/System/Library/Frameworks/CoreVideo.framework/Versions/A/CoreVideo";
constexpr std::string_view core_foundation = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr std::string_view video_toolbox = "/System/Library/Frameworks/VideoToolbox.framework/Versions/A/VideoToolbox";
constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";
constexpr double tick_seconds = 1.0 / 60;
constexpr std::size_t picture_queue_limit = 8;
constexpr double audio_queue_seconds = 1.0;
constexpr std::int64_t av_error_decoder_not_found = -11833, av_error_unknown = -11800;

struct Time { std::int64_t value{}; std::int32_t timescale{}; std::uint32_t flags{}; std::int64_t epoch{}; };
Time read_time(GuestMemory& m, Address at) {
    return {static_cast<std::int64_t>(m.read64(at)), static_cast<std::int32_t>(m.read32(at + 8)), m.read32(at + 12), static_cast<std::int64_t>(m.read64(at + 16))};
}
void write_time(GuestMemory& m, Address at, const Time& t) {
    m.write64(at, static_cast<std::uint64_t>(t.value));
    m.write32(at + 8, static_cast<std::uint32_t>(t.timescale));
    m.write32(at + 12, t.flags);
    m.write64(at + 16, static_cast<std::uint64_t>(t.epoch));
}
double seconds_of(const Time& t) { return (t.flags & 1) && t.timescale > 0 ? static_cast<double>(t.value) / t.timescale : 0.0; }
Time time_of(double seconds) { return {static_cast<std::int64_t>(std::llround(seconds * 600000)), 600000, 1, 0}; }

struct Request { Address queue{}, block{}; bool pending{}; };   // requestMediaDataWhenReadyOnQueue (queue and block retained)

struct Picture { Address image{}; double time{}; bool immediately{}; };   // a retained CVPixelBuffer

// A display layer's state: in a side map, as a CALayer already carries QuartzCore's host state.
struct DisplayState {
    std::deque<Picture> queue;
    Address shown{};                           // the retained BGRA IOSurface pixel buffer on the layer
    Address transfer{};                        // VTPixelTransferSession (+1) or 0
    Address decoder{}, decoder_format{};       // VTDecompressionSession (+1) and its format (retained)
    std::int64_t status{};                     // AVQueuedSampleBufferRenderingStatus
    Address error{}, gravity{};                // retained
    bool prevents_sleep{true};
    Address synchronizer{};                    // not retained
    Request request;
};

struct AudioState final : ObjectPayload {
    std::shared_ptr<std::mutex> mutex = std::make_shared<std::mutex>();
    struct Shared {
        std::deque<float> samples;             // interleaved, from `start` seconds
        double start{-1};                      // the time of the first queued frame (-1: nothing)
        double rate{};
        std::uint32_t channels{};
        float gain{1};
        bool playing{};
        double play_from{};                    // the synchronizer's time when the device started taking frames
        std::unique_ptr<Resampler> resampler;
    };
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();
    float volume{1};
    bool muted{};
    std::int64_t status{};
    Address error{}, pitch_algorithm{};
    Address synchronizer{};                    // not retained
    Request request;
    std::shared_ptr<hal_io::DeviceIo> io;
};

struct Observer {
    Address token{};
    bool periodic{};
    double interval{}, next_fire{};
    std::vector<double> times;
    Address queue{}, block{};
};

struct SyncState final : ObjectPayload {
    std::vector<Address> renderers;            // retained
    double rate{}, position{}, last_tick{};
    std::chrono::steady_clock::time_point anchor{};
    std::vector<Observer> observers;
    bool delays{true};
};

class Renderers : public std::enable_shared_from_this<Renderers> {
public:
    explicit Renderers(Support& support) : support_(support), bridge_(security::bridge()) {}
    void install();

private:
    template <class T> std::shared_ptr<T> state(Address self) { return support_.objc.host_state<T>(self); }
    std::shared_ptr<DisplayState> display(Address layer) {
        auto& slot = displays_[layer];
        if (!slot) slot = std::make_shared<DisplayState>();
        return slot;
    }
    Address retain(Address object) { return object ? support_.send_id(object, "retain") : 0; }
    void release(Address object) { if (object) static_cast<void>(support_.send_id(object, "release")); }
    double sync_time(Address sync) {
        const auto s = state<SyncState>(sync);
        if (s->rate == 0) return s->position;
        return s->position + std::chrono::duration<double>(std::chrono::steady_clock::now() - s->anchor).count() * s->rate;
    }
    bool is_display(Address object) const { return displays_.count(object) != 0; }

    void enqueue_display(Address layer, Address sample);
    void decode_into(Address layer, DisplayState& d, Address sample);
    void present(Address layer, DisplayState& d, Address image);
    void update_display(Address layer);
    void fail_display(DisplayState& d, std::int64_t code, std::string_view why);
    void enqueue_audio(Address renderer, Address sample);
    bool start_audio(Address renderer);
    void stop_audio(Address renderer);
    static void render(AudioState::Shared& a, float* mix, std::uint32_t frames, std::uint32_t channels, double device_rate);
    void serve(Address renderer, Request& request, bool ready);
    bool display_ready(const DisplayState& d) const { return d.queue.size() < picture_queue_limit && d.status != 2; }
    bool audio_ready(Address renderer);
    void set_rate(Address sync, double rate, std::optional<double> time);
    void fire_observers(Address sync, double from, double to);
    void invoke_time_block(Address queue, Address block, double seconds);
    void schedule();
    void tick();

    Support& support_;
    std::shared_ptr<security::Bridge> bridge_;
    std::map<Address, std::shared_ptr<DisplayState>> displays_;
    Address layer_class_{}, audio_class_{}, sync_class_{}, token_class_{}, tick_function_{}, picture_callback_{};
    std::set<Address> running_;                // synchronizers at a non-zero rate (retained)
    bool scheduled_{};
};

void Renderers::fail_display(DisplayState& d, std::int64_t code, std::string_view why) {
    d.status = 2;   // AVQueuedSampleBufferRenderingStatusFailed
    if (!d.error) d.error = bridge_->error("AVFoundationErrorDomain", code, why);
}

// The image as IOSurface-backed BGRA (itself, or converted) on the layer.
void Renderers::present(Address layer, DisplayState& d, Address image) {
    Address shown{};
    const auto format = static_cast<std::uint32_t>(support_.call(core_video, "_CVPixelBufferGetPixelFormatType", {image}));
    if (format == 0x42475241 && support_.call(core_video, "_CVPixelBufferGetIOSurface", {image})) {
        shown = retain(image);
    } else {
        auto& m = support_.memory();
        const auto width = support_.call(core_video, "_CVPixelBufferGetWidth", {image});
        const auto height = support_.call(core_video, "_CVPixelBufferGetHeight", {image});
        const auto format_key = support_.cf.string("PixelFormatType"), surface_key = support_.cf.string("IOSurfaceProperties");
        const auto bgra = support_.cf.integer(0x42475241), empty = support_.cf.dictionary(std::span<const std::pair<std::uint64_t, std::uint64_t>>{});
        const std::array<std::pair<std::uint64_t, std::uint64_t>, 2> entries{{{format_key, bgra}, {surface_key, empty}}};
        const auto attributes = support_.cf.dictionary(entries);
        for (const auto object : {format_key, surface_key, bgra, empty}) support_.release(object);
        const auto out = support_.process.allocate_guest(8, 8);
        m.write64(out, 0);
        const auto created = static_cast<std::int32_t>(support_.call(core_video, "_CVPixelBufferCreate", {0, width, height, 0x42475241, attributes, out}));
        shown = m.read64(out);
        support_.release(attributes);
        if (!d.transfer) {
            m.write64(out, 0);
            static_cast<void>(support_.call(video_toolbox, "_VTPixelTransferSessionCreate", {0, out}));
            d.transfer = m.read64(out);
        }
        support_.process.release_guest(out);
        if (created || !shown || !d.transfer ||
            static_cast<std::int32_t>(support_.call(video_toolbox, "_VTPixelTransferSessionTransferImage", {d.transfer, image, shown}))) {
            if (shown) support_.release(shown);
            fail_display(d, av_error_unknown, "The picture's pixel format cannot be shown");
            return;
        }
    }
    const auto surface = support_.call(core_video, "_CVPixelBufferGetIOSurface", {shown});
    CallRegs r{};
    r.x[2] = surface;
    static_cast<void>(support_.send(layer, "setContents:", r));
    if (d.shown) support_.release(d.shown);
    d.shown = shown;
    if (d.status == 0) d.status = 1;   // AVQueuedSampleBufferRenderingStatusRendering
}

// Compressed samples through a decompression session; its pictures join the queue.
void Renderers::decode_into(Address layer, DisplayState& d, Address sample) {
    auto& m = support_.memory();
    const auto format = support_.call(core_media, "_CMSampleBufferGetFormatDescription", {sample});
    if (!d.decoder || !support_.cf.equal(format, d.decoder_format)) {
        if (d.decoder) {
            static_cast<void>(support_.call(video_toolbox, "_VTDecompressionSessionInvalidate", {d.decoder}));
            support_.release(std::exchange(d.decoder, 0));
            support_.release(std::exchange(d.decoder_format, 0));
        }
        const auto record = support_.process.allocate_guest(24, 8);
        m.write64(record, picture_callback_);
        m.write64(record + 8, layer);
        m.write64(record + 16, 0);
        const auto result = static_cast<std::int32_t>(support_.call(video_toolbox, "_VTDecompressionSessionCreate", {0, format, 0, 0, record, record + 16}));
        d.decoder = m.read64(record + 16);
        support_.process.release_guest(record);
        if (result || !d.decoder) { fail_display(d, av_error_decoder_not_found, "No decoder exists under Alchemy for the samples' format"); return; }
        d.decoder_format = retain(format);
    }
    const auto info = support_.process.allocate_guest(8, 8);
    static_cast<void>(support_.call(video_toolbox, "_VTDecompressionSessionDecodeFrame", {d.decoder, sample, 0, 0, info}));
    support_.process.release_guest(info);
}

void Renderers::enqueue_display(Address layer, Address sample) {
    auto d = display(layer);
    if (d->status == 2 || !sample) return;
    const auto image = support_.call(core_media, "_CMSampleBufferGetImageBuffer", {sample});
    if (!image) { decode_into(layer, *d, sample); update_display(layer); return; }
    Picture p;
    p.image = retain(image);
    auto& m = support_.memory();
    const auto at = support_.process.allocate_guest(24, 8);
    CallRegs r{};
    r.x[0] = sample;
    r.indirect_result = at;
    static_cast<void>(support_.call(core_media, "_CMSampleBufferGetPresentationTimeStamp", r));
    p.time = seconds_of(read_time(m, at));
    support_.process.release_guest(at);
    const auto attachments = support_.call(core_media, "_CMSampleBufferGetSampleAttachmentsArray", {sample, 0});
    const auto first = attachments ? support_.cf.element(attachments, 0) : 0;
    if (first) {
        const auto key = support_.cf.string("DisplayImmediately");
        const auto value = support_.cf.dictionary_value(first, key);
        support_.release(key);
        p.immediately = value && value == support_.cf.boolean(true);
    }
    d->queue.push_back(p);
    update_display(layer);
}

// Shows the newest picture that is due: marked to show at once, or (with a synchronizer) at or
// before its time; without a synchronizer every picture is due.
void Renderers::update_display(Address layer) {
    auto d = display(layer);
    const bool clocked = d->synchronizer != 0;
    const double now = clocked ? sync_time(d->synchronizer) : 0;
    Address due{};
    while (!d->queue.empty() && (!clocked || d->queue.front().immediately || d->queue.front().time <= now + 1e-6)) {
        if (due) release(due);
        due = d->queue.front().image;
        d->queue.pop_front();
    }
    if (due) {
        present(layer, *d, due);
        release(due);
    }
    serve(layer, d->request, display_ready(*d));
}

void Renderers::serve(Address renderer, Request& request, bool ready) {
    if (!request.block || !ready || request.pending) return;
    request.pending = true;
    const auto self = shared_from_this();
    const auto kept = retain(renderer);
    bridge_->dispatch_async(request.queue ? request.queue : bridge_->main_queue(), [self, kept] {
        Request* r = self->is_display(kept) ? &self->display(kept)->request
                                            : &self->state<AudioState>(kept)->request;
        r->pending = false;
        if (r->block) static_cast<void>(self->bridge_->invoke_block(r->block, {}));
        self->release(kept);
    });
}

bool Renderers::audio_ready(Address renderer) {
    const auto a = state<AudioState>(renderer);
    std::lock_guard lock(*a->mutex);
    const auto& sh = *a->shared;
    return a->status != 2 && (!sh.channels || sh.samples.size() / sh.channels < sh.rate * audio_queue_seconds);
}

void Renderers::enqueue_audio(Address renderer, Address sample) {
    auto a = state<AudioState>(renderer);
    if (a->status == 2 || !sample) return;
    auto& m = support_.memory();
    const auto description = support_.call(core_media, "_CMSampleBufferGetFormatDescription", {sample});
    const auto asbd_at = description ? support_.call(core_media, "_CMAudioFormatDescriptionGetStreamBasicDescription", {description}) : 0;
    std::vector<std::uint8_t> asbd(40);
    if (asbd_at) m.read_bytes(asbd_at, asbd);
    std::string why;
    const auto format = asbd_at ? audio_file::pcm_from_asbd(asbd, why) : std::nullopt;
    if (!format) {
        a->status = 2;
        if (!a->error) a->error = bridge_->error("AVFoundationErrorDomain", av_error_decoder_not_found, "The audio renderer plays linear PCM only");
        return;
    }
    const auto frames = static_cast<std::size_t>(support_.call(core_media, "_CMSampleBufferGetNumSamples", {sample}));
    const auto block = support_.call(core_media, "_CMSampleBufferGetDataBuffer", {sample});
    const auto length = block ? support_.call(core_media, "_CMBlockBufferGetDataLength", {block}) : 0;
    const std::uint32_t planes = format->non_interleaved ? format->channels : 1;
    if (!frames || !length || length < frames * format->frame_bytes() * planes) return;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    const auto scratch = support_.process.allocate_guest(length, 16);
    static_cast<void>(support_.call(core_media, "_CMBlockBufferCopyDataBytes", {block, 0, length, scratch}));
    m.read_bytes(scratch, bytes);
    support_.process.release_guest(scratch);
    std::vector<std::span<const std::uint8_t>> buffers;
    const auto plane = frames * format->frame_bytes();
    for (std::uint32_t i = 0; i < planes; ++i) buffers.emplace_back(bytes.data() + i * plane, plane);
    const auto samples = audio_file::decode(*format, buffers, frames);
    const auto at = support_.process.allocate_guest(24, 8);
    CallRegs r{};
    r.x[0] = sample;
    r.indirect_result = at;
    static_cast<void>(support_.call(core_media, "_CMSampleBufferGetPresentationTimeStamp", r));
    const auto time = seconds_of(read_time(m, at));
    support_.process.release_guest(at);
    {
        std::lock_guard lock(*a->mutex);
        auto& sh = *a->shared;
        if (sh.channels != format->channels || sh.rate != format->rate) {
            sh.samples.clear();
            sh.start = -1;
            sh.channels = format->channels;
            sh.rate = format->rate;
            sh.resampler.reset();
        }
        if (sh.start < 0) sh.start = time;
        // Samples continue the queue; a gap before this buffer is silence.
        const auto end = sh.start + static_cast<double>(sh.samples.size() / sh.channels) / sh.rate;
        if (time > end + 0.5 / sh.rate) sh.samples.insert(sh.samples.end(), static_cast<std::size_t>((time - end) * sh.rate) * sh.channels, 0.0f);
        for (const auto v : samples) sh.samples.push_back(static_cast<float>(v));
    }
    if (a->status == 0) a->status = 1;
}

// On the device's I/O thread: the queued audio from the synchronizer's time, resampled.
void Renderers::render(AudioState::Shared& a, float* mix, std::uint32_t frames, std::uint32_t channels, double device_rate) {
    if (!a.playing || !a.channels || a.start < 0) return;
    // Drop what lies before the synchronizer's time when playing started.
    if (a.play_from > a.start) {
        const auto drop = std::min<std::size_t>(a.samples.size() / a.channels, static_cast<std::size_t>((a.play_from - a.start) * a.rate));
        a.samples.erase(a.samples.begin(), a.samples.begin() + static_cast<std::ptrdiff_t>(drop * a.channels));
        a.start += static_cast<double>(drop) / a.rate;
        a.play_from = a.start;
    }
    if (!a.resampler) a.resampler = std::make_unique<Resampler>(a.rate, device_rate, a.channels);
    std::vector<double> converted(static_cast<std::size_t>(frames) * a.channels);
    a.resampler->render(converted.data(), frames, [&](double* out, std::size_t count) {
        const auto values = count * a.channels;
        const auto have = std::min(values, a.samples.size());
        std::copy_n(a.samples.begin(), have, out);
        std::fill(out + have, out + values, 0.0);
        a.samples.erase(a.samples.begin(), a.samples.begin() + static_cast<std::ptrdiff_t>(have));
        a.start += static_cast<double>(have / a.channels) / a.rate;
    });
    for (std::uint32_t f = 0; f < frames; ++f)
        for (std::uint32_t c = 0; c < channels; ++c)
            mix[static_cast<std::size_t>(f) * channels + c] +=
                static_cast<float>(converted[static_cast<std::size_t>(f) * a.channels + std::min(c, a.channels - 1)]) * a.gain;
}

bool Renderers::start_audio(Address renderer) {
    auto a = state<AudioState>(renderer);
    if (!a->io) a->io = hal_device_io(hal_default_output());
    if (!a->io) return false;
    {
        std::lock_guard lock(*a->mutex);
        a->shared->playing = true;
        a->shared->play_from = a->synchronizer ? sync_time(a->synchronizer) : 0;
        a->shared->gain = a->muted ? 0.0f : a->volume;
    }
    if (a->io->running(renderer)) return true;
    const auto shared = a->shared;
    const auto mutex = a->mutex;
    const auto channels = a->io->channels();
    const auto device_rate = a->io->rate();
    return a->io->start(renderer, [shared, mutex, channels, device_rate](float* mix, std::uint32_t frames, const hal_io::Timing&) {
        std::lock_guard lock(*mutex);
        render(*shared, mix, frames, channels, device_rate);
    }, hal_buffer_frames(hal_default_output())) == 0;
}

void Renderers::stop_audio(Address renderer) {
    auto a = state<AudioState>(renderer);
    {
        std::lock_guard lock(*a->mutex);
        a->shared->playing = false;
    }
    if (a->io) a->io->stop(renderer);
}

void Renderers::set_rate(Address sync, double rate, std::optional<double> time) {
    auto s = state<SyncState>(sync);
    s->position = time ? *time : sync_time(sync);
    s->anchor = std::chrono::steady_clock::now();
    s->last_tick = s->position;
    s->rate = rate;
    for (const auto r : s->renderers) {
        if (is_display(r)) { update_display(r); continue; }
        if (rate == 1.0) start_audio(r);
        else stop_audio(r);
    }
    if (rate != 0) {
        if (running_.insert(sync).second) static_cast<void>(retain(sync));
        schedule();
    }
}

void Renderers::invoke_time_block(Address queue, Address block, double seconds) {
    const auto bridge = bridge_;
    auto* sp = &support_;
    const auto copy = bridge_->block_copy(block);
    bridge_->dispatch_async(queue ? queue : bridge_->main_queue(), [bridge, sp, copy, seconds] {
        const auto at = sp->process.allocate_guest(24, 8);
        write_time(sp->memory(), at, time_of(seconds));
        static_cast<void>(bridge->invoke_block(copy, {at}));   // CMTime by reference
        sp->process.release_guest(at);
        bridge->block_release(copy);
    });
}

void Renderers::fire_observers(Address sync, double from, double to) {
    for (auto& o : state<SyncState>(sync)->observers) {
        if (o.periodic) {
            if (o.interval > 0 && to + 1e-9 >= o.next_fire) {
                invoke_time_block(o.queue, o.block, to);
                o.next_fire = (std::floor(to / o.interval) + 1) * o.interval;
            }
        } else {
            for (const auto t : o.times)
                if (t > from && t <= to) invoke_time_block(o.queue, o.block, t);
        }
    }
}

void Renderers::schedule() {
    if (scheduled_ || running_.empty()) return;
    scheduled_ = true;
    const auto when = support_.call(libsystem, "_dispatch_time", {0, static_cast<Address>(tick_seconds * 1e9)});
    static_cast<void>(support_.call(libsystem, "_dispatch_after_f", {when, bridge_->main_queue(), 0, tick_function_}));
}

void Renderers::tick() {
    scheduled_ = false;
    std::vector<Address> stopped;
    for (const auto sync : std::vector<Address>(running_.begin(), running_.end())) {
        auto s = state<SyncState>(sync);
        if (s->rate == 0) { stopped.push_back(sync); continue; }
        const auto t = sync_time(sync);
        for (const auto r : s->renderers) {
            if (is_display(r)) update_display(r);
            else serve(r, state<AudioState>(r)->request, audio_ready(r));
        }
        fire_observers(sync, s->last_tick, t);
        s->last_tick = t;
    }
    for (const auto sync : stopped)
        if (running_.erase(sync)) release(sync);
    schedule();
}

void Renderers::install() {
    auto& support = support_;
    const auto self = shared_from_this();
    const auto declare = [&](const char* name, const char* superclass) { return support.declare_class(name, superclass, av_foundation); };
    layer_class_ = declare("AVSampleBufferDisplayLayer", "CALayer");
    audio_class_ = declare("AVSampleBufferAudioRenderer", "NSObject");
    sync_class_ = declare("AVSampleBufferRenderSynchronizer", "NSObject");
    token_class_ = declare("__AlchemyAVSynchronizerTimeObserver", "NSObject");
    tick_function_ = support.process.install_function("__AlchemyAVSampleBufferTick", [self](CpuState&, GuestMemory&) { self->tick(); }, std::string(av_foundation));
    // The VTDecompressionOutputCallback of display layers decoding compressed samples (refcon: the layer).
    picture_callback_ = support.process.install_function("__AlchemyAVSampleBufferDisplayLayerPicture", [self](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[4] || !self->is_display(s.x[0])) return;
        Picture p;
        p.image = self->retain(s.x[4]);
        p.time = seconds_of(read_time(m, s.x[5]));
        self->display(s.x[0])->queue.push_back(p);
    }, std::string(av_foundation));
    const auto layer = layer_class_, audio = audio_class_, sync = sync_class_;
    const auto no_timebase = [](const char* what) {
        return [what](CpuState&, GuestMemory&) { unsupported(std::string(what) + " (CMTimebase is not implemented under Alchemy)"); };
    };

    // ---- AVSampleBufferDisplayLayer ---------------------------------------------------------------
    support.method(layer, "enqueueSampleBuffer:", [self](CpuState& s, GuestMemory&) { self->enqueue_display(s.x[0], s.x[2]); }, "v@:^{opaqueCMSampleBuffer=}");
    // -flush drops what is queued; -flushAndRemoveImage also clears the layer.
    support.method(layer, "flush", [self](CpuState& s, GuestMemory&) {
        auto d = self->display(s.x[0]);
        for (const auto& p : std::exchange(d->queue, {})) self->release(p.image);
        if (d->status == 2) { d->status = 0; self->release(std::exchange(d->error, 0)); }
    }, "v@:");
    support.method(layer, "flushAndRemoveImage", [self](CpuState& s, GuestMemory&) {
        auto d = self->display(s.x[0]);
        for (const auto& p : std::exchange(d->queue, {})) self->release(p.image);
        static_cast<void>(self->support_.send_id(s.x[0], "setContents:", {0}));
        self->release(std::exchange(d->shown, 0));
    }, "v@:");
    support.method(layer, "isReadyForMoreMediaData", [self](CpuState& s, GuestMemory&) { s.x[0] = self->display_ready(*self->display(s.x[0])) ? 1 : 0; }, "B@:");
    support.method(layer, "requestMediaDataWhenReadyOnQueue:usingBlock:", [self](CpuState& s, GuestMemory&) {
        auto d = self->display(s.x[0]);
        if (d->request.queue) static_cast<void>(self->support_.call(libsystem, "_dispatch_release", {d->request.queue}));
        if (d->request.block) self->bridge_->block_release(d->request.block);
        d->request.queue = s.x[2];
        if (s.x[2]) static_cast<void>(self->support_.call(libsystem, "_dispatch_retain", {s.x[2]}));
        d->request.block = s.x[3] ? self->bridge_->block_copy(s.x[3]) : 0;
        self->serve(s.x[0], d->request, self->display_ready(*d));
    }, "v@:@@?");
    support.method(layer, "stopRequestingMediaData", [self](CpuState& s, GuestMemory&) {
        auto d = self->display(s.x[0]);
        if (d->request.block) self->bridge_->block_release(std::exchange(d->request.block, 0));
        if (d->request.queue) static_cast<void>(self->support_.call(libsystem, "_dispatch_release", {std::exchange(d->request.queue, 0)}));
    }, "v@:");
    support.method(layer, "status", [self](CpuState& s, GuestMemory&) { s.x[0] = static_cast<std::uint64_t>(self->display(s.x[0])->status); }, "q@:");
    support.method(layer, "error", [self](CpuState& s, GuestMemory&) { s.x[0] = self->display(s.x[0])->error; }, "@@:");
    support.method(layer, "requiresFlushToResumeDecoding", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    support.method(layer, "isOutputObscuredDueToInsufficientExternalProtection", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    support.method(layer, "hasSufficientMediaDataForReliablePlaybackStart", [self](CpuState& s, GuestMemory&) {
        const auto d = self->display(s.x[0]);
        s.x[0] = !d->queue.empty() || d->shown ? 1 : 0;
    }, "B@:");
    support.method(layer, "preventsDisplaySleepDuringVideoPlayback", [self](CpuState& s, GuestMemory&) { s.x[0] = self->display(s.x[0])->prevents_sleep ? 1 : 0; }, "B@:");
    support.method(layer, "setPreventsDisplaySleepDuringVideoPlayback:", [self](CpuState& s, GuestMemory&) { self->display(s.x[0])->prevents_sleep = (s.x[2] & 0xff) != 0; }, "v@:B");
    support.method(layer, "videoGravity", [self](CpuState& s, GuestMemory&) {
        const auto d = self->display(s.x[0]);
        s.x[0] = d->gravity ? d->gravity : self->support_.cf.constant_string("AVLayerVideoGravityResizeAspect");
    }, "@@:");
    support.method(layer, "setVideoGravity:", [self](CpuState& s, GuestMemory&) {
        auto d = self->display(s.x[0]);
        const auto old = std::exchange(d->gravity, s.x[2] ? self->support_.cf.copy(s.x[2]) : 0);
        self->release(old);
        const auto name = s.x[2] ? self->support_.cf.utf8(s.x[2]) : std::string();
        const char* contents = name == "AVLayerVideoGravityResize" ? "resize" : name == "AVLayerVideoGravityResizeAspectFill" ? "resizeAspectFill" : "resizeAspect";
        static_cast<void>(self->support_.send_id(s.x[0], "setContentsGravity:", {self->support_.cf.constant_string(contents)}));
    }, "v@:@");
    support.method(layer, "controlTimebase", no_timebase("-[AVSampleBufferDisplayLayer controlTimebase]"), "^{OpaqueCMTimebase=}@:");
    support.method(layer, "setControlTimebase:", no_timebase("-[AVSampleBufferDisplayLayer setControlTimebase:]"), "v@:^{OpaqueCMTimebase=}");
    support.method(layer, "timebase", no_timebase("-[AVSampleBufferDisplayLayer timebase]"), "^{OpaqueCMTimebase=}@:");
    support.method(layer, "dealloc", [self, layer](CpuState& s, GuestMemory&) {
        if (const auto found = self->displays_.find(s.x[0]); found != self->displays_.end()) {
            const auto d = found->second;
            self->displays_.erase(found);
            for (const auto& p : d->queue) self->release(p.image);
            self->release(d->shown);
            self->release(d->error);
            self->release(d->gravity);
            self->release(d->decoder_format);
            if (d->decoder) {
                static_cast<void>(self->support_.call(video_toolbox, "_VTDecompressionSessionInvalidate", {d->decoder}));
                self->release(d->decoder);
            }
            self->release(d->transfer);
            if (d->request.block) self->bridge_->block_release(d->request.block);
            if (d->request.queue) static_cast<void>(self->support_.call(libsystem, "_dispatch_release", {d->request.queue}));
        }
        static_cast<void>(self->support_.objc.send_super(s.x[0], layer, self->support_.objc.selector("dealloc")));
    }, "v@:");
    // Instances register themselves on creation (+alloc goes through -init).
    support.method(layer, "init", [self, layer](CpuState& s, GuestMemory&) {
        const auto made = self->support_.objc.send_super(s.x[0], layer, self->support_.objc.selector("init")).x0;
        if (made) static_cast<void>(self->display(made));
        s.x[0] = made;
    }, "@@:");

    // ---- AVSampleBufferAudioRenderer ------------------------------------------------------------------
    support.method(audio, "enqueueSampleBuffer:", [self](CpuState& s, GuestMemory&) { self->enqueue_audio(s.x[0], s.x[2]); }, "v@:^{opaqueCMSampleBuffer=}");
    support.method(audio, "flush", [self](CpuState& s, GuestMemory&) {
        auto a = self->state<AudioState>(s.x[0]);
        std::lock_guard lock(*a->mutex);
        a->shared->samples.clear();
        a->shared->start = -1;
        if (a->shared->resampler) a->shared->resampler->reset();
    }, "v@:");
    // -flushFromSourceTime:completionHandler: (CMTime by reference): the queued audio from that time on.
    support.method(audio, "flushFromSourceTime:completionHandler:", [self](CpuState& s, GuestMemory& m) {
        auto a = self->state<AudioState>(s.x[0]);
        const auto from = seconds_of(read_time(m, s.x[2]));
        {
            std::lock_guard lock(*a->mutex);
            auto& sh = *a->shared;
            if (sh.channels && sh.start >= 0) {
                const auto keep = from <= sh.start ? 0 : std::min(sh.samples.size() / sh.channels, static_cast<std::size_t>((from - sh.start) * sh.rate));
                sh.samples.resize(keep * sh.channels);
            }
        }
        if (s.x[3]) static_cast<void>(self->bridge_->invoke_block(s.x[3], {1}));
    }, "v@:{?=qiIq}@?");
    support.method(audio, "isReadyForMoreMediaData", [self](CpuState& s, GuestMemory&) { s.x[0] = self->audio_ready(s.x[0]) ? 1 : 0; }, "B@:");
    support.method(audio, "requestMediaDataWhenReadyOnQueue:usingBlock:", [self](CpuState& s, GuestMemory&) {
        auto a = self->state<AudioState>(s.x[0]);
        if (a->request.queue) static_cast<void>(self->support_.call(libsystem, "_dispatch_release", {a->request.queue}));
        if (a->request.block) self->bridge_->block_release(a->request.block);
        a->request.queue = s.x[2];
        if (s.x[2]) static_cast<void>(self->support_.call(libsystem, "_dispatch_retain", {s.x[2]}));
        a->request.block = s.x[3] ? self->bridge_->block_copy(s.x[3]) : 0;
        self->serve(s.x[0], a->request, self->audio_ready(s.x[0]));
    }, "v@:@@?");
    support.method(audio, "stopRequestingMediaData", [self](CpuState& s, GuestMemory&) {
        auto a = self->state<AudioState>(s.x[0]);
        if (a->request.block) self->bridge_->block_release(std::exchange(a->request.block, 0));
        if (a->request.queue) static_cast<void>(self->support_.call(libsystem, "_dispatch_release", {std::exchange(a->request.queue, 0)}));
    }, "v@:");
    support.method(audio, "status", [self](CpuState& s, GuestMemory&) { s.x[0] = static_cast<std::uint64_t>(self->state<AudioState>(s.x[0])->status); }, "q@:");
    support.method(audio, "error", [self](CpuState& s, GuestMemory&) { s.x[0] = self->state<AudioState>(s.x[0])->error; }, "@@:");
    support.method(audio, "volume", [self](CpuState& s, GuestMemory&) { s.v[0] = {std::bit_cast<std::uint32_t>(self->state<AudioState>(s.x[0])->volume), 0}; }, "f@:");
    support.method(audio, "setVolume:", [self](CpuState& s, GuestMemory&) {
        auto a = self->state<AudioState>(s.x[0]);
        a->volume = std::clamp(std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0])), 0.0f, 1.0f);
        std::lock_guard lock(*a->mutex);
        a->shared->gain = a->muted ? 0.0f : a->volume;
    }, "v@:f");
    support.method(audio, "isMuted", [self](CpuState& s, GuestMemory&) { s.x[0] = self->state<AudioState>(s.x[0])->muted ? 1 : 0; }, "B@:");
    support.method(audio, "setMuted:", [self](CpuState& s, GuestMemory&) {
        auto a = self->state<AudioState>(s.x[0]);
        a->muted = (s.x[2] & 0xff) != 0;
        std::lock_guard lock(*a->mutex);
        a->shared->gain = a->muted ? 0.0f : a->volume;
    }, "v@:B");
    // The default output device only: another device's unique ID stops explicitly.
    support.method(audio, "audioOutputDeviceUniqueID", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");
    support.method(audio, "setAudioOutputDeviceUniqueID:", [](CpuState& s, GuestMemory&) {
        if (s.x[2]) unsupported("-[AVSampleBufferAudioRenderer setAudioOutputDeviceUniqueID:] with a device (it plays on the default output device)");
    }, "v@:@");
    support.method(audio, "audioTimePitchAlgorithm", [self](CpuState& s, GuestMemory&) {
        const auto a = self->state<AudioState>(s.x[0]);
        s.x[0] = a->pitch_algorithm ? a->pitch_algorithm : self->support_.cf.constant_string("TimeDomain");
    }, "@@:");
    support.method(audio, "setAudioTimePitchAlgorithm:", [self](CpuState& s, GuestMemory&) {
        auto a = self->state<AudioState>(s.x[0]);
        const auto old = std::exchange(a->pitch_algorithm, s.x[2] ? self->support_.cf.copy(s.x[2]) : 0);
        self->release(old);
    }, "v@:@");
    support.method(audio, "dealloc", [self, audio](CpuState& s, GuestMemory&) {
        if (const auto a = self->support_.objc.find_host_state<AudioState>(s.x[0])) {
            self->stop_audio(s.x[0]);
            self->release(std::exchange(a->error, 0));
            self->release(std::exchange(a->pitch_algorithm, 0));
            if (a->request.block) self->bridge_->block_release(a->request.block);
            if (a->request.queue) static_cast<void>(self->support_.call(libsystem, "_dispatch_release", {a->request.queue}));
        }
        static_cast<void>(self->support_.objc.send_super(s.x[0], audio, self->support_.objc.selector("dealloc")));
    }, "v@:");

    // ---- AVSampleBufferRenderSynchronizer ---------------------------------------------------------------
    support.method(sync, "addRenderer:", [self](CpuState& s, GuestMemory&) {
        auto st = self->state<SyncState>(s.x[0]);
        if (!s.x[2] || std::find(st->renderers.begin(), st->renderers.end(), s.x[2]) != st->renderers.end()) return;
        st->renderers.push_back(self->retain(s.x[2]));
        if (self->is_display(s.x[2])) self->display(s.x[2])->synchronizer = s.x[0];
        else if (const auto a = self->support_.objc.find_host_state<AudioState>(s.x[2])) {
            a->synchronizer = s.x[0];
            if (st->rate == 1.0) self->start_audio(s.x[2]);
        }
    }, "v@:@");
    // -removeRenderer:atTime:completionHandler: (CMTime by reference): removed now; the handler runs on the main queue.
    support.method(sync, "removeRenderer:atTime:completionHandler:", [self](CpuState& s, GuestMemory&) {
        auto st = self->state<SyncState>(s.x[0]);
        const auto found = std::find(st->renderers.begin(), st->renderers.end(), s.x[2]);
        if (found != st->renderers.end()) {
            st->renderers.erase(found);
            if (self->is_display(s.x[2])) self->display(s.x[2])->synchronizer = 0;
            else if (const auto a = self->support_.objc.find_host_state<AudioState>(s.x[2])) { self->stop_audio(s.x[2]); a->synchronizer = 0; }
            self->release(s.x[2]);
        }
        if (!s.x[4]) return;
        const auto block = self->bridge_->block_copy(s.x[4]);
        const auto bridge = self->bridge_;
        bridge->dispatch_async(bridge->main_queue(), [bridge, block] {
            static_cast<void>(bridge->invoke_block(block, {1}));
            bridge->block_release(block);
        });
    }, "v@:@{?=qiIq}@?");
    support.method(sync, "renderers", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->support_.send_id(self->bridge_->array(self->state<SyncState>(s.x[0])->renderers), "autorelease");
    }, "@@:");
    support.method(sync, "rate", [self](CpuState& s, GuestMemory&) { s.v[0] = {std::bit_cast<std::uint32_t>(static_cast<float>(self->state<SyncState>(s.x[0])->rate)), 0}; }, "f@:");
    support.method(sync, "setRate:", [self](CpuState& s, GuestMemory&) {
        self->set_rate(s.x[0], std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0])), std::nullopt);
    }, "v@:f");
    // -setRate:time: and -setRate:time:atHostTime: (CMTimes by reference): the clock reads `time` now.
    support.method(sync, "setRate:time:", [self](CpuState& s, GuestMemory& m) {
        self->set_rate(s.x[0], std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0])), seconds_of(read_time(m, s.x[2])));
    }, "v@:f{?=qiIq}");
    support.method(sync, "setRate:time:atHostTime:", [self](CpuState& s, GuestMemory& m) {
        self->set_rate(s.x[0], std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0])), seconds_of(read_time(m, s.x[2])));
    }, "v@:f{?=qiIq}{?=qiIq}");
    support.method(sync, "currentTime", [self](CpuState& s, GuestMemory& m) { write_time(m, s.x[8], time_of(self->sync_time(s.x[0]))); }, "{?=qiIq}@:");
    support.method(sync, "timebase", no_timebase("-[AVSampleBufferRenderSynchronizer timebase]"), "^{OpaqueCMTimebase=}@:");
    support.method(sync, "delaysRateChangeUntilHasSufficientMediaData", [self](CpuState& s, GuestMemory&) { s.x[0] = self->state<SyncState>(s.x[0])->delays ? 1 : 0; }, "B@:");
    support.method(sync, "setDelaysRateChangeUntilHasSufficientMediaData:", [self](CpuState& s, GuestMemory&) { self->state<SyncState>(s.x[0])->delays = (s.x[2] & 0xff) != 0; }, "v@:B");
    support.method(sync, "addPeriodicTimeObserverForInterval:queue:usingBlock:", [self](CpuState& s, GuestMemory& m) {
        Observer o;
        o.periodic = true;
        o.interval = seconds_of(read_time(m, s.x[2]));
        o.queue = s.x[3];
        if (o.queue) static_cast<void>(self->support_.call(libsystem, "_dispatch_retain", {o.queue}));
        o.block = self->bridge_->block_copy(s.x[4]);
        o.token = self->support_.send_id(self->support_.send_id(self->token_class_, "alloc"), "init");
        const auto now = self->sync_time(s.x[0]);
        o.next_fire = o.interval > 0 ? (std::floor(now / o.interval) + 1) * o.interval : 0;
        self->state<SyncState>(s.x[0])->observers.push_back(o);
        s.x[0] = self->support_.send_id(self->retain(o.token), "autorelease");
    }, "@@:{?=qiIq}@@?");
    support.method(sync, "addBoundaryTimeObserverForTimes:queue:usingBlock:", [self](CpuState& s, GuestMemory& m) {
        Observer o;
        for (const auto value : s.x[2] ? self->support_.cf.array_values(s.x[2]) : std::vector<Address>{}) {
            const auto at = self->support_.process.allocate_guest(24, 8);
            CallRegs r{};
            r.x[2] = at;
            static_cast<void>(self->support_.send(value, "getValue:", r));
            o.times.push_back(seconds_of(read_time(m, at)));
            self->support_.process.release_guest(at);
        }
        o.queue = s.x[3];
        if (o.queue) static_cast<void>(self->support_.call(libsystem, "_dispatch_retain", {o.queue}));
        o.block = self->bridge_->block_copy(s.x[4]);
        o.token = self->support_.send_id(self->support_.send_id(self->token_class_, "alloc"), "init");
        self->state<SyncState>(s.x[0])->observers.push_back(o);
        s.x[0] = self->support_.send_id(self->retain(o.token), "autorelease");
    }, "@@:@@@?");
    support.method(sync, "removeTimeObserver:", [self](CpuState& s, GuestMemory&) {
        auto& observers = self->state<SyncState>(s.x[0])->observers;
        const auto found = std::find_if(observers.begin(), observers.end(), [&](const Observer& o) { return o.token == s.x[2]; });
        if (found == observers.end()) return;
        self->release(found->token);
        self->bridge_->block_release(found->block);
        if (found->queue) static_cast<void>(self->support_.call(libsystem, "_dispatch_release", {found->queue}));
        observers.erase(found);
    }, "v@:@");
    support.method(sync, "dealloc", [self, sync](CpuState& s, GuestMemory&) {
        if (const auto st = self->support_.objc.find_host_state<SyncState>(s.x[0])) {
            for (const auto r : std::exchange(st->renderers, {})) {
                if (self->is_display(r)) self->display(r)->synchronizer = 0;
                else if (const auto a = self->support_.objc.find_host_state<AudioState>(r)) { self->stop_audio(r); a->synchronizer = 0; }
                self->release(r);
            }
            for (auto& o : st->observers) {
                self->release(o.token);
                self->bridge_->block_release(o.block);
                if (o.queue) static_cast<void>(self->support_.call(libsystem, "_dispatch_release", {o.queue}));
            }
        }
        static_cast<void>(self->support_.objc.send_super(s.x[0], sync, self->support_.objc.selector("dealloc")));
    }, "v@:");
}

} // namespace

void install_av_sample_buffer_renderers(Support& support) { std::make_shared<Renderers>(support)->install(); }

} // namespace alchemy::gui::completion_kit::media
