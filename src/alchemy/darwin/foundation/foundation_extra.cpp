// Foundation classes and functions beyond the core bridge: the headless surface
// a Cocoa command-line program reaches (paths, bundles, file manager, dates,
// run loops, notifications, mutable collections, sets, locales, formatting and
// clang constant-literal classes). Objects are CoreFoundation objects or
// native Objective-C instances with host payloads; nothing forwards to Apple code.
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/darwin/foundation/headless_shells.hpp"
#include "alchemy/host/locale.hpp"
#include "alchemy/host/system.hpp"
#include "alchemy/path.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>

namespace alchemy {
namespace {
using Address = std::uint64_t;
using transmute::CpuState;
using transmute::GuestMemory;
constexpr Address not_found = static_cast<Address>(INT64_MAX); // NSNotFound
constexpr std::uint32_t cf_ascii = 0x600, cf_utf8 = 0x08000100, cf_latin1 = 0x201, cf_utf16 = 0x100,
    cf_utf16le = 0x14000100, cf_utf16be = 0x10000100;
[[noreturn]] void fail(std::string_view why) { throw std::runtime_error("Alchemy Foundation: " + std::string(why)); }
// NSStringEncoding to CFStringEncoding for the encodings CoreFoundation implements.
// MacRoman and Windows-1252 agree with ASCII only; other text reports failure.
std::optional<std::uint32_t> cf_encoding(Address encoding) {
    switch (encoding) {
    case 1: case 12: case 30: return cf_ascii;
    case 4: return cf_utf8;
    case 5: return cf_latin1;
    case 10: return cf_utf16;
    case 0x94000100: return cf_utf16le;
    case 0x90000100: return cf_utf16be;
    default: return std::nullopt;
    }
}
double as_double(std::uint64_t bits) { return std::bit_cast<double>(bits); }
void return_double(CpuState& state, double value) { state.v[0] = {std::bit_cast<std::uint64_t>(value), 0}; }
std::string host_text(const std::filesystem::path& path) { const auto bytes = path.u8string(); return {reinterpret_cast<const char*>(bytes.data()), bytes.size()}; }
std::optional<std::string> host_variable(const char* name) {
#ifdef _MSC_VER
    char* buffer = nullptr; std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, name) || !buffer) return std::nullopt;
    std::string value(buffer); std::free(buffer); return value;
#else
    const auto* value = std::getenv(name); if (!value) return std::nullopt; return std::string(value);
#endif
}
// Lexical POSIX path operations with NSString path semantics.
std::vector<std::string> components(std::string_view path) {
    std::vector<std::string> result;
    for (std::size_t at = 0; at <= path.size();) {
        const auto next = std::min(path.find('/', at), path.size());
        if (next > at) result.emplace_back(path.substr(at, next - at));
        at = next + 1;
    }
    return result;
}
std::string join(const std::vector<std::string>& parts, bool absolute) {
    std::string result = absolute ? "/" : "";
    for (std::size_t at = 0; at < parts.size(); ++at) { if (at) result += '/'; result += parts[at]; }
    return result;
}
std::string trim_slashes(std::string path) { while (path.size() > 1 && path.back() == '/') path.pop_back(); return path; }
std::string standardize(std::string_view path) {
    std::vector<std::string> out;
    const bool absolute = path.starts_with('/');
    for (auto& part : components(path)) {
        if (part == ".") continue;
        if (part == ".." && absolute) { if (!out.empty()) out.pop_back(); continue; }
        if (part == ".." && !out.empty() && out.back() != "..") { out.pop_back(); continue; }
        out.push_back(part);
    }
    return join(out, absolute);
}
std::string last_component(std::string_view path) {
    const auto trimmed = trim_slashes(std::string(path));
    if (trimmed == "/") return "/";
    const auto slash = trimmed.find_last_of('/');
    return slash == std::string::npos ? trimmed : trimmed.substr(slash + 1);
}
std::string delete_last_component(std::string_view path) {
    auto trimmed = trim_slashes(std::string(path));
    if (trimmed == "/") return "/";
    const auto slash = trimmed.find_last_of('/');
    if (slash == std::string::npos) return "";
    if (slash == 0) return "/";
    return trim_slashes(trimmed.substr(0, slash));
}
std::string append_component(std::string_view path, std::string_view component) {
    std::string result = trim_slashes(std::string(path));
    std::string addition(component);
    while (addition.starts_with('/')) addition.erase(0, 1);
    if (result.empty()) return trim_slashes(addition);
    if (addition.empty()) return result;
    if (result != "/") result += '/';
    return trim_slashes(result + addition);
}
std::string path_extension(std::string_view path) {
    const auto leaf = last_component(path);
    const auto dot = leaf.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? "" : leaf.substr(dot + 1);
}
std::string delete_extension(std::string_view path) {
    const auto extension = path_extension(path);
    auto trimmed = trim_slashes(std::string(path));
    return extension.empty() ? trimmed : trimmed.substr(0, trimmed.size() - extension.size() - 1);
}
// XML property lists (Apple PropertyList-1.0 DTD), the Info.plist format.
struct PlistParser {
    std::string_view text;
    std::size_t at{};
    void space() { while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n')) ++at; }
    void skip_markup() {
        for (;;) {
            space();
            if (text.substr(at).starts_with("<?")) { const auto end = text.find("?>", at); if (end == std::string_view::npos) fail("unterminated plist declaration"); at = end + 2; }
            else if (text.substr(at).starts_with("<!--")) { const auto end = text.find("-->", at); if (end == std::string_view::npos) fail("unterminated plist comment"); at = end + 3; }
            else if (text.substr(at).starts_with("<!")) { const auto end = text.find('>', at); if (end == std::string_view::npos) fail("unterminated plist doctype"); at = end + 1; }
            else return;
        }
    }
    struct Tag { std::string name; bool closing{}, empty{}; };
    Tag tag() {
        skip_markup();
        if (at >= text.size() || text[at] != '<') fail("malformed plist element");
        const auto end = text.find('>', at);
        if (end == std::string_view::npos) fail("unterminated plist element");
        auto body = text.substr(at + 1, end - at - 1); at = end + 1;
        Tag result;
        if (body.starts_with('/')) { result.closing = true; body.remove_prefix(1); }
        if (body.ends_with('/')) { result.empty = true; body.remove_suffix(1); }
        result.name = std::string(body.substr(0, body.find_first_of(" \t\r\n")));
        return result;
    }
    std::string content(std::string_view name) {
        const auto close = "</" + std::string(name) + ">";
        const auto end = text.find(close, at);
        if (end == std::string_view::npos) fail("unterminated plist " + std::string(name));
        const auto raw = text.substr(at, end - at); at = end + close.size();
        std::string result;
        for (std::size_t index = 0; index < raw.size(); ++index) {
            if (raw[index] != '&') { result += raw[index]; continue; }
            const auto semicolon = raw.find(';', index);
            if (semicolon == std::string_view::npos) fail("malformed plist entity");
            const auto entity = raw.substr(index + 1, semicolon - index - 1); index = semicolon;
            if (entity == "lt") result += '<'; else if (entity == "gt") result += '>'; else if (entity == "amp") result += '&';
            else if (entity == "quot") result += '"'; else if (entity == "apos") result += '\'';
            else if (entity.starts_with('#')) {
                const bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
                std::uint32_t scalar{};
                for (const auto c : entity.substr(hex ? 2 : 1)) {
                    const auto digit = c >= '0' && c <= '9' ? c - '0' : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10 : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                    if (digit < 0 || scalar > 0x10ffff) fail("malformed plist character reference");
                    scalar = scalar * (hex ? 16 : 10) + static_cast<std::uint32_t>(digit);
                }
                if (scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) fail("invalid plist character reference");
                if (scalar < 0x80) result += static_cast<char>(scalar);
                else if (scalar < 0x800) { result += static_cast<char>(0xc0 | scalar >> 6); result += static_cast<char>(0x80 | (scalar & 0x3f)); }
                else if (scalar < 0x10000) { result += static_cast<char>(0xe0 | scalar >> 12); result += static_cast<char>(0x80 | ((scalar >> 6) & 0x3f)); result += static_cast<char>(0x80 | (scalar & 0x3f)); }
                else { result += static_cast<char>(0xf0 | scalar >> 18); result += static_cast<char>(0x80 | ((scalar >> 12) & 0x3f)); result += static_cast<char>(0x80 | ((scalar >> 6) & 0x3f)); result += static_cast<char>(0x80 | (scalar & 0x3f)); }
            } else fail("unsupported plist entity");
        }
        return result;
    }
};

struct SetState final : ObjectPayload { std::mutex mutex; std::vector<Address> values; std::uint64_t mutations{}; bool mutable_set{}; };
// NSEnumerator over a snapshot of a collection (the values are retained).
struct EnumeratorState final : ObjectPayload { std::mutex mutex; std::vector<Address> values; std::size_t next{}; };
struct NotificationState final : ObjectPayload { Address name{}, object{}, user_info{}; };
struct BundleState final : ObjectPayload { std::string path, executable; bool application{}; Address info{}; std::mutex mutex; };
struct Observer { Address observer{}, selector{}, name{}, object{}; };

struct Extra {
    ProcessContext& process;
    ObjcRuntime& objc;
    CoreFoundation& cf;
    std::mutex mutex, main_bundle_mutex;
    Address root{}, string_class{}, number_class{}, array_class{}, dictionary_class{}, data_class{};
    Address mutable_array{}, mutable_dictionary{}, mutable_data{}, set_class{}, mutable_set{}, url_class{}, date_class{};
    Address locale_class{}, bundle_class{}, file_manager{}, notification_center{}, notification_class{}, run_loop_class{};
    Address attributed_class{}, file_manager_object{}, center_object{}, main_bundle{}, distant_past{}, distant_future{};
    std::map<std::uint64_t, Address> run_loops;
    std::vector<Observer> observers;
    std::map<char, Address> type_encodings;

    Extra(ProcessContext& context, ObjcRuntime& runtime, CoreFoundation& foundation) : process(context), objc(runtime), cf(foundation) {
        root = objc.find_class("NSObject");
        string_class = objc.find_class("NSString"); number_class = objc.find_class("NSNumber");
        array_class = objc.find_class("NSArray"); dictionary_class = objc.find_class("NSDictionary"); data_class = objc.find_class("NSData");
        if (!root || !string_class || !number_class || !array_class || !dictionary_class || !data_class) fail("core bridge classes are missing");
        for (const auto cls : {string_class, number_class, array_class, dictionary_class, data_class, root}) alias(cls);
        install_literals();
        install_strings();
        install_numbers();
        install_collections();
        install_sets();
        install_urls();
        install_dates();
        install_locales();
        install_files();
        install_bundles();
        install_notifications();
        install_run_loops();
        install_attributed_strings();
        install_process_info();
        install_geometry();
        install_swift_string_bridging();
        install_collection_bridging();
    }

    // NSEnumerator (snapshots) and the collection selectors the Swift
    // standard library's Cocoa bridging sends (ShadowProtocols.swift).
    Address enumerator_class{};
    Address make_enumerator(std::vector<Address> values) {
        const auto object = objc.allocate(enumerator_class);
        try {
            auto state = attach<EnumeratorState>(object);
            for (const auto value : values) state->values.push_back(retain(value));
        } catch (...) { release(object); throw; }
        return autorelease(object);
    }
    void install_collection_bridging() {
        enumerator_class = define("NSEnumerator", root);
        method(enumerator_class, "nextObject", [this](auto& state, auto&) {
            const auto value = state_of<EnumeratorState>(state.x[0]);
            std::lock_guard lock(value->mutex);
            state.x[0] = value->next < value->values.size() ? value->values[value->next++] : 0;
        });
        method(enumerator_class, "allObjects", [this](auto& state, auto&) {
            const auto value = state_of<EnumeratorState>(state.x[0]);
            std::vector<Address> rest;
            { std::lock_guard lock(value->mutex); rest.assign(value->values.begin() + static_cast<std::ptrdiff_t>(value->next), value->values.end()); value->next = value->values.size(); }
            state.x[0] = autorelease(cf.array(rest));
        });
        method(enumerator_class, "countByEnumeratingWithState:objects:count:", [this](auto& state, auto& memory) {
            const auto value = state_of<EnumeratorState>(state.x[0]);
            std::vector<Address> rest;
            { std::lock_guard lock(value->mutex); rest.assign(value->values.begin() + static_cast<std::ptrdiff_t>(value->next), value->values.end()); }
            state.x[0] = enumerate(memory, state.x[2], state.x[3], state.x[4], rest);
        }, false, "Q@:^v^@Q");
        method(enumerator_class, "dealloc", [this](auto& state, auto&) {
            if (const auto value = state_of<EnumeratorState>(state.x[0], false)) {
                std::vector<Address> values; { std::lock_guard lock(value->mutex); values.swap(value->values); }
                for (const auto item : values) release(item);
            }
            destruct(state.x[0]);
        }, false, "v@:");
        method(array_class, "objectEnumerator", [this](auto& state, auto&) { state.x[0] = make_enumerator(cf.array_values(state.x[0])); });
        method(array_class, "reverseObjectEnumerator", [this](auto& state, auto&) {
            auto values = cf.array_values(state.x[0]); std::reverse(values.begin(), values.end()); state.x[0] = make_enumerator(values);
        });
        // -getObjects:range: (the range arrives in x3/x4).
        method(array_class, "getObjects:range:", [this](auto& state, auto& memory) {
            const auto values = cf.array_values(state.x[0]);
            const auto location = state.x[3], length = state.x[4];
            if (location > values.size() || length > values.size() - location) fail("getObjects:range: out of bounds");
            for (std::uint64_t index = 0; index < length; ++index) memory.write64(state.x[2] + index * 8, values[location + index]);
        }, false, "v@:^@{_NSRange=QQ}");
        const auto entries_of = [this](Address dictionary) { return cf.dictionary_entries(dictionary); };
        method(dictionary_class, "keyEnumerator", [this, entries_of](auto& state, auto&) {
            std::vector<Address> keys; for (const auto& [key, value] : entries_of(state.x[0])) { static_cast<void>(value); keys.push_back(key); }
            state.x[0] = make_enumerator(keys);
        });
        method(dictionary_class, "objectEnumerator", [this, entries_of](auto& state, auto&) {
            std::vector<Address> values; for (const auto& [key, value] : entries_of(state.x[0])) { static_cast<void>(key); values.push_back(value); }
            state.x[0] = make_enumerator(values);
        });
        method(dictionary_class, "getObjects:andKeys:count:", [this, entries_of](auto& state, auto& memory) {
            const auto entries = entries_of(state.x[0]);
            const auto count = std::min<std::uint64_t>(state.x[4], entries.size());
            for (std::uint64_t index = 0; index < count; ++index) {
                if (state.x[2]) memory.write64(state.x[2] + index * 8, entries[index].second);
                if (state.x[3]) memory.write64(state.x[3] + index * 8, entries[index].first);
            }
        }, false, "v@:^@^@Q");
        method(set_class, "objectEnumerator", [this](auto& state, auto&) { state.x[0] = make_enumerator(members(state.x[0])); });
        method(set_class, "getObjects:count:", [this](auto& state, auto& memory) {
            const auto values = members(state.x[0]);
            const auto count = std::min<std::uint64_t>(state.x[3], values.size());
            for (std::uint64_t index = 0; index < count; ++index) memory.write64(state.x[2] + index * 8, values[index]);
        }, false, "v@:^@Q");
    }

