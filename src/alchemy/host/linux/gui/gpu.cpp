// Linux host backend, area "gpu": the Vulkan loader, X11 (XCB) window
// surfaces and the AIR translator library (include/alchemy/host/gpu.hpp).
// Only this file sees vulkan_xcb.h.
//
// - The loader is the system's libvulkan.so.1, from the dynamic linker's
//   own search (never a directory the guest controls). Its drivers are the
//   system's ICDs; on WSL that is Mesa's lavapipe (llvmpipe) unless a GPU
//   driver is installed.
// - A surface is VK_KHR_xcb_surface on the window server's connection
//   (gui/x11.hpp): native_handle() is the X window.
// - The AIR translator is libm2v_capi.so (the LGPL wrapper, built apart from
//   Alchemy and loaded only at run time): the file ALCHEMY_M2V_CAPI names,
//   else libm2v_capi.so next to the running executable.
// Without the Vulkan-Headers (cmake/darwin/metal.cmake) Metal has no device
// and nothing creates a surface; the loader lookup works either way.
#include "alchemy/host/gpu.hpp"
#include "alchemy/host/linux/gui/x11.hpp"

#include <dlfcn.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_XCB_KHR 1
#include <vulkan/vulkan.h>
#endif

namespace alchemy::host::gpu {

Result<void*> vulkan_get_instance_proc_addr() {
    static void* const module = [] {
        void* handle = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        return handle ? handle : dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    }();
    if (!module) return Error::not_found;
    void* const entry = dlsym(module, "vkGetInstanceProcAddr");
    if (!entry) return Error::unsupported;
    return entry;
}

Result<void*> air_translator_symbol(const char* name) {
    static void* const module = []() -> void* {
        std::string path;
        if (const char* named = std::getenv("ALCHEMY_M2V_CAPI"); named && *named) {
            path = named;
        } else {
            std::string executable(4096, '\0');
            const auto length = readlink("/proc/self/exe", executable.data(), executable.size());
            if (length <= 0 || static_cast<std::size_t>(length) >= executable.size()) return nullptr;
            executable.resize(static_cast<std::size_t>(length));
            path = executable.substr(0, executable.find_last_of('/') + 1) + "libm2v_capi.so";
        }
        // A path with a slash: dlopen loads that file, no search.
        return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    }();
    if (!module) return Error::not_found;
    void* const entry = dlsym(module, name);
    if (!entry) return Error::unsupported;
    return entry;
}

#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
std::string_view vulkan_surface_extension() noexcept { return VK_KHR_XCB_SURFACE_EXTENSION_NAME; }

std::int32_t create_vulkan_surface(void* instance, void* get_instance_proc_addr, std::uintptr_t window, std::uint64_t& surface) {
    surface = 0;
    const auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(get_instance_proc_addr);
    const auto create = get ? reinterpret_cast<PFN_vkCreateXcbSurfaceKHR>(get(static_cast<VkInstance>(instance), "vkCreateXcbSurfaceKHR"))
                            : nullptr;
    if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
    auto* connection = linux_gui::connection();
    if (!connection) return VK_ERROR_INITIALIZATION_FAILED;   // no window server, so no such window
    VkXcbSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR};
    info.connection = connection;
    info.window = static_cast<xcb_window_t>(window);
    VkSurfaceKHR created{};
    const auto result = create(static_cast<VkInstance>(instance), &info, nullptr, &created);
    surface = reinterpret_cast<std::uint64_t>(created);
    return result;
}
#else
std::string_view vulkan_surface_extension() noexcept { return "VK_KHR_xcb_surface"; }

std::int32_t create_vulkan_surface(void*, void*, std::uintptr_t, std::uint64_t& surface) {
    surface = 0;
    return -7;   // VK_ERROR_EXTENSION_NOT_PRESENT
}
#endif

} // namespace alchemy::host::gpu
