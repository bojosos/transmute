#include "alchemy/darwin/metal/gpu_identity.hpp"

#include "alchemy/darwin/metal/device.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <mutex>
#include <optional>

// The host's GPUs (gpu_identity.hpp): a VkInstance without extensions or
// layers, its physical devices described the way Device::create sees them
// (the same selection rule, name and registry ID), then destroyed.
namespace alchemy::metal {
namespace {

std::mutex identities_mutex;
std::optional<std::vector<GpuIdentity>> identities;

std::vector<GpuIdentity> enumerate() {
    VulkanFunctions vk = load_global();
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "Alchemy GPU identity";
    application.pEngineName = "Alchemy";
    application.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &application;
    VkInstance instance{};
    check(vk.vkCreateInstance(&info, nullptr, &instance), "vkCreateInstance");
    std::vector<GpuIdentity> result;
    try {
        load_instance(vk, instance);
        std::uint32_t count{};
        check(vk.vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
        std::vector<VkPhysicalDevice> devices(count);
        check(vk.vkEnumeratePhysicalDevices(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
        devices.resize(count);
        const auto selection = metal_device_selection();
        int best_score = -1;
        std::optional<std::size_t> best;
        VkPhysicalDeviceProperties chosen{};
        for (std::size_t index = 0; index < devices.size(); ++index) {
            VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 all{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            all.pNext = &id;
            vk.vkGetPhysicalDeviceProperties2(devices[index], &all);
            const auto& properties = all.properties;
            VkPhysicalDeviceMemoryProperties memory{};
            vk.vkGetPhysicalDeviceMemoryProperties(devices[index], &memory);
            GpuIdentity identity;
            identity.vendor_id = properties.vendorID;
            identity.device_id = properties.deviceID;
            identity.integrated = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
            identity.cpu = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
            for (std::uint32_t heap = 0; heap < memory.memoryHeapCount; ++heap)
                if (memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    identity.device_local_bytes = std::max<std::uint64_t>(identity.device_local_bytes, memory.memoryHeaps[heap].size);
            const auto score = physical_device_score(properties, index, selection);
            if (score > best_score) { best_score = score; best = index; }
            identity.name = properties.deviceName;
            identity.registry_id = physical_device_registry_id(properties, id);
            result.push_back(std::move(identity));
            if (best == index) chosen = properties;
        }
        // MTLDevice.name, its ALCHEMY_METAL_DEVICE_NAME override included,
        // names the chosen device only.
        if (best) {
            result[*best].metal = true;
            result[*best].name = metal_device_name(chosen);
        }
    } catch (...) {
        vk.vkDestroyInstance(instance, nullptr);
        throw;
    }
    vk.vkDestroyInstance(instance, nullptr);
    return result;
}

} // namespace

std::vector<GpuIdentity> gpu_identities() {
    std::lock_guard lock(identities_mutex);
    if (!identities) {
        try {
            identities = enumerate();
        } catch (const std::exception& failure) {
            std::fprintf(stderr, "[alchemy metal] no GPU identities (the IORegistry lists no GPU): %s\n", failure.what());
            identities.emplace();
        }
    }
    return *identities;
}

void set_gpu_identities_for_tests(std::vector<GpuIdentity> replacement) {
    std::lock_guard lock(identities_mutex);
    identities = std::move(replacement);
}

} // namespace alchemy::metal
