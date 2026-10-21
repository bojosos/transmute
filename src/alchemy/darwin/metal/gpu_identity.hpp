#pragma once

// The host's GPUs as macOS names them (owner: Chrome stage 4 g1). One entry
// per Vulkan physical device, with the identity MTLDevice reports for it:
// the IORegistry's IOPCIDevice / IOGraphicsAccelerator2 entries
// (darwin/iokit/iokit_registry.cpp) are built from this list, and the
// accelerator's registry entry ID is the device's MTLDevice.registryID, so
// IORegistryEntryIDMatching([device registryID]) finds it (Dawn's and
// ANGLE's PCI ID lookup).
//
// Without Vulkan headers at build time (metal_unavailable.cpp), or without a
// Vulkan loader or GPU on the host, the list is empty. No Vulkan types cross
// this header, so IOKit includes it without the Vulkan headers.

#include <cstdint>
#include <string>
#include <vector>

namespace alchemy::metal {

struct GpuIdentity {
    std::string name;                      // VkPhysicalDeviceProperties::deviceName (MTLDevice.name)
    std::uint32_t vendor_id{}, device_id{};   // PCI ids for PCI devices (Vulkan's vendorID/deviceID)
    std::uint64_t registry_id{};           // MTLDevice.registryID (the LUID, else vendor << 32 | device)
    bool integrated{}, cpu{};              // VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU / _CPU
    std::uint64_t device_local_bytes{};    // the largest DEVICE_LOCAL heap
    bool metal{};                          // the device MTLCreateSystemDefaultDevice selects
};

// Enumerated once per process (a short-lived VkInstance with no extensions)
// and cached. Never throws: a failure gives an empty list, reported once on
// stderr.
[[nodiscard]] std::vector<GpuIdentity> gpu_identities();

// Tests: replaces the list gpu_identities() returns (before its first call
// in the process, or the cached list is replaced for later calls).
void set_gpu_identities_for_tests(std::vector<GpuIdentity> identities);

} // namespace alchemy::metal
