#include "alchemy/darwin/kit/completion/runtime/common.hpp"

#include "alchemy/darwin/libsystem/errno.hpp"
#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/gui/fill_provider.hpp"
#include "alchemy/host/files.hpp"
#include "alchemy/path.hpp"
#include "alchemy/registration_audit.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>

// Streams: CFReadStream / CFWriteStream and NSInputStream / NSOutputStream, one
// implementation (toll-free bridged: a CFReadStreamRef is an NSInputStream).
// - Kinds: data (an NSData, or bytes without a copy), files (the guest path
//   mapped to the host's file), memory and fixed buffers to write into, bound
//   pairs (CFStreamCreateBoundPair, +getBoundStreamsWithBufferSize:...: a
//   buffer between the two ends), and TCP sockets (CFStreamCreatePairWithSocketToHost,
//   CFStreamCreatePairWithSocket, +getStreamsToHostWithName:port:...) through the
//   guest libSystem's sockets, so the network policy applies.
// - Status and errors as documented (NSStreamStatus; NSError in
//   NSPOSIXErrorDomain, which CFStreamError reports as kCFStreamErrorDomainPOSIX).
// - Events: a stream scheduled on run loops (CFReadStreamScheduleWithRunLoop,
//   -scheduleInRunLoop:forMode:) or a dispatch queue delivers OpenCompleted,
//   HasBytesAvailable, CanAcceptBytes, ErrorOccurred and EndEncountered to its
//   CF client (CFReadStreamSetClient) and its NSStream delegate
//   (-stream:handleEvent:; a stream is its own delegate by default). Readiness
//   events are one-shot until the next read or write, as CF arms them. A socket's
//   readiness comes from a poll(2) worker.
// - Reads and writes block as CF documents (bound pairs and sockets wait for
//   data or space). A guest subclass of NSInputStream/NSOutputStream is used
//   through its own methods by the CF functions.
// - Properties: kCFStreamPropertyDataWritten / NSStreamDataWrittenToMemoryStreamKey,
//   kCFStreamPropertyFileCurrentOffset / NSStreamFileCurrentOffsetKey (read and
//   set), kCFStreamPropertyAppendToFile, kCFStreamPropertySocketNativeHandle,
//   kCFStreamPropertySocketRemoteHostName and RemotePortNumber; others are kept
//   as set and read back.
// - Unsupported, failing explicitly: TLS on socket streams (kCFStreamPropertySSLSettings,
//   NSStreamSocketSecurityLevelKey other than none), SOCKS and HTTP proxies on streams.
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;

// NSStreamStatus / CFStreamStatus
enum Status : std::uint64_t { not_open = 0, opening, open, reading, writing, at_end, closed, error_status };
// NSStreamEvent / CFStreamEventType
enum Event : std::uint64_t { open_completed = 1, has_bytes = 2, can_accept = 4, error_occurred = 8, end_encountered = 16 };
enum class Kind { data, file, memory, buffer, bound, socket };
constexpr std::int64_t kCFStreamErrorDomainPOSIX = 1;
constexpr std::uint32_t econnreset = 54, ebadf = 9, einval = 22;

// The buffer between the two ends of a bound pair.
struct Bound {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::uint8_t> bytes;
    std::size_t capacity{};
    bool reader_closed{}, writer_closed{};
    Address reader{}, writer{};   // the two streams (not retained)
};
// The socket of a socket stream pair.
struct Socket {
    std::mutex mutex;
    std::condition_variable connected_changed;
    std::string host;
    std::uint16_t port{};
    std::int32_t fd{-1};
    bool connecting{}, connected{}, failed{}, own_fd{true};
    std::uint32_t error{};
    int open_ends{};
    Address reader{}, writer{};
};

struct Stream final : ObjectPayload {
    std::mutex mutex;
    Kind kind{Kind::data};
    bool input{};
    std::uint64_t status{not_open};
    Address error{};   // +1 NSError
    // data: guest bytes read in place, from an NSData (retained) or given without a copy
    Address data_object{};                                  // +1 NSData of -initWithData:
    Address bytes{}, bytes_length{}, bytes_deallocator{};   // CFReadStreamCreateWithBytesNoCopy (the deallocator frees them)
    std::size_t offset{};
    // file
    std::string path;
    host::OwnedHandle file;
    bool append{};
    // memory / fixed buffer
    std::vector<std::uint8_t> written;
    Address buffer{};
    std::uint64_t capacity{};
    // pairs
    std::shared_ptr<Bound> bound;
    std::shared_ptr<Socket> socket;
    // properties set by the client (key -> +1 value)
    std::vector<std::pair<Address, Address>> properties;
    // events
    Address delegate{};   // not retained; 0: the stream itself
    Address client{}, client_info{}, client_release{};
    std::uint64_t client_events{};
    std::vector<std::pair<Address, Address>> scheduled;   // run loop, mode (both retained)
    Address source{}, queue{};                            // +1
    std::uint64_t pending{}, armed{};                     // events to deliver; readiness events delivered and not re-armed
    bool watching{};
};

class Streams {
public:
    explicit Streams(Support& value) : support(value) {}
    Support& support;
    Address stream_class{}, input_class{}, output_class{};
    Address perform_entry{}, deliver_entry{}, watch_entry{}, connect_entry{};
    std::mutex mutex;
    std::map<Address, Address> work;   // dispatch contexts -> stream
    Address next_work{1};
    Address null_allocator{};
    std::map<std::string, Address> keys;   // property key name -> constant string

    [[noreturn]] static void stop(const std::string& why) { throw std::runtime_error("Alchemy Foundation: " + why); }
    std::shared_ptr<Stream> find(Address object) { return object ? support.objc.find_host_state<Stream>(object) : nullptr; }
    std::shared_ptr<Stream> of(Address object) {
        auto state = find(object);
        if (!state) stop("not an Alchemy stream (an NSStream subclass without its own methods)");
        return state;
    }
    std::int64_t sys(std::string_view symbol, std::initializer_list<Address> arguments) {
        return static_cast<std::int64_t>(support.call(libsystem, symbol, arguments));
    }
    Address key(std::string_view name) { return keys.at(std::string(name)); }
    bool is_key(Address candidate, std::string_view name) {
        const auto found = keys.find(std::string(name));
        return found != keys.end() && candidate && support.cf.equal(candidate, found->second);
    }
    Address posix_error(std::uint32_t code) { return objc_retain(support, error(support, "NSPOSIXErrorDomain", code, "POSIX error " + std::to_string(code))); }

    // ---- creation ----
    Address create(bool input, Kind kind, std::function<void(Stream&)> setup) {
        auto state = std::make_shared<Stream>();
        state->input = input;
        state->kind = kind;
        if (setup) setup(*state);
        return make_object(support, input ? input_class : output_class, state);   // +1
    }
    void initialize(Address object, bool input, Kind kind, std::function<void(Stream&)> setup) {
        const auto state = support.objc.host_state<Stream>(object);
        state->input = input;
        state->kind = kind;
        if (setup) setup(*state);
    }
    std::string url_path(Address url) {
        if (!url) stop("a file stream needs a URL");
        return support.cf.url_path(url);
    }

    // ---- errors ----
    void fail_with(Stream& state, std::uint32_t code) {
        if (state.error) objc_release(support, state.error);
        state.error = posix_error(code);
        state.status = error_status;
        state.pending |= error_occurred;
    }

