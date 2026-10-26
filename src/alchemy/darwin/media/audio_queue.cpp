#include "alchemy/darwin/media/media_support.hpp"
#include "alchemy/darwin/media/audio_file_core.hpp"
#include "alchemy/darwin/media/hal_io.hpp"
#include "alchemy/darwin/media/resampler.hpp"

#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/gui/runloop.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

// AudioQueue output (<AudioToolbox/AudioQueue.h>) on the default output device's I/O
// (hal_io.hpp). A queue plays linear PCM in any layout and rate (converted and resampled to the
// device, mono to every channel): enqueued buffers play in order, and each one goes back to its
// callback once played (or dropped by AudioQueueReset, AudioQueueStop(immediate) or
// AudioQueueDispose(immediate)). Callbacks run on the main queue for a queue made on the main
// run loop, on the given dispatch queue for AudioQueueNewOutputWithDispatchQueue, and otherwise
// on the queue's own thread. Volume and pan are applied; play rate and pitch are kept
// (kAudioQueueParam_PlayRate applies with kAudioQueueProperty_EnableTimePitch only on macOS, and
// here it is not applied). Unavailable: compressed formats (kAudioFormatUnsupportedDataFormatError:
// there is no decoder), input queues, offline rendering and processing taps.
namespace alchemy::gui::completion_kit::media {
namespace {

namespace af = audio_file;

constexpr std::string_view audio_toolbox = "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox";
constexpr std::string_view core_foundation = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr std::uint32_t four(const char (&code)[5]) { return af::four(code); }

namespace error {
inline constexpr std::int32_t invalid_buffer = -66687, buffer_empty = -66686, invalid_property = -66684, invalid_property_size = -66683,
    invalid_parameter = -66682, invalid_device = -66680, buffer_in_queue = -66679, invalid_run_state = -66678,
    unsupported_format = static_cast<std::int32_t>(four("fmt?")), param = -50;
}

// Parameters and properties.
constexpr std::uint32_t param_volume = 1, param_play_rate = 2, param_pitch = 3, param_volume_ramp_time = 4, param_pan = 13;
constexpr std::uint32_t prop_is_running = four("aqrn"), prop_device_sample_rate = four("aqsr"), prop_device_channels = four("aqdc"),
    prop_current_device = four("aqcd"), prop_magic_cookie = four("aqmc"), prop_maximum_output_packet_size = four("xops"),
    prop_stream_description = four("aqft"), prop_channel_layout = four("aqcl"), prop_enable_level_metering = four("aqme"),
    prop_current_level_meter = four("aqmv"), prop_current_level_meter_db = four("aqmd"), prop_decode_buffer_size_frames = four("dcbf"),
    prop_converter_error = four("qcve"), prop_enable_time_pitch = four("q_tp"), prop_time_pitch_algorithm = four("qtpa"),
    prop_time_pitch_bypass = four("qtpb");
// AudioQueueBuffer layout.
constexpr std::uint64_t buffer_capacity = 0, buffer_data = 8, buffer_size = 16, buffer_user_data = 24, buffer_description_capacity = 32,
    buffer_descriptions = 40, buffer_description_count = 48, buffer_struct = 56;

struct Buffer {
    std::uint64_t address{};           // the AudioQueueBuffer (with its data after it)
    std::uint32_t frames{}, played{};  // while enqueued
    std::uint32_t trim_start{}, trim_end{};
};

struct Listener { std::uint32_t property{}; std::uint64_t proc{}, data{}; };

struct Queue {
    std::uint64_t handle{};
    af::Pcm format;
    std::vector<std::uint8_t> asbd;
    std::uint64_t callback{}, user_data{}, block{}, dispatch_queue{};
    bool main_queue{};
    std::mutex mutex;
    std::map<std::uint64_t, std::uint32_t> buffers;   // allocated: address -> capacity
    std::deque<Buffer> queued;
    bool running{}, paused{}, draining{};             // draining: stop once the queue is empty
    double volume{1}, pan{}, play_rate{1}, pitch{}, ramp{};
    bool metering{};
    std::vector<double> peaks, averages;
    std::vector<Listener> listeners;
    std::shared_ptr<hal_io::DeviceIo> io;
    std::unique_ptr<Resampler> resampler;   // used by the render only; others ask for a reset
    bool reset_pending{};
    bool disposed{};                   // callbacks still posted are dropped
    std::thread::id callback_thread;   // the queue's own thread, once a callback ran there
    double sample_time{};
    std::shared_ptr<CompletionThread> thread;        // callbacks without a run loop or dispatch queue
};

class Queues : public std::enable_shared_from_this<Queues> {
public:
    explicit Queues(Support& support) : support_(support), bridge_(security::bridge()) {}
    void install();

private:
    std::shared_ptr<Queue> find(std::uint64_t handle) {
        std::lock_guard lock(mutex_);
        const auto found = queues_.find(handle);
        return found == queues_.end() ? nullptr : found->second;
    }
    std::uint64_t call(std::uint64_t function, std::initializer_list<std::uint64_t> arguments) {
        return EngineAdapter(support_.process.calling_engine()).call(function, std::span(arguments.begin(), arguments.size()));
    }
    // Hands `buffer` back to the queue's callback where the queue's callbacks run.
    void give_back(const std::shared_ptr<Queue>& q, std::uint64_t buffer);
    void notify(const std::shared_ptr<Queue>& q, std::uint32_t property);
    void render(const std::shared_ptr<Queue>& q, float* mix, std::uint32_t frames, std::uint32_t channels, const hal_io::Timing& timing);
    std::int32_t start(const std::shared_ptr<Queue>& q);
    void stop_now(const std::shared_ptr<Queue>& q, bool give_back_buffers);

