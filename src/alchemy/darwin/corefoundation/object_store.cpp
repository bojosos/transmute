#include "alchemy/object_store.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/perf.hpp"
#include "alchemy/process_context.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace alchemy {
ObjectStore::ObjectStore(ProcessContext& process) : process_(process) {}
ObjectStore::~ObjectStore() = default;

ObjectStore::Shard& ObjectStore::shard(std::uint64_t address) const noexcept {
    // Objects are at least 16-byte aligned; mix the remaining bits (Fibonacci hashing).
    return shards_[static_cast<std::size_t>(((address >> 4) * 0x9e3779b97f4a7c15ULL) >> 58)];
}

void ObjectStore::release_memory(std::uint64_t address, bool heap) {
    if (heap) engine_heap_release(process_.main_engine(), address);
    else process_.release_guest(address);
}

std::uint64_t ObjectStore::create(std::uint64_t size, std::uint64_t class_address,
    ObjectKind kind, std::shared_ptr<ObjectPayload> payload,
    std::function<void(std::uint64_t)> destroy, bool immortal) {
    size = std::max<std::uint64_t>(size, 8);
    // Engine heap blocks are zero-filled like a fresh guest mapping, without
    // one mapping (and one guard gap) per object.
    const auto heap = engine_heap_allocate(process_.main_engine(), size, 16);
    const auto address = heap ? *heap : process_.allocate_guest(size);
    try {
        process_.main_engine().write64(address, class_address);
        auto record = std::make_shared<ObjectRecord>();
        record->address = address;
        record->size = size;
        record->class_address = class_address;
        record->kind = kind;
        record->payload = std::move(payload);
        record->destroy = std::move(destroy);
        record->immortal = immortal;
        record->heap_memory = heap.has_value();
        auto& part = shard(address);
        std::lock_guard lock(part.mutex);
        part.objects.emplace(address, std::move(record));
    } catch (...) { release_memory(address, heap.has_value()); throw; }
    perf::add(perf::objc_alloc);
    return address;
}

void ObjectStore::adopt(std::uint64_t address, std::uint64_t size, std::uint64_t class_address,
    ObjectKind kind, std::shared_ptr<ObjectPayload> payload, bool immortal) {
    if (address == 0 || size < 8) throw std::runtime_error("invalid adopted compatibility object");
    auto record = std::make_shared<ObjectRecord>();
    record->address = address;
    record->size = size;
    record->class_address = class_address;
    record->kind = kind;
    record->payload = std::move(payload);
    record->immortal = immortal;
    record->owned_memory = false;
    auto& part = shard(address);
    std::lock_guard lock(part.mutex);
    const auto [position, inserted] = part.objects.try_emplace(address, record);
    if (!inserted && (position->second->kind != kind || position->second->class_address != class_address))
        throw std::runtime_error("conflicting adopted compatibility object");
}

std::shared_ptr<ObjectRecord> ObjectStore::find(std::uint64_t address) const {
    perf::add(perf::objc_store_lookups);
    const auto& part = shard(address);
    std::lock_guard lock(part.mutex);
    const auto found = part.objects.find(address);
    return found == part.objects.end() ? nullptr : found->second;
}

std::shared_ptr<ObjectRecord> ObjectStore::require(std::uint64_t address) const {
    auto record = find(address);
    if (!record) throw std::runtime_error("invalid or destroyed compatibility object");
    return record;
}

std::optional<ObjectStore::View> ObjectStore::peek(std::uint64_t address) const {
    perf::add(perf::objc_store_lookups);
    const auto& part = shard(address);
    std::lock_guard lock(part.mutex);
    const auto found = part.objects.find(address);
    if (found == part.objects.end()) return std::nullopt;
    const auto& record = *found->second;
    return View{record.class_address.load(std::memory_order_relaxed), record.kind, record.immortal, record.destroying.load()};
}

void ObjectStore::adopt_forwarding(std::uint64_t address, std::uint64_t size, std::uint64_t class_address,
    std::function<void(std::uint64_t)> retain, std::function<bool(std::uint64_t)> release) {
    if (address == 0 || size < 8 || !retain || !release) throw std::runtime_error("invalid forwarding compatibility object");
    auto record = std::make_shared<ObjectRecord>();
    record->address = address;
    record->size = size;
    record->class_address = class_address;
    record->owned_memory = false;
    record->forward_retain = std::move(retain);
    record->forward_release = std::move(release);
    auto& part = shard(address);
    std::lock_guard lock(part.mutex);
    part.objects.insert_or_assign(address, std::move(record));
}

