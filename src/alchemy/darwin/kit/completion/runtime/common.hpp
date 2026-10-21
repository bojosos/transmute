#pragma once

// Shared helpers of the QLab G2 runtime providers (src/alchemy/darwin/kit/completion/runtime/).
// Not a contract header: only this directory includes it.

#include "alchemy/darwin/kit/completion/completion_kit.hpp"

#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace alchemy::gui::completion_kit::rt {

inline constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";
inline constexpr std::string_view libobjc = "/usr/lib/libobjc.A.dylib";
inline constexpr std::string_view core_foundation = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

// A NUL-terminated guest string (at most `limit` bytes).
[[nodiscard]] std::string c_string(GuestMemory& memory, Address address, std::size_t limit = std::size_t{1} << 20);

// Guest heap memory freed by the destructor (allocate_guest/release_guest).
class GuestBlock {
public:
    GuestBlock(Support& support, std::size_t size, bool zero = true);
    GuestBlock(Support& support, std::string_view text);   // NUL-terminated copy
    ~GuestBlock();
    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;
    [[nodiscard]] Address get() const noexcept { return address_; }
    operator Address() const noexcept { return address_; }
private:
    Support& support_;
    Address address_{};
};

// The UTF-8 text of an NSString ("" for nil).
[[nodiscard]] std::string text(Support& support, Address string);
// An autoreleased NSString (nil never).
[[nodiscard]] Address string(Support& support, std::string_view utf8);
[[nodiscard]] Address autoreleased(Support& support, Address object);
// A registered class; fails when no provider registered it.
[[nodiscard]] Address class_named(Support& support, std::string_view name);
// The name of a selector (sel_getName).
[[nodiscard]] std::string selector_name(Support& support, Address selector);
// A +1 instance of `cls` (allocated by the Objective-C runtime, so -dealloc
// runs on the last release) carrying `state` as its host state.
template <class T>
Address make_object(Support& support, Address cls, std::shared_ptr<T> state) {
    const auto object = support.objc.allocate(cls);
    static_cast<void>(support.objc.host_state<T>(object, [&] { return state; }));
    return object;
}
// An autoreleased NSNumber.
[[nodiscard]] Address number(Support& support, std::int64_t value);
[[nodiscard]] Address boolean(Support& support, bool value);
// An autoreleased NSDictionary of the pairs (keys are NSStrings made from the names).
[[nodiscard]] Address dictionary(Support& support, std::initializer_list<std::pair<std::string_view, Address>> entries);
// An autoreleased NSArray.
[[nodiscard]] Address array(Support& support, std::span<const Address> values);
// An autoreleased NSError (NSLocalizedDescriptionKey when `description`
// is given; extra user info entries are added as they are).
[[nodiscard]] Address error(Support& support, std::string_view domain, std::int64_t code, std::string_view description = {},
                            std::initializer_list<std::pair<std::string_view, Address>> info = {});
// *out = value when out is not NULL.
void store(Support& support, Address out, Address value);
// errno of the calling guest thread.
[[nodiscard]] std::uint32_t guest_errno(Support& support);
// objc_retain / objc_release (custom reference counting of any object).
Address objc_retain(Support& support, Address object);
void objc_release(Support& support, Address object);
// Blocks (the guest Blocks runtime).
[[nodiscard]] Address block_copy(Support& support, Address block);
void block_release(Support& support, Address block);
// Calls block(arguments...): x0 is the block, the arguments follow in x1...
CallResult invoke_block(Support& support, Address block, CallRegs registers = {});
Address invoke_block(Support& support, Address block, std::initializer_list<Address> arguments);
// [[NSNotificationCenter defaultCenter] postNotificationName:object:userInfo:]
void post_notification(Support& support, std::string_view name, Address object, Address user_info = 0);
// Calls a guest function pointer with integer arguments.
Address call_address(Support& support, Address function, std::initializer_list<Address> arguments);

} // namespace alchemy::gui::completion_kit::rt

namespace alchemy::gui::completion_kit {
// The calling guest thread's return addresses (lr, then the frame-pointer
// chain from x29), at most `limit` (threads.cpp).
[[nodiscard]] std::vector<Address> guest_backtrace(Support& support, const CpuState& state, std::size_t limit);
// Foundation's -callStackSymbols lines for those addresses.
[[nodiscard]] std::vector<std::string> symbolize(Support& support, const std::vector<Address>& frames);
// NSInvocation helpers (invocation.cpp): a +1 invocation of a message a
// proxy received (its register image), retaining the arguments (and the
// target when `keep_target`); the +0 signature of an instance method, or nil.
[[nodiscard]] Address capture_invocation(Support& support, Address signature, Address target, Address selector,
                                         const CpuState& state, bool keep_target);
[[nodiscard]] Address method_signature(Support& support, Address cls, Address selector);

// Provider install functions of this directory (called by install_group_runtime).
void install_dispatch_objects(Support& support);   // dispatch_objects.cpp
void install_exceptions(Support& support);         // exceptions.cpp
void install_threads(Support& support);            // threads.cpp
void install_locks(Support& support);              // threads.cpp
void install_invocations(Support& support);        // invocation.cpp
void install_undo_manager(Support& support);       // undo.cpp
void install_key_value_observing(Support& support); // kvo.cpp
void install_bindings(Support& support);           // bindings.cpp
void install_kvc_collections(Support& support);    // kvc_collections.cpp
void install_notification_queue(Support& support); // notification_queue.cpp
void install_value_transformers(Support& support); // darwin/foundation/value_transformers.cpp
void install_process_io(Support& support);         // process_io.cpp
void install_files(Support& support);              // files.cpp
void install_bundles(Support& support);            // bundles.cpp
void install_system_functions(Support& support);   // system_c.cpp
void install_os_log_store(Support& support);       // oslog_store.cpp
void install_scripting(Support& support);          // scripting.cpp
void install_xpc(Support& support);                // xpc.cpp
void install_cf_extras(Support& support);          // cf_extras.cpp
// CFBundleGetVersionNumber's encoding of a CFBundleVersion string (cf_extras.cpp).
[[nodiscard]] std::uint32_t bundle_version_number(std::string_view text);
} // namespace alchemy::gui::completion_kit