    // The NSString selectors the Swift standard library's Cocoa string
    // bridging sends (StringBridge.swift's _StringSelectorHolder) beyond the
    // public ones: fast-path inner pointers (nil when not stored that way),
    // bulk transcoding, and the tagged-string factory (no tagged pointers
    // exist under Alchemy, so it always answers nil).
    void install_swift_string_bridging() {
        const auto s = string_class;
        method(s, "_fastCStringContents:", [this](auto& state, auto&) {
            const auto units = cf.units(state.x[0]);
            const bool ascii = std::all_of(units.begin(), units.end(), [](std::uint16_t unit) { return unit < 0x80; });
            state.x[0] = ascii ? cf.c_string(state.x[0], cf_utf8) : 0;
        }, false, "r*@:B");
        method(s, "_fastCharacterContents", [this](auto& state, auto&) {
            state.x[0] = cf.invoke("CFStringGetCharactersPtr", std::array<Address, 1>{state.x[0]});
        }, false, "r^S@:");
        method(s, "_fastestEncodingInCFStringEncoding", [this](auto& state, auto&) {
            const auto units = cf.units(state.x[0]);
            const bool ascii = std::all_of(units.begin(), units.end(), [](std::uint16_t unit) { return unit < 0x80; });
            state.x[0] = ascii ? cf_ascii : 0x0100; // kCFStringEncodingASCII or kCFStringEncodingUnicode
        }, false, "I@:");
        method(s, "newTaggedNSStringWithASCIIBytes_:length_:", [](auto& state, auto&) { state.x[0] = 0; }, true, "@@:r*q");
        // -getBytes:maxLength:usedLength:encoding:options:range:remainingRange:
        // (the range is on the stack: x7 cannot hold the whole 16-byte struct).
        method(s, "getBytes:maxLength:usedLength:encoding:options:range:remainingRange:", [this](auto& state, auto& memory) {
            const auto encoding = cf_encoding(state.x[5]);
            const auto location = memory.read64(state.sp), length = memory.read64(state.sp + 8), remaining = memory.read64(state.sp + 16);
            if (!encoding) { state.x[0] = 0; return; }
            const auto lossy = (state.x[6] & 1) ? Address{'?'} : Address{0};
            const auto used = process.allocate_guest(8);
            Address converted{};
            try {
                converted = cf.invoke("CFStringGetBytes", std::array<Address, 9>{state.x[0], location, length, *encoding, lossy, 0,
                                                                                state.x[2], state.x[2] ? state.x[3] : 0, used});
                if (state.x[4]) memory.write64(state.x[4], memory.read64(used));
                process.release_guest(used);
            } catch (...) { process.release_guest(used); throw; }
            if (remaining) { memory.write64(remaining, location + converted); memory.write64(remaining + 8, length - converted); }
            state.x[0] = converted == length || converted > 0;
        }, false, "B@:^vQ^QQQ{_NSRange=QQ}^{_NSRange=QQ}");
        // -compare:options:range:locale: - literal (code unit) comparison of
        // the receiver's range with the argument; case-insensitive (1) folds ASCII.
        method(s, "compare:options:range:locale:", [this](auto& state, auto&) {
            auto left = cf.units(state.x[0]);
            auto right = state.x[2] ? cf.units(state.x[2]) : std::vector<std::uint16_t>{};
            const auto location = state.x[4], length = state.x[5];
            if (location > left.size() || length > left.size() - location) fail("compare range out of bounds");
            left = std::vector<std::uint16_t>(left.begin() + static_cast<std::ptrdiff_t>(location),
                                              left.begin() + static_cast<std::ptrdiff_t>(location + length));
            if (state.x[3] & 1) for (auto* units : {&left, &right}) for (auto& unit : *units)
                if (unit >= 'A' && unit <= 'Z') unit = static_cast<std::uint16_t>(unit - 'A' + 'a');
            state.x[0] = left < right ? static_cast<Address>(-1) : left > right ? 1 : 0;
        }, false, "q@:@Q{_NSRange=QQ}@");
        // Canonical decomposition / composition (Unicode NFD / NFC) through the
        // host normalizer; Swift hashes non-ASCII NSStrings through NFD.
        for (const bool decompose : {true, false})
            method(s, decompose ? "decomposedStringWithCanonicalMapping" : "precomposedStringWithCanonicalMapping", [this, decompose](auto& state, auto&) {
                const auto units = cf.units(state.x[0]);
                if (units.empty() || std::all_of(units.begin(), units.end(), [](std::uint16_t unit) { return unit < 0x80; })) {
                    state.x[0] = autorelease(retain(state.x[0]));
                    return;
                }
                state.x[0] = autorelease(cf.string_utf16(normalized(units, decompose)));
            });
    }
    static std::vector<std::uint16_t> normalized(const std::vector<std::uint16_t>& units, bool decompose) {
        const auto output = host::locale::normalize(std::u16string_view(reinterpret_cast<const char16_t*>(units.data()), units.size()),
                                                    decompose ? host::locale::Form::d : host::locale::Form::c);
        if (!output) fail("Unicode normalization failed (invalid UTF-16)");
        return {output->begin(), output->end()};
    }

