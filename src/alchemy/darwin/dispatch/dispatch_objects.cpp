#include "alchemy/darwin/kit/completion/runtime/common.hpp"

#include "alchemy/darwin_dispatch.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <sstream>
#include <vector>

// Dispatch objects as Objective-C objects (OS_OBJECT_USE_OBJC). On macOS a
// dispatch_queue_t is an OS_dispatch_queue_* instance: ARC retains and
// releases it through objc_retain/objc_release, Swift's Dispatch overlay
// treats DispatchQueue as the class OS_dispatch_queue, and code may message
// it (-description, -isKindOfClass:, ...). The dispatch provider
// (dispatch.cpp) writes these classes as the isa of its queues,
// groups, sources, semaphores and queue attributes; -retain/-release forward
// to dispatch_retain/dispatch_release, so the Objective-C runtime dispatches
// to them without the object store (as for OS_os_log, oslog.cpp).
//
// Hierarchy (as libdispatch's): OS_object > OS_dispatch_object >
// OS_dispatch_queue > OS_dispatch_queue_serial > OS_dispatch_queue_main,
// OS_dispatch_queue > OS_dispatch_queue_concurrent / _global, and
// OS_dispatch_object > OS_dispatch_queue_attr / _group / _source /
// _semaphore / _io.
//
// dispatch I/O channels (dispatch_io_create, _create_with_path, _create_with_io,
// _read, _write, _barrier, _close, _get_descriptor, _set_high_water,
// _set_low_water, _set_interval) are implemented here over the guest's
// read/write/pread/pwrite on dispatch worker threads; handlers are submitted to
// their queue in order with dispatch_sync_f, data is delivered as
// dispatch_data_t (copied, at most the high-water mark and, before the end, at
// least the low-water mark at a time), and the cleanup handler runs on the
// channel's queue once the channel is closed (or released) and no operation is
// pending. Descriptor ownership stays with the caller, except for a path
// channel's own descriptor, closed at cleanup. Deliveries are not paced by
// dispatch_io_set_interval: data is handed over as it is read. dispatch_read and
// dispatch_write are one-shot operations on a descriptor with a single handler
// call.
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;

constexpr std::uint64_t io_stream = 0, io_random = 1, io_stop = 1;
constexpr std::uint32_t error_canceled = 89;   // ECANCELED
constexpr std::uint64_t chunk = 64 * 1024;

struct Channel {
    std::int32_t descriptor{-1};
    std::uint64_t type{};
    Address queue{}, cleanup{};   // retained queue, copied block (0 when none)
    std::uint64_t references{1}, pending{};
    bool closed{}, stopped{}, cleaned{};
    bool owned{};                 // a path channel's descriptor, closed at cleanup
    std::uint32_t open_error{};   // a path channel whose open failed: every operation and the cleanup report it
    std::uint64_t high_water{UINT64_MAX}, low_water{}, interval{};
    std::vector<Address> barriers;   // copied blocks waiting for pending operations to finish
};

struct Operation {
    Address channel{}, queue{}, handler{}, data{};
    std::int64_t offset{};
    std::uint64_t length{};
    bool write{};
    std::int32_t descriptor{-1};   // dispatch_read / dispatch_write: the descriptor (no channel)
};

struct Channels {
    std::mutex mutex;
    std::map<Address, Channel> objects;
    std::map<Address, Operation> operations;
    Address next{1};
    Address io_class{};
    Address read_entry{}, deliver_entry{}, cleanup_entry{}, oneshot_entry{}, deliver_pair_entry{}, barrier_entry{};
};

// The pending handler call that deliver_entry runs on the target queue.
struct Delivery { Address block{}; bool done{}; Address data{}; std::uint32_t error{}; };

} // namespace

