#include "alchemy/darwin/metal/device.hpp"
#include "alchemy/host/gpu.hpp"
#include "alchemy/host/process.hpp"

#include "alchemy/gui/install.hpp"
#include "alchemy/darwin/metal/formats.hpp"
#include "alchemy/msl/compiler.hpp"
#include "alchemy/perf.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

// Metal on Vulkan device creation, memory, submission and recycling. See
// device.hpp.
namespace alchemy::metal {
namespace {

constexpr VkDeviceSize block_size = 64ULL << 20;
constexpr VkDeviceSize dedicated_threshold = 16ULL << 20;
constexpr std::uint32_t descriptor_pool_sets = 256;

std::string environment(const char* name) {
#ifdef _MSC_VER
    char* buffer = nullptr;
    std::size_t length = 0;
    std::string value;
    if (_dupenv_s(&buffer, &length, name) == 0 && buffer) {
        value = buffer;
        std::free(buffer);
    }
    return value;
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}

bool environment_set(const char* name) {
    const auto value = environment(name);
    return !value.empty() && value != "0";
}

VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment) {
    return alignment <= 1 ? value : (value + alignment - 1) / alignment * alignment;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && data && data->pMessage)
        std::cerr << "[metal-vk] " << data->pMessage << '\n' << std::flush;
    return VK_FALSE;
}

std::vector<VkExtensionProperties> instance_extension_list(const VulkanFunctions& vk) {
    std::uint32_t count{};
    check(vk.vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr), "vkEnumerateInstanceExtensionProperties");
    std::vector<VkExtensionProperties> result(count);
    check(vk.vkEnumerateInstanceExtensionProperties(nullptr, &count, result.data()),
          "vkEnumerateInstanceExtensionProperties");
    result.resize(count);
    return result;
}

bool contains(const std::vector<VkExtensionProperties>& list, std::string_view name) {
    return std::any_of(list.begin(), list.end(), [&](const auto& item) { return name == item.extensionName; });
}

} // namespace

int physical_device_score(const VkPhysicalDeviceProperties& properties, std::size_t index, std::string_view selection) {
    if (properties.apiVersion < VK_API_VERSION_1_3) return -1;
    const std::string_view name = properties.deviceName;
    if (!selection.empty() && selection != std::to_string(index) && name.find(selection) == std::string_view::npos) return -1;
    switch (properties.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return 0;
    default: return 1;
    }
}

std::string metal_device_selection() { return environment("ALCHEMY_METAL_DEVICE"); }

std::string metal_device_name(const VkPhysicalDeviceProperties& properties) {
    if (auto name = environment("ALCHEMY_METAL_DEVICE_NAME"); !name.empty()) return name;
    return properties.deviceName;
}

std::uint64_t physical_device_registry_id(const VkPhysicalDeviceProperties& properties, const VkPhysicalDeviceIDProperties& id) {
    // Stable per adapter: the LUID when the driver has one, else vendor/device ids.
    std::uint64_t registry{};
    if (id.deviceLUIDValid) std::memcpy(&registry, id.deviceLUID, sizeof(registry));
    else registry = (static_cast<std::uint64_t>(properties.vendorID) << 32) | properties.deviceID;
    return registry ? registry : 1;
}

std::optional<std::uint64_t> Device::gpu_timestamp() const {
    if (!vk.vkGetCalibratedTimestampsKHR) return std::nullopt;
    VkCalibratedTimestampInfoKHR info{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR};
    info.timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
    std::uint64_t value{}, deviation{};
    if (vk.vkGetCalibratedTimestampsKHR(device, 1, &info, &value, &deviation) != VK_SUCCESS) return std::nullopt;
    return value;
}

bool DeviceProfile::supports_family(std::int64_t family) const {
    return std::find(families.begin(), families.end(), family) != families.end();
}

Device::Device(VulkanFunctions table) : vk(table) {}

std::shared_ptr<Device> Device::create(const DeviceOptions& options) {
    std::shared_ptr<Device> result(new Device(load_global()));
    result->initialize(options);
    return result;
}

