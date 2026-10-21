#include "alchemy/darwin/libsystem/mach/transport.hpp"

#include "alchemy/host/ipc.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/sync.hpp"
#include "alchemy/process_tree.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

// The transport of transport.hpp over host::ipc channels (named pipes on
// Windows). Frames are a 20-byte header
// {u32 type, u32 sender pid, u64 request id (0: one-way), u32 payload size}
// and the payload. A response carries type `response` and its request's id.
// A connection's reader never blocks its writer.
namespace alchemy::libsystem::mach {
namespace {

constexpr std::uint32_t response = 0xffffffffU;
constexpr std::uint32_t frame_limit = 256U * 1024 * 1024;

std::string channel_name(std::uint32_t pid) { return "alchemy-mach-" + std::to_string(pid); }

// Exactly `size` bytes in one direction; false on any failure or end of channel.
bool transfer(host::Handle channel, bool writing, void* data, std::size_t size) {
    auto* cursor = static_cast<std::byte*>(data);
    while (size) {
        const auto done = writing ? host::ipc::write(channel, std::span<const std::byte>(cursor, size))
                                  : host::ipc::read(channel, std::span<std::byte>(cursor, size));
        if (!done || *done == 0) return false;
        cursor += *done;
        size -= *done;
    }
    return true;
}

struct Header {
    std::uint32_t type{}, sender{};
    std::uint64_t request{};
    std::uint32_t size{};
};

std::vector<std::uint8_t> encode(std::uint32_t type, std::uint32_t sender, std::uint64_t request, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> bytes(20 + payload.size());
    const auto size = static_cast<std::uint32_t>(payload.size());
    std::memcpy(bytes.data(), &type, 4);
    std::memcpy(bytes.data() + 4, &sender, 4);
    std::memcpy(bytes.data() + 8, &request, 8);
    std::memcpy(bytes.data() + 16, &size, 4);
    if (!payload.empty()) std::memcpy(bytes.data() + 20, payload.data(), payload.size());
    return bytes;
}

bool read_frame(host::Handle pipe, Header& header, std::vector<std::uint8_t>& payload) {
    std::uint8_t raw[20];
    if (!transfer(pipe, false, raw, sizeof(raw))) return false;
    std::memcpy(&header.type, raw, 4);
    std::memcpy(&header.sender, raw + 4, 4);
    std::memcpy(&header.request, raw + 8, 8);
    std::memcpy(&header.size, raw + 16, 4);
    if (header.size > frame_limit) return false;
    payload.resize(header.size);
    return header.size == 0 || transfer(pipe, false, payload.data(), header.size);
}

class PipeTransport final : public Transport, public std::enable_shared_from_this<PipeTransport> {
public:
    // The endpoint is named by the guest pid (process_tree), which peers know;
    // it is the host pid unless the image was exec'd in place.
    explicit PipeTransport(std::weak_ptr<Handler> handler)
        : handler_(std::move(handler)), self_(process_tree::self() ? process_tree::self()->pid : host::process::current_pid()) {}
    ~PipeTransport() override { stop(); }

    std::uint32_t self() const override { return self_; }

    bool start() override {
        std::lock_guard lock(mutex_);
        if (started_) return listening_;
        started_ = true;
        if (stopped_) return false;
        // The first instance proves the name is ours.
        const auto first = host::ipc::listen(channel_name(self_), true);
        if (!first) {
            std::cerr << "alchemy: Mach transport: cannot create the channel " << channel_name(self_) << " ("
                      << host::name(first.error()) << ")" << std::endl;
            return false;
        }
        listening_ = true;
        std::thread([self = shared_from_this(), first = *first] { self->accept_loop(first); }).detach();
        return true;
    }

    bool post(std::uint32_t pid, std::uint32_t type, Payload payload) override {
        const auto connection = connect(pid);
        if (!connection) return false;
        std::lock_guard lock(connection->gate);
        if (connection->closed) return false;
        connection->outbox.push_back(encode(type, self_, 0, payload));
        connection->changed.notify_all();
        return true;
    }

