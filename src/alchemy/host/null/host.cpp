// Null host backend: compiles everywhere and fails every call explicitly
// (host::Error::unsupported). It exists to prove that the macOS layer builds
// without OS headers (include/alchemy/host/host.hpp).
#include "alchemy/host/host.hpp"

namespace alchemy::host {

std::string_view backend() noexcept { return "null"; }
void close(Handle) noexcept {}
Result<Handle> duplicate(Handle) { return Error::unsupported; }

} // namespace alchemy::host
