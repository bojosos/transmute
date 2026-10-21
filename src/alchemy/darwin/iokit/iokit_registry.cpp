#include "alchemy/darwin/kit/export_fill/export_fill.hpp"

#include "alchemy/darwin/metal/gpu_identity.hpp"
#include "alchemy/object_store.hpp"
#include "alchemy/host/system.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <optional>


// A small read-only IOKit registry describing the host, replacing the empty
// headless registry of darwin_frameworks.cpp for the lookups a renderer plug-in's
// licensing client (libcgauth) makes:
//
//   IOPlatformExpertDevice   root of the IOService plane ("IOService:/"):
//                            IOPlatformUUID (the Windows MachineGuid),
//                            IOPlatformSerialNumber (derived from it), model.
//   IOEthernetController     one per host adapter with a MAC address:
//     IOEthernetInterface    IOMACAddress on the controller; the interface
//                            carries "BSD Name" (en<n>, as getifaddrs),
//                            IOPrimaryInterface, IOBuiltin.
//   IOPCIDevice "display"    one per host GPU (metal::gpu_identities, the
//                            Vulkan physical devices): vendor-id, device-id
//                            (4-byte little-endian data, as on Macs),
//                            class-code 0x30000 (VGA display), model (the
//                            name). No revision-id: Vulkan reports no PCI
//                            revision, and ANGLE's IOGraphicsAccelerator2
//                            scan reads it only when present (Dawn not at
//                            all), so the key is left out rather than
//                            invented. A software (CPU) Vulkan device is an
//                            IOService entry with the same ids instead: it
//                            is no PCI device and has no class code.
//     AlchemyVulkanAccelerator  IOGraphicsAccelerator2 / IOAccelerator; its
//                            registry entry ID is the GPU's
//                            MTLDevice.registryID, so
//                            IORegistryEntryIDMatching([device registryID])
//                            finds it and its parent has the PCI ids
//                            (Dawn's GetDeviceIORegistryPCIInfo, ANGLE's
//                            IOGraphicsAccelerator2 scan).
//
// io_object_t handles are reference-counted names; matching supports
// IOProviderClass (class conformance), "BSD Name", IORegistryEntryID and
// IOPropertyMatch.
// User clients do not exist: IOServiceOpen fails with kIOReturnUnsupported
// and the IOConnect* calls with kIOReturnBadArgument.
namespace alchemy::export_fill {
// The host UUID (host::system::machine_uuid, the Windows MachineGuid on
// win32) as upper-case 8-4-4-4-12 text: IOPlatformUUID and the ByHost
// preferences identifier.
std::string host_machine_uuid() {
    const auto uuid = alchemy::host::system::machine_uuid();
    if (!uuid) return "00000000-0000-0000-0000-000000000000";
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string value;
    for (std::size_t i = 0; i < uuid->size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) value.push_back('-');
        value.push_back(digits[(*uuid)[i] >> 4]);
        value.push_back(digits[(*uuid)[i] & 0xf]);
    }
    return value;
}

namespace {

constexpr std::string_view iokit = "/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit";
// <IOKit/IOKitKeys.h>, <IOKit/IOTypes.h>
constexpr std::uint64_t kern_success = 0, kern_invalid_argument = 4;
constexpr std::uint64_t iterate_recursively = 1, iterate_parents = 2;
constexpr std::uint32_t first_handle = 0x2c03;

struct Entry {
    std::vector<std::string> classes;   // most derived first
    std::vector<std::pair<std::string, std::uint64_t>> properties;   // immortal CF values
    std::optional<std::size_t> parent;
    std::vector<std::size_t> children;
    std::optional<std::uint64_t> id;     // registry entry ID; default entry_id_base + index
    std::string name;                    // IORegistryEntryGetName; default the class name
};

constexpr std::uint64_t entry_id_base = 0x100000100ULL;
// <IOKit/pci/IOPCIDevice.h> class codes: display controller, VGA-compatible.
constexpr std::uint32_t class_code_display_vga = 0x30000;

struct Object {
    std::uint32_t references{1};
    std::optional<std::size_t> entry;
    std::vector<std::size_t> items;   // iterator contents
    std::size_t position{};
};

class Registry {
public:
    Registry(ProcessContext& process, CoreFoundation& cf) : process_(process), cf_(cf) {}