void install_dispatch_objects(Support& support) {
    // Processes without the libdispatch provider (provider unit tests) have no
    // dispatch objects to classify.
    if (!support.process.resolve(libsystem, "_dispatch_async_f")) return;
    const auto object_class = support.declare_class("OS_object", "NSObject", libsystem);
    const auto dispatch_object = support.declare_class("OS_dispatch_object", "OS_object", libsystem);
    const auto queue = support.declare_class("OS_dispatch_queue", "OS_dispatch_object", libsystem);
    const auto serial = support.declare_class("OS_dispatch_queue_serial", "OS_dispatch_queue", libsystem);
    const auto main = support.declare_class("OS_dispatch_queue_main", "OS_dispatch_queue_serial", libsystem);
    const auto concurrent = support.declare_class("OS_dispatch_queue_concurrent", "OS_dispatch_queue", libsystem);
    const auto global = support.declare_class("OS_dispatch_queue_global", "OS_dispatch_queue", libsystem);
    const auto attribute = support.declare_class("OS_dispatch_queue_attr", "OS_dispatch_object", libsystem);
    const auto group = support.declare_class("OS_dispatch_group", "OS_dispatch_object", libsystem);
    const auto source = support.declare_class("OS_dispatch_source", "OS_dispatch_object", libsystem);
    const auto semaphore = support.declare_class("OS_dispatch_semaphore", "OS_dispatch_object", libsystem);
    static_cast<void>(object_class);
    static_cast<void>(queue);
    auto channels = std::make_shared<Channels>();
    channels->io_class = support.declare_class("OS_dispatch_io", "OS_dispatch_object", libsystem);
    for (const auto& [cls, protocol] : std::array<std::pair<Address, const char*>, 2>{{{dispatch_object, "OS_dispatch_object"}, {queue, "OS_dispatch_queue"}}})
        support.objc.add_native_protocol(cls, protocol);

    // dispatch_retain / dispatch_release resolved at call time: providers
    // installed later (dispatch data, I/O channels) wrap them.
    const auto forward = [&support](std::string_view symbol, Address object) {
        return support.call(libsystem, symbol, {object});
    };
    support.method(dispatch_object, "retain", [forward](CpuState& s, GuestMemory&) {
        static_cast<void>(forward("_dispatch_retain", s.x[0]));
    }, "@@:");
    support.method(dispatch_object, "release", [forward](CpuState& s, GuestMemory&) {
        static_cast<void>(forward("_dispatch_release", s.x[0]));
    }, "v@:");
    // -autorelease queues the object in the thread's innermost pool; the
    // pool's drain sends -release.
    support.method(dispatch_object, "autorelease", [&support](CpuState& s, GuestMemory&) {
        const auto thread = support.process.current_thread();
        if (thread->autorelease_pools.empty()) thread->unpooled_autoreleases.push_back(s.x[0]);
        else thread->autorelease_pools.back().objects.push_back(s.x[0]);
    }, "@@:");
    support.method(dispatch_object, "_tryRetain", [forward](CpuState& s, GuestMemory&) {
        static_cast<void>(forward("_dispatch_retain", s.x[0]));
        s.x[0] = 1;
    }, "B@:");
    support.method(dispatch_object, "_isDeallocating", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    support.method(dispatch_object, "allowsWeakReference", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    support.method(dispatch_object, "retainWeakReference", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    const auto info_of = [&support, channels](Address object) -> std::optional<DispatchObjectInfo> {
        if (auto info = dispatch_object_info(support.process, object)) return info;
        std::lock_guard lock(channels->mutex);
        if (const auto found = channels->objects.find(object); found != channels->objects.end())
            return DispatchObjectInfo{"io", {}, found->second.references, false};
        return std::nullopt;
    };
    support.method(dispatch_object, "retainCount", [info_of](CpuState& s, GuestMemory&) {
        const auto info = info_of(s.x[0]);
        if (!info) fail("-[OS_dispatch_object retainCount] of an unknown dispatch object");
        s.x[0] = info->immortal ? UINT64_MAX : info->references;
    }, "Q@:");
    const auto describe = [&support, info_of](Address object) {
        const auto info = info_of(object);
        std::ostringstream text;
        const auto cls = support.objc.object_class(object);
        text << "<" << (cls ? support.cf.utf8(support.call(foundation_library, "_NSStringFromClass", {cls})) : std::string("OS_dispatch_object"));
        if (info && !info->label.empty()) text << ": " << info->label;
        text << "[0x" << std::hex << object << "]";
        if (info) text << " = { kind = " << info->kind << ", xref = " << std::dec << info->references << " }";
        text << ">";
        return text.str();
    };
    for (const auto* name : {"description", "debugDescription"})
        support.method(dispatch_object, name, [&support, describe](CpuState& s, GuestMemory&) {
            s.x[0] = string(support, describe(s.x[0]));
        }, "@@:");

    DispatchObjectClasses classes;
    classes.serial_queue = serial;
    classes.concurrent_queue = concurrent;
    classes.main_queue = main;
    classes.global_queue = global;
    classes.queue_attribute = attribute;
    classes.group = group;
    classes.source = source;
    classes.semaphore = semaphore;
    set_dispatch_object_classes(support.process, classes);

    // ---- dispatch I/O channels ------------------------------------------------------
    const auto submit = [&support](Address target, Address function, Address context, bool sync) {
        static_cast<void>(support.call(libsystem, sync ? "_dispatch_sync_f" : "_dispatch_async_f", {target, context, function}));
    };
    const auto global_queue = [&support] { return support.call(libsystem, "_dispatch_get_global_queue", {0, 0}); };
    // Runs the cleanup handler once the channel is closed and idle; caller
    // must not hold the mutex.
    const auto maybe_cleanup = [&support, channels, submit](Address object) {
        Address queue_object{}, block{};
        std::uint32_t error{};
        std::int32_t owned_descriptor{-1};
        {
            std::lock_guard lock(channels->mutex);
            const auto found = channels->objects.find(object);
            if (found == channels->objects.end()) return;
            auto& value = found->second;
            if (!value.closed || value.pending || value.cleaned) return;
            value.cleaned = true;
            queue_object = value.queue;
            block = std::exchange(value.cleanup, 0);
            error = value.open_error;
            if (value.owned && value.descriptor >= 0) owned_descriptor = std::exchange(value.descriptor, -1);
        }
        if (owned_descriptor >= 0) static_cast<void>(support.call(libsystem, "_close", {static_cast<Address>(owned_descriptor)}));
        if (block) {
            // cleanup_handler(int error): 0 after an orderly close.
            auto delivery = std::make_shared<Delivery>();
            delivery->block = block;
            Address id{};
            {
                std::lock_guard lock(channels->mutex);
                id = channels->next++;
                channels->operations.emplace(id, Operation{object, queue_object, block, 0, 0, error, false});
            }
            submit(queue_object, channels->cleanup_entry, id, false);
        }
    };
    const auto release_channel = [&support, channels, maybe_cleanup](Address object) {
        bool destroy{};
        Address queue_object{};
        {
            std::lock_guard lock(channels->mutex);
            const auto found = channels->objects.find(object);
            if (found == channels->objects.end()) fail("dispatch_release of an unknown dispatch I/O channel");
            auto& value = found->second;
            if (--value.references) return;
            value.closed = true;   // the last release closes the channel
        }
        maybe_cleanup(object);
        {
            std::lock_guard lock(channels->mutex);
            const auto found = channels->objects.find(object);
            if (found != channels->objects.end() && found->second.cleaned && !found->second.pending) {
                queue_object = found->second.queue;
                channels->objects.erase(found);
                destroy = true;
            }
        }
        if (!destroy) return;
        static_cast<void>(support.call(libsystem, "_dispatch_release", {queue_object}));
        support.process.release_guest(object);
    };
    // dispatch_retain/dispatch_release accept channels (a reference handler of
    // the dispatch provider, which owns both exports).
    if (!support.process.resolve(libsystem, "_dispatch_retain")) fail("_dispatch_retain is not provided");
    add_dispatch_reference_handler(support.process, [channels, release_channel](Address object, bool retain) {
        {
            std::lock_guard lock(channels->mutex);
            const auto found = channels->objects.find(object);
            if (found == channels->objects.end()) return false;
            if (retain) ++found->second.references;
        }
        if (!retain) release_channel(object);
        return true;
    });
    // void handler(bool done, dispatch_data_t data, int error) on the target queue.
    channels->deliver_entry = support.process.install_function("___alchemy_completion_dispatch_io_deliver", [&support](CpuState& s, GuestMemory& m) {
        const auto block = m.read64(s.x[0]), data = m.read64(s.x[0] + 16);
        const auto done = m.read64(s.x[0] + 8), error = m.read64(s.x[0] + 24);
        static_cast<void>(invoke_block(support, block, {done, data, error}));
        s.x[0] = 0;
    }, "alchemy.internal");
    channels->cleanup_entry = support.process.install_function("___alchemy_completion_dispatch_io_cleanup", [&support, channels](CpuState& s, GuestMemory&) {
        Operation operation;
        {
            std::lock_guard lock(channels->mutex);
            const auto found = channels->operations.find(s.x[0]);
            if (found == channels->operations.end()) fail("dispatch I/O cleanup without its record");
            operation = found->second;
            channels->operations.erase(found);
        }
        static_cast<void>(invoke_block(support, operation.handler, {operation.length}));
        block_release(support, operation.handler);
        s.x[0] = 0;
    }, "alchemy.internal");
    // Hands one handler call to the operation's queue and waits for it, so
    // calls stay in order on concurrent queues too.
    const auto deliver = [&support, channels, submit](const Operation& operation, bool done, Address data, std::uint32_t error) {
        GuestBlock record(support, 32);
        auto& m = support.memory();
        m.write64(record, operation.handler);
        m.write64(record + 8, done ? 1 : 0);
        m.write64(record + 16, data);
        m.write64(record + 24, error);
        submit(operation.queue, channels->deliver_entry, record, true);
    };
    const auto finish = [&support, channels, maybe_cleanup, release_channel, submit](const Operation& operation) {
        block_release(support, operation.handler);
        static_cast<void>(support.call(libsystem, "_dispatch_release", {operation.queue}));
        std::vector<Address> barriers;
        Address queue_object{};
        {
            std::lock_guard lock(channels->mutex);
            if (const auto found = channels->objects.find(operation.channel); found != channels->objects.end()) {
                if (!--found->second.pending) barriers.swap(found->second.barriers);
                queue_object = found->second.queue;
            }
        }
        for (const auto barrier : barriers) submit(queue_object, channels->barrier_entry, barrier, false);
        maybe_cleanup(operation.channel);
        release_channel(operation.channel);   // the reference the operation held
    };
    channels->read_entry = support.process.install_function("___alchemy_completion_dispatch_io_run", [&support, channels, deliver, finish](CpuState& s, GuestMemory& m) {
        Operation operation;
        std::int32_t descriptor{};
        std::uint64_t type{}, high_water{UINT64_MAX}, low_water{};
        std::uint32_t open_error{};
        {
            std::lock_guard lock(channels->mutex);
            const auto found = channels->operations.find(s.x[0]);
            if (found == channels->operations.end()) fail("dispatch I/O operation without its record");
            operation = found->second;
            channels->operations.erase(found);
            const auto& channel = channels->objects.at(operation.channel);
            descriptor = channel.descriptor;
            type = channel.type;
            open_error = channel.open_error;
            high_water = std::max<std::uint64_t>(1, channel.high_water);
            low_water = channel.low_water;
        }
        const auto stopped = [&] {
            std::lock_guard lock(channels->mutex);
            return channels->objects.at(operation.channel).stopped;
        };
        std::uint32_t error{};
        if (open_error) {
            const auto empty = support.process.resolve(libsystem, "__dispatch_data_empty");
            deliver(operation, true, operation.write ? operation.data : (empty ? empty->address : 0), open_error);
            if (operation.write) static_cast<void>(support.call(libsystem, "_dispatch_release", {operation.data}));
        } else if (!operation.write) {
            GuestBlock buffer(support, chunk, false);
            std::uint64_t consumed{};
            // Data waiting for the low-water mark (a retained dispatch_data_t, 0 when none).
            Address held{};
            std::uint64_t held_size{};
            const auto hand_over = [&](bool done, std::uint32_t code) {
                const auto empty = support.process.resolve(libsystem, "__dispatch_data_empty");
                deliver(operation, done, held ? held : (empty ? empty->address : 0), code);
                if (held) static_cast<void>(support.call(libsystem, "_dispatch_release", {held}));
                held = 0; held_size = 0;
            };
            while (consumed < operation.length) {
                if (stopped()) { error = error_canceled; break; }
                const auto want = std::min({chunk, high_water - std::min(high_water, held_size), operation.length - consumed});
                if (!want) { hand_over(false, 0); continue; }   // the high-water mark is reached
                const auto count = static_cast<std::int64_t>(type == io_random
                    ? support.call(libsystem, "_pread", {static_cast<Address>(descriptor), buffer, want, static_cast<Address>(operation.offset + static_cast<std::int64_t>(consumed))})
                    : support.call(libsystem, "_read", {static_cast<Address>(descriptor), buffer, want}));
                if (count < 0) { error = guest_errno(support); break; }
                if (count == 0) break;   // end of file
                consumed += static_cast<std::uint64_t>(count);
                const auto data = support.call(libsystem, "_dispatch_data_create", {buffer, static_cast<Address>(count), 0, 0});
                if (held) {
                    const auto joined = support.call(libsystem, "_dispatch_data_create_concat", {held, data});
                    static_cast<void>(support.call(libsystem, "_dispatch_release", {held}));
                    static_cast<void>(support.call(libsystem, "_dispatch_release", {data}));
                    held = joined;
                } else held = data;
                held_size += static_cast<std::uint64_t>(count);
                if (held_size >= std::max<std::uint64_t>(low_water, 1)) hand_over(false, 0);
            }
            hand_over(true, error);
        } else {
            GuestBlock pointer(support, 16);
            const auto map = support.call(libsystem, "_dispatch_data_create_map", {operation.data, pointer, pointer + 8});
            const auto bytes = m.read64(pointer), size = m.read64(pointer + 8);
            std::uint64_t written{};
            while (written < size) {
                if (stopped()) { error = error_canceled; break; }
                const auto count = static_cast<std::int64_t>(type == io_random
                    ? support.call(libsystem, "_pwrite", {static_cast<Address>(descriptor), bytes + written, size - written, static_cast<Address>(operation.offset + static_cast<std::int64_t>(written))})
                    : support.call(libsystem, "_write", {static_cast<Address>(descriptor), bytes + written, size - written}));
                if (count < 0) { error = guest_errno(support); break; }
                written += static_cast<std::uint64_t>(count);
            }
            // The data not written, when the write ended early.
            Address remaining{};
            if (written < size) remaining = support.call(libsystem, "_dispatch_data_create_subrange", {operation.data, written, size - written});
            static_cast<void>(support.call(libsystem, "_dispatch_release", {map}));
            deliver(operation, true, remaining, error);
            if (remaining) static_cast<void>(support.call(libsystem, "_dispatch_release", {remaining}));
            static_cast<void>(support.call(libsystem, "_dispatch_release", {operation.data}));
        }
        finish(operation);
        s.x[0] = 0;
    }, "alchemy.internal");

    // dispatch_io_t dispatch_io_create(dispatch_io_type_t, dispatch_fd_t, dispatch_queue_t, void (^cleanup)(int))
    support.function(libsystem, "_dispatch_io_create", [&support, channels](CpuState& s, GuestMemory& m) {
        if (s.x[0] != io_stream && s.x[0] != io_random) { s.x[0] = 0; return; }
        if (!s.x[2]) fail("dispatch_io_create with a NULL queue");
        Channel channel;
        channel.type = s.x[0];
        channel.descriptor = static_cast<std::int32_t>(s.x[1]);
        channel.queue = s.x[2];
        static_cast<void>(support.call(libsystem, "_dispatch_retain", {channel.queue}));
        channel.cleanup = block_copy(support, s.x[3]);
        const auto object = support.process.allocate_guest(32, 16);
        m.fill_bytes(object, 0, 32);
        m.write64(object, channels->io_class);
        { std::lock_guard lock(channels->mutex); channels->objects.emplace(object, channel); }
        s.x[0] = object;
    });
    const auto start = [&support, channels, submit, global_queue](Operation operation) {
        Address id{};
        {
            std::lock_guard lock(channels->mutex);
            const auto found = channels->objects.find(operation.channel);
            if (found == channels->objects.end()) fail("dispatch I/O on an unknown channel");
            if (found->second.closed) return false;
            ++found->second.pending;
            ++found->second.references;
            id = channels->next++;
        }
        operation.handler = block_copy(support, operation.handler);
        static_cast<void>(support.call(libsystem, "_dispatch_retain", {operation.queue}));
        if (operation.data) static_cast<void>(support.call(libsystem, "_dispatch_retain", {operation.data}));
        { std::lock_guard lock(channels->mutex); channels->operations.emplace(id, operation); }
        submit(global_queue(), channels->read_entry, id, false);
        return true;
    };
    const auto closed_error = [&support, channels, submit](Address target, Address handler) {
        // An operation on a closed channel completes at once with ECANCELED.
        GuestBlock record(support, 32);
        auto& m = support.memory();
        m.write64(record, handler);
        m.write64(record + 8, 1);
        m.write64(record + 16, 0);
        m.write64(record + 24, error_canceled);
        submit(target, channels->deliver_entry, record, true);
    };
    // void dispatch_io_read(dispatch_io_t, off_t, size_t, dispatch_queue_t, handler)
    support.function(libsystem, "_dispatch_io_read", [start, closed_error](CpuState& s, GuestMemory&) {
        if (!s.x[4]) fail("dispatch_io_read with a NULL handler");
        Operation operation{s.x[0], s.x[3], s.x[4], 0, static_cast<std::int64_t>(s.x[1]), s.x[2], false};
        if (!start(operation)) closed_error(s.x[3], s.x[4]);
    });
    // void dispatch_io_write(dispatch_io_t, off_t, dispatch_data_t, dispatch_queue_t, handler)
    support.function(libsystem, "_dispatch_io_write", [start, closed_error](CpuState& s, GuestMemory&) {
        if (!s.x[4] || !s.x[2]) fail("dispatch_io_write with a NULL handler or data");
        Operation operation{s.x[0], s.x[3], s.x[4], s.x[2], static_cast<std::int64_t>(s.x[1]), 0, true};
        if (!start(operation)) closed_error(s.x[3], s.x[4]);
    });
    // dispatch_io_t dispatch_io_create_with_path(type, const char* path, int oflag, mode_t mode,
    // dispatch_queue_t, void (^cleanup)(int)): the channel opens the path now and owns the
    // descriptor; a failed open makes every operation and the cleanup report its errno.
    support.function(libsystem, "_dispatch_io_create_with_path", [&support, channels](CpuState& s, GuestMemory& m) {
        if (s.x[0] != io_stream && s.x[0] != io_random) { s.x[0] = 0; return; }
        if (!s.x[4]) fail("dispatch_io_create_with_path with a NULL queue");
        if (!s.x[1] || m.read8(s.x[1]) != '/') { s.x[0] = 0; return; }   // an absolute path is required
        Channel channel;
        channel.type = s.x[0];
        CallRegs registers{};
        registers.x[0] = s.x[1]; registers.x[1] = s.x[2];
        registers.push_variadic(static_cast<std::uint64_t>(static_cast<std::uint16_t>(s.x[3])));
        const auto descriptor = static_cast<std::int32_t>(support.call(libsystem, "_open", registers).x0);
        if (descriptor < 0) channel.open_error = guest_errno(support);
        else { channel.descriptor = descriptor; channel.owned = true; }
        channel.queue = s.x[4];
        static_cast<void>(support.call(libsystem, "_dispatch_retain", {channel.queue}));
        channel.cleanup = block_copy(support, s.x[5]);
        const auto object = support.process.allocate_guest(32, 16);
        m.fill_bytes(object, 0, 32);
        m.write64(object, channels->io_class);
        { std::lock_guard lock(channels->mutex); channels->objects.emplace(object, channel); }
        s.x[0] = object;
    });
    // dispatch_io_t dispatch_io_create_with_io(type, dispatch_io_t, queue, cleanup): a channel on
    // the other channel's descriptor (owned by neither).
    support.function(libsystem, "_dispatch_io_create_with_io", [&support, channels](CpuState& s, GuestMemory& m) {
        if (s.x[0] != io_stream && s.x[0] != io_random) { s.x[0] = 0; return; }
        if (!s.x[2]) fail("dispatch_io_create_with_io with a NULL queue");
        Channel channel;
        {
            std::lock_guard lock(channels->mutex);
            const auto found = channels->objects.find(s.x[1]);
            if (found == channels->objects.end()) fail("dispatch_io_create_with_io of an unknown channel");
            if (found->second.closed) { s.x[0] = 0; return; }
            channel.descriptor = found->second.descriptor;
            channel.open_error = found->second.open_error;
        }
        channel.type = s.x[0];
        channel.queue = s.x[2];
        static_cast<void>(support.call(libsystem, "_dispatch_retain", {channel.queue}));
        channel.cleanup = block_copy(support, s.x[3]);
        const auto object = support.process.allocate_guest(32, 16);
        m.fill_bytes(object, 0, 32);
        m.write64(object, channels->io_class);
        { std::lock_guard lock(channels->mutex); channels->objects.emplace(object, channel); }
        s.x[0] = object;
    });
    // void dispatch_io_barrier(dispatch_io_t, dispatch_block_t): runs once the operations
    // enqueued before it finished (on the channel's queue).
    channels->barrier_entry = support.process.install_function("___alchemy_completion_dispatch_io_barrier", [&support](CpuState& s, GuestMemory&) {
        static_cast<void>(invoke_block(support, s.x[0], {}));
        block_release(support, s.x[0]);
        s.x[0] = 0;
    }, "alchemy.internal");
    support.function(libsystem, "_dispatch_io_barrier", [&support, channels, submit](CpuState& s, GuestMemory&) {
        if (!s.x[1]) fail("dispatch_io_barrier with a NULL block");
        const auto copy = block_copy(support, s.x[1]);
        Address queue_object{};
        {
            std::lock_guard lock(channels->mutex);
            const auto found = channels->objects.find(s.x[0]);
            if (found == channels->objects.end()) fail("dispatch_io_barrier of an unknown channel");
            if (found->second.pending) { found->second.barriers.push_back(copy); return; }
            queue_object = found->second.queue;
        }
        submit(queue_object, channels->barrier_entry, copy, false);
    });
    // dispatch_io_set_high_water / _set_low_water(channel, size_t) and
    // dispatch_io_set_interval(channel, uint64_t interval, flags).
    for (const auto& [name, which] : std::array<std::pair<const char*, int>, 3>{{
             {"_dispatch_io_set_high_water", 0}, {"_dispatch_io_set_low_water", 1}, {"_dispatch_io_set_interval", 2}}})
        support.function(libsystem, name, [channels, which](CpuState& s, GuestMemory&) {
            std::lock_guard lock(channels->mutex);
            const auto found = channels->objects.find(s.x[0]);
            if (found == channels->objects.end()) fail("dispatch I/O setting of an unknown channel");
            auto& value = found->second;
            if (which == 0) { value.high_water = s.x[1] ? s.x[1] : 1; if (value.low_water > value.high_water) value.low_water = value.high_water; }
            else if (which == 1) { value.low_water = s.x[1]; if (value.high_water < value.low_water) value.high_water = value.low_water; }
            else value.interval = s.x[1];
        });
    // dispatch_read(fd, length, queue, ^(dispatch_data_t data, int error)) and
    // dispatch_write(fd, data, queue, ^(dispatch_data_t remaining, int error)): one handler call
    // once all the data is read (to SIZE_MAX: the end of file) or written, or on an error.
    channels->deliver_pair_entry = support.process.install_function("___alchemy_completion_dispatch_io_deliver_pair", [&support](CpuState& s, GuestMemory& m) {
        static_cast<void>(invoke_block(support, m.read64(s.x[0]), {m.read64(s.x[0] + 8), m.read64(s.x[0] + 16)}));
        s.x[0] = 0;
    }, "alchemy.internal");
    channels->oneshot_entry = support.process.install_function("___alchemy_completion_dispatch_io_oneshot", [&support, channels, submit](CpuState& s, GuestMemory& m) {
        Operation operation;
        {
            std::lock_guard lock(channels->mutex);
            const auto found = channels->operations.find(s.x[0]);
            if (found == channels->operations.end()) fail("dispatch_read/dispatch_write without its record");
            operation = found->second;
            channels->operations.erase(found);
        }
        const auto descriptor = static_cast<Address>(operation.descriptor);
        std::uint32_t error{};
        Address result{};
        if (!operation.write) {
            GuestBlock buffer(support, chunk, false);
            std::uint64_t consumed{};
            while (consumed < operation.length) {
                const auto want = std::min<std::uint64_t>(chunk, operation.length - consumed);
                const auto count = static_cast<std::int64_t>(support.call(libsystem, "_read", {descriptor, buffer, want}));
                if (count < 0) { error = guest_errno(support); break; }
                if (count == 0) break;
                consumed += static_cast<std::uint64_t>(count);
                const auto data = support.call(libsystem, "_dispatch_data_create", {buffer, static_cast<Address>(count), 0, 0});
                if (result) {
                    const auto joined = support.call(libsystem, "_dispatch_data_create_concat", {result, data});
                    static_cast<void>(support.call(libsystem, "_dispatch_release", {result}));
                    static_cast<void>(support.call(libsystem, "_dispatch_release", {data}));
                    result = joined;
                } else result = data;
            }
        } else {
            GuestBlock pointer(support, 16);
            const auto map = support.call(libsystem, "_dispatch_data_create_map", {operation.data, pointer, pointer + 8});
            const auto bytes = m.read64(pointer), size = m.read64(pointer + 8);
            std::uint64_t written{};
            while (written < size) {
                const auto count = static_cast<std::int64_t>(support.call(libsystem, "_write", {descriptor, bytes + written, size - written}));
                if (count < 0) { error = guest_errno(support); break; }
                written += static_cast<std::uint64_t>(count);
            }
            if (written < size) result = support.call(libsystem, "_dispatch_data_create_subrange", {operation.data, written, size - written});
            static_cast<void>(support.call(libsystem, "_dispatch_release", {map}));
            static_cast<void>(support.call(libsystem, "_dispatch_release", {operation.data}));
        }
        const auto empty = support.process.resolve(libsystem, "__dispatch_data_empty");
        GuestBlock record(support, 24);
        m.write64(record, operation.handler);
        m.write64(record + 8, result ? result : (!operation.write && empty ? empty->address : 0));
        m.write64(record + 16, error);
        submit(operation.queue, channels->deliver_pair_entry, record, true);
        if (result) static_cast<void>(support.call(libsystem, "_dispatch_release", {result}));
        block_release(support, operation.handler);
        static_cast<void>(support.call(libsystem, "_dispatch_release", {operation.queue}));
        s.x[0] = 0;
    }, "alchemy.internal");
    for (const bool write : {false, true})
        support.function(libsystem, write ? "_dispatch_write" : "_dispatch_read", [&support, channels, submit, global_queue, write](CpuState& s, GuestMemory&) {
            if (!s.x[2] || !s.x[3] || (write && !s.x[1])) fail(write ? "dispatch_write with a NULL queue, data or handler" : "dispatch_read with a NULL queue or handler");
            Operation operation;
            operation.descriptor = static_cast<std::int32_t>(s.x[0]);
            operation.write = write;
            if (write) { operation.data = s.x[1]; static_cast<void>(support.call(libsystem, "_dispatch_retain", {operation.data})); }
            else operation.length = s.x[1];
            operation.queue = s.x[2];
            static_cast<void>(support.call(libsystem, "_dispatch_retain", {operation.queue}));
            operation.handler = block_copy(support, s.x[3]);
            Address id{};
            { std::lock_guard lock(channels->mutex); id = channels->next++; channels->operations.emplace(id, operation); }
            submit(global_queue(), channels->oneshot_entry, id, false);
        });
    // void dispatch_io_close(dispatch_io_t, dispatch_io_close_flags_t)
    support.function(libsystem, "_dispatch_io_close", [channels, maybe_cleanup](CpuState& s, GuestMemory&) {
        {
            std::lock_guard lock(channels->mutex);
            const auto found = channels->objects.find(s.x[0]);
            if (found == channels->objects.end()) fail("dispatch_io_close of an unknown channel");
            found->second.closed = true;
            if (s.x[1] & io_stop) found->second.stopped = true;
        }
        maybe_cleanup(s.x[0]);
    });
    support.function(libsystem, "_dispatch_io_get_descriptor", [channels](CpuState& s, GuestMemory&) {
        std::lock_guard lock(channels->mutex);
        const auto found = channels->objects.find(s.x[0]);
        if (found == channels->objects.end()) fail("dispatch_io_get_descriptor of an unknown channel");
        s.x[0] = found->second.closed ? static_cast<Address>(static_cast<std::int64_t>(-1)) : static_cast<Address>(static_cast<std::uint32_t>(found->second.descriptor));
    });
}

} // namespace alchemy::gui::completion_kit
