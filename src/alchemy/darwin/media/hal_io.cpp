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

} // namespace alchemy::gui::completion_kit::media::hal_io
