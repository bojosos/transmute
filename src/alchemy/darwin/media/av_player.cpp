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

// AVPlayer, AVPlayerItem, AVPlayerItemVideoOutput and AVPlayerLayer over AVFoundation's assets
// (avfoundation.cpp): one item at a time, played on a clock that runs at the player's rate.
// - An item loads its asset when a player takes it (on the main queue): status becomes
//   AVPlayerItemStatusReadyToPlay when the asset has a readable track, else Failed with the
//   asset's error; key-value observers of `status` (item and player), `rate` and
//   `timeControlStatus` are notified.
// - Video: the item's first decodable video track is read through an AVAssetReader whose output
//   decodes into IOSurface-backed BGRA pixel buffers (or the pixel buffer attributes of the first
//   AVPlayerItemVideoOutput added); the picture due at the item's time becomes the contents of
//   every AVPlayerLayer showing the player, and the outputs hand it out (hasNewPixelBuffer /
//   copyPixelBuffer). Seeking starts a new reader at the time.
// - Audio: the item's readable audio track (audio_file.hpp: PCM, MP3, FLAC, Opus) plays on the
//   default output device at rate 1 with the player's volume and mute (other rates are silent);
//   the main queue decodes ahead of the device.
// - A tick on the main queue (every 1/60 s while a player plays) advances the pictures, fires
//   periodic and boundary time observers on their queues and, at the item's end, posts
//   AVPlayerItemDidPlayToEndTimeNotification and pauses (actionAtItemEnd Pause or None).
namespace alchemy::gui::completion_kit::media {
namespace {

constexpr std::string_view av_foundation = "/System/Library/Frameworks/AVFoundation.framework/Versions/A/AVFoundation";
constexpr std::string_view core_media = "/System/Library/Frameworks/CoreMedia.framework/Versions/A/CoreMedia";
constexpr std::string_view core_video = "/System/Library/Frameworks/CoreVideo.framework/Versions/A/CoreVideo";
constexpr std::string_view core_foundation = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";
constexpr double tick_seconds = 1.0 / 60;
constexpr double audio_ahead_seconds = 0.5;

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
constexpr Time time_invalid{};

struct ItemState final : ObjectPayload {
    Address asset{}, error{};                  // retained
    std::int64_t status{};                     // AVPlayerItemStatus: 0 unknown, 1 ready, 2 failed
    bool loading{};
    double duration{};                         // seconds (0: unknown)
    double width{}, height{};
    // Video: an internal reader of decoded pictures, the picture shown and the next one.
    Address video_track{};                     // the asset's (not retained)
    Address reader{}, output{};                // retained
    Address shown{}, next{};                   // retained CMSampleBuffers
    double next_time{-1};
    bool reader_done{};
    std::uint64_t shown_serial{};
    std::vector<Address> outputs;              // retained AVPlayerItemVideoOutputs
    // Audio: decoded ahead into `ring` (interleaved, the file's rate and channels) on the main
    // queue; the device's I/O thread takes it.
    GuestAudio audio;
    bool has_audio{};
    std::uint64_t decoded_to{};                // frames of the audio decoded into the ring
    std::shared_ptr<std::mutex> ring_mutex = std::make_shared<std::mutex>();
    std::shared_ptr<std::deque<float>> ring = std::make_shared<std::deque<float>>();
};

struct Observer {
    Address token{};                           // retained token object
    bool periodic{};
    double interval{}, next_fire{};
    std::vector<double> times;
    Address queue{}, block{};                  // retained
};

struct PlayerState final : ObjectPayload {
    Address item{};                            // retained
    double rate{};
    double position{};                         // the item's time at `anchor`
    std::chrono::steady_clock::time_point anchor{};
    float volume{1};
    bool muted{};
    std::int64_t action_at_end{1};             // AVPlayerActionAtItemEndPause
    double last_tick_time{};
    std::vector<Observer> observers;
    std::vector<Address> layers;               // not retained (layers retain their player)
    std::shared_ptr<hal_io::DeviceIo> io;
    // What the I/O thread reads.
    std::shared_ptr<std::mutex> audio_mutex = std::make_shared<std::mutex>();
    struct Audio {
        bool playing{};
        float gain{1};
        double rate{};
        std::uint32_t channels{};
        std::shared_ptr<std::mutex> ring_mutex;
        std::shared_ptr<std::deque<float>> ring;
        std::unique_ptr<Resampler> resampler;
    };
    std::shared_ptr<Audio> audio_out = std::make_shared<Audio>();
};

struct OutputState final : ObjectPayload {
    Address attributes{};                      // retained
    Address item{};                            // not retained (the item keeps its outputs)
    std::uint64_t taken_serial{};
};

// An AVPlayerLayer's state: in a side map, as a CALayer already carries QuartzCore's host state.
struct LayerState {
    Address player{};                          // retained
    Address gravity{};                         // retained
    bool ready{};
};

class Players : public std::enable_shared_from_this<Players> {
public:
    explicit Players(Support& support) : support_(support), bridge_(security::bridge()) {}
    void install();

private:
    template <class T> std::shared_ptr<T> state(Address self) { return support_.objc.host_state<T>(self); }
    Address retain(Address object) { return object ? support_.send_id(object, "retain") : 0; }
    void release(Address object) { if (object) static_cast<void>(support_.send_id(object, "release")); }
    void will_change(Address object, const char* key) { change(object, key, "willChangeValueForKey:"); }
    void did_change(Address object, const char* key) { change(object, key, "didChangeValueForKey:"); }
    void change(Address object, const char* key, const char* selector) {
        const auto name = support_.cf.string(key);
        CallRegs r{};
        r.x[2] = name;
        static_cast<void>(support_.objc.send_if_responds(object, support_.objc.selector(selector), r));
        support_.release(name);
    }
    double now_time(const PlayerState& p) const {
        if (p.rate == 0) return p.position;
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - p.anchor).count();
        return p.position + elapsed * p.rate;
    }
    double current_time(Address player) {
        const auto p = state<PlayerState>(player);
        double t = now_time(*p);
        if (p->item) {
            const auto item = state<ItemState>(p->item);
            if (item->duration > 0) t = std::min(t, item->duration);
        }
        return std::max(0.0, t);
    }

