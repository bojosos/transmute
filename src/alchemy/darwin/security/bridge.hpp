#pragma once

// Guest bridge helpers shared by the Security (src/alchemy/security) and
// networking (src/alchemy/network) providers. Not a contract header: other
// subsystems reach these providers only through their exported symbols and
// Objective-C messages.
//
// Every function that calls guest code (retain/release, CF constructors,
// blocks, dispatch) must run on a guest thread inside a synthetic call; the
// host-only helpers (text conversion, DER) may run anywhere.

#include "alchemy/core_foundation.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/gui/install.hpp"
#include "alchemy/objc_runtime.hpp"
#include "alchemy/process_context.hpp"

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace alchemy::gui::security {

using Address = std::uint64_t;
using transmute::CpuState;
using transmute::GuestMemory;
using Bytes = std::vector<std::uint8_t>;

inline constexpr std::string_view security_library = "/System/Library/Frameworks/Security.framework/Versions/A/Security";
inline constexpr std::string_view cfnetwork_library =
    "/System/Library/Frameworks/CFNetwork.framework/Versions/A/CFNetwork";
inline constexpr std::string_view libsystem_library = "/usr/lib/libSystem.B.dylib";

[[noreturn]] void fail(std::string_view why);
[[noreturn]] void unsupported(std::string_view what);

// Host text helpers (wide: UTF-16 where wchar_t has 16 bits).
[[nodiscard]] std::wstring wide(std::string_view utf8);
[[nodiscard]] std::string narrow(std::wstring_view text);
[[nodiscard]] std::string lower(std::string_view text);
[[nodiscard]] bool iequals(std::string_view left, std::string_view right);
[[nodiscard]] std::string host_environment(const char* name);   // "" when unset

// CFTypeID values of the Security and networking CF types (CFGetTypeID is
// extended for them; see Bridge::register_type_id).
enum class TypeId : std::uint64_t {
    error = 0x400, certificate, trust, policy, key, identity, access, static_code, code, requirement,
    ssl_context, http_message, transform, keychain, keychain_item, task, socket
};

// A host-implemented guest block: invoke runs `handler` with the block's
// register image (x0 = the block itself, arguments from x1).
using BlockHandler = std::function<void(CpuState&, GuestMemory&)>;

class Bridge : public std::enable_shared_from_this<Bridge> {
public:
    Bridge(ProcessContext& process, ObjcRuntime& objc, CoreFoundation& cf) : process(process), objc(objc), cf(cf) {}
    ProcessContext& process;
    ObjcRuntime& objc;
    CoreFoundation& cf;

    // ---- guest calls -------------------------------------------------------------
    [[nodiscard]] GuestMemory& memory() const { return process.calling_engine(); }
    Address symbol(std::string_view library, std::string_view name) const;
    Address call(std::string_view library, std::string_view name, std::initializer_list<Address> arguments);
    Address call_address(Address function, std::span<const Address> arguments);
    Address retain(Address object);
    void release(Address object);
    Address autorelease(Address object);
    Address block_copy(Address block);
    void block_release(Address block);
    // block->invoke(block, arguments...) (integer/pointer arguments only).
    Address invoke_block(Address block, std::initializer_list<Address> arguments);
    // A guest block whose invoke is a host handler. The block is a global
    // block (copy/release are no-ops) that lives until the process ends; the
    // handler is dropped once `forget_block` is called (later invocations fail
    // explicitly).
    Address make_block(BlockHandler handler);
    void forget_block(Address block);

    // ---- dispatch ----------------------------------------------------------------
    // dispatch_async_f(queue, work) from a guest thread; `work` runs on the
    // queue's guest thread.
    void dispatch_async(Address queue, std::function<void()> work);
    [[nodiscard]] Address main_queue() const;                  // &_dispatch_main_q
    Address create_serial_queue(std::string_view label);      // +1 dispatch queue

    // ---- CF / Foundation values (+1 results) ---------------------------------------
    Address string(std::string_view utf8);
    [[nodiscard]] std::string text(Address string_object);     // fails unless a CFString
    [[nodiscard]] std::optional<std::string> maybe_text(Address object);
    Address data(std::span<const std::uint8_t> bytes);
    [[nodiscard]] Bytes bytes(Address data_object);              // fails unless a CFData
    [[nodiscard]] bool is_data(Address object);
    [[nodiscard]] bool is_string(Address object);
    [[nodiscard]] bool is_dictionary(Address object);
    [[nodiscard]] bool is_array(Address object);
    Address number(std::int64_t value);
    [[nodiscard]] std::optional<std::int64_t> integer(Address number_object);
    [[nodiscard]] std::optional<double> real(Address number_object);
    // CFDictionaryGetValue (no retain); 0 for a null dictionary.
    [[nodiscard]] Address lookup(Address dictionary, Address key);
    // Dictionary or array with host values (+1, values are retained by the container).
    Address dictionary(const std::vector<std::pair<Address, Address>>& entries);
    Address array(const std::vector<Address>& values);
    void release_all(const std::vector<Address>& objects);

    // ---- classes and exports -------------------------------------------------------
    // The class `name` (registered when missing) published under `library`.
    // `where` (here and below) attributes the registration for the registration
    // audit (alchemy/registration_audit.hpp); callers never pass it.
    Address define_class(std::string name, Address superclass, std::string_view library, const std::source_location& where = std::source_location::current());
    void method(Address cls, std::string_view selector, transmute::GuestCallHandler handler,
                bool class_method = false, std::string types = "@@:", const std::source_location& where = std::source_location::current());
    void function(std::string_view library, std::string_view symbol, transmute::GuestCallHandler handler, const std::source_location& where = std::source_location::current());
    // A CFString constant exported as data (`const CFStringRef symbol`).
    Address export_string(std::string_view library, std::string_view symbol, std::string_view value, const std::source_location& where = std::source_location::current());
    void export_value(std::string_view library, std::string_view symbol, std::uint64_t value, const std::source_location& where = std::source_location::current());
    // The value of a CFString data constant `symbol` (exported by any provider).
    [[nodiscard]] Address constant(std::string_view library, std::string_view symbol) const;
    [[nodiscard]] bool exported(std::string_view library, std::string_view symbol) const;

    // CFGetTypeID answers `id` for instances of `cls` (the first call wraps the
    // current CFGetTypeID export; other objects are forwarded to it).
    void register_type_id(Address cls, TypeId id);
    [[nodiscard]] std::optional<TypeId> classify(Address object) const;

    // ---- errors (security/errors.cpp) --------------------------------------------------
    // +1 NSError/CFError. `user_info` values are retained by the error.
    Address error(std::string_view domain, std::int64_t code, std::string_view description,
                  const std::vector<std::pair<std::string, Address>>& user_info = {});
    Address os_status_error(std::int32_t status, std::string_view description);
    // Writes an error to an optional `CFErrorRef *` / `NSError **` out parameter
    // (autoreleased for NSError conventions when `autoreleased`).
    void store_error(Address out, Address error, bool autoreleased);

    struct HostBlock { BlockHandler handler; };
    mutable std::mutex mutex;
    std::map<Address, std::shared_ptr<HostBlock>> blocks;
    std::map<std::uint64_t, std::function<void()>> work;
    std::uint64_t next_work{1};
    Address block_invoke{}, block_descriptor{}, work_function{};
    std::map<Address, TypeId> type_ids;
    bool type_id_extended{};                 // CoreFoundation's CFGetTypeID consults classify()
    Address error_class{};                    // NSError (security/errors.cpp)
    Address object_class{};                   // NSObject
};

// The process bridge, created by install_security (which runs before the
// network installer). Throws before installation.
[[nodiscard]] std::shared_ptr<Bridge> bridge();
std::shared_ptr<Bridge> create_bridge(InstallContext& context);

// ---- DER (host only) ----------------------------------------------------------------------
namespace der {
struct Node {
    std::uint8_t tag{};
    std::span<const std::uint8_t> value;      // contents
    std::span<const std::uint8_t> whole;      // tag + length + contents
};
// Parses one element at the start of `input`; nullopt when malformed.
[[nodiscard]] std::optional<Node> read(std::span<const std::uint8_t> input);
// The children of a constructed element; nullopt when malformed.
[[nodiscard]] std::optional<std::vector<Node>> children(const Node& node);
// Minimal-length unsigned big-endian integer contents (leading zeros stripped).
[[nodiscard]] Bytes unsigned_integer(std::span<const std::uint8_t> value);
// Encoders.
[[nodiscard]] Bytes encode(std::uint8_t tag, std::span<const std::uint8_t> contents);
[[nodiscard]] Bytes integer(std::span<const std::uint8_t> big_endian_unsigned);
[[nodiscard]] Bytes sequence(std::initializer_list<Bytes> parts);
[[nodiscard]] Bytes sequence(const std::vector<Bytes>& parts);
[[nodiscard]] std::string oid(std::span<const std::uint8_t> value);   // dotted form
} // namespace der

[[nodiscard]] std::string base64_encode(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::optional<Bytes> base64_decode(std::string_view text);
[[nodiscard]] std::string hex(std::span<const std::uint8_t> bytes);

} // namespace alchemy::gui::security
