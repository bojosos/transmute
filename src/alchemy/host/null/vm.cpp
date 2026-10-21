// Null host backend, area "vm": every call fails with Error::unsupported
// (include/alchemy/host/host.hpp).
#include "alchemy/host/vm.hpp"

namespace alchemy::host::vm {

PageInfo page_info() noexcept { return {4096, 65536}; }
Result<Region> query(std::uintptr_t) { return Error::unsupported; }
Status lock(std::uintptr_t, std::size_t) { return Error::unsupported; }
Status unlock(std::uintptr_t, std::size_t) { return Error::unsupported; }

} // namespace alchemy::host::vm