    std::optional<Payload> call(std::uint32_t pid, std::uint32_t type, Payload payload, std::chrono::milliseconds timeout) override {
        const auto connection = connect(pid);
        if (!connection) return std::nullopt;
        const auto id = next_request_.fetch_add(1);
        std::unique_lock lock(connection->gate);
        if (connection->closed) return std::nullopt;
        connection->pending[id];
        connection->outbox.push_back(encode(type, self_, id, payload));
        connection->changed.notify_all();
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!connection->pending[id] && !connection->closed) {
            if (connection->changed.wait_until(lock, deadline) == std::cv_status::timeout) break;
        }
        auto result = std::move(connection->pending[id]);
        connection->pending.erase(id);
        return result;
    }

    void watch(std::uint32_t pid) override {
        {
            std::lock_guard lock(mutex_);
            if (pid == self_ || stopped_ || !watched_.insert(pid).second) return;
        }
        // An Alchemy process of the tree: watched by pid and generation (a
        // reused pid is a different process). The callback reports from its
        // own thread, never from the thread asking (which may hold locks).
        if (const auto version = process_tree::pidversion(pid)) {
            auto asking = std::make_shared<std::atomic<bool>>(true);
            const auto token = process_tree::watch_exit(pid, version, [weak = weak_from_this(), asking](std::uint32_t dead) {
                if (asking->load()) {
                    std::thread([weak, dead] { if (const auto owner = weak.lock()) owner->died(dead); }).detach();
                    return;
                }
                if (const auto owner = weak.lock()) owner->died(dead);
            });
            asking->store(false);
            if (token) {
                std::lock_guard lock(mutex_);
                exit_watches_.push_back(token);
            }
            return;
        }
        // Not (yet) published: the host process itself.
        struct Watch { std::weak_ptr<PipeTransport> owner; std::uint32_t pid; host::Handle process; std::atomic<std::uintptr_t> wait{}; };
        const auto opened = host::process::open(pid);
        auto* watch = new Watch{weak_from_this(), pid, opened ? *opened : host::Handle{}};
        const auto registered = watch->process ? host::sync::watch_once(watch->process, [](void* context) {
                auto* watch = static_cast<Watch*>(context);
                if (const auto owner = watch->owner.lock()) owner->died(watch->pid);
                // The wait is one-shot; its handles are released with it (the
                // registration stores `wait` before the process can end it here).
                while (!watch->wait.load()) std::this_thread::yield();
                host::sync::unwatch(host::Handle{watch->wait.load()}, false);
                host::close(watch->process);
                delete watch;
            }, watch) : host::Result<host::Handle>(host::Error::not_found);
        if (registered) {
            watch->wait.store(registered->value);
        } else {
            // Already gone (or not ours to watch): report it from another thread.
            if (watch->process) host::close(watch->process);
            delete watch;
            std::thread([weak = weak_from_this(), pid] { if (const auto owner = weak.lock()) owner->died(pid); }).detach();
        }
    }

    void stop() override {
        std::map<std::uint32_t, std::shared_ptr<Connection>> connections;
        host::Handle listening;
        std::vector<std::uint64_t> exit_watches;
        {
            std::lock_guard lock(mutex_);
            if (stopped_) return;
            stopped_ = true;
            connections.swap(connections_);
            exit_watches.swap(exit_watches_);
            listening = listening_pipe_;
            listening_pipe_ = {};
        }
        for (const auto token : exit_watches) static_cast<void>(process_tree::unwatch_exit(token));
        if (listening) host::ipc::cancel(listening);
        for (auto& [pid, connection] : connections) {
            static_cast<void>(pid);
            close(*connection);
        }
    }

private:
    struct Connection {
        std::uint32_t pid{};
        host::Handle pipe;
        std::mutex gate;
        std::condition_variable changed;
        std::deque<std::vector<std::uint8_t>> outbox;
        std::map<std::uint64_t, std::optional<Payload>> pending;
        bool closed{};
    };

    void accept_loop(host::Handle pipe) {
        for (;;) {
            {
                std::lock_guard lock(mutex_);
                if (stopped_) { host::close(pipe); return; }
                listening_pipe_ = pipe;
            }
            const bool connected = static_cast<bool>(host::ipc::accept(pipe));
            {
                std::lock_guard lock(mutex_);
                listening_pipe_ = {};
                if (stopped_) { host::close(pipe); return; }
            }
            if (connected) std::thread([self = shared_from_this(), pipe] { self->serve(pipe); }).detach();
            else host::close(pipe);
            const auto next = host::ipc::listen(channel_name(self_), false);
            if (!next) return;
            pipe = *next;
        }
    }

