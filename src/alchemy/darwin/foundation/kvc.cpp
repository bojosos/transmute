#include "alchemy/darwin/kit/completion/completion_kit.hpp"
#include "alchemy/darwin/foundation/kvo.hpp"

#include <optional>
#include <string>
#include <vector>

// NSKeyValueCoding on NSObject (Foundation; QLab's controllers and nib
// outlets use it), added only when NSObject has no -setValue:forKey: yet.
// Search order, as Foundation documents it:
//   -valueForKey:      -get<Key>, -<key>, -is<Key>, -_<key>; then, when
//                      +accessInstanceVariablesDirectly (YES by default), the
//                      instance variables _<key>, _is<Key>, <key>, is<Key>;
//                      else -valueForUndefinedKey:.
//   -setValue:forKey:  -set<Key>:, -_set<Key>:; then the instance variables
//                      in the same order; else -setValue:forUndefinedKey:.
// Scalars are boxed as NSNumber (integers, floating point, BOOL) and unboxed
// with -longLongValue / -doubleValue / -boolValue; nil for a scalar goes to
// -setNilValueForKey:. The default -valueForUndefinedKey:,
// -setValue:forUndefinedKey: and -setNilValueForKey: fail explicitly (Foundation
// raises NSUndefinedKeyException / NSInvalidArgumentException); subclasses
// override them as usual. Not provided (explicit failures): structure-typed
// properties and key-path collection operators (@count, ...). The ordered
// collection accessors -countOf<Key> / -objectIn<Key>AtIndex: (or
// -<key>AtIndexes:) answer -valueForKey: with an array of their elements;
// -mutableArrayValueForKey: is runtime/kvc_collections.cpp's. Object instance
// variables set through KVC retain the new value and release the old one
// (Foundation's behaviour); writes to instance variables post
// will/didChangeValueForKey: when the key is observed with automatic
// notification (runtime/kvo.cpp).
namespace alchemy::gui::completion_kit {
namespace {

constexpr std::string_view objc_library = "/usr/lib/libobjc.A.dylib";

// One type of an Objective-C type encoding ("v24@0:8@16" -> v, @, :, @).
std::vector<std::string> split_types(std::string_view encoding) {
    std::vector<std::string> types;
    std::size_t at = 0;
    const auto skip_digits = [&] { while (at < encoding.size() && ((encoding[at] >= '0' && encoding[at] <= '9') || encoding[at] == '-')) ++at; };
    while (at < encoding.size()) {
        while (at < encoding.size() && std::string_view("rnNoORVA").find(encoding[at]) != std::string_view::npos) ++at;
        if (at >= encoding.size()) break;
        const auto start = at;
        const auto one = [&](auto& self) -> void {
            if (at >= encoding.size()) return;
            const char c = encoding[at];
            if (c == '{' || c == '(' || c == '[') {
                const char close = c == '{' ? '}' : c == '(' ? ')' : ']';
                int depth = 0;
                do {
                    if (encoding[at] == c) ++depth;
                    else if (encoding[at] == close) --depth;
                    ++at;
                } while (at < encoding.size() && depth > 0);
            } else if (c == '^') {
                ++at;
                self(self);
            } else if (c == '@') {
                ++at;
                if (at < encoding.size() && encoding[at] == '?') ++at;
                else if (at < encoding.size() && encoding[at] == '"') {
                    const auto end = encoding.find('"', at + 1);
                    at = end == std::string_view::npos ? encoding.size() : end + 1;
                }
            } else {
                ++at;
            }
        };
        one(one);
        types.emplace_back(encoding.substr(start, at - start));
        skip_digits();
    }
    return types;
}

enum class Scalar { object, signed_integer, unsigned_integer, f32, f64, boolean, unsupported };

struct ScalarType { Scalar kind{Scalar::unsupported}; unsigned size{}; };

ScalarType classify(std::string_view type) {
    if (type.empty()) return {};
    switch (type[0]) {
    case '@': case '#': return {Scalar::object, 8};
    case 'c': return {Scalar::signed_integer, 1};
    case 's': return {Scalar::signed_integer, 2};
    case 'i': return {Scalar::signed_integer, 4};
    case 'l': case 'q': return {Scalar::signed_integer, 8};
    case 'C': return {Scalar::unsigned_integer, 1};
    case 'S': return {Scalar::unsigned_integer, 2};
    case 'I': return {Scalar::unsigned_integer, 4};
    case 'L': case 'Q': return {Scalar::unsigned_integer, 8};
    case 'f': return {Scalar::f32, 4};
    case 'd': return {Scalar::f64, 8};
    case 'B': return {Scalar::boolean, 1};
    default: return {};
    }
}

std::string capitalized(std::string key) {
    if (!key.empty() && key[0] >= 'a' && key[0] <= 'z') key[0] = static_cast<char>(key[0] - 'a' + 'A');
    return key;
}

struct Kvc {
    Support& support;