    // Runtime plumbing.
    void alias(Address cls) {
        bool terminated{};
        const auto name_pointer = call(objc_library, "_class_getName", {cls});
        const auto bytes = process.main_engine().read_string(name_pointer, 1, 4096, &terminated);
        const std::string name(bytes.begin(), bytes.end());
        objc.publish_class(name, cls, core_foundation_library);
    }
    Address define(std::string name, Address super, std::uint32_t size = 16) {
        if (const auto existing = objc.find_class(name)) return existing;
        const auto cls = objc.register_native_class(std::move(name), super, size);
        alias(cls);
        return cls;
    }
    void method(Address cls, std::string name, transmute::GuestCallHandler handler, bool class_method = false, std::string types = "@@:") {
        static_cast<void>(objc.add_native_method(cls, std::move(name), std::move(handler), class_method, std::move(types)));
    }
    Address call(std::string_view library, std::string_view symbol, std::initializer_list<Address> arguments) {
        const auto service = process.resolve(library, symbol);
        if (!service) fail("missing runtime service " + std::string(symbol));
        return process.calling_engine().invoke_callback(service->address, std::span(arguments.begin(), arguments.size()));
    }
    Address send(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {}) {
        std::vector<Address> values{receiver, objc.selector(selector)};
        values.insert(values.end(), arguments.begin(), arguments.end());
        const auto service = process.resolve(objc_library, "_objc_msgSend");
        if (!service) fail("missing objc_msgSend");
        return process.calling_engine().invoke_callback(service->address, values);
    }
    bool send_bool(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {}) { return (send(receiver, selector, arguments) & 0xff) != 0; }
    Address retain(Address object) { return object ? call(objc_library, "_objc_retain", {object}) : 0; }
    void release(Address object) { if (object) static_cast<void>(call(objc_library, "_objc_release", {object})); }
    Address autorelease(Address object) {
        if (!object) return 0;
        try { return call(objc_library, "_objc_autorelease", {object}); }
        catch (...) { process.objects().release(object); throw; }
    }
    void replace(CpuState& state, Address value) {
        try { release(state.x[0]); } catch (...) { release(value); throw; }
        state.x[0] = value;
    }
    template<class Create> void factory(Address cls, std::string factory_name, std::string init_name, Create create, std::string types = "@@:") {
        method(cls, std::move(factory_name), [this, create](auto& state, auto& memory) { state.x[0] = autorelease(create(state, memory)); }, true, types);
        if (!init_name.empty()) method(cls, std::move(init_name), [this, create](auto& state, auto& memory) { const auto value = create(state, memory); replace(state, value); }, false, types);
    }
    GuestMemory& memory() { return process.calling_engine(); }
    std::optional<ObjectKind> kind(Address object) { return object ? cf.kind(object) : std::nullopt; }
    std::string text(Address object) {
        if (kind(object) != ObjectKind::string) fail("expected an NSString");
        return cf.utf8(object);
    }
    Address make_string(std::string_view value) { return cf.string(value); }
    Address auto_string(std::string_view value) { return autorelease(cf.string(value)); }
    Address guest_text(std::string_view value) {
        const auto address = process.allocate_guest(value.size() + 1);
        memory().write_bytes(address, std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
        memory().write8(address + value.size(), 0);
        return address;
    }
    std::string c_text(Address pointer, std::uint64_t limit = 1U << 20) {
        if (!pointer) fail("null C string");
        bool terminated{};
        const auto bytes = memory().read_string(pointer, 1, limit, &terminated);
        if (!terminated) fail("unterminated C string");
        return {bytes.begin(), bytes.end()};
    }
    template<class T> std::shared_ptr<T> state_of(Address object, bool required = true) {
        std::lock_guard lock(mutex);
        auto result = std::dynamic_pointer_cast<T>(process.objects().require(object)->payload);
        if (!result && required) fail("object is not initialized");
        return result;
    }
    template<class T> std::shared_ptr<T> attach(Address object) {
        auto value = std::make_shared<T>();
        const auto record = process.objects().require(object);
        std::lock_guard lock(mutex);
        if (record->payload) fail("object initialized twice");
        record->payload = value;
        return value;
    }
    void destruct(Address object) { static_cast<void>(call(objc_library, "_objc_destructInstance", {object})); }
    Address singleton(Address cls) { return process.objects().create(std::max<Address>(16, call(objc_library, "_class_getInstanceSize", {cls})), cls, ObjectKind::instance, {}, {}, true); }
    Address constant_c_string(char type) {
        std::lock_guard lock(mutex);
        if (const auto found = type_encodings.find(type); found != type_encodings.end()) return found->second;
        const auto address = process.allocate_guest(2); auto& guest = static_cast<GuestMemory&>(process.main_engine()); guest.write8(address, static_cast<std::uint8_t>(type)); guest.write8(address + 1, 0);
        type_encodings.emplace(type, address);
        return address;
    }
    // Apple ARM64 variadic arguments occupy consecutive 8-byte stack slots.
    struct Arguments { GuestMemory& memory; Address next; Address take() { const auto value = memory.read64(next); next += 8; return value; } };
    std::vector<Address> nil_terminated(Address first, Arguments arguments) {
        std::vector<Address> values;
        for (auto value = first; value; value = arguments.take()) { values.push_back(value); if (values.size() > 65536) fail("nil-terminated list exceeds 65536 objects"); }
        return values;
    }
    std::vector<Address> guest_objects(Address pointer, Address count) {
        if (count > 1U << 24) fail("object list is too large");
        std::vector<Address> values(static_cast<std::size_t>(count));
        for (std::size_t index = 0; index < values.size(); ++index) values[index] = memory().read64(pointer + index * 8);
        return values;
    }
    bool same(Address left, Address right) { return left == right || (left && right && send_bool(left, "isEqual:", {right})); }
    Address block_invoke(Address block) { if (!block) fail("null block"); return memory().read64(block + 16); }

    // String formatting: printf conversions plus %@, with Apple ARM64 variadics.
    std::string description_of(Address object) {
        if (!object) return "(null)";
        const auto value = send(object, "description");
        if (!value) return "(null)";
        return kind(value) == ObjectKind::string ? cf.utf8(value) : "<object>";
    }
    // Bytes in the system C string encoding as UTF-8: valid UTF-8 as is,
    // anything else decoded as Mac Roman (macOS's default C string encoding).
    static std::string system_c_string(const std::string& bytes) {
        if (valid_utf8(bytes)) return bytes;
        return host::locale::decode_mac_roman(bytes).value_or({});
    }
    static bool valid_utf8(const std::string& bytes) {
        for (std::size_t at = 0; at < bytes.size();) {
            const auto lead = static_cast<unsigned char>(bytes[at]);
            std::size_t extra{};
            if (lead < 0x80) extra = 0;
            else if ((lead >> 5) == 6) extra = 1;
            else if ((lead >> 4) == 14) extra = 2;
            else if ((lead >> 3) == 30) extra = 3;
            else return false;
            if (at + extra >= bytes.size() && extra) return false;
            for (std::size_t next = 1; next <= extra; ++next)
                if ((static_cast<unsigned char>(bytes[at + next]) >> 6) != 2) return false;
            at += extra + 1;
        }
        return true;
    }
    std::string format(std::string_view text, Arguments arguments) {
        if (text.size() > 1U << 20) fail("format string is too large");
        std::string output;
        for (std::size_t index = 0; index < text.size(); ++index) {
            if (text[index] != '%') { output += text[index]; continue; }
            std::string spec = "%";
            if (++index >= text.size()) fail("incomplete format conversion");
            while (index < text.size() && std::string_view("-+ #0'").find(text[index]) != std::string_view::npos) spec += text[index++];
            auto number = [&] {
                if (index < text.size() && text[index] == '*') { ++index; spec += std::to_string(static_cast<std::int32_t>(arguments.take())); return; }
                while (index < text.size() && text[index] >= '0' && text[index] <= '9') spec += text[index++];
            };
            number();
            if (index < text.size() && text[index] == '$') fail("positional format arguments are unsupported");
            if (index < text.size() && text[index] == '.') { spec += text[index++]; number(); }
            std::string length;
            while (index < text.size() && std::string_view("hlqLzjt").find(text[index]) != std::string_view::npos) length += text[index++];
            if (index >= text.size()) fail("incomplete format conversion");
            const auto conversion = text[index];
            std::array<char, 512> buffer{};
            auto emit = [&](const char* format_spec, auto value) {
                const auto size = std::snprintf(buffer.data(), buffer.size(), format_spec, value);
                if (size < 0) fail("format conversion failed");
                if (static_cast<std::size_t>(size) < buffer.size()) output.append(buffer.data(), static_cast<std::size_t>(size));
                else { std::string large(static_cast<std::size_t>(size) + 1, '\0'); std::snprintf(large.data(), large.size(), format_spec, value); output.append(large.data(), static_cast<std::size_t>(size)); }
            };
            const bool wide = length == "l" || length == "ll" || length == "q" || length == "z" || length == "j" || length == "t";
            switch (conversion) {
            case '%': output += '%'; break;
            case 'd': case 'i': {
                const auto raw = arguments.take();
                std::int64_t value = wide ? std::bit_cast<std::int64_t>(raw) : length == "hh" ? static_cast<std::int8_t>(raw) : length == "h" ? static_cast<std::int16_t>(raw) : static_cast<std::int32_t>(raw);
                emit((spec + "lld").c_str(), static_cast<long long>(value)); break;
            }
            case 'u': case 'o': case 'x': case 'X': {
                const auto raw = arguments.take();
                const std::uint64_t value = wide ? raw : length == "hh" ? static_cast<std::uint8_t>(raw) : length == "h" ? static_cast<std::uint16_t>(raw) : static_cast<std::uint32_t>(raw);
                emit((spec + "ll" + conversion).c_str(), static_cast<unsigned long long>(value)); break;
            }
            case 'c': emit((spec + "c").c_str(), static_cast<int>(static_cast<unsigned char>(arguments.take()))); break;
            case 'C': { const std::array<std::uint16_t, 1> unit{static_cast<std::uint16_t>(arguments.take())}; const auto value = cf.string_utf16(unit); const auto utf8 = cf.utf8(value); process.objects().release(value); emit((spec + "s").c_str(), utf8.c_str()); break; }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
                emit((spec + conversion).c_str(), as_double(arguments.take())); break;
            case 'p': emit("0x%llx", static_cast<unsigned long long>(arguments.take())); break;
            case 's': {
                // 8-bit characters in the system C string encoding (Mac Roman
                // when they are not UTF-8); a precision bounds the bytes read.
                const auto pointer = arguments.take();
                const auto dot = spec.find('.');
                std::optional<std::uint64_t> precision;
                if (dot != std::string::npos) precision = std::strtoull(spec.c_str() + dot + 1, nullptr, 10);
                const auto width_spec = dot == std::string::npos ? spec : spec.substr(0, dot);
                if (!pointer) { emit((width_spec + "s").c_str(), "(null)"); break; }
                std::string bytes;
                for (std::uint64_t at = 0; !precision || at < *precision; ++at) {
                    if (at >= (1U << 20)) fail("unterminated C string");
                    const auto c = memory().read8(pointer + at);
                    if (!c) break;
                    bytes.push_back(static_cast<char>(c));
                }
                emit((width_spec + "s").c_str(), system_c_string(bytes).c_str());
                break;
            }
            case 'S': {
                const auto pointer = arguments.take(); std::string utf8 = "(null)";
                if (pointer) {
                    bool terminated{}; const auto bytes = memory().read_string(pointer, 2, 1U << 20, &terminated);
                    if (!terminated) fail("unterminated unichar string");
                    std::vector<std::uint16_t> units(bytes.size() / 2);
                    for (std::size_t at = 0; at < units.size(); ++at) units[at] = static_cast<std::uint16_t>(bytes[at * 2] | (bytes[at * 2 + 1] << 8));
                    const auto value = cf.string_utf16(units); utf8 = cf.utf8(value); process.objects().release(value);
                }
                emit((spec + "s").c_str(), utf8.c_str()); break;
            }
            case '@': { const auto value = description_of(arguments.take()); emit((spec + "s").c_str(), value.c_str()); break; }
            default: fail("unsupported format conversion %" + std::string(1, conversion));
            }
        }
        return output;
    }

    void install_literals() {
        const std::array<std::pair<const char*, std::pair<Address, LiteralClass>>, 5> literals{{
            {"NSConstantIntegerNumber", {number_class, LiteralClass::integer}}, {"NSConstantFloatNumber", {number_class, LiteralClass::floating}},
            {"NSConstantDoubleNumber", {number_class, LiteralClass::double_precision}}, {"NSConstantArray", {array_class, LiteralClass::array}},
            {"NSConstantDictionary", {dictionary_class, LiteralClass::dictionary}}}};
        for (const auto& [name, value] : literals) cf.set_literal_class(value.second, define(name, value.first));
    }

    void install_strings() {
        const auto s = string_class;
        factory(s, "string", "", [this](auto&, auto&) { return make_string(""); });
        factory(s, "stringWithFormat:", "initWithFormat:", [this](auto& state, auto& memory) {
            return make_string(format(text(state.x[2]), {memory, state.sp}));
        });
        method(s, "initWithFormat:arguments:", [this](auto& state, auto& memory) { replace(state, make_string(format(text(state.x[2]), {memory, state.x[3]}))); });
        factory(s, "stringWithCString:encoding:", "initWithCString:encoding:", [this](auto& state, auto&) {
            const auto encoding = cf_encoding(state.x[3]);
            if (!encoding || (*encoding != cf_ascii && *encoding != cf_utf8 && *encoding != cf_latin1)) return Address{0};
            return cf.invoke("CFStringCreateWithCString", std::array<Address, 3>{0, state.x[2], *encoding});
        }, "@@:*Q");
        factory(s, "stringWithCString:", "initWithCString:", [this](auto& state, auto&) {
            // The default C string encoding is treated as UTF-8 with a Latin-1 fallback.
            const auto value = cf.invoke("CFStringCreateWithCString", std::array<Address, 3>{0, state.x[2], cf_utf8});
            return value ? value : cf.invoke("CFStringCreateWithCString", std::array<Address, 3>{0, state.x[2], cf_latin1});
        }, "@@:*");
        factory(s, "stringWithCharacters:length:", "", [this](auto& state, auto&) { return cf.invoke("CFStringCreateWithCharacters", std::array<Address, 3>{0, state.x[2], state.x[3]}); });
        method(s, "initWithBytes:length:encoding:", [this](auto& state, auto&) {
            const auto encoding = cf_encoding(state.x[4]);
            replace(state, encoding ? cf.invoke("CFStringCreateWithBytes", std::array<Address, 5>{0, state.x[2], state.x[3], *encoding, 0}) : 0);
        });
        method(s, "initWithData:encoding:", [this](auto& state, auto&) {
            const auto encoding = cf_encoding(state.x[3]);
            const auto bytes = state.x[2] ? cf.invoke("CFDataGetBytePtr", std::array<Address, 1>{state.x[2]}) : 0;
            replace(state, encoding && state.x[2] ? cf.invoke("CFStringCreateWithBytes", std::array<Address, 5>{0, bytes, cf.length(state.x[2]), *encoding, 0}) : 0);
        });
        method(s, "cStringUsingEncoding:", [this](auto& state, auto&) {
            const auto encoding = cf_encoding(state.x[2]);
            state.x[0] = encoding ? cf.c_string(state.x[0], *encoding) : 0;
        }, false, "r*@:Q");
        method(s, "fileSystemRepresentation", [this](auto& state, auto&) {
            state.x[0] = cf.c_string(state.x[0], cf_utf8); if (!state.x[0]) fail("path is not representable in UTF-8");
        }, false, "r*@:");
        method(s, "getCString:maxLength:encoding:", [this](auto& state, auto&) {
            const auto encoding = cf_encoding(state.x[4]);
            state.x[0] = encoding && (*encoding == cf_ascii || *encoding == cf_utf8 || *encoding == cf_latin1)
                ? cf.invoke("CFStringGetCString", std::array<Address, 4>{state.x[0], state.x[2], state.x[3], *encoding}) : 0;
        }, false, "B@:*QQ");
        method(s, "getFileSystemRepresentation:maxLength:", [this](auto& state, auto&) {
            state.x[0] = cf.invoke("CFStringGetCString", std::array<Address, 4>{state.x[0], state.x[2], state.x[3], cf_utf8});
        }, false, "B@:*Q");
        const auto encoded_length = [this](Address object, Address ns_encoding) -> Address {
            const auto encoding = cf_encoding(ns_encoding);
            if (!encoding) return 0;
            const auto used = process.allocate_guest(8);
            try {
                const auto size = cf.length(object);
                const auto converted = cf.invoke("CFStringGetBytes", std::array<Address, 9>{object, 0, size, *encoding, 0, 0, 0, 0, used});
                const auto bytes = memory().read64(used); process.release_guest(used);
                return converted == size ? bytes : 0;
            } catch (...) { process.release_guest(used); throw; }
        };
        method(s, "lengthOfBytesUsingEncoding:", [encoded_length](auto& state, auto&) { state.x[0] = encoded_length(state.x[0], state.x[2]); }, false, "Q@:Q");
        method(s, "maximumLengthOfBytesUsingEncoding:", [this](auto& state, auto&) {
            const auto encoding = cf_encoding(state.x[2]);
            state.x[0] = encoding ? cf.invoke("CFStringGetMaximumSizeForEncoding", std::array<Address, 2>{cf.length(state.x[0]), *encoding}) : 0;
        }, false, "Q@:Q");
        const auto data_using = [this, encoded_length](Address object, Address ns_encoding, bool lossy) -> Address {
            const auto encoding = cf_encoding(ns_encoding);
            if (!encoding) return 0;
            const auto size = cf.length(object);
            const auto bytes = lossy ? 3 * size + 2 : encoded_length(object, ns_encoding);
            if (!lossy && !bytes && size) return 0;
            const auto buffer = process.allocate_guest(std::max<Address>(bytes, 1)), used = process.allocate_guest(8);
            try {
                static_cast<void>(cf.invoke("CFStringGetBytes", std::array<Address, 9>{object, 0, size, *encoding, lossy ? Address{'?'} : 0, 0, buffer, bytes, used}));
                const auto result = cf.invoke("CFDataCreate", std::array<Address, 3>{0, buffer, memory().read64(used)});
                process.release_guest(buffer); process.release_guest(used);
                return result;
            } catch (...) { process.release_guest(buffer); process.release_guest(used); throw; }
        };
        method(s, "dataUsingEncoding:", [this, data_using](auto& state, auto&) { state.x[0] = autorelease(data_using(state.x[0], state.x[2], false)); }, false, "@@:Q");
        method(s, "dataUsingEncoding:allowLossyConversion:", [this, data_using](auto& state, auto&) { state.x[0] = autorelease(data_using(state.x[0], state.x[2], (state.x[3] & 0xff) != 0)); }, false, "@@:QB");
        const auto substring = [this](Address object, Address location, Address length) {
            return autorelease(cf.invoke("CFStringCreateWithSubstring", std::array<Address, 4>{0, object, location, length}));
        };
        method(s, "substringWithRange:", [substring](auto& state, auto&) { state.x[0] = substring(state.x[0], state.x[2], state.x[3]); }, false, "@@:{_NSRange=QQ}");
        method(s, "substringFromIndex:", [this, substring](auto& state, auto&) { const auto size = cf.length(state.x[0]); if (state.x[2] > size) fail("substring index out of range"); state.x[0] = substring(state.x[0], state.x[2], size - state.x[2]); }, false, "@@:Q");
        method(s, "substringToIndex:", [substring](auto& state, auto&) { state.x[0] = substring(state.x[0], 0, state.x[2]); }, false, "@@:Q");
        method(s, "rangeOfString:", [this](auto& state, auto&) {
            const auto haystack = cf.units(state.x[0]), needle = cf.units(state.x[2]);
            const auto found = needle.empty() ? haystack.end() : std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end());
            if (found == haystack.end()) { state.x[0] = not_found; state.x[1] = 0; }
            else { state.x[0] = static_cast<Address>(found - haystack.begin()); state.x[1] = needle.size(); }
        }, false, "{_NSRange=QQ}@:@");
        method(s, "hasPrefix:", [this](auto& state, auto&) { const auto a = cf.units(state.x[0]), b = cf.units(state.x[2]); state.x[0] = !b.empty() && a.size() >= b.size() && std::equal(b.begin(), b.end(), a.begin()); }, false, "B@:@");
        method(s, "hasSuffix:", [this](auto& state, auto&) { const auto a = cf.units(state.x[0]), b = cf.units(state.x[2]); state.x[0] = !b.empty() && a.size() >= b.size() && std::equal(b.rbegin(), b.rend(), a.rbegin()); }, false, "B@:@");
        method(s, "containsString:", [this](auto& state, auto&) { const auto a = cf.units(state.x[0]), b = cf.units(state.x[2]); state.x[0] = !b.empty() && std::search(a.begin(), a.end(), b.begin(), b.end()) != a.end(); }, false, "B@:@");
        method(s, "caseInsensitiveCompare:", [this](auto& state, auto&) {
            auto fold = [](std::vector<std::uint16_t> units) { for (auto& unit : units) if (unit >= 'A' && unit <= 'Z') unit = static_cast<std::uint16_t>(unit - 'A' + 'a'); return units; };
            const auto a = fold(cf.units(state.x[0])), b = fold(cf.units(state.x[2]));
            state.x[0] = a < b ? static_cast<Address>(-1) : a > b ? 1 : 0;
        }, false, "q@:@");
        for (const auto upper : {false, true}) method(s, upper ? "uppercaseString" : "lowercaseString", [this, upper](auto& state, auto&) {
            auto units = cf.units(state.x[0]);
            for (auto& unit : units) {
                if (upper && unit >= 'a' && unit <= 'z') unit = static_cast<std::uint16_t>(unit - 'a' + 'A');
                else if (!upper && unit >= 'A' && unit <= 'Z') unit = static_cast<std::uint16_t>(unit - 'A' + 'a');
            }
            state.x[0] = autorelease(cf.string_utf16(units));
        });
        method(s, "stringByReplacingOccurrencesOfString:withString:", [this](auto& state, auto&) {
            const auto source = cf.units(state.x[0]), target = cf.units(state.x[2]), replacement = cf.units(state.x[3]);
            if (target.empty()) { state.x[0] = autorelease(cf.copy(state.x[0])); return; }
            std::vector<std::uint16_t> result;
            for (auto at = source.begin(); at != source.end();) {
                const auto found = std::search(at, source.end(), target.begin(), target.end());
                result.insert(result.end(), at, found);
                if (found == source.end()) break;
                result.insert(result.end(), replacement.begin(), replacement.end()); at = found + static_cast<std::ptrdiff_t>(target.size());
            }
            state.x[0] = autorelease(cf.string_utf16(result));
        });
        method(s, "componentsSeparatedByString:", [this](auto& state, auto&) {
            const auto source = cf.units(state.x[0]), separator = cf.units(state.x[2]);
            if (separator.empty()) fail("empty component separator");
            std::vector<Address> parts;
            try {
                for (auto at = source.begin();;) {
                    const auto found = std::search(at, source.end(), separator.begin(), separator.end());
                    parts.push_back(cf.string_utf16(std::vector<std::uint16_t>(at, found)));
                    if (found == source.end()) break;
                    at = found + static_cast<std::ptrdiff_t>(separator.size());
                }
                const auto result = cf.array(parts);
                for (const auto part : parts) process.objects().release(part);
                state.x[0] = autorelease(result);
            } catch (...) { for (const auto part : parts) process.objects().release(part); throw; }
        });
        method(s, "getCharacters:range:", [this](auto& state, auto&) {
            static_cast<void>(cf.invoke("CFStringGetCharacters", std::array<Address, 4>{state.x[0], state.x[3], state.x[4], state.x[2]}));
        }, false, "v@:^S{_NSRange=QQ}");
        // Path utilities (NSPathUtilities).
        const auto path_method = [this](Address cls, std::string name, auto operation) {
            method(cls, std::move(name), [this, operation](auto& state, auto&) { state.x[0] = auto_string(operation(text(state.x[0]))); });
        };
        path_method(s, "stringByStandardizingPath", [this](const std::string& value) { return trim_slashes(standardize(expand_tilde(value))); });
        path_method(s, "stringByExpandingTildeInPath", [this](const std::string& value) { return expand_tilde(value); });
        path_method(s, "lastPathComponent", [](const std::string& value) { return last_component(value); });
        path_method(s, "stringByDeletingLastPathComponent", [](const std::string& value) { return delete_last_component(value); });
        path_method(s, "pathExtension", [](const std::string& value) { return path_extension(value); });
        path_method(s, "stringByDeletingPathExtension", [](const std::string& value) { return delete_extension(value); });
        method(s, "stringByAppendingPathComponent:", [this](auto& state, auto&) { state.x[0] = auto_string(append_component(text(state.x[0]), text(state.x[2]))); });
        method(s, "stringByAppendingPathExtension:", [this](auto& state, auto&) { state.x[0] = auto_string(trim_slashes(text(state.x[0])) + "." + text(state.x[2])); });
        method(s, "isAbsolutePath", [this](auto& state, auto&) { const auto value = text(state.x[0]); state.x[0] = value.starts_with('/') || value.starts_with('~'); }, false, "B@:");
        method(s, "pathComponents", [this](auto& state, auto&) {
            const auto value = text(state.x[0]);
            auto parts = components(value);
            if (value.starts_with('/')) parts.insert(parts.begin(), "/");
            if (value.size() > 1 && value.ends_with('/')) parts.emplace_back("/");
            state.x[0] = autorelease(string_array(parts));
        });
        // Numeric scanning follows NSString: leading whitespace, then the longest valid prefix, else 0.
        const auto scan = [this](Address object) { const auto value = text(object); return std::strtod(value.c_str(), nullptr); };
        const auto scan_integer = [this](Address object) { const auto value = text(object); return static_cast<std::int64_t>(std::strtoll(value.c_str(), nullptr, 10)); };
        method(s, "intValue", [scan_integer](auto& state, auto&) { const auto value = std::clamp<std::int64_t>(scan_integer(state.x[0]), INT32_MIN, INT32_MAX); state.x[0] = static_cast<std::uint32_t>(static_cast<std::int32_t>(value)); }, false, "i@:");
        for (const auto* name : {"integerValue", "longLongValue"}) method(s, name, [scan_integer](auto& state, auto&) { state.x[0] = static_cast<Address>(scan_integer(state.x[0])); }, false, "q@:");
        method(s, "doubleValue", [scan](auto& state, auto&) { return_double(state, scan(state.x[0])); }, false, "d@:");
        method(s, "floatValue", [scan](auto& state, auto&) { state.v[0] = {std::bit_cast<std::uint32_t>(static_cast<float>(scan(state.x[0]))), 0}; }, false, "f@:");
        method(s, "boolValue", [this](auto& state, auto&) {
            auto value = text(state.x[0]); value.erase(0, value.find_first_not_of(" \t\n"));
            if (!value.empty() && (value[0] == '+' || value[0] == '-')) value.erase(0, 1);
            value.erase(0, value.find_first_not_of('0'));
            state.x[0] = !value.empty() && (value[0] == 'Y' || value[0] == 'y' || value[0] == 'T' || value[0] == 't' || (value[0] >= '1' && value[0] <= '9'));
        }, false, "B@:");
        method(s, "mutableCopy", [this](auto& state, auto&) { state.x[0] = cf.mutable_copy(state.x[0]); });
    }
    std::string home() {
        for (const auto& entry : process.environment) if (entry.starts_with("HOME=") && entry.size() > 5) return trim_slashes(entry.substr(5));
#ifdef _WIN32
        if (const auto profile = host_variable("USERPROFILE")) return trim_slashes(guest_path_string(utf8_path(*profile)));
#else
        if (const auto value = host_variable("HOME")) return trim_slashes(*value);
#endif
        return "/";
    }
    std::string expand_tilde(const std::string& value) {
        if (value == "~" || value.starts_with("~/")) return home() + value.substr(1);
        return value;
    }
    Address string_array(const std::vector<std::string>& values) {
        std::vector<Address> objects;
        try {
            for (const auto& value : values) objects.push_back(make_string(value));
            const auto result = cf.array(objects);
            for (const auto value : objects) process.objects().release(value);
            return result;
        } catch (...) { for (const auto value : objects) process.objects().release(value); throw; }
    }