    // A peer's connection: its frames, in order; requests are answered on it.
    void serve(host::Handle pipe) {
        Header header;
        std::vector<std::uint8_t> payload;
        while (read_frame(pipe, header, payload)) {
            const auto handler = handler_.lock();
            if (!handler || stopped_) break;
            Reader reader{payload};
            try {
                if (header.request) {
                    auto answer = handler->request(header.sender, header.type, reader);
                    auto bytes = encode(response, self_, header.request, answer);
                    if (!transfer(pipe, true, bytes.data(), bytes.size())) break;
                } else {
                    handler->frame(header.sender, header.type, reader);
                }
            } catch (const std::exception& error) {
                std::cerr << "alchemy: Mach transport: frame " << header.type << " from " << header.sender << ": " << error.what() << std::endl;
            }
        }
        host::close(pipe);
    }

    std::shared_ptr<Connection> connect(std::uint32_t pid) {
        {
            std::lock_guard lock(mutex_);
            if (stopped_) return nullptr;
            if (const auto found = connections_.find(pid); found != connections_.end()) return found->second->closed ? nullptr : found->second;
        }
        const auto connected = host::ipc::connect(channel_name(pid));
        if (!connected) return nullptr;
        const auto pipe = *connected;
        auto connection = std::make_shared<Connection>();
        connection->pid = pid;
        connection->pipe = pipe;
        {
            std::lock_guard lock(mutex_);
            if (stopped_) { host::close(pipe); return nullptr; }
            if (const auto found = connections_.find(pid); found != connections_.end()) { host::close(pipe); return found->second; }
            connections_.emplace(pid, connection);
        }
        std::thread([self = shared_from_this(), connection] { self->write_loop(connection); }).detach();
        std::thread([self = shared_from_this(), connection] { self->read_loop(connection); }).detach();
        watch(pid);
        return connection;
    }

    void write_loop(const std::shared_ptr<Connection>& connection) {
        std::unique_lock lock(connection->gate);
        for (;;) {
            connection->changed.wait(lock, [&] { return connection->closed || !connection->outbox.empty(); });
            if (connection->closed) return;
            auto bytes = std::move(connection->outbox.front());
            connection->outbox.pop_front();
            lock.unlock();
            const bool ok = transfer(connection->pipe, true, bytes.data(), bytes.size());
            lock.lock();
            if (!ok) { lock.unlock(); close(*connection); return; }
        }
    }

    void read_loop(const std::shared_ptr<Connection>& connection) {
        Header header;
        std::vector<std::uint8_t> payload;
        while (read_frame(connection->pipe, header, payload)) {
            if (header.type != response) continue;
            std::lock_guard lock(connection->gate);
            if (const auto found = connection->pending.find(header.request); found != connection->pending.end()) {
                found->second = payload;
                connection->changed.notify_all();
            }
        }
        close(*connection);
    }

    void close(Connection& connection) {
        {
            std::lock_guard lock(connection.gate);
            if (connection.closed) return;
            connection.closed = true;
            connection.changed.notify_all();
        }
        host::ipc::cancel(connection.pipe);
        // The handle stays open until both loops are done with it; the
        // process ends or the connection is dropped from the table.
    }

    void died(std::uint32_t pid) {
        std::shared_ptr<Connection> connection;
        {
            std::lock_guard lock(mutex_);
            if (const auto found = connections_.find(pid); found != connections_.end()) {
                connection = found->second;
                connections_.erase(found);
            }
        }
        if (connection) close(*connection);
        if (const auto handler = handler_.lock()) handler->peer_died(pid);
    }

    std::weak_ptr<Handler> handler_;
    const std::uint32_t self_;
    std::mutex mutex_;
    std::map<std::uint32_t, std::shared_ptr<Connection>> connections_;
    std::set<std::uint32_t> watched_;
    std::vector<std::uint64_t> exit_watches_;   // process_tree watches
    std::atomic<std::uint64_t> next_request_{1};
    host::Handle listening_pipe_;
    bool started_{}, listening_{};
    std::atomic<bool> stopped_{false};
};

} // namespace

std::shared_ptr<Transport> Transport::create(std::weak_ptr<Handler> handler) {
    return std::make_shared<PipeTransport>(std::move(handler));
}

} // namespace alchemy::libsystem::mach
