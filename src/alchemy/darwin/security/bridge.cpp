#include "alchemy/darwin/security/bridge.hpp"

#include "alchemy/placeholder_exports.hpp"
#include "alchemy/registration_audit.hpp"

#include "alchemy/host/process.hpp"
#include "alchemy/path.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <utility>

// Shared guest bridge of the Security and networking providers (see
// bridge.hpp). Project-authored; no Apple code.
namespace alchemy::gui::security {

void fail(std::string_view why) { throw std::runtime_error("Alchemy Security: " + std::string(why)); }
void unsupported(std::string_view what) { throw std::runtime_error("unsupported " + std::string(what)); }

// UTF-8 <-> wide text (UTF-16 where wchar_t has 16 bits, UTF-32 elsewhere);
// malformed sequences and unpaired surrogates become U+FFFD.
std::wstring wide(std::string_view utf8) {
    std::wstring result;
    const auto put = [&result](char32_t c) {
        if constexpr (sizeof(wchar_t) == 2) {
            if (c >= 0x10000) {
                c -= 0x10000;
                result.push_back(static_cast<wchar_t>(0xd800 + (c >> 10)));
                result.push_back(static_cast<wchar_t>(0xdc00 + (c & 0x3ff)));
                return;
            }
        }
        result.push_back(static_cast<wchar_t>(c));
    };
    for (std::size_t at = 0; at < utf8.size();) {
        const auto lead = static_cast<unsigned char>(utf8[at]);
        const std::size_t extra = lead < 0x80 ? 0 : (lead >> 5) == 6 ? 1 : (lead >> 4) == 14 ? 2 : (lead >> 3) == 30 ? 3 : 4;
        if (extra == 4) { put(0xfffd); ++at; continue; }   // a stray continuation byte or an invalid lead
        char32_t value = extra ? lead & (0x3fu >> extra) : lead;
        std::size_t index = 1;
        while (index <= extra && at + index < utf8.size() && (static_cast<unsigned char>(utf8[at + index]) >> 6) == 2)
            value = (value << 6) | (static_cast<unsigned char>(utf8[at + index++]) & 0x3fu);
        if (index != extra + 1) { put(0xfffd); at += index; continue; }   // truncated
        constexpr char32_t minimum[] = {0, 0x80, 0x800, 0x10000};
        put(value < minimum[extra] || value > 0x10ffff || (value >= 0xd800 && value < 0xe000) ? char32_t{0xfffd} : value);
        at += extra + 1;
    }
    return result;
}

std::string narrow(std::wstring_view text) {
    std::string result;
    for (std::size_t at = 0; at < text.size(); ++at) {
        auto c = static_cast<char32_t>(text[at]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (c >= 0xd800 && c < 0xdc00 && at + 1 < text.size() && text[at + 1] >= 0xdc00 && text[at + 1] < 0xe000)
                c = 0x10000 + ((c - 0xd800) << 10) + (static_cast<char32_t>(text[++at]) - 0xdc00);
        }
        if ((c >= 0xd800 && c < 0xe000) || c > 0x10ffff) c = 0xfffd;
        if (c < 0x80) result += static_cast<char>(c);
        else if (c < 0x800) { result += static_cast<char>(0xc0 | (c >> 6)); result += static_cast<char>(0x80 | (c & 0x3f)); }
        else if (c < 0x10000) {
            result += static_cast<char>(0xe0 | (c >> 12)); result += static_cast<char>(0x80 | ((c >> 6) & 0x3f)); result += static_cast<char>(0x80 | (c & 0x3f));
        } else {
            result += static_cast<char>(0xf0 | (c >> 18)); result += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
            result += static_cast<char>(0x80 | ((c >> 6) & 0x3f)); result += static_cast<char>(0x80 | (c & 0x3f));
        }
    }
    return result;
}

std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& value : result) value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    return result;
}

bool iequals(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
        std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        });
}

std::string host_environment(const char* name) { return host::process::environment(name).value_or(std::string{}); }

// ---- guest calls ----------------------------------------------------------------------

Address Bridge::symbol(std::string_view library, std::string_view name) const {
    const auto found = process.resolve(library, name);
    if (!found) fail("missing runtime service " + std::string(name));
    return found->address;
}

