#include "alchemy/darwin/kit/completion/completion_kit.hpp"
#include "alchemy/darwin/foundation/json.hpp"

#include <algorithm>
#include <span>
#include <string>
#include <vector>

// NSJSONSerialization over json.cpp, added when Foundation lacks it.
// Foundation objects map to JSON as NSJSONSerialization documents:
// NSDictionary (string keys) <-> object, NSArray <-> array, NSString <->
// string, NSNumber <-> number (integers stay exact; kCFBooleanTrue/False are
// true/false), NSNull <-> null. Reading options: NSJSONReadingMutableContainers
// (1), MutableLeaves (2), FragmentsAllowed (4); JSON5 (8) and streams are not
// provided. Writing options: PrettyPrinted (1), SortedKeys (2),
// FragmentsAllowed (4), WithoutEscapingSlashes (8). Invalid objects fail
// explicitly (Foundation raises NSInvalidArgumentException); malformed data
// returns nil with an NSCocoaErrorDomain 3840 error.
namespace alchemy::gui::completion_kit {
namespace {

constexpr std::uint64_t reading_mutable_containers = 1, reading_mutable_leaves = 2, reading_fragments = 4, reading_json5 = 8;
constexpr std::uint64_t writing_pretty = 1, writing_sorted = 2, writing_fragments = 4, writing_unescaped_slashes = 8;

struct Bridge {
    Support& support;

    std::u16string units16(Address string) {
        const auto units = support.cf.units(string);
        return std::u16string(units.begin(), units.end());
    }
    Address string16(const std::u16string& value) {
        return support.cf.string_utf16(std::span(reinterpret_cast<const std::uint16_t*>(value.data()), value.size()));
    }
    bool is_null(Address object) {
        const auto null_class = support.objc.find_class("NSNull");
        return null_class && support.objc.object_class(object) == null_class;
    }

    // Foundation -> JSON; nullopt (with `why`) when the object is not JSON.
    std::optional<json::Value> to_json(Address object, std::string& why, int depth = 0) {
        if (depth > 512) { why = "nesting is too deep"; return std::nullopt; }
        if (!object) { why = "nil"; return std::nullopt; }
        if (is_null(object)) return json::Value{json::Null{}};
        const auto kind = support.cf.kind(object);
        if (!kind) { why = "an object that is not a dictionary, array, string, number or NSNull"; return std::nullopt; }
        switch (*kind) {
        case ObjectKind::boolean: return json::Value{support.send(object, "boolValue").x0 & 0xff ? true : false};
        case ObjectKind::string: return json::Value{units16(object)};
        case ObjectKind::number: {
            const auto type = support.send(object, "objCType").x0;
            const char code = type ? static_cast<char>(support.memory().read8(type)) : 'q';
            if (code == 'd' || code == 'f') return json::Value{support.cf.number_value(object).value_or(0)};
            return json::Value{static_cast<std::int64_t>(support.send(object, "longLongValue").x0)};
        }
        case ObjectKind::array: {
            auto array = std::make_shared<json::Array>();
            for (const auto element : support.cf.array_values(object)) {
                auto value = to_json(element, why, depth + 1);
                if (!value) return std::nullopt;
                array->push_back(std::move(*value));
            }
            return json::Value{array};
        }
        case ObjectKind::dictionary: {
            auto entries = std::make_shared<json::Object>();
            for (const auto& [key, element] : support.cf.dictionary_entries(object)) {
                if (!key || support.cf.kind(key) != ObjectKind::string) { why = "a dictionary key that is not a string"; return std::nullopt; }
                auto value = to_json(element, why, depth + 1);
                if (!value) return std::nullopt;
                entries->emplace_back(units16(key), std::move(*value));
            }
            return json::Value{entries};
        }
        default: why = "an object that is not a dictionary, array, string, number or NSNull"; return std::nullopt;
        }
    }

    // JSON -> Foundation (+1).
    Address from_json(const json::Value& value, std::uint64_t options) {
        if (std::holds_alternative<json::Null>(value.data)) {
            const auto null_class = support.objc.find_class("NSNull");
            if (!null_class) fail("NSJSONSerialization: NSNull is not provided");
            return support.send_id(support.send_id(null_class, "null"), "retain");
        }
        if (const auto* flag = std::get_if<bool>(&value.data)) return support.cf.boolean(*flag);
        if (const auto* integer = std::get_if<std::int64_t>(&value.data)) return support.cf.integer(*integer);
        if (const auto* real = std::get_if<double>(&value.data)) return support.cf.real(*real);
        if (const auto* text = std::get_if<std::u16string>(&value.data)) {
            const auto made = string16(*text);
            if (!(options & reading_mutable_leaves)) return made;
            const auto mutable_string = support.cf.mutable_copy(made);
            support.release(made);
            return mutable_string;
        }
        const bool mutable_container = (options & reading_mutable_containers) != 0;
        std::vector<Address> made;
        const auto release_made = [&] { for (const auto object : made) support.release(object); };
        try {
            if (const auto* array = std::get_if<std::shared_ptr<json::Array>>(&value.data)) {
                for (const auto& element : **array) made.push_back(from_json(element, options));
                const auto result = support.cf.array(made, mutable_container);
                release_made();
                return result;
            }
            const auto& object = **std::get_if<std::shared_ptr<json::Object>>(&value.data);
            std::vector<std::pair<Address, Address>> entries;
            for (const auto& [key, element] : object) {
                made.push_back(string16(key));
                const auto key_object = made.back();
                made.push_back(from_json(element, options));
                entries.emplace_back(key_object, made.back());
            }
            const auto result = support.cf.dictionary(entries, mutable_container);
            release_made();
            return result;
        } catch (...) { release_made(); throw; }
    }