    // NSNumber accessors; booleans are CFBoolean objects with NSNumber behavior.
    std::int64_t integer_of(Address object) {
        if (kind(object) == ObjectKind::boolean) return cf.invoke("CFBooleanGetValue", std::array<Address, 1>{object}) ? 1 : 0;
        const auto scratch = process.allocate_guest(8);
        try {
            static_cast<void>(cf.invoke("CFNumberGetValue", std::array<Address, 3>{object, 4, scratch}));
            const auto value = std::bit_cast<std::int64_t>(memory().read64(scratch)); process.release_guest(scratch); return value;
        } catch (...) { process.release_guest(scratch); throw; }
    }
    double double_of(Address object) {
        const auto value = cf.number_value(object);
        if (value) return *value;
        // NSNumber subclasses that are not CF numbers (NSDecimalNumber).
        if (object && kind(object) != ObjectKind::number && objc.responds(object, objc.selector("doubleValue"))) return std::bit_cast<double>(objc.send(object, objc.selector("doubleValue")).v[0][0]);
        fail("expected an NSNumber");
    }
    Address number(std::int64_t value, unsigned type = 4) { return cf.integer(value, type); }
    void install_numbers() {
        const auto n = number_class;
        const std::array<std::tuple<const char*, const char*, unsigned, int, const char*>, 10> integers{{
            {"numberWithChar:", "initWithChar:", 7, 8, "@@:c"}, {"numberWithUnsignedChar:", "initWithUnsignedChar:", 8, -8, "@@:C"},
            {"numberWithShort:", "initWithShort:", 8, 16, "@@:s"}, {"numberWithUnsignedShort:", "initWithUnsignedShort:", 9, -16, "@@:S"},
            {"numberWithUnsignedInt:", "initWithUnsignedInt:", 4, -32, "@@:I"}, {"numberWithLong:", "initWithLong:", 10, 64, "@@:q"},
            {"numberWithUnsignedLong:", "initWithUnsignedLong:", 4, -64, "@@:Q"}, {"numberWithInteger:", "initWithInteger:", 15, 64, "@@:q"},
            {"numberWithUnsignedInteger:", "initWithUnsignedInteger:", 4, -64, "@@:Q"}, {"numberWithUnsignedLongLong:", "initWithUnsignedLongLong:", 4, -64, "@@:Q"}}};
        for (const auto& [make, init, type, bits, types] : integers) {
            const auto width = bits < 0 ? -bits : bits; const bool is_signed = bits > 0;
            factory(n, make, init, [this, type, width, is_signed](auto& state, auto&) {
                const auto raw = state.x[2];
                std::int64_t value;
                if (width == 64) {
                    if (!is_signed && raw > static_cast<Address>(INT64_MAX)) return cf.real(static_cast<double>(raw)); // Precision beyond SInt64 is not represented.
                    value = std::bit_cast<std::int64_t>(raw);
                } else if (is_signed) { const auto shift = 64 - width; value = std::bit_cast<std::int64_t>(raw << shift) >> shift; }
                else value = static_cast<std::int64_t>(raw & ((1ULL << width) - 1));
                return number(value, type);
            }, types);
        }
        factory(n, "numberWithFloat:", "initWithFloat:", [this](auto& state, auto&) { return cf.real(std::bit_cast<float>(static_cast<std::uint32_t>(state.v[0][0]))); }, "@@:f");
        const auto integer_getter = [this](Address cls, const char* name, int width, bool is_signed, const char* types) {
            method(cls, name, [this, width, is_signed](auto& state, auto&) {
                // Apple ARM64 returns sub-32-bit integers extended to 32 bits by the callee.
                const auto value = integer_of(state.x[0]);
                if (width == 64) state.x[0] = static_cast<Address>(value);
                else if (is_signed) { const auto shift = 64 - width; state.x[0] = static_cast<std::uint32_t>(static_cast<std::int32_t>((value << shift) >> shift)); }
                else state.x[0] = static_cast<Address>(value) & ((1ULL << width) - 1);
            }, false, types);
        };
        integer_getter(n, "charValue", 8, true, "c@:"); integer_getter(n, "unsignedCharValue", 8, false, "C@:");
        integer_getter(n, "shortValue", 16, true, "s@:"); integer_getter(n, "unsignedShortValue", 16, false, "S@:");
        integer_getter(n, "unsignedIntValue", 32, false, "I@:"); integer_getter(n, "longValue", 64, true, "q@:");
        integer_getter(n, "unsignedLongValue", 64, false, "Q@:"); integer_getter(n, "integerValue", 64, true, "q@:");
        integer_getter(n, "unsignedIntegerValue", 64, false, "Q@:"); integer_getter(n, "unsignedLongLongValue", 64, false, "Q@:");
        method(n, "floatValue", [this](auto& state, auto&) { state.v[0] = {std::bit_cast<std::uint32_t>(static_cast<float>(double_of(state.x[0]))), 0}; }, false, "f@:");
        method(n, "stringValue", [this](auto& state, auto&) { state.x[0] = autorelease(cf.description(state.x[0])); });
        method(n, "compare:", [this](auto& state, auto&) {
            if (kind(state.x[0]) == ObjectKind::number && kind(state.x[2]) == ObjectKind::number) { state.x[0] = cf.invoke("CFNumberCompare", std::array<Address, 3>{state.x[0], state.x[2], 0}); return; }
            const auto a = double_of(state.x[0]), b = double_of(state.x[2]);
            state.x[0] = a < b ? static_cast<Address>(-1) : a > b ? 1 : 0;
        }, false, "q@:@");
        method(n, "objCType", [this](auto& state, auto&) {
            if (kind(state.x[0]) == ObjectKind::boolean) { state.x[0] = constant_c_string('c'); return; }
            const auto type = cf.invoke("CFNumberGetType", std::array<Address, 1>{state.x[0]});
            constexpr std::string_view codes = "?csiqfdcsiqqfdqqd";
            state.x[0] = constant_c_string(type < codes.size() ? codes[static_cast<std::size_t>(type)] : 'q');
        }, false, "r*@:");
    }