Address Bridge::call(std::string_view library, std::string_view name, std::initializer_list<Address> arguments) {
    return call_address(symbol(library, name), std::span(arguments.begin(), arguments.size()));
}

Address Bridge::call_address(Address function, std::span<const Address> arguments) {
    return EngineAdapter(process.calling_engine()).call(function, arguments);
}

Address Bridge::retain(Address object) { return object ? call(objc_library, "_objc_retain", {object}) : 0; }
void Bridge::release(Address object) { if (object) static_cast<void>(call(objc_library, "_objc_release", {object})); }
Address Bridge::autorelease(Address object) { return object ? call(objc_library, "_objc_autorelease", {object}) : 0; }
Address Bridge::block_copy(Address block) { return block ? call(libsystem_library, "__Block_copy", {block}) : 0; }
void Bridge::block_release(Address block) { if (block) static_cast<void>(call(libsystem_library, "__Block_release", {block})); }

Address Bridge::invoke_block(Address block, std::initializer_list<Address> arguments) {
    if (!block) fail("null block");
    std::vector<Address> values{block};
    values.insert(values.end(), arguments.begin(), arguments.end());
    if (values.size() > 8) fail("block call with more than seven arguments");
    return call_address(memory().read64(block + 16), values);
}

Address Bridge::make_block(BlockHandler handler) {
    constexpr Address size = 40;
    const auto block = process.allocate_guest(size);
    auto& guest = process.calling_engine();
    guest.write64(block, symbol(libsystem_library, "__NSConcreteGlobalBlock"));
    guest.write32(block + 8, 1U << 28);   // BLOCK_IS_GLOBAL: copy and release are no-ops
    guest.write32(block + 12, 0);
    guest.write64(block + 16, block_invoke);
    guest.write64(block + 24, block_descriptor);
    guest.write64(block + 32, 0);
    std::lock_guard lock(mutex);
    blocks[block] = std::make_shared<HostBlock>(HostBlock{std::move(handler)});
    return block;
}

void Bridge::forget_block(Address block) {
    std::lock_guard lock(mutex);
    blocks.erase(block);
}

void Bridge::dispatch_async(Address queue, std::function<void()> body) {
    if (!queue) fail("dispatch to a null queue");
    std::uint64_t id{};
    {
        std::lock_guard lock(mutex);
        id = next_work++;
        work.emplace(id, std::move(body));
    }
    try {
        static_cast<void>(call(libsystem_library, "_dispatch_async_f", {queue, id, work_function}));
    } catch (...) {
        std::lock_guard lock(mutex);
        work.erase(id);
        throw;
    }
}

Address Bridge::main_queue() const { return symbol(libsystem_library, "__dispatch_main_q"); }

Address Bridge::create_serial_queue(std::string_view label) {
    // dispatch keeps the label pointer; the text lives as long as the process.
    const auto text = process.allocate_guest(label.size() + 1);
    auto& guest = memory();
    guest.write_bytes(text, std::span(reinterpret_cast<const std::uint8_t*>(label.data()), label.size()));
    guest.write8(text + label.size(), 0);
    return call(libsystem_library, "_dispatch_queue_create", {text, 0});
}

// ---- CF values -----------------------------------------------------------------------------

Address Bridge::string(std::string_view utf8) { return cf.string(utf8); }

std::string Bridge::text(Address object) {
    if (!object || cf.kind(object) != ObjectKind::string) fail("expected a CFString");
    return cf.utf8(object);
}

std::optional<std::string> Bridge::maybe_text(Address object) {
    if (!object || cf.kind(object) != ObjectKind::string) return std::nullopt;
    return cf.utf8(object);
}

Address Bridge::data(std::span<const std::uint8_t> value) {
    const auto buffer = process.allocate_guest(std::max<std::uint64_t>(value.size(), 1));
    try {
        if (!value.empty()) memory().write_bytes(buffer, value);
        const std::array<Address, 3> arguments{0, buffer, value.size()};
        const auto result = cf.invoke("CFDataCreate", arguments);
        process.release_guest(buffer);
        return result;
    } catch (...) {
        process.release_guest(buffer);
        throw;
    }
}