    Support& support_;
    std::shared_ptr<security::Bridge> bridge_;
    std::mutex mutex_;
    std::map<std::uint64_t, std::shared_ptr<Queue>> queues_;
};

void Queues::give_back(const std::shared_ptr<Queue>& q, std::uint64_t buffer) {
    auto self = shared_from_this();
    const auto run = [self, q, buffer] {
        {
            std::lock_guard lock(q->mutex);
            if (q->disposed) return;
            if (!q->dispatch_queue && !q->main_queue) q->callback_thread = std::this_thread::get_id();
        }
        if (q->block) static_cast<void>(self->bridge_->invoke_block(q->block, {q->handle, buffer}));
        else if (q->callback) static_cast<void>(self->call(q->callback, {q->user_data, q->handle, buffer}));
    };
    if (q->dispatch_queue) return bridge_->dispatch_async(q->dispatch_queue, run);
    if (q->main_queue) return bridge_->dispatch_async(bridge_->main_queue(), run);
    std::shared_ptr<CompletionThread> thread;
    {
        std::lock_guard lock(q->mutex);
        if (!q->thread) q->thread = start_completion_thread(support_.process, "com.apple.audio.AudioQueue");
        thread = q->thread;
    }
    thread->post(run);
}

void Queues::notify(const std::shared_ptr<Queue>& q, std::uint32_t property) {
    std::vector<Listener> listeners;
    {
        std::lock_guard lock(q->mutex);
        for (const auto& l : q->listeners)
            if (l.property == property) listeners.push_back(l);
    }
    for (const auto& l : listeners) static_cast<void>(call(l.proc, {l.data, q->handle, property}));
}

void Queues::render(const std::shared_ptr<Queue>& q, float* mix, std::uint32_t frames, std::uint32_t channels, const hal_io::Timing& timing) {
    auto& m = support_.process.calling_engine();
    std::vector<std::uint64_t> finished;
    bool ran_dry = false;
    const auto pull = [&](double* out, std::size_t count) {
        std::size_t done = 0;
        while (done < count) {
            Buffer current;
            {
                std::lock_guard lock(q->mutex);
                if (q->queued.empty() || q->paused) break;
                current = q->queued.front();
            }
            const auto end = current.frames - current.trim_end;
            const auto start = std::max(current.played, current.trim_start);
            const auto take = static_cast<std::uint32_t>(std::min<std::size_t>(count - done, end > start ? end - start : 0));
            if (take) {
                const auto data = m.read64(current.address + buffer_data);
                const auto bytes = static_cast<std::size_t>(take) * q->format.frame_bytes();
                std::vector<std::vector<std::uint8_t>> storage;
                std::vector<std::span<const std::uint8_t>> spans;
                const auto planes = q->format.non_interleaved ? q->format.channels : 1U;
                const auto plane_bytes = static_cast<std::size_t>(current.frames) * q->format.frame_bytes();
                for (std::uint32_t p = 0; p < planes; ++p) {
                    storage.emplace_back(bytes);
                    m.read_bytes(data + p * plane_bytes + static_cast<std::size_t>(start) * q->format.frame_bytes(), storage.back());
                    spans.emplace_back(storage.back());
                }
                const auto samples = af::decode(q->format, spans, take);
                for (std::uint32_t f = 0; f < take; ++f)
                    for (std::uint32_t c = 0; c < channels; ++c) {
                        const auto source = q->format.channels == 1 ? 0U : c;
                        out[(done + f) * channels + c] = source < q->format.channels ? samples[f * q->format.channels + source] : 0.0;
                    }
                done += take;
            }
            std::lock_guard lock(q->mutex);
            if (q->queued.empty() || q->queued.front().address != current.address) continue;
            q->queued.front().played = start + take;
            if (start + take >= end) {
                finished.push_back(current.address);
                q->queued.pop_front();
            }
        }
        if (done < count) ran_dry = true;
        std::fill(out + done * channels, out + count * channels, 0.0);
    };
    {
        std::lock_guard lock(q->mutex);
        if (!q->resampler) q->resampler = std::make_unique<Resampler>(q->format.rate, timing.rate, channels);
        else if (q->reset_pending) q->resampler->reset();
        q->reset_pending = false;
    }
    std::vector<double> converted(static_cast<std::size_t>(frames) * channels);
    q->resampler->render(converted.data(), frames, pull);
    double volume{}, pan{};
    bool metering{};
    {
        std::lock_guard lock(q->mutex);
        volume = q->volume;
        pan = q->pan;
        metering = q->metering;
        q->sample_time += frames * q->resampler->ratio();
    }
    std::vector<double> peaks(channels), sums(channels);
    for (std::size_t f = 0; f < frames; ++f)
        for (std::uint32_t c = 0; c < channels; ++c) {
            double gain = volume;
            if (channels == 2) gain *= c == 0 ? std::min(1.0, 1.0 - pan) : std::min(1.0, 1.0 + pan);
            const auto value = converted[f * channels + c] * gain;
            mix[f * channels + c] += static_cast<float>(value);
            peaks[c] = std::max(peaks[c], std::abs(value));
            sums[c] += value * value;
        }
    if (metering) {
        std::lock_guard lock(q->mutex);
        q->peaks = peaks;
        q->averages.resize(channels);
        for (std::uint32_t c = 0; c < channels; ++c) q->averages[c] = std::sqrt(sums[c] / std::max<std::size_t>(frames, 1));
    }
    for (const auto buffer : finished) give_back(q, buffer);
    bool stop = false;
    {
        std::lock_guard lock(q->mutex);
        stop = ran_dry && q->draining && q->queued.empty();
    }
    if (stop) {
        // Every buffer played after AudioQueueStop(false): the queue stops (off the I/O thread's
        // render, through the device, which allows a client to stop itself).
        stop_now(q, false);
    }
}

std::int32_t Queues::start(const std::shared_ptr<Queue>& q) {
    {
        std::lock_guard lock(q->mutex);
        q->paused = false;
        q->draining = false;
        if (q->running) return 0;
    }
    const auto device = hal_default_output();
    const auto io = hal_device_io(device);
    if (!io) return error::invalid_device;
    auto self = shared_from_this();
    const auto channels = io->channels();
    const auto status = io->start(q->handle, [self, q, channels](float* mix, std::uint32_t frames, const hal_io::Timing& timing) {
        self->render(q, mix, frames, channels, timing);
    }, hal_buffer_frames(device));
    if (status) return status;
    {
        std::lock_guard lock(q->mutex);
        q->io = io;
        q->running = true;
    }
    notify(q, prop_is_running);
    return 0;
}

void Queues::stop_now(const std::shared_ptr<Queue>& q, bool give_back_buffers) {
    std::shared_ptr<hal_io::DeviceIo> io;
    std::vector<std::uint64_t> dropped;
    bool was_running{};
    {
        std::lock_guard lock(q->mutex);
        was_running = q->running;
        q->running = false;
        q->draining = false;
        io = std::move(q->io);
        if (give_back_buffers)
            for (const auto& b : q->queued) dropped.push_back(b.address);
        if (give_back_buffers) q->queued.clear();
        q->reset_pending = true;
    }
    if (io) io->stop(q->handle);
    for (const auto buffer : dropped) give_back(q, buffer);
    if (was_running) notify(q, prop_is_running);
}

void Queues::install() {
    auto self = shared_from_this();
    const auto status = [](CpuState& s, std::int32_t value) { s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(value)); };
    const auto make = [self](GuestMemory& m, std::uint64_t asbd, std::uint64_t out, const std::function<void(Queue&)>& setup) -> std::int32_t {
        if (out) m.write64(out, 0);
        if (!asbd || !out) return error::param;
        std::vector<std::uint8_t> bytes(40);
        m.read_bytes(asbd, bytes);
        std::string why;
        const auto format = af::pcm_from_asbd(bytes, why);
        if (!format) return error::unsupported_format;
        auto q = std::make_shared<Queue>();
        q->format = *format;
        q->asbd = bytes;
        setup(*q);
        q->handle = self->support_.process.allocate_guest(16, 16);
        m.fill_bytes(q->handle, 0, 16);
        {
            std::lock_guard lock(self->mutex_);
            self->queues_[q->handle] = q;
        }
        m.write64(out, q->handle);
        return 0;
    };
    // AudioQueueNewOutput(format, callback, userData, runLoop, runLoopMode, flags, AudioQueueRef *out)
    support_.function(audio_toolbox, "_AudioQueueNewOutput", [self, make, status](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) {
            if (s.x[6]) m.write64(s.x[6], 0);
            return status(s, error::param);
        }
        const auto main_loop = s.x[3] && s.x[3] == self->support_.call(core_foundation, "_CFRunLoopGetMain", {});
        status(s, make(m, s.x[0], s.x[6], [&](Queue& q) {
            q.callback = s.x[1];
            q.user_data = s.x[2];
            q.main_queue = main_loop;
        }));
    });
    // AudioQueueNewOutputWithDispatchQueue(AudioQueueRef *out, format, flags, queue, block)
    support_.function(audio_toolbox, "_AudioQueueNewOutputWithDispatchQueue", [self, make, status](CpuState& s, GuestMemory& m) {
        if (!s.x[3] || !s.x[4]) {
            if (s.x[0]) m.write64(s.x[0], 0);
            return status(s, error::param);
        }
        status(s, make(m, s.x[1], s.x[0], [&](Queue& q) {
            q.block = self->bridge_->block_copy(s.x[4]);
            q.dispatch_queue = self->bridge_->retain(s.x[3]);
        }));
    });
    support_.function(audio_toolbox, "_AudioQueueDispose", [self, status](CpuState& s, GuestMemory&) {
        std::shared_ptr<Queue> q;
        {
            std::lock_guard lock(self->mutex_);
            const auto found = self->queues_.find(s.x[0]);
            if (found == self->queues_.end()) return status(s, error::param);
            q = std::move(found->second);
            self->queues_.erase(found);
        }
        self->stop_now(q, false);
        std::vector<std::uint64_t> buffers;
        bool on_own_thread{};
        {
            std::lock_guard lock(q->mutex);
            q->disposed = true;
            on_own_thread = q->callback_thread == std::this_thread::get_id();
            for (const auto& [address, capacity] : q->buffers) buffers.push_back(address);
            q->buffers.clear();
            q->queued.clear();
        }
        for (const auto address : buffers) self->support_.process.release_guest(address);
        if (q->block) self->bridge_->block_release(q->block);
        if (q->dispatch_queue) self->bridge_->release(q->dispatch_queue);
        // Disposed from its own callback: the thread cannot join itself, so the main queue does.
        if (q->thread && on_own_thread) self->bridge_->dispatch_async(self->bridge_->main_queue(), [thread = q->thread] { thread->shutdown(); });
        else if (q->thread) q->thread->shutdown();
        self->support_.process.release_guest(q->handle);
        status(s, 0);
    });
    const auto allocate = [self](GuestMemory& m, const std::shared_ptr<Queue>& q, std::uint32_t size, std::uint32_t descriptions, std::uint64_t out) {
        const auto total = buffer_struct + size + 16ULL * descriptions;
        const auto address = self->support_.process.allocate_guest(total, 16);
        m.fill_bytes(address, 0, total);
        m.write32(address + buffer_capacity, size);
        m.write64(address + buffer_data, address + buffer_struct);
        m.write32(address + buffer_description_capacity, descriptions);
        if (descriptions) m.write64(address + buffer_descriptions, address + buffer_struct + size);
        {
            std::lock_guard lock(q->mutex);
            q->buffers[address] = size;
        }
        m.write64(out, address);
    };
    // AudioQueueAllocateBuffer(aq, size, AudioQueueBufferRef *out)
    support_.function(audio_toolbox, "_AudioQueueAllocateBuffer", [self, allocate, status](CpuState& s, GuestMemory& m) {
        const auto q = self->find(s.x[0]);
        if (!q) return status(s, error::param);
        if (!s.x[2]) return status(s, error::param);
        allocate(m, q, static_cast<std::uint32_t>(s.x[1]), 0, s.x[2]);
        status(s, 0);
    });
    // AudioQueueAllocateBufferWithPacketDescriptions(aq, size, numberPacketDescriptions, out)
    support_.function(audio_toolbox, "_AudioQueueAllocateBufferWithPacketDescriptions", [self, allocate, status](CpuState& s, GuestMemory& m) {
        const auto q = self->find(s.x[0]);
        if (!q) return status(s, error::param);
        if (!s.x[3]) return status(s, error::param);
        allocate(m, q, static_cast<std::uint32_t>(s.x[1]), static_cast<std::uint32_t>(s.x[2]), s.x[3]);
        status(s, 0);
    });
    support_.function(audio_toolbox, "_AudioQueueFreeBuffer", [self, status](CpuState& s, GuestMemory&) {
        const auto q = self->find(s.x[0]);
        if (!q) return status(s, error::param);
        {
            std::lock_guard lock(q->mutex);
            if (std::any_of(q->queued.begin(), q->queued.end(), [&](const Buffer& b) { return b.address == s.x[1]; }))
                return status(s, error::buffer_in_queue);
            if (!q->buffers.erase(s.x[1])) return status(s, error::invalid_buffer);
        }
        self->support_.process.release_guest(s.x[1]);
        status(s, 0);
    });
    const auto enqueue = [self](CpuState& s, GuestMemory& m, std::uint32_t trim_start, std::uint32_t trim_end) -> std::int32_t {
        const auto q = self->find(s.x[0]);
        if (!q) return error::param;
        const auto buffer = s.x[1];
        std::lock_guard lock(q->mutex);
        if (!q->buffers.contains(buffer)) return error::invalid_buffer;
        const auto bytes = m.read32(buffer + buffer_size);
        const auto per_frame = q->format.frame_bytes() * (q->format.non_interleaved ? q->format.channels : 1U);
        if (!bytes) return error::buffer_empty;
        if (bytes > q->buffers[buffer]) return error::invalid_buffer;
        Buffer b;
        b.address = buffer;
        b.frames = bytes / per_frame;
        b.trim_start = std::min(trim_start, b.frames);
        b.trim_end = std::min(trim_end, b.frames - b.trim_start);
        q->queued.push_back(b);
        return 0;
    };
    // AudioQueueEnqueueBuffer(aq, buffer, numPacketDescs, descs)
    support_.function(audio_toolbox, "_AudioQueueEnqueueBuffer", [enqueue, status](CpuState& s, GuestMemory& m) { status(s, enqueue(s, m, 0, 0)); });
    // AudioQueueEnqueueBufferWithParameters(aq, buffer, numPacketDescs, descs, trimFramesAtStart,
    //   trimFramesAtEnd, numParamValues, paramValues, startTime, actualStartTime)
    support_.function(audio_toolbox, "_AudioQueueEnqueueBufferWithParameters", [enqueue, status](CpuState& s, GuestMemory& m) {
        status(s, enqueue(s, m, static_cast<std::uint32_t>(s.x[4]), static_cast<std::uint32_t>(s.x[5])));
    });
    // AudioQueueStart(aq, startTime): now (a future start time is not honored: playback starts now).
    support_.function(audio_toolbox, "_AudioQueueStart", [self, status](CpuState& s, GuestMemory&) {
        const auto q = self->find(s.x[0]);
        if (!q) return status(s, error::param);
        status(s, self->start(q));
    });
    support_.function(audio_toolbox, "_AudioQueuePrime", [self, status](CpuState& s, GuestMemory& m) {
        const auto q = self->find(s.x[0]);
        if (!q) return status(s, error::param);
        if (s.x[2]) m.write32(s.x[2], static_cast<std::uint32_t>(s.x[1]));
        status(s, 0);
    });
    // AudioQueueStop(aq, immediate): immediately drops the queued buffers (back to the callback);
    // otherwise the queue plays them out and then stops.
    support_.function(audio_toolbox, "_AudioQueueStop", [self, status](CpuState& s, GuestMemory&) {
        const auto q = self->find(s.x[0]);
        if (!q) return status(s, error::param);
        if ((s.x[1] & 0xFF) != 0) {
            self->stop_now(q, true);
        } else {
            bool empty{};
            {
                std::lock_guard lock(q->mutex);
                q->draining = true;
                empty = q->queued.empty();
            }
            if (empty) self->stop_now(q, false);
        }
        status(s, 0);
    });
    support_.function(audio_toolbox, "_AudioQueuePause", [self, status](CpuState& s, GuestMemory&) {
        const auto q = self->find(s.x[0]);
        if (!q) return status(s, error::param);
        std::lock_guard lock(q->mutex);
        q->paused = true;
        status(s, 0);
    });
    support_.function(audio_toolbox, "_AudioQueueFlush", [self, status](CpuState& s, GuestMemory&) {
        status(s, self->find(s.x[0]) ? 0 : error::param);
    });
    support_.function(audio_toolbox, "_AudioQueueReset", [self, status](CpuState& s, GuestMemory&) {
        const auto q = self->find(s.x[0]);
        if (!q) return status(s, error::param);
        std::vector<std::uint64_t> dropped;
        {
            std::lock_guard lock(q->mutex);
            for (const auto& b : q->queued) dropped.push_back(b.address);
            q->queued.clear();
            q->reset_pending = true;
        }
        for (const auto buffer : dropped) self->give_back(q, buffer);
        status(s, 0);
    });
    // AudioQueueGetParameter(aq, id, AudioQueueParameterValue *out)
    support_.function(audio_toolbox, "_AudioQueueGetParameter", [self, status](CpuState& s, GuestMemory& m) {
        const auto q = self->find(s.x[0]);
        if (!q) return status(s, error::param);
        if (!s.x[2]) return status(s, error::param);
        std::lock_guard lock(q->mutex);
        double value{};
        switch (static_cast<std::uint32_t>(s.x[1])) {
        case param_volume: value = q->volume; break;
        case param_play_rate: value = q->play_rate; break;
        case param_pitch: value = q->pitch; break;
        case param_volume_ramp_time: value = q->ramp; break;
        case param_pan: value = q->pan; break;
        default: return status(s, error::invalid_parameter);
        }
        m.write32(s.x[2], std::bit_cast<std::uint32_t>(static_cast<float>(value)));
        status(s, 0);
    });
    // AudioQueueSetParameter(aq, id, AudioQueueParameterValue value (s0))
    support_.function(audio_toolbox, "_AudioQueueSetParameter", [self, status](CpuState& s, GuestMemory&) {
        const auto q = self->find(s.x[0]);
        if (!q) return status(s, error::param);
        const double value = std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0]));
        std::lock_guard lock(q->mutex);
        switch (static_cast<std::uint32_t>(s.x[1])) {
        case param_volume: q->volume = std::clamp(value, 0.0, 1.0); break;
        case param_play_rate: q->play_rate = std::clamp(value, 0.5, 2.0); break;
        case param_pitch: q->pitch = std::clamp(value, -2400.0, 2400.0); break;
        case param_volume_ramp_time: q->ramp = std::max(value, 0.0); break;
        case param_pan: q->pan = std::clamp(value, -1.0, 1.0); break;
        default: return status(s, error::invalid_parameter);
        }
        status(s, 0);
    });
    const auto property = [self](Queue& q, std::uint32_t id, std::vector<std::uint8_t>& out) -> std::int32_t {
        const auto u32 = [&](std::uint32_t value) { out.resize(4); std::memcpy(out.data(), &value, 4); };
        const auto device = hal_device_io(hal_default_output());
        switch (id) {
        case prop_is_running: u32(q.running ? 1 : 0); return 0;
        case prop_device_sample_rate: { const double rate = device ? device->rate() : 0; out.resize(8); std::memcpy(out.data(), &rate, 8); return 0; }
        case prop_device_channels: u32(device ? device->channels() : 0); return 0;
        case prop_stream_description: out = q.asbd; return 0;
        case prop_maximum_output_packet_size: u32(q.format.frame_bytes() * (q.format.non_interleaved ? q.format.channels : 1U)); return 0;
        case prop_enable_level_metering: u32(q.metering ? 1 : 0); return 0;
        case prop_decode_buffer_size_frames: u32(2048); return 0;
        case prop_converter_error: u32(0); return 0;
        case prop_enable_time_pitch: case prop_time_pitch_bypass: u32(0); return 0;
        case prop_current_level_meter: case prop_current_level_meter_db: {
            // AudioQueueLevelMeterState {Float32 mAveragePower; Float32 mPeakPower} per channel.
            const auto channels = std::max<std::size_t>(q.peaks.size(), q.format.channels);
            out.assign(channels * 8, 0);
            for (std::size_t c = 0; c < q.peaks.size(); ++c) {
                auto average = static_cast<float>(c < q.averages.size() ? q.averages[c] : 0.0), peak = static_cast<float>(q.peaks[c]);
                if (id == prop_current_level_meter_db) {
                    average = average > 0 ? 20 * std::log10(average) : -120.0f;
                    peak = peak > 0 ? 20 * std::log10(peak) : -120.0f;
                }
                std::memcpy(out.data() + c * 8, &average, 4);
                std::memcpy(out.data() + c * 8 + 4, &peak, 4);
            }
            return 0;
        }
        default: return error::invalid_property;
        }
    };
    // AudioQueueGetPropertySize(aq, id, UInt32 *outSize)
    support_.function(audio_toolbox, "_AudioQueueGetPropertySize", [self, property, status](CpuState& s, GuestMemory& m) {
        const auto q = self->find(s.x[0]);
        if (!q) return status(s, error::param);
        std::vector<std::uint8_t> value;
        std::lock_guard lock(q->mutex);
        if (const auto result = property(*q, static_cast<std::uint32_t>(s.x[1]), value)) return status(s, result);
        if (s.x[2]) m.write32(s.x[2], static_cast<std::uint32_t>(value.size()));
        status(s, 0);
    });
    // AudioQueueGetProperty(aq, id, void *outData, UInt32 *ioDataSize)
    support_.function(audio_toolbox, "_AudioQueueGetProperty", [self, property, status](CpuState& s, GuestMemory& m) {
        const auto q = self->find(s.x[0]);
        if (!q) return status(s, error::param);
        if (!s.x[2] || !s.x[3]) return status(s, error::param);
        std::vector<std::uint8_t> value;
        std::lock_guard lock(q->mutex);
        if (static_cast<std::uint32_t>(s.x[1]) == prop_current_device) {
            if (m.read32(s.x[3]) < 8) return status(s, error::invalid_property_size);
            m.write64(s.x[2], 0);   // NULL: the default output device
            m.write32(s.x[3], 8);
            return status(s, 0);
        }
        if (const auto result = property(*q, static_cast<std::uint32_t>(s.x[1]), value)) return status(s, result);
        if (m.read32(s.x[3]) < value.size()) return status(s, error::invalid_property_size);
        if (!value.empty()) m.write_bytes(s.x[2], value);
        m.write32(s.x[3], static_cast<std::uint32_t>(value.size()));
        status(s, 0);
    });
    // AudioQueueSetProperty(aq, id, const void *data, UInt32 size)
    support_.function(audio_toolbox, "_AudioQueueSetProperty", [self, status](CpuState& s, GuestMemory& m) {
        const auto q = self->find(s.x[0]);
        if (!q) return status(s, error::param);
        const auto id = static_cast<std::uint32_t>(s.x[1]);
        std::uint32_t value{};
        if (s.x[2] && s.x[3] >= 4) value = m.read32(s.x[2]);
        std::lock_guard lock(q->mutex);
        switch (id) {
        case prop_enable_level_metering: q->metering = value != 0; break;
        case prop_current_device:
            // A device UID (CFString): only the default output plays here; NULL selects it.
            if (s.x[2] && s.x[3] >= 8 && m.read64(s.x[2])) return status(s, error::invalid_device);
            break;
        case prop_enable_time_pitch: case prop_time_pitch_algorithm: case prop_time_pitch_bypass: case prop_decode_buffer_size_frames:
        case prop_channel_layout: case prop_magic_cookie:
            break;
        default: return status(s, error::invalid_property);
        }
        status(s, 0);
    });
    // AudioQueueAddPropertyListener(aq, id, proc, userData)
    support_.function(audio_toolbox, "_AudioQueueAddPropertyListener", [self, status](CpuState& s, GuestMemory&) {
        const auto q = self->find(s.x[0]);
        if (!q) return status(s, error::param);
        if (!s.x[2]) return status(s, error::param);
        std::lock_guard lock(q->mutex);
        q->listeners.push_back({static_cast<std::uint32_t>(s.x[1]), s.x[2], s.x[3]});
        status(s, 0);
    });
    support_.function(audio_toolbox, "_AudioQueueRemovePropertyListener", [self, status](CpuState& s, GuestMemory&) {
        const auto q = self->find(s.x[0]);
        if (!q) return status(s, error::param);
        std::lock_guard lock(q->mutex);
        std::erase_if(q->listeners, [&](const Listener& l) {
            return l.property == static_cast<std::uint32_t>(s.x[1]) && l.proc == s.x[2] && l.data == s.x[3];
        });
        status(s, 0);
    });
    // AudioQueueGetCurrentTime(aq, timeline, AudioTimeStamp *out, Boolean *outDiscontinuity)
    support_.function(audio_toolbox, "_AudioQueueGetCurrentTime", [self, status](CpuState& s, GuestMemory& m) {
        const auto q = self->find(s.x[0]);
        if (!q) return status(s, error::param);
        if (!s.x[2]) return status(s, error::param);
        std::lock_guard lock(q->mutex);
        if (!q->running) return status(s, error::invalid_run_state);
        m.fill_bytes(s.x[2], 0, 64);
        m.write64(s.x[2], std::bit_cast<std::uint64_t>(q->sample_time));
        m.write64(s.x[2] + 8, host_time_now());
        m.write64(s.x[2] + 16, std::bit_cast<std::uint64_t>(1.0));
        m.write32(s.x[2] + 56, 7);
        if (s.x[3]) m.write8(s.x[3], 0);
        status(s, 0);
    });
    // AudioQueueCreateTimeline(aq, AudioQueueTimelineRef *out) / AudioQueueDisposeTimeline: the
    // timeline only answers "no discontinuity".
    support_.function(audio_toolbox, "_AudioQueueCreateTimeline", [self, status](CpuState& s, GuestMemory& m) {
        if (!self->find(s.x[0])) return status(s, error::param);
        if (!s.x[1]) return status(s, error::param);
        m.write64(s.x[1], s.x[0]);
        status(s, 0);
    });
    support_.function(audio_toolbox, "_AudioQueueDisposeTimeline", [self, status](CpuState& s, GuestMemory&) {
        status(s, self->find(s.x[0]) ? 0 : error::param);
    });
}

} // namespace

void install_audio_queue(Support& support) { std::make_shared<Queues>(support)->install(); }

} // namespace alchemy::gui::completion_kit::media
