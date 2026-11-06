#include "alchemy/darwin/libsystem/misc/xpc_internal.hpp"

#include <stdexcept>

// XPC connections, endpoints, replies, xpc_main and transactions (<xpc/connection.h>), moved
// here from libsystem/misc/xpc_objects.cpp (the objects stay there; their internals are
// misc/xpc_internal.hpp).
//
// No launchd XPC service host runs yet, so no named or anonymous XPC service can be reached. A
// connection behaves as one whose service does not exist: once resumed, its event handler
// receives XPC_ERROR_CONNECTION_INVALID; messages are discarded; replies are
// XPC_ERROR_CONNECTION_INVALID (asynchronously, or returned by _sync); cancelling delivers
// XPC_ERROR_CONNECTION_INVALID last. xpc_main stops (no process here is launched as an XPC
// service).
namespace alchemy::export_fill::xpc_detail {

void install_xpc_connections(Provider& p, const std::shared_ptr<Xpc>& xpc) {
    // Only a message received from a connection has a reply; none ever arrives here.
    p.libsystem("_xpc_dictionary_create_reply", [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; }, 0);
    p.libsystem("_xpc_dictionary_get_remote_connection", [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; }, 0);
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
    // xpc_main(handler): this process was not launched as an XPC service (no launchd).
    p.libsystem("_xpc_main", [](CpuState&, GuestMemory&) -> std::uint64_t {
        throw std::runtime_error("Alchemy: xpc_main is unsupported (no launchd: this process is not an XPC service)");
    });
    // Transactions count outstanding work for sudden termination, which never happens here.
    for (const auto* name : {"_xpc_transaction_begin", "_xpc_transaction_end"})
        p.libsystem(name, [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; });}

} // namespace alchemy::export_fill::xpc_detail
