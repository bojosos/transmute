#include "alchemy/engine_adapter.hpp"
#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/darwin/libsystem/install.hpp"

#include "alchemy/object_store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
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

// XPC objects (<xpc/xpc.h>) as Objective-C objects of the OS_xpc_* classes: dictionaries,
// arrays, strings, signed and unsigned integers, booleans (the XPC_BOOL_TRUE/FALSE
// singletons), doubles, dates, data, UUIDs, file descriptors (each object owns a dup of the
// descriptor), null, errors and endpoints. xpc_get_type is the object's class, and the
// XPC_TYPE_* descriptors are those classes. Values are retained by the containers that hold
// them, and returned borrowed by the getters, as in libxpc.
//
// Connections: no launchd runs under Alchemy, so no named or anonymous XPC service can be
// reached. A connection behaves as one whose service does not exist: once resumed, its event
// handler receives XPC_ERROR_CONNECTION_INVALID; messages are discarded; replies are
// XPC_ERROR_CONNECTION_INVALID (asynchronously, or returned by _sync); cancelling delivers
// XPC_ERROR_CONNECTION_INVALID last. xpc_main stops (no process here is launched as an XPC
// service). Shared memory objects name guest memory, which every thread of the process
// already shares, so mapping one returns its own region. Activities are kept by launchd, which
// is not there to run them: registering one succeeds and its handler never runs.
namespace alchemy::export_fill {
namespace {

enum class Type : unsigned {
    dictionary, array, string, int64, uint64, boolean, double_value, date, data, uuid, fd, null, error, connection, endpoint, shmem, mach_send, count
};
constexpr std::array<const char*, static_cast<std::size_t>(Type::count)> type_names{
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

std::string read_text(GuestMemory& m, std::uint64_t address) { return guest_string(m, address, 1 << 20); }

} // namespace

void install_libsystem_xpc(Provider& p, ObjcRuntime& objc) {
    const auto xpc = std::make_shared<Xpc>(p, objc);
    auto& process = p.process();
    auto& memory = process.calling_engine();
    // OS_xpc_object and its classes; XPC_TYPE_* are the classes themselves.
    xpc->root = objc.find_class("OS_xpc_object");
    if (!xpc->root) xpc->root = objc.register_native_class("OS_xpc_object", objc.find_class("NSObject"), 16);
    for (std::size_t index = 0; index < type_names.size(); ++index) {
        const auto name = "OS_xpc_" + std::string(type_names[index]);
        auto cls = objc.find_class(name);
        if (!cls) cls = objc.register_native_class(name, xpc->root, 16);
        xpc->classes[index] = cls;
        xpc->names[index] = process.allocate_guest(16);
        put_string(memory, xpc->names[index], type_names[index]);
        process.install_export(std::string(libsystem_library), "__xpc_type_" + std::string(type_names[index]), {cls, ExportKind::data});
    }
    // The singletons: XPC_BOOL_TRUE/FALSE and the connection errors are the exported objects.
    xpc->bool_true = xpc->make(Type::boolean, [](Value& v) { v.integer = 1; }, true);
    xpc->bool_false = xpc->make(Type::boolean, [](Value& v) { v.integer = 0; }, true);
    process.install_export(std::string(libsystem_library), "__xpc_bool_true", {xpc->bool_true, ExportKind::data});
    process.install_export(std::string(libsystem_library), "__xpc_bool_false", {xpc->bool_false, ExportKind::data});
    xpc->connection_invalid = xpc->error("Connection invalid");
    xpc->connection_interrupted = xpc->error("Connection interrupted");
    xpc->termination_imminent = xpc->error("Process termination imminent");
    process.install_export(std::string(libsystem_library), "__xpc_error_connection_invalid", {xpc->connection_invalid, ExportKind::data});
    process.install_export(std::string(libsystem_library), "__xpc_error_connection_interrupted", {xpc->connection_interrupted, ExportKind::data});
    process.install_export(std::string(libsystem_library), "__xpc_error_termination_imminent", {xpc->termination_imminent, ExportKind::data});
    // const char* const _xpc_error_key_description / _xpc_event_key_name
    for (const auto& [symbol, text] : std::array<std::pair<const char*, const char*>, 2>{{
             {"__xpc_error_key_description", "XPCErrorDescription"}, {"__xpc_event_key_name", "XPCEventName"}}}) {
        const auto string = process.allocate_guest(32), cell = process.allocate_guest(8);
        put_string(memory, string, text);
        memory.write64(cell, string);
        process.install_export(std::string(libsystem_library), symbol, {cell, ExportKind::data});
    }
    // Deliveries: block(argument) on its queue.
    xpc->deliver_entry = process.install_function("__alchemy_xpc_deliver", [xpc](CpuState& s, GuestMemory& m) {
        Xpc::Delivery delivery;
        {
            std::lock_guard lock(xpc->mutex);
            const auto found = xpc->deliveries.find(s.x[0]);
            if (found == xpc->deliveries.end()) { s.x[0] = 0; return; }
            delivery = found->second;
            xpc->deliveries.erase(found);
        }
        static_cast<void>(EngineAdapter(xpc->process().calling_engine()).call(m.read64(delivery.block + 16), std::array<std::uint64_t, 2>{delivery.block, delivery.argument}));
        static_cast<void>(xpc->p.call(libsystem_library, "__Block_release", {delivery.block}));
        xpc->release(delivery.argument);
        s.x[0] = 0;
    }, "alchemy.internal");

    // ---- objects ----
    p.libsystem("_xpc_retain", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t { return xpc->retain(s.x[0]); }, 0);
    p.libsystem("_xpc_release", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t { xpc->release(s.x[0]); return 0; });
    p.libsystem("_xpc_get_type", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto found = xpc->value(s.x[0]);
        return found ? xpc->classes[static_cast<std::size_t>(found->type)] : 0;
    }, 0);
    p.libsystem("_xpc_type_get_name", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t {
        for (std::size_t index = 0; index < xpc->classes.size(); ++index)
            if (xpc->classes[index] == s.x[0]) return xpc->names[index];
        return 0;
    }, 0);
    p.libsystem("_xpc_copy", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t { return xpc->copy(s.x[0]); }, 0);
    p.libsystem("_xpc_equal", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t { return xpc->equal(s.x[0], s.x[1]) ? 1 : 0; });
    p.libsystem("_xpc_hash", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto text = xpc->describe(s.x[0]);
        std::uint64_t hash = 1469598103934665603ULL;
        for (const auto c : text) { hash ^= static_cast<std::uint8_t>(c); hash *= 1099511628211ULL; }
        return hash;
    });
    p.libsystem("_xpc_copy_description", [xpc](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto text = xpc->describe(s.x[0]);
        const auto result = xpc->p.malloc(text.size() + 1);
        put_string(m, result, text);
        return result;
    }, 0);

