// Win32 host backend, area "gpu": the Vulkan loader and Win32 window surfaces
// (include/alchemy/host/gpu.hpp). Only this file sees vulkan_win32.h.
// Without the Vulkan-Headers (cmake/darwin/metal.cmake) Metal has no device and
// nothing creates a surface; the loader lookup works either way.
#include "alchemy/host/gpu.hpp"
#include "alchemy/host/win32/win32.hpp"

#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR 1
#include <vulkan/vulkan.h>
#endif

namespace alchemy::host::gpu {

Result<void*> vulkan_get_instance_proc_addr() {
    // System32 only: never pick up a vulkan-1.dll from the guest's directory.
    static const auto module = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return Error::not_found;
    const auto entry = reinterpret_cast<void*>(GetProcAddress(module, "vkGetInstanceProcAddr"));
    if (!entry) return Error::unsupported;
    return entry;
}

#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
std::string_view vulkan_surface_extension() noexcept { return VK_KHR_WIN32_SURFACE_EXTENSION_NAME; }

std::int32_t create_vulkan_surface(void* instance, void* get_instance_proc_addr, std::uintptr_t window, std::uint64_t& surface) {
    const auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(get_instance_proc_addr);
    const auto create = get ? reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(get(static_cast<VkInstance>(instance), "vkCreateWin32SurfaceKHR"))
                            : nullptr;
    if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    info.hinstance = GetModuleHandleW(nullptr);
    info.hwnd = reinterpret_cast<HWND>(window);
    VkSurfaceKHR created{};
    const auto result = create(static_cast<VkInstance>(instance), &info, nullptr, &created);
    surface = reinterpret_cast<std::uint64_t>(created);
    return result;
}
#else
std::string_view vulkan_surface_extension() noexcept { return "VK_KHR_win32_surface"; }

std::int32_t create_vulkan_surface(void*, void*, std::uintptr_t, std::uint64_t& surface) {
    surface = 0;
    return -7;   // VK_ERROR_EXTENSION_NOT_PRESENT
}
#endif

} // namespace alchemy::host::gpu