    void load_item(Address item);
    void attach(Address player, Address item);
    void set_rate(Address player, double rate);
    void seek(Address player, double seconds);
    void start_reader(Address item, double from);
    void stop_reader(ItemState& item);
    bool advance_video(Address item, double t);
    void show(Address player);
    void fill_audio(ItemState& item);
    void reset_audio(Address player, double seconds);
    bool start_audio(Address player);
    void stop_audio(Address player);
    void render(const std::shared_ptr<PlayerState::Audio>& a, float* mix, std::uint32_t frames, std::uint32_t channels, double device_rate);
    void schedule();
    void tick();
    void fire_observers(Address player, double from, double to);
    void post_end(Address item);
    void invoke_time_block(Address queue, Address block, double seconds);

    Support& support_;
    std::shared_ptr<security::Bridge> bridge_;
    Address player_class_{}, item_class_{}, output_class_{}, layer_class_{}, token_class_{};
    Address tick_function_{};
    std::set<Address> players_;                // every player (not retained)
    std::map<Address, std::shared_ptr<LayerState>> layers_;   // AVPlayerLayers
    std::shared_ptr<LayerState> layer_state(Address layer) {
        auto& slot = layers_[layer];
        if (!slot) slot = std::make_shared<LayerState>();
        return slot;
    }
    std::set<Address> playing_;                // players to tick (retained)
    Address owner_of(Address item) const {
        for (const auto p : players_) if (support_.objc.find_host_state<PlayerState>(p) && support_.objc.find_host_state<PlayerState>(p)->item == item) return p;
        return 0;
    }
    bool scheduled_{};
};

// The item's asset loaded: status, duration, its video track and audio.
void Players::load_item(Address item) {
    auto s = state<ItemState>(item);
    if (s->status != 0 || !s->asset) return;
    const auto tracks = support_.send_id(s->asset, "tracks");
    const auto count = tracks ? support_.cf.length(tracks) : 0;
    std::int64_t status = 2;
    if (count) {
        status = 1;
        auto& m = support_.memory();
        const auto at = support_.process.allocate_guest(24, 8);
        CallRegs r{};
        r.indirect_result = at;
        static_cast<void>(support_.send(s->asset, "duration", r));
        s->duration = seconds_of(read_time(m, at));
        support_.process.release_guest(at);
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto track = support_.cf.element(tracks, i);
            const auto type = support_.cf.utf8(support_.send_id(track, "mediaType"));
            if (type == "vide" && !s->video_track && (support_.send_id(track, "isDecodable") & 0xff)) {
                s->video_track = track;
                const auto size = support_.send(track, "naturalSize", {});
                s->width = size.d(0);
                s->height = size.d(1);
            }
        }
        // The readable audio (PCM, or decoded).
        const auto url = support_.send_id(s->asset, "URL");
        if (url && support_.cf.utf8(support_.send_id(url, "scheme")) == "file") {
            GuestAudio audio;
            if (!open_guest_audio(support_, support_.cf.url_path(url), audio)) {
                s->audio = std::move(audio);
                s->has_audio = true;
            }
        }
    } else {
        s->error = bridge_->error("AVFoundationErrorDomain", -11828, "Cannot Open (no track of the media can be read under Alchemy)");
    }
    will_change(item, "status");
    s->status = status;
    did_change(item, "status");
    if (status == 1 && s->video_track) start_reader(item, 0);
}

void Players::stop_reader(ItemState& s) {
    if (s.reader) static_cast<void>(support_.send_id(s.reader, "cancelReading"));
    release(std::exchange(s.reader, 0));
    release(std::exchange(s.output, 0));
    release(std::exchange(s.next, 0));
    s.next_time = -1;
    s.reader_done = false;
}

// A reader of the video track's pictures from `from` seconds: in the first video output's pixel
// buffer attributes, else IOSurface-backed BGRA.
void Players::start_reader(Address item, double from) {
    auto s = state<ItemState>(item);
    stop_reader(*s);
    if (!s->video_track) return;
    Address settings{};
    if (!s->outputs.empty() && state<OutputState>(s->outputs.front())->attributes) {
        settings = retain(state<OutputState>(s->outputs.front())->attributes);
    } else {
        const auto format_key = support_.cf.string("PixelFormatType"), surface_key = support_.cf.string("IOSurfaceProperties");
        const auto bgra = support_.cf.integer(0x42475241), empty = support_.cf.dictionary(std::span<const std::pair<std::uint64_t, std::uint64_t>>{});
        const std::array<std::pair<std::uint64_t, std::uint64_t>, 2> entries{{{format_key, bgra}, {surface_key, empty}}};
        settings = support_.cf.dictionary(entries);
        for (const auto object : {format_key, surface_key, bgra, empty}) support_.release(object);
    }
    CallRegs make{};
    make.x[2] = s->asset;
    make.x[3] = 0;
    s->reader = support_.send(support_.send_id(support_.objc.find_class("AVAssetReader"), "alloc"), "initWithAsset:error:", make).x0;
    CallRegs make_output{};
    make_output.x[2] = s->video_track;
    make_output.x[3] = settings;
    s->output = support_.send(support_.send_id(support_.objc.find_class("AVAssetReaderTrackOutput"), "alloc"), "initWithTrack:outputSettings:", make_output).x0;
    release(settings);
    if (!s->reader || !s->output) { stop_reader(*s); return; }
    CallRegs add{};
    add.x[2] = s->output;
    static_cast<void>(support_.send(s->reader, "addOutput:", add));
    auto& m = support_.memory();
    const auto range = support_.process.allocate_guest(48, 8);
    write_time(m, range, time_of(from));
    write_time(m, range + 24, {0, 0, 1 | 4, 0});   // +infinity
    CallRegs set_range{};
    set_range.x[2] = range;
    static_cast<void>(support_.send(s->reader, "setTimeRange:", set_range));
    support_.process.release_guest(range);
    if (!(support_.send_id(s->reader, "startReading") & 0xff)) stop_reader(*s);
}

