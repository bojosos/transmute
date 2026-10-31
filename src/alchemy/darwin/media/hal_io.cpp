#include "alchemy/darwin/media/hal_io.hpp"

#include "alchemy/engine_adapter.hpp"
#include "alchemy/gui/runloop.hpp"

#include <algorithm>
#include <chrono>

namespace alchemy::gui::completion_kit::media::hal_io {
namespace {

constexpr std::int32_t unspecified_error = static_cast<std::int32_t>(hal::four("what"));

} // namespace

DeviceIo::DeviceIo(Support& support, hal::Backend& backend, std::uint32_t id, hal::Device device)
    : support_(support), backend_(backend), id_(id), device_(std::move(device)) {
    channels_ = std::max(device_.channels, 1U);
    rate_ = device_.sample_rate > 0 ? device_.sample_rate : 48000.0;
}

DeviceIo::~DeviceIo() {
    std::unique_ptr<hal::OutputStream> stream;
    std::shared_ptr<CompletionThread> thread;
    {
        std::lock_guard lock(mutex_);
        active_ = false;
        ++generation_;
        stream = std::move(stream_);
        thread = std::move(thread_);
    }
    wake_.notify_all();
    stream.reset();
    // Not from the completion thread itself (the last reference may be its loop's).
    if (thread && !on_io_thread()) thread->shutdown();
}

std::int32_t DeviceIo::start(std::uint64_t key, Client client, std::uint32_t buffer_frames) {
    std::unique_lock lock(mutex_);
    if (clients_.contains(key)) return 0;
    clients_.emplace(key, std::move(client));
    if (!active_ && !changing_) buffer_frames_ = std::clamp(buffer_frames, 15U, 8192U);
    settle(lock);
    // From inside a render while another thread changes the device, that thread starts it.
    if (active_ || changing_) return 0;
    clients_.erase(key);
    return unspecified_error;
}

void DeviceIo::stop(std::uint64_t key) {
    std::unique_lock lock(mutex_);
    if (!clients_.erase(key)) return;
    // A render in progress may still be running the client.
    if (!on_io_thread()) wake_.wait(lock, [this] { return !rendering_; });
    settle(lock);
}

void DeviceIo::settle(std::unique_lock<std::mutex>& lock) {
    if (changing_) {
        if (on_io_thread()) return;
        wake_.wait(lock, [this] { return !changing_; });
    }
    changing_ = true;
    for (;;) {
        if (!clients_.empty() && !active_) {
            if (!activate(lock)) break;
            continue;
        }
        if (clients_.empty() && active_) {
            deactivate(lock);
            continue;
        }
        break;
    }
    changing_ = false;
    wake_.notify_all();
}

bool DeviceIo::activate(std::unique_lock<std::mutex>& lock) {
    if (!stream_) {
        stream_ = backend_.open_output(device_, [this](float* samples, std::uint32_t frames, std::uint32_t queued) { take(samples, frames, queued); });
        if (!stream_) return false;
    }
    if (!thread_) {
        lock.unlock();
        auto thread = start_completion_thread(support_.process, "com.apple.audio.IOThread.client");
        lock.lock();
        if (!thread_) thread_ = std::move(thread);
    }
    active_ = true;
    const auto generation = ++generation_;
    fifo_.clear();
    host_queued_ = 0;
    thread_->post([self = shared_from_this(), generation] { self->loop(generation); });
    auto* stream = stream_.get();
    lock.unlock();
    const bool started = stream->start();
    lock.lock();
    if (started) return true;
    active_ = false;
    ++generation_;
    wake_.notify_all();
    if (!on_io_thread()) wake_.wait(lock, [this] { return loops_ == 0; });
    return false;
}

void DeviceIo::deactivate(std::unique_lock<std::mutex>& lock) {
    active_ = false;
    ++generation_;
    wake_.notify_all();
    // The I/O thread ends its loop after the render in progress; a client stopping the device
    // from inside its render does not wait for itself.
    if (!on_io_thread()) wake_.wait(lock, [this] { return loops_ == 0; });
    auto* stream = stream_.get();
    lock.unlock();
    if (stream) stream->stop();   // waits for take(), which takes the lock
    lock.lock();
    fifo_.clear();
    host_queued_ = 0;
}

bool DeviceIo::running(std::uint64_t key) const {
    std::lock_guard lock(mutex_);
    return active_ && clients_.contains(key);
}

bool DeviceIo::running() const {
    std::lock_guard lock(mutex_);
    return active_;
}

bool DeviceIo::now(Timing& out) const {
    std::lock_guard lock(mutex_);
    if (!active_) return false;
    out.rate = rate_;
    out.now_sample = static_cast<double>(consumed_);
    out.now_host = host_time_now();
    out.output_sample = static_cast<double>(produced_);
    out.output_host = out.now_host + static_cast<std::uint64_t>((fifo_.size() / channels_ + host_queued_) / rate_ * 1e9);
    return true;
}

void DeviceIo::take(float* samples, std::uint32_t frames, std::uint32_t queued) {
    {
        std::lock_guard lock(mutex_);
        const auto wanted = static_cast<std::size_t>(frames) * channels_;
        const auto have = std::min(wanted, fifo_.size());
        std::copy_n(fifo_.begin(), have, samples);
        std::fill(samples + have, samples + wanted, 0.0f);
        fifo_.erase(fifo_.begin(), fifo_.begin() + static_cast<std::ptrdiff_t>(have));
        consumed_ += frames;
        host_queued_ = queued;
    }
    wake_.notify_all();
}

void DeviceIo::loop(std::uint64_t generation) {
    std::vector<float> mix, own;
    std::unique_lock lock(mutex_);
    ++loops_;
    io_thread_ = std::this_thread::get_id();
    while (active_ && generation == generation_) {
        const auto frames = buffer_frames_;
        const auto ahead = static_cast<std::size_t>(frames) * channels_ * 2;
        wake_.wait_for(lock, std::chrono::milliseconds(20), [&] { return !active_ || generation != generation_ || fifo_.size() < ahead; });
        if (!active_ || generation != generation_) break;
        // The process is ending (an engine stop, or an exit request the engine never sees).
        if (engine_stop_requested(support_.process.calling_engine()) || support_.process.requested_exit()) break;
        if (fifo_.size() >= ahead) continue;
        Timing timing;
        timing.rate = rate_;
        timing.now_sample = static_cast<double>(consumed_);
        timing.now_host = host_time_now();
        timing.output_sample = static_cast<double>(produced_);
        timing.output_host = timing.now_host + static_cast<std::uint64_t>((fifo_.size() / channels_ + host_queued_) / rate_ * 1e9);
        std::vector<Client> clients;
        clients.reserve(clients_.size());
        for (const auto& [key, client] : clients_) clients.push_back(client);
        rendering_ = true;
        lock.unlock();
        const auto samples = static_cast<std::size_t>(frames) * channels_;
        mix.assign(samples, 0.0f);
        for (const auto& client : clients) {
            own.assign(samples, 0.0f);
            client(own.data(), frames, timing);
            for (std::size_t i = 0; i < samples; ++i) mix[i] += own[i];
        }
        lock.lock();
        rendering_ = false;
        wake_.notify_all();
        if (!active_ || generation != generation_) break;
        fifo_.insert(fifo_.end(), mix.begin(), mix.end());
        produced_ += frames;
    }
    --loops_;
    lock.unlock();
    wake_.notify_all();
}

InputIo::InputIo(Support& support, std::uint32_t id, std::uint32_t channels, double rate)
    : support_(support), id_(id), channels_(std::max(channels, 1U)), rate_(rate > 0 ? rate : 48000.0) {}

InputIo::~InputIo() {
    std::unique_ptr<hal::InputStream> stream;
    std::shared_ptr<CompletionThread> thread;
    {
        std::lock_guard lock(mutex_);
        clients_.clear();
        stream = std::move(stream_);
        thread = std::move(thread_);
    }
    if (stream) stream->stop();
    stream.reset();
    if (thread && std::this_thread::get_id() != delivery_thread_) thread->shutdown();
}

std::int32_t InputIo::start(std::uint64_t key, InputClient client, std::uint32_t buffer_frames) {
    {
        std::lock_guard lock(mutex_);
        if (clients_.contains(key)) return 0;
        auto entry = std::make_shared<Entry>();
        entry->client = std::move(client);
        entry->frames = buffer_frames;
        clients_[key] = std::move(entry);
        if (!thread_) thread_ = start_completion_thread(support_.process, "com.apple.audio.IOThread.input");
        if (stream_) return 0;
    }
    std::weak_ptr<InputIo> weak = shared_from_this();
    auto stream = hal_open_input(id_, [weak](const float* samples, std::uint32_t frames, std::uint64_t host) {
        if (const auto self = weak.lock()) self->captured(samples, frames, host);
    });
    if (!stream || !stream->start()) {
        std::lock_guard lock(mutex_);
        clients_.erase(key);
        return unspecified_error;
    }
    std::unique_ptr<hal::InputStream> extra;
    {
        std::lock_guard lock(mutex_);
        if (stream_ || clients_.empty()) extra = std::move(stream);   // raced with another start, or stopped meanwhile
        else stream_ = std::move(stream);
    }
    if (extra) extra->stop();
    return 0;
}

void InputIo::stop(std::uint64_t key) {
    std::unique_ptr<hal::InputStream> stream;
    {
        std::unique_lock lock(mutex_);
        if (!clients_.erase(key)) return;
        if (std::this_thread::get_id() != delivery_thread_) idle_.wait(lock, [this] { return !delivering_; });
        if (clients_.empty()) {
            stream = std::move(stream_);
            pending_.clear();
            pending_frames_ = 0;
        }
    }
    if (stream) stream->stop();
}

bool InputIo::running(std::uint64_t key) const {
    std::lock_guard lock(mutex_);
    return clients_.contains(key);
}

void InputIo::captured(const float* samples, std::uint32_t frames, std::uint64_t host) {
    std::shared_ptr<CompletionThread> thread;
    {
        std::lock_guard lock(mutex_);
        if (clients_.empty() || !thread_) return;
        pending_.push_back({std::vector<float>(samples, samples + static_cast<std::size_t>(frames) * channels_), host});
        pending_frames_ += frames;
        while (pending_frames_ > static_cast<std::size_t>(rate_) && pending_.size() > 1) {
            const auto dropped = pending_.front().samples.size() / channels_;
            pending_frames_ -= dropped;
            sample_time_ += static_cast<double>(dropped);   // dropped frames still count
            pending_.pop_front();
        }
        if (std::exchange(posted_, true)) return;
        thread = thread_;
    }
    std::weak_ptr<InputIo> weak = shared_from_this();
    thread->post([weak] {
        if (const auto self = weak.lock()) self->deliver();
    });
}

void InputIo::deliver() {
    for (;;) {
        Packet packet;
        std::vector<std::shared_ptr<Entry>> entries;
        double first{};
        {
            std::lock_guard lock(mutex_);
            if (pending_.empty()) {
                posted_ = false;
                return;
            }
            packet = std::move(pending_.front());
            pending_.pop_front();
            const auto count = packet.samples.size() / channels_;
            pending_frames_ -= count;
            first = sample_time_;
            sample_time_ += static_cast<double>(count);
            for (const auto& [key, entry] : clients_) entries.push_back(entry);
            delivering_ = true;
            delivery_thread_ = std::this_thread::get_id();
        }
        const auto frames = static_cast<std::uint32_t>(packet.samples.size() / channels_);
        const auto host_of = [&](double sample) {
            const double offset = (sample - first) / rate_ * 1e9;
            return static_cast<std::uint64_t>(static_cast<double>(packet.host) + offset);
        };
        for (const auto& entry : entries) {
            {
                std::lock_guard lock(mutex_);
                if (std::none_of(clients_.begin(), clients_.end(), [&](const auto& c) { return c.second == entry; })) continue;   // stopped meanwhile
            }
            if (entry->frames == 0) {
                const Timing timing{first, packet.host, first, packet.host, rate_};
                entry->client(packet.samples.data(), frames, timing);
                continue;
            }
            if (entry->carry.empty()) entry->carry_sample = first;
            entry->carry.insert(entry->carry.end(), packet.samples.begin(), packet.samples.end());
            const auto chunk = static_cast<std::size_t>(entry->frames) * channels_;
            std::size_t at = 0;
            while (entry->carry.size() - at >= chunk) {
                const auto sample = entry->carry_sample + static_cast<double>(at / channels_);
                const auto host = host_of(sample);
                const Timing timing{sample, host, sample, host, rate_};
                entry->client(entry->carry.data() + at, entry->frames, timing);
                at += chunk;
            }
            entry->carry.erase(entry->carry.begin(), entry->carry.begin() + static_cast<std::ptrdiff_t>(at));
            entry->carry_sample += static_cast<double>(at / channels_);
        }
        {
            std::lock_guard lock(mutex_);
            delivering_ = false;
        }
        idle_.notify_all();
    }
}

} // namespace alchemy::gui::completion_kit::media::hal_io