    // ---- scalars ----
    p.libsystem("_xpc_null_create", [xpc](CpuState&, GuestMemory&) -> std::uint64_t { return xpc->make(Type::null); }, 0);
    p.libsystem("_xpc_bool_create", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t { return s.x[0] & 0xff ? xpc->bool_true : xpc->bool_false; }, 0);
    p.libsystem("_xpc_bool_get_value", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto found = xpc->typed(s.x[0], Type::boolean); return found && found->integer ? 1 : 0;
    });
    for (const auto& [name, type] : std::array<std::pair<const char*, Type>, 3>{{{"_xpc_int64_create", Type::int64}, {"_xpc_uint64_create", Type::uint64}, {"_xpc_date_create", Type::date}}})
        p.libsystem(name, [xpc, type = type](CpuState& s, GuestMemory&) -> std::uint64_t {
            return xpc->make(type, [&](Value& v) { v.integer = static_cast<std::int64_t>(s.x[0]); });
        }, 0);
    p.libsystem("_xpc_date_create_from_current", [xpc](CpuState&, GuestMemory&) -> std::uint64_t {
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        return xpc->make(Type::date, [&](Value& v) { v.integer = now; });
    }, 0);
    for (const auto& [name, type] : std::array<std::pair<const char*, Type>, 3>{{{"_xpc_int64_get_value", Type::int64}, {"_xpc_uint64_get_value", Type::uint64}, {"_xpc_date_get_value", Type::date}}})
        p.libsystem(name, [xpc, type = type](CpuState& s, GuestMemory&) -> std::uint64_t {
            const auto found = xpc->typed(s.x[0], type); return found ? static_cast<std::uint64_t>(found->integer) : 0;
        });
    p.libsystem("_xpc_double_create", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t {
        double real{}; std::memcpy(&real, &s.v[0][0], sizeof real);
        return xpc->make(Type::double_value, [&](Value& v) { v.real = real; });
    }, 0);
    p.libsystem("_xpc_double_get_value", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto found = xpc->typed(s.x[0], Type::double_value);
        const double real = found ? found->real : 0.0;
        std::memcpy(&s.v[0][0], &real, sizeof real); s.v[0][1] = 0;
        return 0;
    });
    p.libsystem("_xpc_string_create", [xpc](CpuState& s, GuestMemory& m) -> std::uint64_t { return xpc->string_object(read_text(m, s.x[0])); }, 0);
    // xpc_string_create_with_format(format, ...): vsnprintf over the variadic stack slots.
    p.libsystem("_xpc_string_create_with_format", [xpc](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto length = xpc->p.call(libsystem_library, "_vsnprintf", {0, 0, s.x[0], s.sp});
        if (static_cast<std::int32_t>(length) < 0) return 0;
        const auto buffer = xpc->p.malloc(length + 1);
        static_cast<void>(xpc->p.call(libsystem_library, "_vsnprintf", {buffer, length + 1, s.x[0], s.sp}));
        const auto result = xpc->string_object(read_text(m, buffer));
        static_cast<void>(xpc->p.call(libsystem_library, "_free", {buffer}));
        return result;
    }, 0);
    p.libsystem("_xpc_string_get_string_ptr", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto found = xpc->typed(s.x[0], Type::string); return found ? found->guest : 0;
    }, 0);
    p.libsystem("_xpc_string_get_length", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto found = xpc->typed(s.x[0], Type::string); return found ? found->text.size() : 0;
    });
    p.libsystem("_xpc_data_create", [xpc](CpuState& s, GuestMemory& m) -> std::uint64_t {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(s.x[1]));
        if (!bytes.empty()) m.read_bytes(s.x[0], bytes);
        return xpc->make(Type::data, [&](Value& v) { v.bytes = std::move(bytes); });
    }, 0);
    p.libsystem("_xpc_data_create_with_dispatch_data", [xpc](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto cell = xpc->process().allocate_guest(16);
        const auto map = xpc->p.call(libsystem_library, "_dispatch_data_create_map", {s.x[0], cell, cell + 8});
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(m.read64(cell + 8)));
        if (!bytes.empty()) m.read_bytes(m.read64(cell), bytes);
        static_cast<void>(xpc->p.call(libsystem_library, "_dispatch_release", {map}));
        xpc->process().release_guest(cell);
        return xpc->make(Type::data, [&](Value& v) { v.bytes = std::move(bytes); });
    }, 0);
    p.libsystem("_xpc_data_get_bytes_ptr", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto found = xpc->typed(s.x[0], Type::data); return found && !found->bytes.empty() ? found->guest : 0;
    }, 0);
    p.libsystem("_xpc_data_get_length", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto found = xpc->typed(s.x[0], Type::data); return found ? found->bytes.size() : 0;
    });
    // size_t xpc_data_get_bytes(data, void* buffer, size_t offset, size_t length): the bytes copied.
    p.libsystem("_xpc_data_get_bytes", [xpc](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto found = xpc->typed(s.x[0], Type::data);
        if (!found || s.x[2] >= found->bytes.size()) return 0;
        const auto count = std::min<std::uint64_t>(s.x[3], found->bytes.size() - s.x[2]);
        m.write_bytes(s.x[1], std::span(found->bytes).subspan(static_cast<std::size_t>(s.x[2]), static_cast<std::size_t>(count)));
        return count;
    });
    p.libsystem("_xpc_uuid_create", [xpc](CpuState& s, GuestMemory& m) -> std::uint64_t {
        std::vector<std::uint8_t> bytes(16);
        m.read_bytes(s.x[0], bytes);
        return xpc->make(Type::uuid, [&](Value& v) { v.bytes = std::move(bytes); });
    }, 0);
    p.libsystem("_xpc_uuid_get_bytes", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto found = xpc->typed(s.x[0], Type::uuid); return found ? found->guest : 0;
    }, 0);
    // xpc_fd_create(fd): the object owns a duplicate; xpc_fd_dup returns another one.
    p.libsystem("_xpc_fd_create", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto duplicate = static_cast<std::int32_t>(xpc->p.call(libsystem_library, "_dup", {s.x[0]}));
        if (duplicate < 0) return 0;
        return xpc->make(Type::fd, [&](Value& v) { v.integer = duplicate; });
    }, 0);
    p.libsystem("_xpc_fd_dup", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto found = xpc->typed(s.x[0], Type::fd);
        if (!found) return UINT64_MAX;
        return xpc->p.call(libsystem_library, "_dup", {static_cast<std::uint64_t>(found->integer)});
    }, UINT64_MAX);

    // ---- dictionaries ----
    p.libsystem("_xpc_dictionary_create", [xpc](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto dictionary = xpc->make(Type::dictionary);
        for (std::uint64_t index = 0; index < s.x[2]; ++index) {
            const auto key = m.read64(s.x[0] + index * 8), item = m.read64(s.x[1] + index * 8);
            if (key && item) xpc->set(dictionary, read_text(m, key), item, false);
        }
        return dictionary;
    }, 0);
    p.libsystem("_xpc_dictionary_create_empty", [xpc](CpuState&, GuestMemory&) -> std::uint64_t { return xpc->make(Type::dictionary); }, 0);
    // Only a message received from a connection has a reply; none ever arrives here.
    p.libsystem("_xpc_dictionary_create_reply", [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; }, 0);
    p.libsystem("_xpc_dictionary_get_remote_connection", [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; }, 0);
    p.libsystem("_xpc_dictionary_set_value", [xpc](CpuState& s, GuestMemory& m) -> std::uint64_t {
        xpc->set(s.x[0], read_text(m, s.x[1]), s.x[2], false); return 0;
    });
    p.libsystem("_xpc_dictionary_get_value", [xpc](CpuState& s, GuestMemory& m) -> std::uint64_t { return xpc->get(s.x[0], read_text(m, s.x[1])); }, 0);
    p.libsystem("_xpc_dictionary_get_count", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto found = xpc->value(s.x[0]);
        if (!found) return 0;
        std::lock_guard lock(found->mutex); return found->entries.size();
    });
    // bool xpc_dictionary_apply(dictionary, ^bool(const char* key, xpc_object_t value)): stops at false.
    p.libsystem("_xpc_dictionary_apply", [xpc](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto found = xpc->value(s.x[0]);
        if (!found) return 0;
        std::vector<std::pair<std::string, std::uint64_t>> entries;
        { std::lock_guard lock(found->mutex); entries = found->entries; }
        for (const auto& [key, item] : entries) {
            const auto name = xpc->process().allocate_guest(key.size() + 1);
            put_string(m, name, key);
            const auto keep = EngineAdapter(xpc->process().calling_engine()).call(m.read64(s.x[1] + 16), std::array<std::uint64_t, 3>{s.x[1], name, item});
            xpc->process().release_guest(name);
            if (!(keep & 0xff)) return 0;
        }
        return 1;
    });
    // Typed dictionary accessors: a missing or differently typed value reads as 0 / NULL / false.
    const auto install_typed = [&](const char* setter, const char* getter, Type type, auto make, auto read) {
        p.libsystem(setter, [xpc, make](CpuState& s, GuestMemory& m) -> std::uint64_t {
            xpc->set(s.x[0], read_text(m, s.x[1]), make(*xpc, s, m), true); return 0;
        });
        p.libsystem(getter, [xpc, type, read](CpuState& s, GuestMemory& m) -> std::uint64_t {
            const auto found = xpc->typed(xpc->get(s.x[0], read_text(m, s.x[1])), type);
            return found ? read(*xpc, *found, s, m) : 0;
        }, 0);
    };
    install_typed("_xpc_dictionary_set_string", "_xpc_dictionary_get_string", Type::string,
        [](Xpc& x, CpuState& s, GuestMemory& m) { return x.string_object(read_text(m, s.x[2])); },
        [](Xpc&, Value& v, CpuState&, GuestMemory&) { return v.guest; });
    install_typed("_xpc_dictionary_set_int64", "_xpc_dictionary_get_int64", Type::int64,
        [](Xpc& x, CpuState& s, GuestMemory&) { return x.make(Type::int64, [&](Value& v) { v.integer = static_cast<std::int64_t>(s.x[2]); }); },
        [](Xpc&, Value& v, CpuState&, GuestMemory&) { return static_cast<std::uint64_t>(v.integer); });
    install_typed("_xpc_dictionary_set_uint64", "_xpc_dictionary_get_uint64", Type::uint64,
        [](Xpc& x, CpuState& s, GuestMemory&) { return x.make(Type::uint64, [&](Value& v) { v.integer = static_cast<std::int64_t>(s.x[2]); }); },
        [](Xpc&, Value& v, CpuState&, GuestMemory&) { return static_cast<std::uint64_t>(v.integer); });
    install_typed("_xpc_dictionary_set_date", "_xpc_dictionary_get_date", Type::date,
        [](Xpc& x, CpuState& s, GuestMemory&) { return x.make(Type::date, [&](Value& v) { v.integer = static_cast<std::int64_t>(s.x[2]); }); },
        [](Xpc&, Value& v, CpuState&, GuestMemory&) { return static_cast<std::uint64_t>(v.integer); });
    install_typed("_xpc_dictionary_set_bool", "_xpc_dictionary_get_bool", Type::boolean,
        [](Xpc& x, CpuState& s, GuestMemory&) { return x.retain(s.x[2] & 0xff ? x.bool_true : x.bool_false); },
        [](Xpc&, Value& v, CpuState&, GuestMemory&) { return static_cast<std::uint64_t>(v.integer ? 1 : 0); });
    install_typed("_xpc_dictionary_set_double", "_xpc_dictionary_get_double", Type::double_value,
        [](Xpc& x, CpuState& s, GuestMemory&) { double r{}; std::memcpy(&r, &s.v[0][0], sizeof r); return x.make(Type::double_value, [&](Value& v) { v.real = r; }); },
        [](Xpc&, Value& v, CpuState& s, GuestMemory&) { std::memcpy(&s.v[0][0], &v.real, sizeof v.real); s.v[0][1] = 0; return std::uint64_t{0}; });
    install_typed("_xpc_dictionary_set_uuid", "_xpc_dictionary_get_uuid", Type::uuid,
        [](Xpc& x, CpuState& s, GuestMemory& m) { std::vector<std::uint8_t> b(16); m.read_bytes(s.x[2], b); return x.make(Type::uuid, [&](Value& v) { v.bytes = b; }); },
        [](Xpc&, Value& v, CpuState&, GuestMemory&) { return v.guest; });
    // set_data(dictionary, key, bytes, length) / get_data(dictionary, key, size_t* length)
    p.libsystem("_xpc_dictionary_set_data", [xpc](CpuState& s, GuestMemory& m) -> std::uint64_t {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(s.x[3]));
        if (!bytes.empty()) m.read_bytes(s.x[2], bytes);
        xpc->set(s.x[0], read_text(m, s.x[1]), xpc->make(Type::data, [&](Value& v) { v.bytes = std::move(bytes); }), true);
        return 0;
    });
    p.libsystem("_xpc_dictionary_get_data", [xpc](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto found = xpc->typed(xpc->get(s.x[0], read_text(m, s.x[1])), Type::data);
        if (s.x[2]) m.write64(s.x[2], found ? found->bytes.size() : 0);
        return found && !found->bytes.empty() ? found->guest : 0;
    }, 0);
    p.libsystem("_xpc_dictionary_set_fd", [xpc](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto duplicate = static_cast<std::int32_t>(xpc->p.call(libsystem_library, "_dup", {s.x[2]}));
        if (duplicate < 0) return 0;
        xpc->set(s.x[0], read_text(m, s.x[1]), xpc->make(Type::fd, [&](Value& v) { v.integer = duplicate; }), true);
        return 0;
    });
    p.libsystem("_xpc_dictionary_dup_fd", [xpc](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto found = xpc->typed(xpc->get(s.x[0], read_text(m, s.x[1])), Type::fd);
        return found ? xpc->p.call(libsystem_library, "_dup", {static_cast<std::uint64_t>(found->integer)}) : UINT64_MAX;
    }, UINT64_MAX);
    for (const auto& [name, type] : std::array<std::pair<const char*, Type>, 2>{{{"_xpc_dictionary_get_dictionary", Type::dictionary}, {"_xpc_dictionary_get_array", Type::array}}})
        p.libsystem(name, [xpc, type = type](CpuState& s, GuestMemory& m) -> std::uint64_t {
            const auto item = xpc->get(s.x[0], read_text(m, s.x[1]));
            return xpc->typed(item, type) ? item : 0;
        }, 0);

    // ---- arrays ----
    p.libsystem("_xpc_array_create", [xpc](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto array = xpc->make(Type::array);
        for (std::uint64_t index = 0; index < s.x[1]; ++index) if (const auto item = m.read64(s.x[0] + index * 8)) xpc->put(array, UINT64_MAX, item, false);
        return array;
    }, 0);
    p.libsystem("_xpc_array_create_empty", [xpc](CpuState&, GuestMemory&) -> std::uint64_t { return xpc->make(Type::array); }, 0);
    p.libsystem("_xpc_array_append_value", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t { xpc->put(s.x[0], UINT64_MAX, s.x[1], false); return 0; });
    p.libsystem("_xpc_array_set_value", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t { xpc->put(s.x[0], s.x[1], s.x[2], false); return 0; });
    p.libsystem("_xpc_array_get_value", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t { return xpc->at(s.x[0], s.x[1]); }, 0);
    p.libsystem("_xpc_array_get_count", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto found = xpc->typed(s.x[0], Type::array);
        if (!found) return 0;
        std::lock_guard lock(found->mutex); return found->items.size();
    });
    // bool xpc_array_apply(array, ^bool(size_t index, xpc_object_t value))
    p.libsystem("_xpc_array_apply", [xpc](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto found = xpc->typed(s.x[0], Type::array);
        if (!found) return 0;
        std::vector<std::uint64_t> items;
        { std::lock_guard lock(found->mutex); items = found->items; }
        for (std::size_t index = 0; index < items.size(); ++index) {
            const auto keep = EngineAdapter(xpc->process().calling_engine()).call(m.read64(s.x[1] + 16), std::array<std::uint64_t, 3>{s.x[1], index, items[index]});
            if (!(keep & 0xff)) return 0;
        }
        return 1;
    });
    // Typed array accessors: index XPC_ARRAY_APPEND (SIZE_MAX) appends.
    const auto install_array = [&](const char* setter, const char* getter, Type type, auto make, auto read) {
        p.libsystem(setter, [xpc, make](CpuState& s, GuestMemory& m) -> std::uint64_t { xpc->put(s.x[0], s.x[1], make(*xpc, s, m), true); return 0; });
        p.libsystem(getter, [xpc, type, read](CpuState& s, GuestMemory& m) -> std::uint64_t {
            const auto found = xpc->typed(xpc->at(s.x[0], s.x[1]), type);
            return found ? read(*xpc, *found, s, m) : 0;
        }, 0);
    };
    install_array("_xpc_array_set_string", "_xpc_array_get_string", Type::string,
        [](Xpc& x, CpuState& s, GuestMemory& m) { return x.string_object(read_text(m, s.x[2])); },
        [](Xpc&, Value& v, CpuState&, GuestMemory&) { return v.guest; });
    install_array("_xpc_array_set_int64", "_xpc_array_get_int64", Type::int64,
        [](Xpc& x, CpuState& s, GuestMemory&) { return x.make(Type::int64, [&](Value& v) { v.integer = static_cast<std::int64_t>(s.x[2]); }); },
        [](Xpc&, Value& v, CpuState&, GuestMemory&) { return static_cast<std::uint64_t>(v.integer); });
    install_array("_xpc_array_set_uint64", "_xpc_array_get_uint64", Type::uint64,
        [](Xpc& x, CpuState& s, GuestMemory&) { return x.make(Type::uint64, [&](Value& v) { v.integer = static_cast<std::int64_t>(s.x[2]); }); },
        [](Xpc&, Value& v, CpuState&, GuestMemory&) { return static_cast<std::uint64_t>(v.integer); });
    install_array("_xpc_array_set_bool", "_xpc_array_get_bool", Type::boolean,
        [](Xpc& x, CpuState& s, GuestMemory&) { return x.retain(s.x[2] & 0xff ? x.bool_true : x.bool_false); },
        [](Xpc&, Value& v, CpuState&, GuestMemory&) { return static_cast<std::uint64_t>(v.integer ? 1 : 0); });
    install_array("_xpc_array_set_double", "_xpc_array_get_double", Type::double_value,
        [](Xpc& x, CpuState& s, GuestMemory&) { double r{}; std::memcpy(&r, &s.v[0][0], sizeof r); return x.make(Type::double_value, [&](Value& v) { v.real = r; }); },
        [](Xpc&, Value& v, CpuState& s, GuestMemory&) { std::memcpy(&s.v[0][0], &v.real, sizeof v.real); s.v[0][1] = 0; return std::uint64_t{0}; });
    install_array("_xpc_array_set_uuid", "_xpc_array_get_uuid", Type::uuid,
        [](Xpc& x, CpuState& s, GuestMemory& m) { std::vector<std::uint8_t> b(16); m.read_bytes(s.x[2], b); return x.make(Type::uuid, [&](Value& v) { v.bytes = b; }); },
        [](Xpc&, Value& v, CpuState&, GuestMemory&) { return v.guest; });
    p.libsystem("_xpc_array_set_data", [xpc](CpuState& s, GuestMemory& m) -> std::uint64_t {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(s.x[3]));
        if (!bytes.empty()) m.read_bytes(s.x[2], bytes);
        xpc->put(s.x[0], s.x[1], xpc->make(Type::data, [&](Value& v) { v.bytes = std::move(bytes); }), true);
        return 0;
    });
    p.libsystem("_xpc_array_get_data", [xpc](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto found = xpc->typed(xpc->at(s.x[0], s.x[1]), Type::data);
        if (s.x[2]) m.write64(s.x[2], found ? found->bytes.size() : 0);
        return found && !found->bytes.empty() ? found->guest : 0;
    }, 0);
    p.libsystem("_xpc_array_set_fd", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto duplicate = static_cast<std::int32_t>(xpc->p.call(libsystem_library, "_dup", {s.x[2]}));
        if (duplicate < 0) return 0;
        xpc->put(s.x[0], s.x[1], xpc->make(Type::fd, [&](Value& v) { v.integer = duplicate; }), true);
        return 0;
    });
    p.libsystem("_xpc_array_dup_fd", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto found = xpc->typed(xpc->at(s.x[0], s.x[1]), Type::fd);
        return found ? xpc->p.call(libsystem_library, "_dup", {static_cast<std::uint64_t>(found->integer)}) : UINT64_MAX;
    }, UINT64_MAX);
    for (const auto& [name, type] : std::array<std::pair<const char*, Type>, 2>{{{"_xpc_array_get_dictionary", Type::dictionary}, {"_xpc_array_get_array", Type::array}}})
        p.libsystem(name, [xpc, type = type](CpuState& s, GuestMemory&) -> std::uint64_t {
            const auto item = xpc->at(s.x[0], s.x[1]);
            return xpc->typed(item, type) ? item : 0;
        }, 0);

    // ---- connections (no service is reachable) ----
    const auto make_connection = [xpc](GuestMemory& m, std::uint64_t name, std::uint64_t queue) {
        const auto text = name ? read_text(m, name) : std::string("anonymous");
        if (queue) static_cast<void>(xpc->p.call(libsystem_library, "_dispatch_retain", {queue}));
        return xpc->make(Type::connection, [&](Value& v) { v.text = text; v.queue = queue; });
    };
    p.libsystem("_xpc_connection_create", [make_connection](CpuState& s, GuestMemory& m) -> std::uint64_t { return make_connection(m, s.x[0], s.x[1]); }, 0);
    p.libsystem("_xpc_connection_create_mach_service", [make_connection](CpuState& s, GuestMemory& m) -> std::uint64_t {
        return make_connection(m, s.x[0], s.x[1]);
    }, 0);
    p.libsystem("_xpc_connection_create_from_endpoint", [xpc, make_connection](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto endpoint = xpc->typed(s.x[0], Type::endpoint);
        if (!endpoint) return 0;
        const auto target = xpc->typed(endpoint->connection, Type::connection);
        const auto name = xpc->process().allocate_guest(256);
        put_string(m, name, target ? target->text : std::string("endpoint"));
        const auto result = make_connection(m, name, 0);
        xpc->process().release_guest(name);
        return result;
    }, 0);
    const auto connection_of = [xpc](std::uint64_t object) {
        const auto found = xpc->typed(object, Type::connection);
        if (!found) throw std::runtime_error("Alchemy XPC: a connection function on an object that is not an XPC connection");
        return found;
    };
    // Once resumed (or activated), the connection's handler learns the service is not there.
    const auto invalidate = [xpc, connection_of](std::uint64_t object) {
        const auto found = connection_of(object);
        std::uint64_t handler{}, queue{};
        {
            std::lock_guard lock(found->mutex);
            if (found->invalidated || !found->resumed) return;
            found->invalidated = true;
            handler = found->handler; queue = found->queue;
        }
        xpc->deliver(queue, handler, xpc->connection_invalid);
    };
    p.libsystem("_xpc_connection_set_event_handler", [xpc, connection_of](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto found = connection_of(s.x[0]);
        const auto copy = s.x[1] ? xpc->p.call(libsystem_library, "__Block_copy", {s.x[1]}) : 0;
        std::uint64_t old;
        { std::lock_guard lock(found->mutex); old = std::exchange(found->handler, copy); }
        if (old) static_cast<void>(xpc->p.call(libsystem_library, "__Block_release", {old}));
        return 0;
    });
    p.libsystem("_xpc_connection_set_target_queue", [xpc, connection_of](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto found = connection_of(s.x[0]);
        if (s.x[1]) static_cast<void>(xpc->p.call(libsystem_library, "_dispatch_retain", {s.x[1]}));
        std::uint64_t old;
        { std::lock_guard lock(found->mutex); old = std::exchange(found->queue, s.x[1]); }
        if (old) static_cast<void>(xpc->p.call(libsystem_library, "_dispatch_release", {old}));
        return 0;
    });
    for (const auto* name : {"_xpc_connection_resume", "_xpc_connection_activate"})
        p.libsystem(name, [connection_of, invalidate](CpuState& s, GuestMemory&) -> std::uint64_t {
            const auto found = connection_of(s.x[0]);
            { std::lock_guard lock(found->mutex); found->resumed = true; }
            invalidate(s.x[0]);
            return 0;
        });
    p.libsystem("_xpc_connection_suspend", [connection_of](CpuState& s, GuestMemory&) -> std::uint64_t {
        static_cast<void>(connection_of(s.x[0])); return 0;
    });
    p.libsystem("_xpc_connection_cancel", [connection_of, invalidate](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto found = connection_of(s.x[0]);
        { std::lock_guard lock(found->mutex); found->cancelled = true; found->resumed = true; }
        invalidate(s.x[0]);
        return 0;
    });
    // Messages to a connection whose service does not exist are discarded.
    p.libsystem("_xpc_connection_send_message", [connection_of](CpuState& s, GuestMemory&) -> std::uint64_t { static_cast<void>(connection_of(s.x[0])); return 0; });
    p.libsystem("_xpc_connection_send_message_with_reply", [xpc, connection_of](CpuState& s, GuestMemory&) -> std::uint64_t {
        static_cast<void>(connection_of(s.x[0]));
        xpc->deliver(s.x[2], s.x[3], xpc->connection_invalid);
        return 0;
    });
    p.libsystem("_xpc_connection_send_message_with_reply_sync", [xpc, connection_of](CpuState& s, GuestMemory&) -> std::uint64_t {
        static_cast<void>(connection_of(s.x[0]));
        return xpc->retain(xpc->connection_invalid);
    }, 0);
    p.libsystem("_xpc_connection_send_barrier", [xpc, connection_of](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto found = connection_of(s.x[0]);
        std::uint64_t queue;
        { std::lock_guard lock(found->mutex); queue = found->queue; }
        if (s.x[1]) {
            // dispatch_block_t: no argument; deliver passes one, which the block ignores.
            xpc->deliver(queue, s.x[1], 0);
        }
        return 0;
    });
    p.libsystem("_xpc_connection_get_name", [xpc, connection_of](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto found = connection_of(s.x[0]);
        std::lock_guard lock(found->mutex);
        if (!found->guest) found->guest = xpc->guest_copy(std::span(reinterpret_cast<const std::uint8_t*>(found->text.data()), found->text.size()), true);
        return found->guest;
    }, 0);
    // No peer: pid, uid, gid and audit session are 0.
    for (const auto* name : {"_xpc_connection_get_pid", "_xpc_connection_get_euid", "_xpc_connection_get_egid", "_xpc_connection_get_asid"})
        p.libsystem(name, [connection_of](CpuState& s, GuestMemory&) -> std::uint64_t { static_cast<void>(connection_of(s.x[0])); return 0; });
    p.libsystem("_xpc_connection_set_context", [connection_of](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto found = connection_of(s.x[0]); std::lock_guard lock(found->mutex); found->context = s.x[1]; return 0;
    });
    p.libsystem("_xpc_connection_get_context", [connection_of](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto found = connection_of(s.x[0]); std::lock_guard lock(found->mutex); return found->context;
    }, 0);
    p.libsystem("_xpc_connection_set_finalizer_f", [connection_of](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto found = connection_of(s.x[0]); std::lock_guard lock(found->mutex); found->finalizer = s.x[1]; return 0;
    });
    // int xpc_connection_set_peer_code_signing_requirement(connection, const char*): the requirement is
    // kept (no peer ever connects to be checked).
    p.libsystem("_xpc_connection_set_peer_code_signing_requirement", [connection_of](CpuState& s, GuestMemory&) -> std::uint64_t {
        static_cast<void>(connection_of(s.x[0])); return 0;
    });
    p.libsystem("_xpc_endpoint_create", [xpc, connection_of](CpuState& s, GuestMemory&) -> std::uint64_t {
        static_cast<void>(connection_of(s.x[0]));
        const auto connection = xpc->retain(s.x[0]);
        return xpc->make(Type::endpoint, [&](Value& v) { v.connection = connection; });
    }, 0);
    // xpc_connection_get_audit_token(connection, audit_token_t*): no peer, an all-zero token.
    p.libsystem("_xpc_connection_get_audit_token", [connection_of](CpuState& s, GuestMemory& m) -> std::uint64_t {
        static_cast<void>(connection_of(s.x[0]));
        if (s.x[1]) m.write_bytes(s.x[1], std::array<std::uint8_t, 32>{});
        return 0;
    });
    // Connections stored in dictionaries (only XPC messages carry them between processes).
    p.libsystem("_xpc_dictionary_set_connection", [xpc, connection_of](CpuState& s, GuestMemory& m) -> std::uint64_t {
        static_cast<void>(connection_of(s.x[2]));
        xpc->set(s.x[0], read_text(m, s.x[1]), s.x[2], false);
        return 0;
    });
    p.libsystem("_xpc_dictionary_create_connection", [xpc](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto item = xpc->get(s.x[0], read_text(m, s.x[1]));
        return xpc->typed(item, Type::connection) ? xpc->retain(item) : 0;
    }, 0);
    // Mach send rights in dictionaries: the port name is kept and handed back.
    p.libsystem("_xpc_dictionary_set_mach_send", [xpc](CpuState& s, GuestMemory& m) -> std::uint64_t {
        xpc->set(s.x[0], read_text(m, s.x[1]), xpc->make(Type::mach_send, [&](Value& v) { v.integer = static_cast<std::uint32_t>(s.x[2]); }), true);
        return 0;
    });
    p.libsystem("_xpc_dictionary_copy_mach_send", [xpc](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto found = xpc->typed(xpc->get(s.x[0], read_text(m, s.x[1])), Type::mach_send);
        return found ? static_cast<std::uint64_t>(found->integer) : 0;
    });
    // xpc_shmem_create(region, length) / xpc_shmem_map(shmem, void** region) -> its length.
    p.libsystem("_xpc_shmem_create", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t {
        if (!s.x[0] || !s.x[1]) return 0;
        return xpc->make(Type::shmem, [&](Value& v) { v.region = s.x[0]; v.length = s.x[1]; });
    }, 0);
    p.libsystem("_xpc_shmem_map", [xpc](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto found = xpc->typed(s.x[0], Type::shmem);
        if (!found) return 0;
        if (s.x[1]) m.write64(s.x[1], found->region);
        return found->length;
    });
    // const char* xpc_strerror(int): the errno text.
    p.libsystem("_xpc_strerror", [xpc](CpuState& s, GuestMemory&) -> std::uint64_t {
        return xpc->p.call(libsystem_library, "_strerror", {s.x[0]});
    }, 0);
    // Activities: launchd schedules them; with no launchd, none ever runs.
    p.libsystem("_xpc_activity_register", [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; });
    p.libsystem("_xpc_activity_unregister", [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; });
    p.libsystem("_xpc_activity_copy_criteria", [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; }, 0);
    p.libsystem("_xpc_activity_set_criteria", [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; });
    p.libsystem("_xpc_activity_get_state", [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; });   // XPC_ACTIVITY_STATE_CHECK_IN
    p.libsystem("_xpc_activity_set_state", [](CpuState&, GuestMemory&) -> std::uint64_t { return 1; });
    p.libsystem("_xpc_activity_should_defer", [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; });
    // xpc_main(handler): this process was not launched as an XPC service (no launchd).
    p.libsystem("_xpc_main", [](CpuState&, GuestMemory&) -> std::uint64_t {
        throw std::runtime_error("Alchemy: xpc_main is unsupported (no launchd: this process is not an XPC service)");
    });
    // Transactions count outstanding work for sudden termination, which never happens here.
    for (const auto* name : {"_xpc_transaction_begin", "_xpc_transaction_end"})
        p.libsystem(name, [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; });
    // xpc_set_event_stream_handler(stream, queue, handler): launchd sends no events here.
    p.libsystem("_xpc_set_event_stream_handler", [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; });
}

} // namespace alchemy::export_fill