    std::string text(Address string) { return support.cf.utf8(string); }
    Address autoreleased(Address object) { return object ? support.send_id(object, "autorelease") : 0; }
    Address string(std::string_view value) { return autoreleased(support.cf.string(value)); }

    std::string read_c_string(Address pointer) {
        std::string result;
        if (!pointer) return result;
        for (auto at = pointer; result.size() < 4096; ++at) {
            const auto c = support.memory().read8(at);
            if (!c) break;
            result.push_back(static_cast<char>(c));
        }
        return result;
    }
    Address with_c_string(const std::string& value, auto&& use) {
        const auto block = support.process.allocate_guest(value.size() + 1);
        try {
            support.memory().write_bytes(block, std::span(reinterpret_cast<const std::uint8_t*>(value.c_str()), value.size() + 1));
            const auto result = use(block);
            support.process.release_guest(block);
            return result;
        } catch (...) {
            support.process.release_guest(block);
            throw;
        }
    }

    bool responds(Address object, const std::string& selector) { return support.objc.responds(object, support.objc.selector(selector)); }
    std::vector<std::string> method_types(Address object, const std::string& selector) {
        const auto cls = support.objc.object_class(object);
        const auto method = support.call(objc_library, "_class_getInstanceMethod", {cls, support.objc.selector(selector)});
        if (!method) return {};
        return split_types(read_c_string(support.call(objc_library, "_method_getTypeEncoding", {method})));
    }
    struct Ivar { Address offset{}; std::string type; };
    std::optional<Ivar> ivar(Address object, const std::string& name) {
        const auto cls = support.objc.object_class(object);
        const auto found = with_c_string(name, [&](Address block) { return support.call(objc_library, "_class_getInstanceVariable", {cls, block}); });
        if (!found) return std::nullopt;
        return Ivar{support.call(objc_library, "_ivar_getOffset", {found}), read_c_string(support.call(objc_library, "_ivar_getTypeEncoding", {found}))};
    }
    bool ivars_accessible(Address object) {
        const auto cls = support.objc.object_class(object);
        return (support.send(cls, "accessInstanceVariablesDirectly").x0 & 0xff) != 0;
    }

    // Boxing (autoreleased NSNumber / object as is).
    Address box_bits(ScalarType type, std::uint64_t bits) {
        switch (type.kind) {
        case Scalar::object: return bits;
        case Scalar::boolean: return support.cf.boolean((bits & 0xff) != 0);
        case Scalar::signed_integer: {
            const auto shift = 64 - 8 * type.size;
            const auto value = static_cast<std::int64_t>(bits << shift) >> shift;
            return autoreleased(support.cf.integer(value));
        }
        case Scalar::unsigned_integer: {
            const auto value = type.size == 8 ? bits : bits & ((std::uint64_t{1} << (8 * type.size)) - 1);
            if (value > static_cast<std::uint64_t>(INT64_MAX)) unsupported("key-value coding of an unsigned value above INT64_MAX");
            return autoreleased(support.cf.integer(static_cast<std::int64_t>(value)));
        }
        case Scalar::f32: return autoreleased(support.cf.real(std::bit_cast<float>(static_cast<std::uint32_t>(bits))));
        case Scalar::f64: return autoreleased(support.cf.real(std::bit_cast<double>(bits)));
        case Scalar::unsupported: break;
        }
        unsupported("key-value coding of a non-object, non-numeric property");
    }
    // Unboxing to raw bits (the value is a non-nil NSNumber or NSString).
    std::uint64_t unbox_bits(ScalarType type, Address value) {
        switch (type.kind) {
        case Scalar::object: return value;
        case Scalar::boolean: return (support.send(value, "boolValue").x0 & 0xff) != 0;
        case Scalar::signed_integer:
        case Scalar::unsigned_integer: return support.send(value, "longLongValue").x0;
        case Scalar::f32: return std::bit_cast<std::uint32_t>(static_cast<float>(support.send(value, "doubleValue").d(0)));
        case Scalar::f64: return std::bit_cast<std::uint64_t>(support.send(value, "doubleValue").d(0));
        case Scalar::unsupported: break;
        }
        unsupported("key-value coding of a non-object, non-numeric property");
    }