    void build() {
        const auto uuid = host_machine_uuid();
        std::string serial;
        for (const auto c : uuid) if (c != '-' && serial.size() < 12) serial.push_back(c);
        const auto platform = add({"IOPlatformExpertDevice", "IOService", "IORegistryEntry"}, {});
        property(platform, "IOPlatformUUID", constant(cf_.string(uuid)));
        property(platform, "IOPlatformSerialNumber", constant(cf_.string(serial)));
        property(platform, "IOPlatformSystemSerialNumber", constant(cf_.string(serial)));
        property(platform, "model", data("Alchemy1,1", true));
        property(platform, "manufacturer", data("Alchemy", true));
        bool primary = true;
        for (const auto& item : host_interfaces()) {
            if (item.loopback || item.mac.size() != 6 || !item.name.starts_with("en")) continue;
            const auto controller = add({"IOEthernetController", "IONetworkController", "IOService", "IORegistryEntry"}, platform);
            property(controller, "IOMACAddress", data(std::string(item.mac.begin(), item.mac.end()), false));
            const auto interface = add({"IOEthernetInterface", "IONetworkInterface", "IOService", "IORegistryEntry"}, controller);
            property(interface, "BSD Name", constant(cf_.string(item.name)));
            property(interface, "IOInterfaceName", constant(cf_.string("en")));
            property(interface, "IOInterfaceUnit", constant(cf_.integer(std::stoi(item.name.substr(2)))));
            property(interface, "IOPrimaryInterface", cf_.boolean(primary));
            property(interface, "IOBuiltin", cf_.boolean(true));
            primary = false;
        }
        add_gpus(platform);
    }

    std::uint32_t handle(std::optional<std::size_t> entry, std::vector<std::size_t> items = {}) {
        std::lock_guard lock(mutex_);
        const auto name = next_++;
        objects_[name] = Object{1, entry, std::move(items), 0};
        return name;
    }
    std::optional<Object> object(std::uint32_t name) {
        std::lock_guard lock(mutex_);
        const auto found = objects_.find(name);
        if (found == objects_.end()) return std::nullopt;
        return found->second;
    }
    bool retain(std::uint32_t name) {
        std::lock_guard lock(mutex_);
        const auto found = objects_.find(name);
        if (found == objects_.end()) return false;
        ++found->second.references;
        return true;
    }
    bool release(std::uint32_t name) {
        std::lock_guard lock(mutex_);
        const auto found = objects_.find(name);
        if (found == objects_.end()) return false;
        if (--found->second.references == 0) objects_.erase(found);
        return true;
    }
    std::optional<std::size_t> next(std::uint32_t iterator) {
        std::lock_guard lock(mutex_);
        const auto found = objects_.find(iterator);
        if (found == objects_.end() || found->second.entry) return std::nullopt;
        auto& value = found->second;
        if (value.position >= value.items.size()) return std::nullopt;
        return value.items[value.position++];
    }

    void reset(std::uint32_t iterator) {
        std::lock_guard lock(mutex_);
        if (const auto found = objects_.find(iterator); found != objects_.end()) found->second.position = 0;
    }
    const Entry& entry(std::size_t index) const { return entries_.at(index); }
    std::uint64_t entry_id(std::size_t index) const { return entries_.at(index).id.value_or(entry_id_base + index); }
    std::string entry_name(std::size_t index) const {
        const auto& value = entries_.at(index);
        return value.name.empty() ? value.classes.front() : value.name;
    }
    std::size_t size() const noexcept { return entries_.size(); }
    std::optional<std::size_t> root() const { return entries_.empty() ? std::nullopt : std::optional<std::size_t>{0}; }