    // ---- events ----
    bool scheduled(const Stream& state) const { return state.source || state.queue; }
    // Queues `events` (deliverable ones only) and wakes the stream's run loops or queue.
    void signal(Address object, std::uint64_t events) {
        const auto state = find(object);
        if (!state) return;
        Address source{}, queue{};
        std::vector<Address> loops;
        {
            std::lock_guard lock(state->mutex);
            events &= ~state->armed;
            if (!events || !scheduled(*state)) return;
            state->pending |= events;
            state->armed |= events & (has_bytes | can_accept);
            source = state->source;
            queue = state->queue;
            for (const auto& [loop, mode] : state->scheduled) loops.push_back(loop);
        }
        if (source) {
            static_cast<void>(support.call(core_foundation, "_CFRunLoopSourceSignal", {source}));
            for (const auto loop : loops) static_cast<void>(support.call(core_foundation, "_CFRunLoopWakeUp", {loop}));
        }
        if (queue) {
            objc_retain(support, object);
            Address id;
            { std::lock_guard lock(mutex); id = next_work++; work[id] = object; }
            static_cast<void>(support.call(libsystem, "_dispatch_async_f", {queue, id, deliver_entry}));
        }
    }
    // The run loop source's perform, and the dispatch queue's work: deliver what is pending.
    void deliver(Address object) {
        const auto state = find(object);
        if (!state) return;
        for (const std::uint64_t event : {open_completed, has_bytes, can_accept, error_occurred, end_encountered}) {
            Address client{}, info{}, delegate{};
            std::uint64_t mask{};
            {
                std::lock_guard lock(state->mutex);
                if (!(state->pending & event)) continue;
                state->pending &= ~event;
                client = state->client;
                info = state->client_info;
                mask = state->client_events;
                delegate = state->delegate ? state->delegate : object;
            }
            if (client && (mask & event)) static_cast<void>(call_address(support, client, {object, event, info}));
            const auto selector = support.objc.selector("stream:handleEvent:");
            if (support.send_id(delegate, "respondsToSelector:", {selector}) & 0xff)
                static_cast<void>(support.send_id(delegate, "stream:handleEvent:", {object, event}));
        }
    }
    // The readiness events of the stream's current state (after open, a read, a write or scheduling).
    std::uint64_t readiness(Stream& state) {
        if (state.status == error_status) return error_occurred;
        if (state.status == at_end) return end_encountered;
        if (state.status != open) return 0;
        if (state.input) {
            switch (state.kind) {
            case Kind::data: return state.offset < data_size(state) ? has_bytes : end_encountered;
            case Kind::file: return has_bytes;
            case Kind::bound: {
                std::lock_guard lock(state.bound->mutex);
                if (!state.bound->bytes.empty()) return has_bytes;
                return state.bound->writer_closed ? end_encountered : 0;
            }
            case Kind::socket: return 0;   // the poll worker reports
            default: return 0;
            }
        }
        switch (state.kind) {
        case Kind::buffer: return state.written.size() < state.capacity ? can_accept : 0;
        case Kind::bound: {
            std::lock_guard lock(state.bound->mutex);
            return state.bound->bytes.size() < state.bound->capacity ? can_accept : 0;
        }
        case Kind::socket: return 0;
        default: return can_accept;
        }
    }
    void refresh(Address object, std::uint64_t also = 0) {
        const auto state = find(object);
        if (!state) return;
        std::uint64_t events;
        bool watch_socket{};
        {
            std::lock_guard lock(state->mutex);
            events = readiness(*state) | also | (state->pending & error_occurred);
            watch_socket = state->kind == Kind::socket && state->status == open && scheduled(*state) && !state->watching;
            if (watch_socket) state->watching = true;
        }
        if (events) signal(object, events);
        if (watch_socket) start_watch(object);
    }
    void schedule(Address object, Address loop, Address mode) {
        const auto state = of(object);
        Address source{};
        {
            std::lock_guard lock(state->mutex);
            for (const auto& entry : state->scheduled)
                if (entry.first == loop && support.cf.equal(entry.second, mode)) return;
            source = state->source;
        }
        if (!source) {
            // CFRunLoopSourceContext (version 0): version, info, retain, release, copyDescription, equal, hash, schedule, cancel, perform.
            GuestBlock context(support, 80);
            support.memory().write64(context + 8, object);
            support.memory().write64(context + 72, perform_entry);
            source = support.call(core_foundation, "_CFRunLoopSourceCreate", {0, 0, context});
            std::lock_guard lock(state->mutex);
            if (state->source) { support.release(source); source = state->source; }
            else state->source = source;
        }
        {
            std::lock_guard lock(state->mutex);
            state->scheduled.emplace_back(support.retain(loop), support.retain(mode));
        }
        static_cast<void>(support.call(core_foundation, "_CFRunLoopAddSource", {loop, source, mode}));
        refresh(object);
    }
    void unschedule(Address object, Address loop, Address mode) {
        const auto state = of(object);
        Address source{};
        std::pair<Address, Address> removed{};
        {
            std::lock_guard lock(state->mutex);
            const auto found = std::find_if(state->scheduled.begin(), state->scheduled.end(), [&](const auto& entry) {
                return entry.first == loop && support.cf.equal(entry.second, mode);
            });
            if (found == state->scheduled.end()) return;
            removed = *found;
            state->scheduled.erase(found);
            source = state->source;
        }
        if (source) static_cast<void>(support.call(core_foundation, "_CFRunLoopRemoveSource", {loop, source, mode}));
        support.release(removed.first);
        support.release(removed.second);
    }
    void set_queue(Address object, Address queue) {
        const auto state = of(object);
        Address old{};
        if (queue) static_cast<void>(support.call(libsystem, "_dispatch_retain", {queue}));
        { std::lock_guard lock(state->mutex); old = std::exchange(state->queue, queue); }
        if (old) static_cast<void>(support.call(libsystem, "_dispatch_release", {old}));
        if (queue) refresh(object);
    }
    // Every scheduling, the source and the client go when the stream closes or is released.
    void teardown(Address object, Stream& state) {
        std::vector<std::pair<Address, Address>> scheduled;
        Address source{}, queue{}, info{}, release{};
        {
            std::lock_guard lock(state.mutex);
            scheduled.swap(state.scheduled);
            source = std::exchange(state.source, 0);
            queue = std::exchange(state.queue, 0);
            info = std::exchange(state.client_info, 0);
            release = std::exchange(state.client_release, 0);
            state.client = 0;
        }
        for (const auto& [loop, mode] : scheduled) {
            if (source) static_cast<void>(support.call(core_foundation, "_CFRunLoopRemoveSource", {loop, source, mode}));
            support.release(loop);
            support.release(mode);
        }
        if (source) {
            static_cast<void>(support.call(core_foundation, "_CFRunLoopSourceInvalidate", {source}));
            support.release(source);
        }
        if (queue) static_cast<void>(support.call(libsystem, "_dispatch_release", {queue}));
        if (info && release) static_cast<void>(call_address(support, release, {info}));
        static_cast<void>(object);
    }