    // Collections. Fast enumeration: NSFastEnumerationState {state, itemsPtr, mutationsPtr, extra[5]}.
    Address enumerate(GuestMemory& memory, Address state, Address buffer, Address capacity, const std::vector<Address>& values) {
        const auto start = memory.read64(state);
        if (start == 0) memory.write64(state + 24, 0);
        if (start >= values.size() || !capacity) return 0;
        const auto count = std::min<Address>(capacity, values.size() - start);
        for (Address index = 0; index < count; ++index) memory.write64(buffer + index * 8, values[static_cast<std::size_t>(start + index)]);
        memory.write64(state, start + count); memory.write64(state + 8, buffer); memory.write64(state + 16, state + 24);
        return count;
    }
    std::vector<Address> merge_sort(std::vector<Address> values, const std::function<std::int64_t(Address, Address)>& compare) {
        std::vector<Address> scratch(values.size());
        for (std::size_t width = 1; width < values.size(); width *= 2) {
            for (std::size_t low = 0; low < values.size(); low += 2 * width) {
                const auto middle = std::min(low + width, values.size()), high = std::min(low + 2 * width, values.size());
                std::size_t left = low, right = middle, out = low;
                while (left < middle && right < high) scratch[out++] = compare(values[right], values[left]) < 0 ? values[right++] : values[left++];
                while (left < middle) scratch[out++] = values[left++];
                while (right < high) scratch[out++] = values[right++];
            }
            values.swap(scratch);
        }
        return values;
    }
    void install_collections() {
        const auto a = array_class, d = dictionary_class;
        mutable_array = define("NSMutableArray", a); mutable_dictionary = define("NSMutableDictionary", d); mutable_data = define("NSMutableData", data_class);
        cf.set_mutable_class(ObjectKind::array, mutable_array); cf.set_mutable_class(ObjectKind::dictionary, mutable_dictionary); cf.set_mutable_class(ObjectKind::data, mutable_data);
        const auto make_array = [this](std::span<const Address> values, bool mutable_object) { return cf.array(values, mutable_object); };
        for (const auto [cls, mutable_object] : {std::pair{a, false}, std::pair{mutable_array, true}}) {
            const auto m = mutable_object;
            factory(cls, "array", "init", [make_array, m](auto&, auto&) { return make_array({}, m); });
            factory(cls, "arrayWithArray:", "initWithArray:", [this, make_array, m](auto& state, auto&) { const auto values = state.x[2] ? cf.array_values(state.x[2]) : std::vector<Address>{}; return make_array(values, m); });
            factory(cls, "arrayWithObject:", "", [make_array, m](auto& state, auto&) { const std::array<Address, 1> values{state.x[2]}; return make_array(values, m); });
            factory(cls, "arrayWithObjects:count:", "initWithObjects:count:", [this, make_array, m](auto& state, auto&) { return make_array(guest_objects(state.x[2], state.x[3]), m); }, "@@:^@Q");
            factory(cls, "arrayWithObjects:", "initWithObjects:", [this, make_array, m](auto& state, auto& memory) { return make_array(nil_terminated(state.x[2], {memory, state.sp}), m); });
        }
        factory(mutable_array, "arrayWithCapacity:", "initWithCapacity:", [make_array](auto&, auto&) { return make_array({}, true); }, "@@:Q");
        method(a, "firstObject", [this](auto& state, auto&) { const auto values = cf.array_values(state.x[0]); state.x[0] = values.empty() ? 0 : values.front(); });
        method(a, "lastObject", [this](auto& state, auto&) { const auto values = cf.array_values(state.x[0]); state.x[0] = values.empty() ? 0 : values.back(); });
        method(a, "containsObject:", [this](auto& state, auto&) { const auto values = cf.array_values(state.x[0]); state.x[0] = std::any_of(values.begin(), values.end(), [&](Address value) { return same(value, state.x[2]); }); }, false, "B@:@");
        method(a, "indexOfObject:", [this](auto& state, auto&) {
            const auto values = cf.array_values(state.x[0]); const auto target = state.x[2]; state.x[0] = not_found;
            for (std::size_t index = 0; index < values.size(); ++index) if (same(values[index], target)) { state.x[0] = index; break; }
        }, false, "Q@:@");
        method(a, "indexOfObjectIdenticalTo:", [this](auto& state, auto&) {
            const auto values = cf.array_values(state.x[0]); const auto found = std::find(values.begin(), values.end(), state.x[2]);
            state.x[0] = found == values.end() ? not_found : static_cast<Address>(found - values.begin());
        }, false, "Q@:@");
        method(a, "countByEnumeratingWithState:objects:count:", [this](auto& state, auto& memory) {
            state.x[0] = enumerate(memory, state.x[2], state.x[3], state.x[4], cf.array_values(state.x[0]));
        }, false, "Q@:^v^@Q");
        method(a, "arrayByAddingObject:", [this](auto& state, auto&) { auto values = cf.array_values(state.x[0]); values.push_back(state.x[2]); state.x[0] = autorelease(cf.array(values)); });
        method(a, "arrayByAddingObjectsFromArray:", [this](auto& state, auto&) { auto values = cf.array_values(state.x[0]); const auto more = cf.array_values(state.x[2]); values.insert(values.end(), more.begin(), more.end()); state.x[0] = autorelease(cf.array(values)); });
        method(a, "componentsJoinedByString:", [this](auto& state, auto&) {
            const auto separator = state.x[2] ? text(state.x[2]) : std::string{}; std::string result; bool first = true;
            for (const auto value : cf.array_values(state.x[0])) { if (!first) result += separator; first = false; result += description_of(value); }
            state.x[0] = auto_string(result);
        });
        method(a, "isEqualToArray:", [this](auto& state, auto&) {
            const auto left = cf.array_values(state.x[0]); const auto right = state.x[2] ? cf.array_values(state.x[2]) : std::vector<Address>{};
            bool equal = state.x[2] && left.size() == right.size();
            for (std::size_t index = 0; equal && index < left.size(); ++index) equal = same(left[index], right[index]);
            state.x[0] = equal;
        }, false, "B@:@");
        method(a, "enumerateObjectsUsingBlock:", [this](auto& state, auto&) {
            const auto block = state.x[2]; const auto invoke = block_invoke(block);
            const auto values = cf.array_values(state.x[0]); const auto stop = process.allocate_guest(8);
            try {
                memory().write64(stop, 0);
                for (std::size_t index = 0; index < values.size(); ++index) {
                    const std::array<Address, 4> arguments{block, values[index], index, stop};
                    static_cast<void>(process.calling_engine().invoke_callback(invoke, arguments));
                    if (memory().read8(stop)) break;
                }
                process.release_guest(stop);
            } catch (...) { process.release_guest(stop); throw; }
        }, false, "v@:@?");
        const auto sorted = [this](Address array, Address block) {
            const auto invoke = block_invoke(block);
            return merge_sort(cf.array_values(array), [this, invoke, block](Address left, Address right) {
                const std::array<Address, 3> arguments{block, left, right};
                return std::bit_cast<std::int64_t>(process.calling_engine().invoke_callback(invoke, arguments));
            });
        };
        method(a, "sortedArrayUsingComparator:", [this, sorted](auto& state, auto&) { state.x[0] = autorelease(cf.array(sorted(state.x[0], state.x[2]))); }, false, "@@:@?");
        // Test blocks: BOOL (^)(id obj, NSUInteger idx, BOOL *stop). NSEnumerationReverse (2)
        // is honoured; NSEnumerationConcurrent (1) runs sequentially (an allowed ordering).
        const auto passing = [this](Address array, Address block, std::uint64_t options, bool all) {
            const auto invoke = block_invoke(block);
            const auto values = cf.array_values(array);
            const auto stop = process.allocate_guest(8);
            std::vector<std::size_t> found;
            try {
                memory().write64(stop, 0);
                for (std::size_t step = 0; step < values.size(); ++step) {
                    const auto index = (options & 2) ? values.size() - 1 - step : step;
                    const std::array<Address, 4> arguments{block, values[index], index, stop};
                    if (process.calling_engine().invoke_callback(invoke, arguments) & 0xff) {
                        found.push_back(index);
                        if (!all) break;
                    }
                    if (memory().read8(stop)) break;
                }
                process.release_guest(stop);
            } catch (...) { process.release_guest(stop); throw; }
            return found;
        };
        method(a, "indexOfObjectPassingTest:", [passing](auto& state, auto&) {
            const auto found = passing(state.x[0], state.x[2], 0, false);
            state.x[0] = found.empty() ? static_cast<Address>(INT64_MAX) : found.front();   // NSNotFound
        }, false, "Q@:@?");
        method(a, "indexOfObjectWithOptions:passingTest:", [passing](auto& state, auto&) {
            const auto found = passing(state.x[0], state.x[3], state.x[2], false);
            state.x[0] = found.empty() ? static_cast<Address>(INT64_MAX) : found.front();
        }, false, "Q@:Q@?");
        const auto index_set = [this](std::vector<std::size_t> indexes) {
            const auto cls = objc.find_class("NSMutableIndexSet");
            if (!cls) fail("NSArray index-set results need NSMutableIndexSet");
            const auto set = send(send(cls, "alloc"), "init");
            std::sort(indexes.begin(), indexes.end());
            for (const auto index : indexes) send(set, "addIndex:", {static_cast<Address>(index)});
            return autorelease(set);
        };
        method(a, "indexesOfObjectsPassingTest:", [passing, index_set](auto& state, auto&) { state.x[0] = index_set(passing(state.x[0], state.x[2], 0, true)); }, false, "@@:@?");
        method(a, "indexesOfObjectsWithOptions:passingTest:", [passing, index_set](auto& state, auto&) { state.x[0] = index_set(passing(state.x[0], state.x[3], state.x[2], true)); }, false, "@@:Q@?");
        method(a, "enumerateObjectsWithOptions:usingBlock:", [this](auto& state, auto&) {
            const auto block = state.x[3]; const auto invoke = block_invoke(block);
            const auto values = cf.array_values(state.x[0]); const auto stop = process.allocate_guest(8);
            try {
                memory().write64(stop, 0);
                for (std::size_t step = 0; step < values.size(); ++step) {
                    const auto index = (state.x[2] & 2) ? values.size() - 1 - step : step;
                    const std::array<Address, 4> arguments{block, values[index], index, stop};
                    static_cast<void>(process.calling_engine().invoke_callback(invoke, arguments));
                    if (memory().read8(stop)) break;
                }
                process.release_guest(stop);
            } catch (...) { process.release_guest(stop); throw; }
        }, false, "v@:Q@?");
        method(a, "mutableCopy", [this](auto& state, auto&) { state.x[0] = cf.mutable_copy(state.x[0]); });
        method(a, "mutableCopyWithZone:", [this](auto& state, auto&) { state.x[0] = cf.mutable_copy(state.x[0]); });
        const auto put = [this](Address array, std::string_view operation, std::initializer_list<Address> arguments) {
            if (!cf.is_mutable(array)) fail("mutation of an immutable NSArray");
            static_cast<void>(cf.invoke(operation, std::span(arguments.begin(), arguments.size())));
        };
        const auto object_required = [](Address value) { if (!value) fail("attempt to insert nil object"); return value; };
        const auto ma = mutable_array;
        method(ma, "addObject:", [put, object_required](auto& state, auto&) { put(state.x[0], "CFArrayAppendValue", {state.x[0], object_required(state.x[2])}); }, false, "v@:@");
        method(ma, "insertObject:atIndex:", [put, object_required](auto& state, auto&) { put(state.x[0], "CFArrayInsertValueAtIndex", {state.x[0], state.x[3], object_required(state.x[2])}); }, false, "v@:@Q");
        method(ma, "replaceObjectAtIndex:withObject:", [put, object_required](auto& state, auto&) { put(state.x[0], "CFArraySetValueAtIndex", {state.x[0], state.x[2], object_required(state.x[3])}); }, false, "v@:Q@");
        method(ma, "setObject:atIndexedSubscript:", [put, object_required](auto& state, auto&) { put(state.x[0], "CFArraySetValueAtIndex", {state.x[0], state.x[3], object_required(state.x[2])}); }, false, "v@:@Q");
        method(ma, "removeObjectAtIndex:", [put](auto& state, auto&) { put(state.x[0], "CFArrayRemoveValueAtIndex", {state.x[0], state.x[2]}); }, false, "v@:Q");
        method(ma, "removeLastObject", [this, put](auto& state, auto&) { const auto size = cf.length(state.x[0]); if (size) put(state.x[0], "CFArrayRemoveValueAtIndex", {state.x[0], size - 1}); }, false, "v@:");
        method(ma, "removeAllObjects", [put](auto& state, auto&) { put(state.x[0], "CFArrayRemoveAllValues", {state.x[0]}); }, false, "v@:");
        method(ma, "addObjectsFromArray:", [this, put](auto& state, auto&) { for (const auto value : cf.array_values(state.x[2])) put(state.x[0], "CFArrayAppendValue", {state.x[0], value}); }, false, "v@:@");
        for (const auto identical : {false, true}) method(ma, identical ? "removeObjectIdenticalTo:" : "removeObject:", [this, put, identical](auto& state, auto&) {
            const auto values = cf.array_values(state.x[0]);
            for (auto index = values.size(); index-- > 0;)
                if (identical ? values[index] == state.x[2] : same(values[index], state.x[2])) put(state.x[0], "CFArrayRemoveValueAtIndex", {state.x[0], index});
        }, false, "v@:@");
        method(ma, "sortUsingComparator:", [this, sorted, put](auto& state, auto&) {
            const auto order = sorted(state.x[0], state.x[2]);
            const auto hold = cf.array(order); // Keeps elements alive while the array is rebuilt.
            try { put(state.x[0], "CFArrayRemoveAllValues", {state.x[0]}); for (const auto value : order) put(state.x[0], "CFArrayAppendValue", {state.x[0], value}); }
            catch (...) { process.objects().release(hold); throw; }
            process.objects().release(hold);
        }, false, "v@:@?");
        method(ma, "copy", [this](auto& state, auto&) { state.x[0] = cf.copy(state.x[0]); });
        method(ma, "copyWithZone:", [this](auto& state, auto&) { state.x[0] = cf.copy(state.x[0]); });

        const auto make_dictionary = [this](const std::vector<std::pair<Address, Address>>& values, bool mutable_object) { return cf.dictionary(values, mutable_object); };
        for (const auto [cls, mutable_object] : {std::pair{d, false}, std::pair{mutable_dictionary, true}}) {
            const auto m = mutable_object;
            factory(cls, "dictionary", "init", [make_dictionary, m](auto&, auto&) { return make_dictionary({}, m); });
            factory(cls, "dictionaryWithObject:forKey:", "", [make_dictionary, m](auto& state, auto&) { return make_dictionary({{state.x[3], state.x[2]}}, m); });
            // object, key, object, key, ..., nil (variadic after the first object).
            factory(cls, "dictionaryWithObjectsAndKeys:", "initWithObjectsAndKeys:", [this, make_dictionary, m](auto& state, auto& memory) {
                const auto values = nil_terminated(state.x[2], {memory, state.sp});
                if (values.size() % 2) fail("dictionaryWithObjectsAndKeys: needs an object/key pair list (odd count before nil)");
                std::vector<std::pair<Address, Address>> entries;
                for (std::size_t index = 0; index < values.size(); index += 2) entries.emplace_back(values[index + 1], values[index]);
                return make_dictionary(entries, m);
            });
            factory(cls, "dictionaryWithDictionary:", "initWithDictionary:", [this, make_dictionary, m](auto& state, auto&) { return make_dictionary(state.x[2] ? cf.dictionary_entries(state.x[2]) : std::vector<std::pair<Address, Address>>{}, m); });
            factory(cls, "dictionaryWithObjects:forKeys:count:", "initWithObjects:forKeys:count:", [this, make_dictionary, m](auto& state, auto&) {
                const auto values = guest_objects(state.x[2], state.x[4]), keys = guest_objects(state.x[3], state.x[4]);
                std::vector<std::pair<Address, Address>> entries;
                for (std::size_t index = 0; index < keys.size(); ++index) entries.emplace_back(keys[index], values[index]);
                return make_dictionary(entries, m);
            }, "@@:^@^@Q");
            factory(cls, "dictionaryWithObjects:forKeys:", "initWithObjects:forKeys:", [this, make_dictionary, m](auto& state, auto&) {
                const auto values = cf.array_values(state.x[2]), keys = cf.array_values(state.x[3]);
                if (values.size() != keys.size()) fail("dictionary key and value counts differ");
                std::vector<std::pair<Address, Address>> entries;
                for (std::size_t index = 0; index < keys.size(); ++index) entries.emplace_back(keys[index], values[index]);
                return make_dictionary(entries, m);
            });
        }
        factory(mutable_dictionary, "dictionaryWithCapacity:", "initWithCapacity:", [make_dictionary](auto&, auto&) { return make_dictionary({}, true); }, "@@:Q");
        const auto entries_part = [this](Address dictionary, bool keys) {
            std::vector<Address> values;
            for (const auto& [key, value] : cf.dictionary_entries(dictionary)) values.push_back(keys ? key : value);
            return values;
        };
        method(d, "allKeys", [this, entries_part](auto& state, auto&) { state.x[0] = autorelease(cf.array(entries_part(state.x[0], true))); });
        method(d, "allValues", [this, entries_part](auto& state, auto&) { state.x[0] = autorelease(cf.array(entries_part(state.x[0], false))); });
        method(d, "valueForKey:", [this](auto& state, auto&) { state.x[0] = cf.dictionary_value(state.x[0], state.x[2]); });
        method(d, "countByEnumeratingWithState:objects:count:", [this, entries_part](auto& state, auto& memory) {
            state.x[0] = enumerate(memory, state.x[2], state.x[3], state.x[4], entries_part(state.x[0], true));
        }, false, "Q@:^v^@Q");
        method(d, "isEqualToDictionary:", [this](auto& state, auto&) { state.x[0] = state.x[2] && cf.equal(state.x[0], state.x[2]); }, false, "B@:@");
        method(d, "enumerateKeysAndObjectsUsingBlock:", [this](auto& state, auto&) {
            const auto block = state.x[2]; const auto invoke = block_invoke(block);
            const auto entries = cf.dictionary_entries(state.x[0]); const auto stop = process.allocate_guest(8);
            try {
                memory().write64(stop, 0);
                for (const auto& [key, value] : entries) {
                    const std::array<Address, 4> arguments{block, key, value, stop};
                    static_cast<void>(process.calling_engine().invoke_callback(invoke, arguments));
                    if (memory().read8(stop)) break;
                }
                process.release_guest(stop);
            } catch (...) { process.release_guest(stop); throw; }
        }, false, "v@:@?");
        method(d, "mutableCopy", [this](auto& state, auto&) { state.x[0] = cf.mutable_copy(state.x[0]); });
        const auto md = mutable_dictionary;
        const auto dictionary_put = [this](Address dictionary, std::string_view operation, std::initializer_list<Address> arguments) {
            if (!cf.is_mutable(dictionary)) fail("mutation of an immutable NSDictionary");
            static_cast<void>(cf.invoke(operation, std::span(arguments.begin(), arguments.size())));
        };
        method(md, "setObject:forKey:", [dictionary_put](auto& state, auto&) { if (!state.x[2] || !state.x[3]) fail("nil dictionary key or value"); dictionary_put(state.x[0], "CFDictionarySetValue", {state.x[0], state.x[3], state.x[2]}); }, false, "v@:@@");
        method(md, "setObject:forKeyedSubscript:", [dictionary_put](auto& state, auto&) {
            if (!state.x[3]) fail("nil dictionary key");
            if (state.x[2]) dictionary_put(state.x[0], "CFDictionarySetValue", {state.x[0], state.x[3], state.x[2]});
            else dictionary_put(state.x[0], "CFDictionaryRemoveValue", {state.x[0], state.x[3]});
        }, false, "v@:@@");
        method(md, "setValue:forKey:", [dictionary_put](auto& state, auto&) {
            if (state.x[2]) dictionary_put(state.x[0], "CFDictionarySetValue", {state.x[0], state.x[3], state.x[2]});
            else dictionary_put(state.x[0], "CFDictionaryRemoveValue", {state.x[0], state.x[3]});
        }, false, "v@:@@");
        method(md, "removeObjectForKey:", [dictionary_put](auto& state, auto&) { dictionary_put(state.x[0], "CFDictionaryRemoveValue", {state.x[0], state.x[2]}); }, false, "v@:@");
        method(md, "removeAllObjects", [dictionary_put](auto& state, auto&) { dictionary_put(state.x[0], "CFDictionaryRemoveAllValues", {state.x[0]}); }, false, "v@:");
        method(md, "addEntriesFromDictionary:", [this, dictionary_put](auto& state, auto&) {
            for (const auto& [key, value] : cf.dictionary_entries(state.x[2])) dictionary_put(state.x[0], "CFDictionarySetValue", {state.x[0], key, value});
        }, false, "v@:@");
        method(md, "copy", [this](auto& state, auto&) { state.x[0] = cf.copy(state.x[0]); });
        // NSData mutability and accessors beyond the core bridge.
        factory(mutable_data, "data", "init", [this](auto&, auto&) { return cf.invoke("CFDataCreateMutable", std::array<Address, 2>{0, 0}); });
        factory(mutable_data, "dataWithCapacity:", "initWithCapacity:", [this](auto&, auto&) { return cf.invoke("CFDataCreateMutable", std::array<Address, 2>{0, 0}); }, "@@:Q");
        factory(mutable_data, "dataWithLength:", "initWithLength:", [this](auto& state, auto&) {
            const auto value = cf.invoke("CFDataCreateMutable", std::array<Address, 2>{0, 0});
            try { static_cast<void>(cf.invoke("CFDataSetLength", std::array<Address, 2>{value, state.x[2]})); } catch (...) { process.objects().release(value); throw; }
            return value;
        }, "@@:Q");
        method(mutable_data, "appendBytes:length:", [this](auto& state, auto&) { static_cast<void>(cf.invoke("CFDataAppendBytes", std::array<Address, 3>{state.x[0], state.x[2], state.x[3]})); }, false, "v@:^vQ");
        method(mutable_data, "setLength:", [this](auto& state, auto&) { static_cast<void>(cf.invoke("CFDataSetLength", std::array<Address, 2>{state.x[0], state.x[2]})); }, false, "v@:Q");
        method(mutable_data, "appendData:", [this](auto& state, auto&) {
            if (!state.x[2]) return;
            const auto length = cf.length(state.x[2]);
            if (length) static_cast<void>(cf.invoke("CFDataAppendBytes", std::array<Address, 3>{state.x[0], cf.invoke("CFDataGetBytePtr", std::array<Address, 1>{state.x[2]}), length}));
        }, false, "v@:@");
        method(mutable_data, "increaseLengthBy:", [this](auto& state, auto&) { static_cast<void>(cf.invoke("CFDataIncreaseLength", std::array<Address, 2>{state.x[0], state.x[2]})); }, false, "v@:Q");
        const auto replace_bytes = [this](Address data, Address location, Address length, Address bytes, Address count) {
            const auto size = cf.length(data);
            if (location > size || length > size - location) fail("NSRangeException: -[NSMutableData replaceBytesInRange:] beyond the data");
            static_cast<void>(cf.invoke("CFDataReplaceBytes", std::array<Address, 5>{data, location, length, bytes, count}));
        };
        method(mutable_data, "replaceBytesInRange:withBytes:", [replace_bytes](auto& state, auto&) { replace_bytes(state.x[0], state.x[2], state.x[3], state.x[4], state.x[3]); }, false, "v@:{_NSRange=QQ}^v");
        method(mutable_data, "replaceBytesInRange:withBytes:length:", [replace_bytes](auto& state, auto&) { replace_bytes(state.x[0], state.x[2], state.x[3], state.x[4], state.x[5]); }, false, "v@:{_NSRange=QQ}^vQ");
        method(mutable_data, "mutableBytes", [this](auto& state, auto&) { state.x[0] = cf.invoke("CFDataGetMutableBytePtr", std::array<Address, 1>{state.x[0]}); }, false, "^v@:");
        method(data_class, "getBytes:length:", [this](auto& state, auto&) {
            const auto size = std::min(cf.length(state.x[0]), state.x[3]);
            static_cast<void>(cf.invoke("CFDataGetBytes", std::array<Address, 4>{state.x[0], 0, size, state.x[2]}));
        }, false, "v@:^vQ");
        // Ranges are (location x2/x3.., length) as NSRange in two registers.
        const auto check_range = [this](Address data, Address location, Address length, const char* what) {
            const auto size = cf.length(data);
            if (location > size || length > size - location)
                fail(std::string("NSRangeException: -[NSData ") + what + "] range {" + std::to_string(location) + ", " + std::to_string(length) + "} beyond " + std::to_string(size));
        };
        method(data_class, "getBytes:range:", [this, check_range](auto& state, auto&) {
            check_range(state.x[0], state.x[3], state.x[4], "getBytes:range:");
            static_cast<void>(cf.invoke("CFDataGetBytes", std::array<Address, 4>{state.x[0], state.x[3], state.x[4], state.x[2]}));
        }, false, "v@:^v{_NSRange=QQ}");
        method(data_class, "subdataWithRange:", [this, check_range](auto& state, auto&) {
            check_range(state.x[0], state.x[2], state.x[3], "subdataWithRange:");
            const auto bytes = cf.invoke("CFDataGetBytePtr", std::array<Address, 1>{state.x[0]});
            state.x[0] = autorelease(cf.invoke("CFDataCreate", std::array<Address, 3>{0, state.x[3] ? bytes + state.x[2] : 0, state.x[3]}));
        }, false, "@@:{_NSRange=QQ}");
        method(data_class, "mutableCopy", [this](auto& state, auto&) { state.x[0] = cf.mutable_copy(state.x[0]); });
    }

    // NSSet/NSMutableSet: native instances; membership uses -isEqual:.
    Address make_set(Address cls, const std::vector<Address>& values, bool is_mutable) {
        const auto object = objc.allocate(cls);
        try { initialize_set(object, values, is_mutable); } catch (...) { release(object); throw; }
        return object;
    }
    void initialize_set(Address object, const std::vector<Address>& values, bool is_mutable) {
        auto set = attach<SetState>(object);
        set->mutable_set = is_mutable;
        for (const auto value : values) add_member(*set, value);
    }
    void add_member(SetState& set, Address value) {
        if (!value) fail("attempt to insert nil into NSSet");
        std::vector<Address> snapshot;
        { std::lock_guard lock(set.mutex); snapshot = set.values; }
        if (std::any_of(snapshot.begin(), snapshot.end(), [&](Address member) { return same(member, value); })) return;
        const auto retained = retain(value);
        std::lock_guard lock(set.mutex); set.values.push_back(retained); ++set.mutations;
    }
    std::vector<Address> members(Address object) { const auto set = state_of<SetState>(object); std::lock_guard lock(set->mutex); return set->values; }
    void install_sets() {
        set_class = define("NSSet", root); mutable_set = define("NSMutableSet", set_class);
        for (const auto [cls, mutable_object] : {std::pair{set_class, false}, std::pair{mutable_set, true}}) {
            const auto m = mutable_object; const auto target = cls;
            method(cls, "set", [this, target, m](auto& state, auto&) { state.x[0] = autorelease(make_set(target, {}, m)); }, true);
            method(cls, "setWithArray:", [this, target, m](auto& state, auto&) { state.x[0] = autorelease(make_set(target, state.x[2] ? cf.array_values(state.x[2]) : std::vector<Address>{}, m)); }, true);
            method(cls, "setWithObject:", [this, target, m](auto& state, auto&) { state.x[0] = autorelease(make_set(target, {state.x[2]}, m)); }, true);
            method(cls, "setWithObjects:count:", [this, target, m](auto& state, auto&) { state.x[0] = autorelease(make_set(target, guest_objects(state.x[2], state.x[3]), m)); }, true, "@@:^@Q");
            method(cls, "setWithObjects:", [this, target, m](auto& state, auto& memory) { state.x[0] = autorelease(make_set(target, nil_terminated(state.x[2], {memory, state.sp}), m)); }, true);
            // From another set (its members; nil is the empty set).
            method(cls, "setWithSet:", [this, target, m](auto& state, auto&) { state.x[0] = autorelease(make_set(target, state.x[2] ? members(state.x[2]) : std::vector<Address>{}, m)); }, true);
            method(cls, "initWithSet:", [this, m](auto& state, auto&) { initialize_set(state.x[0], state.x[2] ? members(state.x[2]) : std::vector<Address>{}, m); });
            method(cls, "init", [this, m](auto& state, auto&) { initialize_set(state.x[0], {}, m); });
            method(cls, "initWithArray:", [this, m](auto& state, auto&) { initialize_set(state.x[0], state.x[2] ? cf.array_values(state.x[2]) : std::vector<Address>{}, m); });
            method(cls, "initWithObjects:count:", [this, m](auto& state, auto&) { initialize_set(state.x[0], guest_objects(state.x[2], state.x[3]), m); }, false, "@@:^@Q");
        }
        method(mutable_set, "setWithCapacity:", [this](auto& state, auto&) { state.x[0] = autorelease(make_set(mutable_set, {}, true)); }, true, "@@:Q");
        method(mutable_set, "initWithCapacity:", [this](auto& state, auto&) { initialize_set(state.x[0], {}, true); }, false, "@@:Q");
        const auto s = set_class;
        method(s, "dealloc", [this](auto& state, auto&) {
            if (const auto set = state_of<SetState>(state.x[0], false)) {
                std::vector<Address> values; { std::lock_guard lock(set->mutex); values.swap(set->values); }
                for (const auto value : values) release(value);
            }
            destruct(state.x[0]);
        }, false, "v@:");
        method(s, "count", [this](auto& state, auto&) { state.x[0] = members(state.x[0]).size(); }, false, "Q@:");
        method(s, "containsObject:", [this](auto& state, auto&) { const auto values = members(state.x[0]); state.x[0] = std::any_of(values.begin(), values.end(), [&](Address value) { return same(value, state.x[2]); }); }, false, "B@:@");
        method(s, "member:", [this](auto& state, auto&) {
            const auto values = members(state.x[0]); const auto found = std::find_if(values.begin(), values.end(), [&](Address value) { return same(value, state.x[2]); });
            state.x[0] = found == values.end() ? 0 : *found;
        });
        method(s, "anyObject", [this](auto& state, auto&) { const auto values = members(state.x[0]); state.x[0] = values.empty() ? 0 : values.front(); });
        method(s, "allObjects", [this](auto& state, auto&) { state.x[0] = autorelease(cf.array(members(state.x[0]))); });
        method(s, "countByEnumeratingWithState:objects:count:", [this](auto& state, auto& memory) { state.x[0] = enumerate(memory, state.x[2], state.x[3], state.x[4], members(state.x[0])); }, false, "Q@:^v^@Q");
        method(s, "copy", [this](auto& state, auto&) { state.x[0] = make_set(set_class, members(state.x[0]), false); });
        method(s, "mutableCopy", [this](auto& state, auto&) { state.x[0] = make_set(mutable_set, members(state.x[0]), true); });
        method(s, "isEqualToSet:", [this](auto& state, auto&) {
            const auto left = members(state.x[0]); const auto right = state.x[2] ? members(state.x[2]) : std::vector<Address>{};
            state.x[0] = state.x[2] && left.size() == right.size() && std::all_of(left.begin(), left.end(), [&](Address value) { return std::any_of(right.begin(), right.end(), [&](Address other) { return same(value, other); }); });
        }, false, "B@:@");
        const auto mutable_state = [this](Address object) { auto set = state_of<SetState>(object); if (!set->mutable_set) fail("mutation of an immutable NSSet"); return set; };
        const auto ms = mutable_set;
        method(ms, "addObject:", [this, mutable_state](auto& state, auto&) { add_member(*mutable_state(state.x[0]), state.x[2]); }, false, "v@:@");
        method(ms, "addObjectsFromArray:", [this, mutable_state](auto& state, auto&) { const auto set = mutable_state(state.x[0]); for (const auto value : cf.array_values(state.x[2])) add_member(*set, value); }, false, "v@:@");
        method(ms, "unionSet:", [this, mutable_state](auto& state, auto&) { const auto set = mutable_state(state.x[0]); for (const auto value : members(state.x[2])) add_member(*set, value); }, false, "v@:@");
        method(ms, "removeObject:", [this, mutable_state](auto& state, auto&) {
            const auto set = mutable_state(state.x[0]); std::vector<Address> snapshot; { std::lock_guard lock(set->mutex); snapshot = set->values; }
            const auto found = std::find_if(snapshot.begin(), snapshot.end(), [&](Address value) { return same(value, state.x[2]); });
            if (found == snapshot.end()) return;
            const auto victim = *found;
            { std::lock_guard lock(set->mutex); const auto at = std::find(set->values.begin(), set->values.end(), victim); if (at == set->values.end()) return; set->values.erase(at); ++set->mutations; }
            release(victim);
        }, false, "v@:@");
        method(ms, "removeAllObjects", [this, mutable_state](auto& state, auto&) {
            const auto set = mutable_state(state.x[0]); std::vector<Address> values; { std::lock_guard lock(set->mutex); values.swap(set->values); ++set->mutations; }
            for (const auto value : values) release(value);
        }, false, "v@:");
    }

