// Host memory imported into the Metal device (owner: g3, darwin/metal/
// external_memory.cpp): an aligned host range imported with
// VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT backs a buffer with a device address,
// GPU writes land in exactly the host bytes they target, and a misaligned
// range is refused with a reason. Exit code 77 (ctest SKIP) without a
// Vulkan 1.3 device or VK_EXT_external_memory_host.
#include "alchemy/darwin/metal/device.hpp"
#include "alchemy/darwin/metal/interop.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>

namespace {
namespace metal = alchemy::metal;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void imported_buffer(metal::Device& device, VkDeviceSize alignment) {
    constexpr VkDeviceSize size = 64 * 1024;
    const auto host_size = (size + alignment - 1) / alignment * alignment;
    auto* bytes = static_cast<std::uint8_t*>(::operator new(host_size, std::align_val_t(alignment)));
    std::memset(bytes, 0x11, host_size);
    VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    create.pNext = &external;
    create.size = host_size;
    create.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VkBuffer buffer{};
    require(device.vk.vkCreateBuffer(device.device, &create, nullptr, &buffer) == VK_SUCCESS, "vkCreateBuffer");
    VkMemoryRequirements requirements{};
    device.vk.vkGetBufferMemoryRequirements(device.device, buffer, &requirements);
    std::string why;
    const auto imported = metal::import_host_memory(device, bytes, host_size, requirements.memoryTypeBits, &why,
                                                    VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT);
    require(imported.has_value(), "import_host_memory with DEVICE_ADDRESS: " + why);
    require(device.vk.vkBindBufferMemory(device.device, buffer, imported->memory, 0) == VK_SUCCESS, "vkBindBufferMemory");
    VkBufferDeviceAddressInfo address_info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    address_info.buffer = buffer;
    require(device.vk.vkGetBufferDeviceAddress(device.device, &address_info) != 0, "the imported buffer has a device address");
    // The GPU writes 8 KiB at offset 4 KiB: the host sees exactly those bytes.
    device.immediate([&](VkCommandBuffer command) { device.vk.vkCmdFillBuffer(command, buffer, 4096, 8192, 0xabcdabcdU); });
    for (VkDeviceSize at = 0; at < host_size; ++at) {
        const bool written = at >= 4096 && at < 4096 + 8192;
        const std::uint8_t want = written ? (at % 2 ? 0xab : 0xcd) : 0x11;
        require(bytes[at] == want, "host byte " + std::to_string(at) + " is " + std::to_string(bytes[at]));
    }
    device.vk.vkDestroyBuffer(device.device, buffer, nullptr);
    device.vk.vkFreeMemory(device.device, imported->memory, nullptr);
    ::operator delete(bytes, std::align_val_t(alignment));
    std::cout << "imported buffer with a device address ok\n";
}

void misaligned(metal::Device& device, VkDeviceSize alignment) {
    auto* bytes = static_cast<std::uint8_t*>(::operator new(2 * alignment, std::align_val_t(alignment)));
    std::string why;
    require(!metal::import_host_memory(device, bytes + 16, alignment, ~0U, &why) && why.find("aligned") != std::string::npos,
            "a misaligned range is refused with a reason (" + why + ")");
    require(!metal::import_host_memory(device, bytes, alignment + 16, ~0U, &why), "a misaligned size is refused");
    ::operator delete(bytes, std::align_val_t(alignment));
    std::cout << "misaligned ranges refused ok\n";
}

} // namespace

int main() {
    std::shared_ptr<metal::Device> device;
    try {
        metal::DeviceOptions options;
        options.pipeline_cache = std::filesystem::path{};
        options.presentation = false;
        device = metal::Device::create(options);
    } catch (const std::exception& error) {
        std::cout << "SKIP: no Vulkan 1.3 device for Metal: " << error.what() << '\n';
        return 77;
    }
    const auto alignment = metal::host_import_alignment(*device);
    if (!alignment) {
        std::cout << "SKIP: the device has no VK_EXT_external_memory_host\n";
        return 77;
    }
    try {
        std::cout << "minImportedHostPointerAlignment " << alignment << '\n';
        imported_buffer(*device, alignment);
        misaligned(*device, alignment);
    } catch (const std::exception& error) {
        std::cerr << "external memory test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Alchemy Metal external memory tests passed\n";
    return 0;
}