std::uint64_t ObjectStore::retain(std::uint64_t address) {
    if (address == 0) return 0;
    perf::add(perf::objc_store_lookups);
    auto& part = shard(address);
    std::unique_lock lock(part.mutex);
    const auto found = part.objects.find(address);
    if (found == part.objects.end()) throw std::runtime_error("retain of an invalid compatibility object");
    auto& record = *found->second;
    if (record.immortal) return address;
    if (record.forward_retain) {
        const auto hook = record.forward_retain;
        lock.unlock();
        hook(address);
        return address;
    }
    if (record.destroying || record.references == 0) throw std::runtime_error("retain during object destruction");
    if (record.references == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("object retain count overflow");
    ++record.references;
    return address;
}

bool ObjectStore::try_retain(std::uint64_t address) {
    if (address == 0) return false;
    auto& part = shard(address);
    std::lock_guard lock(part.mutex);
    const auto found = part.objects.find(address);
    if (found == part.objects.end()) return false;
    auto& record = *found->second;
    if (record.immortal) return true;
    if (record.forward_retain) return false;   // no weak loads of forwarded objects
    if (record.destroying || record.references == 0) return false;
    if (record.references == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("object retain count overflow");
    ++record.references;
    return true;
}

void ObjectStore::finish(const std::shared_ptr<ObjectRecord>& record) {
    bool removed = false;
    {
        auto& part = shard(record->address);
        std::lock_guard lock(part.mutex);
        const auto found = part.objects.find(record->address);
        if (found != part.objects.end() && found->second == record) {
            part.objects.erase(found);
            removed = true;
        }
    }
    if (!removed) return;
    perf::add(perf::objc_dealloc);
    if (record->weakly_referenced && weak_clearer_) weak_clearer_(record->address);
    if (!record->owned_memory) return;
    // A message to the freed object misses the dispatch cache (isa 0) and
    // fails explicitly in the slow path instead of running a stale method.
    try { process_.main_engine().write64(record->address, 0); } catch (const std::exception&) {}
    release_memory(record->address, record->heap_memory);
}

void ObjectStore::release(std::uint64_t address) {
    if (address == 0) return;
    perf::add(perf::objc_store_lookups);
    std::shared_ptr<ObjectRecord> record;
    {
        auto& part = shard(address);
        std::lock_guard lock(part.mutex);
        const auto found = part.objects.find(address);
        if (found == part.objects.end()) throw std::runtime_error("release of an invalid compatibility object");
        record = found->second;
        if (record->immortal) return;
        if (!record->forward_release) {
            if (record->destroying || record->references == 0) throw std::runtime_error("release during object destruction");
            if (--record->references != 0) return;
            record->destroying = true;
        }
    }
    if (record->forward_release) {
        if (record->forward_release(address)) finish(record);
        return;
    }
    try { if (record->destroy) record->destroy(address); }
    catch (...) { finish(record); throw; }
    if (record->destructed && !record->disposed) {
        // The dealloc destructed the object and kept its memory (objc_destructInstance without
        // object_dispose): the address stays taken until object_dispose.
        record->destroying = false;
        return;
    }
    finish(record);
}

bool ObjectStore::destructed(std::uint64_t address) const {
    if (address == 0) return false;
    auto& part = shard(address);
    std::lock_guard lock(part.mutex);
    const auto found = part.objects.find(address);
    return found != part.objects.end() && found->second->destructed && !found->second->destroying;
}

void ObjectStore::note_destructed(std::uint64_t address) {
    if (address == 0) return;
    auto& part = shard(address);
    std::lock_guard lock(part.mutex);
    const auto found = part.objects.find(address);
    if (found != part.objects.end() && found->second->destroying) found->second->destructed = true;
}

void ObjectStore::dispose(std::uint64_t address) {
    if (address == 0) return;
    std::shared_ptr<ObjectRecord> record;
    {
        auto& part = shard(address);
        std::lock_guard lock(part.mutex);
        const auto found = part.objects.find(address);
        if (found == part.objects.end()) throw std::runtime_error("dispose of an invalid compatibility object");
        record = found->second;
        if (record->immortal) return;
        if (record->destroying) { record->disposed = true; return; }   // inside its dealloc: freed after it
        record->references = 0;
        record->destroying = true;
    }
    finish(record);
}

void ObjectStore::set_class(ObjectKind kind, std::uint64_t class_address) {
    for (auto& part : shards_) {
        std::lock_guard lock(part.mutex);
        for (const auto& [address, record] : part.objects) {
            if (record->kind != kind) continue;
            record->class_address = class_address;
            // Compiler constants can be in read-only Mach-O storage.
            if (record->owned_memory) process_.main_engine().write64(address, class_address);
        }
    }
}

bool ObjectStore::mark_weakly_referenced(std::uint64_t address) {
    auto& part = shard(address);
    std::lock_guard lock(part.mutex);
    const auto found = part.objects.find(address);
    if (found == part.objects.end()) return false;
    auto& record = *found->second;
    if (record.destroying || (!record.immortal && record.references == 0)) return false;
    record.weakly_referenced = true;
    return true;
}

void ObjectStore::set_weak_clearer(std::function<void(std::uint64_t)> clearer) { weak_clearer_ = std::move(clearer); }

std::size_t ObjectStore::live_count(bool include_immortal) const {
    std::size_t count = 0;
    for (const auto& part : shards_) {
        std::lock_guard lock(part.mutex);
        count += static_cast<std::size_t>(std::count_if(part.objects.begin(), part.objects.end(),
            [include_immortal](const auto& item) { return include_immortal || !item.second->immortal; }));
    }
    return count;
}
} // namespace alchemy