    // NSURL is the CFURL bridge class (file URLs).
    Address url_for(const std::string& path, std::optional<bool> directory) {
        if (path.empty()) fail("empty file URL path");
        const auto absolute = path.starts_with('/') ? path : append_component(guest_path_string(process.working_directory), path);
        bool is_directory = directory.value_or(false);
        if (!directory) {
            try { std::error_code error; is_directory = std::filesystem::is_directory(process.map_guest_path ? process.map_guest_path(absolute) : utf8_path(absolute), error); }
            catch (const std::runtime_error&) { is_directory = false; }
        }
        return cf.file_url(absolute, is_directory);
    }
    Address resolve_relative(const std::string& path, Address base) {
        if (path.starts_with('/') || !base) return 0;
        return url_for(append_component(cf.url_path(base), path), std::nullopt);
    }
    void install_urls() {
        url_class = define("NSURL", root);
        cf.set_class(ObjectKind::url, url_class);
        const auto u = url_class;
        factory(u, "fileURLWithPath:", "initFileURLWithPath:", [this](auto& state, auto&) { return url_for(text(state.x[2]), std::nullopt); });
        factory(u, "fileURLWithPath:isDirectory:", "initFileURLWithPath:isDirectory:", [this](auto& state, auto&) { return url_for(text(state.x[2]), (state.x[3] & 0xff) != 0); }, "@@:@B");
        factory(u, "fileURLWithFileSystemRepresentation:isDirectory:relativeToURL:", "initFileURLWithFileSystemRepresentation:isDirectory:relativeToURL:", [this](auto& state, auto&) {
            const auto path = c_text(state.x[2]); const bool directory = (state.x[3] & 0xff) != 0;
            if (const auto relative = !path.starts_with('/') && state.x[4] ? append_component(cf.url_path(state.x[4]), path) : std::string{}; !relative.empty()) return url_for(relative, directory);
            return url_for(path, directory);
        }, "@@:rcB@");
        factory(u, "URLWithString:", "initWithString:", [this](auto& state, auto&) -> Address {
            if (const auto handled = cf.handle_url_string(state.x[2])) return *handled;   // network URLs (CoreFoundation::extend_url_strings)
            const auto value = text(state.x[2]);
            if (!value.starts_with("file://")) return 0; // Only file URLs are represented; other schemes report nil.
            std::string path; auto rest = std::string_view(value).substr(7);
            if (rest.starts_with("localhost")) rest.remove_prefix(9);
            if (!rest.starts_with('/')) return 0;
            for (std::size_t index = 0; index < rest.size(); ++index) {
                if (rest[index] == '%' && index + 2 < rest.size()) {
                    const auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
                    const auto high = hex(rest[index + 1]), low = hex(rest[index + 2]);
                    if (high < 0 || low < 0) return 0;
                    path += static_cast<char>(high * 16 + low); index += 2;
                } else if (rest[index] == '?' || rest[index] == '#') break;
                else path += rest[index];
            }
            return url_for(path, path.ends_with('/'));
        });
        method(u, "URLByResolvingAliasFileAtURL:options:error:", [this](auto& state, auto& memory) {
            // Hosts have no Finder aliases: a reachable URL resolves to itself, following symbolic links.
            const auto source = state.x[2];
            const auto reachable = cf.resource_value(source, make_constant("NSURLIsDirectoryKey"));
            if (!reachable) { if (state.x[4]) memory.write64(state.x[4], 0); state.x[0] = 0; return; }
            std::error_code error;
            const auto host = process.map_guest_path ? process.map_guest_path(cf.url_path(source)) : utf8_path(cf.url_path(source));
            const auto target = std::filesystem::is_symlink(std::filesystem::symlink_status(host, error)) ? std::filesystem::canonical(host, error) : host;
            if (error) { if (state.x[4]) memory.write64(state.x[4], 0); state.x[0] = 0; return; }
            state.x[0] = autorelease(target == host ? retain(source) : url_for(guest_path_string(target), std::nullopt));
        }, true, "@@:@Q^@");
        method(u, "path", [this](auto& state, auto&) { state.x[0] = auto_string(cf.url_path(state.x[0])); });
        method(u, "relativePath", [this](auto& state, auto&) { state.x[0] = auto_string(cf.url_path(state.x[0])); });
        method(u, "absoluteString", [this](auto& state, auto&) { state.x[0] = cf.invoke("CFURLGetString", std::array<Address, 1>{state.x[0]}); });
        method(u, "relativeString", [this](auto& state, auto&) { state.x[0] = cf.invoke("CFURLGetString", std::array<Address, 1>{state.x[0]}); });
        method(u, "scheme", [this](auto& state, auto&) { state.x[0] = auto_string("file"); });
        method(u, "isFileURL", [](auto& state, auto&) { state.x[0] = 1; }, false, "B@:");
        method(u, "hasDirectoryPath", [this](auto& state, auto&) { state.x[0] = cf.url_is_directory(state.x[0]); }, false, "B@:");
        method(u, "fileSystemRepresentation", [this](auto& state, auto&) {
            const auto path = make_string(cf.url_path(state.x[0]));
            state.x[0] = cf.c_string(autorelease(path), cf_utf8);
        }, false, "r*@:");
        method(u, "getFileSystemRepresentation:maxLength:", [this](auto& state, auto&) {
            state.x[0] = cf.invoke("CFURLGetFileSystemRepresentation", std::array<Address, 4>{state.x[0], 1, state.x[2], state.x[3]});
        }, false, "B@:^cQ");
        method(u, "lastPathComponent", [this](auto& state, auto&) { state.x[0] = auto_string(last_component(cf.url_path(state.x[0]))); });
        method(u, "pathExtension", [this](auto& state, auto&) { state.x[0] = auto_string(path_extension(cf.url_path(state.x[0]))); });
        method(u, "URLByAppendingPathComponent:", [this](auto& state, auto&) { state.x[0] = autorelease(url_for(append_component(cf.url_path(state.x[0]), text(state.x[2])), std::nullopt)); });
        method(u, "URLByAppendingPathComponent:isDirectory:", [this](auto& state, auto&) { state.x[0] = autorelease(url_for(append_component(cf.url_path(state.x[0]), text(state.x[2])), (state.x[3] & 0xff) != 0)); }, false, "@@:@B");
        method(u, "URLByDeletingLastPathComponent", [this](auto& state, auto&) { state.x[0] = autorelease(url_for(delete_last_component(cf.url_path(state.x[0])), true)); });
        method(u, "URLByAppendingPathExtension:", [this](auto& state, auto&) { state.x[0] = autorelease(url_for(cf.url_path(state.x[0]) + "." + text(state.x[2]), false)); });
        method(u, "URLByDeletingPathExtension", [this](auto& state, auto&) { state.x[0] = autorelease(url_for(delete_extension(cf.url_path(state.x[0])), cf.url_is_directory(state.x[0]))); });
        for (const auto* name : {"URLByStandardizingPath", "standardizedURL", "absoluteURL", "URLByResolvingSymlinksInPath"})
            method(u, name, [this](auto& state, auto&) { state.x[0] = autorelease(retain(state.x[0])); });
        method(u, "resourceValuesForKeys:error:", [this](auto& state, auto& memory) {
            std::vector<std::pair<Address, Address>> entries;
            const auto keys = state.x[2] ? cf.array_values(state.x[2]) : std::vector<Address>{};
            try {
                for (const auto key : keys) {
                    const auto value = cf.resource_value(state.x[0], key);
                    if (!value) {
                        for (const auto& entry : entries) process.objects().release(entry.second);
                        if (state.x[3]) memory.write64(state.x[3], 0);
                        state.x[0] = 0; return;
                    }
                    if (*value) entries.emplace_back(key, *value);
                }
                const auto result = cf.dictionary(entries);
                for (const auto& entry : entries) process.objects().release(entry.second);
                state.x[0] = autorelease(result);
            } catch (...) { for (const auto& entry : entries) process.objects().release(entry.second); throw; }
        }, false, "@@:@^@");
        method(u, "getResourceValue:forKey:error:", [this](auto& state, auto& memory) {
            const auto value = cf.resource_value(state.x[0], state.x[3]);
            if (!value) { memory.write64(state.x[2], 0); if (state.x[4]) memory.write64(state.x[4], 0); state.x[0] = 0; return; }
            memory.write64(state.x[2], *value ? autorelease(*value) : 0);
            state.x[0] = 1;
        }, false, "B@:^@@^@");
        method(u, "checkResourceIsReachableAndReturnError:", [this](auto& state, auto& memory) {
            const auto value = cf.resource_value(state.x[0], make_constant("NSURLIsDirectoryKey"));
            if (!value && state.x[2]) memory.write64(state.x[2], 0);
            state.x[0] = value.has_value();
        }, false, "B@:^@");
    }
    Address make_constant(std::string_view name) {
        const auto value = process.resolve(core_foundation_library, "_" + std::string(name));
        if (!value) fail("missing constant " + std::string(name));
        return process.main_engine().read64(value->address);
    }

    // NSDate is the CFDate bridge class; times are seconds since 2001-01-01 UTC.
    void install_dates() {
        date_class = define("NSDate", root);
        cf.set_class(ObjectKind::date, date_class);
        distant_past = cf.date(-63114076800.0, true); distant_future = cf.date(63113904000.0, true);
        const auto t = date_class;
        const auto when = [](auto& state) { return as_double(state.v[0][0]); };
        factory(t, "date", "init", [this](auto&, auto&) { return cf.date(CoreFoundation::absolute_time_now()); });
        factory(t, "dateWithTimeIntervalSinceNow:", "initWithTimeIntervalSinceNow:", [this, when](auto& state, auto&) { return cf.date(CoreFoundation::absolute_time_now() + when(state)); }, "@@:d");
        factory(t, "dateWithTimeIntervalSinceReferenceDate:", "initWithTimeIntervalSinceReferenceDate:", [this, when](auto& state, auto&) { return cf.date(when(state)); }, "@@:d");
        factory(t, "dateWithTimeIntervalSince1970:", "initWithTimeIntervalSince1970:", [this, when](auto& state, auto&) { return cf.date(when(state) - 978307200.0); }, "@@:d");
        method(t, "distantPast", [this](auto& state, auto&) { state.x[0] = distant_past; }, true);
        method(t, "distantFuture", [this](auto& state, auto&) { state.x[0] = distant_future; }, true);
        method(t, "timeIntervalSinceReferenceDate", [](auto& state, auto&) { return_double(state, CoreFoundation::absolute_time_now()); }, true, "d@:");
        method(t, "timeIntervalSinceReferenceDate", [this](auto& state, auto&) { return_double(state, cf.date_time(state.x[0])); }, false, "d@:");
        method(t, "timeIntervalSince1970", [this](auto& state, auto&) { return_double(state, cf.date_time(state.x[0]) + 978307200.0); }, false, "d@:");
        method(t, "timeIntervalSinceNow", [this](auto& state, auto&) { return_double(state, cf.date_time(state.x[0]) - CoreFoundation::absolute_time_now()); }, false, "d@:");
        method(t, "timeIntervalSinceDate:", [this](auto& state, auto&) { return_double(state, cf.date_time(state.x[0]) - cf.date_time(state.x[2])); }, false, "d@:@");
        method(t, "dateByAddingTimeInterval:", [this, when](auto& state, auto&) { state.x[0] = autorelease(cf.date(cf.date_time(state.x[0]) + when(state))); }, false, "@@:d");
        method(t, "compare:", [this](auto& state, auto&) { state.x[0] = cf.invoke("CFDateCompare", std::array<Address, 3>{state.x[0], state.x[2], 0}); }, false, "q@:@");
        method(t, "isEqualToDate:", [this](auto& state, auto&) { state.x[0] = state.x[2] && cf.date_time(state.x[0]) == cf.date_time(state.x[2]); }, false, "B@:@");
        for (const auto later : {false, true}) method(t, later ? "laterDate:" : "earlierDate:", [this, later](auto& state, auto&) {
            const auto a = cf.date_time(state.x[0]), b = cf.date_time(state.x[2]);
            if (later ? b > a : b < a) state.x[0] = state.x[2];
        });
    }

