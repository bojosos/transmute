// Linux host backend (not written yet), area "gpu": every call fails with Error::unsupported
// (include/alchemy/host/host.hpp).
#include "alchemy/host/gpu.hpp"

namespace alchemy::host::gpu {

Result<void*> vulkan_get_instance_proc_addr() { return Error::not_found; }   // no loader
Result<void*> air_translator_symbol(const char*) { return Error::not_found; }
std::string_view vulkan_surface_extension() noexcept { return ""; }
std::int32_t create_vulkan_surface(void*, void*, std::uintptr_t, std::uint64_t& surface) {
    surface = 0;
    return -7;   // VK_ERROR_EXTENSION_NOT_PRESENT
}

} // namespace alchemy::host::gpu