    Address error(const std::string& description) {
        const auto error_class = support.objc.find_class("NSError");
        if (!error_class) return 0;
        const auto key = support.send_id(support.cf.string("NSDebugDescription"), "autorelease");
        const auto text = support.send_id(support.cf.string(description), "autorelease");
        std::vector<std::pair<Address, Address>> entries{{key, text}};
        const auto info = support.send_id(support.cf.dictionary(entries), "autorelease");
        CallRegs registers;
        registers.x[2] = support.send_id(support.cf.string("NSCocoaErrorDomain"), "autorelease");
        registers.x[3] = 3840;
        registers.x[4] = info;
        return support.send(error_class, "errorWithDomain:code:userInfo:", registers).x0;
    }
};

} // namespace

void install_json_serialization(Support& support) {
    if (support.objc.find_class("NSJSONSerialization")) return;
    const auto js = support.declare_class("NSJSONSerialization", "NSObject", foundation_library);
    auto bridge = std::make_shared<Bridge>(Bridge{support});
    support.method(js, "isValidJSONObject:", [bridge](CpuState& s, GuestMemory&) {
        std::string why;
        const auto kind = s.x[2] ? bridge->support.cf.kind(s.x[2]) : std::nullopt;
        // The top level must be an array or a dictionary.
        if (!kind || (*kind != ObjectKind::array && *kind != ObjectKind::dictionary)) { s.x[0] = 0; return; }
        s.x[0] = bridge->to_json(s.x[2], why).has_value();
    }, "B@:@", true);
    support.method(js, "dataWithJSONObject:options:error:", [bridge](CpuState& s, GuestMemory& m) {
        auto& sp = bridge->support;
        const auto options = s.x[3];
        if (!(options & writing_fragments) && s.x[2]) {
            const auto kind = sp.cf.kind(s.x[2]);
            if (!kind || (*kind != ObjectKind::array && *kind != ObjectKind::dictionary))
                fail("NSInvalidArgumentException: +[NSJSONSerialization dataWithJSONObject:options:error:] top level is not an array or dictionary");
        }
        std::string why;
        const auto value = bridge->to_json(s.x[2], why);
        if (!value) fail("NSInvalidArgumentException: invalid type in JSON write (" + why + ")");
        const auto text = json::write(*value, {(options & writing_pretty) != 0, (options & writing_sorted) != 0, (options & writing_unescaped_slashes) == 0});
        if (!text) {
            if (s.x[4]) m.write64(s.x[4], bridge->error("Invalid number value (NaN or infinity) in JSON write"));
            s.x[0] = 0;
            return;
        }
        const auto block = sp.process.allocate_guest(std::max<std::size_t>(text->size(), 1));
        Address data{};
        try {
            if (!text->empty()) m.write_bytes(block, std::span(reinterpret_cast<const std::uint8_t*>(text->data()), text->size()));
            CallRegs make;
            make.x[2] = block;
            make.x[3] = text->size();
            data = sp.send(sp.objc.find_class("NSData"), "dataWithBytes:length:", make).x0;
        } catch (...) { sp.process.release_guest(block); throw; }
        sp.process.release_guest(block);
        s.x[0] = data;
    }, "@@:@Q^@", true);
    support.method(js, "JSONObjectWithData:options:error:", [bridge](CpuState& s, GuestMemory& m) {
        auto& sp = bridge->support;
        if (!s.x[2]) fail("NSInvalidArgumentException: +[NSJSONSerialization JSONObjectWithData:options:error:] data parameter is nil");
        if (s.x[3] & reading_json5) unsupported("NSJSONReadingJSON5Allowed");
        const auto length = sp.cf.length(s.x[2]);
        std::string text(static_cast<std::size_t>(length), '\0');
        if (length) {
            const auto bytes = sp.send(s.x[2], "bytes").x0;
            m.read_bytes(bytes, std::span(reinterpret_cast<std::uint8_t*>(text.data()), text.size()));
        }
        const auto parsed = json::parse(text, (s.x[3] & reading_fragments) != 0);
        if (const auto* failure = std::get_if<json::ParseError>(&parsed)) {
            if (s.x[4]) m.write64(s.x[4], bridge->error(failure->message + " around character " + std::to_string(failure->offset) + "."));
            s.x[0] = 0;
            return;
        }
        s.x[0] = sp.send_id(bridge->from_json(std::get<json::Value>(parsed), s.x[3]), "autorelease");
    }, "@@:@Q^@", true);
    for (const auto* name : {"writeJSONObject:toStream:options:error:", "JSONObjectWithStream:options:error:"})
        support.method(js, name, [name](CpuState&, GuestMemory&) { unsupported(std::string("+[NSJSONSerialization ") + name + "] (streams)"); }, "@@:@@Q^@", true);
}

} // namespace alchemy::gui::completion_kit