    // ---- sockets ----
    void start_watch(Address object) {
        objc_retain(support, object);
        Address id;
        { std::lock_guard lock(mutex); id = next_work++; work[id] = object; }
        const auto queue = support.call(libsystem, "_dispatch_get_global_queue", {0, 0});
        static_cast<void>(support.call(libsystem, "_dispatch_async_f", {queue, id, watch_entry}));
    }
    Address take_work(Address id) {
        std::lock_guard lock(mutex);
        const auto found = work.find(id);
        if (found == work.end()) stop("stream work without its stream");
        const auto object = found->second;
        work.erase(found);
        return object;
    }
    // poll(2) on the socket while the stream is open and scheduled: readiness becomes events.
    void watch(Address object) {
        const auto state = of(object);
        GuestBlock entry(support, 8);
        for (;;) {
            std::int32_t fd;
            bool input;
            {
                std::lock_guard lock(state->mutex);
                if (state->status != open || !scheduled(*state) || support.process.requested_exit()) { state->watching = false; break; }
                fd = state->socket->fd;
                input = state->input;
                if (state->armed & (input ? has_bytes : can_accept)) {   // already reported, waiting for a read or write
                    fd = -2;
                }
            }
            if (fd == -2) { static_cast<void>(sys("_usleep", {20000})); continue; }
            support.memory().write32(entry, static_cast<std::uint32_t>(fd));
            support.memory().write32(entry + 4, input ? 0x1u : 0x4u);   // POLLIN / POLLOUT
            if (sys("_poll", {entry, 1, 50}) <= 0) continue;
            const auto revents = support.memory().read32(entry + 4) >> 16;
            if (revents & 0x20) { std::lock_guard lock(state->mutex); state->watching = false; break; }   // POLLNVAL
            if (revents & (input ? 0x19u : 0x1cu)) signal(object, input ? has_bytes : can_accept);   // POLLIN|POLLERR|POLLHUP, POLLOUT|...
        }
        objc_release(support, object);
    }
    // Resolves and connects (on a worker for an open from any thread); both ends then report OpenCompleted.
    void connect_socket(const std::shared_ptr<Socket>& socket) {
        std::int32_t fd = -1;
        std::uint32_t code = 0;
        {
            GuestBlock host(support, socket->host);
            GuestBlock port(support, std::to_string(socket->port));
            GuestBlock hints(support, 48);
            support.memory().write32(hints + 8, 1);   // ai_socktype SOCK_STREAM
            GuestBlock result(support, 8);
            if (sys("_getaddrinfo", {host, port, hints, result}) != 0) code = 65;   // EHOSTUNREACH
            else {
                const auto list = support.memory().read64(result);
                code = econnreset;
                for (auto info = list; info; info = support.memory().read64(info + 40)) {
                    const auto family = support.memory().read32(info + 4), type = support.memory().read32(info + 8),
                               protocol = support.memory().read32(info + 12), length = support.memory().read32(info + 16);
                    fd = static_cast<std::int32_t>(sys("_socket", {family, type, protocol}));
                    if (fd < 0) { code = guest_errno(support); continue; }
                    if (sys("_connect", {static_cast<Address>(fd), support.memory().read64(info + 32), length}) == 0) { code = 0; break; }
                    code = guest_errno(support);
                    static_cast<void>(sys("_close", {static_cast<Address>(fd)}));
                    fd = -1;
                }
                static_cast<void>(sys("_freeaddrinfo", {list}));
            }
        }
        Address reader{}, writer{};
        {
            std::lock_guard lock(socket->mutex);
            socket->connecting = false;
            socket->fd = fd;
            socket->connected = code == 0;
            socket->failed = code != 0;
            socket->error = code;
            reader = socket->reader;
            writer = socket->writer;
        }
        socket->connected_changed.notify_all();
        for (const auto end : {reader, writer}) {
            const auto state = find(end);
            if (!state) continue;
            {
                std::lock_guard lock(state->mutex);
                if (state->status != opening) continue;
                if (code) fail_with(*state, code);
                else { state->status = open; }
            }
            refresh(end, code ? 0 : open_completed);
        }
    }

