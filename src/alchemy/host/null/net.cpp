// Null host backend, area "net": every call fails with Error::unsupported
// (include/alchemy/host/host.hpp).
#include "alchemy/host/net.hpp"

namespace alchemy::host::net {

Status initialize() { return Error::unsupported; }
Result<std::vector<Interface>> interfaces() { return Error::unsupported; }
Result<DnsConfiguration> dns_configuration(std::optional<std::uint32_t>) { return Error::unsupported; }
Result<std::uint32_t> best_interface(const Address&) { return Error::unsupported; }
Result<Handle> open_socket(Family, SocketType, int) { return Error::unsupported; }
void close_socket(Handle) noexcept {}
Status set_blocking(Handle, bool) { return Error::unsupported; }
Status bind(Handle, const Address&) { return Error::unsupported; }
Status listen(Handle, int) { return Error::unsupported; }
Result<Accepted> accept(Handle) { return Error::unsupported; }
Status connect(Handle, const Address&) { return Error::unsupported; }
Result<std::size_t> send(Handle, ConstBytes, std::uint32_t) { return Error::unsupported; }
Result<std::size_t> send_to(Handle, ConstBytes, const Address&, std::uint32_t) { return Error::unsupported; }
Result<std::size_t> receive(Handle, Bytes, std::uint32_t) { return Error::unsupported; }
Result<Received> receive_from(Handle, Bytes, std::uint32_t) { return Error::unsupported; }
Status shutdown(Handle, Shutdown) { return Error::unsupported; }
Result<Address> local_address(Handle) { return Error::unsupported; }
Result<Address> peer_address(Handle) { return Error::unsupported; }
Result<std::size_t> readable_bytes(Handle) { return Error::unsupported; }
Result<SocketError> pending_error(Handle) { return Error::unsupported; }
Status set_option(Handle, Option, int) { return Error::unsupported; }
Result<int> get_option(Handle, Option) { return Error::unsupported; }
Status set_linger(Handle, Linger) { return Error::unsupported; }
Result<Linger> get_linger(Handle) { return Error::unsupported; }
Result<std::uint16_t> ready(Handle, std::uint16_t) { return Error::unsupported; }
Result<std::uint16_t> ready(Handle, std::uint16_t, std::uint64_t) { return Error::unsupported; }
Result<std::vector<std::uint8_t>> export_socket(Handle, std::uint32_t) { return Error::unsupported; }
Result<Handle> import_socket(std::span<const std::uint8_t>) { return Error::unsupported; }
Result<std::vector<Resolved>> resolve(std::optional<std::string_view>, std::optional<std::string_view>, const Hints&) { return Error::unsupported; }
Result<Name> name_info(const Address&, std::uint32_t) { return Error::unsupported; }
Result<std::string> host_name() { return Error::unsupported; }
Result<HostEntry> host_by_name(std::string_view) { return Error::unsupported; }
Result<HostEntry> host_by_address(const Address&) { return Error::unsupported; }
Result<ServiceEntry> service_by_name(std::string_view, std::optional<std::string_view>) { return Error::unsupported; }
Result<ServiceEntry> service_by_port(std::uint16_t, std::optional<std::string_view>) { return Error::unsupported; }
Result<ProtocolEntry> protocol_by_name(std::string_view) { return Error::unsupported; }

} // namespace alchemy::host::net
