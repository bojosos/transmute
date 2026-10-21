// Null host backend, area "midi": every call fails with Error::unsupported
// (include/alchemy/host/host.hpp).
#include "alchemy/host/midi.hpp"

namespace alchemy::host::midi {

struct Devices::State {};

Devices::Devices() = default;
Devices::~Devices() = default;
Result<std::vector<Port>> Devices::inputs() { return Error::unsupported; }
Result<std::vector<Port>> Devices::outputs() { return Error::unsupported; }
Status Devices::open_input(std::size_t, Receiver) { return Error::unsupported; }
void Devices::close_input(std::size_t) noexcept {}
Status Devices::send(std::size_t, std::span<const std::uint8_t>) { return Error::unsupported; }

} // namespace alchemy::host::midi
