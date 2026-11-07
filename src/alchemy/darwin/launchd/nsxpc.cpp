#include "alchemy/darwin/kit/completion/runtime/common.hpp"

#include <atomic>
#include <bit>
#include <cctype>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>

// NSXPCInterface, NSXPCConnection and NSXPCListener (Foundation; Sparkle), on this session's C
// XPC connections (darwin/launchd/xpc_connections.cpp): the transport, bundled XPC services
// (initWithServiceName:), launchd job services (initWithMachServiceName:) and peer identity
// are theirs. Both ends are this Foundation, so the message format is Alchemy's own: an XPC
// dictionary {sel, args, reply}, `args` an NSKeyedArchiver archive of a dictionary of the
// arguments by position (nil absent; scalars as NSNumber), and a reply {args} for the reply
// block's arguments.
//
// * A remote object proxy answers the selectors of the remote interface's protocol (required
//   and optional instance methods, incorporated protocols too). Its arguments come from the
//   call's registers by the protocol's types; a reply block (the last block argument) is
//   called with the reply's arguments, or the error handler with an NSError when the
//   connection fails first (NSCocoaErrorDomain 4097 interrupted, 4099 invalid). The
//   synchronous proxy waits for the reply.
// * Exported objects are called through objc_msgSend with the arguments in registers by the
//   exported interface's types; a reply block argument is a block made here that sends its
//   arguments back. +currentConnection is the connection whose message is being handled.
// * Listeners hand each new peer to -listener:shouldAcceptNewConnection:; +serviceListener's
//   -resume serves forever, as xpc_main does.
// * Without XPC connections (alchemy.exe, unit fixtures) a connection keeps its configuration
//   and -resume fails explicitly.
// Not here: unarchiving does not enforce the interface's allowed classes (setClasses: is
// kept, not applied); proxies and listener endpoints as arguments, NSXPCCoder-specific
// coding and NSProgress across XPC are refused with an error.
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;

constexpr std::int64_t interrupted_code = 4097, invalid_code = 4099, reply_invalid_code = 4101;

// ---- Objective-C type encodings ---------------------------------------------------------
struct ArgType {
    enum Kind { void_type, integer, unsigned_integer, boolean, real32, real64, object, block, selector, pointer, other } kind{other};
    std::string signature;   // a block's own signature (extended encodings: @?<...>)
};

std::vector<ArgType> parse_types(std::string_view text) {
    std::vector<ArgType> out;
    std::size_t at = 0;
    const auto skip_digits = [&] { while (at < text.size() && (std::isdigit(static_cast<unsigned char>(text[at])) || text[at] == '-')) ++at; };
    const auto skip_balanced = [&](char open, char close) {
        int depth = 0;
        for (; at < text.size(); ++at) {
            if (text[at] == open) ++depth;
            else if (text[at] == close && --depth == 0) { ++at; return; }
        }
    };
    while (at < text.size()) {
        while (at < text.size() && std::string_view("rnNoORVAj").find(text[at]) != std::string_view::npos) ++at;
        if (at >= text.size()) break;
        ArgType arg;
        const char c = text[at++];
        switch (c) {
        case 'v': arg.kind = ArgType::void_type; break;
        case 'c': case 's': case 'i': case 'l': case 'q': arg.kind = ArgType::integer; break;
        case 'C': case 'S': case 'I': case 'L': case 'Q': arg.kind = ArgType::unsigned_integer; break;
        case 'B': arg.kind = ArgType::boolean; break;
        case 'f': arg.kind = ArgType::real32; break;
        case 'd': arg.kind = ArgType::real64; break;
        case ':': arg.kind = ArgType::selector; break;
        case '#': arg.kind = ArgType::object; break;
        case '*': arg.kind = ArgType::pointer; break;
        case '@':
            arg.kind = ArgType::object;
            if (at < text.size() && text[at] == '?') {
                arg.kind = ArgType::block;
                ++at;
                if (at < text.size() && text[at] == '<') {
                    const auto begin = at + 1;
                    skip_balanced('<', '>');
                    arg.signature = std::string(text.substr(begin, at - 1 - begin));
                }
            } else if (at < text.size() && text[at] == '"') {
                const auto end = text.find('"', at + 1);
                at = end == std::string_view::npos ? text.size() : end + 1;
            }
            break;
        case '^':
            arg.kind = ArgType::pointer;
            if (at < text.size()) {
                if (text[at] == '{') skip_balanced('{', '}');
                else ++at;
            }
            break;
        case '{': --at; skip_balanced('{', '}'); break;
        case '(': --at; skip_balanced('(', ')'); break;
        case '[': --at; skip_balanced('[', ']'); break;
        default: break;
        }
        skip_digits();
        out.push_back(std::move(arg));
    }
    return out;
}

// ---- blocks made here ---------------------------------------------------------------------
// A heap block (as Block_copy makes them: needs-free, one reference) whose invoke runs a host
// function. Block_copy and Block_release count its references; its dispose helper, on the
// last release, drops the host function and runs `dispose`. The maker releases its own
// reference once it has handed the block over.
struct Blocks {
    explicit Blocks(Support& s) : support(s) {}
    Support& support;
    Address entry{}, descriptor{}, isa{};
    struct Entry { std::function<void(CpuState&)> body; std::function<void()> dispose; };
    std::mutex mutex;
    std::map<std::uint64_t, Entry> entries;
    std::uint64_t next{1};

