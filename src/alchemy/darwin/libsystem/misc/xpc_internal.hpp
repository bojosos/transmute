#pragma once

// XPC objects' internals (misc/xpc_objects.cpp), for the XPC connections of
// darwin/launchd/xpc_connections.cpp (agreed between fw-system and
// launchd-xpc, 2026-11-06): the value record, the process's XPC state and its
// helpers. Not a contract header: only these two files include it.

#include "alchemy/engine_adapter.hpp"
#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/darwin/libsystem/install.hpp"

#include "alchemy/object_store.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace alchemy::export_fill::xpc_detail {

enum class Type : unsigned {
    dictionary, array, string, int64, uint64, boolean, double_value, date, data, uuid, fd, null, error, connection, endpoint, shmem, mach_send, count
};
inline constexpr std::array<const char*, static_cast<std::size_t>(Type::count)> type_names{
    "dictionary", "array", "string", "int64", "uint64", "bool", "double", "date", "data", "uuid", "fd", "null", "error", "connection", "endpoint", "shmem", "mach_send"};

struct Value : ObjectPayload {
    Type type{};
    std::mutex mutex;
    std::vector<std::pair<std::string, std::uint64_t>> entries;   // dictionary and error: key -> retained value, in insertion order
    std::vector<std::uint64_t> items;                              // array: retained values
    std::string text;                                              // string, connection name
    std::uint64_t guest{};                                         // guest copy of the string, data or UUID bytes
    std::int64_t integer{};                                        // int64, uint64, bool, date, fd
    double real{};
    std::vector<std::uint8_t> bytes;                               // data, uuid
    // connection
    std::uint64_t handler{}, queue{}, context{}, finalizer{};
    bool resumed{}, cancelled{}, invalidated{};
    std::uint64_t connection{};                                    // endpoint: its connection (retained)
    std::uint64_t region{}, length{};                              // shmem: the guest region it names
};

struct Xpc {
    explicit Xpc(Provider& provider, ObjcRuntime& runtime) : p(provider), objc(runtime) {}
    Provider p;
    ObjcRuntime& objc;
    std::array<std::uint64_t, static_cast<std::size_t>(Type::count)> classes{}, names{};
    std::uint64_t root{}, bool_true{}, bool_false{}, connection_invalid{}, connection_interrupted{}, termination_imminent{};
    std::uint64_t deliver_entry{};
    std::mutex mutex;
    struct Delivery { std::uint64_t block{}, argument{}, connection{}; };
    std::map<std::uint64_t, Delivery> deliveries;
    std::uint64_t next_delivery{1};