Bytes Bridge::bytes(Address object) {
    if (!is_data(object)) fail("expected a CFData");
    const auto size = cf.length(object);
    Bytes result(static_cast<std::size_t>(size));
    if (size) {
        const std::array<Address, 1> arguments{object};
        memory().read_bytes(cf.invoke("CFDataGetBytePtr", arguments), result);
    }
    return result;
}

bool Bridge::is_data(Address object) { return object && cf.kind(object) == ObjectKind::data; }
bool Bridge::is_string(Address object) { return object && cf.kind(object) == ObjectKind::string; }
bool Bridge::is_dictionary(Address object) { return object && cf.kind(object) == ObjectKind::dictionary; }
bool Bridge::is_array(Address object) { return object && cf.kind(object) == ObjectKind::array; }

Address Bridge::number(std::int64_t value) { return cf.integer(value, 4); }

std::optional<std::int64_t> Bridge::integer(Address object) {
    if (!object) return std::nullopt;
    const auto kind = cf.kind(object);
    if (kind == ObjectKind::boolean) {
        const std::array<Address, 1> arguments{object};
        return static_cast<std::int64_t>(cf.invoke("CFBooleanGetValue", arguments) & 0xff);
    }
    if (kind != ObjectKind::number) return std::nullopt;
    const auto out = process.allocate_guest(8);
    std::int64_t value{};
    try {
        const std::array<Address, 3> arguments{object, 4, out};   // kCFNumberSInt64Type
        static_cast<void>(cf.invoke("CFNumberGetValue", arguments));
        value = static_cast<std::int64_t>(memory().read64(out));
    } catch (...) {
        process.release_guest(out);
        throw;
    }
    process.release_guest(out);
    return value;
}

std::optional<double> Bridge::real(Address object) {
    if (!object) return std::nullopt;
    const auto kind = cf.kind(object);
    if (kind != ObjectKind::number && kind != ObjectKind::boolean) return std::nullopt;
    return cf.number_value(object);
}

Address Bridge::lookup(Address dictionary_object, Address key) {
    if (!dictionary_object) return 0;
    if (!is_dictionary(dictionary_object)) fail("expected a CFDictionary");
    return cf.dictionary_value(dictionary_object, key);
}

Address Bridge::dictionary(const std::vector<std::pair<Address, Address>>& entries) { return cf.dictionary(entries); }
Address Bridge::array(const std::vector<Address>& values) { return cf.array(values); }
void Bridge::release_all(const std::vector<Address>& objects) {
    for (const auto object : objects) if (object) process.objects().release(object);
}

// ---- classes and exports ---------------------------------------------------------------------

Address Bridge::define_class(std::string name, Address superclass, std::string_view library, const std::source_location& where) {
    registration_audit::Site site(where);
    auto cls = objc.find_class(name);
    if (!cls) cls = objc.register_native_class(name, superclass, 8);
    objc.publish_class(name, cls, library);
    return cls;
}

void Bridge::method(Address cls, std::string_view selector, transmute::GuestCallHandler handler, bool class_method, std::string types,
                    const std::source_location& where) {
    registration_audit::Site site(where);
    static_cast<void>(objc.add_native_method(cls, std::string(selector), std::move(handler), class_method, std::move(types)));
}

void Bridge::function(std::string_view library, std::string_view name, transmute::GuestCallHandler handler,
                      const std::source_location& where) {
    registration_audit::Site site(where);
    static_cast<void>(process.install_function(std::string(name), std::move(handler), std::string(library)));
}

Address Bridge::export_string(std::string_view library, std::string_view name, std::string_view value,
                              const std::source_location& where) {
    registration_audit::Site site(where);
    const auto object = cf.constant_string(value);
    export_value(library, name, object);
    return object;
}

void Bridge::export_value(std::string_view library, std::string_view name, std::uint64_t value, const std::source_location& where) {
    registration_audit::Site site(where);
    const auto cell = process.allocate_guest(8);
    process.main_engine().write64(cell, value);
    process.install_export(std::string(library), std::string(name), {cell, ExportKind::data});
}

Address Bridge::constant(std::string_view library, std::string_view name) const {
    return process.main_engine().read64(symbol(library, name));
}