    Address make(std::function<void(CpuState&)> body, std::function<void()> dispose = {}) {
        std::uint64_t id;
        { std::lock_guard lock(mutex); id = next++; entries[id] = {std::move(body), std::move(dispose)}; }
        const auto block = support.process.allocate_guest(48);
        auto& m = support.memory();
        m.write64(block, isa);
        m.write32(block + 8, (1u << 24) | (1u << 25) | 2);   // BLOCK_NEEDS_FREE, BLOCK_HAS_COPY_DISPOSE, one reference
        m.write32(block + 12, 0);
        m.write64(block + 16, entry);
        m.write64(block + 24, descriptor);
        m.write64(block + 32, id);
        return block;
    }
    void release(Address block) { block_release(support, block); }
    void run(CpuState& s, GuestMemory& m) {
        const auto id = m.read64(s.x[0] + 32);
        std::function<void(CpuState&)> body;
        { std::lock_guard lock(mutex); const auto found = entries.find(id); if (found != entries.end()) body = found->second.body; }
        if (body) body(s);
    }
    void dispose(Address block, GuestMemory& m) {
        Entry gone;
        {
            std::lock_guard lock(mutex);
            const auto found = entries.find(m.read64(block + 32));
            if (found == entries.end()) return;
            gone = std::move(found->second);
            entries.erase(found);
        }
        if (gone.dispose) gone.dispose();
    }
};

// ---- state ------------------------------------------------------------------------------------
struct InterfaceState final : ObjectPayload {
    std::mutex mutex;
    Address protocol{};
    std::map<std::tuple<Address, std::uint64_t, bool>, Address> classes, interfaces;   // (selector, index, reply) -> +1 set / interface
};

struct ConnectionState final : ObjectPayload {
    std::mutex mutex;
    Address name{}, exported_interface{}, exported_object{}, remote_interface{};   // +1
    Address interruption{}, invalidation{};                                       // copied blocks
    bool mach{}, invalid{}, resumed{};
    std::uint64_t options{};
    bool gone{};            // deallocated: late events are dropped
    Address xpc{};          // the C connection (+1)
    Address endpoint{};     // initWithListenerEndpoint: the NSXPCListenerEndpoint (+1)
};

struct ListenerState final : ObjectPayload {
    std::mutex mutex;
    Address name{}, delegate{};   // name +1; delegate not retained
    bool service{}, anonymous{}, resumed{}, gone{};
    Address xpc{};
    std::vector<Address> accepted;   // +1: the connections the delegate accepted
};

struct EndpointState final : ObjectPayload { Address xpc{}; };   // the xpc endpoint (+1)

struct ProxyState final : ObjectPayload {
    Address connection{};        // the NSXPCConnection (+1)
    Address error_handler{};     // copied block, or 0
    bool synchronous{};
};

thread_local Address current_connection = 0;

class Nsxpc {
public:
    Nsxpc(Support& s, std::shared_ptr<Blocks> b) : support(s), blocks(std::move(b)) {}
    Support& support;
    std::shared_ptr<Blocks> blocks;
    Address interface_class{}, connection_class{}, listener_class{}, endpoint_class{}, proxy_class{};
    std::mutex mutex;
    std::set<std::string> proxy_selectors;

    Address xpc(const char* name, std::initializer_list<Address> arguments) { return support.call(libsystem, name, arguments); }
    Address symbol(const char* name) { const auto found = support.process.resolve(libsystem, name); return found ? found->address : 0; }
    // XPC connections are installed (alchemy-fx): without them a connection keeps its
    // configuration and connecting fails explicitly.
    bool transport() { return symbol("_xpc_connection_create") != 0; }
    [[noreturn]] void no_transport(std::string_view what) { unsupported(std::string(what) + ": no XPC connection (XPC connections are not installed in this process)"); }
    std::shared_ptr<ConnectionState> connection_of(Address object) { return support.objc.host_state<ConnectionState>(object); }
    std::shared_ptr<InterfaceState> interface_of(Address object) { return object ? support.objc.find_host_state<InterfaceState>(object) : nullptr; }

    // ---- protocols ---------------------------------------------------------------------------
    // The method's types in `protocol` (required, then optional; incorporated protocols too):
    // the extended encoding when there is one.
    std::optional<std::string> method_types(Address protocol, Address selector) {
        if (!protocol) return std::nullopt;
        for (const Address required : {1u, 0u}) {
            const auto types = support.call(libobjc, "__protocol_getMethodTypeEncoding", {protocol, selector, required, 1});
            if (types) return c_string(support.memory(), types, 4096);
        }
        return std::nullopt;
    }
    // Every instance selector of `protocol` and the protocols it incorporates.
    void selectors_of(Address protocol, std::set<Address>& out, int depth = 0) {
        if (!protocol || depth > 16) return;
        auto& m = support.memory();
        for (const bool required : {true, false}) {
            const auto count_cell = support.process.allocate_guest(8);
            m.write32(count_cell, 0);
            const auto list = support.call(libobjc, "_protocol_copyMethodDescriptionList", {protocol, required ? 1u : 0u, 1, count_cell});
            const auto count = m.read32(count_cell);
            for (std::uint32_t i = 0; i < count; ++i) out.insert(m.read64(list + i * 16));
            if (list) support.call(libsystem, "_free", {list});
            support.process.release_guest(count_cell);
        }
        const auto count_cell = support.process.allocate_guest(8);
        m.write32(count_cell, 0);
        const auto list = support.call(libobjc, "_protocol_copyProtocolList", {protocol, count_cell});
        const auto count = m.read32(count_cell);
        for (std::uint32_t i = 0; i < count; ++i) selectors_of(m.read64(list + i * 8), out, depth + 1);
        if (list) support.call(libsystem, "_free", {list});
        support.process.release_guest(count_cell);
    }