    // ---- open, read, write, close ----
    bool open_stream(Address object) {
        const auto state = of(object);
        bool start_connect{};
        std::shared_ptr<Socket> socket;
        {
            std::lock_guard lock(state->mutex);
            if (state->status != not_open) return false;
            switch (state->kind) {
            case Kind::file: {
                const auto host_path = support.process.map_guest_path ? path_utf8(support.process.map_guest_path(state->path)) : state->path;
                using host::files::Access;
                using host::files::Create;
                auto handle = state->input ? host::files::open(host_path, Access::read, Create::open_existing)
                                           : host::files::open(host_path, Access::write, state->append ? Create::open_or_create : Create::create_or_truncate);
                if (!handle) { fail_with(*state, alchemy::libsystem::darwin_errno(handle.error())); break; }
                state->file = host::OwnedHandle(*handle);
                if (!state->input && state->append)
                    if (!host::files::seek(state->file.get(), 0, host::files::Whence::end)) { fail_with(*state, ebadf); break; }
                if (state->input && state->offset)
                    if (!host::files::seek(state->file.get(), static_cast<std::int64_t>(state->offset), host::files::Whence::set)) { fail_with(*state, einval); break; }
                state->status = open;
                break;
            }
            case Kind::socket: {
                socket = state->socket;
                std::lock_guard socket_lock(socket->mutex);
                ++socket->open_ends;
                if (socket->connected) state->status = open;
                else if (socket->failed) fail_with(*state, socket->error);
                else {
                    state->status = opening;
                    if (!socket->connecting && socket->fd < 0) { socket->connecting = true; start_connect = true; }
                }
                break;
            }
            default: state->status = open; break;
            }
        }
        if (start_connect) {
            Address id;
            objc_retain(support, object);
            { std::lock_guard lock(mutex); id = next_work++; work[id] = object; }
            const auto queue = support.call(libsystem, "_dispatch_get_global_queue", {0, 0});
            static_cast<void>(support.call(libsystem, "_dispatch_async_f", {queue, id, connect_entry}));
        }
        std::uint64_t status;
        { std::lock_guard lock(state->mutex); status = state->status; }
        if (status == open) refresh(object, open_completed);
        else if (status == error_status) refresh(object);
        return status != error_status;
    }
    std::size_t data_size(const Stream& state) const { return static_cast<std::size_t>(state.bytes_length); }
    // CFReadStreamRead: bytes read, 0 at the end, -1 on error.
    std::int64_t read(Address object, Address buffer, std::uint64_t length) {
        const auto state = of(object);
        std::int64_t result = 0;
        std::shared_ptr<Bound> bound;
        std::shared_ptr<Socket> socket;
        {
            std::lock_guard lock(state->mutex);
            if (!state->input) stop("reading an output stream");
            if (state->status == at_end) return 0;
            if (state->status != open) return -1;
            state->armed &= ~has_bytes;
            switch (state->kind) {
            case Kind::data: {
                const auto size = data_size(*state);
                const auto count = std::min<std::size_t>(static_cast<std::size_t>(length), size - state->offset);
                if (count) {
                    std::vector<std::uint8_t> chunk(count);
                    support.memory().read_bytes(state->bytes + state->offset, chunk);
                    support.memory().write_bytes(buffer, chunk);
                }
                state->offset += count;
                if (state->offset == size) state->status = at_end;
                result = static_cast<std::int64_t>(count);
                break;
            }
            case Kind::file: {
                std::vector<std::byte> chunk(static_cast<std::size_t>(std::min<std::uint64_t>(length, 1u << 24)));
                const auto count = host::files::read(state->file.get(), chunk);
                if (!count) { fail_with(*state, alchemy::libsystem::darwin_errno(count.error())); result = -1; break; }
                if (*count) support.memory().write_bytes(buffer, std::span(reinterpret_cast<const std::uint8_t*>(chunk.data()), *count));
                else state->status = at_end;
                state->offset += *count;
                result = static_cast<std::int64_t>(*count);
                break;
            }
            case Kind::bound: bound = state->bound; break;
            case Kind::socket: socket = state->socket; break;
            default: stop("reading an output stream");
            }
        }
        if (bound) {
            std::vector<std::uint8_t> chunk;
            Address writer{};
            {
                std::unique_lock lock(bound->mutex);
                bound->changed.wait(lock, [&] { return !bound->bytes.empty() || bound->writer_closed || bound->reader_closed; });
                const auto count = std::min<std::size_t>(static_cast<std::size_t>(length), bound->bytes.size());
                chunk.assign(bound->bytes.begin(), bound->bytes.begin() + static_cast<std::ptrdiff_t>(count));
                bound->bytes.erase(bound->bytes.begin(), bound->bytes.begin() + static_cast<std::ptrdiff_t>(count));
                writer = bound->writer;
            }
            bound->changed.notify_all();
            if (!chunk.empty()) support.memory().write_bytes(buffer, chunk);
            else { std::lock_guard lock(state->mutex); state->status = at_end; }
            result = static_cast<std::int64_t>(chunk.size());
            if (writer && !chunk.empty()) refresh(writer);
        }
        if (socket) {
            const auto count = sys("_read", {static_cast<Address>(socket->fd), buffer, length});
            std::lock_guard lock(state->mutex);
            if (count < 0) { fail_with(*state, guest_errno(support)); result = -1; }
            else if (count == 0) { state->status = at_end; result = 0; }
            else result = count;
        }
        refresh(object);
        return result;
    }
    // CFWriteStreamWrite: bytes written, 0 when a fixed buffer is full, -1 on error.
    std::int64_t write(Address object, Address buffer, std::uint64_t length) {
        const auto state = of(object);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::min<std::uint64_t>(length, 1u << 26)));
        if (!bytes.empty()) support.memory().read_bytes(buffer, bytes);
        std::int64_t result = 0;
        std::shared_ptr<Bound> bound;
        std::shared_ptr<Socket> socket;
        {
            std::lock_guard lock(state->mutex);
            if (state->input) stop("writing an input stream");
            if (state->status == at_end && state->kind == Kind::buffer) return 0;   // a fixed buffer filled to capacity
            if (state->status != open) return -1;
            state->armed &= ~can_accept;
            switch (state->kind) {
            case Kind::memory:
                state->written.insert(state->written.end(), bytes.begin(), bytes.end());
                result = static_cast<std::int64_t>(bytes.size());
                break;
            case Kind::buffer: {
                const auto count = std::min<std::size_t>(bytes.size(), static_cast<std::size_t>(state->capacity - state->written.size()));
                if (count) support.memory().write_bytes(state->buffer + state->written.size(), std::span(bytes.data(), count));
                state->written.insert(state->written.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(count));
                if (state->written.size() == state->capacity) state->status = at_end;
                result = static_cast<std::int64_t>(count);
                break;
            }
            case Kind::file: {
                const auto count = host::files::write(state->file.get(), std::span(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
                if (!count) { fail_with(*state, alchemy::libsystem::darwin_errno(count.error())); result = -1; break; }
                state->offset += *count;
                result = static_cast<std::int64_t>(*count);
                break;
            }
            case Kind::bound: bound = state->bound; break;
            case Kind::socket: socket = state->socket; break;
            default: stop("writing an input stream");
            }
        }
        if (bound) {
            Address reader{};
            std::size_t count{};
            {
                std::unique_lock lock(bound->mutex);
                bound->changed.wait(lock, [&] { return bound->bytes.size() < bound->capacity || bound->reader_closed || bound->writer_closed; });
                if (bound->reader_closed) { lock.unlock(); std::lock_guard state_lock(state->mutex); fail_with(*state, 32); return -1; }   // EPIPE
                count = std::min(bytes.size(), bound->capacity - bound->bytes.size());
                bound->bytes.insert(bound->bytes.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(count));
                reader = bound->reader;
            }
            bound->changed.notify_all();
            result = static_cast<std::int64_t>(count);
            if (reader && count) refresh(reader);
        }
        if (socket) {
            GuestBlock block(support, bytes.size() ? bytes.size() : 1, false);
            if (!bytes.empty()) support.memory().write_bytes(block, bytes);
            const auto count = sys("_write", {static_cast<Address>(socket->fd), block, bytes.size()});
            std::lock_guard lock(state->mutex);
            if (count < 0) { fail_with(*state, guest_errno(support)); result = -1; }
            else result = count;
        }
        refresh(object);
        return result;
    }
    void close_stream(Address object) {
        const auto state = find(object);
        if (!state) return;
        std::shared_ptr<Bound> bound;
        std::shared_ptr<Socket> socket;
        {
            std::lock_guard lock(state->mutex);
            if (state->status == closed) return;
            state->status = closed;
            state->file.reset();
            state->pending = 0;
            bound = state->bound;
            socket = state->socket;
        }
        if (bound) {
            Address peer{};
            {
                std::lock_guard lock(bound->mutex);
                (state->input ? bound->reader_closed : bound->writer_closed) = true;
                peer = state->input ? bound->writer : bound->reader;
            }
            bound->changed.notify_all();
            if (peer) refresh(peer);
        }
        if (socket) {
            std::int32_t fd = -1;
            {
                std::lock_guard lock(socket->mutex);
                if (--socket->open_ends <= 0 && socket->own_fd) fd = std::exchange(socket->fd, -1);
                else if (socket->fd >= 0) static_cast<void>(sys("_shutdown", {static_cast<Address>(socket->fd), state->input ? 0u : 1u}));
            }
            if (fd >= 0) static_cast<void>(sys("_close", {static_cast<Address>(fd)}));
        }
        teardown(object, *state);
    }

    // ---- properties ----
    // A +1 value (CFReadStreamCopyProperty), or 0.
    Address copy_property(Address object, Address name) {
        const auto state = of(object);
        std::lock_guard lock(state->mutex);
        if (is_key(name, "kCFStreamPropertyDataWritten")) {
            if (state->kind != Kind::memory) return 0;
            GuestBlock block(support, state->written.size() ? state->written.size() : 1, false);
            if (!state->written.empty()) support.memory().write_bytes(block, state->written);
            return support.cf.invoke("CFDataCreate", std::array<Address, 3>{0, block, state->written.size()});
        }
        if (is_key(name, "kCFStreamPropertyFileCurrentOffset")) {
            if (state->kind != Kind::file) return 0;
            return support.cf.integer(static_cast<std::int64_t>(state->offset), 4);
        }
        if (is_key(name, "kCFStreamPropertyAppendToFile")) {
            if (state->kind != Kind::file || state->input) return 0;
            return support.retain(support.cf.boolean(state->append));
        }
        if (state->socket) {
            if (is_key(name, "kCFStreamPropertySocketNativeHandle")) {
                std::lock_guard socket_lock(state->socket->mutex);
                if (state->socket->fd < 0) return 0;
                const auto fd = state->socket->fd;
                GuestBlock block(support, 4);
                support.memory().write32(block, static_cast<std::uint32_t>(fd));
                return support.cf.invoke("CFDataCreate", std::array<Address, 3>{0, block, 4});
            }
            if (is_key(name, "kCFStreamPropertySocketRemoteHostName"))
                return state->socket->host.empty() ? 0 : support.cf.string(state->socket->host);
            if (is_key(name, "kCFStreamPropertySocketRemotePortNumber"))
                return state->socket->port ? support.cf.integer(state->socket->port, 4) : 0;
        }
        for (const auto& [property, value] : state->properties)
            if (support.cf.equal(property, name)) return support.retain(value);
        return 0;
    }
    bool set_property(Address object, Address name, Address value) {
        const auto state = of(object);
        if (is_key(name, "kCFStreamPropertySSLSettings") || (is_key(name, "kCFStreamPropertySocketSecurityLevel") && value &&
                                                             !support.cf.equal(value, key("kCFStreamSocketSecurityLevelNone"))))
            stop("TLS on socket streams is unsupported (kCFStreamPropertySSLSettings / NSStreamSocketSecurityLevelKey)");
        if (is_key(name, "kCFStreamPropertySOCKSProxy") || is_key(name, "kCFStreamPropertyHTTPProxy"))
            stop("proxies on socket streams are unsupported");
        std::lock_guard lock(state->mutex);
        if (is_key(name, "kCFStreamPropertyFileCurrentOffset")) {
            if (state->kind != Kind::file || !value) return false;
            const auto offset = support.cf.number_value(value).value_or(0);
            state->offset = static_cast<std::size_t>(offset);
            if (state->file && !host::files::seek(state->file.get(), static_cast<std::int64_t>(offset), host::files::Whence::set)) return false;
            return true;
        }
        if (is_key(name, "kCFStreamPropertyAppendToFile")) {
            if (state->kind != Kind::file || state->input || state->status != not_open) return false;
            state->append = value && support.cf.equal(value, support.cf.boolean(true));
            return true;
        }
        for (auto& [property, old] : state->properties)
            if (support.cf.equal(property, name)) {
                if (old) support.release(old);
                old = value ? support.retain(value) : 0;
                return true;
            }
        state->properties.emplace_back(support.retain(name), value ? support.retain(value) : 0);
        return true;
    }

    // ---- pairs ----
    std::pair<Address, Address> bound_pair(std::uint64_t capacity) {
        auto bound = std::make_shared<Bound>();
        bound->capacity = std::max<std::uint64_t>(capacity, 1);
        const auto reader = create(true, Kind::bound, [&](Stream& s) { s.bound = bound; });
        const auto writer = create(false, Kind::bound, [&](Stream& s) { s.bound = bound; });
        bound->reader = reader;
        bound->writer = writer;
        return {reader, writer};
    }
    std::pair<Address, Address> socket_pair(std::string host, std::uint16_t port, std::int32_t fd, bool own_fd) {
        auto socket = std::make_shared<Socket>();
        socket->host = std::move(host);
        socket->port = port;
        socket->fd = fd;
        socket->own_fd = own_fd;
        socket->connected = fd >= 0;
        const auto reader = create(true, Kind::socket, [&](Stream& s) { s.socket = socket; });
        const auto writer = create(false, Kind::socket, [&](Stream& s) { s.socket = socket; });
        socket->reader = reader;
        socket->writer = writer;
        return {reader, writer};
    }

    void install();
    void install_cf();
    void install_ns();
};