bool Bridge::exported(std::string_view library, std::string_view name) const {
    // A placeholder (an explicit-failure stub bound only so images link) is
    // not an implementation: report it missing so the caller replaces it.
    return process.resolve(library, name).has_value() && !take_placeholder_export(process, library, name);
}

void Bridge::register_type_id(Address cls, TypeId id) {
    bool extend{};
    {
        std::lock_guard lock(mutex);
        type_ids[cls] = id;
        extend = !std::exchange(type_id_extended, true);
    }
    if (!extend) return;
    // CFGetTypeID: the Security / networking CF types answer their own ids;
    // everything else is CoreFoundation's (CoreFoundation::extend, so the
    // export keeps one owner).
    std::weak_ptr<Bridge> self = shared_from_this();
    cf.extend("CFGetTypeID", "Security and networking CF types (Bridge::register_type_id)", [self](CpuState& s, GuestMemory&) -> std::optional<std::uint64_t> {
        const auto bridge = self.lock();
        if (!bridge) fail("CFGetTypeID after the Security provider was destroyed");
        if (const auto id = bridge->classify(s.x[0])) return static_cast<std::uint64_t>(*id);
        return std::nullopt;
    });
}

std::optional<TypeId> Bridge::classify(Address object) const {
    if (!object) return std::nullopt;
    const auto view = process.objects().peek(object);
    if (!view) return std::nullopt;
    std::lock_guard lock(mutex);
    const auto found = type_ids.find(view->class_address);
    if (found == type_ids.end()) return std::nullopt;
    return found->second;
}

void Bridge::store_error(Address out, Address error_object, bool autoreleased) {
    if (!out) { release(error_object); return; }
    memory().write64(out, autoreleased ? autorelease(error_object) : error_object);
}

// ---- process bridge ------------------------------------------------------------------------------

namespace {
std::mutex installed_mutex;
std::shared_ptr<Bridge> installed;
} // namespace

std::shared_ptr<Bridge> bridge() {
    std::lock_guard lock(installed_mutex);
    if (!installed) fail("the Security provider is not installed");
    return installed;
}

std::shared_ptr<Bridge> create_bridge(InstallContext& context) {
    auto result = std::make_shared<Bridge>(context.process, context.objc, context.cf);
    result->object_class = context.objc.find_class("NSObject");
    if (!result->object_class) fail("NSObject is missing");
    std::weak_ptr<Bridge> self = result;
    result->block_invoke = context.process.install_function("__alchemy_security_block_invoke", [self](CpuState& s, GuestMemory& m) {
        const auto owner = self.lock();
        if (!owner) fail("host block called after the Security provider was destroyed");
        std::shared_ptr<Bridge::HostBlock> block;
        {
            std::lock_guard lock(owner->mutex);
            const auto found = owner->blocks.find(s.x[0]);
            if (found == owner->blocks.end()) fail("host completion block called after it was consumed");
            block = found->second;
        }
        block->handler(s, m);
    }, std::string(libsystem_library));
    result->work_function = context.process.install_function("__alchemy_security_dispatch_work", [self](CpuState& s, GuestMemory&) {
        const auto owner = self.lock();
        if (!owner) fail("dispatch work after the Security provider was destroyed");
        std::function<void()> body;
        {
            std::lock_guard lock(owner->mutex);
            const auto found = owner->work.find(s.x[0]);
            if (found == owner->work.end()) fail("dispatch work item ran twice");
            body = std::move(found->second);
            owner->work.erase(found);
        }
        body();
        s.x[0] = 0;
    }, std::string(libsystem_library));
    // Shared block descriptor {reserved, size}.
    result->block_descriptor = context.process.allocate_guest(16);
    context.process.main_engine().write64(result->block_descriptor, 0);
    context.process.main_engine().write64(result->block_descriptor + 8, 40);
    std::lock_guard lock(installed_mutex);
    installed = result;
    return result;
}

// ---- DER ----------------------------------------------------------------------------------------

