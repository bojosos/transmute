#include "alchemy/gui/value_provider.hpp"

#include "alchemy/darwin_threads.hpp"
#include "alchemy/host/files.hpp"
#include "alchemy/path.hpp"

#include <array>
#include <bit>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <thread>

// FSEvents (CoreServices): FSEventStream over the host's recursive directory
// watches (host::files::watch_tree).
//
// A started stream has a watcher, a guest dispatch worker that reads the host
// watches of its paths and turns each change into an event: the item's path
// and kFSEventStreamEventFlagItemCreated / Removed / Renamed / Modified with
// ItemIsFile or ItemIsDir (from the item as it is now; a removed item counts
// as a file). Without kFSEventStreamCreateFlagFileEvents an event names the
// changed item's directory ("/dir/"), flags 0, as FSEvents does. Changes the
// host lost become one kFSEventStreamEventFlagMustScanSubDirs |
// UserDropped event for the watched path. Events of one path within the
// stream's latency are coalesced (flags OR-ed); paths under an exclusion
// path are dropped.
//
// Delivery: the callback runs on the stream's dispatch queue
// (FSEventStreamSetDispatchQueue) or, when the stream is scheduled on run
// loops, from a version-0 run loop source on them, with the event paths as
// a C string array, or a CFArray of CFStrings (kFSEventStreamCreateFlagUseCFTypes),
// or of CFDictionaries {path, fileID} (UseExtendedData; fileID is 0: the
// host's file ids are not exposed). Event ids count up from 1 per process.
//
// Not provided: history (sinceWhen other than kFSEventStreamEventIdSinceNow
// replays nothing; no HistoryDone event is sent), device-relative streams
// (FSEventStreamCreateRelativeToDevice fails: NULL), IgnoreSelf / MarkSelf
// (own events are not told apart), and root-path change tracking (WatchRoot).
namespace alchemy::gui::values {
namespace {

using fill::core_foundation;
using fill::core_services;

constexpr std::uint32_t create_use_cf_types = 0x1, create_file_events = 0x10, create_use_extended_data = 0x40;
constexpr std::uint32_t flag_must_scan_subdirs = 0x1, flag_user_dropped = 0x2, item_created = 0x100, item_removed = 0x200,
    item_renamed = 0x800, item_modified = 0x1000, item_is_file = 0x10000, item_is_dir = 0x20000;

std::atomic<std::uint64_t> last_event_id{0};

struct Event {
    std::string path;
    std::uint32_t flags{};
    std::uint64_t id{};
};

struct Stream final : ObjectPayload {
    std::mutex mutex;
    Address callback{}, info{}, info_release{};
    std::vector<std::string> paths, exclusions;   // guest paths without a trailing '/'
    double latency{};
    std::uint32_t flags{};
    Address queue{};                                         // retained dispatch queue
    Address source{};                                        // +1 run loop source
    std::vector<std::pair<Address, Address>> schedules;      // run loop, mode (+1 each)
    bool started{}, invalidated{}, delivery_requested{};
    std::uint64_t generation{}, latest{};
    std::vector<Event> pending;
};

std::string trimmed(std::string path) {
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
}

bool under(const std::string& path, const std::string& prefix) {
    return path == prefix || (path.size() > prefix.size() && path.starts_with(prefix) && path[prefix.size()] == '/');
}

} // namespace

void Provider::install_fsevents() {
    const auto stream_class = cls(core_services, "__FSEventStream", "NSObject");
    const auto state_of = [this](Address object) {
        auto value = object ? objc.find_host_state<Stream>(object) : nullptr;
        if (!value) throw std::runtime_error("Alchemy: FSEvents call needs an FSEventStreamRef");
        return value;
    };

    // Runs the callback with the pending events (on the queue or run loop thread).
    const auto deliver = [this, state_of](Address stream) {
        const auto state = state_of(stream);
        std::vector<Event> events;
        Address callback{}, info{};
        std::uint32_t flags{};
        {
            std::lock_guard lock(state->mutex);
            state->delivery_requested = false;
            if (state->invalidated || state->pending.empty()) return;
            events.swap(state->pending);
            callback = state->callback;
            info = state->info;
            flags = state->flags;
            state->latest = events.back().id;
        }
        auto& m = memory();
        const auto count = events.size();
        const auto flag_array = process.allocate_guest(count * 4), id_array = process.allocate_guest(count * 8);
        std::vector<Address> owned{flag_array, id_array}, objects;
        Address paths{};
        try {
            if (flags & create_use_cf_types) {
                for (const auto& event : events) {
                    const auto text = cf.string(event.path);
                    if (flags & create_use_extended_data) {
                        const auto file_id = cf.integer(0, 4);
                        const std::array<std::pair<Address, Address>, 2> entries{{{cf.string("path"), text}, {cf.string("fileID"), file_id}}};
                        objects.push_back(cf.dictionary(entries));
                        for (const auto& [key, value] : entries) { release(key); release(value); }
                    } else objects.push_back(text);
                }
                paths = cf.array(objects);
            } else {
                paths = process.allocate_guest(count * 8);
                owned.push_back(paths);
                for (std::size_t index = 0; index < count; ++index) {
                    const auto text = process.allocate_guest(events[index].path.size() + 1);
                    owned.push_back(text);
                    m.write_bytes(text, std::span(reinterpret_cast<const std::uint8_t*>(events[index].path.data()), events[index].path.size()));
                    m.write8(text + events[index].path.size(), 0);
                    m.write64(paths + index * 8, text);
                }
            }
            for (std::size_t index = 0; index < count; ++index) {
                m.write32(flag_array + index * 4, events[index].flags);
                m.write64(id_array + index * 8, events[index].id);
            }
            // void callback(ConstFSEventStreamRef, void* info, size_t numEvents, void* eventPaths,
            //               const FSEventStreamEventFlags flags[], const FSEventStreamEventId ids[])
            const std::array<Address, 6> arguments{stream, info, count, paths, flag_array, id_array};
            static_cast<void>(process.calling_engine().invoke_callback(callback, arguments));
        } catch (...) {
            if (flags & create_use_cf_types) { for (const auto object : objects) release(object); release(paths); }
            for (const auto block : owned) process.release_guest(block);
            throw;
        }
        if (flags & create_use_cf_types) { for (const auto object : objects) release(object); release(paths); }
        for (const auto block : owned) process.release_guest(block);
    };

    // The run loop source's perform(info = the stream).
    const auto perform = process.install_function("_alchemy_fsevents_perform", [deliver](CpuState& s, GuestMemory&) {
        deliver(s.x[0]);
        s.x[0] = 0;
    }, "alchemy.internal");

    // Asks for a delivery (a guest thread): on the queue, or by signalling the source.
    const auto request = [this, state_of, deliver](Address stream) {
        const auto state = state_of(stream);
        Address queue{}, source{};
        std::vector<Address> loops;
        {
            std::lock_guard lock(state->mutex);
            if (state->delivery_requested || state->pending.empty() || state->invalidated) return;
            state->delivery_requested = true;
            queue = state->queue;
            source = state->source;
            for (const auto& [loop, mode] : state->schedules) loops.push_back(loop);
        }
        if (queue) {
            static_cast<void>(retain(stream));
            bridge->dispatch_async(queue, [this, deliver, stream] {
                try { deliver(stream); } catch (...) { release(stream); throw; }
                release(stream);
            });
        } else if (source) {
            static_cast<void>(call(core_foundation, "_CFRunLoopSourceSignal", {source}));
            for (const auto loop : loops) static_cast<void>(call(core_foundation, "_CFRunLoopWakeUp", {loop}));
        }
    };

    // The watcher of one start (a guest dispatch worker).
    const auto watch = [this, state_of, request](Address stream, std::uint64_t generation) {
        const auto state = state_of(stream);
        std::vector<std::string> paths;
        std::uint32_t flags{};
        double latency{};
        {
            std::lock_guard lock(state->mutex);
            paths = state->paths;
            flags = state->flags;
            latency = state->latency;
        }
        struct Watch { std::string root, host_root; std::unique_ptr<host::files::TreeWatch> watch; };
        std::vector<Watch> watches;
        for (const auto& path : paths) {
            const auto host_root = path_utf8(process.map_guest_path(path));
            if (auto opened = host::files::watch_tree(host_root)) watches.push_back({path, host_root, std::move(*opened)});
        }
        const auto add = [&](std::string path, std::uint32_t event_flags) {
            std::lock_guard lock(state->mutex);
            for (const auto& exclusion : state->exclusions) if (under(trimmed(path), exclusion)) return;
            for (auto& event : state->pending)
                if (event.path == path) { event.flags |= event_flags; return; }
            state->pending.push_back({std::move(path), event_flags, ++last_event_id});
        };
        auto first_pending = std::chrono::steady_clock::time_point::max();
        for (;;) {
            if (guest_worker_must_stop(process)) throw GuestThreadStop{};
            {
                std::lock_guard lock(state->mutex);
                if (!state->started || state->invalidated || state->generation != generation) break;
            }
            bool any{};
            for (auto& entry : watches) {
                bool overflow{};
                const auto changes = entry.watch->read(watches.size() > 1 ? 20 : 100, overflow);
                if (!changes) continue;
                if (overflow) { add(entry.root, flag_must_scan_subdirs | flag_user_dropped); any = true; }
                for (const auto& change : *changes) {
                    any = true;
                    const auto full = entry.root + "/" + change.path;
                    if (!(flags & create_file_events)) {
                        add(full.substr(0, full.rfind('/') + 1), 0);
                        continue;
                    }
                    std::uint32_t event_flags{};
                    switch (change.kind) {
                    case host::files::TreeChange::Kind::created: event_flags = item_created; break;
                    case host::files::TreeChange::Kind::removed: event_flags = item_removed; break;
                    case host::files::TreeChange::Kind::modified: event_flags = item_modified; break;
                    default: event_flags = item_renamed; break;
                    }
                    std::error_code error;
                    const auto host_item = utf8_path(entry.host_root + "/" + change.path);
                    event_flags |= std::filesystem::is_directory(host_item, error) ? item_is_dir : item_is_file;
                    add(full, event_flags);
                }
            }
            if (watches.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const auto now = std::chrono::steady_clock::now();
            if (any && first_pending == std::chrono::steady_clock::time_point::max()) first_pending = now;
            if (first_pending != std::chrono::steady_clock::time_point::max() &&
                now - first_pending >= std::chrono::duration<double>(latency)) {
                first_pending = std::chrono::steady_clock::time_point::max();
                request(stream);
            }
        }
        for (auto& entry : watches) entry.watch->cancel();
        release(stream);   // the start's reference
    };

    const auto stop = [state_of](Address stream) {
        const auto state = state_of(stream);
        std::lock_guard lock(state->mutex);
        if (!state->started) return;
        state->started = false;
        ++state->generation;
    };
    const auto unschedule_all = [this, state_of](Address stream) {
        const auto state = state_of(stream);
        std::vector<std::pair<Address, Address>> schedules;
        Address source{}, queue{};
        {
            std::lock_guard lock(state->mutex);
            schedules.swap(state->schedules);
            source = std::exchange(state->source, 0);
            queue = std::exchange(state->queue, 0);
        }
        for (const auto& [loop, mode] : schedules) {
            if (source) static_cast<void>(call(core_foundation, "_CFRunLoopRemoveSource", {loop, source, mode}));
            release(loop);
            release(mode);
        }
        if (source) {
            static_cast<void>(call(core_foundation, "_CFRunLoopSourceInvalidate", {source}));
            release(source);
        }
        if (queue) static_cast<void>(call(fill::libsystem, "_dispatch_release", {queue}));
    };

    // FSEventStreamRef FSEventStreamCreate(alloc, callback, FSEventStreamContext*, CFArrayRef paths,
    //                                      FSEventStreamEventId sinceWhen, CFTimeInterval latency (d0), flags)
    raw(core_services, "_FSEventStreamCreate", [this, stream_class](CpuState& s, GuestMemory& m) {
        if (!s.x[1] || !s.x[3]) { s.x[0] = 0; return; }
        auto state = std::make_shared<Stream>();
        state->callback = s.x[1];
        state->latency = std::max(0.0, std::bit_cast<double>(s.v[0][0]));
        state->flags = static_cast<std::uint32_t>(s.x[5]);
        for (const auto path : cf.array_values(s.x[3])) state->paths.push_back(trimmed(cf.utf8(path)));
        if (state->paths.empty()) { s.x[0] = 0; return; }
        // FSEventStreamContext {CFIndex version; void* info; retain; release; copyDescription}.
        if (s.x[2]) {
            state->info = m.read64(s.x[2] + 8);
            const auto retain_info = m.read64(s.x[2] + 16);
            state->info_release = m.read64(s.x[2] + 24);
            if (retain_info) state->info = process.calling_engine().invoke_callback(retain_info, std::array<Address, 1>{state->info});
        }
        const auto object = objc.allocate(stream_class);
        static_cast<void>(objc.host_state<Stream>(object, [&] { return state; }));
        s.x[0] = object;
    });
    raw(core_services, "_FSEventStreamCreateRelativeToDevice", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });   // not provided
    method(stream_class, "dealloc", [this, stream_class, unschedule_all](CpuState& s, GuestMemory&) {
        const auto object = s.x[0];
        if (const auto state = objc.find_host_state<Stream>(object)) {
            {
                std::lock_guard lock(state->mutex);
                state->invalidated = true;
                ++state->generation;
            }
            unschedule_all(object);
            Address info{}, info_release{};
            {
                std::lock_guard lock(state->mutex);
                info = std::exchange(state->info, 0);
                info_release = std::exchange(state->info_release, 0);
            }
            if (info_release) static_cast<void>(process.calling_engine().invoke_callback(info_release, std::array<Address, 1>{info}));
        }
        static_cast<void>(objc.send_super(object, stream_class, objc.selector("dealloc")));
    }, false, "v@:");
    raw(core_services, "_FSEventStreamRetain", [this](CpuState& s, GuestMemory&) { static_cast<void>(retain(s.x[0])); });
    raw(core_services, "_FSEventStreamRelease", [this](CpuState& s, GuestMemory&) { release(s.x[0]); });

    raw(core_services, "_FSEventStreamScheduleWithRunLoop", [this, state_of, perform](CpuState& s, GuestMemory& m) {
        const auto state = state_of(s.x[0]);
        if (!s.x[1] || !s.x[2]) return;
        Address source{};
        {
            std::lock_guard lock(state->mutex);
            source = state->source;
        }
        if (!source) {
            // CFRunLoopSourceContext (version 0): info = the stream (not retained), perform.
            const auto context = process.allocate_guest(80);
            const std::array<Address, 10> fields{0, s.x[0], 0, 0, 0, 0, 0, 0, 0, perform};
            for (std::size_t index = 0; index < fields.size(); ++index) m.write64(context + index * 8, fields[index]);
            try { source = call(core_foundation, "_CFRunLoopSourceCreate", {0, 0, context}); }
            catch (...) { process.release_guest(context); throw; }
            process.release_guest(context);
            std::lock_guard lock(state->mutex);
            state->source = source;
        }
        const auto loop = retain(s.x[1]), mode = retain(s.x[2]);
        {
            std::lock_guard lock(state->mutex);
            state->schedules.emplace_back(loop, mode);
        }
        static_cast<void>(call(core_foundation, "_CFRunLoopAddSource", {s.x[1], source, s.x[2]}));
    });
    raw(core_services, "_FSEventStreamUnscheduleFromRunLoop", [this, state_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        std::vector<std::pair<Address, Address>> schedules;
        Address source{};
        {
            std::lock_guard lock(state->mutex);
            source = state->source;
            schedules = state->schedules;
        }
        std::pair<Address, Address> removed{};
        for (const auto& entry : schedules)
            if (entry.first == s.x[1] && call(core_foundation, "_CFEqual", {entry.second, s.x[2]})) { removed = entry; break; }
        if (!removed.first) return;
        {
            std::lock_guard lock(state->mutex);
            std::erase(state->schedules, removed);
        }
        if (source) static_cast<void>(call(core_foundation, "_CFRunLoopRemoveSource", {removed.first, source, removed.second}));
        release(removed.first);
        release(removed.second);
    });
    raw(core_services, "_FSEventStreamSetDispatchQueue", [this, state_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        if (s.x[1]) static_cast<void>(call(fill::libsystem, "_dispatch_retain", {s.x[1]}));
        Address previous{};
        {
            std::lock_guard lock(state->mutex);
            previous = std::exchange(state->queue, s.x[1]);
        }
        if (previous) static_cast<void>(call(fill::libsystem, "_dispatch_release", {previous}));
    });
    // Boolean FSEventStreamStart(stream): false unless scheduled on a run loop or queue.
    raw(core_services, "_FSEventStreamStart", [this, state_of, watch](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        std::uint64_t generation{};
        {
            std::lock_guard lock(state->mutex);
            if (state->invalidated || (!state->queue && state->schedules.empty())) { s.x[0] = 0; return; }
            if (state->started) { s.x[0] = 1; return; }
            state->started = true;
            generation = ++state->generation;
        }
        const auto stream = s.x[0];
        static_cast<void>(retain(stream));   // released by the watcher when it ends
        const auto worker = call(fill::libsystem, "_dispatch_get_global_queue", {0, 0});
        bridge->dispatch_async(worker, [watch, stream, generation] { watch(stream, generation); });
        s.x[0] = 1;
    });
    raw(core_services, "_FSEventStreamStop", [stop](CpuState& s, GuestMemory&) { stop(s.x[0]); });
    raw(core_services, "_FSEventStreamInvalidate", [state_of, stop, unschedule_all](CpuState& s, GuestMemory&) {
        stop(s.x[0]);
        {
            const auto state = state_of(s.x[0]);
            std::lock_guard lock(state->mutex);
            state->invalidated = true;
            state->pending.clear();
        }
        unschedule_all(s.x[0]);
    });
    // Flushing: FlushAsync asks for delivery and returns the latest event id
    // queued; FlushSync delivers the pending events now, on the calling thread.
    raw(core_services, "_FSEventStreamFlushAsync", [state_of, request](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        std::uint64_t latest{};
        {
            std::lock_guard lock(state->mutex);
            latest = state->pending.empty() ? state->latest : state->pending.back().id;
        }
        request(s.x[0]);
        s.x[0] = latest;
    });
    raw(core_services, "_FSEventStreamFlushSync", [deliver](CpuState& s, GuestMemory&) { deliver(s.x[0]); });
    raw(core_services, "_FSEventStreamGetLatestEventId", [state_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = state->latest;
    });
    raw(core_services, "_FSEventsGetCurrentEventId", [](CpuState& s, GuestMemory&) { s.x[0] = last_event_id.load(); });
    raw(core_services, "_FSEventStreamCopyPathsBeingWatched", [this, state_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        std::vector<std::string> paths;
        {
            std::lock_guard lock(state->mutex);
            paths = state->paths;
        }
        std::vector<Address> strings;
        for (const auto& path : paths) strings.push_back(cf.string(path));
        s.x[0] = cf.array(strings);
        for (const auto string : strings) release(string);
    });
    // Boolean FSEventStreamSetExclusionPaths(stream, CFArrayRef): at most 8 paths.
    raw(core_services, "_FSEventStreamSetExclusionPaths", [this, state_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        std::vector<std::string> paths;
        if (s.x[1]) for (const auto path : cf.array_values(s.x[1])) paths.push_back(trimmed(cf.utf8(path)));
        if (paths.size() > 8) { s.x[0] = 0; return; }
        std::lock_guard lock(state->mutex);
        state->exclusions = std::move(paths);
        s.x[0] = 1;
    });
}

} // namespace alchemy::gui::values