    bool conforms(std::size_t index, std::string_view name) const {
        const auto& classes = entries_.at(index).classes;
        return std::find(classes.begin(), classes.end(), name) != classes.end();
    }
    std::optional<std::uint64_t> value(std::size_t index, std::string_view key) const {
        for (const auto& [name, object] : entries_.at(index).properties) if (name == key) return object;
        return std::nullopt;
    }
    // Entries reached from `start` in the given direction (not including it).
    std::vector<std::size_t> walk(std::size_t start, bool parents, bool recursive) const {
        std::vector<std::size_t> result;
        if (parents) {
            for (auto at = entries_.at(start).parent; at; at = recursive ? entries_.at(*at).parent : std::nullopt) result.push_back(*at);
            return result;
        }
        std::vector<std::size_t> stack(entries_.at(start).children.rbegin(), entries_.at(start).children.rend());
        while (!stack.empty()) {
            const auto at = stack.back();
            stack.pop_back();
            result.push_back(at);
            if (recursive) for (auto child = entries_.at(at).children.rbegin(); child != entries_.at(at).children.rend(); ++child) stack.push_back(*child);
        }
        return result;
    }
    // IOServiceMatching semantics over a CFDictionary.
    std::vector<std::size_t> match(std::uint64_t dictionary) {
        std::vector<std::size_t> result;
        const auto entries = cf_.dictionary_entries(dictionary);
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            bool ok = true;
            for (const auto& [key, value] : entries) {
                const auto name = cf_.utf8(key);
                if (name == "IOProviderClass" || name == "IONameMatch") ok = conforms(index, cf_.utf8(value));
                else if (name == "BSD Name") { const auto own = this->value(index, "BSD Name"); ok = own && cf_.equal(*own, value); }
                else if (name == "IOPropertyMatch") {
                    for (const auto& [property, expected] : cf_.dictionary_entries(value)) {
                        const auto own = this->value(index, cf_.utf8(property));
                        if (!own || !cf_.equal(*own, expected)) { ok = false; break; }
                    }
                } else if (name == "IORegistryEntryID") {
                    const auto own = cf_.integer(static_cast<std::int64_t>(entry_id(index)));
                    ok = cf_.equal(own, value);
                    process_.objects().release(own);
                }
                else throw std::runtime_error("Alchemy IOKit: unsupported matching key " + name);
                if (!ok) break;
            }
            if (ok) result.push_back(index);
        }
        return result;
    }

private:
    std::uint64_t constant(std::uint64_t object) {
        if (const auto record = process_.objects().find(object)) record->immortal = true;
        return object;
    }
    std::uint64_t data(const std::string& bytes, bool terminate) {
        auto copy = bytes;
        if (terminate) copy.push_back('\0');
        const auto buffer = process_.allocate_guest(copy.size() + 1);
        process_.main_engine().write_bytes(buffer, std::span(reinterpret_cast<const std::uint8_t*>(copy.data()), copy.size()));
        const std::array<std::uint64_t, 3> arguments{0, buffer, copy.size()};
        const auto object = constant(cf_.invoke("CFDataCreate", arguments));
        process_.release_guest(buffer);
        return object;
    }
    std::size_t add(std::vector<std::string> classes, std::optional<std::size_t> parent) {
        entries_.push_back(Entry{std::move(classes), {}, parent, {}});
        const auto index = entries_.size() - 1;
        if (parent) entries_[*parent].children.push_back(index);
        return index;
    }
    void property(std::size_t index, std::string name, std::uint64_t value) { entries_[index].properties.emplace_back(std::move(name), value); }
    // A 32-bit registry number as Macs store PCI ids: 4 bytes, little-endian.
    std::uint64_t number_data(std::uint32_t value) {
        std::string bytes(4, '\0');
        for (int i = 0; i < 4; ++i) bytes[i] = static_cast<char>((value >> (8 * i)) & 0xff);
        return data(bytes, false);
    }
    // One provider entry and accelerator per GPU; duplicates of one adapter
    // (one registry ID listed by two Vulkan drivers) keep the Metal device's.
    void add_gpus(std::size_t root) {
        std::vector<metal::GpuIdentity> gpus;
        for (auto& gpu : metal::gpu_identities()) {
            const auto same = std::find_if(gpus.begin(), gpus.end(), [&](const auto& other) { return other.registry_id == gpu.registry_id; });
            if (same == gpus.end()) gpus.push_back(gpu);
            else if (gpu.metal) *same = gpu;
        }
        for (const auto& gpu : gpus) {
            const auto device = gpu.cpu ? add({"AlchemySoftwareGPU", "IOService", "IORegistryEntry"}, root)
                                        : add({"IOPCIDevice", "IOService", "IORegistryEntry"}, root);
            entries_[device].name = gpu.cpu ? "AlchemySoftwareGPU" : "display";
            property(device, "vendor-id", number_data(gpu.vendor_id));
            property(device, "device-id", number_data(gpu.device_id));
            if (!gpu.cpu) property(device, "class-code", number_data(class_code_display_vga));
            property(device, "model", data(gpu.name, true));
            property(device, "IOName", constant(cf_.string(entries_[device].name)));
            const auto accelerator = add({"AlchemyVulkanAccelerator", "IOGraphicsAccelerator2", "IOAccelerator", "IOService", "IORegistryEntry"}, device);
            entries_[accelerator].id = gpu.registry_id;
            property(accelerator, "IOClass", constant(cf_.string("AlchemyVulkanAccelerator")));
        }
    }