namespace der {

std::optional<Node> read(std::span<const std::uint8_t> input) {
    if (input.size() < 2) return std::nullopt;
    Node node;
    node.tag = input[0];
    if ((node.tag & 0x1f) == 0x1f) return std::nullopt;   // high tag numbers are not used here
    std::size_t at = 1;
    std::size_t length = input[at++];
    if (length & 0x80) {
        const auto count = length & 0x7f;
        if (count == 0 || count > 4 || at + count > input.size()) return std::nullopt;
        length = 0;
        for (std::size_t index = 0; index < count; ++index) length = (length << 8) | input[at++];
    }
    if (length > input.size() - at) return std::nullopt;
    node.value = input.subspan(at, length);
    node.whole = input.subspan(0, at + length);
    return node;
}

std::optional<std::vector<Node>> children(const Node& node) {
    std::vector<Node> result;
    auto rest = node.value;
    while (!rest.empty()) {
        const auto child = read(rest);
        if (!child) return std::nullopt;
        result.push_back(*child);
        rest = rest.subspan(child->whole.size());
    }
    return result;
}

Bytes unsigned_integer(std::span<const std::uint8_t> value) {
    std::size_t at = 0;
    while (at + 1 < value.size() && value[at] == 0) ++at;
    return Bytes(value.begin() + static_cast<std::ptrdiff_t>(at), value.end());
}

Bytes encode(std::uint8_t tag, std::span<const std::uint8_t> contents) {
    Bytes result{tag};
    const auto size = contents.size();
    if (size < 0x80) result.push_back(static_cast<std::uint8_t>(size));
    else {
        Bytes length;
        for (auto value = size; value; value >>= 8) length.insert(length.begin(), static_cast<std::uint8_t>(value & 0xff));
        result.push_back(static_cast<std::uint8_t>(0x80 | length.size()));
        result.insert(result.end(), length.begin(), length.end());
    }
    result.insert(result.end(), contents.begin(), contents.end());
    return result;
}

Bytes integer(std::span<const std::uint8_t> big_endian) {
    auto value = unsigned_integer(big_endian);
    if (value.empty()) value.push_back(0);
    if (value[0] & 0x80) value.insert(value.begin(), 0);
    return encode(0x02, value);
}

Bytes sequence(const std::vector<Bytes>& parts) {
    Bytes contents;
    for (const auto& part : parts) contents.insert(contents.end(), part.begin(), part.end());
    return encode(0x30, contents);
}

Bytes sequence(std::initializer_list<Bytes> parts) { return sequence(std::vector<Bytes>(parts)); }

std::string oid(std::span<const std::uint8_t> value) {
    if (value.empty()) return {};
    std::string result = std::to_string(value[0] / 40) + "." + std::to_string(value[0] % 40);
    std::uint64_t component = 0;
    for (std::size_t index = 1; index < value.size(); ++index) {
        component = (component << 7) | (value[index] & 0x7f);
        if (!(value[index] & 0x80)) { result += "." + std::to_string(component); component = 0; }
    }
    return result;
}

} // namespace der

std::string base64_encode(std::span<const std::uint8_t> bytes) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (std::size_t at = 0; at < bytes.size(); at += 3) {
        const std::uint32_t chunk = (bytes[at] << 16) | (at + 1 < bytes.size() ? bytes[at + 1] << 8 : 0) |
                                    (at + 2 < bytes.size() ? bytes[at + 2] : 0);
        result += alphabet[(chunk >> 18) & 63];
        result += alphabet[(chunk >> 12) & 63];
        result += at + 1 < bytes.size() ? alphabet[(chunk >> 6) & 63] : '=';
        result += at + 2 < bytes.size() ? alphabet[chunk & 63] : '=';
    }
    return result;
}

std::optional<Bytes> base64_decode(std::string_view text) {
    Bytes result;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const auto c : text) {
        int value;
        if (c >= 'A' && c <= 'Z') value = c - 'A';
        else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
        else if (c >= '0' && c <= '9') value = c - '0' + 52;
        else if (c == '+' || c == '-') value = 62;
        else if (c == '/' || c == '_') value = 63;
        else if (c == '=' || c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
        else return std::nullopt;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xff));
        }
    }
    return result;
}

std::string hex(std::span<const std::uint8_t> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto value : bytes) { result += digits[value >> 4]; result += digits[value & 15]; }
    return result;
}

} // namespace alchemy::gui::security
