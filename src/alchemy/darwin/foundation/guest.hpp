#pragma once

// Guest helpers shared by the G1 data providers (src/alchemy/darwin/kit/completion/data/).
#include "alchemy/darwin/kit/completion/completion_kit.hpp"

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui::completion_kit::data {

inline constexpr Address not_found = static_cast<Address>(INT64_MAX);   // NSNotFound
inline constexpr std::string_view core_foundation_library = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
inline constexpr std::string_view objc_library = "/usr/lib/libobjc.A.dylib";
inline constexpr std::string_view libsystem_library = "/usr/lib/libSystem.B.dylib";

struct Guest {
    Support& s;

    [[nodiscard]] GuestMemory& memory() const { return s.memory(); }
    Address autorelease(Address object) { return object ? s.send_id(object, "autorelease") : 0; }
    Address retain(Address object) { return object ? s.send_id(object, "retain") : 0; }
    void release(Address object) { if (object) s.send_id(object, "release"); }

    // Strings.
    std::u16string text(Address string);                  // UTF-16 units of an NSString (nil: empty)
    std::string utf8(Address string) { return string ? s.cf.utf8(string) : std::string(); }
    Address string(std::u16string_view value);            // +1 NSString
    Address auto_string(std::u16string_view value) { return autorelease(string(value)); }
    Address auto_string(std::string_view utf8) { return autorelease(s.cf.string(utf8)); }
    Address mutable_string(std::u16string_view value);    // +1 NSMutableString
    bool is_string(Address object) { return object && s.cf.kind(object) == ObjectKind::string; }

    // Collections: the members of an array, set, ordered set, hash table or
    // any NSFastEnumeration object (nil: none).
    std::vector<Address> elements(Address collection);
    Address array(const std::vector<Address>& values, bool mutable_array = false);        // +1 NSArray (CF array)
    Address auto_array(const std::vector<Address>& values) { return autorelease(array(values)); }
    // NSIndexSet contents through its public interface.
    std::vector<std::uint64_t> indexes(Address index_set);
    Address index_set(const std::vector<std::uint64_t>& values, bool mutable_set = false);   // autoreleased
    Address set_of(Address cls, const std::vector<Address>& values);                      // autoreleased [cls setWithArray:]

    // Classes and messages.
    std::string class_name(Address cls);
    Address superclass(Address cls);
    bool is_kind(Address object, Address cls);
    bool is_subclass(Address cls, Address of);   // cls == of or inherits from it
    bool responds(Address object, std::string_view selector) { return object && s.objc.responds(object, s.objc.selector(selector)); }
    // class_getInstanceMethod on the class (or its metaclass): any
    // implementation, inherited included.
    bool has_method(Address cls, std::string_view selector, bool class_method = false);
    bool is_equal(Address left, Address right);
    std::uint64_t hash(Address object) { return object ? s.send(object, "hash").x0 : 0; }
    std::int64_t compare(Address left, Address right, std::string_view selector);   // NSComparisonResult
    Address value_for_key_path(Address object, Address key_path) { return s.send_id(object, "valueForKeyPath:", {key_path}); }
    Address value_for_key(Address object, Address key) { return s.send_id(object, "valueForKey:", {key}); }

    // Blocks: x0 of the call (the block itself is the first argument).
    Address invoke_block(Address block, std::initializer_list<Address> arguments);
    // Calls `selector` on `receiver` with up to six object/integer arguments.
    Address perform(Address receiver, Address selector, std::initializer_list<Address> arguments = {});

    // NSError (autoreleased).
    Address error(std::string_view domain, std::int64_t code, std::string_view description = {}, Address user_info_extra = 0);
    void set_error(Address error_out, Address error) { if (error_out) memory().write64(error_out, error); }

    // Guest scratch memory released at scope exit.
    struct Scratch {
        Guest& g;
        Address address;
        Scratch(Guest& guest, std::size_t size);
        ~Scratch();
        Scratch(const Scratch&) = delete;
        Scratch& operator=(const Scratch&) = delete;
    };
};

// Adds `name` to `cls` only when neither it nor a superclass implements it
// (another provider's implementation wins).
void fill_method(Support& support, Address cls, const std::string& name, Handler handler, const std::string& types, bool class_method = false);

// NSRange arguments are two registers; NSRange results are x0/x1.
inline void return_range(CpuState& s, std::int64_t location, std::int64_t length) {
    s.x[0] = location < 0 ? not_found : static_cast<Address>(location);
    s.x[1] = location < 0 ? 0 : static_cast<Address>(length);
}
inline void return_bool(CpuState& s, bool value) { s.x[0] = value ? 1 : 0; }
inline bool arg_bool(Address value) { return (value & 0xff) != 0; }

} // namespace alchemy::gui::completion_kit::data
