// Null host backend, area "camera": every call fails with Error::unsupported
// (include/alchemy/host/host.hpp).
#include "alchemy/host/camera.hpp"

namespace alchemy::host::camera {

Result<std::vector<Camera>> cameras() { return Error::unsupported; }
Access camera_access() { return Access::unknown; }
Result<Stream*> open(std::string_view, std::uint32_t, std::uint32_t, double, Deliver) { return Error::unsupported; }
void size(const Stream*, std::uint32_t& width, std::uint32_t& height) noexcept { width = height = 0; }
Status start(Stream*) { return Error::unsupported; }
void stop(Stream*) noexcept {}
void close(Stream*) noexcept {}

} // namespace alchemy::host::camera