// Moves the shown picture up to time `t`; true when it changed.
bool Players::advance_video(Address item, double t) {
    auto s = state<ItemState>(item);
    bool changed = false;
    auto& m = support_.memory();
    for (;;) {
        if (!s->next && s->output && !s->reader_done) {
            s->next = support_.send_id(s->output, "copyNextSampleBuffer");
            if (!s->next) { s->reader_done = true; break; }
            const auto at = support_.process.allocate_guest(24, 8);
            CallRegs r{};
            r.x[0] = s->next;
            r.indirect_result = at;
            static_cast<void>(support_.call(core_media, "_CMSampleBufferGetPresentationTimeStamp", r));
            s->next_time = seconds_of(read_time(m, at));
            support_.process.release_guest(at);
        }
        if (!s->next || (s->shown && s->next_time > t + 1e-6)) break;
        release(std::exchange(s->shown, s->next));
        s->next = 0;
        ++s->shown_serial;
        changed = true;
    }
    return changed;
}

// The shown picture as the contents of the player's layers.
void Players::show(Address player) {
    const auto p = state<PlayerState>(player);
    if (!p->item) return;
    const auto s = state<ItemState>(p->item);
    const auto image = s->shown ? support_.call(core_media, "_CMSampleBufferGetImageBuffer", {s->shown}) : 0;
    const auto surface = image ? support_.call(core_video, "_CVPixelBufferGetIOSurface", {image}) : 0;
    for (const auto layer : p->layers) {
        if (!surface) continue;
        CallRegs r{};
        r.x[2] = surface;
        static_cast<void>(support_.send(layer, "setContents:", r));
        auto l = layer_state(layer);
        if (!l->ready) {
            will_change(layer, "readyForDisplay");
            l->ready = true;
            did_change(layer, "readyForDisplay");
        }
    }
}

// Decodes the item's audio ahead of the device.
void Players::fill_audio(ItemState& s) {
    if (!s.has_audio) return;
    const auto rate = s.audio.container.format.rate;
    const auto channels = s.audio.container.format.channels;
    std::size_t buffered{};
    {
        std::lock_guard lock(*s.ring_mutex);
        buffered = s.ring->size() / std::max<std::uint32_t>(channels, 1);
    }
    const auto wanted = static_cast<std::size_t>(rate * audio_ahead_seconds);
    if (buffered >= wanted || s.decoded_to >= s.audio.frames()) return;
    std::vector<double> samples;
    const auto got = read_audio_frames(support_, s.audio, s.decoded_to, wanted - buffered, samples);
    s.decoded_to += got;
    std::lock_guard lock(*s.ring_mutex);
    for (const auto v : samples) s.ring->push_back(static_cast<float>(v));
}

void Players::reset_audio(Address player, double seconds) {
    const auto p = state<PlayerState>(player);
    if (!p->item) return;
    auto s = state<ItemState>(p->item);
    if (!s->has_audio) return;
    {
        std::lock_guard lock(*s->ring_mutex);
        s->ring->clear();
    }
    s->decoded_to = static_cast<std::uint64_t>(std::max(0.0, seconds) * s->audio.container.format.rate);
    std::lock_guard lock(*p->audio_mutex);
    if (p->audio_out->resampler) p->audio_out->resampler->reset();
}

bool Players::start_audio(Address player) {
    const auto p = state<PlayerState>(player);
    if (!p->item || !state<ItemState>(p->item)->has_audio) return false;
    auto s = state<ItemState>(p->item);
    if (!p->io) p->io = hal_device_io(hal_default_output());
    if (!p->io) return false;
    {
        std::lock_guard lock(*p->audio_mutex);
        auto& a = *p->audio_out;
        a.rate = s->audio.container.format.rate;
        a.channels = s->audio.container.format.channels;
        a.ring_mutex = s->ring_mutex;
        a.ring = s->ring;
        a.gain = p->muted ? 0.0f : p->volume;
        a.playing = p->rate == 1.0;
    }
    fill_audio(*s);
    if (p->io->running(player)) return true;
    const auto owner = shared_from_this();
    const auto out = p->audio_out;
    const auto mutex = p->audio_mutex;
    const auto channels = p->io->channels();
    const auto device_rate = p->io->rate();
    return p->io->start(player, [owner, out, mutex, channels, device_rate](float* mix, std::uint32_t frames, const hal_io::Timing&) {
        std::lock_guard lock(*mutex);
        owner->render(out, mix, frames, channels, device_rate);
    }, hal_buffer_frames(hal_default_output())) == 0;
}

void Players::stop_audio(Address player) {
    const auto p = state<PlayerState>(player);
    {
        std::lock_guard lock(*p->audio_mutex);
        p->audio_out->playing = false;
    }
    if (p->io) p->io->stop(player);
}

// On the device's I/O thread (the player's audio mutex held): the ring, resampled to the device.
void Players::render(const std::shared_ptr<PlayerState::Audio>& a, float* mix, std::uint32_t frames, std::uint32_t channels, double device_rate) {
    if (!a->playing || !a->ring || !a->channels) return;
    const auto source = a->channels;
    if (!a->resampler) a->resampler = std::make_unique<Resampler>(a->rate, device_rate, source);
    std::vector<double> converted(static_cast<std::size_t>(frames) * source);
    a->resampler->render(converted.data(), frames, [&](double* out, std::size_t count) {
        std::lock_guard lock(*a->ring_mutex);
        const auto values = count * source;
        const auto have = std::min(values, a->ring->size());
        std::copy_n(a->ring->begin(), have, out);
        std::fill(out + have, out + values, 0.0);
        a->ring->erase(a->ring->begin(), a->ring->begin() + static_cast<std::ptrdiff_t>(have));
    });
    for (std::uint32_t f = 0; f < frames; ++f) {
        const double* frame = converted.data() + static_cast<std::size_t>(f) * source;
        float* out = mix + static_cast<std::size_t>(f) * channels;
        for (std::uint32_t c = 0; c < channels; ++c) out[c] += static_cast<float>(frame[source == 1 ? 0 : std::min(c, source - 1)]) * a->gain;
    }
}