    // ---- arguments ----------------------------------------------------------------------------
    // A dictionary {"0": first argument, ...} of the call's arguments (registers by `types`,
    // from argument `first`), archived; the block argument (if any) is returned, not archived.
    struct Packed { Address data{}; Address reply_block{}; std::string reply_types; };
    std::optional<Packed> pack(const std::vector<ArgType>& types, std::size_t first, const CpuState& s, unsigned x_start, std::string& why) {
        const auto dictionary = support.send_id(class_named(support, "NSMutableDictionary"), "dictionary");
        unsigned xi = x_start, vi = 0;
        Packed packed;
        for (std::size_t index = first; index < types.size(); ++index) {
            const auto& type = types[index];
            const auto position = string(support, std::to_string(index - first));
            Address boxed = 0;
            const auto next_x = [&]() -> std::optional<std::uint64_t> { if (xi > 7) return std::nullopt; return s.x[xi++]; };
            switch (type.kind) {
            case ArgType::object: {
                const auto value = next_x();
                if (!value) { why = "more than 8 integer arguments"; return std::nullopt; }
                boxed = *value;
                break;
            }
            case ArgType::integer: case ArgType::unsigned_integer: case ArgType::boolean: {
                const auto value = next_x();
                if (!value) { why = "more than 8 integer arguments"; return std::nullopt; }
                boxed = type.kind == ArgType::boolean ? boolean(support, (*value & 0xff) != 0)
                      : number(support, static_cast<std::int64_t>(*value));
                break;
            }
            case ArgType::real32: case ArgType::real64: {
                if (vi > 7) { why = "more than 8 floating-point arguments"; return std::nullopt; }
                CallRegs registers;
                registers.x[0] = class_named(support, "NSNumber");
                registers.x[1] = support.objc.selector("numberWithDouble:");
                const auto bits = s.v[vi++][0];
                registers.set_d(0, type.kind == ArgType::real32 ? static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(bits))) : std::bit_cast<double>(bits));
                boxed = support.send(registers.x[0], "numberWithDouble:", registers).x0;
                break;
            }
            case ArgType::block: {
                const auto value = next_x();
                if (!value) { why = "more than 8 integer arguments"; return std::nullopt; }
                packed.reply_block = *value;
                packed.reply_types = type.signature;
                continue;
            }
            default:
                why = "an argument type XPC cannot carry";
                return std::nullopt;
            }
            if (boxed) support.send_id(dictionary, "setObject:forKey:", {boxed, position});
        }
        packed.data = support.send_id(class_named(support, "NSKeyedArchiver"), "archivedDataWithRootObject:", {dictionary});
        if (!packed.data) { why = "the arguments could not be archived"; return std::nullopt; }
        return packed;
    }

    // Registers for a call with the archived arguments (`types` from `first`); objects are +0.
    bool unpack(Address data, const std::vector<ArgType>& types, std::size_t first, CallRegs& registers, unsigned x_start,
                const std::function<Address(const ArgType&)>& block_for) {
        const auto dictionary = data ? support.send_id(class_named(support, "NSKeyedUnarchiver"), "unarchiveObjectWithData:", {data}) : 0;
        unsigned xi = x_start, vi = 0;
        for (std::size_t index = first; index < types.size(); ++index) {
            const auto& type = types[index];
            const auto value = dictionary ? support.send_id(dictionary, "objectForKey:", {string(support, std::to_string(index - first))}) : 0;
            switch (type.kind) {
            case ArgType::object: if (xi > 7) return false; registers.x[xi++] = value; break;
            case ArgType::integer: case ArgType::unsigned_integer:
                if (xi > 7) return false;
                registers.x[xi++] = value ? support.send(value, "longLongValue").x0 : 0;
                break;
            case ArgType::boolean:
                if (xi > 7) return false;
                registers.x[xi++] = value ? (support.send(value, "boolValue").x0 & 0xff) : 0;
                break;
            case ArgType::real32: case ArgType::real64: {
                if (vi > 7) return false;
                const double real = value ? std::bit_cast<double>(support.send(value, "doubleValue").v[0][0]) : 0.0;
                if (type.kind == ArgType::real32) registers.set_s(vi++, static_cast<float>(real)); else registers.set_d(vi++, real);
                break;
            }
            case ArgType::block: if (xi > 7) return false; registers.x[xi++] = block_for(type); break;
            default: return false;
            }
        }
        return true;
    }

    Address nserror(std::int64_t code, const std::string& description) { return error(support, "NSCocoaErrorDomain", code, description); }

    // ---- sending ----------------------------------------------------------------------------
    // Calls the reply block with the reply's arguments, or the error handler with an error.
    void deliver_reply(Address reply, Address user_block, const std::string& block_types, Address error_handler) {
        const auto is_error = xpc("_xpc_get_type", {reply}) == symbol("__xpc_type_error");
        if (is_error || !user_block) {
            if (is_error && error_handler) {
                const auto code = reply == symbol("__xpc_error_connection_interrupted") ? interrupted_code : invalid_code;
                static_cast<void>(invoke_block(support, error_handler, {nserror(code, code == interrupted_code ? "The connection was interrupted." : "The connection was invalidated.")}));
            }
            return;
        }
        auto types = parse_types(block_types);   // return, block itself, arguments
        CallRegs registers;
        registers.x[0] = user_block;
        const auto length_cell = support.process.allocate_guest(8);
        const auto key = support.process.allocate_guest(8);
        put_bytes(key, "args");
        const auto bytes = xpc("_xpc_dictionary_get_data", {reply, key, length_cell});
        const auto length = support.memory().read64(length_cell);
        support.process.release_guest(length_cell);
        support.process.release_guest(key);
        const auto data = bytes ? support.send_id(class_named(support, "NSData"), "dataWithBytes:length:", {bytes, length}) : 0;
        if (types.size() < 2 || !unpack(data, types, 2, registers, 1, [](const ArgType&) { return Address{0}; })) {
            if (error_handler) static_cast<void>(invoke_block(support, error_handler, {nserror(reply_invalid_code, "The reply could not be read.")}));
            return;
        }
        static_cast<void>(invoke_block(support, user_block, registers));
    }

    void put_bytes(Address at, std::string_view text) {
        std::vector<std::uint8_t> bytes(text.begin(), text.end());
        bytes.push_back(0);
        support.memory().write_bytes(at, bytes);
    }
    Address c_text(std::string_view text) {
        const auto at = support.process.allocate_guest(text.size() + 1);
        put_bytes(at, text);
        return at;
    }

    // A proxy method: archive the arguments, send, and handle the reply.
    void forward(Address proxy, Address selector, const CpuState& s) {
        const auto state = support.objc.find_host_state<ProxyState>(proxy);
        if (!state) fail("NSXPC proxy without its connection");
        const auto connection = connection_of(state->connection);
        Address remote_interface, xpc_connection;
        { std::lock_guard lock(connection->mutex); remote_interface = connection->remote_interface; xpc_connection = connection->xpc; }
        const auto interface = interface_of(remote_interface);
        Address protocol = 0;
        if (interface) { std::lock_guard lock(interface->mutex); protocol = interface->protocol; }
        const auto name = selector_name(support, selector);
        const auto types_text = method_types(protocol, selector);
        std::string why;
        std::optional<Packed> packed;
        if (!types_text) why = "the remote interface has no method " + name;
        else packed = pack(parse_types(*types_text), 3, s, 2, why);
        if (!packed || !xpc_connection) {
            if (state->error_handler) static_cast<void>(invoke_block(support, state->error_handler, {nserror(invalid_code, why.empty() ? "The connection is not set up." : why)}));
            return;
        }
        const auto message = xpc("_xpc_dictionary_create", {0, 0, 0});
        const auto sel_key = c_text("sel"), args_key = c_text("args"), sel_value = c_text(name);
        xpc("_xpc_dictionary_set_string", {message, sel_key, sel_value});
        const auto bytes = support.send_id(packed->data, "bytes");
        const auto length = support.send(packed->data, "length").x0;
        xpc("_xpc_dictionary_set_data", {message, args_key, bytes, length});
        if (packed->reply_block) {
            const auto reply_key = c_text("reply");
            xpc("_xpc_dictionary_set_bool", {message, reply_key, 1});
            support.process.release_guest(reply_key);
        }
        support.process.release_guest(sel_key);
        support.process.release_guest(args_key);
        support.process.release_guest(sel_value);
        if (!packed->reply_block) {
            xpc("_xpc_connection_send_message", {xpc_connection, message});
        } else if (state->synchronous) {
            const auto reply = xpc("_xpc_connection_send_message_with_reply_sync", {xpc_connection, message});
            deliver_reply(reply, packed->reply_block, packed->reply_types, state->error_handler);
            xpc("_xpc_release", {reply});
        } else {
            const auto user_block = block_copy(support, packed->reply_block);
            const auto error_handler = state->error_handler ? block_copy(support, state->error_handler) : 0;
            const auto reply_types = packed->reply_types;
            const auto self = this;
            const auto reply_block = blocks->make([self, user_block, error_handler, reply_types](CpuState& r) {
                self->deliver_reply(r.x[1], user_block, reply_types, error_handler);
            }, [self, user_block, error_handler] {
                block_release(self->support, user_block);
                if (error_handler) block_release(self->support, error_handler);
            });
            xpc("_xpc_connection_send_message_with_reply", {xpc_connection, message, 0, reply_block});
            blocks->release(reply_block);
        }
        xpc("_xpc_release", {message});
    }

    // The proxy class answers the selectors of `protocol`.
    void prepare_proxy(Address protocol) {
        std::set<Address> selectors;
        selectors_of(protocol, selectors);
        for (const auto selector : selectors) {
            const auto name = selector_name(support, selector);
            {
                std::lock_guard lock(mutex);
                if (!proxy_selectors.insert(name).second) continue;
            }
            support.method(proxy_class, name, [this, selector](CpuState& s, GuestMemory&) {
                forward(s.x[0], selector, s);
                s.x[0] = 0;
                s.x[1] = 0;
            }, "v@:");
        }
    }

    Address make_proxy(Address connection, Address error_handler, bool synchronous) {
        const auto state = connection_of(connection);
        Address remote_interface;
        { std::lock_guard lock(state->mutex); remote_interface = state->remote_interface; }
        const auto interface = interface_of(remote_interface);
        if (interface) { Address protocol; { std::lock_guard lock(interface->mutex); protocol = interface->protocol; } prepare_proxy(protocol); }
        auto proxy = std::make_shared<ProxyState>();
        proxy->connection = objc_retain(support, connection);
        proxy->error_handler = error_handler ? block_copy(support, error_handler) : 0;
        proxy->synchronous = synchronous;
        return autoreleased(support, make_object(support, proxy_class, proxy));
    }

    // ---- receiving ----------------------------------------------------------------------------
    // A message for the exported object.
    void receive(Address connection, Address message) {
        const auto state = connection_of(connection);
        Address exported_interface, exported_object, xpc_connection;
        { std::lock_guard lock(state->mutex); exported_interface = state->exported_interface; exported_object = state->exported_object; xpc_connection = state->xpc; }
        auto& m = support.memory();
        const auto sel_key = c_text("sel"), args_key = c_text("args"), reply_key = c_text("reply");
        const auto name_text = xpc("_xpc_dictionary_get_string", {message, sel_key});
        const auto length_cell = support.process.allocate_guest(8);
        const auto bytes = xpc("_xpc_dictionary_get_data", {message, args_key, length_cell});
        const auto length = m.read64(length_cell);
        const bool wants_reply = xpc("_xpc_dictionary_get_bool", {message, reply_key}) != 0;
        for (const auto at : {sel_key, args_key, reply_key, length_cell}) support.process.release_guest(at);
        if (!name_text || !exported_object) return;
        const auto selector = support.objc.selector(c_string(m, name_text, 1024));
        const auto interface = interface_of(exported_interface);
        Address protocol = 0;
        if (interface) { std::lock_guard lock(interface->mutex); protocol = interface->protocol; }
        const auto types_text = method_types(protocol, selector);
        if (!types_text) return;   // not in the exported interface: ignored, as Foundation does
        const auto types = parse_types(*types_text);
        const auto data = bytes ? support.send_id(class_named(support, "NSData"), "dataWithBytes:length:", {bytes, length}) : 0;
        CallRegs registers;
        registers.x[0] = exported_object;
        registers.x[1] = selector;
        // A reply block answers the message once; the message is kept until its blocks are gone.
        struct Pending {
            Nsxpc* self;
            Address message;
            std::atomic<bool> answered{};
            Pending(Nsxpc* owner, Address kept) : self(owner), message(kept) {}
            ~Pending() { self->xpc("_xpc_release", {message}); }
        };
        const auto pending = wants_reply ? std::make_shared<Pending>(this, xpc("_xpc_retain", {message})) : nullptr;
        std::vector<Address> made;
        const auto self = this;
        const bool ok = unpack(data, types, 3, registers, 2, [&](const ArgType& type) -> Address {
            if (!pending) return 0;
            const auto block_types = type.signature;
            const auto reply_block = blocks->make([self, block_types, pending](CpuState& r) {
                // The reply block's arguments, archived, back on the message's reply right.
                if (pending->answered.exchange(true)) return;
                auto parsed = parse_types(block_types);
                std::string why;
                const auto packed = parsed.size() >= 2 ? self->pack(parsed, 2, r, 1, why) : std::nullopt;
                const auto reply = self->xpc("_xpc_dictionary_create_reply", {pending->message});
                if (!reply) return;
                if (packed) {
                    const auto key = self->c_text("args");
                    const auto reply_bytes = self->support.send_id(packed->data, "bytes");
                    const auto reply_length = self->support.send(packed->data, "length").x0;
                    self->xpc("_xpc_dictionary_set_data", {reply, key, reply_bytes, reply_length});
                    self->support.process.release_guest(key);
                }
                self->xpc("_xpc_connection_send_message", {self->xpc("_xpc_dictionary_get_remote_connection", {pending->message}), reply});
                self->xpc("_xpc_release", {reply});
            }, [pending] {});
            made.push_back(reply_block);
            return reply_block;
        });
        if (ok) {
            const auto previous = std::exchange(current_connection, connection);
            static_cast<void>(support.send(exported_object, selector_name(support, selector), registers));
            current_connection = previous;
        }
        for (const auto block : made) blocks->release(block);   // the method copied the ones it keeps
    }

    // The C connection's events: messages, and the errors that end or interrupt it. Late
    // events of a deallocated connection are dropped.
    Address events_block(Address connection) {
        const auto self = this;
        const std::weak_ptr<ConnectionState> weak = connection_of(connection);
        return blocks->make([self, connection, weak](CpuState& s) {
            const auto state = weak.lock();
            if (!state) return;
            { std::lock_guard lock(state->mutex); if (state->gone) return; }
            const auto event = s.x[1];
            const auto type = self->xpc("_xpc_get_type", {event});
            if (type == self->symbol("__xpc_type_dictionary")) { self->receive(connection, event); return; }
            if (type != self->symbol("__xpc_type_error")) return;
            Address handler = 0;
            {
                std::lock_guard lock(state->mutex);
                if (event == self->symbol("__xpc_error_connection_interrupted")) handler = state->interruption ? block_copy(self->support, state->interruption) : 0;
                else if (!state->invalid) { state->invalid = true; handler = std::exchange(state->invalidation, 0); }
            }
            if (handler) {
                static_cast<void>(invoke_block(self->support, handler, {}));
                block_release(self->support, handler);
            }
        });
    }

    void resume(Address connection) {
        const auto state = connection_of(connection);
        if (!transport()) no_transport("-[NSXPCConnection resume]");
        Address xpc_connection;
        {
            std::lock_guard lock(state->mutex);
            if (state->resumed || !state->xpc) return;
            state->resumed = true;
            xpc_connection = state->xpc;
        }
        const auto events = events_block(connection);
        xpc("_xpc_connection_set_event_handler", {xpc_connection, events});   // copied: XPC keeps it
        blocks->release(events);
        xpc("_xpc_connection_resume", {xpc_connection});
    }
};

} // namespace

