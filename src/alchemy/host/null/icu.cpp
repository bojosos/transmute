// Null host backend, area "icu": the host has no ICU
// (include/alchemy/host/icu.hpp).
#include "alchemy/host/icu.hpp"

namespace alchemy::host::icu {

const Api* api() noexcept { return nullptr; }

} // namespace alchemy::host::icu