    std::optional<Address> get_by_accessor(Address object, const std::string& key) {
        const auto cap = capitalized(key);
        for (const auto& selector : {"get" + cap, key, "is" + cap, "_" + key}) {
            if (!responds(object, selector)) continue;
            const auto types = method_types(object, selector);
            if (types.size() != 3) continue;   // takes arguments: not a getter
            const auto type = classify(types[0]);
            if (types[0] == "v") continue;
            if (type.kind == Scalar::unsupported) unsupported("key-value coding of -" + selector + " returning " + types[0]);
            const auto result = support.send(object, selector);
            if (type.kind == Scalar::f32) return box_bits(type, std::bit_cast<std::uint32_t>(result.s(0)));
            if (type.kind == Scalar::f64) return box_bits(type, std::bit_cast<std::uint64_t>(result.d(0)));
            return box_bits(type, result.x0);
        }
        return std::nullopt;
    }
    std::uint64_t read_ivar(Address object, const Ivar& field, ScalarType type) {
        const auto at = object + field.offset;
        switch (type.size) {
        case 1: return support.memory().read8(at);
        case 2: return support.memory().read8(at) | (std::uint64_t{support.memory().read8(at + 1)} << 8);
        case 4: return support.memory().read32(at);
        default: return support.memory().read64(at);
        }
    }
    void write_ivar(Address object, const Ivar& field, ScalarType type, std::uint64_t bits) {
        const auto at = object + field.offset;
        switch (type.size) {
        case 1: support.memory().write8(at, static_cast<std::uint8_t>(bits)); break;
        case 2: support.memory().write8(at, static_cast<std::uint8_t>(bits)); support.memory().write8(at + 1, static_cast<std::uint8_t>(bits >> 8)); break;
        case 4: support.memory().write32(at, static_cast<std::uint32_t>(bits)); break;
        default: support.memory().write64(at, bits); break;
        }
    }
    std::optional<std::pair<Ivar, ScalarType>> find_ivar(Address object, const std::string& key) {
        if (!ivars_accessible(object)) return std::nullopt;
        const auto cap = capitalized(key);
        for (const auto& name : {"_" + key, "_is" + cap, key, "is" + cap}) {
            const auto field = ivar(object, name);
            if (!field) continue;
            const auto type = classify(field->type);
            if (type.kind == Scalar::unsupported) unsupported("key-value coding of instance variable " + name + " of type " + field->type);
            return std::pair{*field, type};
        }
        return std::nullopt;
    }

    Address value_for_key(Address object, Address key_object) {
        const auto key = text(key_object);
        if (key.empty()) return support.send_id(object, "valueForUndefinedKey:", {key_object});
        if (const auto value = get_by_accessor(object, key)) return *value;
        const auto cap = capitalized(key);
        if (responds(object, "countOf" + cap)) {
            const bool by_index = responds(object, "objectIn" + cap + "AtIndex:");
            if (!by_index && !responds(object, key + "AtIndexes:"))
                unsupported("key-value coding collection accessors (-countOf" + cap + " without -objectIn" + cap + "AtIndex:)");
            const auto count = support.send(object, "countOf" + cap).x0;
            std::vector<Address> values;
            if (by_index) {
                for (Address index = 0; index < count; ++index) {
                    CallRegs registers;
                    registers.x[2] = index;
                    values.push_back(support.send(object, "objectIn" + cap + "AtIndex:", registers).x0);
                }
                return autoreleased(support.cf.array(values));
            }
            const auto indexes = support.send_id(support.objc.find_class("NSIndexSet"), "indexSetWithIndexesInRange:", {0, count});
            return support.send_id(object, key + "AtIndexes:", {indexes});
        }
        if (const auto found = find_ivar(object, key)) return box_bits(found->second, read_ivar(object, found->first, found->second));
        return support.send_id(object, "valueForUndefinedKey:", {key_object});
    }

