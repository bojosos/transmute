// Linux host backend (not written yet), area "audio": every call fails with Error::unsupported
// (include/alchemy/host/host.hpp).
#include "alchemy/host/audio.hpp"

namespace alchemy::host::audio {

Result<Endpoints> endpoints() { return Error::unsupported; }
Result<OutputStream*> open_output(std::string_view, std::uint32_t, std::uint32_t, Render) { return Error::unsupported; }
OutputFormat output_format(const OutputStream*) noexcept { return {}; }
Status start_output(OutputStream*) { return Error::unsupported; }
void stop_output(OutputStream*) noexcept {}
void close_output(OutputStream*) noexcept {}
Result<InputStream*> open_input(std::string_view, std::uint32_t, std::uint32_t, Capture) { return Error::unsupported; }
InputFormat input_format(const InputStream*) noexcept { return {}; }
Status start_input(InputStream*) { return Error::unsupported; }
void stop_input(InputStream*) noexcept {}
void close_input(InputStream*) noexcept {}
Access microphone_access() { return Access::unknown; }

} // namespace alchemy::host::audio
