// Null host backend, area "ipc": every call fails with Error::unsupported
// (include/alchemy/host/host.hpp).
#include "alchemy/host/ipc.hpp"

namespace alchemy::host::ipc {

Result<Handle> listen(std::string_view, bool) { return Error::unsupported; }
Status accept(Handle) { return Error::unsupported; }
Result<Handle> connect(std::string_view) { return Error::unsupported; }
Result<std::size_t> read(Handle, Bytes) { return Error::unsupported; }
Result<std::size_t> write(Handle, ConstBytes) { return Error::unsupported; }
void cancel(Handle) noexcept {}

} // namespace alchemy::host::ipc
