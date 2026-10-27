#include "alchemy/darwin/network/network_internal.hpp"
#include "alchemy/darwin/network/cookies.hpp"

#include <cmath>
#include <limits>
#include <map>
#include <mutex>

// NSURLCache and NSCachedURLResponse. A cache keeps NSCachedURLResponses by
// request URL (without its fragment) in this process, most recently used
// last; its budget is memoryCapacity + diskCapacity, and storing beyond it
// evicts the least recently used entries. Nothing is written to disk: the
// disk tier lives in memory for the process's lifetime (currentDiskUsage is
// the part beyond memoryCapacity), so a new process starts with an empty
// cache. As in Foundation, a response larger than 5 % of the budget is not
// stored, nor one whose storagePolicy is NSURLCacheStorageNotAllowed.
//   NSCachedURLResponse: -initWithResponse:data:,
//   -initWithResponse:data:userInfo:storagePolicy:, -response, -data,
//   -userInfo, -storagePolicy.
//   NSURLCache: +sharedURLCache / +setSharedURLCache: (4 MB memory, 20 MB
//   disk), -initWithMemoryCapacity:diskCapacity:diskPath: / directoryURL:,
//   the capacities and usages, -cachedResponseForRequest:,
//   -storeCachedResponse:forRequest:, -removeCachedResponseForRequest:,
//   -removeAllCachedResponses, -removeCachedResponsesSinceDate:, and the data
//   task variants (through the task's current request).
// The URL loading system (loading.cpp) reads and fills a session's cache with
// -cachedResponseForRequest: and -storeCachedResponse:forRequest:, so guest
// subclasses that override those see every lookup and store.
namespace alchemy::gui::network {
namespace {

constexpr double reference_to_unix = 978307200.0;

struct CachedState final : ObjectPayload {
    Address response{}, data{}, user_info{};   // +1
    std::uint64_t policy{};                    // NSURLCacheStoragePolicy
    double stored{};                           // Unix seconds at creation
};

struct CacheState final : ObjectPayload {
    struct Entry {
        Address cached{};                      // +1 NSCachedURLResponse
        std::size_t size{};
        std::uint64_t used{};
        double stored{};
    };
    std::mutex mutex;
    std::uint64_t memory{}, disk{}, clock{};
    std::size_t usage{};
    std::map<std::string, Entry> entries;
};

} // namespace

double cached_response_time(Bridge& b, Address cached) {
    const auto state = cached ? b.objc.find_host_state<CachedState>(cached) : nullptr;
    return state ? state->stored : std::numeric_limits<double>::quiet_NaN();
}

void install_url_cache(const std::shared_ptr<Bridge>& bridge) {
    const auto b = bridge;
    const auto object = b->objc.find_class("NSObject");
    const auto cached_class = b->define_class("NSCachedURLResponse", object, foundation);
    const auto cache_class = b->define_class("NSURLCache", object, foundation);

    // ---- NSCachedURLResponse ----------------------------------------------------------------------------
    const auto cached_state = [b](Address value) {
        const auto state = b->objc.find_host_state<CachedState>(value);
        if (!state) security::fail("NSCachedURLResponse used before -initWithResponse:data:");
        return state;
    };
    const auto init_cached = [b](Address self, Address response, Address data, Address user_info, std::uint64_t policy) {
        if (!response) security::fail("NSInvalidArgumentException: -[NSCachedURLResponse initWithResponse:data:] needs a response");
        if (policy > 2) security::fail("invalid NSURLCacheStoragePolicy " + std::to_string(policy));
        auto state = std::make_shared<CachedState>();
        state->response = b->retain(response);
        state->data = data ? b->objc.send(data, "copy").x0 : b->data({});
        state->user_info = user_info ? b->objc.send(user_info, "copy").x0 : 0;
        state->policy = policy;
        state->stored = unix_now();
        static_cast<void>(b->objc.host_state<CachedState>(self, [&] { return state; }));
    };
    b->method(cached_class, "initWithResponse:data:", [init_cached](CpuState& s, GuestMemory&) {
        init_cached(s.x[0], s.x[2], s.x[3], 0, 0);
    }, false, "@@:@@");
    b->method(cached_class, "initWithResponse:data:userInfo:storagePolicy:", [init_cached](CpuState& s, GuestMemory&) {
        init_cached(s.x[0], s.x[2], s.x[3], s.x[4], s.x[5]);
    }, false, "@@:@@@Q");
    b->method(cached_class, "response", [cached_state](CpuState& s, GuestMemory&) { s.x[0] = cached_state(s.x[0])->response; });
    b->method(cached_class, "data", [cached_state](CpuState& s, GuestMemory&) { s.x[0] = cached_state(s.x[0])->data; });
    b->method(cached_class, "userInfo", [cached_state](CpuState& s, GuestMemory&) { s.x[0] = cached_state(s.x[0])->user_info; });
    b->method(cached_class, "storagePolicy", [cached_state](CpuState& s, GuestMemory&) { s.x[0] = cached_state(s.x[0])->policy; }, false, "Q@:");
    b->method(cached_class, "copyWithZone:", [b](CpuState& s, GuestMemory&) { s.x[0] = b->retain(s.x[0]); }, false, "@@:^v");
    b->method(cached_class, "dealloc", [b, cached_class](CpuState& s, GuestMemory&) {
        if (const auto state = b->objc.find_host_state<CachedState>(s.x[0]))
            b->release_all({std::exchange(state->response, 0), std::exchange(state->data, 0), std::exchange(state->user_info, 0)});
        static_cast<void>(b->objc.send_super(s.x[0], cached_class, b->objc.selector("dealloc")));
    }, false, "v@:");

    // ---- NSURLCache -----------------------------------------------------------------------------------------
    const auto cache_state = [b](Address value) {
        const auto state = b->objc.find_host_state<CacheState>(value);
        if (!state) security::fail("NSURLCache used before -init");
        return state;
    };
    const auto init_cache = [b](Address self, std::uint64_t memory, std::uint64_t disk) {
        static_cast<void>(b->objc.host_state<CacheState>(self, [&] {
            auto state = std::make_shared<CacheState>();
            state->memory = memory;
            state->disk = disk;
            return state;
        }));
    };
    // The cache key of an NSURLRequest: its URL's absolute string without the fragment (nullopt without a URL).
    const auto key_of = [b](Address request) -> std::optional<std::string> {
        const auto url = request ? b->objc.send(request, "URL").x0 : 0;
        const auto text = url ? b->maybe_text(b->objc.send(url, "absoluteString").x0) : std::nullopt;
        if (!text) return std::nullopt;
        return text->substr(0, text->find('#'));
    };
    // Evicts least recently used entries until `usage` fits `budget`; returns the released objects.
    const auto evict = [](CacheState& state, std::uint64_t budget) {
        std::vector<Address> released;
        while (state.usage > budget && !state.entries.empty()) {
            auto oldest = state.entries.begin();
            for (auto it = state.entries.begin(); it != state.entries.end(); ++it) if (it->second.used < oldest->second.used) oldest = it;
            state.usage -= oldest->second.size;
            released.push_back(oldest->second.cached);
            state.entries.erase(oldest);
        }
        return released;
    };
    const auto lookup = [b, cache_state, key_of](Address cache, Address request) -> Address {
        const auto key = key_of(request);
        if (!key) return 0;
        const auto state = cache_state(cache);
        std::lock_guard lock(state->mutex);
        const auto found = state->entries.find(*key);
        if (found == state->entries.end()) return 0;
        found->second.used = ++state->clock;
        return b->autorelease(b->retain(found->second.cached));
    };
    const auto store = [b, cache_state, key_of, evict](Address cache, Address cached, Address request) {
        const auto key = key_of(request);
        const auto value = cached ? b->objc.find_host_state<CachedState>(cached) : nullptr;
        if (!key || !value || value->policy == 2) return;   // NSURLCacheStorageNotAllowed
        const auto data = value->data;
        const auto size = static_cast<std::size_t>(b->cf.length(data)) + 512;   // the body plus an allowance for the response
        const auto state = cache_state(cache);
        std::vector<Address> released;
        {
            std::lock_guard lock(state->mutex);
            const auto budget = state->memory + state->disk;
            if (budget == 0 || size > budget / 20) return;   // larger than 5 % of the cache: not stored
            if (const auto previous = state->entries.find(*key); previous != state->entries.end()) {
                state->usage -= previous->second.size;
                released.push_back(previous->second.cached);
                state->entries.erase(previous);
            }
            state->entries[*key] = {b->retain(cached), size, ++state->clock, value->stored};
            state->usage += size;
            auto evicted = evict(*state, budget);
            released.insert(released.end(), evicted.begin(), evicted.end());
        }
        b->release_all(released);
    };
    const auto remove = [b, cache_state, key_of](Address cache, Address request) {
        const auto key = key_of(request);
        if (!key) return;
        const auto state = cache_state(cache);
        Address released{};
        {
            std::lock_guard lock(state->mutex);
            const auto found = state->entries.find(*key);
            if (found == state->entries.end()) return;
            state->usage -= found->second.size;
            released = found->second.cached;
            state->entries.erase(found);
        }
        b->release(released);
    };
    const auto remove_where = [b, cache_state](Address cache, auto predicate) {
        const auto state = cache_state(cache);
        std::vector<Address> released;
        {
            std::lock_guard lock(state->mutex);
            for (auto it = state->entries.begin(); it != state->entries.end();) {
                if (predicate(it->second)) {
                    state->usage -= it->second.size;
                    released.push_back(it->second.cached);
                    it = state->entries.erase(it);
                } else {
                    ++it;
                }
            }
        }
        b->release_all(released);
    };

    static std::mutex shared_mutex;
    static Address shared{};   // +1
    b->method(cache_class, "sharedURLCache", [b, cache_class, init_cache](CpuState& s, GuestMemory&) {
        std::lock_guard lock(shared_mutex);
        if (!shared) {
            shared = b->objc.allocate(cache_class);
            init_cache(shared, 4 * 1024 * 1024, 20 * 1024 * 1024);
        }
        s.x[0] = shared;
    }, true);
    b->method(cache_class, "setSharedURLCache:", [b](CpuState& s, GuestMemory&) {
        if (!s.x[2]) security::fail("NSInvalidArgumentException: +[NSURLCache setSharedURLCache:] needs a cache");
        const auto cache = b->retain(s.x[2]);
        Address previous{};
        { std::lock_guard lock(shared_mutex); previous = std::exchange(shared, cache); }
        b->release(previous);
    }, true, "v@:@");
    b->method(cache_class, "init", [init_cache](CpuState& s, GuestMemory&) { init_cache(s.x[0], 0, 0); });
    for (const auto* name : {"initWithMemoryCapacity:diskCapacity:diskPath:", "initWithMemoryCapacity:diskCapacity:directoryURL:"})
        b->method(cache_class, name, [init_cache](CpuState& s, GuestMemory&) { init_cache(s.x[0], s.x[2], s.x[3]); }, false, "@@:QQ@");
    b->method(cache_class, "dealloc", [b, cache_class](CpuState& s, GuestMemory&) {
        if (const auto state = b->objc.find_host_state<CacheState>(s.x[0])) {
            std::vector<Address> released;
            { std::lock_guard lock(state->mutex); for (auto& [key, entry] : state->entries) released.push_back(entry.cached); state->entries.clear(); }
            b->release_all(released);
        }
        static_cast<void>(b->objc.send_super(s.x[0], cache_class, b->objc.selector("dealloc")));
    }, false, "v@:");
    b->method(cache_class, "memoryCapacity", [cache_state](CpuState& s, GuestMemory&) {
        const auto state = cache_state(s.x[0]); std::lock_guard lock(state->mutex); s.x[0] = state->memory;
    }, false, "Q@:");
    b->method(cache_class, "diskCapacity", [cache_state](CpuState& s, GuestMemory&) {
        const auto state = cache_state(s.x[0]); std::lock_guard lock(state->mutex); s.x[0] = state->disk;
    }, false, "Q@:");
    const auto set_capacity = [b, cache_state, evict](Address cache, std::uint64_t value, bool memory) {
        const auto state = cache_state(cache);
        std::vector<Address> released;
        {
            std::lock_guard lock(state->mutex);
            (memory ? state->memory : state->disk) = value;
            released = evict(*state, state->memory + state->disk);
        }
        b->release_all(released);
    };
    b->method(cache_class, "setMemoryCapacity:", [set_capacity](CpuState& s, GuestMemory&) { set_capacity(s.x[0], s.x[2], true); }, false, "v@:Q");
    b->method(cache_class, "setDiskCapacity:", [set_capacity](CpuState& s, GuestMemory&) { set_capacity(s.x[0], s.x[2], false); }, false, "v@:Q");
    b->method(cache_class, "currentMemoryUsage", [cache_state](CpuState& s, GuestMemory&) {
        const auto state = cache_state(s.x[0]); std::lock_guard lock(state->mutex);
        s.x[0] = std::min<std::uint64_t>(state->usage, state->memory);
    }, false, "Q@:");
    b->method(cache_class, "currentDiskUsage", [cache_state](CpuState& s, GuestMemory&) {
        const auto state = cache_state(s.x[0]); std::lock_guard lock(state->mutex);
        s.x[0] = state->usage > state->memory ? state->usage - state->memory : 0;
    }, false, "Q@:");
    b->method(cache_class, "cachedResponseForRequest:", [lookup](CpuState& s, GuestMemory&) { s.x[0] = lookup(s.x[0], s.x[2]); });
    b->method(cache_class, "storeCachedResponse:forRequest:", [store](CpuState& s, GuestMemory&) { store(s.x[0], s.x[2], s.x[3]); }, false, "v@:@@");
    b->method(cache_class, "removeCachedResponseForRequest:", [remove](CpuState& s, GuestMemory&) { remove(s.x[0], s.x[2]); }, false, "v@:@");
    b->method(cache_class, "removeAllCachedResponses", [remove_where](CpuState& s, GuestMemory&) {
        remove_where(s.x[0], [](const CacheState::Entry&) { return true; });
    }, false, "v@:");
    b->method(cache_class, "removeCachedResponsesSinceDate:", [b, remove_where](CpuState& s, GuestMemory&) {
        if (!s.x[2]) security::fail("NSInvalidArgumentException: -[NSURLCache removeCachedResponsesSinceDate:] needs a date");
        const auto since = b->cf.date_time(s.x[2]) + reference_to_unix;
        remove_where(s.x[0], [since](const CacheState::Entry& entry) { return entry.stored >= since; });
    }, false, "v@:@");
    // The data task variants go through the task's current request (and so through overrides of the request methods).
    const auto task_request = [b](Address task) { return task ? b->objc.send(task, "currentRequest").x0 : 0; };
    b->method(cache_class, "storeCachedResponse:forDataTask:", [b, task_request](CpuState& s, GuestMemory&) {
        CallRegs registers;
        registers.x[2] = s.x[2];
        registers.x[3] = task_request(s.x[3]);
        static_cast<void>(b->objc.send(s.x[0], "storeCachedResponse:forRequest:", registers));
    }, false, "v@:@@");
    b->method(cache_class, "getCachedResponseForDataTask:completionHandler:", [b, task_request](CpuState& s, GuestMemory&) {
        CallRegs registers;
        registers.x[2] = task_request(s.x[2]);
        const auto cached = b->objc.send(s.x[0], "cachedResponseForRequest:", registers).x0;
        if (s.x[3]) static_cast<void>(b->invoke_block(s.x[3], {cached}));
    }, false, "v@:@@?");
    b->method(cache_class, "removeCachedResponseForDataTask:", [b, task_request](CpuState& s, GuestMemory&) {
        CallRegs registers;
        registers.x[2] = task_request(s.x[2]);
        static_cast<void>(b->objc.send(s.x[0], "removeCachedResponseForRequest:", registers));
    }, false, "v@:@");
}

} // namespace alchemy::gui::network