    ProcessContext& process_;
    CoreFoundation& cf_;
    std::vector<Entry> entries_;
    std::mutex mutex_;
    std::map<std::uint32_t, Object> objects_;
    std::uint32_t next_{first_handle};
};

} // namespace

void install_iokit_registry(Provider& p, ObjcRuntime&, CoreFoundation& cf) {
    auto& process = p.process();
    auto registry = std::make_shared<Registry>(process, cf);
    auto built = std::make_shared<std::once_flag>();
    // Built on first use: host queries and CF objects are not created at install.
    const auto ready = [registry, built] { std::call_once(*built, [&] { registry->build(); }); return registry; };
    const auto install = [&process](const char* name, transmute::GuestCallHandler handler) {
        static_cast<void>(process.install_function(name, std::move(handler), std::string(iokit)));
    };
    const auto mutable_dictionary = [&cf, &process] {
        const std::array<std::uint64_t, 4> create{0, 0, process.resolve(core_foundation_library, "_kCFTypeDictionaryKeyCallBacks")->address,
                                                  process.resolve(core_foundation_library, "_kCFTypeDictionaryValueCallBacks")->address};
        return cf.invoke("CFDictionaryCreateMutable", create);
    };
    const auto set = [&cf, &process](std::uint64_t dictionary, std::string_view key, std::uint64_t value) {
        const auto name = cf.string(key);
        const std::array<std::uint64_t, 3> arguments{dictionary, name, value};
        static_cast<void>(cf.invoke("CFDictionarySetValue", arguments));
        process.objects().release(name);
    };
    const auto retained = [&cf](std::uint64_t object) {
        const std::array<std::uint64_t, 1> arguments{object};
        return cf.invoke("CFRetain", arguments);
    };

    // CFMutableDictionaryRef IOServiceMatching(const char* name)
    install("_IOServiceMatching", [mutable_dictionary, set, &cf, &process](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) { s.x[0] = 0; return; }
        const auto dictionary = mutable_dictionary();
        const auto name = cf.string(guest_string(m, s.x[0], 256));
        set(dictionary, "IOProviderClass", name);
        process.objects().release(name);
        s.x[0] = dictionary;
    });
    install("_IOServiceNameMatching", [mutable_dictionary, set, &cf, &process](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) { s.x[0] = 0; return; }
        const auto dictionary = mutable_dictionary();
        const auto name = cf.string(guest_string(m, s.x[0], 256));
        set(dictionary, "IONameMatch", name);
        process.objects().release(name);
        s.x[0] = dictionary;
    });
    // CFMutableDictionaryRef IOBSDNameMatching(mach_port_t, uint32_t options, const char* bsdName)
    install("_IOBSDNameMatching", [mutable_dictionary, set, &cf, &process](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) { s.x[0] = 0; return; }
        const auto dictionary = mutable_dictionary();
        const auto name = cf.string(guest_string(m, s.x[2], 256));
        set(dictionary, "BSD Name", name);
        process.objects().release(name);
        s.x[0] = dictionary;
    });
    // io_service_t IOServiceGetMatchingService(mach_port_t, CFDictionaryRef matching /* consumed */)
    install("_IOServiceGetMatchingService", [ready, &process](CpuState& s, GuestMemory&) {
        if (!s.x[1]) { s.x[0] = 0; return; }
        const auto registry = ready();
        const auto found = registry->match(s.x[1]);
        process.objects().release(s.x[1]);
        s.x[0] = found.empty() ? 0 : registry->handle(found.front());
    });
    // kern_return_t IOServiceGetMatchingServices(mach_port_t, CFDictionaryRef /* consumed */, io_iterator_t*)
    install("_IOServiceGetMatchingServices", [ready, &process](CpuState& s, GuestMemory& m) {
        if (!s.x[1] || !s.x[2]) { if (s.x[1]) process.objects().release(s.x[1]); s.x[0] = kern_invalid_argument; return; }
        const auto registry = ready();
        auto found = registry->match(s.x[1]);
        process.objects().release(s.x[1]);
        m.write32(s.x[2], registry->handle(std::nullopt, std::move(found)));
        s.x[0] = kern_success;
    });
    // io_registry_entry_t IORegistryEntryFromPath(mach_port_t, const io_string_t path)
    install("_IORegistryEntryFromPath", [ready](CpuState& s, GuestMemory& m) {
        const auto path = guest_string(m, s.x[1], 512);
        const auto registry = ready();
        s.x[0] = (path == "IOService:/" || path == "IOService:" || path == "IODeviceTree:/") && registry->root() ? registry->handle(registry->root()) : 0;
    });
    install("_IOIteratorNext", [ready](CpuState& s, GuestMemory&) {
        const auto registry = ready();
        const auto next = registry->next(static_cast<std::uint32_t>(s.x[0]));
        s.x[0] = next ? registry->handle(*next) : 0;
    });
    install("_IOIteratorReset", [ready](CpuState& s, GuestMemory&) { ready()->reset(static_cast<std::uint32_t>(s.x[0])); s.x[0] = 0; });
    install("_IOObjectRelease", [ready](CpuState& s, GuestMemory&) {
        s.x[0] = !s.x[0] || ready()->release(static_cast<std::uint32_t>(s.x[0])) ? kern_success : kern_invalid_argument;
    });
    install("_IOObjectRetain", [ready](CpuState& s, GuestMemory&) {
        s.x[0] = ready()->retain(static_cast<std::uint32_t>(s.x[0])) ? kern_success : kern_invalid_argument;
    });
    // boolean_t IOObjectConformsTo(io_object_t, const io_name_t className)
    install("_IOObjectConformsTo", [ready](CpuState& s, GuestMemory& m) {
        const auto registry = ready();
        const auto object = registry->object(static_cast<std::uint32_t>(s.x[0]));
        const auto name = guest_string(m, s.x[1], 128);
        s.x[0] = object && object->entry ? (registry->conforms(*object->entry, name) ? 1 : 0)
                                         : (object && name == "IOIterator" ? 1 : 0);
    });
    // CFTypeRef IORegistryEntryCreateCFProperty(io_registry_entry_t, CFStringRef key, CFAllocatorRef, IOOptionBits)
    install("_IORegistryEntryCreateCFProperty", [ready, retained, &cf](CpuState& s, GuestMemory&) {
        const auto registry = ready();
        const auto object = registry->object(static_cast<std::uint32_t>(s.x[0]));
        if (!object || !object->entry || !s.x[1]) { s.x[0] = 0; return; }
        const auto value = registry->value(*object->entry, cf.utf8(s.x[1]));
        s.x[0] = value ? retained(*value) : 0;
    });
    // kern_return_t IORegistryEntryCreateCFProperties(entry, CFMutableDictionaryRef *properties, CFAllocatorRef, IOOptionBits)
    install("_IORegistryEntryCreateCFProperties", [ready, mutable_dictionary, set](CpuState& s, GuestMemory& m) {
        const auto registry = ready();
        const auto object = registry->object(static_cast<std::uint32_t>(s.x[0]));
        if (!object || !object->entry || !s.x[1]) { s.x[0] = kern_invalid_argument; return; }
        const auto dictionary = mutable_dictionary();
        for (const auto& [name, value] : registry->entry(*object->entry).properties) set(dictionary, name, value);
        m.write64(s.x[1], dictionary);
        s.x[0] = kern_success;
    });
    // kern_return_t IORegistryEntryGetName(entry, io_name_t name): the entry's class name (its default IOName).
    install("_IORegistryEntryGetName", [ready](CpuState& s, GuestMemory& m) {
        const auto registry = ready();
        const auto object = registry->object(static_cast<std::uint32_t>(s.x[0]));
        if (!object || !object->entry || !s.x[1]) { s.x[0] = kern_invalid_argument; return; }
        auto name = registry->entry_name(*object->entry);
        if (name.size() > 127) name.resize(127);
        std::vector<std::uint8_t> bytes(name.begin(), name.end());
        bytes.push_back(0);
        m.write_bytes(s.x[1], bytes);
        s.x[0] = kern_success;
    });
    // kern_return_t IORegistryEntryGetRegistryEntryID(entry, uint64_t *entryID): stable per entry.
    install("_IORegistryEntryGetRegistryEntryID", [ready](CpuState& s, GuestMemory& m) {
        const auto registry = ready();
        const auto object = registry->object(static_cast<std::uint32_t>(s.x[0]));
        if (!object || !object->entry || !s.x[1]) { s.x[0] = kern_invalid_argument; return; }
        m.write64(s.x[1], registry->entry_id(*object->entry));
        s.x[0] = kern_success;
    });
    // CFTypeRef IORegistryEntrySearchCFProperty(entry, const io_name_t plane, CFStringRef key, CFAllocatorRef, IOOptionBits)
    install("_IORegistryEntrySearchCFProperty", [ready, retained, &cf](CpuState& s, GuestMemory&) {
        const auto registry = ready();
        const auto object = registry->object(static_cast<std::uint32_t>(s.x[0]));
        if (!object || !object->entry || !s.x[2]) { s.x[0] = 0; return; }
        const auto key = cf.utf8(s.x[2]);
        if (const auto own = registry->value(*object->entry, key)) { s.x[0] = retained(*own); return; }
        if (s.x[4] & iterate_recursively) {
            for (const auto index : registry->walk(*object->entry, (s.x[4] & iterate_parents) != 0, true))
                if (const auto found = registry->value(index, key)) { s.x[0] = retained(*found); return; }
        }
        s.x[0] = 0;
    });
    // kern_return_t IORegistryEntryCreateIterator(entry, plane, IOOptionBits, io_iterator_t*)
    install("_IORegistryEntryCreateIterator", [ready](CpuState& s, GuestMemory& m) {
        const auto registry = ready();
        const auto object = registry->object(static_cast<std::uint32_t>(s.x[0]));
        if (!object || !object->entry || !s.x[3]) { s.x[0] = kern_invalid_argument; return; }
        m.write32(s.x[3], registry->handle(std::nullopt,
            registry->walk(*object->entry, (s.x[2] & iterate_parents) != 0, (s.x[2] & iterate_recursively) != 0)));
        s.x[0] = kern_success;
    });
    // kern_return_t IORegistryEntryGetParentEntry(entry, plane, io_registry_entry_t* parent)
    install("_IORegistryEntryGetParentEntry", [ready](CpuState& s, GuestMemory& m) {
        const auto registry = ready();
        const auto object = registry->object(static_cast<std::uint32_t>(s.x[0]));
        if (!object || !object->entry || !s.x[2]) { s.x[0] = kern_invalid_argument; return; }
        const auto parent = registry->entry(*object->entry).parent;
        if (!parent) { m.write32(s.x[2], 0); s.x[0] = 0xe00002c0; return; }   // kIOReturnNoDevice
        m.write32(s.x[2], registry->handle(parent));
        s.x[0] = kern_success;
    });
    // kern_return_t IOServiceOpen(io_service_t, task_port_t, uint32_t type, io_connect_t* connect):
    // no registry entry has a user client (driver connections need kernel
    // drivers), so opening a service fails with kIOReturnUnsupported; an
    // invalid service with kIOReturnBadArgument.
    install("_IOServiceOpen", [ready](CpuState& s, GuestMemory& m) {
        if (s.x[3]) m.write32(s.x[3], 0);
        const auto object = s.x[0] ? ready()->object(static_cast<std::uint32_t>(s.x[0])) : std::nullopt;
        s.x[0] = object && object->entry ? 0xe00002c7 : 0xe00002c2;
    });
    // Connections are never handed out, so every connect argument is invalid
    // (IOServiceClose: iokit.cpp, which also closes power root ports).
    for (const auto name : {"_IOConnectCallMethod", "_IOConnectCallStructMethod", "_IOConnectCallScalarMethod",
                            "_IOConnectCallAsyncMethod", "_IOConnectCallAsyncStructMethod", "_IOConnectCallAsyncScalarMethod"})
        install(name, [](CpuState& s, GuestMemory&) { s.x[0] = 0xe00002c2; });
}

} // namespace alchemy::export_fill
