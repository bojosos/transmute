#pragma once

#include "alchemy/hash_map.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace alchemy {
class ProcessContext;

enum class ObjectKind { instance, string, data, number, boolean, array, dictionary, url, locale, date, url_enumerator };
struct ObjectPayload { virtual ~ObjectPayload() = default; };
struct ObjectRecord {
    std::uint64_t address{}, size{};
    std::atomic<std::uint64_t> class_address{0}, references{1};
    ObjectKind kind{ObjectKind::instance};
    std::shared_ptr<ObjectPayload> payload;
    std::function<void(std::uint64_t)> destroy;
    bool immortal{}, owned_memory{true};
    bool heap_memory{}; // owned memory came from the engine heap (engine_heap_allocate)
    std::atomic<bool> destroying{false};
    std::atomic<bool> weakly_referenced{false}; // an Objective-C weak reference names this object
    // The dealloc destructed the object (objc_destructInstance) and nothing disposed it
    // (object_dispose, or the root -[NSObject dealloc], which disposes): the guest keeps the
    // memory (Chrome's Objective-C zombies), so it is not freed then. A kept record stays
    // `destructed` until object_dispose frees it.
    std::atomic<bool> destructed{false}, disposed{false};
    // Forwarding records (adopt_forwarding): objects whose reference count
    // lives elsewhere (heap Blocks). retain/release run these instead of the
    // record's count; forward_release returns true when the object is gone.
    std::function<void(std::uint64_t)> forward_retain;
    std::function<bool(std::uint64_t)> forward_release;
};

// Guest pointers identify objects. Payloads and ownership never escape to guest code.
// Destruction runs outside the store lock and may invoke guest code recursively.
// The table is sharded by address: lookups of different objects rarely share a lock.
class ObjectStore {
public:
    explicit ObjectStore(ProcessContext& process);
    ~ObjectStore();
    // Owned object memory comes from the engine's guest heap when it has one
    // (fx), else from a mapped guest region; it is zero-filled with the isa
    // at offset 0.
    [[nodiscard]] std::uint64_t create(std::uint64_t size, std::uint64_t class_address,
        ObjectKind kind = ObjectKind::instance, std::shared_ptr<ObjectPayload> payload = {},
        std::function<void(std::uint64_t)> destroy = {}, bool immortal = false);
    void adopt(std::uint64_t address, std::uint64_t size, std::uint64_t class_address,
        ObjectKind kind, std::shared_ptr<ObjectPayload> payload, bool immortal = true);
    // An object whose reference count another runtime keeps (a heap Block):
    // retain/release forward to the hooks, and the record goes away when
    // `release` reports the object gone.
    void adopt_forwarding(std::uint64_t address, std::uint64_t size, std::uint64_t class_address,
        std::function<void(std::uint64_t)> retain, std::function<bool(std::uint64_t)> release);
    [[nodiscard]] std::shared_ptr<ObjectRecord> find(std::uint64_t address) const;
    [[nodiscard]] std::shared_ptr<ObjectRecord> require(std::uint64_t address) const;
    // The registered class and kind of a live object without taking shared
    // ownership of its record (the Objective-C dispatch path).
    struct View { std::uint64_t class_address{}; ObjectKind kind{}; bool immortal{}, destroying{}; };
    [[nodiscard]] std::optional<View> peek(std::uint64_t address) const;
    [[nodiscard]] std::uint64_t retain(std::uint64_t address);
    // retain unless the object is unknown or being destroyed (weak loads);
    // returns whether a reference was taken (always true for immortal objects).
    [[nodiscard]] bool try_retain(std::uint64_t address);
    void release(std::uint64_t address);
    void dispose(std::uint64_t address);
    // objc_destructInstance on an object being destroyed. When its dealloc does that and returns
    // without object_dispose (or the root -[NSObject dealloc], which disposes), the record and
    // its memory stay (references 0) until object_dispose frees them: whoever destructed it owns
    // the memory, as with Apple's runtime, and no new object takes its address. Host deallocs
    // that end an object without [super dealloc] call object_dispose, as Apple's runtime frees.
    void note_destructed(std::uint64_t address);
    // Whether a live record was destructed and kept (object_dispose then only frees it: its isa
    // may already name another class, as a zombie's does).
    [[nodiscard]] bool destructed(std::uint64_t address) const;
    void set_class(ObjectKind kind, std::uint64_t class_address);
    [[nodiscard]] std::size_t live_count(bool include_immortal = false) const;
    // Objective-C weak references (objc_storeWeak): marks a live object, false
    // when it is unknown or being destroyed. When a marked object's record is
    // removed, the clearer runs once with its address (outside the store
    // locks, before the memory is released). Set by the Objective-C runtime
    // while no guest thread runs; an empty function removes it.
    [[nodiscard]] bool mark_weakly_referenced(std::uint64_t address);
    void set_weak_clearer(std::function<void(std::uint64_t)> clearer);
private:
    struct Shard {
        mutable std::mutex mutex;
        // Records are shared_ptr values: nothing keeps a reference into the
        // table across an insert or erase (open addressing moves elements).
        hash_map<std::uint64_t, std::shared_ptr<ObjectRecord>, address_hash> objects;
    };
    static constexpr std::size_t shard_count = 64;
    [[nodiscard]] Shard& shard(std::uint64_t address) const noexcept;
    void finish(const std::shared_ptr<ObjectRecord>& record);
    void release_memory(std::uint64_t address, bool heap);
    ProcessContext& process_;
    mutable std::array<Shard, shard_count> shards_;
    std::function<void(std::uint64_t)> weak_clearer_;
};
} // namespace alchemy