    // NSLocale is the CFLocale bridge class.
    void install_locales() {
        locale_class = define("NSLocale", root);
        cf.set_class(ObjectKind::locale, locale_class);
        const auto l = locale_class;
        for (const auto* name : {"currentLocale", "autoupdatingCurrentLocale"}) method(l, name, [this](auto& state, auto&) { state.x[0] = cf.current_locale(); }, true);
        method(l, "systemLocale", [this](auto& state, auto&) { state.x[0] = cf.invoke("CFLocaleGetSystem", std::span<const Address>{}); }, true);
        factory(l, "localeWithLocaleIdentifier:", "initWithLocaleIdentifier:", [this](auto& state, auto&) { return cf.locale(text(state.x[2])); });
        method(l, "preferredLanguages", [this](auto& state, auto&) { state.x[0] = autorelease(cf.invoke("CFLocaleCopyPreferredLanguages", std::span<const Address>{})); }, true);
        const std::array<std::pair<const char*, const char*>, 5> lists{{
            {"availableLocaleIdentifiers", "_CFLocaleCopyAvailableLocaleIdentifiers"}, {"ISOLanguageCodes", "_CFLocaleCopyISOLanguageCodes"},
            {"ISOCountryCodes", "_CFLocaleCopyISOCountryCodes"}, {"ISOCurrencyCodes", "_CFLocaleCopyISOCurrencyCodes"},
            {"commonISOCurrencyCodes", "_CFLocaleCopyCommonISOCurrencyCodes"}}};
        for (const auto& [selector, function] : lists)   // CoreFoundation's guest functions (cf_locale.cpp)
            method(l, selector, [this, function](auto& state, auto&) { state.x[0] = autorelease(call(core_foundation_library, function, {})); }, true);
        method(l, "canonicalLocaleIdentifierFromString:",[this](auto& state, auto&) { state.x[0] = autorelease(cf.invoke("CFLocaleCreateCanonicalLocaleIdentifierFromString", std::array<Address, 2>{0, state.x[2]})); }, true);
        const std::array<std::pair<const char*, const char*>, 9> keyed{{
            {"localeIdentifier", "kCFLocaleIdentifier"}, {"languageCode", "kCFLocaleLanguageCode"}, {"countryCode", "kCFLocaleCountryCode"},
            {"regionCode", "kCFLocaleCountryCode"}, {"scriptCode", "kCFLocaleScriptCode"}, {"variantCode", "kCFLocaleVariantCode"},
            {"decimalSeparator", "kCFLocaleDecimalSeparator"}, {"groupingSeparator", "kCFLocaleGroupingSeparator"}, {"currencyCode", "kCFLocaleCurrencyCode"}}};
        for (const auto& [selector, key] : keyed) {
            const auto constant = make_constant(key);
            method(l, selector, [this, constant](auto& state, auto&) { state.x[0] = cf.invoke("CFLocaleGetValue", std::array<Address, 2>{state.x[0], constant}); });
        }
        method(l, "objectForKey:", [this](auto& state, auto&) { state.x[0] = cf.invoke("CFLocaleGetValue", std::array<Address, 2>{state.x[0], state.x[2]}); });
        const auto metric = make_constant("kCFLocaleUsesMetricSystem");
        method(l, "usesMetricSystem", [this, metric](auto& state, auto&) { state.x[0] = cf.invoke("CFBooleanGetValue", std::array<Address, 1>{cf.invoke("CFLocaleGetValue", std::array<Address, 2>{state.x[0], metric})}); }, false, "B@:");
    }

    // Standard directories (NSSearchPathDirectory x NSSearchPathDomainMask), as
    // macOS lays them out under the guest home and root.
    std::vector<std::string> search_paths(Address directory, Address domains, bool expand) {
        struct Entry { Address directory; const char* user; const char* local; const char* system; };
        static constexpr Entry entries[] = {
            {1, "Applications", "/Applications", "/System/Applications"}, {2, "Applications/Demos", "/Applications/Demos", nullptr},
            {3, "Developer/Applications", "/Developer/Applications", nullptr}, {4, "Applications/Utilities", "/Applications/Utilities", "/System/Applications/Utilities"},
            {5, "Library", "/Library", "/System/Library"}, {6, "Developer", "/Developer", nullptr}, {7, nullptr, "/Users", nullptr},
            {8, "Library/Documentation", "/Library/Documentation", "/System/Library/Documentation"}, {9, "Documents", nullptr, nullptr},
            {10, nullptr, nullptr, "/System/Library/CoreServices"}, {11, "Library/Autosave Information", nullptr, nullptr},
            {12, "Desktop", nullptr, nullptr}, {13, "Library/Caches", "/Library/Caches", "/System/Library/Caches"},
            {14, "Library/Application Support", "/Library/Application Support", nullptr}, {15, "Downloads", nullptr, nullptr},
            {16, "Library/Input Methods", "/Library/Input Methods", "/System/Library/Input Methods"}, {17, "Movies", nullptr, nullptr},
            {18, "Music", nullptr, nullptr}, {19, "Pictures", nullptr, nullptr}, {20, nullptr, "/Library/Printers/PPDs", "/System/Library/Printers/PPDs"},
            {21, "Public", nullptr, nullptr}, {22, "Library/PreferencePanes", "/Library/PreferencePanes", "/System/Library/PreferencePanes"},
            {23, "Library/Application Scripts", nullptr, nullptr}, {102, ".Trash", nullptr, nullptr}};
        std::vector<std::string> result;
        auto add = [&](Address wanted) {
            for (const auto& entry : entries) {
                if (entry.directory != wanted) continue;
                if ((domains & 1) && entry.user) result.push_back((expand ? home() : std::string("~")) + "/" + entry.user);
                if ((domains & 2) && entry.local) result.emplace_back(entry.local);
                if ((domains & 8) && entry.system) result.emplace_back(entry.system);
            }
        };
        if (directory == 100) for (const auto value : {1, 2, 3, 4}) add(static_cast<Address>(value));
        else if (directory == 101) for (const auto value : {5, 6}) add(static_cast<Address>(value));
        else add(directory);
        return result;
    }
    std::filesystem::path host_path(const std::string& guest) { return process.map_guest_path ? process.map_guest_path(guest) : utf8_path(guest); }
    std::optional<std::filesystem::path> try_host_path(Address string) {
        try { return host_path(text(string)); } catch (const std::runtime_error&) { return std::nullopt; }
    }
    void install_files() {
        file_manager = define("NSFileManager", root);
        file_manager_object = singleton(file_manager);
        const auto f = file_manager;
        method(f, "defaultManager", [this](auto& state, auto&) { state.x[0] = file_manager_object; }, true);
        static_cast<void>(process.install_function("_NSSearchPathForDirectoriesInDomains", [this](auto& state, auto&) {
            state.x[0] = autorelease(string_array(search_paths(state.x[0], state.x[1], (state.x[2] & 0xff) != 0)));
        }, foundation_library));
        static_cast<void>(process.install_function("_NSHomeDirectory", [this](auto& state, auto&) { state.x[0] = auto_string(home()); }, foundation_library));
        static_cast<void>(process.install_function("_NSTemporaryDirectory", [this](auto& state, auto&) { state.x[0] = auto_string("/tmp/"); }, foundation_library));
        method(f, "URLsForDirectory:inDomains:", [this](auto& state, auto&) {
            std::vector<Address> urls;
            try {
                for (const auto& path : search_paths(state.x[2], state.x[3], true)) urls.push_back(cf.file_url(path, true));
                const auto result = cf.array(urls);
                for (const auto value : urls) process.objects().release(value);
                state.x[0] = autorelease(result);
            } catch (...) { for (const auto value : urls) process.objects().release(value); throw; }
        }, false, "@@:QQ");
        method(f, "homeDirectoryForCurrentUser", [this](auto& state, auto&) { state.x[0] = autorelease(cf.file_url(home(), true)); });
        method(f, "temporaryDirectory", [this](auto& state, auto&) { state.x[0] = autorelease(cf.file_url("/tmp", true)); });
        method(f, "currentDirectoryPath", [this](auto& state, auto&) { state.x[0] = auto_string(guest_path_string(process.working_directory)); });
        method(f, "changeCurrentDirectoryPath:", [this](auto& state, auto&) {
            const auto path = guest_text(text(state.x[2]));
            try { const auto result = call("/usr/lib/libSystem.B.dylib", "_chdir", {path}); process.release_guest(path); state.x[0] = static_cast<std::uint32_t>(result) == 0; }
            catch (...) { process.release_guest(path); throw; }
        }, false, "B@:@");
        method(f, "fileExistsAtPath:", [this](auto& state, auto&) {
            const auto host = state.x[2] ? try_host_path(state.x[2]) : std::nullopt; std::error_code error;
            state.x[0] = host && std::filesystem::exists(*host, error);
        }, false, "B@:@");
        method(f, "fileExistsAtPath:isDirectory:", [this](auto& state, auto& memory) {
            const auto host = state.x[2] ? try_host_path(state.x[2]) : std::nullopt; std::error_code error;
            const auto status = host ? std::filesystem::status(*host, error) : std::filesystem::file_status{};
            const bool exists = host && !error && std::filesystem::exists(status);
            if (exists && state.x[3]) memory.write8(state.x[3], std::filesystem::is_directory(status));
            state.x[0] = exists;
        }, false, "B@:@^B");
        const auto access = [this](Address path, int mode) {
            const auto guest = guest_text(text(path));
            try { const auto result = call("/usr/lib/libSystem.B.dylib", "_access", {guest, static_cast<Address>(mode)}); process.release_guest(guest); return static_cast<std::uint32_t>(result) == 0; }
            catch (...) { process.release_guest(guest); throw; }
        };
        method(f, "isReadableFileAtPath:", [access](auto& state, auto&) { state.x[0] = access(state.x[2], 4); }, false, "B@:@");
        method(f, "isWritableFileAtPath:", [access](auto& state, auto&) { state.x[0] = access(state.x[2], 2); }, false, "B@:@");
        method(f, "isExecutableFileAtPath:", [access](auto& state, auto&) { state.x[0] = access(state.x[2], 1); }, false, "B@:@");
        method(f, "isDeletableFileAtPath:", [this, access](auto& state, auto&) { state.x[0] = access(auto_string(delete_last_component(text(state.x[2]))), 2); }, false, "B@:@");
        const auto create_directory = [this](const std::string& guest, bool intermediate, Address error_out) {
            std::error_code error; const auto host = host_path(guest);
            const bool created = intermediate ? (std::filesystem::create_directories(host, error), !error && std::filesystem::is_directory(host)) : std::filesystem::create_directory(host, error);
            if (!created && error_out) memory().write64(error_out, 0);
            return created;
        };
        method(f, "createDirectoryAtPath:withIntermediateDirectories:attributes:error:", [this, create_directory](auto& state, auto&) {
            state.x[0] = create_directory(text(state.x[2]), (state.x[3] & 0xff) != 0, state.x[5]);
        }, false, "B@:@B@^@");
        method(f, "createDirectoryAtURL:withIntermediateDirectories:attributes:error:", [this, create_directory](auto& state, auto&) {
            state.x[0] = create_directory(cf.url_path(state.x[2]), (state.x[3] & 0xff) != 0, state.x[5]);
        }, false, "B@:@B@^@");
        const auto remove = [this](const std::string& guest, Address error_out) {
            std::error_code error; const auto removed = std::filesystem::remove_all(host_path(guest), error);
            const bool ok = !error && removed > 0;
            if (!ok && error_out) memory().write64(error_out, 0);
            return ok;
        };
        method(f, "removeItemAtPath:error:", [this, remove](auto& state, auto&) { state.x[0] = remove(text(state.x[2]), state.x[3]); }, false, "B@:@^@");
        method(f, "removeItemAtURL:error:", [this, remove](auto& state, auto&) { state.x[0] = remove(cf.url_path(state.x[2]), state.x[3]); }, false, "B@:@^@");
        for (const auto move : {false, true}) method(f, move ? "moveItemAtPath:toPath:error:" : "copyItemAtPath:toPath:error:", [this, move](auto& state, auto& memory) {
            std::error_code error; const auto from = host_path(text(state.x[2])), to = host_path(text(state.x[3]));
            if (std::filesystem::exists(to, error)) error = std::make_error_code(std::errc::file_exists);
            else if (move) std::filesystem::rename(from, to, error);
            else std::filesystem::copy(from, to, std::filesystem::copy_options::recursive, error);
            if (error && state.x[4]) memory.write64(state.x[4], 0);
            state.x[0] = !error;
        }, false, "B@:@@^@");
        method(f, "contentsOfDirectoryAtPath:error:", [this](auto& state, auto& memory) {
            std::error_code error; std::vector<std::string> names;
            for (std::filesystem::directory_iterator it(host_path(text(state.x[2])), error), end; !error && it != end; it.increment(error)) names.push_back(host_text(it->path().filename()));
            if (error) { if (state.x[3]) memory.write64(state.x[3], 0); state.x[0] = 0; return; }
            state.x[0] = autorelease(string_array(names));
        }, false, "@@:@^@");
        method(f, "trashItemAtURL:resultingItemURL:error:", [](auto& state, auto& memory) {
            // The host has no guest-visible Trash; the item is left in place and the call fails.
            if (state.x[4]) memory.write64(state.x[4], 0);
            state.x[0] = 0;
        }, false, "B@:@^@^@");
        method(f, "fileSystemRepresentationWithPath:", [this](auto& state, auto&) { state.x[0] = cf.c_string(state.x[2], cf_utf8); }, false, "r*@:@");
        method(f, "stringWithFileSystemRepresentation:length:", [this](auto& state, auto&) { state.x[0] = autorelease(cf.invoke("CFStringCreateWithBytes", std::array<Address, 5>{0, state.x[2], state.x[3], cf_utf8, 0})); }, false, "@@:r*Q");
        method(f, "displayNameAtPath:", [this](auto& state, auto&) { state.x[0] = auto_string(last_component(text(state.x[2]))); });
    }