void Streams::install() {
    const auto& foundation = foundation_library;
    stream_class = support.objc.find_class("NSStream") ? support.objc.find_class("NSStream") : support.declare_class("NSStream", "NSObject", foundation);
    input_class = support.declare_class("NSInputStream", "NSStream", foundation);
    output_class = support.declare_class("NSOutputStream", "NSStream", foundation);
    // NSInputStream's class symbol is also exported from CoreFoundation (cf_file_descriptors.cpp
    // publishes the class identities it finds; this group installs after it).
    if (!support.process.resolve(core_foundation, "_OBJC_CLASS_$_NSInputStream")) support.objc.publish_class("NSInputStream", input_class, core_foundation);
    if (const auto bridge = security::bridge()) {
        bridge->register_type_id(input_class, static_cast<security::TypeId>(fill::TypeId::read_stream));
        bridge->register_type_id(output_class, static_cast<security::TypeId>(fill::TypeId::write_stream));
    }
    if (const auto found = support.process.resolve(core_foundation, "_kCFAllocatorNull")) null_allocator = support.memory().read64(found->address);
    const auto self = this;
    perform_entry = support.process.install_function("___alchemy_foundation_stream_perform", [self](CpuState& s, GuestMemory&) {
        self->deliver(s.x[0]);
        s.x[0] = 0;
    }, "alchemy.internal");
    deliver_entry = support.process.install_function("___alchemy_foundation_stream_deliver", [self](CpuState& s, GuestMemory&) {
        const auto object = self->take_work(s.x[0]);
        self->deliver(object);
        objc_release(self->support, object);
        s.x[0] = 0;
    }, "alchemy.internal");
    watch_entry = support.process.install_function("___alchemy_foundation_stream_watch", [self](CpuState& s, GuestMemory&) {
        self->watch(self->take_work(s.x[0]));
        s.x[0] = 0;
    }, "alchemy.internal");
    connect_entry = support.process.install_function("___alchemy_foundation_stream_connect", [self](CpuState& s, GuestMemory&) {
        const auto object = self->take_work(s.x[0]);
        if (const auto state = self->find(object)) self->connect_socket(state->socket);
        objc_release(self->support, object);
        s.x[0] = 0;
    }, "alchemy.internal");
    for (const auto target : {input_class, output_class}) {
        support.method(target, "dealloc", [self, target](CpuState& s, GuestMemory&) {
            if (const auto state = self->find(s.x[0])) {
                self->close_stream(s.x[0]);
                Address error{}, bytes{}, deallocator{}, data{};
                std::vector<std::pair<Address, Address>> properties;
                {
                    std::lock_guard lock(state->mutex);
                    error = std::exchange(state->error, 0);
                    properties.swap(state->properties);
                    bytes = std::exchange(state->bytes, 0);
                    deallocator = state->bytes_deallocator;
                    data = std::exchange(state->data_object, 0);
                }
                if (error) objc_release(self->support, error);
                if (data) objc_release(self->support, data);
                for (const auto& [name, value] : properties) { self->support.release(name); self->support.release(value); }
                // No-copy bytes go to their deallocator (kCFAllocatorNull: the caller keeps them).
                if (bytes && !data && deallocator != self->null_allocator) static_cast<void>(self->support.call(libsystem, "_free", {bytes}));
                if (state->bound) {
                    std::lock_guard lock(state->bound->mutex);
                    (state->input ? state->bound->reader : state->bound->writer) = 0;
                }
                if (state->socket) {
                    std::lock_guard lock(state->socket->mutex);
                    (state->input ? state->socket->reader : state->socket->writer) = 0;
                }
            }
            static_cast<void>(self->support.objc.send_super(s.x[0], target, self->support.objc.selector("dealloc")));
        }, "v@:");
    }
    install_cf();
    install_ns();
}