void Players::attach(Address player, Address item) {
    const auto p = state<PlayerState>(player);
    if (p->item == item) return;
    stop_audio(player);
    will_change(player, "currentItem");
    const auto old = std::exchange(p->item, retain(item));
    p->position = 0;
    p->anchor = std::chrono::steady_clock::now();
    did_change(player, "currentItem");
    release(old);
    if (!item) return;
    // The item loads on the main queue, as AVFoundation reports its status asynchronously.
    const auto owner = shared_from_this();
    const auto kept = retain(item);
    const auto kept_player = retain(player);
    bridge_->dispatch_async(bridge_->main_queue(), [owner, kept, kept_player] {
        owner->load_item(kept);
        const auto p = owner->state<PlayerState>(kept_player);
        if (p->item == kept) {
            owner->reset_audio(kept_player, 0);
            if (p->rate != 0) {
                owner->start_audio(kept_player);
                owner->schedule();
            }
            owner->advance_video(kept, 0);
            owner->show(kept_player);
        }
        owner->release(kept);
        owner->release(kept_player);
    });
}

void Players::set_rate(Address player, double rate) {
    const auto p = state<PlayerState>(player);
    if (p->rate == rate) return;
    p->position = current_time(player);
    p->anchor = std::chrono::steady_clock::now();
    p->last_tick_time = p->position;
    will_change(player, "rate");
    will_change(player, "timeControlStatus");
    p->rate = rate;
    did_change(player, "timeControlStatus");
    did_change(player, "rate");
    if (rate != 0) {
        if (playing_.insert(player).second) static_cast<void>(retain(player));
        if (p->item && state<ItemState>(p->item)->status == 1) start_audio(player);
        schedule();
    } else {
        stop_audio(player);
    }
}

void Players::seek(Address player, double seconds) {
    const auto p = state<PlayerState>(player);
    if (p->item) {
        const auto s = state<ItemState>(p->item);
        if (s->duration > 0) seconds = std::min(seconds, s->duration);
    }
    seconds = std::max(0.0, seconds);
    p->position = seconds;
    p->anchor = std::chrono::steady_clock::now();
    p->last_tick_time = seconds;
    if (!p->item) return;
    const auto s = state<ItemState>(p->item);
    if (s->status != 1) return;
    release(std::exchange(s->shown, 0));
    start_reader(p->item, seconds);
    advance_video(p->item, seconds);
    show(player);
    reset_audio(player, seconds);
    if (p->rate != 0) fill_audio(*s);
}

