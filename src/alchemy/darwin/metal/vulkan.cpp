#include "alchemy/darwin/metal/vulkan.hpp"

#include "alchemy/host/gpu.hpp"

#include <mutex>

// The host's Vulkan loader (host::gpu; vulkan-1.dll on win32). See vulkan.hpp.
namespace alchemy::metal {

std::string_view result_name(VkResult result) noexcept {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_INVALID_SHADER_NV: return "VK_ERROR_INVALID_SHADER_NV";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    default: return "VkResult";
    }
}

VulkanError::VulkanError(std::string_view call, VkResult result)
    : std::runtime_error("Alchemy Metal: " + std::string(call) + " failed with " + std::string(result_name(result)) +
                         " (" + std::to_string(static_cast<int>(result)) + ")"),
      result_(result) {}

namespace {

std::mutex load_mutex;
VulkanFunctions global_table;
bool global_loaded{};
std::string load_error;

template <class T>
T resolve_instance(const VulkanFunctions& table, VkInstance instance, const char* name) {
    return reinterpret_cast<T>(table.vkGetInstanceProcAddr(instance, name));
}

} // namespace

const VulkanFunctions& load_global() {
    std::lock_guard lock(load_mutex);
    if (global_loaded) return global_table;
    if (!load_error.empty()) throw VulkanError(load_error);
    // The system loader only: never one from the guest's directory.
    const auto entry = host::gpu::vulkan_get_instance_proc_addr();
    if (!entry) {
        load_error = entry.error() == host::Error::unsupported
            ? std::string("Alchemy Metal: the Vulkan loader lacks vkGetInstanceProcAddr")
            : "Alchemy Metal: no Vulkan loader (" + std::string(host::name(entry.error())) + "); Metal devices are unavailable";
        throw VulkanError(load_error);
    }
    global_table.vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(*entry);
    global_table.vkCreateInstance = resolve_instance<PFN_vkCreateInstance>(global_table, nullptr, "vkCreateInstance");
    global_table.vkEnumerateInstanceExtensionProperties = resolve_instance<PFN_vkEnumerateInstanceExtensionProperties>(
        global_table, nullptr, "vkEnumerateInstanceExtensionProperties");
    global_table.vkEnumerateInstanceLayerProperties = resolve_instance<PFN_vkEnumerateInstanceLayerProperties>(
        global_table, nullptr, "vkEnumerateInstanceLayerProperties");
    global_table.vkEnumerateInstanceVersion =
        resolve_instance<PFN_vkEnumerateInstanceVersion>(global_table, nullptr, "vkEnumerateInstanceVersion");
    if (!global_table.vkCreateInstance || !global_table.vkEnumerateInstanceExtensionProperties) {
        load_error = "Alchemy Metal: the Vulkan loader lacks global entry points";
        throw VulkanError(load_error);
    }
    global_loaded = true;
    return global_table;
}

void load_instance(VulkanFunctions& table, VkInstance instance) {
#define ALCHEMY_VK_LOAD(name)                                                                      \
    table.name = resolve_instance<PFN_##name>(table, instance, #name);                             \
    if (!table.name) throw VulkanError("Alchemy Metal: Vulkan instance lacks " #name);
    ALCHEMY_VK_INSTANCE_FUNCTIONS(ALCHEMY_VK_LOAD)
#undef ALCHEMY_VK_LOAD
    table.vkSetDebugUtilsObjectNameEXT =
        resolve_instance<PFN_vkSetDebugUtilsObjectNameEXT>(table, instance, "vkSetDebugUtilsObjectNameEXT");
    table.vkCmdBeginDebugUtilsLabelEXT =
        resolve_instance<PFN_vkCmdBeginDebugUtilsLabelEXT>(table, instance, "vkCmdBeginDebugUtilsLabelEXT");
    table.vkCmdEndDebugUtilsLabelEXT =
        resolve_instance<PFN_vkCmdEndDebugUtilsLabelEXT>(table, instance, "vkCmdEndDebugUtilsLabelEXT");
    table.vkCmdInsertDebugUtilsLabelEXT =
        resolve_instance<PFN_vkCmdInsertDebugUtilsLabelEXT>(table, instance, "vkCmdInsertDebugUtilsLabelEXT");
    table.vkCreateDebugUtilsMessengerEXT =
        resolve_instance<PFN_vkCreateDebugUtilsMessengerEXT>(table, instance, "vkCreateDebugUtilsMessengerEXT");
    table.vkDestroyDebugUtilsMessengerEXT =
        resolve_instance<PFN_vkDestroyDebugUtilsMessengerEXT>(table, instance, "vkDestroyDebugUtilsMessengerEXT");
    table.vkGetPhysicalDeviceCalibrateableTimeDomainsKHR = resolve_instance<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR>(
        table, instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR");
    if (!table.vkGetPhysicalDeviceCalibrateableTimeDomainsKHR)
        table.vkGetPhysicalDeviceCalibrateableTimeDomainsKHR = resolve_instance<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR>(
            table, instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT");
}

void load_device(VulkanFunctions& table, VkDevice device) {
#define ALCHEMY_VK_LOAD(name)                                                                      \
    table.name = reinterpret_cast<PFN_##name>(table.vkGetDeviceProcAddr(device, #name));           \
    if (!table.name) throw VulkanError("Alchemy Metal: Vulkan device lacks " #name);
    ALCHEMY_VK_DEVICE_FUNCTIONS(ALCHEMY_VK_LOAD)
#undef ALCHEMY_VK_LOAD
}

} // namespace alchemy::metal