void Streams::install_cf() {
    const auto self = this;
    const std::string_view cfnetwork = security::cfnetwork_library;
    // Property keys: CFNetwork's kCFStreamPropertySocket* and CoreFoundation's others, with Foundation's names.
    for (const auto& [library, name, value] : std::array<std::tuple<std::string_view, const char*, const char*>, 9>{{
             {core_foundation, "kCFStreamPropertyDataWritten", "kCFStreamPropertyDataWritten"},
             {core_foundation, "kCFStreamPropertyFileCurrentOffset", "kCFStreamPropertyFileCurrentOffset"},
             {core_foundation, "kCFStreamPropertyAppendToFile", "kCFStreamPropertyAppendToFile"},
             {core_foundation, "kCFStreamPropertySocketNativeHandle", "kCFStreamPropertySocketNativeHandle"},
             {core_foundation, "kCFStreamPropertySocketRemoteHostName", "kCFStreamPropertySocketRemoteHostName"},
             {core_foundation, "kCFStreamPropertySocketRemotePortNumber", "kCFStreamPropertySocketRemotePortNumber"},
             {cfnetwork, "kCFStreamPropertySOCKSProxy", "kCFStreamPropertySOCKSProxy"},
             {cfnetwork, "kCFStreamPropertyHTTPProxy", "kCFStreamPropertyHTTPProxy"},
             {core_foundation, "kCFStreamPropertyShouldCloseNativeSocket", "kCFStreamPropertyShouldCloseNativeSocket"}}}) {
        const auto symbol = "_" + std::string(name);
        if (!support.process.resolve(library, symbol)) support.string_constant(library, symbol, value);
        keys[name] = support.memory().read64(support.process.resolve(library, symbol)->address);
    }
    // Security's keys (security.cpp) as they are exported.
    for (const auto* name : {"kCFStreamPropertySSLSettings", "kCFStreamPropertySocketSecurityLevel", "kCFStreamSocketSecurityLevelNone"}) {
        if (const auto found = support.process.resolve(cfnetwork, std::string("_") + name)) keys[name] = support.memory().read64(found->address);
        else keys[name] = support.cf.constant_string(name);
    }
    // The NSStream keys are the same strings (toll-free: one dictionary of properties).
    for (const auto& [ns, cf] : std::array<std::pair<const char*, const char*>, 3>{{
             {"NSStreamDataWrittenToMemoryStreamKey", "kCFStreamPropertyDataWritten"},
             {"NSStreamFileCurrentOffsetKey", "kCFStreamPropertyFileCurrentOffset"},
             {"NSStreamSocketSecurityLevelKey", "kCFStreamPropertySocketSecurityLevel"}}}) {
        const auto symbol = "_" + std::string(ns);
        if (support.process.resolve(foundation_library, symbol)) continue;
        const auto slot = support.process.allocate_guest(8);
        support.memory().write64(slot, keys.at(cf));
        registration_audit::Site site(std::source_location::current());
        support.process.install_export(std::string(foundation_library), symbol, {slot, ExportKind::data});
    }
    support.function(core_foundation, "_CFReadStreamGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(fill::TypeId::read_stream); });
    support.function(core_foundation, "_CFWriteStreamGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(fill::TypeId::write_stream); });

    // ---- creation ----
    support.function(core_foundation, "_CFReadStreamCreateWithFile", [self](CpuState& s, GuestMemory&) {
        const auto path = self->url_path(s.x[1]);
        s.x[0] = self->create(true, Kind::file, [&](Stream& state) { state.path = path; });
    });
    support.function(core_foundation, "_CFWriteStreamCreateWithFile", [self](CpuState& s, GuestMemory&) {
        const auto path = self->url_path(s.x[1]);
        s.x[0] = self->create(false, Kind::file, [&](Stream& state) { state.path = path; });
    });
    // CFReadStreamCreateWithBytesNoCopy(alloc, bytes, length, bytesDeallocator)
    support.function(core_foundation, "_CFReadStreamCreateWithBytesNoCopy", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->create(true, Kind::data, [&](Stream& state) {
            state.bytes = s.x[1];
            state.bytes_length = s.x[2];
            state.bytes_deallocator = s.x[3];
            if (!state.bytes) state.bytes_length = 0;
        });
    });
    // CFWriteStreamCreateWithBuffer(alloc, buffer, capacity)
    support.function(core_foundation, "_CFWriteStreamCreateWithBuffer", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->create(false, Kind::buffer, [&](Stream& state) { state.buffer = s.x[1]; state.capacity = s.x[2]; });
    });
    support.function(core_foundation, "_CFWriteStreamCreateWithAllocatedBuffers", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->create(false, Kind::memory, {});
    });
    // void CFStreamCreateBoundPair(alloc, CFReadStreamRef*, CFWriteStreamRef*, CFIndex transferBufferSize)
    support.function(core_foundation, "_CFStreamCreateBoundPair", [self](CpuState& s, GuestMemory& m) {
        const auto [reader, writer] = self->bound_pair(s.x[3]);
        if (s.x[1]) m.write64(s.x[1], reader); else self->support.release(reader);
        if (s.x[2]) m.write64(s.x[2], writer); else self->support.release(writer);
    });
    // void CFStreamCreatePairWithSocketToHost(alloc, CFStringRef host, UInt32 port, CFReadStreamRef*, CFWriteStreamRef*)
    support.function(core_foundation, "_CFStreamCreatePairWithSocketToHost", [self](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) stop("CFStreamCreatePairWithSocketToHost without a host");
        const auto [reader, writer] = self->socket_pair(self->support.cf.utf8(s.x[1]), static_cast<std::uint16_t>(s.x[2]), -1, true);
        if (s.x[3]) m.write64(s.x[3], reader); else self->support.release(reader);
        if (s.x[4]) m.write64(s.x[4], writer); else self->support.release(writer);
    });
    // void CFStreamCreatePairWithSocket(alloc, CFSocketNativeHandle, CFReadStreamRef*, CFWriteStreamRef*)
    support.function(core_foundation, "_CFStreamCreatePairWithSocket", [self](CpuState& s, GuestMemory& m) {
        const auto [reader, writer] = self->socket_pair({}, 0, static_cast<std::int32_t>(s.x[1]), false);
        if (s.x[2]) m.write64(s.x[2], reader); else self->support.release(reader);
        if (s.x[3]) m.write64(s.x[3], writer); else self->support.release(writer);
    });

    // ---- the read and write calls (CF on any NSStream: Alchemy's, or a guest subclass's methods) ----
    for (const bool input : {true, false}) {
        const std::string prefix = input ? "_CFReadStream" : "_CFWriteStream";
        support.function(core_foundation, prefix + "Open", [self](CpuState& s, GuestMemory&) {
            if (!self->find(s.x[0])) { static_cast<void>(self->support.send_id(s.x[0], "open")); s.x[0] = 1; return; }
            s.x[0] = self->open_stream(s.x[0]);
        });
        support.function(core_foundation, prefix + "Close", [self](CpuState& s, GuestMemory&) {
            if (!self->find(s.x[0])) { static_cast<void>(self->support.send_id(s.x[0], "close")); return; }
            self->close_stream(s.x[0]);
        });
        support.function(core_foundation, prefix + "GetStatus", [self](CpuState& s, GuestMemory&) {
            if (!self->find(s.x[0])) { s.x[0] = self->support.send_id(s.x[0], "streamStatus"); return; }
            const auto state = self->of(s.x[0]);
            std::lock_guard lock(state->mutex);
            s.x[0] = state->status;
        });
        support.function(core_foundation, prefix + "CopyError", [self](CpuState& s, GuestMemory&) {
            if (!self->find(s.x[0])) { s.x[0] = objc_retain(self->support, self->support.send_id(s.x[0], "streamError")); return; }
            const auto state = self->of(s.x[0]);
            std::lock_guard lock(state->mutex);
            s.x[0] = state->error ? objc_retain(self->support, state->error) : 0;
        });
        // CFStreamError CFReadStreamGetError(stream): { CFIndex domain; SInt32 error } in x0/x1.
        support.function(core_foundation, prefix + "GetError", [self](CpuState& s, GuestMemory&) {
            const auto state = self->of(s.x[0]);
            Address error{};
            { std::lock_guard lock(state->mutex); error = state->error; }
            s.x[0] = error ? static_cast<Address>(kCFStreamErrorDomainPOSIX) : 0;
            s.x[1] = error ? static_cast<std::uint32_t>(self->support.send_id(error, "code")) : 0;
        });
        support.function(core_foundation, prefix + "CopyProperty", [self](CpuState& s, GuestMemory&) {
            if (!self->find(s.x[0])) { s.x[0] = objc_retain(self->support, self->support.send_id(s.x[0], "propertyForKey:", {s.x[1]})); return; }
            s.x[0] = self->copy_property(s.x[0], s.x[1]);
        });
        support.function(core_foundation, prefix + "SetProperty", [self](CpuState& s, GuestMemory&) {
            if (!self->find(s.x[0])) { s.x[0] = self->support.send_id(s.x[0], "setProperty:forKey:", {s.x[2], s.x[1]}) & 0xff; return; }
            s.x[0] = self->set_property(s.x[0], s.x[1], s.x[2]);
        });
        // Boolean CF*StreamSetClient(stream, CFOptionFlags events, callback, CFStreamClientContext*)
        support.function(core_foundation, prefix + "SetClient", [self](CpuState& s, GuestMemory& m) {
            const auto state = self->of(s.x[0]);
            Address info{}, retain{}, release{};
            if (s.x[2] && s.x[3]) {   // { version, info, retain, release, copyDescription }
                info = m.read64(s.x[3] + 8);
                retain = m.read64(s.x[3] + 16);
                release = m.read64(s.x[3] + 24);
                if (info && retain) info = call_address(self->support, retain, {info});
            }
            Address old_info{}, old_release{};
            {
                std::lock_guard lock(state->mutex);
                old_info = std::exchange(state->client_info, s.x[2] ? info : 0);
                old_release = std::exchange(state->client_release, s.x[2] ? release : 0);
                state->client = s.x[2];
                state->client_events = s.x[2] ? s.x[1] : 0;
            }
            if (old_info && old_release) static_cast<void>(call_address(self->support, old_release, {old_info}));
            s.x[0] = 1;
        });
        support.function(core_foundation, prefix + "ScheduleWithRunLoop", [self](CpuState& s, GuestMemory&) {
            if (!self->find(s.x[0])) { static_cast<void>(self->support.send_id(s.x[0], "scheduleInRunLoop:forMode:", {s.x[1], s.x[2]})); return; }
            self->schedule(s.x[0], s.x[1], s.x[2]);
        });
        support.function(core_foundation, prefix + "UnscheduleFromRunLoop", [self](CpuState& s, GuestMemory&) {
            if (!self->find(s.x[0])) { static_cast<void>(self->support.send_id(s.x[0], "removeFromRunLoop:forMode:", {s.x[1], s.x[2]})); return; }
            self->unschedule(s.x[0], s.x[1], s.x[2]);
        });
        support.function(core_foundation, prefix + "SetDispatchQueue", [self](CpuState& s, GuestMemory&) { self->set_queue(s.x[0], s.x[1]); });
        support.function(core_foundation, prefix + "CopyDispatchQueue", [self](CpuState& s, GuestMemory&) {
            const auto state = self->of(s.x[0]);
            Address queue{};
            { std::lock_guard lock(state->mutex); queue = state->queue; }
            if (queue) static_cast<void>(self->support.call(libsystem, "_dispatch_retain", {queue}));
            s.x[0] = queue;
        });
    }
    support.function(core_foundation, "_CFReadStreamRead", [self](CpuState& s, GuestMemory&) {
        if (!self->find(s.x[0])) { s.x[0] = self->support.send_id(s.x[0], "read:maxLength:", {s.x[1], s.x[2]}); return; }
        s.x[0] = static_cast<Address>(self->read(s.x[0], s.x[1], s.x[2]));
    });
    support.function(core_foundation, "_CFReadStreamHasBytesAvailable", [self](CpuState& s, GuestMemory&) {
        if (!self->find(s.x[0])) { s.x[0] = self->support.send_id(s.x[0], "hasBytesAvailable") & 0xff; return; }
        const auto state = self->of(s.x[0]);
        std::shared_ptr<Socket> socket;
        {
            std::lock_guard lock(state->mutex);
            if (state->status != open) { s.x[0] = 0; return; }
            if (state->kind != Kind::socket) { s.x[0] = (self->readiness(*state) & has_bytes) != 0; return; }
            socket = state->socket;
        }
        GuestBlock entry(self->support, 8);
        self->support.memory().write32(entry, static_cast<std::uint32_t>(socket->fd));
        self->support.memory().write32(entry + 4, 1);
        s.x[0] = self->sys("_poll", {entry, 1, 0}) > 0;
    });
    // const UInt8* CFReadStreamGetBuffer(stream, CFIndex maxBytesToRead, CFIndex* numBytesRead): no-copy data streams only.
    support.function(core_foundation, "_CFReadStreamGetBuffer", [self](CpuState& s, GuestMemory& m) {
        const auto state = self->of(s.x[0]);
        std::lock_guard lock(state->mutex);
        if (state->kind != Kind::data || state->status != open) {
            if (s.x[2]) m.write64(s.x[2], state->status == at_end ? 0 : static_cast<Address>(-1));
            s.x[0] = 0;
            return;
        }
        const auto available = state->bytes_length - state->offset;
        const auto count = static_cast<std::int64_t>(s.x[1]) > 0 ? std::min<Address>(available, s.x[1]) : available;
        s.x[0] = state->bytes + state->offset;
        state->offset += static_cast<std::size_t>(count);
        if (state->offset == state->bytes_length) state->status = at_end;
        if (s.x[2]) m.write64(s.x[2], count);
    });
    support.function(core_foundation, "_CFWriteStreamWrite", [self](CpuState& s, GuestMemory&) {
        if (!self->find(s.x[0])) { s.x[0] = self->support.send_id(s.x[0], "write:maxLength:", {s.x[1], s.x[2]}); return; }
        s.x[0] = static_cast<Address>(self->write(s.x[0], s.x[1], s.x[2]));
    });
    support.function(core_foundation, "_CFWriteStreamCanAcceptBytes", [self](CpuState& s, GuestMemory&) {
        if (!self->find(s.x[0])) { s.x[0] = self->support.send_id(s.x[0], "hasSpaceAvailable") & 0xff; return; }
        const auto state = self->of(s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = state->status == open && (state->kind == Kind::socket || (self->readiness(*state) & can_accept) != 0);
    });
}

