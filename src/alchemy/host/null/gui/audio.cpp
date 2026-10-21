// Null host backend, area "audio": every call fails with Error::unsupported
// (include/alchemy/host/host.hpp).
#include "alchemy/host/audio.hpp"

namespace alchemy::host::audio {

Result<Endpoints> endpoints() { return Error::unsupported; }

} // namespace alchemy::host::audio