void Players::invoke_time_block(Address queue, Address block, double seconds) {
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

void Players::fire_observers(Address player, double from, double to) {
    const auto p = state<PlayerState>(player);
    for (auto& o : p->observers) {
        if (o.periodic) {
            if (o.interval <= 0) continue;
            if (to + 1e-9 >= o.next_fire) {
                invoke_time_block(o.queue, o.block, to);
                o.next_fire = (std::floor(to / o.interval) + 1) * o.interval;
            }
        } else {
            for (const auto t : o.times)
                if (t > from && t <= to) invoke_time_block(o.queue, o.block, t);
        }
    }
}

void Players::post_end(Address item) {
    const auto center = support_.send_id(support_.objc.find_class("NSNotificationCenter"), "defaultCenter");
    const auto name = support_.cf.string("AVPlayerItemDidPlayToEndTimeNotification");
    CallRegs r{};
    r.x[2] = name;
    r.x[3] = item;
    static_cast<void>(support_.send(center, "postNotificationName:object:", r));
    support_.release(name);
}

// dispatch_after_f on the main queue, once.
void Players::schedule() {
    if (scheduled_ || playing_.empty()) return;
    scheduled_ = true;
    const auto when = support_.call(libsystem, "_dispatch_time", {0, static_cast<Address>(tick_seconds * 1e9)});
    static_cast<void>(support_.call(libsystem, "_dispatch_after_f", {when, bridge_->main_queue(), 0, tick_function_}));
}

void Players::tick() {
    scheduled_ = false;
    std::vector<Address> done;
    for (const auto player : std::vector<Address>(playing_.begin(), playing_.end())) {
        const auto p = state<PlayerState>(player);
        if (p->rate == 0 || !p->item) { done.push_back(player); continue; }
        const auto s = state<ItemState>(p->item);
        if (s->status != 1) continue;   // still loading
        const auto t = current_time(player);
        if (advance_video(p->item, t)) show(player);
        fill_audio(*s);
        fire_observers(player, p->last_tick_time, t);
        p->last_tick_time = t;
        if (s->duration > 0 && t >= s->duration) {
            const auto item = p->item;
            if (p->action_at_end != 2) {   // AVPlayerActionAtItemEndNone keeps the rate
                set_rate(player, 0);
                p->position = s->duration;
            }
            post_end(item);
            done.push_back(player);
        }
    }
    for (const auto player : done) {
        if (playing_.erase(player)) release(player);
    }
    schedule();
}

void Players::install() {
    auto& support = support_;
    const auto self = shared_from_this();
    const auto declare = [&](const char* name, const char* superclass) { return support.declare_class(name, superclass, av_foundation); };
    player_class_ = declare("AVPlayer", "NSObject");
    item_class_ = declare("AVPlayerItem", "NSObject");
    output_class_ = declare("AVPlayerItemVideoOutput", "NSObject");
    layer_class_ = declare("AVPlayerLayer", "CALayer");
    token_class_ = declare("__AlchemyAVPlayerTimeObserver", "NSObject");
    tick_function_ = support.process.install_function("__AlchemyAVPlayerTick", [self](CpuState&, GuestMemory&) { self->tick(); }, std::string(av_foundation));
    const auto player = player_class_, item = item_class_, output = output_class_, layer = layer_class_;
    const auto return_time = [](GuestMemory& m, CpuState& s, double seconds) { write_time(m, s.x[8], time_of(seconds)); };

    // ---- AVPlayerItem ---------------------------------------------------------------------
    support.method(item, "playerItemWithURL:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->support_.send_id(self->support_.send_id(self->support_.send_id(s.x[0], "alloc"), "initWithURL:", {s.x[2]}), "autorelease");
    }, "@@:@", true);
    support.method(item, "playerItemWithAsset:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->support_.send_id(self->support_.send_id(self->support_.send_id(s.x[0], "alloc"), "initWithAsset:", {s.x[2]}), "autorelease");
    }, "@@:@", true);
    support.method(item, "initWithURL:", [self](CpuState& s, GuestMemory&) {
        if (!s.x[2]) unsupported("-[AVPlayerItem initWithURL:] with a nil URL");
        CallRegs r{};
        r.x[2] = s.x[2];
        r.x[3] = 0;
        const auto asset = self->support_.send(self->support_.send_id(self->support_.objc.find_class("AVURLAsset"), "alloc"), "initWithURL:options:", r).x0;
        self->state<ItemState>(s.x[0])->asset = asset;   // +1 from alloc/init
    }, "@@:@");
    support.method(item, "initWithAsset:", [self](CpuState& s, GuestMemory&) {
        if (!s.x[2]) unsupported("-[AVPlayerItem initWithAsset:] with a nil asset");
        self->state<ItemState>(s.x[0])->asset = self->retain(s.x[2]);
    }, "@@:@");
    support.method(item, "dealloc", [self, item](CpuState& s, GuestMemory&) {
        if (const auto st = self->support_.objc.find_host_state<ItemState>(s.x[0])) {
            self->stop_reader(*st);
            self->release(std::exchange(st->shown, 0));
            for (const auto o : std::exchange(st->outputs, {})) self->release(o);
            close_guest_audio(self->support_, st->audio);
            self->release(std::exchange(st->asset, 0));
            self->release(std::exchange(st->error, 0));
        }
        static_cast<void>(self->support_.objc.send_super(s.x[0], item, self->support_.objc.selector("dealloc")));
    }, "v@:");
    support.method(item, "asset", [self](CpuState& s, GuestMemory&) { s.x[0] = self->state<ItemState>(s.x[0])->asset; }, "@@:");
    support.method(item, "status", [self](CpuState& s, GuestMemory&) { s.x[0] = static_cast<std::uint64_t>(self->state<ItemState>(s.x[0])->status); }, "q@:");
    support.method(item, "error", [self](CpuState& s, GuestMemory&) { s.x[0] = self->state<ItemState>(s.x[0])->error; }, "@@:");
    support.method(item, "duration", [self](CpuState& s, GuestMemory& m) {
        const auto st = self->state<ItemState>(s.x[0]);
        if (st->status != 1) { write_time(m, s.x[8], time_invalid); return; }
        CallRegs r{};
        r.indirect_result = s.x[8];
        static_cast<void>(self->support_.send(st->asset, "duration", r));
    }, "{?=qiIq}@:");
    // The item's time: its player's (an item belongs to the player that took it last).
    support.method(item, "currentTime", [self, return_time](CpuState& s, GuestMemory& m) {
        const Address owner = self->owner_of(s.x[0]);
        return_time(m, s, owner ? self->current_time(owner) : 0.0);
    }, "{?=qiIq}@:");
    support.method(item, "presentationSize", [self](CpuState& s, GuestMemory&) {
        const auto st = self->state<ItemState>(s.x[0]);
        s.v[0] = {std::bit_cast<std::uint64_t>(st->width), 0};
        s.v[1] = {std::bit_cast<std::uint64_t>(st->height), 0};
    }, "{CGSize=dd}@:");
    support.method(item, "outputs", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->support_.send_id(self->bridge_->array(self->state<ItemState>(s.x[0])->outputs), "autorelease");
    }, "@@:");
    support.method(item, "addOutput:", [self](CpuState& s, GuestMemory&) {
        const auto st = self->state<ItemState>(s.x[0]);
        if (!s.x[2] || std::find(st->outputs.begin(), st->outputs.end(), s.x[2]) != st->outputs.end()) return;
        st->outputs.push_back(self->retain(s.x[2]));
        self->state<OutputState>(s.x[2])->item = s.x[0];
    }, "v@:@");
    support.method(item, "removeOutput:", [self](CpuState& s, GuestMemory&) {
        const auto st = self->state<ItemState>(s.x[0]);
        const auto found = std::find(st->outputs.begin(), st->outputs.end(), s.x[2]);
        if (found == st->outputs.end()) return;
        st->outputs.erase(found);
        self->state<OutputState>(s.x[2])->item = 0;
        self->release(s.x[2]);
    }, "v@:@");
    for (const auto* name : {"canPlayFastForward", "canPlaySlowForward", "canStepForward", "isPlaybackLikelyToKeepUp", "isPlaybackBufferFull"})
        support.method(item, name, [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    for (const auto* name : {"canPlayReverse", "canPlayFastReverse", "canPlaySlowReverse", "canStepBackward", "isPlaybackBufferEmpty"})
        support.method(item, name, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");

    // ---- AVPlayerItemVideoOutput ----------------------------------------------------------------
    support.method(output, "initWithPixelBufferAttributes:", [self](CpuState& s, GuestMemory&) {
        self->state<OutputState>(s.x[0])->attributes = self->retain(s.x[2]);
    }, "@@:@");
    support.method(output, "dealloc", [self, output](CpuState& s, GuestMemory&) {
        if (const auto st = self->support_.objc.find_host_state<OutputState>(s.x[0])) self->release(std::exchange(st->attributes, 0));
        static_cast<void>(self->support_.objc.send_super(s.x[0], output, self->support_.objc.selector("dealloc")));
    }, "v@:");
    // -hasNewPixelBufferForItemTime: (CMTime by reference): a picture not yet copied.
    support.method(output, "hasNewPixelBufferForItemTime:", [self](CpuState& s, GuestMemory&) {
        const auto o = self->state<OutputState>(s.x[0]);
        const auto item_state = o->item ? self->state<ItemState>(o->item) : nullptr;
        s.x[0] = item_state && item_state->shown && item_state->shown_serial != o->taken_serial ? 1 : 0;
    }, "B@:{?=qiIq}");
    // -copyPixelBufferForItemTime:itemTimeForDisplay: (+1 CVPixelBuffer, the picture shown now).
    support.method(output, "copyPixelBufferForItemTime:itemTimeForDisplay:", [self](CpuState& s, GuestMemory& m) {
        const auto o = self->state<OutputState>(s.x[0]);
        const auto item_state = o->item ? self->state<ItemState>(o->item) : nullptr;
        if (s.x[3]) write_time(m, s.x[3], time_invalid);
        if (!item_state || !item_state->shown) { s.x[0] = 0; return; }
        const auto image = self->support_.call(core_media, "_CMSampleBufferGetImageBuffer", {item_state->shown});
        o->taken_serial = item_state->shown_serial;
        if (s.x[3]) {
            CallRegs r{};
            r.x[0] = item_state->shown;
            r.indirect_result = s.x[3];
            static_cast<void>(self->support_.call(core_media, "_CMSampleBufferGetPresentationTimeStamp", r));
        }
        s.x[0] = image ? self->support_.call(core_foundation, "_CFRetain", {image}) : 0;
    }, "^{__CVBuffer=}@:{?=qiIq}^{?=qiIq}");
    // -itemTimeForHostTime: (CFTimeInterval in d0) and -itemTimeForMachAbsoluteTime: (x2): the item's time now.
    for (const auto* name : {"itemTimeForHostTime:", "itemTimeForMachAbsoluteTime:", "itemTimeForCVTimeStamp:"}) {
        support.method(output, name, [self, return_time](CpuState& s, GuestMemory& m) {
            const auto o = self->state<OutputState>(s.x[0]);
            const Address owner = o->item ? self->owner_of(o->item) : 0;
            return_time(m, s, owner ? self->current_time(owner) : 0.0);
        }, "{?=qiIq}@:d");
    }

    // ---- AVPlayer ---------------------------------------------------------------------------
    support.method(player, "playerWithURL:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->support_.send_id(self->support_.send_id(self->support_.send_id(s.x[0], "alloc"), "initWithURL:", {s.x[2]}), "autorelease");
    }, "@@:@", true);
    support.method(player, "playerWithPlayerItem:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->support_.send_id(self->support_.send_id(self->support_.send_id(s.x[0], "alloc"), "initWithPlayerItem:", {s.x[2]}), "autorelease");
    }, "@@:@", true);
    support.method(player, "initWithURL:", [self](CpuState& s, GuestMemory&) {
        const auto made = self->support_.send_id(self->support_.send_id(self->item_class_, "alloc"), "initWithURL:", {s.x[2]});
        static_cast<void>(self->state<PlayerState>(s.x[0]));
        self->players_.insert(s.x[0]);
        self->attach(s.x[0], made);
        self->release(made);
    }, "@@:@");
    support.method(player, "initWithPlayerItem:", [self](CpuState& s, GuestMemory&) {
        static_cast<void>(self->state<PlayerState>(s.x[0]));
        self->players_.insert(s.x[0]);
        self->attach(s.x[0], s.x[2]);
    }, "@@:@");
    support.method(player, "dealloc", [self, player](CpuState& s, GuestMemory&) {
        self->players_.erase(s.x[0]);
        if (const auto st = self->support_.objc.find_host_state<PlayerState>(s.x[0])) {
            self->stop_audio(s.x[0]);
            for (auto& o : st->observers) {
                self->release(o.token);
                self->bridge_->block_release(o.block);
                if (o.queue) static_cast<void>(self->support_.call(libsystem, "_dispatch_release", {o.queue}));
            }
            st->observers.clear();
            self->release(std::exchange(st->item, 0));
        }
        static_cast<void>(self->support_.objc.send_super(s.x[0], player, self->support_.objc.selector("dealloc")));
    }, "v@:");
    support.method(player, "currentItem", [self](CpuState& s, GuestMemory&) { s.x[0] = self->state<PlayerState>(s.x[0])->item; }, "@@:");
    support.method(player, "replaceCurrentItemWithPlayerItem:", [self](CpuState& s, GuestMemory&) { self->attach(s.x[0], s.x[2]); }, "v@:@");
    support.method(player, "status", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "q@:");   // AVPlayerStatusReadyToPlay: the player itself can always play
    support.method(player, "error", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");
    support.method(player, "play", [self](CpuState& s, GuestMemory&) { self->set_rate(s.x[0], 1.0); }, "v@:");
    support.method(player, "pause", [self](CpuState& s, GuestMemory&) { self->set_rate(s.x[0], 0.0); }, "v@:");
    support.method(player, "rate", [self](CpuState& s, GuestMemory&) {
        s.v[0] = {std::bit_cast<std::uint32_t>(static_cast<float>(self->state<PlayerState>(s.x[0])->rate)), 0};
    }, "f@:");
    support.method(player, "setRate:", [self](CpuState& s, GuestMemory&) {
        self->set_rate(s.x[0], std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0])));
    }, "v@:f");
    // AVPlayerTimeControlStatus: 0 paused, 2 playing (nothing ever waits to buffer).
    support.method(player, "timeControlStatus", [self](CpuState& s, GuestMemory&) { s.x[0] = self->state<PlayerState>(s.x[0])->rate != 0 ? 2 : 0; }, "q@:");
    support.method(player, "reasonForWaitingToPlay", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");
    support.method(player, "currentTime", [self, return_time](CpuState& s, GuestMemory& m) { return_time(m, s, self->current_time(s.x[0])); }, "{?=qiIq}@:");
    // -seekToTime: and the forms with tolerances and a completion handler (CMTimes by reference);
    // seeks are exact and done at once; the handler runs on the main queue with finished YES.
    const auto seek_with = [self](CpuState& s, GuestMemory& m, Address handler) {
        self->seek(s.x[0], seconds_of(read_time(m, s.x[2])));
        if (!handler) return;
        const auto block = self->bridge_->block_copy(handler);
        const auto bridge = self->bridge_;
        bridge->dispatch_async(bridge->main_queue(), [bridge, block] {
            static_cast<void>(bridge->invoke_block(block, {1}));
            bridge->block_release(block);
        });
    };
    support.method(player, "seekToTime:", [seek_with](CpuState& s, GuestMemory& m) { seek_with(s, m, 0); }, "v@:{?=qiIq}");
    support.method(player, "seekToTime:completionHandler:", [seek_with](CpuState& s, GuestMemory& m) { seek_with(s, m, s.x[3]); }, "v@:{?=qiIq}@?");
    support.method(player, "seekToTime:toleranceBefore:toleranceAfter:", [seek_with](CpuState& s, GuestMemory& m) { seek_with(s, m, 0); }, "v@:{?=qiIq}{?=qiIq}{?=qiIq}");
    support.method(player, "seekToTime:toleranceBefore:toleranceAfter:completionHandler:", [seek_with](CpuState& s, GuestMemory& m) { seek_with(s, m, s.x[5]); },
                   "v@:{?=qiIq}{?=qiIq}{?=qiIq}@?");
    support.method(item, "seekToTime:completionHandler:", [self, seek_with](CpuState& s, GuestMemory& m) {
        const Address owner = self->owner_of(s.x[0]);
        if (owner) { s.x[0] = owner; seek_with(s, m, s.x[3]); }
        else if (s.x[3]) static_cast<void>(self->bridge_->invoke_block(s.x[3], {0}));
    }, "v@:{?=qiIq}@?");
    support.method(player, "volume", [self](CpuState& s, GuestMemory&) { s.v[0] = {std::bit_cast<std::uint32_t>(self->state<PlayerState>(s.x[0])->volume), 0}; }, "f@:");
    support.method(player, "setVolume:", [self](CpuState& s, GuestMemory&) {
        const auto p = self->state<PlayerState>(s.x[0]);
        p->volume = std::clamp(std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0])), 0.0f, 1.0f);
        std::lock_guard lock(*p->audio_mutex);
        p->audio_out->gain = p->muted ? 0.0f : p->volume;
    }, "v@:f");
    support.method(player, "isMuted", [self](CpuState& s, GuestMemory&) { s.x[0] = self->state<PlayerState>(s.x[0])->muted ? 1 : 0; }, "B@:");
    support.method(player, "setMuted:", [self](CpuState& s, GuestMemory&) {
        const auto p = self->state<PlayerState>(s.x[0]);
        p->muted = (s.x[2] & 0xff) != 0;
        std::lock_guard lock(*p->audio_mutex);
        p->audio_out->gain = p->muted ? 0.0f : p->volume;
    }, "v@:B");
    support.method(player, "actionAtItemEnd", [self](CpuState& s, GuestMemory&) { s.x[0] = static_cast<std::uint64_t>(self->state<PlayerState>(s.x[0])->action_at_end); }, "q@:");
    support.method(player, "setActionAtItemEnd:", [self](CpuState& s, GuestMemory&) { self->state<PlayerState>(s.x[0])->action_at_end = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    for (const auto* name : {"automaticallyWaitsToMinimizeStalling", "allowsExternalPlayback"})
        support.method(player, name, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    for (const auto* name : {"setAutomaticallyWaitsToMinimizeStalling:", "setAllowsExternalPlayback:", "setPreventsDisplaySleepDuringVideoPlayback:"})
        support.method(player, name, [](CpuState&, GuestMemory&) {}, "v@:B");
    // -addPeriodicTimeObserverForInterval:queue:usingBlock: (CMTime by reference) and
    // -addBoundaryTimeObserverForTimes:queue:usingBlock: (NSValues of CMTimes): a token for
    // -removeTimeObserver:. Periodic blocks also run once when added.
    support.method(player, "addPeriodicTimeObserverForInterval:queue:usingBlock:", [self](CpuState& s, GuestMemory& m) {
        Observer o;
        o.periodic = true;
        o.interval = seconds_of(read_time(m, s.x[2]));
        o.queue = s.x[3];
        if (o.queue) static_cast<void>(self->support_.call(libsystem, "_dispatch_retain", {o.queue}));
        o.block = self->bridge_->block_copy(s.x[4]);
        o.token = self->support_.send_id(self->support_.send_id(self->token_class_, "alloc"), "init");
        const auto now = self->current_time(s.x[0]);
        o.next_fire = o.interval > 0 ? (std::floor(now / o.interval) + 1) * o.interval : 0;
        self->invoke_time_block(o.queue, o.block, now);
        self->state<PlayerState>(s.x[0])->observers.push_back(o);
        s.x[0] = self->support_.send_id(self->retain(o.token), "autorelease");
    }, "@@:{?=qiIq}@@?");
    support.method(player, "addBoundaryTimeObserverForTimes:queue:usingBlock:", [self](CpuState& s, GuestMemory& m) {
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
        self->state<PlayerState>(s.x[0])->observers.push_back(o);
        s.x[0] = self->support_.send_id(self->retain(o.token), "autorelease");
    }, "@@:@@@?");
    support.method(player, "removeTimeObserver:", [self](CpuState& s, GuestMemory&) {
        auto& observers = self->state<PlayerState>(s.x[0])->observers;
        const auto found = std::find_if(observers.begin(), observers.end(), [&](const Observer& o) { return o.token == s.x[2]; });
        if (found == observers.end()) return;
        self->release(found->token);
        self->bridge_->block_release(found->block);
        if (found->queue) static_cast<void>(self->support_.call(libsystem, "_dispatch_release", {found->queue}));
        observers.erase(found);
    }, "v@:@");

    // ---- AVPlayerLayer ------------------------------------------------------------------------
    support.method(layer, "playerLayerWithPlayer:", [self](CpuState& s, GuestMemory&) {
        const auto made = self->support_.send_id(self->support_.send_id(s.x[0], "alloc"), "init");
        static_cast<void>(self->support_.send_id(made, "setPlayer:", {s.x[2]}));
        s.x[0] = self->support_.send_id(made, "autorelease");
    }, "@@:@", true);
    support.method(layer, "player", [self](CpuState& s, GuestMemory&) { s.x[0] = self->layer_state(s.x[0])->player; }, "@@:");
    support.method(layer, "setPlayer:", [self](CpuState& s, GuestMemory&) {
        const auto l = self->layer_state(s.x[0]);
        if (l->player == s.x[2]) return;
        if (l->player) {
            auto& layers = self->state<PlayerState>(l->player)->layers;
            layers.erase(std::remove(layers.begin(), layers.end(), s.x[0]), layers.end());
        }
        const auto old = std::exchange(l->player, self->retain(s.x[2]));
        self->release(old);
        if (s.x[2]) {
            self->state<PlayerState>(s.x[2])->layers.push_back(s.x[0]);
            self->show(s.x[2]);
        }
    }, "v@:@");
    support.method(layer, "dealloc", [self, layer](CpuState& s, GuestMemory&) {
        if (const auto found = self->layers_.find(s.x[0]); found != self->layers_.end()) {
            const auto l = found->second;
            self->layers_.erase(found);
            if (l->player) {
                auto& layers = self->state<PlayerState>(l->player)->layers;
                layers.erase(std::remove(layers.begin(), layers.end(), s.x[0]), layers.end());
            }
            self->release(std::exchange(l->player, 0));
            self->release(std::exchange(l->gravity, 0));
        }
        static_cast<void>(self->support_.objc.send_super(s.x[0], layer, self->support_.objc.selector("dealloc")));
    }, "v@:");
    support.method(layer, "isReadyForDisplay", [self](CpuState& s, GuestMemory&) { s.x[0] = self->layer_state(s.x[0])->ready ? 1 : 0; }, "B@:");
    // -videoGravity: AVLayerVideoGravityResizeAspect unless set; it sets the layer's contentsGravity.
    support.method(layer, "videoGravity", [self](CpuState& s, GuestMemory&) {
        const auto l = self->layer_state(s.x[0]);
        s.x[0] = l->gravity ? l->gravity : self->support_.cf.constant_string("AVLayerVideoGravityResizeAspect");
    }, "@@:");
    support.method(layer, "setVideoGravity:", [self](CpuState& s, GuestMemory&) {
        const auto l = self->layer_state(s.x[0]);
        const auto old = std::exchange(l->gravity, s.x[2] ? self->support_.cf.copy(s.x[2]) : 0);
        self->release(old);
        const auto name = s.x[2] ? self->support_.cf.utf8(s.x[2]) : std::string();
        const char* contents = name == "AVLayerVideoGravityResize" ? "resize" : name == "AVLayerVideoGravityResizeAspectFill" ? "resizeAspectFill" : "resizeAspect";
        static_cast<void>(self->support_.send_id(s.x[0], "setContentsGravity:", {self->support_.cf.constant_string(contents)}));
    }, "v@:@");
    // CGRect -videoRect (d0-d3): the picture fitted to the bounds by the gravity.
    support.method(layer, "videoRect", [self](CpuState& s, GuestMemory&) {
        const auto l = self->layer_state(s.x[0]);
        const auto bounds = self->support_.send(s.x[0], "bounds", {});
        double x = bounds.d(0), y = bounds.d(1), w = bounds.d(2), h = bounds.d(3);
        const auto item = l->player ? self->state<PlayerState>(l->player)->item : 0;
        const auto st = item ? self->state<ItemState>(item) : nullptr;
        if (!st || st->width <= 0 || st->height <= 0) { w = h = 0; }
        else {
            const auto name = l->gravity ? self->support_.cf.utf8(l->gravity) : std::string("AVLayerVideoGravityResizeAspect");
            if (name != "AVLayerVideoGravityResize") {
                const double scale = name == "AVLayerVideoGravityResizeAspectFill" ? std::max(w / st->width, h / st->height) : std::min(w / st->width, h / st->height);
                const double fw = st->width * scale, fh = st->height * scale;
                x += (w - fw) / 2;
                y += (h - fh) / 2;
                w = fw;
                h = fh;
            }
        }
        for (unsigned i = 0; i < 4; ++i) s.v[i] = {std::bit_cast<std::uint64_t>(std::array<double, 4>{x, y, w, h}[i]), 0};
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");

    // Constants (AVLayerVideoGravityResize is video.cpp's).
    for (const auto& [symbol, value] : std::array<std::pair<const char*, const char*>, 5>{{
             {"_AVLayerVideoGravityResizeAspect", "AVLayerVideoGravityResizeAspect"},
             {"_AVLayerVideoGravityResizeAspectFill", "AVLayerVideoGravityResizeAspectFill"},
             {"_AVPlayerItemDidPlayToEndTimeNotification", "AVPlayerItemDidPlayToEndTimeNotification"},
             {"_AVPlayerItemFailedToPlayToEndTimeNotification", "AVPlayerItemFailedToPlayToEndTimeNotification"},
             {"_AVPlayerItemTimeJumpedNotification", "AVPlayerItemTimeJumpedNotification"}}})
        bridge_->export_string(av_foundation, symbol, value);
}

} // namespace

void install_av_player(Support& support) { std::make_shared<Players>(support)->install(); }

} // namespace alchemy::gui::completion_kit::media