void install_xpc(Support& support) {
    const auto blocks = std::make_shared<Blocks>(support);
    blocks->entry = support.process.install_function("__alchemy_nsxpc_block", [blocks](CpuState& s, GuestMemory& m) {
        blocks->run(s, m);
        s.x[0] = 0;
    });
    // {reserved, size, copy helper, dispose helper}: the blocks are made on the heap, so the
    // copy helper (stack to heap) never runs; dispose drops the host function.
    const auto copy_helper = support.process.install_function("__alchemy_nsxpc_block_copy", [](CpuState&, GuestMemory&) {});
    const auto dispose_helper = support.process.install_function("__alchemy_nsxpc_block_dispose", [blocks](CpuState& s, GuestMemory& m) {
        blocks->dispose(s.x[0], m);
    });
    blocks->descriptor = support.process.allocate_guest(32);
    support.memory().write64(blocks->descriptor, 0);
    support.memory().write64(blocks->descriptor + 8, 48);
    support.memory().write64(blocks->descriptor + 16, copy_helper);
    support.memory().write64(blocks->descriptor + 24, dispose_helper);
    if (const auto found = support.process.resolve(libsystem, "__NSConcreteMallocBlock")) blocks->isa = found->address;
    const auto self = std::make_shared<Nsxpc>(support, blocks);

    const auto interface = self->interface_class = support.declare_class("NSXPCInterface", "NSObject", foundation_library);
    const auto connection = self->connection_class = support.declare_class("NSXPCConnection", "NSObject", foundation_library);
    const auto listener = self->listener_class = support.declare_class("NSXPCListener", "NSObject", foundation_library);
    const auto endpoint = self->endpoint_class = support.objc.find_class("NSXPCListenerEndpoint")
        ? support.objc.find_class("NSXPCListenerEndpoint") : support.declare_class("NSXPCListenerEndpoint", "NSObject", foundation_library);
    self->proxy_class = support.declare_class("_NSXPCDistantObject", "NSObject", foundation_library);

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
    support.method(connection, "initWithServiceName:", [&support, self, connection_of](CpuState& s, GuestMemory& m) {
        const auto state = connection_of(s.x[0]);
        state->name = s.x[2] ? support.send_id(s.x[2], "copy") : 0;
        const auto name = s.x[2] ? self->c_text(text(support, s.x[2])) : 0;
        state->xpc = name && self->transport() ? self->xpc("_xpc_connection_create", {name, 0}) : 0;   // a bundled XPC service
        if (name) support.process.release_guest(name);
        static_cast<void>(m);
    }, "@@:@");
    support.method(connection, "initWithMachServiceName:options:", [&support, self, connection_of](CpuState& s, GuestMemory&) {
        const auto state = connection_of(s.x[0]);
        state->name = s.x[2] ? support.send_id(s.x[2], "copy") : 0;
        state->mach = true;
        state->options = s.x[3];
        const auto name = s.x[2] ? self->c_text(text(support, s.x[2])) : 0;
        // NSXPCConnectionPrivileged (1 << 12): the system domain, which XPC refuses.
        state->xpc = name && self->transport() ? self->xpc("_xpc_connection_create_mach_service", {name, 0, (s.x[3] & (1u << 12)) ? 2u : 0u}) : 0;
        if (name) support.process.release_guest(name);
    }, "@@:@Q");
    support.method(connection, "initWithListenerEndpoint:", [&support, self, connection_of](CpuState& s, GuestMemory&) {
        const auto state = connection_of(s.x[0]);
        const auto endpoint_state = s.x[2] ? support.objc.find_host_state<EndpointState>(s.x[2]) : nullptr;
        state->endpoint = objc_retain(support, s.x[2]);
        state->xpc = endpoint_state && endpoint_state->xpc ? self->xpc("_xpc_connection_create_from_endpoint", {endpoint_state->xpc}) : 0;
    }, "@@:@");
    support.method(connection, "serviceName", [connection_of](CpuState& s, GuestMemory&) {
        const auto state = connection_of(s.x[0]);
        s.x[0] = state->mach ? 0 : state->name;
    }, "@@:");
    support.method(connection, "endpoint", [&support, self, connection_of](CpuState& s, GuestMemory&) {
        // A connection's endpoint names the same service (a new connection to it).
        const auto state = connection_of(s.x[0]);
        const auto xpc_endpoint = state->xpc ? self->xpc("_xpc_endpoint_create", {state->xpc}) : 0;
        if (!xpc_endpoint) { s.x[0] = 0; return; }
        auto made = std::make_shared<EndpointState>();
        made->xpc = xpc_endpoint;
        s.x[0] = autoreleased(support, make_object(support, self->endpoint_class, made));
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
    for (const auto* name : {"resume", "activate"})
        support.method(connection, name, [self](CpuState& s, GuestMemory&) { self->resume(s.x[0]); }, "v@:");
    support.method(connection, "suspend", [self, connection_of](CpuState& s, GuestMemory&) {
        const auto state = connection_of(s.x[0]);
        if (state->xpc) self->xpc("_xpc_connection_suspend", {state->xpc});
    }, "v@:");
    support.method(connection, "remoteObjectProxy", [self](CpuState& s, GuestMemory&) { s.x[0] = self->make_proxy(s.x[0], 0, false); }, "@@:");
    support.method(connection, "remoteObjectProxyWithErrorHandler:", [self](CpuState& s, GuestMemory&) { s.x[0] = self->make_proxy(s.x[0], s.x[2], false); }, "@@:@?");
    support.method(connection, "synchronousRemoteObjectProxyWithErrorHandler:", [self](CpuState& s, GuestMemory&) { s.x[0] = self->make_proxy(s.x[0], s.x[2], true); }, "@@:@?");
    support.method(connection, "invalidate", [&support, self, connection_of](CpuState& s, GuestMemory&) {
        const auto state = connection_of(s.x[0]);
        Address xpc_connection, handler{};
        bool resumed;
        {
            std::lock_guard lock(state->mutex);
            xpc_connection = state->xpc;
            resumed = state->resumed;
            if (!resumed && !state->invalid) { state->invalid = true; handler = std::exchange(state->invalidation, 0); }
        }
        // A resumed connection's invalidation handler runs from the XPC error; an unresumed one now, on a global queue.
        if (xpc_connection && resumed) self->xpc("_xpc_connection_cancel", {xpc_connection});
        if (handler) {
            static_cast<void>(support.call(libsystem, "_dispatch_async", {support.call(libsystem, "_dispatch_get_global_queue", {0, 0}), handler}));
            block_release(support, handler);
        }
    }, "v@:");
    const std::array<std::pair<const char*, const char*>, 4> identity{{
        {"processIdentifier", "_xpc_connection_get_pid"}, {"effectiveUserIdentifier", "_xpc_connection_get_euid"},
        {"effectiveGroupIdentifier", "_xpc_connection_get_egid"}, {"auditSessionIdentifier", "_xpc_connection_get_asid"}}};
    for (const auto& [name, call] : identity)
        support.method(connection, name, [self, connection_of, call = call](CpuState& s, GuestMemory&) {
            const auto state = connection_of(s.x[0]);
            s.x[0] = state->xpc ? static_cast<std::uint32_t>(self->xpc(call, {state->xpc})) : 0;
        }, "i@:");
    support.method(connection, "currentConnection", [](CpuState& s, GuestMemory&) { s.x[0] = current_connection; }, "@@:", true);
    support.method(connection, "dealloc", [&support, self, connection](CpuState& s, GuestMemory&) {
        if (const auto state = support.objc.find_host_state<ConnectionState>(s.x[0])) {
            for (auto* slot : {&state->name, &state->exported_interface, &state->exported_object, &state->remote_interface, &state->endpoint})
                objc_release(support, std::exchange(*slot, 0));
            block_release(support, std::exchange(state->interruption, 0));
            block_release(support, std::exchange(state->invalidation, 0));
            if (const auto xpc_connection = std::exchange(state->xpc, 0)) {
                if (state->resumed) self->xpc("_xpc_connection_cancel", {xpc_connection});
                self->xpc("_xpc_release", {xpc_connection});
            }
            std::lock_guard lock(state->mutex);
            state->gone = true;   // the C connection may still deliver its last error to the events block
        }
        static_cast<void>(support.objc.send_super(s.x[0], connection, support.objc.selector("dealloc")));
    }, "v@:");

    // ---- _NSXPCDistantObject (remote object proxies) ----
    support.method(self->proxy_class, "dealloc", [&support, self](CpuState& s, GuestMemory&) {
        if (const auto state = support.objc.find_host_state<ProxyState>(s.x[0])) {
            objc_release(support, std::exchange(state->connection, 0));
            block_release(support, std::exchange(state->error_handler, 0));
        }
        static_cast<void>(support.objc.send_super(s.x[0], self->proxy_class, support.objc.selector("dealloc")));
    }, "v@:");

    // ---- NSXPCListenerEndpoint ----
    support.method(endpoint, "dealloc", [&support, self, endpoint](CpuState& s, GuestMemory&) {
        if (const auto state = support.objc.find_host_state<EndpointState>(s.x[0]))
            if (const auto xpc_endpoint = std::exchange(state->xpc, 0)) self->xpc("_xpc_release", {xpc_endpoint});
        static_cast<void>(support.objc.send_super(s.x[0], endpoint, support.objc.selector("dealloc")));
    }, "v@:");

    // ---- NSXPCListener ----
    const auto listener_of = [&support](Address object) { return support.objc.host_state<ListenerState>(object); };
    support.method(listener, "serviceListener", [&support, listener, listener_of](CpuState& s, GuestMemory&) {
        static Address shared = 0;
        if (!shared) {
            shared = support.send_id(support.send_id(listener, "alloc"), "init");
            listener_of(shared)->service = true;
        }
        s.x[0] = shared;
    }, "@@:", true);
    support.method(listener, "anonymousListener", [&support, self, listener, listener_of](CpuState& s, GuestMemory&) {
        const auto object = support.send_id(support.send_id(listener, "alloc"), "init");
        const auto state = listener_of(object);
        state->anonymous = true;
        if (self->transport()) state->xpc = self->xpc("_xpc_connection_create", {0, 0});
        s.x[0] = autoreleased(support, object);
    }, "@@:", true);
    support.method(listener, "initWithMachServiceName:", [&support, listener_of](CpuState& s, GuestMemory&) {
        listener_of(s.x[0])->name = s.x[2] ? support.send_id(s.x[2], "copy") : 0;
    }, "@@:@");
    support.method(listener, "delegate", [listener_of](CpuState& s, GuestMemory&) { s.x[0] = listener_of(s.x[0])->delegate; }, "@@:");
    support.method(listener, "setDelegate:", [listener_of](CpuState& s, GuestMemory&) { listener_of(s.x[0])->delegate = s.x[2]; }, "v@:@");
    support.method(listener, "resume", [&support, self, listener_of](CpuState& s, GuestMemory&) {
        const auto object = s.x[0];
        const auto state = listener_of(object);
        if (!self->transport()) self->no_transport("-[NSXPCListener resume]");
        if (!state->xpc) {
            std::string name;
            if (state->service) {
                const auto key = self->c_text("XPC_SERVICE_NAME");
                const auto value = support.call(libsystem, "_getenv", {key});
                support.process.release_guest(key);
                if (!value) unsupported("+[NSXPCListener serviceListener] -resume: this process was not started as an XPC service");
                name = c_string(support.memory(), value, 1024);
            } else if (state->name) {
                name = text(support, state->name);
            }
            if (name.empty()) unsupported("-[NSXPCListener resume]: a listener without a service name");
            const auto text_name = self->c_text(name);
            state->xpc = self->xpc("_xpc_connection_create_mach_service", {text_name, 0, 1 /* XPC_CONNECTION_MACH_SERVICE_LISTENER */});
            support.process.release_guest(text_name);
        }
        {
            std::lock_guard lock(state->mutex);
            if (std::exchange(state->resumed, true)) return;
        }
        const std::weak_ptr<ListenerState> weak = state;
        const auto events = self->blocks->make([self, object, weak](CpuState& e) {
                const auto listener_state = weak.lock();
                if (!listener_state) return;
                { std::lock_guard lock(listener_state->mutex); if (listener_state->gone) return; }
                const auto peer = e.x[1];
                if (self->xpc("_xpc_get_type", {peer}) != self->symbol("__xpc_type_connection")) return;
                const auto incoming = self->support.send_id(self->support.send_id(self->connection_class, "alloc"), "init");
                self->connection_of(incoming)->xpc = self->xpc("_xpc_retain", {peer});
                const auto delegate = listener_state->delegate;
                const bool accept = delegate &&
                    (self->support.send(delegate, "listener:shouldAcceptNewConnection:", [&] { CallRegs r; r.x[2] = object; r.x[3] = incoming; return r; }()).x0 & 0xff) != 0;
                if (!accept) {
                    self->xpc("_xpc_connection_cancel", {peer});
                    objc_release(self->support, incoming);
                    return;
                }
                std::lock_guard lock(listener_state->mutex);
                listener_state->accepted.push_back(incoming);   // kept for the listener's life, as XPC keeps an accepted connection
            });
        self->xpc("_xpc_connection_set_event_handler", {state->xpc, events});   // copied: XPC keeps it
        self->blocks->release(events);
        self->xpc("_xpc_connection_resume", {state->xpc});
        // The service listener serves until the process ends (xpc_main does the same).
        if (state->service) static_cast<void>(support.call(libsystem, "_dispatch_main", {}));
    }, "v@:");
    support.method(listener, "suspend", [self, listener_of](CpuState& s, GuestMemory&) {
        const auto state = listener_of(s.x[0]);
        if (state->xpc) self->xpc("_xpc_connection_suspend", {state->xpc});
    }, "v@:");
    support.method(listener, "invalidate", [self, listener_of](CpuState& s, GuestMemory&) {
        const auto state = listener_of(s.x[0]);
        if (state->xpc) self->xpc("_xpc_connection_cancel", {state->xpc});
    }, "v@:");
    support.method(listener, "endpoint", [&support, self, listener_of](CpuState& s, GuestMemory&) {
        const auto state = listener_of(s.x[0]);
        if (!state->anonymous || !state->xpc) unsupported("-[NSXPCListener endpoint]: only an anonymous listener has an endpoint");
        auto made = std::make_shared<EndpointState>();
        made->xpc = self->xpc("_xpc_endpoint_create", {state->xpc});
        s.x[0] = autoreleased(support, make_object(support, self->endpoint_class, made));
    }, "@@:");
    support.method(listener, "dealloc", [&support, self, listener](CpuState& s, GuestMemory&) {
        if (const auto state = support.objc.find_host_state<ListenerState>(s.x[0])) {
            objc_release(support, std::exchange(state->name, 0));
            for (const auto accepted : std::exchange(state->accepted, {})) objc_release(support, accepted);
            if (const auto xpc_connection = std::exchange(state->xpc, 0)) {
                self->xpc("_xpc_connection_cancel", {xpc_connection});
                self->xpc("_xpc_release", {xpc_connection});
            }
            std::lock_guard lock(state->mutex);
            state->gone = true;
        }
        static_cast<void>(support.objc.send_super(s.x[0], listener, support.objc.selector("dealloc")));
    }, "v@:");
}

} // namespace alchemy::gui::completion_kit