    // NSBundle: the main bundle is the .app enclosing Contents/MacOS/<executable>,
    // else the executable's directory; Info.plist is read from the bundle.
    Address plist_value(PlistParser& parser, const PlistParser::Tag& tag) {
        if (tag.closing) fail("unexpected plist close tag");
        if (tag.name == "string") return make_string(tag.empty ? std::string{} : parser.content("string"));
        if (tag.name == "true" || tag.name == "false") { if (!tag.empty) parser.content(tag.name); return cf.boolean(tag.name == "true"); }
        if (tag.name == "integer") { const auto value = parser.content("integer"); return number(std::strtoll(value.c_str(), nullptr, 10)); }
        if (tag.name == "real") { const auto value = parser.content("real"); return cf.real(std::strtod(value.c_str(), nullptr)); }
        if (tag.name == "date") return make_string(parser.content("date")); // Kept as its ISO 8601 text.
        if (tag.name == "data") {
            const auto encoded = parser.content("data"); std::vector<std::uint8_t> bytes; std::uint32_t buffer{}; int bits{};
            for (const auto c : encoded) {
                const auto value = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
                if (value < 0) continue;
                buffer = (buffer << 6) | static_cast<std::uint32_t>(value); bits += 6;
                if (bits >= 8) { bits -= 8; bytes.push_back(static_cast<std::uint8_t>(buffer >> bits)); }
            }
            const auto storage = process.allocate_guest(std::max<std::size_t>(bytes.size(), 1));
            try { memory().write_bytes(storage, bytes); const auto value = cf.invoke("CFDataCreate", std::array<Address, 3>{0, storage, bytes.size()}); process.release_guest(storage); return value; }
            catch (...) { process.release_guest(storage); throw; }
        }
        const bool is_array = tag.name == "array";
        if (!is_array && tag.name != "dict") fail("unsupported plist element " + tag.name);
        std::vector<Address> owned; std::vector<std::pair<Address, Address>> entries;
        try {
            if (!tag.empty) for (;;) {
                auto next = parser.tag();
                if (next.closing) { if (next.name != tag.name) fail("mismatched plist close tag"); break; }
                if (is_array) { owned.push_back(plist_value(parser, next)); continue; }
                if (next.name != "key") fail("plist dictionary entry without a key");
                const auto key = make_string(next.empty ? std::string{} : parser.content("key")); owned.push_back(key);
                const auto value = plist_value(parser, parser.tag()); owned.push_back(value);
                entries.emplace_back(key, value);
            }
            const auto result = is_array ? cf.array(owned) : cf.dictionary(entries);
            for (const auto value : owned) process.objects().release(value);
            return result;
        } catch (...) { for (const auto value : owned) process.objects().release(value); throw; }
    }
    Address read_plist(const std::filesystem::path& host) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(host, error)) return 0;
        const auto size = std::filesystem::file_size(host, error);
        if (error || size > (16U << 20)) return 0;
        std::string content(static_cast<std::size_t>(size), '\0');
        std::ifstream file(host, std::ios::binary);
        if (!file.read(content.data(), static_cast<std::streamsize>(content.size()))) return 0;
        if (content.starts_with("bplist")) fail("binary property lists are unsupported");
        PlistParser parser{content};
        const auto root_tag = parser.tag();
        if (root_tag.name != "plist" || root_tag.closing) fail("Info.plist has no plist root");
        return plist_value(parser, parser.tag());
    }
    Address make_bundle(const std::string& path, const std::string& executable) {
        const auto object = objc.allocate(bundle_class);
        try {
            auto bundle = attach<BundleState>(object);
            bundle->path = trim_slashes(path); bundle->executable = executable;
            std::error_code error;
            bundle->application = std::filesystem::is_directory(host_path(bundle->path + "/Contents"), error);
        } catch (...) { release(object); throw; }
        return object;
    }
    std::shared_ptr<BundleState> bundle_of(Address object) { return state_of<BundleState>(object); }
    std::string contents(const BundleState& bundle) { return bundle.application ? bundle.path + "/Contents" : bundle.path; }
    // The resource directory: Contents/Resources of an application; for a
    // framework (X.framework or X.framework/Versions/<v>) its Resources
    // directory (Resources, else Versions/Current/Resources); else the bundle
    // root (flat bundles).
    std::string resources(const BundleState& bundle) {
        if (bundle.application) return contents(bundle) + "/Resources";
        std::error_code error;
        for (const auto* candidate : {"/Resources", "/Versions/Current/Resources"})
            if (std::filesystem::is_directory(host_path(bundle.path + candidate), error)) return bundle.path + candidate;
        return bundle.path;
    }
    Address info(Address object) {
        const auto bundle = bundle_of(object);
        std::lock_guard lock(bundle->mutex);
        if (!bundle->info) {
            const auto plist = read_plist(host_path(bundle->application ? contents(*bundle) + "/Info.plist" : resources(*bundle) + "/Info.plist"));
            bundle->info = plist ? plist : cf.dictionary({});
        }
        return bundle->info;
    }
    void install_bundles() {
        bundle_class = define("NSBundle", root);
        const auto b = bundle_class;
        method(b, "mainBundle", [this](auto& state, auto&) {
            std::lock_guard lock(main_bundle_mutex);
            if (!main_bundle) {
                const auto executable = guest_path_string(process.executable);
                auto directory = delete_last_component(executable);
                const auto contents_dir = delete_last_component(directory);
                if (last_component(directory) == "MacOS" && last_component(contents_dir) == "Contents") directory = delete_last_component(contents_dir);
                main_bundle = make_bundle(directory, executable); // Owned for the process lifetime.
            }
            state.x[0] = main_bundle;
        }, true);
        method(b, "bundleWithPath:", [this](auto& state, auto&) {
            std::error_code error;
            const auto path = text(state.x[2]);
            if (!std::filesystem::is_directory(host_path(path), error)) { state.x[0] = 0; return; }
            state.x[0] = autorelease(make_bundle(path, {}));
        }, true);
        method(b, "initWithPath:", [this](auto& state, auto&) {
            std::error_code error; const auto path = text(state.x[2]);
            if (!std::filesystem::is_directory(host_path(path), error)) { replace(state, 0); return; }
            auto bundle = attach<BundleState>(state.x[0]); bundle->path = trim_slashes(path);
            bundle->application = std::filesystem::is_directory(host_path(bundle->path + "/Contents"), error);
        });
        method(b, "dealloc", [this](auto& state, auto&) {
            if (const auto bundle = state_of<BundleState>(state.x[0], false)) process.objects().release(std::exchange(bundle->info, 0));
            destruct(state.x[0]);
        }, false, "v@:");
        const auto path_method = [this](const char* name, auto compute, bool as_url) {
            method(bundle_class, name, [this, compute, as_url](auto& state, auto&) {
                const auto bundle = bundle_of(state.x[0]);
                const std::optional<std::string> path = compute(*bundle);
                if (!path) { state.x[0] = 0; return; }
                state.x[0] = as_url ? autorelease(cf.file_url(*path, true)) : auto_string(*path);
            });
        };
        for (const auto as_url : {false, true}) {
            path_method(as_url ? "bundleURL" : "bundlePath", [](const BundleState& bundle) -> std::optional<std::string> { return bundle.path; }, as_url);
            path_method(as_url ? "resourceURL" : "resourcePath", [this](const BundleState& bundle) -> std::optional<std::string> { return resources(bundle); }, as_url);
            path_method(as_url ? "builtInPlugInsURL" : "builtInPlugInsPath", [this](const BundleState& bundle) -> std::optional<std::string> { return contents(bundle) + "/PlugIns"; }, as_url);
            path_method(as_url ? "privateFrameworksURL" : "privateFrameworksPath", [this](const BundleState& bundle) -> std::optional<std::string> { return contents(bundle) + "/Frameworks"; }, as_url);
            path_method(as_url ? "sharedSupportURL" : "sharedSupportPath", [this](const BundleState& bundle) -> std::optional<std::string> { return contents(bundle) + "/SharedSupport"; }, as_url);
        }
        method(b, "executablePath", [this](auto& state, auto&) {
            const auto bundle = bundle_of(state.x[0]);
            if (!bundle->executable.empty()) { state.x[0] = auto_string(bundle->executable); return; }
            const auto name = cf.dictionary_value(info(state.x[0]), make_constant("kCFBundleExecutableKey"));
            state.x[0] = name && bundle->application ? auto_string(contents(*bundle) + "/MacOS/" + text(name)) : 0;
        });
        method(b, "infoDictionary", [this](auto& state, auto&) { state.x[0] = info(state.x[0]); });
        method(b, "localizedInfoDictionary", [this](auto& state, auto&) { state.x[0] = info(state.x[0]); });
        method(b, "objectForInfoDictionaryKey:", [this](auto& state, auto&) { state.x[0] = state.x[2] ? cf.dictionary_value(info(state.x[0]), state.x[2]) : 0; });
        method(b, "bundleIdentifier", [this](auto& state, auto&) { state.x[0] = cf.dictionary_value(info(state.x[0]), make_constant("kCFBundleIdentifierKey")); });
        const auto resource = [this](Address object, Address name, Address type, Address directory) -> std::optional<std::string> {
            const auto bundle = bundle_of(object);
            auto path = resources(*bundle);
            if (directory) path = append_component(path, text(directory));
            std::string file = name ? text(name) : std::string{};
            if (type && !text(type).empty()) { auto extension = text(type); if (extension.starts_with('.')) extension.erase(0, 1); file += "." + extension; }
            if (file.empty()) return std::nullopt;
            // Non-localized resources first, then the development language's
            // localization (en/English) and Base.lproj, as NSBundle searches.
            for (const auto* localization : {"", "en.lproj", "English.lproj", "Base.lproj"}) {
                const auto folder = *localization ? append_component(path, localization) : path;
                const auto candidate = append_component(folder, file);
                std::error_code error;
                if (std::filesystem::exists(host_path(candidate), error)) return candidate;
            }
            return std::nullopt;
        };
        method(b, "pathForResource:ofType:", [this, resource](auto& state, auto&) { const auto path = resource(state.x[0], state.x[2], state.x[3], 0); state.x[0] = path ? auto_string(*path) : 0; });
        method(b, "pathForResource:ofType:inDirectory:", [this, resource](auto& state, auto&) { const auto path = resource(state.x[0], state.x[2], state.x[3], state.x[4]); state.x[0] = path ? auto_string(*path) : 0; });
        method(b, "URLForResource:withExtension:", [this, resource](auto& state, auto&) { const auto path = resource(state.x[0], state.x[2], state.x[3], 0); state.x[0] = path ? autorelease(url_for(*path, std::nullopt)) : 0; });
        method(b, "localizedStringForKey:value:table:", [this](auto& state, auto&) {
            // No .strings tables are loaded: the development-language value (or key) is the answer.
            const auto value = state.x[3] && cf.length(state.x[3]) ? state.x[3] : state.x[2];
            state.x[0] = value ? autorelease(retain(value)) : auto_string("");
        });
        method(b, "load", [](auto& state, auto&) { state.x[0] = 1; }, false, "B@:");
        method(b, "isLoaded", [](auto& state, auto&) { state.x[0] = 1; }, false, "B@:");
    }

    // NSNotificationCenter: in-process, synchronous delivery; observers are not retained.
    Address make_notification(Address name, Address object, Address user_info) {
        if (!name) fail("notification requires a name");
        const auto notification = objc.allocate(notification_class);
        try {
            auto value = attach<NotificationState>(notification);
            value->name = cf.copy(name); value->object = retain(object); value->user_info = retain(user_info);
        } catch (...) { release(notification); throw; }
        return notification;
    }
    void post(Address notification) {
        const auto value = state_of<NotificationState>(notification);
        std::vector<Observer> targets;
        {
            std::lock_guard lock(mutex);
            for (const auto& observer : observers)
                if ((!observer.name || cf.equal(observer.name, value->name)) && (!observer.object || observer.object == value->object)) targets.push_back(observer);
        }
        for (const auto& observer : targets) {
            bool live{};
            { std::lock_guard lock(mutex); live = std::any_of(observers.begin(), observers.end(), [&](const Observer& entry) { return entry.observer == observer.observer && entry.selector == observer.selector && entry.name == observer.name && entry.object == observer.object; }); }
            if (!live) continue; // Removed by an earlier observer during this post.
            const std::array<Address, 3> arguments{observer.observer, observer.selector, notification};
            const auto send_address = process.resolve(objc_library, "_objc_msgSend");
            if (!send_address) fail("missing objc_msgSend");
            static_cast<void>(process.calling_engine().invoke_callback(send_address->address, arguments));
        }
    }
    void remove_observers(Address observer, Address name, Address object) {
        std::vector<Address> names;
        {
            std::lock_guard lock(mutex);
            for (auto it = observers.begin(); it != observers.end();) {
                if (it->observer == observer && (!name || (it->name && cf.equal(it->name, name))) && (!object || it->object == object)) { if (it->name) names.push_back(it->name); it = observers.erase(it); }
                else ++it;
            }
        }
        for (const auto value : names) process.objects().release(value);
    }
    void install_notifications() {
        notification_class = define("NSNotification", root);
        notification_center = define("NSNotificationCenter", root);
        center_object = singleton(notification_center);
        const auto n = notification_class, c = notification_center;
        method(n, "notificationWithName:object:", [this](auto& state, auto&) { state.x[0] = autorelease(make_notification(state.x[2], state.x[3], 0)); }, true);
        method(n, "notificationWithName:object:userInfo:", [this](auto& state, auto&) { state.x[0] = autorelease(make_notification(state.x[2], state.x[3], state.x[4])); }, true);
        method(n, "name", [this](auto& state, auto&) { state.x[0] = state_of<NotificationState>(state.x[0])->name; });
        method(n, "object", [this](auto& state, auto&) { state.x[0] = state_of<NotificationState>(state.x[0])->object; });
        method(n, "userInfo", [this](auto& state, auto&) { state.x[0] = state_of<NotificationState>(state.x[0])->user_info; });
        method(n, "dealloc", [this](auto& state, auto&) {
            if (const auto value = state_of<NotificationState>(state.x[0], false)) {
                process.objects().release(std::exchange(value->name, 0)); release(std::exchange(value->object, 0)); release(std::exchange(value->user_info, 0));
            }
            destruct(state.x[0]);
        }, false, "v@:");
        method(c, "defaultCenter", [this](auto& state, auto&) { state.x[0] = center_object; }, true);
        method(c, "addObserver:selector:name:object:", [this](auto& state, auto&) {
            if (!state.x[2] || !state.x[3]) fail("notification observer and selector are required");
            const auto name = state.x[4] ? cf.copy(state.x[4]) : 0;
            std::lock_guard lock(mutex); observers.push_back({state.x[2], state.x[3], name, state.x[5]});
        }, false, "v@:@:@@");
        foundation_shells::install_notification_block_shell(objc, c);   // alchemy.exe only (headless_shells.hpp)
        // An observer removed for every name and object hears -_alchemyObserverRemoved
        // when it answers it: the block observers of addObserverForName:object:queue:usingBlock:
        // (notification_blocks.cpp) drop the center's reference then.
        const auto removed = [this](Address observer) {
            if (observer && objc.responds(observer, objc.selector("_alchemyObserverRemoved"))) static_cast<void>(send(observer, "_alchemyObserverRemoved"));
        };
        method(c, "removeObserver:", [this, removed](auto& state, auto&) { remove_observers(state.x[2], 0, 0); removed(state.x[2]); }, false, "v@:@");
        method(c, "removeObserver:name:object:", [this, removed](auto& state, auto&) {
            remove_observers(state.x[2], state.x[3], state.x[4]);
            if (!state.x[3] && !state.x[4]) removed(state.x[2]);
        }, false, "v@:@@@");
        method(c, "postNotification:", [this](auto& state, auto&) { post(state.x[2]); }, false, "v@:@");
        method(c, "postNotificationName:object:", [this](auto& state, auto&) {
            const auto notification = make_notification(state.x[2], state.x[3], 0);
            try { post(notification); } catch (...) { release(notification); throw; }
            release(notification);
        }, false, "v@:@@");
        method(c, "postNotificationName:object:userInfo:", [this](auto& state, auto&) {
            const auto notification = make_notification(state.x[2], state.x[3], state.x[4]);
            try { post(notification); } catch (...) { release(notification); throw; }
            release(notification);
        }, false, "v@:@@@");
    }

    // NSRunLoop: one per thread. A headless process registers no input sources or
    // timers here, so running a mode returns at once, as a source-less run loop does.
    Address run_loop_for(std::uint64_t identity) {
        std::lock_guard lock(mutex);
        if (const auto found = run_loops.find(identity); found != run_loops.end()) return found->second;
        const auto value = singleton(run_loop_class);
        run_loops.emplace(identity, value);
        return value;
    }
    void install_run_loops() {
        run_loop_class = define("NSRunLoop", root);
        const auto r = run_loop_class;
        // alchemy.exe only (headless_shells.hpp); alchemy-fx's run loop is darwin/runloop/timers.cpp.
        foundation_shells::install_run_loop_shell(objc, process, r, [this](std::uint64_t identity) { return run_loop_for(identity); });
        method(r, "addPort:forMode:", [](auto&, auto&) { fail("NSRunLoop ports are unsupported"); }, false, "v@:@@");
    }

    void install_attributed_strings() {
        attributed_class = define("NSAttributedString", root);
        // alchemy.exe only (headless_shells.hpp); alchemy-fx's is darwin/coretext/attributed.cpp.
        foundation_shells::install_attributed_string_shell(process, objc, cf, attributed_class);
    }

    void install_process_info() {
        const auto p = objc.find_class("NSProcessInfo");
        if (!p) return;
        method(p, "systemUptime", [](auto& state, auto&) {
            return_double(state, std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }, false, "d@:");
        method(p, "physicalMemory", [](auto& state, auto&) {
            const auto memory = host::system::memory();
            state.x[0] = memory ? memory->physical : 0;
        }, false, "Q@:");
        method(p, "hostName", [this](auto& state, auto&) {
            const auto buffer = process.allocate_guest(256);
            try {
                const auto result = call("/usr/lib/libSystem.B.dylib", "_gethostname", {buffer, 256});
                const auto name = static_cast<std::uint32_t>(result) == 0 ? c_text(buffer, 256) : std::string("localhost");
                process.release_guest(buffer); state.x[0] = auto_string(name);
            } catch (...) { process.release_guest(buffer); throw; }
        });
        method(p, "globallyUniqueString", [this](auto& state, auto&) {
            std::random_device device; std::array<std::uint8_t, 16> bytes{};
            for (auto& byte : bytes) byte = static_cast<std::uint8_t>(device());
            bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x40); bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
            std::string value; constexpr char hex[] = "0123456789ABCDEF";
            for (std::size_t index = 0; index < bytes.size(); ++index) { if (index == 4 || index == 6 || index == 8 || index == 10) value += '-'; value += hex[bytes[index] >> 4]; value += hex[bytes[index] & 15]; }
            state.x[0] = auto_string(value);
        });
        method(p, "thermalState", [](auto& state, auto&) { state.x[0] = 0; }, false, "q@:"); // NSProcessInfoThermalStateNominal
        method(p, "isLowPowerModeEnabled", [](auto& state, auto&) { state.x[0] = 0; }, false, "B@:");
    }

    void install_geometry() {
        for (const auto& [name, size] : {std::pair{"_NSZeroPoint", 16}, std::pair{"_NSZeroSize", 16}, std::pair{"_NSZeroRect", 32}}) {
            const auto address = process.allocate_guest(static_cast<std::uint64_t>(size));
            process.main_engine().fill_bytes(address, 0, static_cast<std::uint64_t>(size));
            process.install_export(foundation_library, name, {address, ExportKind::data});
        }
        // NSRect is an HFA of four doubles: d0-d3 in and out, insets in d4 and d5.
        static_cast<void>(process.install_function("_NSInsetRect", [](auto& state, auto&) {
            const auto dx = as_double(state.v[4][0]), dy = as_double(state.v[5][0]);
            const std::array<double, 4> result{as_double(state.v[0][0]) + dx, as_double(state.v[1][0]) + dy, as_double(state.v[2][0]) - 2 * dx, as_double(state.v[3][0]) - 2 * dy};
            for (std::size_t index = 0; index < result.size(); ++index) state.v[index] = {std::bit_cast<std::uint64_t>(result[index]), 0};
        }, foundation_library));
        static_cast<void>(process.install_function("_NSIntersectsRect", [](auto& state, auto&) {
            const auto x = [&](int index) { return as_double(state.v[static_cast<std::size_t>(index)][0]); };
            state.x[0] = !(x(2) <= 0 || x(3) <= 0 || x(6) <= 0 || x(7) <= 0 || x(0) >= x(4) + x(6) || x(4) >= x(0) + x(2) || x(1) >= x(5) + x(7) || x(5) >= x(1) + x(3));
        }, foundation_library));
    }
};
} // namespace

std::shared_ptr<void> detail::install_foundation_extra(ProcessContext& process, ObjcRuntime& objc, CoreFoundation& cf) {
    return std::make_shared<Extra>(process, objc, cf);
}
} // namespace alchemy
