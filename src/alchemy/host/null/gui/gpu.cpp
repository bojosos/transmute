// Null host backend, area "gpu": every call fails with Error::unsupported
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
Result<Pixels> decode_image(std::span<const std::uint8_t>) { return Error::unsupported; }
Result<std::vector<std::uint8_t>> encode_image(const Pixels&, ImageFormat) { return Error::unsupported; }
Result<std::size_t> image_frame_count(std::span<const std::uint8_t>) { return Error::unsupported; }
Result<ImageFrame> image_frame(std::span<const std::uint8_t>, std::size_t, bool) { return Error::unsupported; }
Result<std::vector<std::uint8_t>> encode_image_frames(ImageFormat, std::span<const Pixels>, double) { return Error::unsupported; }

} // namespace alchemy::host::gpu