    ProcessContext& process() { return p.process(); }
    GuestMemory& memory() { return process().calling_engine(); }
    std::shared_ptr<Value> value(std::uint64_t object) {
        if (!object) return nullptr;
        const auto record = process().objects().find(object);
        return record ? std::dynamic_pointer_cast<Value>(record->payload) : nullptr;
    }
    std::shared_ptr<Value> typed(std::uint64_t object, Type type) {
        const auto found = value(object);
        return found && found->type == type ? found : nullptr;
    }
    std::uint64_t retain(std::uint64_t object) { return object ? process().objects().retain(object) : 0; }
    void release(std::uint64_t object) { if (object) process().objects().release(object); }
    std::uint64_t guest_copy(std::span<const std::uint8_t> bytes, bool terminate) {
        const auto address = process().allocate_guest(bytes.size() + (terminate ? 1 : 0) + 1);
        if (!bytes.empty()) memory().write_bytes(address, bytes);
        if (terminate) memory().write8(address + bytes.size(), 0);
        return address;
    }
    // A new object of `type` (+1). Its destruction releases what it holds.
    std::uint64_t make(Type type, const std::function<void(Value&)>& fill = {}, bool immortal = false) {
        auto payload = std::make_shared<Value>();
        payload->type = type;
        if (fill) fill(*payload);
        if (type == Type::string || type == Type::data || type == Type::uuid) {
            const auto& source = type == Type::string ? std::vector<std::uint8_t>(payload->text.begin(), payload->text.end()) : payload->bytes;
            payload->guest = guest_copy(source, type == Type::string);
        }
        return process().objects().create(16, classes[static_cast<std::size_t>(type)], ObjectKind::instance, payload,
            [this, held = payload](std::uint64_t) {
                for (const auto& [key, item] : held->entries) { static_cast<void>(key); release(item); }
                for (const auto item : held->items) release(item);
                if (held->type == Type::fd && held->integer >= 0) static_cast<void>(p.call(libsystem_library, "_close", {static_cast<std::uint64_t>(held->integer)}));
                if (held->type == Type::connection) {
                    if (held->finalizer) static_cast<void>(EngineAdapter(process().calling_engine()).call(held->finalizer, std::array<std::uint64_t, 1>{held->context}));
                    if (held->handler) static_cast<void>(p.call(libsystem_library, "__Block_release", {held->handler}));
                    if (held->queue) static_cast<void>(p.call(libsystem_library, "_dispatch_release", {held->queue}));
                }
                if (held->type == Type::endpoint) release(held->connection);
                if (held->guest) process().release_guest(held->guest);
            }, immortal);
    }
    std::uint64_t string_object(const std::string& text) { return make(Type::string, [&](Value& v) { v.text = text; }); }
    // Dictionary access (key order kept for xpc_dictionary_apply).
    std::uint64_t get(std::uint64_t dictionary, const std::string& key) {
        const auto found = value(dictionary);
        if (!found || (found->type != Type::dictionary && found->type != Type::error)) return 0;
        std::lock_guard lock(found->mutex);
        for (const auto& [name, item] : found->entries) if (name == key) return item;
        return 0;
    }
    void set(std::uint64_t dictionary, const std::string& key, std::uint64_t item, bool retained) {
        const auto found = typed(dictionary, Type::dictionary);
        if (!found) throw std::runtime_error("Alchemy XPC: a dictionary function on an object that is not an XPC dictionary");
        if (item && !retained) item = retain(item);
        std::uint64_t old{};
        {
            std::lock_guard lock(found->mutex);
            const auto at = std::find_if(found->entries.begin(), found->entries.end(), [&](const auto& entry) { return entry.first == key; });
            if (at != found->entries.end()) {
                old = at->second;
                if (item) at->second = item; else found->entries.erase(at);
            } else if (item) found->entries.emplace_back(key, item);
        }
        release(old);
    }
    std::uint64_t at(std::uint64_t array, std::uint64_t index) {
        const auto found = typed(array, Type::array);
        if (!found) return 0;
        std::lock_guard lock(found->mutex);
        return index < found->items.size() ? found->items[static_cast<std::size_t>(index)] : 0;
    }
    // index SIZE_MAX (XPC_ARRAY_APPEND) appends.
    void put(std::uint64_t array, std::uint64_t index, std::uint64_t item, bool retained) {
        const auto found = typed(array, Type::array);
        if (!found) throw std::runtime_error("Alchemy XPC: an array function on an object that is not an XPC array");
        if (!item) throw std::runtime_error("Alchemy XPC: a NULL value for an XPC array");
        if (!retained) item = retain(item);
        std::uint64_t old{};
        {
            std::lock_guard lock(found->mutex);
            if (index == UINT64_MAX) found->items.push_back(item);
            else if (index < found->items.size()) old = std::exchange(found->items[static_cast<std::size_t>(index)], item);
            else { release(item); throw std::runtime_error("Alchemy XPC: an array index past the end"); }
        }
        release(old);
    }
    std::uint64_t error(const std::string& description) {
        const auto text = string_object(description);
        return make(Type::error, [&](Value& v) { v.entries.emplace_back("XPCErrorDescription", text); }, true);
    }
    // Calls `block(argument)` on `queue` (the default global queue when 0), retaining the
    // argument and the block until it ran.
    void deliver(std::uint64_t queue, std::uint64_t block, std::uint64_t argument) {
        if (!block) return;
        const auto copy = p.call(libsystem_library, "__Block_copy", {block});
        std::uint64_t id;
        { std::lock_guard lock(mutex); id = next_delivery++; deliveries[id] = Delivery{copy, retain(argument)}; }
        const auto target = queue ? queue : p.call(libsystem_library, "_dispatch_get_global_queue", {0, 0});
        static_cast<void>(p.call(libsystem_library, "_dispatch_async_f", {target, id, deliver_entry}));
    }
    std::string describe(std::uint64_t object, unsigned depth = 0) {
        const auto found = value(object);
        if (!found) return "<invalid>";
        std::unique_lock lock(found->mutex);
        char buffer[64];
        switch (found->type) {
        case Type::string: return "<string: \"" + found->text + "\">";
        case Type::int64: std::snprintf(buffer, sizeof buffer, "<int64: %lld>", static_cast<long long>(found->integer)); return buffer;
        case Type::uint64: std::snprintf(buffer, sizeof buffer, "<uint64: %llu>", static_cast<unsigned long long>(found->integer)); return buffer;
        case Type::boolean: return found->integer ? "<bool: true>" : "<bool: false>";
        case Type::double_value: std::snprintf(buffer, sizeof buffer, "<double: %g>", found->real); return buffer;
        case Type::date: std::snprintf(buffer, sizeof buffer, "<date: %lld>", static_cast<long long>(found->integer)); return buffer;
        case Type::data: return "<data: { length = " + std::to_string(found->bytes.size()) + " bytes }>";
        case Type::null: return "<null>";
        case Type::fd: return "<fd: " + std::to_string(found->integer) + ">";
        case Type::connection: return "<connection: { name = " + found->text + " }>";
        default: break;
        }
        if (depth > 16) return "<...>";
        std::string text = found->type == Type::array ? "<array: { count = " + std::to_string(found->items.size()) + ", contents =" :
            "<" + std::string(type_names[static_cast<std::size_t>(found->type)]) + ": { count = " + std::to_string(found->entries.size()) + ", contents =";
        // Children are described without this object's lock.
        const auto entries = found->entries;
        const auto items = found->items;
        lock.unlock();
        for (const auto& [key, item] : entries) text += "\n\t\"" + key + "\" => " + describe(item, depth + 1);
        for (std::size_t index = 0; index < items.size(); ++index) text += "\n\t" + std::to_string(index) + ": " + describe(items[index], depth + 1);
        return text + "\n}>";
    }
    std::uint64_t copy(std::uint64_t object) {
        const auto found = value(object);
        if (!found) return 0;
        if (found->type == Type::boolean || found->type == Type::error || found->type == Type::null) return retain(object);
        if (found->type == Type::connection || found->type == Type::endpoint) return retain(object);
        std::vector<std::pair<std::string, std::uint64_t>> entries;
        std::vector<std::uint64_t> items;
        Value scalar;
        {
            std::lock_guard lock(found->mutex);
            entries = found->entries; items = found->items;
            scalar.text = found->text; scalar.integer = found->integer; scalar.real = found->real; scalar.bytes = found->bytes;
        }
        for (auto& [key, item] : entries) { static_cast<void>(key); item = copy(item); }
        for (auto& item : items) item = copy(item);
        if (found->type == Type::fd) {
            const auto duplicate = static_cast<std::int32_t>(p.call(libsystem_library, "_dup", {static_cast<std::uint64_t>(scalar.integer)}));
            if (duplicate < 0) return 0;
            scalar.integer = duplicate;
        }
        return make(found->type, [&](Value& v) {
            v.entries = std::move(entries); v.items = std::move(items);
            v.text = scalar.text; v.integer = scalar.integer; v.real = scalar.real; v.bytes = scalar.bytes;
        });
    }
    bool equal(std::uint64_t left, std::uint64_t right) {
        if (left == right) return true;
        const auto a = value(left), b = value(right);
        if (!a || !b || a->type != b->type) return false;
        std::vector<std::pair<std::string, std::uint64_t>> ea, eb;
        std::vector<std::uint64_t> ia, ib;
        {
            std::scoped_lock lock(a->mutex, b->mutex);
            if (a->text != b->text || a->integer != b->integer || a->bytes != b->bytes ||
                std::memcmp(&a->real, &b->real, sizeof a->real) != 0) return false;
            if (a->entries.size() != b->entries.size() || a->items.size() != b->items.size()) return false;
            ea = a->entries; eb = b->entries; ia = a->items; ib = b->items;
        }
        for (const auto& [key, item] : ea) {
            const auto other = std::find_if(eb.begin(), eb.end(), [&](const auto& entry) { return entry.first == key; });
            if (other == eb.end() || !equal(item, other->second)) return false;
        }
        for (std::size_t index = 0; index < ia.size(); ++index) if (!equal(ia[index], ib[index])) return false;
        return true;
    }
};

inline std::string read_text(GuestMemory& m, std::uint64_t address) { return guest_string(m, address, 1 << 20); }

// The connections (darwin/launchd/xpc_connections.cpp), installed by
// install_libsystem_xpc after the objects.
void install_xpc_connections(Provider& p, const std::shared_ptr<Xpc>& xpc);

} // namespace alchemy::export_fill::xpc_detail
