#include "alchemy/darwin/kit/completion/runtime/common.hpp"

#include <map>

// NSXPCInterface, NSXPCConnection and NSXPCListener (Foundation; Sparkle).
//
// Interfaces are real descriptions: the protocol, and the classes and
// sub-interfaces declared for selectors' arguments. Connections keep their
// configuration (service or Mach service name, interfaces, exported object,
// handlers). There is no launchd and no XPC service host under Alchemy, so
// nothing can be reached: -resume, -remoteObjectProxy and its variants fail
// explicitly; -invalidate runs the invalidation handler on a global queue,
// as Foundation does. Listeners keep their delegate; -resume fails explicitly
// (only Sparkle's helper processes and XPC services listen).
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;

struct InterfaceState final : ObjectPayload {
    std::mutex mutex;
    Address protocol{};
    std::map<std::tuple<Address, std::uint64_t, bool>, Address> classes, interfaces;   // (selector, index, reply) -> +1 set / interface
};

struct ConnectionState final : ObjectPayload {
    std::mutex mutex;
    Address name{}, exported_interface{}, exported_object{}, remote_interface{};   // +1
    Address interruption{}, invalidation{};                                       // copied blocks
    bool mach{}, invalid{};
    std::uint64_t options{};
};

struct ListenerState final : ObjectPayload { Address name{}, delegate{}; };   // name +1; delegate not retained

} // namespace

