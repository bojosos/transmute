#pragma once

// Dynamic Vulkan loader for Metal on Vulkan (owner: A5).
//
// vulkan-1.dll is loaded with LoadLibrary the first time a Metal device is
// requested; nothing links against it, so alchemy-fx starts (and runs
// headless Blender) on machines without a Vulkan driver. Entry points are
// resolved into one table per level (global, instance, device) through
// vkGetInstanceProcAddr / vkGetDeviceProcAddr. Only the Vulkan-Headers are a
// build dependency (VK_NO_PROTOTYPES).
//
// Threading: load() is thread-safe and idempotent; the tables are immutable
// after load()/load_instance()/load_device() return.

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <stdexcept>
#include <string>
#include <string_view>

namespace alchemy::metal {

// Instance-level entry points (all core 1.0-1.3 unless marked).
#define ALCHEMY_VK_INSTANCE_FUNCTIONS(X)                                                           \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)            \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures2)                              \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties)             \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceFormatProperties2)                 \
    X(vkGetPhysicalDeviceImageFormatProperties)                                                    \
    X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)

// Device-level entry points (core 1.3).
#define ALCHEMY_VK_DEVICE_FUNCTIONS(X)                                                             \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkQueueSubmit2) X(vkQueueWaitIdle) X(vkDeviceWaitIdle) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory)                            \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory)    \
    X(vkGetBufferDeviceAddress) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateBufferView)          \
    X(vkDestroyBufferView) X(vkCreateSampler) X(vkDestroySampler) X(vkCreateSemaphore)             \
    X(vkDestroySemaphore) X(vkGetSemaphoreCounterValue) X(vkWaitSemaphores) X(vkSignalSemaphore)   \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkResetCommandPool)                           \
    X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkBeginCommandBuffer)                    \
    X(vkEndCommandBuffer) X(vkCreateShaderModule) X(vkDestroyShaderModule)                         \
    X(vkCreatePipelineCache) X(vkDestroyPipelineCache) X(vkGetPipelineCacheData)                   \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout)       \
    X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines)            \
    X(vkDestroyPipeline) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool)                      \
    X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets)                 \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkGetQueryPoolResults)                            \
    X(vkCmdPipelineBarrier2) X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline)      \
    X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdBindVertexBuffers) X(vkCmdBindVertexBuffers2)                   \
    X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdDrawIndirect)                  \
    X(vkCmdDrawIndexedIndirect) X(vkCmdDispatch) X(vkCmdDispatchIndirect)                          \
    X(vkCmdSetViewportWithCount) X(vkCmdSetScissorWithCount) X(vkCmdSetCullMode) X(vkCmdSetViewport) X(vkCmdSetScissor)                 \
    X(vkCmdSetFrontFace) X(vkCmdSetPrimitiveTopology) X(vkCmdSetPrimitiveRestartEnable)            \
    X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp)               \
    X(vkCmdSetDepthBoundsTestEnable) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp)             \
    X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference)          \
    X(vkCmdSetDepthBias) X(vkCmdSetDepthBiasEnable) X(vkCmdSetBlendConstants)                      \
    X(vkCmdSetRasterizerDiscardEnable) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage)                \
    X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdFillBuffer)               \
    X(vkCmdUpdateBuffer) X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkCmdResetQueryPool)                \
    X(vkCmdCopyQueryPoolResults) X(vkCmdResolveImage) X(vkCmdWriteTimestamp2) X(vkResetQueryPool)

struct VulkanFunctions {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr{};
    PFN_vkCreateInstance vkCreateInstance{};
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties{};
    PFN_vkEnumerateInstanceLayerProperties vkEnumerateInstanceLayerProperties{};
    PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion{};
#define ALCHEMY_VK_MEMBER(name) PFN_##name name{};
    ALCHEMY_VK_INSTANCE_FUNCTIONS(ALCHEMY_VK_MEMBER)
    ALCHEMY_VK_DEVICE_FUNCTIONS(ALCHEMY_VK_MEMBER)
#undef ALCHEMY_VK_MEMBER
    // VK_EXT_debug_utils (optional; null when the instance lacks it).
    PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectNameEXT{};
    PFN_vkCmdBeginDebugUtilsLabelEXT vkCmdBeginDebugUtilsLabelEXT{};
    PFN_vkCmdEndDebugUtilsLabelEXT vkCmdEndDebugUtilsLabelEXT{};
    PFN_vkCmdInsertDebugUtilsLabelEXT vkCmdInsertDebugUtilsLabelEXT{};
    PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT{};
    PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT{};
    // VK_KHR/EXT_calibrated_timestamps (optional; null without the extension).
    PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR vkGetPhysicalDeviceCalibrateableTimeDomainsKHR{};
    PFN_vkGetCalibratedTimestampsKHR vkGetCalibratedTimestampsKHR{};
};

// Thrown for every Vulkan failure Metal cannot express as a documented
// failure value; the message names the call and the VkResult.
class VulkanError : public std::runtime_error {
public:
    VulkanError(std::string_view call, VkResult result);
    explicit VulkanError(const std::string& message) : std::runtime_error(message), result_(VK_ERROR_UNKNOWN) {}
    [[nodiscard]] VkResult result() const noexcept { return result_; }
private:
    VkResult result_;
};

[[nodiscard]] std::string_view result_name(VkResult result) noexcept;

inline void check(VkResult result, std::string_view call) {
    if (result < VK_SUCCESS) throw VulkanError(call, result);
}

// Loads vulkan-1.dll and the global entry points. Returns the global table,
// or throws VulkanError("...vulkan-1.dll...") when no loader is installed.
// The DLL stays loaded for the process lifetime.
[[nodiscard]] const VulkanFunctions& load_global();
// Fills the instance-level members of `table` (copied from the global table).
void load_instance(VulkanFunctions& table, VkInstance instance);
// Fills the device-level members of `table`; missing core entry points throw.
void load_device(VulkanFunctions& table, VkDevice device);

} // namespace alchemy::metal
