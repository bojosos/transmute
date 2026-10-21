// IOKit registry GPU entries (darwin/iokit/iokit_registry.cpp) from injected
// GPU identities: one IOPCIDevice per GPU with Mac-style 4-byte PCI id data
// and an IOGraphicsAccelerator2 child whose registry entry ID is the
// MTLDevice.registryID, found the way Dawn's GetDeviceIORegistryPCIInfo and
// ANGLE's accelerator scan look (IORegistryEntryIDMatching, parent entry,
// IORegistryEntrySearchCFProperty). A software Vulkan device is no PCI
// device; one adapter listed twice keeps the Metal device's entry.
#include "alchemy/gui/tests/fill_fixture.hpp"
#include "alchemy/darwin/metal/gpu_identity.hpp"

namespace {
using namespace fill_test;

constexpr std::string_view iokit = "/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit";
constexpr std::string_view cf_library = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr std::uint64_t discrete_id = 0x1234abcd, integrated_id = 0x5678, software_id = 0x10005ULL << 32 | 0x77;
constexpr std::uint64_t search_parents = 1 | 2;   // kIORegistryIterateRecursively | kIORegistryIterateParents

void inject() {
    std::vector<alchemy::metal::GpuIdentity> gpus;
    alchemy::metal::GpuIdentity discrete;
    discrete.name = "Example Discrete GPU";
    discrete.vendor_id = 0x10de;
    discrete.device_id = 0x2684;
    discrete.registry_id = discrete_id;
    discrete.metal = true;
    gpus.push_back(discrete);
    alchemy::metal::GpuIdentity duplicate = discrete;   // the same adapter through another driver
    duplicate.name = "Example Discrete GPU (second driver)";
    duplicate.metal = false;
    gpus.push_back(duplicate);
    alchemy::metal::GpuIdentity integrated;
    integrated.name = "Example Integrated GPU";
    integrated.vendor_id = 0x8086;
    integrated.device_id = 0xa7a1;
    integrated.registry_id = integrated_id;
    integrated.integrated = true;
    gpus.push_back(integrated);
    alchemy::metal::GpuIdentity software;
    software.name = "llvmpipe";
    software.vendor_id = 0x10005;
    software.device_id = 0x77;
    software.registry_id = software_id;
    software.cpu = true;
    gpus.push_back(software);
    alchemy::metal::set_gpu_identities_for_tests(std::move(gpus));
}

Address key(Fixture& f, std::string_view name) { return f.cf.string(name); }

// The 4 bytes of a CFData registry number, little-endian.
std::optional<std::uint32_t> number(Fixture& f, Address data) {
    if (!data || f.call(cf_library, "_CFDataGetLength", {data}) != 4) return std::nullopt;
    const auto bytes = f.call(cf_library, "_CFDataGetBytePtr", {data});
    return f.engine.read32(bytes);
}

std::optional<std::uint32_t> search(Fixture& f, Address entry, std::string_view name) {
    const auto value = f.call(iokit, "_IORegistryEntrySearchCFProperty", {entry, f.text("IOService"), key(f, name), 0, search_parents});
    return number(f, value);
}

Address accelerator_for(Fixture& f, std::uint64_t registry_id) {
    return f.call(iokit, "_IOServiceGetMatchingService", {0, f.call(iokit, "_IORegistryEntryIDMatching", {registry_id})});
}

Address parent(Fixture& f, Address entry) {
    const auto out = f.block(4);
    require(f.status(iokit, "_IORegistryEntryGetParentEntry", {entry, f.text("IOService"), out}) == 0, "parent entry");
    return f.engine.read32(out);
}

std::vector<Address> matches(Fixture& f, std::string_view cls) {
    const auto iterator = f.block(4);
    require(f.status(iokit, "_IOServiceGetMatchingServices", {0, f.call(iokit, "_IOServiceMatching", {f.text(cls)}), iterator}) == 0,
            "IOServiceGetMatchingServices");
    std::vector<Address> found;
    while (const auto next = f.call(iokit, "_IOIteratorNext", {f.engine.read32(iterator)})) found.push_back(next);
    return found;
}

std::uint64_t entry_id(Fixture& f, Address entry) {
    const auto out = f.block(8);
    require(f.status(iokit, "_IORegistryEntryGetRegistryEntryID", {entry, out}) == 0, "entry id");
    return f.engine.read64(out);
}

void dawn_lookup(Fixture& f) {
    // Dawn: the accelerator by [device registryID], its parent's PCI ids.
    const auto accelerator = accelerator_for(f, discrete_id);
    require(accelerator != 0, "IORegistryEntryIDMatching finds the accelerator");
    require(entry_id(f, accelerator) == discrete_id, "the accelerator's entry ID is the registry ID");
    require(f.call(iokit, "_IOObjectConformsTo", {accelerator, f.text("IOGraphicsAccelerator2")}) == 1 &&
            f.call(iokit, "_IOObjectConformsTo", {accelerator, f.text("IOAccelerator")}) == 1, "accelerator classes");
    const auto device = parent(f, accelerator);
    require(device != 0 && f.call(iokit, "_IOObjectConformsTo", {device, f.text("IOPCIDevice")}) == 1, "parent is the IOPCIDevice");
    require(search(f, device, "vendor-id") == 0x10de && search(f, device, "device-id") == 0x2684, "PCI ids");
    require(search(f, device, "class-code") == 0x30000, "VGA display class code");
    // Vulkan reports no PCI revision, so there is no revision-id (ANGLE reads
    // it only if present; an invented 0 would read as a real revision).
    require(!f.call(iokit, "_IORegistryEntryCreateCFProperty", {device, key(f, "revision-id"), 0, 0}), "no revision-id");
    // Recursive parent search from the accelerator reaches the same data.
    require(search(f, accelerator, "vendor-id") == 0x10de, "vendor-id through the parents");
    const auto model = f.call(iokit, "_IORegistryEntryCreateCFProperty", {device, key(f, "model"), 0, 0});
    require(model && f.read_text(f.call(cf_library, "_CFDataGetBytePtr", {model})) == "Example Discrete GPU", "model is the Metal device's name");
    const auto name = f.block(128);
    require(f.status(iokit, "_IORegistryEntryGetName", {device, name}) == 0 && f.read_text(name) == "display", "PCI entry name");
    require(accelerator_for(f, 0x9999) == 0, "an unknown registry ID matches nothing");
}

void angle_scan(Fixture& f) {
    // ANGLE: every IOGraphicsAccelerator2, its parent's class code and ids.
    const auto accelerators = matches(f, "IOGraphicsAccelerator2");
    require(accelerators.size() == 3, "one accelerator per adapter (the duplicate collapses)");
    std::vector<std::uint64_t> ids;
    std::size_t vga = 0;
    for (const auto accelerator : accelerators) {
        ids.push_back(entry_id(f, accelerator));
        if (search(f, parent(f, accelerator), "class-code") == 0x30000) ++vga;
    }
    require(std::find(ids.begin(), ids.end(), integrated_id) != ids.end(), "integrated accelerator");
    require(vga == 2, "two PCI display devices");
    require(matches(f, "IOPCIDevice").size() == 2, "the software device is no IOPCIDevice");
    const auto software = accelerator_for(f, software_id);
    require(software && !search(f, parent(f, software), "class-code") && search(f, parent(f, software), "vendor-id") == 0x10005,
            "software device ids without a class code");
    // The duplicate kept the Metal device's identity.
    const auto model = f.call(iokit, "_IORegistryEntryCreateCFProperty", {parent(f, accelerator_for(f, discrete_id)), key(f, "model"), 0, 0});
    require(f.read_text(f.call(cf_library, "_CFDataGetBytePtr", {model})) == "Example Discrete GPU", "the Metal device's entry wins");
    // Existing lookups are unchanged.
    require(matches(f, "IOPlatformExpertDevice").size() == 1, "one platform expert");
}

} // namespace

int main() {
    inject();   // before the registry is built (on first use)
    return run("gpu registry", {{"dawn lookup", dawn_lookup}, {"angle scan", angle_scan}});
}