void install_xpc(Support& support) {
    const auto interface = support.declare_class("NSXPCInterface", "NSObject", foundation_library);
    const auto connection = support.declare_class("NSXPCConnection", "NSObject", foundation_library);
    const auto listener = support.declare_class("NSXPCListener", "NSObject", foundation_library);
    if (!support.objc.find_class("NSXPCListenerEndpoint")) static_cast<void>(support.declare_class("NSXPCListenerEndpoint", "NSObject", foundation_library));

    // ---- NSXPCInterface ----
    const auto interface_of = [&support](Address object) { return support.objc.host_state<InterfaceState>(object); };
    support.method(interface, "interfaceWithProtocol:", [&support, interface_of](CpuState& s, GuestMemory&) {
        const auto object = support.send_id(support.send_id(s.x[0], "alloc"), "init");
        interface_of(object)->protocol = s.x[2];   // protocols are immortal runtime objects
        s.x[0] = autoreleased(support, object);
    }, "@@:@", true);
    support.method(interface, "protocol", [interface_of](CpuState& s, GuestMemory&) { s.x[0] = interface_of(s.x[0])->protocol; }, "@@:");
    support.method(interface, "setProtocol:", [interface_of](CpuState& s, GuestMemory&) { interface_of(s.x[0])->protocol = s.x[2]; }, "v@:@");
    const auto table_set = [&support, interface_of](Address object, bool classes, Address value, Address selector, std::uint64_t index, bool reply) {
        const auto state = interface_of(object);
        const auto kept = objc_retain(support, value);
        Address old{};
        {
            std::lock_guard lock(state->mutex);
            auto& table = classes ? state->classes : state->interfaces;
            auto& slot = table[{selector, index, reply}];
            old = std::exchange(slot, kept);
        }
        objc_release(support, old);
    };
    const auto table_get = [interface_of](Address object, bool classes, Address selector, std::uint64_t index, bool reply) -> Address {
        const auto state = interface_of(object);
        std::lock_guard lock(state->mutex);
        const auto& table = classes ? state->classes : state->interfaces;
        const auto found = table.find({selector, index, reply});
        return found == table.end() ? 0 : found->second;
    };
    support.method(interface, "setClasses:forSelector:argumentIndex:ofReply:", [table_set](CpuState& s, GuestMemory&) {
        table_set(s.x[0], true, s.x[2], s.x[3], s.x[4], (s.x[5] & 0xff) != 0);
    }, "v@:@:QB");
    support.method(interface, "classesForSelector:argumentIndex:ofReply:", [&support, table_get](CpuState& s, GuestMemory&) {
        const auto found = table_get(s.x[0], true, s.x[2], s.x[3], (s.x[4] & 0xff) != 0);
        s.x[0] = found ? found : support.send_id(class_named(support, "NSSet"), "set");
    }, "@@::QB");
    support.method(interface, "setInterface:forSelector:argumentIndex:ofReply:", [table_set](CpuState& s, GuestMemory&) {
        table_set(s.x[0], false, s.x[2], s.x[3], s.x[4], (s.x[5] & 0xff) != 0);
    }, "v@:@:QB");
    support.method(interface, "interfaceForSelector:argumentIndex:ofReply:", [table_get](CpuState& s, GuestMemory&) {
        s.x[0] = table_get(s.x[0], false, s.x[2], s.x[3], (s.x[4] & 0xff) != 0);
    }, "@@::QB");
    support.method(interface, "dealloc", [&support, interface](CpuState& s, GuestMemory&) {
        if (const auto state = support.objc.find_host_state<InterfaceState>(s.x[0])) {
            for (auto* table : {&state->classes, &state->interfaces})
                for (const auto& [key, value] : std::exchange(*table, {})) { static_cast<void>(key); objc_release(support, value); }
        }
        static_cast<void>(support.objc.send_super(s.x[0], interface, support.objc.selector("dealloc")));
    }, "v@:");

    // ---- NSXPCConnection ----
    const auto connection_of = [&support](Address object) { return support.objc.host_state<ConnectionState>(object); };
    support.method(connection, "initWithServiceName:", [&support, connection_of](CpuState& s, GuestMemory&) {
        connection_of(s.x[0])->name = s.x[2] ? support.send_id(s.x[2], "copy") : 0;
    }, "@@:@");
    support.method(connection, "initWithMachServiceName:options:", [&support, connection_of](CpuState& s, GuestMemory&) {
        const auto state = connection_of(s.x[0]);
        state->name = s.x[2] ? support.send_id(s.x[2], "copy") : 0;
        state->mach = true;
        state->options = s.x[3];
    }, "@@:@Q");
    support.method(connection, "initWithListenerEndpoint:", [](CpuState&, GuestMemory&) {
        unsupported("-[NSXPCConnection initWithListenerEndpoint:] (no XPC listeners under Alchemy)");
    }, "@@:@");
    support.method(connection, "serviceName", [connection_of](CpuState& s, GuestMemory&) {
        const auto state = connection_of(s.x[0]);
        s.x[0] = state->mach ? 0 : state->name;
    }, "@@:");
    struct Field { const char* getter; const char* setter; Address ConnectionState::*member; bool block; };
    for (const auto& field : {Field{"exportedInterface", "setExportedInterface:", &ConnectionState::exported_interface, false},
                              Field{"exportedObject", "setExportedObject:", &ConnectionState::exported_object, false},
                              Field{"remoteObjectInterface", "setRemoteObjectInterface:", &ConnectionState::remote_interface, false},
                              Field{"interruptionHandler", "setInterruptionHandler:", &ConnectionState::interruption, true},
                              Field{"invalidationHandler", "setInvalidationHandler:", &ConnectionState::invalidation, true}}) {
        support.method(connection, field.getter, [connection_of, member = field.member](CpuState& s, GuestMemory&) {
            const auto state = connection_of(s.x[0]);
            std::lock_guard lock(state->mutex);
            s.x[0] = (*state).*member;
        }, field.block ? "@?@:" : "@@:");
        support.method(connection, field.setter, [&support, connection_of, member = field.member, block = field.block](CpuState& s, GuestMemory&) {
            const auto state = connection_of(s.x[0]);
            const auto kept = block ? block_copy(support, s.x[2]) : objc_retain(support, s.x[2]);
            Address old;
            { std::lock_guard lock(state->mutex); old = std::exchange((*state).*member, kept); }
            if (block) block_release(support, old); else objc_release(support, old);
        }, field.block ? "v@:@?" : "v@:@");
    }
    const auto unreachable = [connection_of, &support](Address object, std::string_view what) {
        const auto state = connection_of(object);
        std::string name;
        { std::lock_guard lock(state->mutex); name = state->name ? text(support, state->name) : std::string("(none)"); }
        unsupported("-[NSXPCConnection " + std::string(what) + "] for " + name + " (no launchd or XPC services under Alchemy)");
    };
    support.method(connection, "resume", [unreachable](CpuState& s, GuestMemory&) { unreachable(s.x[0], "resume"); }, "v@:");
    support.method(connection, "activate", [unreachable](CpuState& s, GuestMemory&) { unreachable(s.x[0], "activate"); }, "v@:");
    support.method(connection, "suspend", [](CpuState&, GuestMemory&) {}, "v@:");   // never resumed: nothing to suspend
    for (const auto* name : {"remoteObjectProxy", "remoteObjectProxyWithErrorHandler:", "synchronousRemoteObjectProxyWithErrorHandler:"})
        support.method(connection, name, [unreachable, name = std::string(name)](CpuState& s, GuestMemory&) { unreachable(s.x[0], name); }, "@@:");
    support.method(connection, "invalidate", [&support, connection_of](CpuState& s, GuestMemory&) {
        const auto state = connection_of(s.x[0]);
        Address handler;
        {
            std::lock_guard lock(state->mutex);
            if (state->invalid) return;
            state->invalid = true;
            handler = std::exchange(state->invalidation, 0);
        }
        if (!handler) return;
        // The handler runs on a global queue, once.
        static_cast<void>(support.call(libsystem, "_dispatch_async", {support.call(libsystem, "_dispatch_get_global_queue", {0, 0}), handler}));
        block_release(support, handler);
    }, "v@:");
    support.method(connection, "processIdentifier", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "i@:");   // no remote process
    support.method(connection, "dealloc", [&support, connection](CpuState& s, GuestMemory&) {
        if (const auto state = support.objc.find_host_state<ConnectionState>(s.x[0])) {
            for (auto* slot : {&state->name, &state->exported_interface, &state->exported_object, &state->remote_interface})
                objc_release(support, std::exchange(*slot, 0));
            block_release(support, std::exchange(state->interruption, 0));
            block_release(support, std::exchange(state->invalidation, 0));
        }
        static_cast<void>(support.objc.send_super(s.x[0], connection, support.objc.selector("dealloc")));
    }, "v@:");

    // ---- NSXPCListener ----
    const auto listener_of = [&support](Address object) { return support.objc.host_state<ListenerState>(object); };
    support.method(listener, "serviceListener", [](CpuState&, GuestMemory&) {
        unsupported("+[NSXPCListener serviceListener] (this process is not an XPC service)");
    }, "@@:", true);
    support.method(listener, "anonymousListener", [&support, listener](CpuState& s, GuestMemory&) {
        s.x[0] = autoreleased(support, support.send_id(support.send_id(listener, "alloc"), "init"));
    }, "@@:", true);
    support.method(listener, "initWithMachServiceName:", [&support, listener_of](CpuState& s, GuestMemory&) {
        listener_of(s.x[0])->name = s.x[2] ? support.send_id(s.x[2], "copy") : 0;
    }, "@@:@");
    support.method(listener, "delegate", [listener_of](CpuState& s, GuestMemory&) { s.x[0] = listener_of(s.x[0])->delegate; }, "@@:");
    support.method(listener, "setDelegate:", [listener_of](CpuState& s, GuestMemory&) { listener_of(s.x[0])->delegate = s.x[2]; }, "v@:@");
    support.method(listener, "resume", [](CpuState&, GuestMemory&) { unsupported("-[NSXPCListener resume] (no launchd or XPC services under Alchemy)"); }, "v@:");
    support.method(listener, "suspend", [](CpuState&, GuestMemory&) {}, "v@:");
    support.method(listener, "invalidate", [](CpuState&, GuestMemory&) {}, "v@:");   // never listening
    support.method(listener, "endpoint", [](CpuState&, GuestMemory&) { unsupported("-[NSXPCListener endpoint]"); }, "@@:");
    support.method(listener, "dealloc", [&support, listener](CpuState& s, GuestMemory&) {
        if (const auto state = support.objc.find_host_state<ListenerState>(s.x[0])) objc_release(support, std::exchange(state->name, 0));
        static_cast<void>(support.objc.send_super(s.x[0], listener, support.objc.selector("dealloc")));
    }, "v@:");
}

} // namespace alchemy::gui::completion_kit
