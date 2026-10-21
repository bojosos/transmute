#include "alchemy/darwin/coregraphics/q2d_internal.hpp"
#include "alchemy/darwin/coregraphics/quartz2d.hpp"

#include "alchemy/path.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>

// The Quartz provider object's helpers (its installation is install.cpp).
namespace alchemy::gui::quartz2d {
void fail(std::string_view why) { throw std::runtime_error("Alchemy Quartz: " + std::string(why)); }
void unsupported(std::string_view what) { throw std::runtime_error("unsupported: Alchemy Quartz: " + std::string(what)); }
void diagnostic(std::string_view text) {
    std::fprintf(stderr, "Alchemy Quartz <Error>: %.*s\n", static_cast<int>(text.size()), text.data());
    std::fflush(stderr);
}

Address Quartz::declare_class(std::string_view name, std::string_view superclass, std::string_view library) {
    auto cls = objc.find_class(name);
    if (!cls) {
        const auto parent = objc.find_class(superclass);
        if (!parent) fail("missing superclass " + std::string(superclass));
        cls = objc.register_native_class(std::string(name), parent, 8);
    }
    if (!library.empty()) objc.publish_class(name, cls, library);
    return cls;
}

void Quartz::function(std::string_view library, std::string symbol, Handler handler,
                       const std::source_location& where) {
    registration_audit::Site site(where);
    static_cast<void>(process.install_function(std::move(symbol),
        [owner = shared_from_this(), handler = std::move(handler)](CpuState& s, GuestMemory& m) {
            const ProviderScope scope;
            handler(s, m);
        },
        std::string(library)));
}

void Quartz::string_constant(std::string_view library, std::string symbol, std::string_view value,
                       const std::source_location& where) {
    registration_audit::Site site(where);
    const auto slot = process.allocate_guest(8);
    process.main_engine().write64(slot, cf.constant_string(value));
    process.install_export(std::string(library), std::move(symbol), {slot, ExportKind::data});
}

void Quartz::data_constant(std::string_view library, std::string symbol, std::span<const std::uint8_t> bytes,
                       const std::source_location& where) {
    registration_audit::Site site(where);
    const auto slot = process.allocate_guest(std::max<std::size_t>(bytes.size(), 8));
    process.main_engine().write_bytes(slot, bytes);
    process.install_export(std::string(library), std::move(symbol), {slot, ExportKind::data});
}

Address Quartz::create(Address cls, std::shared_ptr<ObjectPayload> payload, std::function<void(Address)> destroy) {
    if (destroy) destroy = [destroy = std::move(destroy)](Address object) {
        const ProviderScope scope;
        destroy(object);
    };
    return process.objects().create(16, cls, ObjectKind::instance, std::move(payload), std::move(destroy));
}

Address Quartz::autorelease(Address object) { return object ? send(object, "autorelease").x0 : 0; }

CallResult Quartz::send(Address receiver, std::string_view selector, CallRegs registers) {
    return objc.send(receiver, objc.selector(selector), std::move(registers));
}
Address Quartz::send_id(Address receiver, std::string_view selector, std::initializer_list<Address> arguments) {
    CallRegs registers;
    unsigned index = 2;
    for (const auto value : arguments) registers.x.at(index++) = value;
    return send(receiver, selector, registers).x0;
}
bool Quartz::is_kind(Address object, Address cls) {
    if (!object || !cls) return false;
    for (auto current = objc.object_class(object); current; current = memory().read64(current + 8))
        if (current == cls) return true;
    return false;
}
Address Quartz::call_guest(Address function, std::initializer_list<Address> arguments) {
    if (!function) return 0;
    return process.calling_engine().invoke_callback(function, std::span(arguments.begin(), arguments.size()));
}

std::vector<std::uint8_t> Quartz::read(Address address, std::size_t size) const {
    std::vector<std::uint8_t> bytes(size);
    if (size) memory().read_bytes(address, bytes);
    return bytes;
}
void Quartz::write(Address address, std::span<const std::uint8_t> bytes) const {
    if (!bytes.empty()) memory().write_bytes(address, bytes);
}
std::vector<std::uint16_t> Quartz::read_units(Address address, std::size_t count) const {
    std::vector<std::uint16_t> units(count);
    if (count) memory().read_bytes(address, std::span(reinterpret_cast<std::uint8_t*>(units.data()), count * 2));
    return units;
}
void Quartz::write_units(Address address, std::span<const std::uint16_t> units) const {
    if (!units.empty()) memory().write_bytes(address, std::span(reinterpret_cast<const std::uint8_t*>(units.data()), units.size() * 2));
}
Affine Quartz::read_affine(Address address) const {
    if (!address) fail("null CGAffineTransform pointer");
    return {read_double(address), read_double(address + 8), read_double(address + 16), read_double(address + 24),
            read_double(address + 32), read_double(address + 40)};
}
void Quartz::write_affine(Address address, const Affine& value) const {
    if (!address) fail("null CGAffineTransform result pointer");
    write_double(address, value.a); write_double(address + 8, value.b); write_double(address + 16, value.c);
    write_double(address + 24, value.d); write_double(address + 32, value.tx); write_double(address + 40, value.ty);
}
Rect Quartz::read_rect(Address address) const {
    return {{read_double(address), read_double(address + 8)}, {read_double(address + 16), read_double(address + 24)}};
}
Point Quartz::read_point(Address address) const { return {read_double(address), read_double(address + 8)}; }

Quartz::GuestBlock Quartz::allocate(std::size_t size) {
    size = std::max<std::size_t>(size, 16);
    if (const auto heap = engine_heap_allocate(process.main_engine(), size, 16)) return {*heap, true};
    return {process.allocate_guest(size), false};
}
void Quartz::free(GuestBlock block) {
    if (!block.address) return;
    if (block.heap) engine_heap_release(process.main_engine(), block.address);
    else process.release_guest(block.address);
}

Address Quartz::data_object(std::span<const std::uint8_t> bytes) {
    Scratch buffer(*this, bytes.size());
    write(buffer.address(), bytes);
    const std::array<Address, 3> arguments{0, buffer.address(), bytes.size()};
    return cf.invoke("CFDataCreate", arguments);
}
std::vector<std::uint8_t> Quartz::data_bytes(Address data) {
    if (!data) return {};
    const std::array<Address, 1> argument{data};
    const auto length = cf.invoke("CFDataGetLength", argument);
    if (length > (1ULL << 32)) fail("CFData is too large");
    const auto pointer = cf.invoke("CFDataGetBytePtr", argument);
    return read(pointer, static_cast<std::size_t>(length));
}
void Quartz::append_data(Address mutable_data, std::span<const std::uint8_t> bytes) {
    Scratch buffer(*this, bytes.size());
    write(buffer.address(), bytes);
    const std::array<Address, 3> arguments{mutable_data, buffer.address(), bytes.size()};
    static_cast<void>(cf.invoke("CFDataAppendBytes", arguments));
}
std::optional<double> Quartz::number(Address object) {
    if (!object) return std::nullopt;
    const auto kind = cf.kind(object);
    if (kind == ObjectKind::number) return cf.number_value(object);
    if (kind == ObjectKind::boolean) return object == cf.boolean(true) ? 1.0 : 0.0;
    return std::nullopt;
}
Address Quartz::dictionary_get(Address dictionary, std::string_view key) {
    if (!dictionary || cf.kind(dictionary) != ObjectKind::dictionary) return 0;
    for (const auto& [name, value] : cf.dictionary_entries(dictionary))
        if (cf.kind(name) == ObjectKind::string && cf.utf8(name) == key) return value;
    return 0;
}

std::filesystem::path Quartz::host_path(std::string_view guest) const {
    return process.map_guest_path ? process.map_guest_path(guest) : utf8_path(guest);
}
std::optional<std::vector<std::uint8_t>> Quartz::read_file(std::string_view guest) const {
    std::ifstream file(host_path(guest), std::ios::binary);
    if (!file) return std::nullopt;
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}
bool Quartz::write_file(std::string_view guest, std::span<const std::uint8_t> bytes) const {
    std::ofstream file(host_path(guest), std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file);
}

} // namespace alchemy::gui::quartz2d
