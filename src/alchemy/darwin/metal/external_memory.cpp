#include "alchemy/darwin/metal/interop.hpp"

// Host memory imported as Vulkan device memory (owner: g3, Chrome stage 4):
// VK_EXT_external_memory_host, for IOSurface pages (surface_textures.cpp)
// and for buffers over guest memory such as -[MTLDevice
// newBufferWithBytesNoCopy:length:options:deallocator:] (g4). Guest VA ==
// host VA in alchemy-fx, so a guest range is the host pointer itself.
namespace alchemy::metal {

VkDeviceSize host_import_alignment(const Device& device) {
    if (!device.has_device_extension(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) return 0;
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &host;
    device.vk.vkGetPhysicalDeviceProperties2(device.physical, &properties);
    return std::max<VkDeviceSize>(host.minImportedHostPointerAlignment, 1);
}

std::optional<HostImport> import_host_memory(const Device& device, void* pointer, VkDeviceSize size, std::uint32_t memory_type_bits,
                                             std::string* why, VkMemoryAllocateFlags flags) {
    const auto failed = [why](std::string reason) -> std::optional<HostImport> {
        if (why) *why = std::move(reason);
        return std::nullopt;
    };
    const auto alignment = host_import_alignment(device);
    if (!alignment) return failed("the device has no VK_EXT_external_memory_host");
    if (!pointer || !size || reinterpret_cast<std::uintptr_t>(pointer) % alignment || size % alignment)
        return failed("the range is not aligned to minImportedHostPointerAlignment (" + std::to_string(alignment) + ")");
    const auto get_properties = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
        device.vk.vkGetDeviceProcAddr(device.device, "vkGetMemoryHostPointerPropertiesEXT"));
    if (!get_properties) return failed("no vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT pointer_properties{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (get_properties(device.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, pointer, &pointer_properties) != VK_SUCCESS)
        return failed("vkGetMemoryHostPointerPropertiesEXT failed");
    // Host-coherent: the CPU reads and writes the range without flushes.
    std::uint32_t type = UINT32_MAX;
    const auto bits = memory_type_bits & pointer_properties.memoryTypeBits;
    for (std::uint32_t index = 0; index < device.memory_properties.memoryTypeCount; ++index)
        if ((bits & (1U << index)) && (device.memory_properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = index;
            break;
        }
    if (type == UINT32_MAX) return failed("no host-coherent memory type imports the range");
    VkMemoryAllocateFlagsInfo allocate_flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    allocate_flags.flags = flags;
    VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    import.pNext = flags ? &allocate_flags : nullptr;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    import.pHostPointer = pointer;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.pNext = &import;
    allocate.allocationSize = size;
    allocate.memoryTypeIndex = type;
    HostImport result;
    result.type = type;
    if (const auto status = device.vk.vkAllocateMemory(device.device, &allocate, nullptr, &result.memory); status != VK_SUCCESS)
        return failed("vkAllocateMemory importing the range failed (" + std::to_string(status) + ")");
    return result;
}

} // namespace alchemy::metal