    void set_value_for_key(Address object, Address value, Address key_object) {
        const auto key = text(key_object);
        const auto cap = capitalized(key);
        for (const auto& selector : {"set" + cap + ":", "_set" + cap + ":"}) {
            if (key.empty() || !responds(object, selector)) continue;
            const auto types = method_types(object, selector);
            if (types.size() != 4) continue;
            const auto type = classify(types[3]);
            if (type.kind == Scalar::unsupported) unsupported("key-value coding of -" + selector + " taking " + types[3]);
            if (type.kind != Scalar::object && !value) { support.send_id(object, "setNilValueForKey:", {key_object}); return; }
            CallRegs registers;
            const auto bits = unbox_bits(type, value);
            if (type.kind == Scalar::f32) registers.set_s(0, std::bit_cast<float>(static_cast<std::uint32_t>(bits)));
            else if (type.kind == Scalar::f64) registers.set_d(0, std::bit_cast<double>(bits));
            else registers.x[2] = bits;
            static_cast<void>(support.send(object, selector, registers));
            return;
        }
        if (!key.empty())
            if (const auto found = find_ivar(object, key)) {
                const auto& [field, type] = *found;
                if (type.kind != Scalar::object && !value) { support.send_id(object, "setNilValueForKey:", {key_object}); return; }
                const bool notify = kvo::notifies_automatically(support, object, key);
                if (notify) support.send_id(object, "willChangeValueForKey:", {key_object});
                if (type.kind == Scalar::object) {
                    const auto retained = value ? support.send_id(value, "retain") : 0;
                    const auto old = read_ivar(object, field, type);
                    write_ivar(object, field, type, retained);
                    if (old) static_cast<void>(support.send(old, "release"));
                } else {
                    write_ivar(object, field, type, unbox_bits(type, value));
                }
                if (notify) support.send_id(object, "didChangeValueForKey:", {key_object});
                return;
            }
        support.send_id(object, "setValue:forUndefinedKey:", {value, key_object});
    }
};

} // namespace

void install_key_value_coding(Support& support) {
    const auto root = support.objc.find_class("NSObject");
    if (!root) return;
    {
        const auto probe = support.objc.allocate(root);
        const bool present = support.objc.responds(probe, support.objc.selector("setValue:forKey:"));
        support.process.objects().release(probe);
        if (present) return;
    }
    auto kvc = std::make_shared<Kvc>(Kvc{support});
    // NSArray: -valueForKey: is the array of the elements' values (NSNull for
    // nil); -setValue:forKey: sets it on every element. Added before NSObject's
    // so the probe sees whether the array class has its own.
    if (const auto empty = support.cf.array({}); empty) {
        const bool array_has_kvc = support.objc.responds(empty, support.objc.selector("valueForKey:"));
        const auto array_class = support.objc.object_class(empty);
        support.release(empty);
        if (!array_has_kvc && array_class) {
            support.method(array_class, "valueForKey:", [kvc](CpuState& s, GuestMemory&) {
                auto& sp = kvc->support;
                const auto null_class = sp.objc.find_class("NSNull");
                std::vector<Address> values;
                for (const auto element : sp.cf.array_values(s.x[0])) {
                    auto value = sp.send_id(element, "valueForKey:", {s.x[2]});
                    if (!value && null_class) value = sp.send_id(null_class, "null");
                    if (!value) fail("-[NSArray valueForKey:] needs NSNull for a nil value");
                    values.push_back(value);
                }
                s.x[0] = kvc->autoreleased(sp.cf.array(values));
            }, "@@:@");
            support.method(array_class, "setValue:forKey:", [kvc](CpuState& s, GuestMemory&) {
                for (const auto element : kvc->support.cf.array_values(s.x[0])) kvc->support.send_id(element, "setValue:forKey:", {s.x[2], s.x[3]});
            }, "v@:@@");
        }
    }
    // NSUserDefaults: its keys are its key-value coding keys (Foundation's
    // behaviour; bindings and QLab read defaults with -valueForKey:).
    if (const auto defaults_class = support.objc.find_class("NSUserDefaults")) {
        support.method(defaults_class, "valueForKey:", [kvc](CpuState& s, GuestMemory&) {
            s.x[0] = s.x[2] ? kvc->support.send_id(s.x[0], "objectForKey:", {s.x[2]}) : 0;
        }, "@@:@");
        support.method(defaults_class, "setValue:forKey:", [kvc](CpuState& s, GuestMemory&) {
            if (!s.x[3]) fail("-[NSUserDefaults setValue:forKey:] with a nil key");
            if (s.x[2]) kvc->support.send_id(s.x[0], "setObject:forKey:", {s.x[2], s.x[3]});
            else kvc->support.send_id(s.x[0], "removeObjectForKey:", {s.x[3]});
        }, "v@:@@");
    }
    support.method(root, "accessInstanceVariablesDirectly", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:", true);
    support.method(root, "valueForKey:", [kvc](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("-valueForKey: with a nil key");
        s.x[0] = kvc->value_for_key(s.x[0], s.x[2]);
    }, "@@:@");
    support.method(root, "setValue:forKey:", [kvc](CpuState& s, GuestMemory&) {
        if (!s.x[3]) fail("-setValue:forKey: with a nil key");
        kvc->set_value_for_key(s.x[0], s.x[2], s.x[3]);
    }, "v@:@@");
    support.method(root, "valueForUndefinedKey:", [kvc](CpuState& s, GuestMemory&) {
        fail("NSUndefinedKeyException: valueForUndefinedKey: '" + kvc->text(s.x[2]) + "' (no accessor or instance variable)");
    }, "@@:@");
    support.method(root, "setValue:forUndefinedKey:", [kvc](CpuState& s, GuestMemory&) {
        fail("NSUndefinedKeyException: setValue:forUndefinedKey: '" + kvc->text(s.x[3]) + "' (no setter or instance variable)");
    }, "v@:@@");
    support.method(root, "setNilValueForKey:", [kvc](CpuState& s, GuestMemory&) {
        fail("NSInvalidArgumentException: setNilValueForKey: '" + kvc->text(s.x[2]) + "' (nil for a scalar property)");
    }, "v@:@");
    // Key paths: the first key, then the rest on its value.
    support.method(root, "valueForKeyPath:", [kvc](CpuState& s, GuestMemory&) {
        const auto path = kvc->text(s.x[2]);
        if (!path.empty() && path[0] == '@') unsupported("key-path collection operators (" + path + ")");
        const auto dot = path.find('.');
        if (dot == std::string::npos) { s.x[0] = kvc->support.send_id(s.x[0], "valueForKey:", {s.x[2]}); return; }
        const auto head = kvc->support.send_id(s.x[0], "valueForKey:", {kvc->string(path.substr(0, dot))});
        s.x[0] = head ? kvc->support.send_id(head, "valueForKeyPath:", {kvc->string(path.substr(dot + 1))}) : 0;
    }, "@@:@");
    support.method(root, "setValue:forKeyPath:", [kvc](CpuState& s, GuestMemory&) {
        const auto path = kvc->text(s.x[3]);
        const auto dot = path.find('.');
        if (dot == std::string::npos) { kvc->support.send_id(s.x[0], "setValue:forKey:", {s.x[2], s.x[3]}); return; }
        const auto head = kvc->support.send_id(s.x[0], "valueForKey:", {kvc->string(path.substr(0, dot))});
        if (head) kvc->support.send_id(head, "setValue:forKeyPath:", {s.x[2], kvc->string(path.substr(dot + 1))});
    }, "v@:@@");
    support.method(root, "dictionaryWithValuesForKeys:", [kvc](CpuState& s, GuestMemory&) {
        auto& sp = kvc->support;
        std::vector<std::pair<Address, Address>> entries;
        const auto null_class = sp.objc.find_class("NSNull");
        for (const auto key : sp.cf.array_values(s.x[2])) {
            auto value = sp.send_id(s.x[0], "valueForKey:", {key});
            if (!value && null_class) value = sp.send_id(null_class, "null");
            if (!value) fail("dictionaryWithValuesForKeys: needs NSNull for a nil value");
            entries.emplace_back(key, value);
        }
        s.x[0] = kvc->autoreleased(sp.cf.dictionary(entries));
    }, "@@:@");
    support.method(root, "setValuesForKeysWithDictionary:", [kvc](CpuState& s, GuestMemory&) {
        auto& sp = kvc->support;
        const auto null_class = sp.objc.find_class("NSNull");
        const auto null_value = null_class ? sp.send_id(null_class, "null") : 0;
        for (const auto& [key, value] : sp.cf.dictionary_entries(s.x[2]))
            sp.send_id(s.x[0], "setValue:forKey:", {value == null_value ? 0 : value, key});
    }, "v@:@");
    // Validation: -validate<Key>:error: when the class has one, else valid.
    support.method(root, "validateValue:forKey:error:", [kvc](CpuState& s, GuestMemory&) {
        const auto selector = "validate" + capitalized(kvc->text(s.x[3])) + ":error:";
        if (!kvc->responds(s.x[0], selector)) { s.x[0] = 1; return; }
        CallRegs registers;
        registers.x[2] = s.x[2];
        registers.x[3] = s.x[4];
        s.x[0] = kvc->support.send(s.x[0], selector, registers).x0 & 0xff;
    }, "B@:^@@^@");
}

} // namespace alchemy::gui::completion_kit
