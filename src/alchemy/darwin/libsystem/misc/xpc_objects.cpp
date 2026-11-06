#include "alchemy/darwin/libsystem/misc/xpc_internal.hpp"
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
// Connections, endpoints, replies and xpc_main are darwin/launchd/xpc_connections.cpp (the
// internals both files share: xpc_internal.hpp). Shared memory objects name guest memory,
// which every thread of the process already shares, so mapping one returns its own region. Activities are kept by launchd, which
// is not there to run them: registering one succeeds and its handler never runs.
namespace alchemy::export_fill {

using namespace xpc_detail;


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

    // Connections, endpoints, replies, xpc_main and transactions: darwin/launchd/xpc_connections.cpp.
    xpc_detail::install_xpc_connections(p, xpc);
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
    // xpc_set_event_stream_handler(stream, queue, handler): launchd sends no events here.
    p.libsystem("_xpc_set_event_stream_handler", [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; });
}

} // namespace alchemy::export_fill
