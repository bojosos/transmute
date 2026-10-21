#include "alchemy/darwin/foundation/guest.hpp"

#include <array>

namespace alchemy::gui::completion_kit::data {

std::u16string Guest::text(Address string) {
    if (!string) return {};
    if (s.cf.kind(string) != ObjectKind::string) {
        // Another NSString implementation: through -description's characters.
        if (!responds(string, "length")) fail("an object that is not a string where an NSString is required");
        const auto described = s.send_id(string, "description");
        if (!described || s.cf.kind(described) != ObjectKind::string) fail("an NSString subclass without CF characters");
        string = described;
    }
    const auto units = s.cf.units(string);
    return std::u16string(units.begin(), units.end());
}

Address Guest::string(std::u16string_view value) {
    const std::vector<std::uint16_t> units(value.begin(), value.end());
    return s.cf.string_utf16(units);
}

Address Guest::mutable_string(std::u16string_view value) {
    const auto immutable = string(value);
    const auto made = s.cf.mutable_copy(immutable);
    s.release(immutable);
    return made;
}

std::vector<Address> Guest::elements(Address collection) {
    if (!collection) return {};
    if (s.cf.kind(collection) == ObjectKind::array) return s.cf.array_values(collection);
    if (s.cf.kind(collection) == ObjectKind::dictionary) {
        std::vector<Address> keys;
        for (const auto& [key, value] : s.cf.dictionary_entries(collection)) keys.push_back(key);
        return keys;
    }
    if (!responds(collection, "countByEnumeratingWithState:objects:count:")) fail("a collection that does not support fast enumeration");
    std::vector<Address> out;
    Scratch state(*this, 64 + 16 * 8);
    for (unsigned i = 0; i < 8; ++i) memory().write64(state.address + i * 8, 0);
    const auto buffer = state.address + 64;
    for (;;) {
        CallRegs r;
        r.x[2] = state.address;
        r.x[3] = buffer;
        r.x[4] = 16;
        const auto count = s.send(collection, "countByEnumeratingWithState:objects:count:", r).x0;
        if (!count) break;
        const auto items = memory().read64(state.address + 8);
        for (Address i = 0; i < count; ++i) out.push_back(memory().read64(items + i * 8));
        if (out.size() > (1u << 26)) fail("fast enumeration does not end");
    }
    return out;
}

Address Guest::array(const std::vector<Address>& values, bool mutable_array) {
    // Class objects are adopted by class_objects.cpp when CF retains them.
    return s.cf.array(values, mutable_array);
}

std::vector<std::uint64_t> Guest::indexes(Address index_set) {
    std::vector<std::uint64_t> out;
    if (!index_set) return out;
    auto index = s.send(index_set, "firstIndex").x0;
    while (index != not_found) {
        out.push_back(index);
        CallRegs r;
        r.x[2] = index;
        index = s.send(index_set, "indexGreaterThanIndex:", r).x0;
        if (out.size() > (1u << 26)) fail("NSIndexSet enumeration does not end");
    }
    return out;
}

Address Guest::index_set(const std::vector<std::uint64_t>& values, bool mutable_set) {
    const auto made = s.send_id(s.objc.find_class("NSMutableIndexSet"), "indexSet");
    for (const auto value : values) {
        CallRegs r;
        r.x[2] = value;
        static_cast<void>(s.send(made, "addIndex:", r));
    }
    if (mutable_set) return made;
    return autorelease(s.send_id(made, "copy"));
}

Address Guest::set_of(Address cls, const std::vector<Address>& values) {
    const auto list = array(values);
    Address made{};
    try { made = s.send_id(cls, "setWithArray:", {list}); } catch (...) { release(list); throw; }
    release(list);
    return made;
}

std::string Guest::class_name(Address cls) {
    const auto pointer = s.call(objc_library, "_class_getName", {cls});
    std::string name;
    for (auto at = pointer; at && name.size() < 1024; ++at) {
        const auto c = memory().read8(at);
        if (!c) break;
        name.push_back(static_cast<char>(c));
    }
    return name;
}

Address Guest::superclass(Address cls) { return s.call(objc_library, "_class_getSuperclass", {cls}); }

bool Guest::is_kind(Address object, Address cls) {
    if (!object || !cls) return false;
    for (auto current = s.objc.object_class(object); current; current = superclass(current)) if (current == cls) return true;
    return false;
}

bool Guest::is_subclass(Address cls, Address of) {
    if (!cls || !of) return false;
    for (auto current = cls; current; current = superclass(current)) if (current == of) return true;
    return false;
}

bool Guest::has_method(Address cls, std::string_view selector, bool class_method) {
    const auto target = class_method ? s.process.main_engine().read64(cls) : cls;   // the metaclass for class methods
    return s.call(objc_library, "_class_getInstanceMethod", {target, s.objc.selector(selector)}) != 0;
}

bool Guest::is_equal(Address left, Address right) {
    if (left == right) return true;
    if (!left || !right) return false;
    CallRegs r;
    r.x[2] = right;
    return (s.send(left, "isEqual:", r).x0 & 0xff) != 0;
}

std::int64_t Guest::compare(Address left, Address right, std::string_view selector) {
    CallRegs r;
    r.x[2] = right;
    return static_cast<std::int64_t>(s.send(left, selector, r).x0);
}

Address Guest::invoke_block(Address block, std::initializer_list<Address> arguments) {
    if (!block) fail("a nil block");
    const auto invoke = memory().read64(block + 16);
    std::vector<Address> all{block};
    all.insert(all.end(), arguments.begin(), arguments.end());
    return s.process.calling_engine().invoke_callback(invoke, all);
}

Address Guest::perform(Address receiver, Address selector, std::initializer_list<Address> arguments) {
    CallRegs r;
    unsigned index = 2;
    for (const auto value : arguments) r.x.at(index++) = value;
    return s.objc.send(receiver, selector, r).x0;
}

Address Guest::error(std::string_view domain, std::int64_t code, std::string_view description, Address user_info_extra) {
    const auto error_class = s.objc.find_class("NSError");
    if (!error_class) fail("NSError is not available");
    std::vector<std::pair<Address, Address>> entries;
    if (user_info_extra) entries = s.cf.dictionary_entries(user_info_extra);
    if (!description.empty()) entries.emplace_back(s.cf.constant_string("NSDebugDescription"), autorelease(s.cf.string(description)));
    const auto info = autorelease(s.cf.dictionary(entries));
    CallRegs r;
    r.x[2] = s.cf.constant_string(domain);
    r.x[3] = static_cast<Address>(code);
    r.x[4] = info;
    return s.send(error_class, "errorWithDomain:code:userInfo:", r).x0;
}

Guest::Scratch::Scratch(Guest& guest, std::size_t size) : g(guest), address(guest.s.process.allocate_guest(std::max<std::size_t>(size, 8))) {}
Guest::Scratch::~Scratch() { g.s.process.release_guest(address); }

void fill_method(Support& support, Address cls, const std::string& name, Handler handler, const std::string& types, bool class_method) {
    Guest g{support};
    if (g.has_method(cls, name, class_method)) return;
    support.method(cls, name, std::move(handler), types, class_method);
}

} // namespace alchemy::gui::completion_kit::data