void Streams::install_ns() {
    const auto self = this;
    const auto input = input_class, output = output_class, stream = stream_class;
    // ---- initializers and factories ----
    support.method(input, "initWithData:", [self](CpuState& s, GuestMemory&) {
        // A copy of the data (an NSMutableData may change later), kept for the stream's lifetime.
        const auto data = s.x[2] ? self->support.send_id(s.x[2], "copy") : 0;
        self->initialize(s.x[0], true, Kind::data, [&](Stream& state) {
            state.data_object = data;
            state.bytes = data ? self->support.send_id(data, "bytes") : 0;
            state.bytes_length = data ? self->support.send_id(data, "length") : 0;
            state.bytes_deallocator = self->null_allocator;   // the NSData owns them
        });
    }, "@@:@");
    support.method(input, "initWithFileAtPath:", [self](CpuState& s, GuestMemory&) {
        const auto path = text(self->support, s.x[2]);
        self->initialize(s.x[0], true, Kind::file, [&](Stream& state) { state.path = path; });
    }, "@@:@");
    support.method(input, "initWithURL:", [self](CpuState& s, GuestMemory&) {
        const auto path = self->url_path(s.x[2]);
        self->initialize(s.x[0], true, Kind::file, [&](Stream& state) { state.path = path; });
    }, "@@:@");
    for (const auto& [name, init] : std::array<std::pair<const char*, const char*>, 3>{{
             {"inputStreamWithFileAtPath:", "initWithFileAtPath:"}, {"inputStreamWithURL:", "initWithURL:"}, {"inputStreamWithData:", "initWithData:"}}})
        support.method(input, name, [self, init = std::string(init)](CpuState& s, GuestMemory&) {
            s.x[0] = autoreleased(self->support, self->support.send_id(self->support.send_id(s.x[0], "alloc"), init, {s.x[2]}));
        }, "@@:@", true);
    support.method(output, "initToMemory", [self](CpuState& s, GuestMemory&) { self->initialize(s.x[0], false, Kind::memory, {}); }, "@@:");
    support.method(output, "initToBuffer:capacity:", [self](CpuState& s, GuestMemory&) {
        self->initialize(s.x[0], false, Kind::buffer, [&](Stream& state) { state.buffer = s.x[2]; state.capacity = s.x[3]; });
    }, "@@:^CQ");
    support.method(output, "initToFileAtPath:append:", [self](CpuState& s, GuestMemory&) {
        const auto path = text(self->support, s.x[2]);
        self->initialize(s.x[0], false, Kind::file, [&](Stream& state) { state.path = path; state.append = (s.x[3] & 0xff) != 0; });
    }, "@@:@B");
    support.method(output, "initWithURL:append:", [self](CpuState& s, GuestMemory&) {
        const auto path = self->url_path(s.x[2]);
        self->initialize(s.x[0], false, Kind::file, [&](Stream& state) { state.path = path; state.append = (s.x[3] & 0xff) != 0; });
    }, "@@:@B");
    support.method(output, "outputStreamToMemory", [self](CpuState& s, GuestMemory&) {
        s.x[0] = autoreleased(self->support, self->support.send_id(self->support.send_id(s.x[0], "alloc"), "initToMemory"));
    }, "@@:", true);
    support.method(output, "outputStreamToBuffer:capacity:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = autoreleased(self->support, self->support.send_id(self->support.send_id(s.x[0], "alloc"), "initToBuffer:capacity:", {s.x[2], s.x[3]}));
    }, "@@:^CQ", true);
    support.method(output, "outputStreamToFileAtPath:append:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = autoreleased(self->support, self->support.send_id(self->support.send_id(s.x[0], "alloc"), "initToFileAtPath:append:", {s.x[2], s.x[3]}));
    }, "@@:@B", true);
    support.method(output, "outputStreamWithURL:append:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = autoreleased(self->support, self->support.send_id(self->support.send_id(s.x[0], "alloc"), "initWithURL:append:", {s.x[2], s.x[3]}));
    }, "@@:@B", true);
    // +getBoundStreamsWithBufferSize:inputStream:outputStream: and +getStreamsToHostWithName:port:inputStream:outputStream:
    support.method(stream, "getBoundStreamsWithBufferSize:inputStream:outputStream:", [self](CpuState& s, GuestMemory& m) {
        const auto [reader, writer] = self->bound_pair(s.x[2]);
        if (s.x[3]) m.write64(s.x[3], autoreleased(self->support, reader)); else self->support.release(reader);
        if (s.x[4]) m.write64(s.x[4], autoreleased(self->support, writer)); else self->support.release(writer);
    }, "v@:Q^@^@", true);
    support.method(stream, "getStreamsToHostWithName:port:inputStream:outputStream:", [self](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) stop("+[NSStream getStreamsToHostWithName:...] without a host");
        const auto [reader, writer] = self->socket_pair(text(self->support, s.x[2]), static_cast<std::uint16_t>(s.x[3]), -1, true);
        if (s.x[4]) m.write64(s.x[4], autoreleased(self->support, reader)); else self->support.release(reader);
        if (s.x[5]) m.write64(s.x[5], autoreleased(self->support, writer)); else self->support.release(writer);
    }, "v@:@q^@^@", true);

    // ---- NSStream ----
    support.method(stream, "open", [self](CpuState& s, GuestMemory&) { static_cast<void>(self->open_stream(s.x[0])); }, "v@:");
    support.method(stream, "close", [self](CpuState& s, GuestMemory&) { self->close_stream(s.x[0]); }, "v@:");
    support.method(stream, "streamStatus", [self](CpuState& s, GuestMemory&) {
        const auto state = self->of(s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = state->status;
    }, "Q@:");
    support.method(stream, "streamError", [self](CpuState& s, GuestMemory&) {
        const auto state = self->of(s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = state->error;
    }, "@@:");
    support.method(stream, "delegate", [self](CpuState& s, GuestMemory&) {
        const auto state = self->of(s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = state->delegate ? state->delegate : s.x[0];
    }, "@@:");
    support.method(stream, "setDelegate:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->of(s.x[0]);
        std::lock_guard lock(state->mutex);
        state->delegate = s.x[2] == s.x[0] ? 0 : s.x[2];
    }, "v@:@");
    support.method(stream, "propertyForKey:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = autoreleased(self->support, self->copy_property(s.x[0], s.x[2]));
    }, "@@:@");
    support.method(stream, "setProperty:forKey:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->set_property(s.x[0], s.x[3], s.x[2]);
    }, "B@:@@");
    support.method(stream, "scheduleInRunLoop:forMode:", [self](CpuState& s, GuestMemory&) {
        const auto loop = s.x[2] ? self->support.send_id(s.x[2], "getCFRunLoop") : 0;
        if (!loop) stop("-[NSStream scheduleInRunLoop:forMode:] without a run loop");
        self->schedule(s.x[0], loop, s.x[3]);
    }, "v@:@@");
    support.method(stream, "removeFromRunLoop:forMode:", [self](CpuState& s, GuestMemory&) {
        const auto loop = s.x[2] ? self->support.send_id(s.x[2], "getCFRunLoop") : 0;
        if (loop) self->unschedule(s.x[0], loop, s.x[3]);
    }, "v@:@@");

    // ---- NSInputStream / NSOutputStream ----
    support.method(input, "read:maxLength:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<Address>(self->read(s.x[0], s.x[2], s.x[3]));
    }, "q@:^CQ");
    support.method(input, "hasBytesAvailable", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->support.call(core_foundation, "_CFReadStreamHasBytesAvailable", {s.x[0]}) & 0xff;
    }, "B@:");
    support.method(input, "getBuffer:length:", [self](CpuState& s, GuestMemory& m) {
        const auto length = self->support.process.allocate_guest(8);
        const auto buffer = self->support.call(core_foundation, "_CFReadStreamGetBuffer", {s.x[0], 0, length});
        const auto count = m.read64(length);
        self->support.process.release_guest(length);
        if (!buffer) { s.x[0] = 0; return; }
        if (s.x[2]) m.write64(s.x[2], buffer);
        if (s.x[3]) m.write64(s.x[3], count);
        s.x[0] = 1;
    }, "B@:^*^Q");
    support.method(output, "write:maxLength:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<Address>(self->write(s.x[0], s.x[2], s.x[3]));
    }, "q@:r^CQ");
    support.method(output, "hasSpaceAvailable", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->support.call(core_foundation, "_CFWriteStreamCanAcceptBytes", {s.x[0]}) & 0xff;
    }, "B@:");
}

std::unique_ptr<Streams> streams_instance;

} // namespace

void install_streams(Support& support) {
    streams_instance = std::make_unique<Streams>(support);
    streams_instance->install();
}

} // namespace alchemy::gui::completion_kit