void Device::initialize(const DeviceOptions& options) {
    auto& table = const_cast<VulkanFunctions&>(vk);
    std::uint32_t loader_version = VK_API_VERSION_1_0;
    if (vk.vkEnumerateInstanceVersion) check(vk.vkEnumerateInstanceVersion(&loader_version), "vkEnumerateInstanceVersion");
    if (loader_version < VK_API_VERSION_1_3)
        throw VulkanError("Alchemy Metal requires a Vulkan 1.3 loader (found " +
                          std::to_string(VK_API_VERSION_MAJOR(loader_version)) + "." +
                          std::to_string(VK_API_VERSION_MINOR(loader_version)) + ")");

    // ---- instance ------------------------------------------------------------
    const auto available = instance_extension_list(vk);
    std::vector<const char*> extensions;
    const auto want = [&](const char* name) {
        if (!contains(available, name)) return false;
        extensions.push_back(name);
        instance_extensions.emplace_back(name);
        return true;
    };
    if (options.presentation) {
        want(VK_KHR_SURFACE_EXTENSION_NAME);
        want(host::gpu::vulkan_surface_extension().data());   // the host's window surface
        want(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        want(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    }
    const bool validation = options.validation || environment_set("ALCHEMY_METAL_VALIDATION");
    const bool debug_utils = want(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    std::vector<const char*> layers;
    if (validation && vk.vkEnumerateInstanceLayerProperties) {
        std::uint32_t count{};
        check(vk.vkEnumerateInstanceLayerProperties(&count, nullptr), "vkEnumerateInstanceLayerProperties");
        std::vector<VkLayerProperties> list(count);
        check(vk.vkEnumerateInstanceLayerProperties(&count, list.data()), "vkEnumerateInstanceLayerProperties");
        for (const auto& layer : list)
            if (std::string_view(layer.layerName) == "VK_LAYER_KHRONOS_validation") layers.push_back("VK_LAYER_KHRONOS_validation");
        if (layers.empty()) std::cerr << "Alchemy Metal: validation requested but VK_LAYER_KHRONOS_validation is absent\n";
    }
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "Alchemy Metal";
    application.pEngineName = "Alchemy";
    application.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &application;
    instance_info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    instance_info.ppEnabledExtensionNames = extensions.data();
    instance_info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    instance_info.ppEnabledLayerNames = layers.data();
    check(vk.vkCreateInstance(&instance_info, nullptr, &instance), "vkCreateInstance");
    load_instance(table, instance);
    if (debug_utils && !layers.empty() && vk.vkCreateDebugUtilsMessengerEXT) {
        VkDebugUtilsMessengerCreateInfoEXT messenger{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger.pfnUserCallback = debug_callback;
        check(vk.vkCreateDebugUtilsMessengerEXT(instance, &messenger, nullptr, &messenger_), "vkCreateDebugUtilsMessengerEXT");
    }
    if (!debug_utils) {
        table.vkSetDebugUtilsObjectNameEXT = nullptr;
        table.vkCmdBeginDebugUtilsLabelEXT = nullptr;
        table.vkCmdEndDebugUtilsLabelEXT = nullptr;
        table.vkCmdInsertDebugUtilsLabelEXT = nullptr;
    }

    // ---- physical device -----------------------------------------------------
    std::uint32_t count{};
    check(vk.vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> devices(count);
    check(vk.vkEnumeratePhysicalDevices(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
    devices.resize(count);
    const auto selection = environment("ALCHEMY_METAL_DEVICE");   // index or name substring
    std::string rejected;
    int best_score = -1;
    for (std::size_t index = 0; index < devices.size(); ++index) {
        VkPhysicalDeviceProperties candidate{};
        vk.vkGetPhysicalDeviceProperties(devices[index], &candidate);
        const std::string name = candidate.deviceName;
        if (candidate.apiVersion < VK_API_VERSION_1_3)
            rejected += " " + name + " (Vulkan " + std::to_string(VK_API_VERSION_MAJOR(candidate.apiVersion)) + "." +
                        std::to_string(VK_API_VERSION_MINOR(candidate.apiVersion)) + ")";
        const auto score = physical_device_score(candidate, index, selection);
        if (score > best_score) { best_score = score; physical = devices[index]; properties = candidate; }
    }
    if (!physical)
        throw VulkanError("Alchemy Metal: no Vulkan 1.3 GPU" +
                          (selection.empty() ? std::string{} : " matching ALCHEMY_METAL_DEVICE=" + selection) +
                          (rejected.empty() ? std::string{} : "; rejected:" + rejected));
    api_version = properties.apiVersion;
    vk.vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);

    std::uint32_t family_count{};
    vk.vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vk.vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
    queue_family = UINT32_MAX;
    for (std::uint32_t index = 0; index < family_count; ++index) {
        const auto flags = families[index].queueFlags;
        if ((flags & VK_QUEUE_GRAPHICS_BIT) && (flags & VK_QUEUE_COMPUTE_BIT)) { queue_family = index; break; }
    }
    if (queue_family != UINT32_MAX) timestamp_valid_bits = families[queue_family].timestampValidBits;
    if (queue_family == UINT32_MAX) throw VulkanError("Alchemy Metal: " + std::string(properties.deviceName) +
                                                      " has no graphics+compute queue family");

    std::uint32_t extension_count{};
    check(vk.vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, nullptr),
          "vkEnumerateDeviceExtensionProperties");
    std::vector<VkExtensionProperties> device_available(extension_count);
    check(vk.vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, device_available.data()),
          "vkEnumerateDeviceExtensionProperties");
    device_available.resize(extension_count);
    std::vector<const char*> device_names;
    const auto want_device = [&](const char* name) {
        if (!contains(device_available, name)) return false;
        device_names.push_back(name);
        device_extensions.emplace_back(name);
        return true;
    };
    if (options.presentation && has_instance_extension(VK_KHR_SURFACE_EXTENSION_NAME)) want_device(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    const bool barycentric = want_device(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
    const bool list_restart = want_device(VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME);
    const bool local_read = want_device(VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
    // [[stencil]] fragment outputs (SPV_EXT_shader_stencil_export).
    want_device(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
    // g3: IOSurface-backed textures copy straight into the surfaces' pages
    // (surface_textures.cpp; without it they stage through host memory).
    want_device(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    // -[MTLDevice sampleTimestamps:gpuTimestamp:] in timestamp-query ticks.
    const bool calibrated_khr = want_device(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    const bool calibrated = calibrated_khr || want_device(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);

    // ---- features --------------------------------------------------------------
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR supported_bary{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
    VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT supported_restart{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT};
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR supported_local_read{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR};
    VkPhysicalDeviceVulkan13Features supported13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features supported12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features supported11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    supported.pNext = &supported11;
    supported11.pNext = &supported12;
    supported12.pNext = &supported13;
    void** tail = &supported13.pNext;
    if (barycentric) { *tail = &supported_bary; tail = &supported_bary.pNext; }
    if (list_restart) { *tail = &supported_restart; tail = &supported_restart.pNext; }
    if (local_read) { *tail = &supported_local_read; tail = &supported_local_read.pNext; }
    vk.vkGetPhysicalDeviceFeatures2(physical, &supported);

    std::string missing;
    const auto require = [&](VkBool32 value, const char* name) { if (!value) missing += std::string(" ") + name; };
    require(supported13.dynamicRendering, "dynamicRendering");
    require(supported13.synchronization2, "synchronization2");
    require(supported12.timelineSemaphore, "timelineSemaphore");
    require(supported12.bufferDeviceAddress, "bufferDeviceAddress");
    require(supported12.scalarBlockLayout, "scalarBlockLayout");
    require(supported.features.shaderInt64, "shaderInt64");
    if (!missing.empty())
        throw VulkanError("Alchemy Metal: " + std::string(properties.deviceName) + " lacks required Vulkan features:" + missing);

    VkPhysicalDeviceFeatures2 enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features enabled11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features enabled12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features enabled13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR enabled_bary{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
    VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT enabled_restart{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT};
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR enabled_local_read{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR};
#define ALCHEMY_ENABLE(target, source, field) target.field = source.field;
    // Core 1.0: everything Metal's feature set maps to (no geometry or
    // tessellation shaders, no wide lines, no float64, no robustness).
#define ALCHEMY_F10(X) X(multiViewport) X(imageCubeArray) X(independentBlend) X(dualSrcBlend) X(fillModeNonSolid) X(depthClamp)     \
    X(depthBiasClamp) X(largePoints) X(multiDrawIndirect) X(drawIndirectFirstInstance) X(samplerAnisotropy)       \
    X(textureCompressionBC) X(occlusionQueryPrecise) X(fragmentStoresAndAtomics)                                   \
    X(vertexPipelineStoresAndAtomics) X(shaderImageGatherExtended) X(shaderStorageImageExtendedFormats)           \
    X(shaderStorageImageReadWithoutFormat) X(shaderStorageImageWriteWithoutFormat)                                 \
    X(shaderUniformBufferArrayDynamicIndexing) X(shaderSampledImageArrayDynamicIndexing)                          \
    X(shaderStorageBufferArrayDynamicIndexing) X(shaderStorageImageArrayDynamicIndexing) X(shaderClipDistance)     \
    X(shaderCullDistance) X(shaderInt64) X(shaderInt16) X(sampleRateShading) X(shaderResourceMinLod)
#define ALCHEMY_F11(X) X(storageBuffer16BitAccess) X(uniformAndStorageBuffer16BitAccess) X(storagePushConstant16)   \
    X(storageInputOutput16) X(shaderDrawParameters) X(variablePointersStorageBuffer) X(variablePointers)
#define ALCHEMY_F12(X) X(samplerMirrorClampToEdge) X(drawIndirectCount) X(storageBuffer8BitAccess)                 \
    X(uniformAndStorageBuffer8BitAccess) X(storagePushConstant8) X(shaderBufferInt64Atomics)                     \
    X(shaderSharedInt64Atomics) X(shaderFloat16) X(shaderInt8) X(descriptorIndexing)                             \
    X(shaderSampledImageArrayNonUniformIndexing) X(shaderStorageImageArrayNonUniformIndexing)                    \
    X(shaderStorageBufferArrayNonUniformIndexing) X(shaderUniformTexelBufferArrayDynamicIndexing)                \
    X(shaderStorageTexelBufferArrayDynamicIndexing) X(shaderUniformTexelBufferArrayNonUniformIndexing)           \
    X(shaderStorageTexelBufferArrayNonUniformIndexing) X(descriptorBindingPartiallyBound)                        \
    X(descriptorBindingVariableDescriptorCount) X(runtimeDescriptorArray) X(samplerFilterMinmax)                 \
    X(scalarBlockLayout) X(uniformBufferStandardLayout) X(shaderSubgroupExtendedTypes)                           \
    X(separateDepthStencilLayouts) X(hostQueryReset) X(timelineSemaphore) X(bufferDeviceAddress)                 \
    X(vulkanMemoryModel) X(vulkanMemoryModelDeviceScope) X(shaderOutputViewportIndex) X(shaderOutputLayer)       \
    X(subgroupBroadcastDynamicId)
#define ALCHEMY_F13(X) X(pipelineCreationCacheControl) X(shaderDemoteToHelperInvocation)                           \
    X(shaderTerminateInvocation) X(subgroupSizeControl) X(computeFullSubgroups) X(synchronization2)               \
    X(shaderZeroInitializeWorkgroupMemory) X(dynamicRendering) X(shaderIntegerDotProduct) X(maintenance4)
#define ALCHEMY_E10(field) ALCHEMY_ENABLE(enabled.features, supported.features, field)
#define ALCHEMY_E11(field) ALCHEMY_ENABLE(enabled11, supported11, field)
#define ALCHEMY_E12(field) ALCHEMY_ENABLE(enabled12, supported12, field)
#define ALCHEMY_E13(field) ALCHEMY_ENABLE(enabled13, supported13, field)
    ALCHEMY_F10(ALCHEMY_E10)
    ALCHEMY_F11(ALCHEMY_E11)
    ALCHEMY_F12(ALCHEMY_E12)
    ALCHEMY_F13(ALCHEMY_E13)
#undef ALCHEMY_E10
#undef ALCHEMY_E11
#undef ALCHEMY_E12
#undef ALCHEMY_E13
#undef ALCHEMY_F10
#undef ALCHEMY_F11
#undef ALCHEMY_F12
#undef ALCHEMY_F13
#undef ALCHEMY_ENABLE
    enabled.pNext = &enabled11;
    enabled11.pNext = &enabled12;
    enabled12.pNext = &enabled13;
    tail = &enabled13.pNext;
    if (barycentric) {
        enabled_bary.fragmentShaderBarycentric = supported_bary.fragmentShaderBarycentric;
        *tail = &enabled_bary; tail = &enabled_bary.pNext;
    }
    if (list_restart) {
        enabled_restart.primitiveTopologyListRestart = supported_restart.primitiveTopologyListRestart;
        *tail = &enabled_restart; tail = &enabled_restart.pNext;
    }
    if (local_read && supported_local_read.dynamicRenderingLocalRead) {
        enabled_local_read.dynamicRenderingLocalRead = VK_TRUE;
        *tail = &enabled_local_read; tail = &enabled_local_read.pNext;
        dynamic_rendering_local_read = true;
    }
    features10 = enabled.features;
    features12 = enabled12;
    features12.pNext = nullptr;
    features13 = enabled13;
    features13.pNext = nullptr;

    const float priority = 1.0F;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &enabled;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = static_cast<std::uint32_t>(device_names.size());
    device_info.ppEnabledExtensionNames = device_names.data();
    check(vk.vkCreateDevice(physical, &device_info, nullptr, &device), "vkCreateDevice");
    load_device(table, device);
    vk.vkGetDeviceQueue(device, queue_family, 0, &queue);
    if (calibrated) {
        // The device's own time domain is the one sampleTimestamps: reads.
        table.vkGetCalibratedTimestampsKHR = reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(
            vk.vkGetDeviceProcAddr(device, calibrated_khr ? "vkGetCalibratedTimestampsKHR" : "vkGetCalibratedTimestampsEXT"));
        bool device_domain{};
        if (vk.vkGetPhysicalDeviceCalibrateableTimeDomainsKHR) {
            std::uint32_t count{};
            if (vk.vkGetPhysicalDeviceCalibrateableTimeDomainsKHR(physical, &count, nullptr) == VK_SUCCESS) {
                std::vector<VkTimeDomainKHR> domains(count);
                if (vk.vkGetPhysicalDeviceCalibrateableTimeDomainsKHR(physical, &count, domains.data()) == VK_SUCCESS)
                    device_domain = std::find(domains.begin(), domains.begin() + count, VK_TIME_DOMAIN_DEVICE_KHR) != domains.begin() + count;
            }
        }
        if (!device_domain) table.vkGetCalibratedTimestampsKHR = nullptr;
    }

    VkSemaphoreTypeCreateInfo timeline_type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timeline_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semaphore_info.pNext = &timeline_type;
    check(vk.vkCreateSemaphore(device, &semaphore_info, nullptr, &timeline), "vkCreateSemaphore");

    build_profile();
    profile_.barycentric_coordinates = barycentric && supported_bary.fragmentShaderBarycentric;
    // Diagnostics only (bring-up on GPUs without the extension): claim
    // barycentric support anyway. Shaders that read [[barycentric_coord]]
    // then fail to compile or link explicitly.
    if (environment_set("ALCHEMY_METAL_ASSUME_BARYCENTRICS")) profile_.barycentric_coordinates = true;

    // ---- pipeline cache ---------------------------------------------------------
    if (options.pipeline_cache) cache_path_ = *options.pipeline_cache;
    else if (const auto configured = environment("ALCHEMY_METAL_PIPELINE_CACHE"); !configured.empty()) {
        if (configured != "0") cache_path_ = configured;
    } else if (const auto local = environment("LOCALAPPDATA"); !local.empty()) {
        std::ostringstream name;
        name << "metal-pipeline-cache-" << std::hex << properties.vendorID << '-' << properties.deviceID << ".bin";
        cache_path_ = std::filesystem::path(local) / "Alchemy" / name.str();
    }
    std::vector<char> initial;
    if (!cache_path_.empty()) {
        std::ifstream input(cache_path_, std::ios::binary);
        if (input) initial.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    VkPipelineCacheCreateInfo cache_info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    // The header (vendor, device, UUID) is validated by the driver; a
    // mismatched or stale file is ignored by it, not trusted.
    cache_info.initialDataSize = initial.size();
    cache_info.pInitialData = initial.empty() ? nullptr : initial.data();
    if (vk.vkCreatePipelineCache(device, &cache_info, nullptr, &pipeline_cache) != VK_SUCCESS) {
        cache_info.initialDataSize = 0;
        cache_info.pInitialData = nullptr;
        check(vk.vkCreatePipelineCache(device, &cache_info, nullptr, &pipeline_cache), "vkCreatePipelineCache");
    }
    cache_saved_size_ = initial.size();
    if (perf::enabled()) {
        std::fprintf(stderr, "[alchemy perf] VkPipelineCache %s: loaded %zu bytes\n", cache_path_.string().c_str(), initial.size());
        perf::phase("Metal device created (Vulkan device, pipeline cache loaded)");
    }
}

void Device::build_profile() {
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceVulkan11Properties properties11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceMaintenance4Properties maintenance4{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES};
    VkPhysicalDeviceProperties2 all{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    all.pNext = &id;
    id.pNext = &properties11;
    properties11.pNext = &maintenance4;
    vk.vkGetPhysicalDeviceProperties2(physical, &all);
    const auto& limits = properties.limits;

    auto& p = profile_;
    // MTLDevice.name is the adapter's marketing name, as on Macs ("Intel(R)
    // Iris(R) Xe Graphics", "AMD Radeon Pro 5500M"). Clients classify the
    // vendor from it: Blender 4.5's MTLBackend::platform_init rates a name
    // containing "Intel" GPU_SUPPORT_LEVEL_LIMITED (its verdict on Intel Mac
    // GPUs; families, argument buffer tier and barycentrics do not enter
    // into it) and shows "Limited Platform Support ... <name> Metal API 1.2"
    // ("Metal API" and "1.2" are Blender's constants). Names with "AMD",
    // "ATI", "NVIDIA" or "Apple" are rated supported, anything else is
    // GPU_DEVICE_UNKNOWN. When supportsShaderBarycentricCoordinates is NO,
    // metal_is_supported accepts the device only through a whitelist: Mac2
    // family and a name with "AMD", "Apple"/"APPLE" or "Intel"/"INTEL". The
    // real name is reported either way, never one chosen for a rating.
    // ALCHEMY_METAL_DEVICE_NAME overrides it for diagnostics only.
    p.name = metal_device_name(properties);
    p.registry_id = physical_device_registry_id(properties, id);
    p.unified_memory = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ||
                       properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    p.low_power = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    VkDeviceSize device_local{};
    for (std::uint32_t heap = 0; heap < memory_properties.memoryHeapCount; ++heap)
        if (memory_properties.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            device_local = std::max(device_local, memory_properties.memoryHeaps[heap].size);
    // Metal recommends ~3/4 of the GPU's memory as the working set.
    p.recommended_working_set = device_local / 4 * 3;
    p.max_buffer_length = std::min<std::uint64_t>(properties11.maxMemoryAllocationSize, maintenance4.maxBufferSize);
    for (int axis = 0; axis < 3; ++axis) p.max_threads_per_threadgroup[axis] = limits.maxComputeWorkGroupSize[axis];
    p.max_total_threads_per_threadgroup = limits.maxComputeWorkGroupInvocations;
    p.max_threadgroup_memory = limits.maxComputeSharedMemorySize;
    p.thread_execution_width = properties11.subgroupSize ? properties11.subgroupSize : 32;
    // Tier 2 argument buffers: arbitrary buffer pointers (buffer device
    // address) plus descriptor indexing for resource arrays.
    const bool indexing = features12.runtimeDescriptorArray && features12.descriptorBindingPartiallyBound &&
                          features12.shaderSampledImageArrayNonUniformIndexing;
    p.argument_buffers_tier = features12.bufferDeviceAddress && indexing ? 1 : 0;
    // MTLReadWriteTextureTier2 is defined by a format list (Metal feature
    // set tables): read_write access to R32/RGBA32/RGBA16 float and integer,
    // RGBA8, R16 and R8 textures. Vulkan 1.3 backs it per format with
    // STORAGE_READ/WRITE_WITHOUT_FORMAT (shaders declare Unknown formats).
    // Tier 1 covers R32Float/R32Uint/R32Sint only.
    const auto read_write = [&](std::initializer_list<VkFormat> formats) {
        for (const auto format : formats) {
            VkFormatProperties3 properties3{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
            VkFormatProperties2 properties2{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
            properties2.pNext = &properties3;
            vk.vkGetPhysicalDeviceFormatProperties2(physical, format, &properties2);
            const auto needed = VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT |
                                VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
            if ((properties3.optimalTilingFeatures & needed) != needed) return false;
        }
        return true;
    };
    const bool tier1 = read_write({VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT});
    const bool tier2 = tier1 && read_write({VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT,
        VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_UINT,
        VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT,
        VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UINT,
        VK_FORMAT_R8_SINT});
    p.read_write_texture_tier = tier2 ? 2 : tier1 ? 1 : 0;
    p.bc_compression = features10.textureCompressionBC;
    p.float32_filtering = (format_features(VK_FORMAT_R32G32B32A32_SFLOAT) &
                           VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    p.float32_blending = (format_features(VK_FORMAT_R32G32B32A32_SFLOAT) &
                          VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0;
    p.depth24_stencil8 = (format_features(VK_FORMAT_D24_UNORM_S8_UINT) &
                          VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    p.shader_draw_parameters = true;
    p.texture_sample_counts = limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts &
                              limits.sampledImageColorSampleCounts;
    p.max_texture_2d = limits.maxImageDimension2D;
    p.max_texture_3d = limits.maxImageDimension3D;
    p.max_texture_cube = limits.maxImageDimensionCube;
    p.max_texture_layers = limits.maxImageArrayLayers;
    p.max_color_attachments = std::min(limits.maxColorAttachments, 8U);
    p.max_vertex_attributes = std::min(limits.maxVertexInputAttributes, 31U);
    p.max_vertex_bindings = std::min(limits.maxVertexInputBindings, 31U);
    p.min_uniform_offset_alignment = static_cast<std::uint32_t>(limits.minUniformBufferOffsetAlignment);
    p.timestamps = limits.timestampComputeAndGraphics && timestamp_valid_bits > 0 && limits.timestampPeriod > 0;
    p.timestamp_period = limits.timestampPeriod;
    p.min_texel_buffer_alignment = static_cast<std::uint32_t>(limits.minTexelBufferOffsetAlignment);

    // GPU families: the common families are the portable baseline. Mac1/Mac2
    // describe the AMD/Intel-class Mac feature sets (BC compression, cube
    // arrays, layered rendering, tier-2 argument buffers). The Apple families
    // imply tile memory, imageblocks, programmable blending and memoryless
    // targets, and Metal 3 implies mesh shading and function pointers: none
    // of those is translated, so they are not claimed.
    p.families = {gpu_family::common1, gpu_family::common2, gpu_family::common3};
    const bool mac1 = features10.textureCompressionBC && features10.imageCubeArray && features10.depthClamp &&
                      features12.shaderOutputLayer;
    if (mac1) p.families.push_back(gpu_family::mac1);
    if (mac1 && p.argument_buffers_tier == 1 && p.read_write_texture_tier == 2) p.families.push_back(gpu_family::mac2);
    if (const auto overridden = environment("ALCHEMY_METAL_FAMILIES"); !overridden.empty()) {
        // Diagnostics only: replaces the list ("2001,2002,1007").
        p.families.clear();
        std::stringstream list(overridden);
        for (std::string item; std::getline(list, item, ',');)
            if (!item.empty()) p.families.push_back(std::stoll(item));
    }
}

Device::~Device() {
    if (!device) {
        if (instance) vk.vkDestroyInstance(instance, nullptr);
        return;
    }
    vk.vkDeviceWaitIdle(device);
    {
        std::lock_guard lock(deferred_mutex_);
        for (auto& item : deferred_) item.destroy();
        deferred_.clear();
    }
    try { save_pipeline_cache(); } catch (const std::exception&) {}
    for (auto it = cleanups_.rbegin(); it != cleanups_.rend(); ++it) (*it)();
    cleanups_.clear();
    extensions_.clear();
    {
        std::lock_guard lock(deferred_mutex_);
        for (auto& item : deferred_) item.destroy();
        deferred_.clear();
    }
    for (auto& context : free_commands_) vk.vkDestroyCommandPool(device, context.pool, nullptr);
    for (const auto pool : free_descriptor_pools_) vk.vkDestroyDescriptorPool(device, pool, nullptr);
    if (pipeline_cache) vk.vkDestroyPipelineCache(device, pipeline_cache, nullptr);
    for (auto& block : blocks_) {
        if (block.mapped) vk.vkUnmapMemory(device, block.memory);
        vk.vkFreeMemory(device, block.memory, nullptr);
    }
    if (timeline) vk.vkDestroySemaphore(device, timeline, nullptr);
    vk.vkDestroyDevice(device, nullptr);
    if (messenger_ && vk.vkDestroyDebugUtilsMessengerEXT) vk.vkDestroyDebugUtilsMessengerEXT(instance, messenger_, nullptr);
    vk.vkDestroyInstance(instance, nullptr);
}

bool Device::has_device_extension(std::string_view name) const {
    return std::find(device_extensions.begin(), device_extensions.end(), name) != device_extensions.end();
}
bool Device::has_instance_extension(std::string_view name) const {
    return std::find(instance_extensions.begin(), instance_extensions.end(), name) != instance_extensions.end();
}

VkFormatFeatureFlags Device::format_features(VkFormat format, bool optimal) const {
    VkFormatProperties value{};
    vk.vkGetPhysicalDeviceFormatProperties(physical, format, &value);
    return optimal ? value.optimalTilingFeatures : value.linearTilingFeatures;
}

VkFormat Device::texture_format(std::uint32_t metal) const {
    const auto* info = pixel_format_info(metal);
    if (!info) return VK_FORMAT_UNDEFINED;
    const auto usable = [&](VkFormat format) {
        return (format_features(format) & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    };
    switch (info->vulkan) {
    case VK_FORMAT_D24_UNORM_S8_UINT:
        return usable(VK_FORMAT_D24_UNORM_S8_UINT) ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
    case VK_FORMAT_S8_UINT:
        if (usable(VK_FORMAT_S8_UINT)) return VK_FORMAT_S8_UINT;
        return usable(VK_FORMAT_D24_UNORM_S8_UINT) ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
    default: return info->vulkan;
    }
}

// ---- memory ---------------------------------------------------------------------

std::uint32_t Device::memory_type(std::uint32_t bits, MemoryKind kind) const {
    const auto pick = [&](VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred, bool large_heap) {
        for (std::uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index) {
            if (!(bits & (1U << index))) continue;
            const auto flags = memory_properties.memoryTypes[index].propertyFlags;
            if ((flags & (required | preferred)) != (required | preferred)) continue;
            const auto heap = memory_properties.memoryHeaps[memory_properties.memoryTypes[index].heapIndex];
            if (large_heap && heap.size <= (256ULL << 20)) continue;   // skip small BAR windows
            return index;
        }
        return UINT32_MAX;
    };
    std::uint32_t found = UINT32_MAX;
    const auto host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    switch (kind) {
    case MemoryKind::device_local:
        found = pick(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false);
        if (found == UINT32_MAX) found = pick(0, 0, false);
        break;
    case MemoryKind::host_visible:
        found = pick(host, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
        if (found == UINT32_MAX) found = pick(host, 0, false);
        break;
    case MemoryKind::host_cached:
        found = pick(host, VK_MEMORY_PROPERTY_HOST_CACHED_BIT, false);
        if (found == UINT32_MAX) found = pick(host, 0, false);
        break;
    }
    if (found == UINT32_MAX) throw VulkanError("Alchemy Metal: no Vulkan memory type for the requested storage mode");
    return found;
}

Allocation Device::allocate(const VkMemoryRequirements& requirements, MemoryKind kind, bool image) {
    const auto type = memory_type(requirements.memoryTypeBits, kind);
    const bool host = (memory_properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    const auto allocate_memory = [&](VkDeviceSize size, VkDeviceMemory& memory, std::uint8_t*& mapped) {
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        info.pNext = image ? nullptr : &flags;
        info.allocationSize = size;
        info.memoryTypeIndex = type;
        check(vk.vkAllocateMemory(device, &info, nullptr, &memory), "vkAllocateMemory");
        mapped = nullptr;
        if (host) {
            void* pointer{};
            const auto result = vk.vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &pointer);
            if (result != VK_SUCCESS) { vk.vkFreeMemory(device, memory, nullptr); check(result, "vkMapMemory"); }
            mapped = static_cast<std::uint8_t*>(pointer);
        }
    };
    Allocation result;
    result.type = type;
    result.image = image;
    result.size = requirements.size;
    if (requirements.size >= dedicated_threshold) {
        allocate_memory(requirements.size, result.memory, result.mapped);
        allocated_ += requirements.size;
        return result;
    }
    std::lock_guard lock(memory_mutex_);
    const auto try_block = [&](std::uint32_t index) {
        auto& block = blocks_[index];
        for (auto it = block.free.begin(); it != block.free.end(); ++it) {
            const auto [offset, size] = *it;
            const auto aligned = align_up(offset, requirements.alignment);
            if (aligned + requirements.size > offset + size) continue;
            block.free.erase(it);
            if (aligned > offset) block.free.emplace(offset, aligned - offset);
            const auto end = aligned + requirements.size;
            if (end < offset + size) block.free.emplace(end, offset + size - end);
            result.memory = block.memory;
            result.offset = aligned;
            result.block = index;
            result.mapped = block.mapped ? block.mapped + aligned : nullptr;
            return true;
        }
        return false;
    };
    for (std::uint32_t index = 0; index < blocks_.size(); ++index)
        if (blocks_[index].type == type && blocks_[index].image == image && blocks_[index].memory && try_block(index)) {
            allocated_ += requirements.size;
            return result;
        }
    Block block;
    block.type = type;
    block.image = image;
    block.size = block_size;
    allocate_memory(block_size, block.memory, block.mapped);
    block.free.emplace(0, block_size);
    blocks_.push_back(std::move(block));
    if (!try_block(static_cast<std::uint32_t>(blocks_.size() - 1)))
        throw VulkanError("Alchemy Metal: allocation does not fit a fresh memory block");
    allocated_ += requirements.size;
    return result;
}

void Device::free(Allocation& allocation) {
    if (!allocation.memory) return;
    allocated_ -= allocation.size;
    if (allocation.block == UINT32_MAX) {
        if (allocation.mapped) vk.vkUnmapMemory(device, allocation.memory);
        vk.vkFreeMemory(device, allocation.memory, nullptr);
        allocation = {};
        return;
    }
    std::lock_guard lock(memory_mutex_);
    auto& free = blocks_.at(allocation.block).free;
    auto offset = allocation.offset;
    auto size = allocation.size;
    // Coalesce with the neighbours.
    if (auto next = free.find(offset + size); next != free.end()) { size += next->second; free.erase(next); }
    if (auto it = free.lower_bound(offset); it != free.begin()) {
        auto previous = std::prev(it);
        if (previous->first + previous->second == offset) {
            offset = previous->first;
            size += previous->second;
            free.erase(previous);
        }
    }
    free.emplace(offset, size);
    allocation = {};
}

// ---- submission -----------------------------------------------------------------

std::uint64_t Device::completed_value() const {
    std::uint64_t value{};
    check(vk.vkGetSemaphoreCounterValue(device, timeline, &value), "vkGetSemaphoreCounterValue");
    return value;
}

bool Device::wait(std::uint64_t value, std::uint64_t timeout_ns) const {
    perf::Scope timed(perf::mtl_cpu_wait_ticks, perf::mtl_cpu_waits);
    VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    info.semaphoreCount = 1;
    info.pSemaphores = &timeline;
    info.pValues = &value;
    const auto result = vk.vkWaitSemaphores(device, &info, timeout_ns);
    if (result == VK_TIMEOUT) return false;
    check(result, "vkWaitSemaphores");
    return true;
}

void Device::queue_initial_layout(VkImage image, VkImageAspectFlags aspects) {
    std::lock_guard lock(layout_mutex_);
    initial_layouts_[image] = aspects;
}

void Device::cancel_initial_layout(VkImage image) {
    std::lock_guard lock(layout_mutex_);
    initial_layouts_.erase(image);
}

std::uint64_t Device::submit(std::vector<Batch>& batches) {
    if (batches.empty()) batches.emplace_back();
    std::uint64_t value{};
    std::optional<CommandContext> initial;
    // Held until the submission is recorded in submitted_, so a texture
    // destroyed concurrently either leaves the pending list first or defers
    // its destruction past this submission (cancel_initial_layout waits).
    std::unique_lock layout_lock(layout_mutex_);
    {
        std::map<VkImage, VkImageAspectFlags> images;
        images.swap(initial_layouts_);
        if (!images.empty()) {
            initial = acquire_commands();
            VkCommandBuffer buffer{};
            if (initial->buffers.empty()) {
                VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                allocate.commandPool = initial->pool;
                allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocate.commandBufferCount = 1;
                check(vk.vkAllocateCommandBuffers(device, &allocate, &buffer), "vkAllocateCommandBuffers");
                initial->buffers.push_back(buffer);
            }
            buffer = initial->buffers[0];
            initial->used = 1;
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(vk.vkBeginCommandBuffer(buffer, &begin), "vkBeginCommandBuffer");
            std::vector<VkImageMemoryBarrier2> barriers;
            for (const auto& [image, aspects] : images) {
                VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = image;
                barrier.subresourceRange = {aspects, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
                barriers.push_back(barrier);
            }
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
            dependency.pImageMemoryBarriers = barriers.data();
            vk.vkCmdPipelineBarrier2(buffer, &dependency);
            check(vk.vkEndCommandBuffer(buffer), "vkEndCommandBuffer");
            batches.front().command_buffers.insert(batches.front().command_buffers.begin(), buffer);
        }
    }
    {
        std::lock_guard lock(queue_mutex_);
        value = submitted_.load() + 1;
        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signal.semaphore = timeline;
        signal.value = value;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        batches.back().signals.push_back(signal);
        std::vector<std::vector<VkCommandBufferSubmitInfo>> buffers(batches.size());
        std::vector<VkSubmitInfo2> infos(batches.size());
        for (std::size_t index = 0; index < batches.size(); ++index) {
            for (const auto buffer : batches[index].command_buffers) {
                VkCommandBufferSubmitInfo item{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
                item.commandBuffer = buffer;
                buffers[index].push_back(item);
            }
            auto& info = infos[index];
            info = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            info.commandBufferInfoCount = static_cast<std::uint32_t>(buffers[index].size());
            info.pCommandBufferInfos = buffers[index].data();
            info.waitSemaphoreInfoCount = static_cast<std::uint32_t>(batches[index].waits.size());
            info.pWaitSemaphoreInfos = batches[index].waits.data();
            info.signalSemaphoreInfoCount = static_cast<std::uint32_t>(batches[index].signals.size());
            info.pSignalSemaphoreInfos = batches[index].signals.data();
        }
        check(vk.vkQueueSubmit2(queue, static_cast<std::uint32_t>(infos.size()), infos.data(), VK_NULL_HANDLE),
              "vkQueueSubmit2");
        perf::add(perf::mtl_queue_submits);
        submitted_.store(value);
    }
    layout_lock.unlock();
    if (auto dirty = cache_dirty_ms_.load(std::memory_order_relaxed); dirty) {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now - dirty >= 2000 && cache_dirty_ms_.compare_exchange_strong(dirty, 0)) {
            try { save_pipeline_cache(); } catch (const std::exception&) {}
        }
    }
    if (initial) {
        auto shared = std::make_shared<CommandContext>(std::move(*initial));
        defer([this, shared] { recycle_commands(std::move(*shared)); });
    }
    collect();
    return value;
}

void Device::immediate(const std::function<void(VkCommandBuffer)>& record) {
    auto context = acquire_commands();
    VkCommandBuffer buffer{};
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = context.pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    if (context.buffers.empty()) {
        check(vk.vkAllocateCommandBuffers(device, &allocate, &buffer), "vkAllocateCommandBuffers");
        context.buffers.push_back(buffer);
    }
    buffer = context.buffers[0];
    context.used = 1;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vk.vkBeginCommandBuffer(buffer, &begin), "vkBeginCommandBuffer");
    // Everything submitted earlier is visible to this work.
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vk.vkCmdPipelineBarrier2(buffer, &dependency);
    record(buffer);
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    vk.vkCmdPipelineBarrier2(buffer, &dependency);
    check(vk.vkEndCommandBuffer(buffer), "vkEndCommandBuffer");
    std::vector<Batch> batches(1);
    batches[0].command_buffers.push_back(buffer);
    const auto value = submit(batches);
    wait(value, UINT64_MAX);
    recycle_commands(std::move(context));
    collect();
}

void Device::defer(std::function<void()> destroy) {
    const auto value = submitted_.load();
    if (completed_value() >= value) { destroy(); return; }
    std::lock_guard lock(deferred_mutex_);
    deferred_.push_back({value, std::move(destroy)});
}

void Device::collect() {
    std::vector<std::function<void()>> due;
    {
        std::lock_guard lock(deferred_mutex_);
        if (deferred_.empty()) return;
        const auto completed = completed_value();
        for (auto it = deferred_.begin(); it != deferred_.end();) {
            if (it->value <= completed) { due.push_back(std::move(it->destroy)); it = deferred_.erase(it); }
            else ++it;
        }
    }
    for (auto& destroy : due) destroy();
}

// ---- recycling ------------------------------------------------------------------

CommandContext Device::acquire_commands() {
    {
        std::lock_guard lock(recycle_mutex_);
        if (!free_commands_.empty()) {
            auto context = std::move(free_commands_.back());
            free_commands_.pop_back();
            return context;
        }
    }
    CommandContext context;
    VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    info.queueFamilyIndex = queue_family;
    check(vk.vkCreateCommandPool(device, &info, nullptr, &context.pool), "vkCreateCommandPool");
    return context;
}

void Device::recycle_commands(CommandContext context) {
    check(vk.vkResetCommandPool(device, context.pool, 0), "vkResetCommandPool");
    context.used = 0;
    std::lock_guard lock(recycle_mutex_);
    free_commands_.push_back(std::move(context));
}

VkDescriptorPool Device::acquire_descriptor_pool() {
    {
        std::lock_guard lock(recycle_mutex_);
        if (!free_descriptor_pools_.empty()) {
            const auto pool = free_descriptor_pools_.back();
            free_descriptor_pools_.pop_back();
            return pool;
        }
    }
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, descriptor_pool_sets},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, descriptor_pool_sets * 16},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, descriptor_pool_sets * 4},
        {VK_DESCRIPTOR_TYPE_SAMPLER, descriptor_pool_sets * 16},
        {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, descriptor_pool_sets * 2},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, descriptor_pool_sets * 4},
        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, descriptor_pool_sets * 4},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, descriptor_pool_sets}};
    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    info.maxSets = descriptor_pool_sets;
    info.poolSizeCount = static_cast<std::uint32_t>(std::size(sizes));
    info.pPoolSizes = sizes;
    VkDescriptorPool pool{};
    check(vk.vkCreateDescriptorPool(device, &info, nullptr, &pool), "vkCreateDescriptorPool");
    return pool;
}

void Device::recycle_descriptor_pool(VkDescriptorPool pool) {
    check(vk.vkResetDescriptorPool(device, pool, 0), "vkResetDescriptorPool");
    std::lock_guard lock(recycle_mutex_);
    free_descriptor_pools_.push_back(pool);
}

// ---- pipeline cache -------------------------------------------------------------

void Device::note_pipeline_cache_dirty() noexcept {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    cache_dirty_ms_.store(std::max<std::int64_t>(now, 1), std::memory_order_relaxed);
}

void Device::save_pipeline_cache() {
    if (cache_path_.empty() || !pipeline_cache) return;
    std::size_t size{};
    check(vk.vkGetPipelineCacheData(device, pipeline_cache, &size, nullptr), "vkGetPipelineCacheData");
    if (size == cache_saved_size_) return;
    std::vector<char> data(size);
    check(vk.vkGetPipelineCacheData(device, pipeline_cache, &size, data.data()), "vkGetPipelineCacheData");
    data.resize(size);
    std::error_code error;
    std::filesystem::create_directories(cache_path_.parent_path(), error);
    const auto temporary = cache_path_.string() + ".tmp" +
                           std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return;   // an unwritable cache directory only costs warm starts
        output.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!output) return;
    }
    std::filesystem::rename(temporary, cache_path_, error);
    if (error) std::filesystem::remove(temporary, error);
    else {
        cache_saved_size_ = size;
        if (perf::enabled()) std::fprintf(stderr, "[alchemy perf] VkPipelineCache saved: %zu bytes\n", size);
    }
}

void Device::set_name(VkObjectType type, std::uint64_t handle, const std::string& name) const {
    if (!vk.vkSetDebugUtilsObjectNameEXT || name.empty() || !handle) return;
    VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name.c_str();
    vk.vkSetDebugUtilsObjectNameEXT(device, &info);
}

// ---- the system device ------------------------------------------------------------

namespace {
std::mutex system_mutex;
std::shared_ptr<Device> system_instance;
std::string system_error;
} // namespace

std::shared_ptr<Device> system_device() {
    std::lock_guard lock(system_mutex);
    if (system_instance) return system_instance;
    if (!system_error.empty()) throw VulkanError(system_error);
    try {
        // ALCHEMY_PERF: how long the first MTLCreateSystemDefaultDevice /
        // MTLCopyAllDevices takes in each process (instance, device,
        // pipeline cache), and which process paid it.
        const auto start = std::chrono::steady_clock::now();
        system_instance = Device::create();
        if (perf::enabled())
            std::fprintf(stderr, "[alchemy perf] Metal device created in %.1f ms (host pid %u)\n",
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
                         host::process::current_pid());
    } catch (const std::exception& error) {
        system_error = error.what();
        throw;
    }
    return system_instance;
}

std::string system_device_error() {
    std::lock_guard lock(system_mutex);
    return system_error;
}

} // namespace alchemy::metal

namespace alchemy::gui {

std::shared_ptr<void> prefetch_metal_device() {
    if (metal::environment("ALCHEMY_METAL_PREFETCH") == "0") return nullptr;
    struct Threads {
        std::thread device, msl;
        ~Threads() {
            if (device.joinable()) device.join();
            if (msl.joinable()) msl.join();
        }
    };
    auto threads = std::make_shared<Threads>();
    // A failure is recorded by system_device() and reported when the guest
    // asks for the device, exactly as without the prefetch.
    threads->device = std::thread([] {
        try { static_cast<void>(metal::system_device()); } catch (...) {}
    });
    // The MSL result cache: one file per library, each open costing ~0.5-1 ms
    // here; read them all off the critical path.
    threads->msl = std::thread([] {
        try {
            const auto loaded = msl::preload_cache();
            if (perf::enabled()) std::fprintf(stderr, "[alchemy perf] MSL result cache preloaded: %zu results\n", loaded);
        } catch (...) {}
    });
    return threads;
}

} // namespace alchemy::gui
