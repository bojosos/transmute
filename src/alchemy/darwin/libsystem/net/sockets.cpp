// Independently authored Darwin socket ABI adapters. Guest descriptors share
// the file table; native sockets never escape into guest structures.
#include "alchemy/darwin/libsystem/services_impl.hpp"

namespace alchemy {
namespace {
namespace net = host::net;
net::Family host_family(int domain) {
    if (domain == 2) return net::Family::ipv4;
    if (domain == 30) return net::Family::ipv6;
    if (!domain) return net::Family::unspecified;
    throw ServiceError{47};
}
int guest_family(net::Family family) { return family == net::Family::ipv4 ? 2 : family == net::Family::ipv6 ? 30 : 0; }
// A guest sockaddr_in / sockaddr_in6 (Darwin layout: sin_len, sin_family, port in network order).
net::Address read_network_address(const transmute::GuestMemory& memory, std::uint64_t address, std::uint64_t length) {
    if (length < 2 || length > 255) throw ServiceError{einval};
    validate(memory, address, length, false);
    const auto family = memory.read8(address + 1);
    if (family == 2) {
        if (length < 16 || (memory.read8(address) && memory.read8(address) != 16)) throw ServiceError{einval};
        std::array<std::uint8_t, 6> bytes{}; memory.read_bytes(address + 2, bytes);
        return net::Address::ipv4({bytes[2], bytes[3], bytes[4], bytes[5]}, static_cast<std::uint16_t>(bytes[0] << 8 | bytes[1]));
    }
    if (family == 30) {
        if (length < 28 || (memory.read8(address) && memory.read8(address) != 28)) throw ServiceError{einval};
        std::array<std::uint8_t, 22> bytes{}; memory.read_bytes(address + 2, bytes);
        std::array<std::uint8_t, 16> value{}; std::copy_n(bytes.begin() + 6, 16, value.begin());
        auto result = net::Address::ipv6(value, static_cast<std::uint16_t>(bytes[0] << 8 | bytes[1]), memory.read32(address + 24));
        std::memcpy(&result.flow_info, bytes.data() + 2, 4);   // as the sockaddr holds it
        return result;
    }
    throw ServiceError{47};
}
std::vector<std::uint8_t> encode_network_address(const net::Address& source, int logical_domain = 0) {
    if (logical_domain == 1) return {2, 1}; // Anonymous socketpair has no pathname.
    std::vector<std::uint8_t> bytes;
    if (source.family == net::Family::ipv4) {
        bytes.resize(16); bytes[0] = 16; bytes[1] = 2;
        bytes[2] = static_cast<std::uint8_t>(source.port >> 8); bytes[3] = static_cast<std::uint8_t>(source.port);
        std::copy_n(source.bytes.begin(), 4, bytes.begin() + 4);
    } else if (source.family == net::Family::ipv6) {
        bytes.resize(28); bytes[0] = 28; bytes[1] = 30;
        bytes[2] = static_cast<std::uint8_t>(source.port >> 8); bytes[3] = static_cast<std::uint8_t>(source.port);
        std::memcpy(bytes.data() + 4, &source.flow_info, 4);
        std::copy_n(source.bytes.begin(), 16, bytes.begin() + 8);
        for (unsigned index = 0; index < 4; ++index) bytes[24 + index] = static_cast<std::uint8_t>(source.scope_id >> (index * 8));
    } else throw ServiceError{47};
    return bytes;
}
// Darwin struct sockaddr_un {sun_len, sun_family (AF_UNIX = 1), sun_path[104]}.
constexpr std::size_t darwin_unix_path = 104;
std::vector<std::uint8_t> encode_unix_address(const std::string& path) {
    if (path.empty()) return {2, 1};   // unnamed, as socketpair's ends
    std::vector<std::uint8_t> bytes{static_cast<std::uint8_t>(2 + path.size() + 1), 1};
    bytes.insert(bytes.end(), path.begin(), path.end());
    bytes.push_back(0);
    return bytes;
}
void write_unix_address(transmute::GuestMemory& memory, std::uint64_t address, std::uint64_t length, const std::string& path) {
    if (!address) return;
    const auto bytes = encode_unix_address(path);
    const auto copied = std::min<std::size_t>(memory.read32(length), bytes.size());
    memory.write_bytes(address, std::span(bytes.data(), copied)); memory.write32(length, static_cast<std::uint32_t>(bytes.size()));
}
void validate_network_output(const transmute::GuestMemory& memory, std::uint64_t address, std::uint64_t length) {
    if (!address) return;
    validate(memory, length, 4, true); validate(memory, length, 4, false);
    validate(memory, address, std::min<std::uint32_t>(memory.read32(length), 255), true);
}
void write_network_address(transmute::GuestMemory& memory, std::uint64_t address, std::uint64_t length,
                           const net::Address& value, int domain = 0) {
    if (!address) return;
    const auto bytes = encode_network_address(value, domain);
    const auto copied = std::min<std::size_t>(memory.read32(length), bytes.size());
    memory.write_bytes(address, std::span(bytes.data(), copied)); memory.write32(length, static_cast<std::uint32_t>(bytes.size()));
}
// MSG_OOB, MSG_PEEK and MSG_DONTROUTE are the host flags of the same values;
// WAITALL and DONTWAIT are managed by Alchemy, never native blocking.
std::uint32_t native_message_flags(std::uint64_t flags) {
    constexpr auto supported = 1ULL | 2ULL | 4ULL | 0x40ULL | 0x80ULL | 0x80000ULL;
    if (flags & ~supported) throw ServiceError{45};
    if ((flags & (2 | 0x40)) == (2 | 0x40)) throw ServiceError{45};
    return static_cast<std::uint32_t>(flags & (net::message_out_of_band | net::message_peek | net::message_dont_route));
}
std::chrono::microseconds network_timeval(const transmute::GuestMemory& memory, std::uint64_t address) {
    validate(memory, address, 16, false);
    const auto seconds = std::bit_cast<std::int64_t>(memory.read64(address));
    const auto micros = std::bit_cast<std::int32_t>(memory.read32(address + 8));
    if (seconds < 0 || seconds > INT64_MAX / 1000000 || micros < 0 || micros >= 1000000 ||
        seconds * 1000000 > INT64_MAX - micros) throw ServiceError{einval};
    return std::chrono::microseconds(seconds * 1000000 + micros);
}
std::chrono::steady_clock::time_point network_deadline(std::chrono::microseconds duration) {
    const auto now = std::chrono::steady_clock::now();
    const auto maximum = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::time_point::max() - now);
    return duration >= maximum ? std::chrono::steady_clock::time_point::max() : now + duration;
}
// The Darwin EAI_* code of a failed host resolution.
std::uint32_t resolver_error(host::Error error) {
    switch (error) {
    case host::Error::none: return 0;
    case host::Error::not_found: return 8;              // EAI_NONAME
    case host::Error::would_block: return 2;            // EAI_AGAIN
    case host::Error::io: return 4;                     // EAI_FAIL
    case host::Error::address_not_available: return 7;  // EAI_NODATA
    case host::Error::no_memory: return 6;              // EAI_MEMORY
    case host::Error::invalid_argument: return 3;       // EAI_BADFLAGS
    case host::Error::address_family: return 5;         // EAI_FAMILY
    case host::Error::protocol_not_supported: return 9; // EAI_SERVICE
    case host::Error::socket_type: return 10;           // EAI_SOCKTYPE
    case host::Error::fault: return 14;                 // EAI_OVERFLOW
    default: return 11;                                 // EAI_SYSTEM
    }
}
// The Darwin AI_* flags are the host's resolve_* flags of the same values.
std::uint32_t resolver_flags(std::uint32_t flags) {
    if (flags & ~(1U | 2U | 4U | 0x100U | 0x400U | 0x800U | 0x1000U)) throw ServiceError{einval};
    return flags;
}
template<class Value> struct NetworkQuery {
    std::mutex mutex; std::condition_variable condition; bool complete{};
    std::uint32_t error{}; Value value{};
};
template<class Value, class Operation>
std::shared_ptr<NetworkQuery<Value>> query_network(ProcessContext& context, std::shared_ptr<NetworkSession> session, Operation operation) {
    auto query = std::make_shared<NetworkQuery<Value>>();
    if (session->queries.fetch_add(1) >= 64) { --session->queries; throw ServiceError{35}; }
    // A resolver may outlive cancellation; it owns every native input/result
    // and the Winsock session, and never touches ProcessContext or guest memory.
    try { std::thread([query, session, operation = std::move(operation)] {
        struct Active { std::shared_ptr<NetworkSession> session; ~Active() { --session->queries; } } active{session};
        std::uint32_t error = 0; Value result;
        try { error = operation(result); } catch (const std::bad_alloc&) { error = 6; } catch (...) { error = 4; }
        { std::lock_guard lock(query->mutex); query->value = std::move(result); query->error = error; query->complete = true; }
        query->condition.notify_one();
    }).detach(); } catch (const std::system_error&) { --session->queries; throw ServiceError{35}; }
    catch (...) { --session->queries; throw; }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::unique_lock lock(query->mutex);
    while (!query->complete) {
        if (context.requested_exit()) throw ServiceError{89};
        if (std::chrono::steady_clock::now() >= deadline) return {};
        query->condition.wait_for(lock, std::chrono::milliseconds(10));
    }
    return query;
}
// The host option for a Darwin level and integer option (ENOPROTOOPT when none).
net::Option native_socket_option(int level, int option) {
    if (level == 6) {
        // TCP_NODELAY; TCP_KEEPALIVE (idle seconds before probes), TCP_KEEPINTVL, TCP_KEEPCNT.
        if (option == 1) return net::Option::tcp_no_delay;
        if (option == 0x10) return net::Option::tcp_keep_idle;
        if (option == 0x101) return net::Option::tcp_keep_interval;
        if (option == 0x102) return net::Option::tcp_keep_count;
        throw ServiceError{42};
    }
    if (level == 0) {
        if (option == 3) return net::Option::ip_type_of_service; if (option == 4) return net::Option::ip_time_to_live;
        throw ServiceError{42};
    }
    if (level == 41) {
        if (option == 27) return net::Option::ipv6_only;
        throw ServiceError{42};
    }
    if (level != 0xffff) throw ServiceError{42};
    switch (option) {
    case 1: return net::Option::debug; case 2: return net::Option::accepting; case 4: return net::Option::reuse_address;
    case 8: return net::Option::keep_alive; case 0x10: return net::Option::dont_route; case 0x20: return net::Option::broadcast;
    case 0x100: return net::Option::out_of_band_inline;
    case 0x1001: return net::Option::send_buffer; case 0x1002: return net::Option::receive_buffer; default: throw ServiceError{42};
    }
}
struct LegacyHost {
    std::string name;
    std::vector<std::string> aliases;
    std::vector<std::vector<std::uint8_t>> addresses;
    int family{}, length{};
};
struct LegacyService { std::string name, protocol; std::vector<std::string> aliases; int port{}; };
struct LegacyProtocol { std::string name; std::vector<std::string> aliases; int protocol{}; };
// h_errno: HOST_NOT_FOUND 1, TRY_AGAIN 2, NO_RECOVERY 3, NO_DATA 4.
std::uint32_t legacy_resolver_error(host::Error error) {
    switch (error) {
    case host::Error::not_found: return 1;
    case host::Error::would_block: return 2;
    case host::Error::address_not_available: return 4;
    default: return 3;
    }
}
// A port in network order as a Darwin int (servent.s_port, getservbyport's argument).
std::uint16_t swap16(std::uint64_t value) { return static_cast<std::uint16_t>(((value & 0xff) << 8) | ((value >> 8) & 0xff)); }
struct NetworkInterface { std::uint32_t index; std::string name; };
std::vector<NetworkInterface> network_interfaces() {
    const auto interfaces = net::interfaces();
    if (!interfaces) throw ServiceError{file_failure(interfaces.error())};
    std::vector<NetworkInterface> result;
    for (const auto& entry : *interfaces) {
        if (!entry.index) continue;
        // Darwin names must fit IF_NAMESIZE (16); host adapter names need not.
        // These are explicit Alchemy names for the real host indices.
        result.push_back({entry.index, std::string(entry.kind == net::InterfaceKind::loopback ? "lo" : "if") + std::to_string(entry.index)});
    }
    return result;
}
// The 32-bit value of an inet_aton dotted address (one to four parts, each
// decimal, octal with a leading 0 or hex with 0x; the last part fills the
// remaining bytes), in host order.
std::optional<std::uint32_t> inet_aton_value(std::string_view text) {
    std::array<std::uint64_t, 4> parts{};
    std::size_t count = 0;
    while (true) {
        if (text.empty() || count == 4) return std::nullopt;
        int base = 10;
        if (text.size() > 1 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) { base = 16; text.remove_prefix(2); }
        else if (text.size() > 1 && text[0] == '0' && text[1] >= '0' && text[1] <= '9') { base = 8; text.remove_prefix(1); }
        std::uint64_t value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
        if (error != std::errc{} || end == text.data() || value > 0xffffffffULL) return std::nullopt;
        parts[count++] = value;
        text.remove_prefix(static_cast<std::size_t>(end - text.data()));
        if (text.empty()) break;
        if (text.front() != '.') {
            // Trailing white space ends the address.
            if (text.find_first_not_of(" \t\n\r\f\v") == std::string_view::npos) break;
            return std::nullopt;
        }
        text.remove_prefix(1);
    }
    const std::uint64_t limits[] = {0xffffffffULL, 0xffffffULL, 0xffffULL, 0xffULL};
    if (parts[count - 1] > limits[count - 1]) return std::nullopt;
    for (std::size_t index = 0; index + 1 < count; ++index) if (parts[index] > 0xff) return std::nullopt;
    std::uint64_t value = parts[count - 1];
    for (std::size_t index = 0; index + 1 < count; ++index) value |= parts[index] << (24 - 8 * index);
    return static_cast<std::uint32_t>(value);
}
} // namespace

// A guest AF_UNIX pathname. The path maps through the Alchemy root like file
// paths and follows symlink records (the host has no symlink there).
//
// Windows: the socket itself lives in a socket home directory under a short
// hashed name, and the guest pathname holds a small placeholder file. Native
// AF_UNIX works only in some directories (on the audit host, nothing under
// %LOCALAPPDATA%, so neither the Windows temporary directory nor the default
// Alchemy root: bind succeeds there but connect fails with WSAEINVAL and the
// socket file cannot be removed) and allows 108-byte paths. The home is the
// first of ALCHEMY_SOCKET_DIR, <root>/.alchemy/sockets and
// %USERPROFILE%/.alchemy/sockets where a bind/listen/connect self-test
// passes. Darwin's pathname rules hold for the placeholder: bind fails with
// EADDRINUSE when the path exists, unlink removes it (and the socket file),
// connect fails with ENOENT without it and ECONNREFUSED when it is not a
// socket. lstat/stat report the placeholder as a regular file.
struct DarwinServices::Impl::UnixAddress {
    net::Address native;
    std::string guest;
    std::filesystem::path host;
};
namespace {
constexpr std::string_view unix_placeholder_marker = "alchemy-unix-socket\n";
bool unix_home_works(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (!std::filesystem::is_directory(directory, error)) return false;
    const auto probe = directory / ("probe-" + std::to_string(host::process::current_pid()) + ".sock");
    std::filesystem::remove(probe, error);
    const auto address = net::Address::local(path_text(std::filesystem::path(probe).make_preferred()));
    const auto server = net::open_socket(net::Family::local, net::SocketType::stream), client = net::open_socket(net::Family::local, net::SocketType::stream);
    bool works = server && client && net::bind(*server, address) && net::listen(*server, 1) && net::set_blocking(*client, true) &&
        net::connect(*client, address);
    if (client) net::close_socket(*client);
    if (server) net::close_socket(*server);
    works = std::filesystem::remove(probe, error) && works;
    return works;
}
std::filesystem::path unix_socket_home() {
    static std::mutex mutex;
    static std::optional<std::filesystem::path> home;
    static bool searched = false;
    std::lock_guard lock(mutex);
    if (!searched) {
        searched = true;
        std::vector<std::filesystem::path> candidates;
        if (const auto value = host::process::environment("ALCHEMY_SOCKET_DIR"); value && !value->empty()) candidates.push_back(utf8_path(*value));
        if (const auto root = configured_alchemy_root()) candidates.push_back(*root / ".alchemy" / "sockets");
        if (const auto profile = host::process::environment("USERPROFILE"); profile && !profile->empty()) candidates.push_back(utf8_path(*profile) / ".alchemy" / "sockets");
        for (const auto& candidate : candidates) if (unix_home_works(candidate)) { home = candidate; break; }
        if (!home) std::fprintf(stderr, "alchemy: no directory supports AF_UNIX sockets (set ALCHEMY_SOCKET_DIR); AF_UNIX bind and connect fail with EOPNOTSUPP\n");
    }
    if (!home) throw ServiceError{45};
    return *home;
}
// The socket file for a guest pathname: FNV-1a of the host path, case-folded.
std::filesystem::path unix_socket_file(const std::filesystem::path& host) {
    auto text = path_text(host.lexically_normal());
    std::uint64_t hash = 14695981039346656037ULL;
    for (auto character : text) {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
        if (character == '\\') character = '/';
        hash = (hash ^ static_cast<std::uint8_t>(character)) * 1099511628211ULL;
    }
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.sock", static_cast<unsigned long long>(hash));
    return unix_socket_home() / name;
}
bool is_unix_placeholder(const std::filesystem::path& host) {
    const auto file = host::files::open(path_text(host), host::files::Access::read, host::files::Create::open_existing);
    if (!file) return false;
    std::array<std::byte, 32> bytes{};
    const auto read = host::files::read_at(*file, std::span(bytes.data(), unix_placeholder_marker.size()), 0);
    host::close(*file);
    return read && std::string_view(reinterpret_cast<const char*>(bytes.data()), *read) == unix_placeholder_marker;
}
} // namespace
DarwinServices::Impl::UnixAddress DarwinServices::Impl::unix_address(const transmute::GuestMemory& memory, std::uint64_t address,
                                                                     std::uint64_t length, bool connecting) {
    if (length < 3 || length > 255) throw ServiceError{einval};
    validate(memory, address, length, false);
    if (memory.read8(address + 1) != 1) throw ServiceError{47};
    const auto available = std::min<std::uint64_t>(length - 2, darwin_unix_path);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(available));
    memory.read_bytes(address + 2, bytes);
    const auto end = std::find(bytes.begin(), bytes.end(), std::uint8_t{0});
    if (end == bytes.end() && length - 2 > darwin_unix_path) throw ServiceError{einval};
    UnixAddress result;
    result.guest.assign(bytes.begin(), end);
    if (result.guest.empty()) throw ServiceError{einval};
    auto host = translate_path(result.guest);
    // Follow links to the placeholder they name, whose path names the socket
    // file: symlink records, and the host symlinks symlink(2) makes where the
    // host allows them (an administrator, or Developer Mode on Windows).
    for (int hops = 0; hops < 32; ++hops) {
        if (const auto record = libsystem::symlink_record(host)) {
            if (hops == 31) throw ServiceError{62};   // ELOOP
            host = record->front() == '/' ? translate_path(*record) : (host.parent_path() / std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t*>(record->data()), record->size()))).lexically_normal();
            continue;
        }
        std::error_code error;
        if (!std::filesystem::is_symlink(host, error)) break;
        const auto target = std::filesystem::read_symlink(host, error);
        if (error) break;
        if (hops == 31) throw ServiceError{62};   // ELOOP
        host = (target.is_absolute() ? target : host.parent_path() / target).lexically_normal();
    }
    result.host = host;
    if (connecting) {
        if (host::files::attributes(path_text(host)).error() == host::Error::not_found) throw ServiceError{enoent};
        if (!is_unix_placeholder(host)) throw ServiceError{61};   // not a socket: ECONNREFUSED
    }
    // A socket home path too long for the host fails at bind/connect (ENAMETOOLONG).
    result.native = net::Address::local(path_text(std::filesystem::path(unix_socket_file(host)).make_preferred()));
    return result;
}

// unlink(2) of an AF_UNIX placeholder also removes its socket file.
void remove_unix_socket_file(const std::filesystem::path& host) {
    if (!is_unix_placeholder(host)) return;
    std::error_code error;
    try { std::filesystem::remove(unix_socket_file(host), error); } catch (const ServiceError&) {}
}

std::shared_ptr<DarwinServices::Impl::File> DarwinServices::Impl::network_file(int descriptor) {
    const auto record = descriptor_file(descriptor);
    if (!record || !record->socket) throw ServiceError{38};
    if (record->closed) throw ServiceError{ebadf};
    return record;
}
// socketpair(AF_UNIX, SOCK_STREAM) and pseudo-terminals: two connected
// loopback stream sockets that read and close as an AF_UNIX stream pair.
std::pair<std::shared_ptr<NetworkSocket>, std::shared_ptr<NetworkSocket>> DarwinServices::Impl::connected_stream_pair() {
    auto first = create_socket(2, 1, 0), second = create_socket(2, 1, 0);
    const auto check = [](const host::Status& status) { if (!status) throw ServiceError{network_error(status.error())}; };
    check(net::bind(first->handle, net::Address::ipv4({127, 0, 0, 1})));
    const auto first_address = net::local_address(first->handle);
    if (!first_address) throw ServiceError{network_error(first_address.error())};
    auto listener = first; check(net::listen(listener->handle, 4));
    if (const auto connecting = net::connect(second->handle, *first_address); !connecting) {
        const auto error = network_error(connecting.error()); if (error != 35 && error != 36) throw ServiceError{error};
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
        if (context.requested_exit()) throw ServiceError{89};
        auto connection = net::accept(listener->handle);
        if (connection) {
            auto accepted = std::make_shared<NetworkSocket>(); accepted->session = network_session; accepted->type = 1;
            accepted->handle = connection->socket;
            const auto client = net::local_address(second->handle);
            if (!client) throw ServiceError{network_error(client.error())};
            if (connection->peer == *client) {
                { std::lock_guard lock(mutex); accepted->identity = ++network_identity; }
                first = std::move(accepted); break;
            }
        } else { const auto error = network_error(connection.error()); if (error != 35) throw ServiceError{error}; }
        if (std::chrono::steady_clock::now() >= deadline) throw ServiceError{60};
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    first->domain = second->domain = 1;
    return {std::move(first), std::move(second)};
}

std::shared_ptr<NetworkSocket> DarwinServices::Impl::create_socket(int domain, int type, int protocol) {
    if (domain == 1) {
        // AF_UNIX stream sockets are native (Windows AF_UNIX, afunix.h). The
        // host has no AF_UNIX datagram or seqpacket sockets (ESOCKTNOSUPPORT);
        // socketpair(AF_UNIX) is provided separately over loopback.
        if (type != 1) throw ServiceError{44};
        if (protocol != 0) throw ServiceError{43};
        auto result = std::make_shared<NetworkSocket>(); result->session = network_session;
        const auto opened = net::open_socket(net::Family::local, net::SocketType::stream);
        if (!opened) throw ServiceError{network_error(opened.error())};
        result->handle = *opened;
        result->domain = 1; result->type = 1; result->protocol = 0;
        { std::lock_guard lock(mutex); result->identity = ++network_identity; }
        return result;
    }
    const auto family = host_family(domain);
    if (type != 1 && type != 2) throw ServiceError{44};
    if (protocol != 0 && protocol != (type == 1 ? 6 : 17)) throw ServiceError{43};   // IPPROTO_TCP, IPPROTO_UDP
    auto result = std::make_shared<NetworkSocket>(); result->session = network_session;
    const auto opened = net::open_socket(family, type == 1 ? net::SocketType::stream : net::SocketType::datagram, protocol);
    if (!opened) throw ServiceError{network_error(opened.error())};
    result->handle = *opened;
    result->domain = domain; result->type = type; result->protocol = protocol;
    { std::lock_guard lock(mutex); result->identity = ++network_identity; }
    return result;
}
int DarwinServices::Impl::add_socket(std::shared_ptr<NetworkSocket> socket) {
    auto record = std::make_shared<File>(); record->socket = std::move(socket);
    record->readable = record->writable = true; record->flags = 2;
    std::lock_guard lock(mutex);
    for (int descriptor = 3; descriptor < 4096; ++descriptor) if (!files.contains(descriptor)) {
        files.emplace(descriptor, record); return descriptor;
    }
    throw ServiceError{emfile};
}
std::uint16_t DarwinServices::Impl::network_ready(const std::shared_ptr<File>& record, std::uint16_t events) {
    if (record && record->closed) return 32;
    if (record && record->pipe) return pipe_ready(record, events);
    if (!record || !record->socket) return static_cast<std::uint16_t>(events & (1 | 4 | 0x40));
    const auto socket = record->socket;
    std::lock_guard lock(socket->gate);
    if (record->closed || socket->closed) return 32;
    // POLLPRI (urgent data) is never reported ready: the host has no urgent-data
    // readiness. libcurl asks for POLLRDBAND|POLLPRI on a connecting socket.
    std::uint16_t asked = 0;
    if (events & (1 | 0x40 | 0x80)) asked |= net::ready_readable;
    if (events & (4 | 0x100)) asked |= net::ready_writable;
    const auto state = net::ready(socket->handle, asked);
    if (!state) throw ServiceError{network_error(state.error())};
    unsigned ready = 0;
    if (*state & net::ready_readable) ready |= events & (1 | 0x40 | 0x80);
    if (*state & net::ready_writable) ready |= events & (4 | 0x100);
    if (*state & net::ready_error) ready |= 8;
    if (*state & net::ready_hang_up) ready |= 16;
    if (*state & net::ready_invalid) ready |= 32;
    if ((ready & 1) && socket->type == 1 && !socket->listening && !socket->connecting) {
        std::array<std::byte, 1> byte{};
        const auto peeked = net::receive(socket->handle, byte, net::message_peek);
        if (peeked && *peeked == 0) ready |= 16;
    }
    return static_cast<std::uint16_t>(ready);
}
// EVFILT_VNODE: the watched file or directory is opened again (attributes
// only, sharing read/write/delete so the guest can still rename or remove
// it) and compared with its last state: NOTE_DELETE when it is delete-pending
// or has no links left, NOTE_RENAME when its final path changes, NOTE_WRITE
// when its last-write time changes (directories: a create, delete or rename
// inside, from a change notification), NOTE_EXTEND when it grows, NOTE_LINK
// when its link count (directories: subdirectory count) changes, NOTE_ATTRIB
// when its attributes or change time change otherwise. NOTE_REVOKE (unmount)
// and NOTE_FUNLOCK are never reported. Checked at most every 20 ms.
struct DarwinServices::Impl::VnodeState {
    std::mutex gate;
    host::Handle handle, change;
    bool directory{}, deleted{};
    std::string path;
    std::int64_t write{}, changed{};
    std::uint64_t size{}, subdirectories{}, links{};
    std::uint32_t attributes{};
    std::uint32_t pending{};
    std::chrono::steady_clock::time_point next{};
    ~VnodeState() {
        host::files::close_watch(change);
        if (handle) host::close(handle);
    }
};
namespace {
struct VnodeSnapshot { bool ok{}, deleted{}; std::string path; std::int64_t write{}, changed{}; std::uint64_t size{}, links{}; std::uint32_t attributes{}; };
VnodeSnapshot vnode_snapshot(host::Handle handle) {
    VnodeSnapshot snapshot;
    const auto info = host::files::info(handle);
    if (!info) return snapshot;
    snapshot.ok = true;
    snapshot.write = info->modified_ns;
    snapshot.changed = info->changed_ns;
    snapshot.attributes = info->mode;
    snapshot.size = info->size;
    snapshot.links = info->links;
    snapshot.deleted = info->delete_pending;
    if (auto path = host::files::final_path(handle)) snapshot.path = std::move(*path);
    return snapshot;
}
std::uint64_t vnode_subdirectories(const std::filesystem::path& path) {
    std::uint64_t count = 0;
    std::error_code error;
    for (std::filesystem::directory_iterator it(path, error), end; !error && it != end; it.increment(error))
        if (it->is_directory(error)) ++count;
    return count;
}
} // namespace
std::shared_ptr<DarwinServices::Impl::VnodeState> DarwinServices::Impl::vnode_watch(const std::shared_ptr<File>& record) {
    auto state = std::make_shared<VnodeState>();
    const auto opened = host::files::open_metadata(path_text(record->path));
    if (!opened) throw ServiceError{file_failure(opened.error())};
    state->handle = *opened;
    const auto snapshot = vnode_snapshot(state->handle);
    if (!snapshot.ok) throw ServiceError{file_failure(host::files::info(state->handle).error())};
    state->path = snapshot.path; state->write = snapshot.write; state->changed = snapshot.changed;
    state->size = snapshot.size; state->attributes = snapshot.attributes; state->links = snapshot.links;
    state->deleted = snapshot.deleted;
    state->directory = record->directory || host::files::info(state->handle)->directory;
    if (state->directory) {
        const auto watch = host::files::watch_directory(path_text(record->path));
        if (!watch) throw ServiceError{file_failure(watch.error())};
        state->change = *watch;
        state->subdirectories = vnode_subdirectories(state->path.empty() ? record->path : utf8_path(state->path));
    }
    return state;
}
std::uint32_t DarwinServices::Impl::vnode_poll(VnodeState& state) {
    constexpr std::uint32_t note_delete = 0x1, note_write = 0x2, note_extend = 0x4, note_attrib = 0x8, note_link = 0x10, note_rename = 0x20;
    std::lock_guard lock(state.gate);
    const auto now = std::chrono::steady_clock::now();
    if (state.deleted || now < state.next) return 0;
    state.next = now + std::chrono::milliseconds(20);
    const auto snapshot = vnode_snapshot(state.handle);
    if (!snapshot.ok) return 0;
    std::uint32_t notes = 0;
    if (snapshot.deleted) { notes |= note_delete; state.deleted = true; }
    if (!snapshot.path.empty() && !state.path.empty() && snapshot.path != state.path) notes |= note_rename;
    if (state.directory) {
        if (const auto changed = host::files::directory_changed(state.change); changed && *changed) {
            notes |= note_write;
            const auto subdirectories = vnode_subdirectories(utf8_path(snapshot.path.empty() ? state.path : snapshot.path));
            if (subdirectories != state.subdirectories) { notes |= note_link; state.subdirectories = subdirectories; }
        }
        if (snapshot.attributes != state.attributes) notes |= note_attrib;
    } else {
        // A size change is a write too (the last-write time can lag behind it).
        if (snapshot.write != state.write || snapshot.size != state.size) notes |= note_write;
        if (snapshot.size > state.size) notes |= note_extend;
        if (snapshot.links != state.links) notes |= note_link;
        if (snapshot.attributes != state.attributes ||
            (snapshot.changed != state.changed && !(notes & (note_write | note_extend | note_link | note_rename | note_delete))))
            notes |= note_attrib;
    }
    if (!snapshot.path.empty()) state.path = snapshot.path;
    state.write = snapshot.write; state.changed = snapshot.changed; state.size = snapshot.size;
    state.attributes = snapshot.attributes; state.links = snapshot.links;
    return notes;
}

std::uint64_t DarwinServices::Impl::network_read_hint(const std::shared_ptr<File>& record) {
    if (!record) throw ServiceError{45};
    if (record->pipe) return pipe_available(record);   // bytes in the pipe (EVFILT_READ data)
    if (record->socket) {
        std::lock_guard lock(record->socket->gate); if (record->closed) throw ServiceError{ebadf};
        // Readability of a listener guarantees a pending connection. Its hint
        // denotes at least one connection, without claiming exact backlog size.
        if (record->socket->listening) return 1;
        const auto count = net::readable_bytes(record->socket->handle);
        if (!count) throw ServiceError{network_error(count.error())};
        return static_cast<std::uint64_t>(*count);
    }
    std::lock_guard lock(record->mutex); if (record->closed) throw ServiceError{ebadf};
    if (record->random_device) return 1;
    if (record->standard >= 0 || record->directory || record->queue) throw ServiceError{45};
    if (!record->native) throw ServiceError{ebadf};
    const auto size = file_size(record->native);
    const auto position = host::files::seek(record->native, 0, host::files::Whence::current);
    if (!position) throw ServiceError{29};
    return *position >= size ? 0 : size - *position;
}
std::uint64_t DarwinServices::Impl::network_io(transmute::GuestMemory& memory, const std::shared_ptr<File>& record,
    std::uint64_t address, std::uint64_t size, std::uint64_t flags, bool sending,
    std::uint64_t peer, std::uint64_t peer_length) {
    if (size > transfer_limit || size > INT_MAX) throw ServiceError{einval};
    validate(memory, address, size, !sending);
    const auto native_flags = native_message_flags(flags);
    net::Address destination;
    if (sending && peer) {
        destination = read_network_address(memory, peer, peer_length);
        // ALCHEMY_NETWORK=loopback (network_policy.hpp): nothing leaves the machine.
        if (network_policy::loopback_only() && !network_policy::loopback_address(destination)) {
            network_policy::deny("sendto", network_policy::address_text(destination));
            throw ServiceError{51};   // ENETUNREACH
        }
        if (network_policy::refuse_listed_address("sendto", destination)) throw ServiceError{51};   // ALCHEMY_NETWORK_DENY
        network_policy::allowed("sendto", destination);
    }
    if (!sending) validate_network_output(memory, peer, peer_length);
    const auto socket = record->socket;
    std::chrono::microseconds timeout;
    { std::lock_guard lock(socket->gate); timeout = sending ? socket->send_timeout : socket->receive_timeout; }
    const auto deadline = timeout.count() ? network_deadline(timeout) : std::chrono::steady_clock::time_point::max();
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::max<std::uint64_t>(size, 1)));
    if (sending && size) memory.read_bytes(address, std::span(bytes.data(), static_cast<std::size_t>(size)));
    if (!size && socket->type == 1) return 0;
    std::uint64_t transferred = 0;
    for (;;) {
        if (record->closed || socket->closed) { if (transferred) return transferred; throw ServiceError{ebadf}; }
        if (context.requested_exit()) { if (transferred) return transferred; throw ServiceError{89}; }
        net::Address source;
        std::int64_t count = -1; std::uint32_t error = 0;
        {
            std::lock_guard lock(socket->gate);
            if (record->closed || socket->closed) throw ServiceError{ebadf};
            // A datagram longer than the buffer is truncated to it, without
            // MSG_TRUNC, as on Darwin.
            const auto buffer = std::span(reinterpret_cast<std::byte*>(bytes.data()) + static_cast<std::size_t>(transferred),
                                          static_cast<std::size_t>(size - transferred));
            host::Result<std::size_t> moved = host::Error::none;
            if (sending) moved = peer ? net::send_to(socket->handle, buffer, destination, native_flags) : net::send(socket->handle, buffer, native_flags);
            else if (peer) {
                auto received = net::receive_from(socket->handle, buffer, native_flags);
                if (received) { source = received->from; moved = received->size; } else moved = received.error();
            } else moved = net::receive(socket->handle, buffer, native_flags);
            if (moved) count = static_cast<std::int64_t>(*moved); else error = network_error(moved.error());
        }
        // A Darwin AF_UNIX stream whose peer closed reads end-of-file; the host
        // (Windows AF_UNIX) resets the connection when unread data was left.
        if (!sending && error == 54 && socket->domain == 1 && socket->type == 1) return transferred;
        if (count >= 0) {
            if (!sending && count) memory.write_bytes(address + transferred,
                std::span(bytes.data() + static_cast<std::size_t>(transferred), static_cast<std::size_t>(count)));
            transferred += static_cast<std::uint64_t>(count);
            if (!sending && peer) write_network_address(memory, peer, peer_length, source, socket->domain);
            if (!sending && socket->type == 1 && (flags & 0x40) && count && transferred < size && !(socket->status_flags & 4)) continue;
            return transferred;
        }
        if (error != 35) { if (transferred) return transferred; throw ServiceError{error}; }
        if ((socket->status_flags & 4) || (flags & 0x80)) { if (transferred) return transferred; throw ServiceError{35}; }
        if (std::chrono::steady_clock::now() >= deadline) { if (transferred) return transferred; throw ServiceError{35}; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
std::uint64_t DarwinServices::Impl::network_poll(transmute::CpuState& state, transmute::GuestMemory& memory, bool select_mode) {
    struct Wait { std::shared_ptr<File> file; int descriptor; std::uint16_t requested, ready{}; };
    std::vector<Wait> waits;
    auto deadline = std::chrono::steady_clock::time_point::max();
    std::array<std::vector<std::uint32_t>, 3> masks;
    std::size_t words = 0;
    if (select_mode) {
        const auto nfds = static_cast<std::int32_t>(state.x[0]);
        if (nfds < 0 || nfds > 4096) throw ServiceError{einval};
        words = (static_cast<std::size_t>(nfds) + 31) / 32;
        for (unsigned set = 0; set < 3; ++set) if (state.x[set + 1]) {
            validate(memory, state.x[set + 1], words * 4, false); validate(memory, state.x[set + 1], words * 4, true);
            masks[set].resize(words); for (std::size_t word = 0; word < words; ++word) masks[set][word] = memory.read32(state.x[set + 1] + word * 4);
        }
        if (state.x[4]) deadline = network_deadline(network_timeval(memory, state.x[4]));
        for (int descriptor = 0; descriptor < nfds; ++descriptor) {
            std::uint16_t events = 0;
            for (unsigned set = 0; set < 3; ++set) if (!masks[set].empty() && (masks[set][static_cast<std::size_t>(descriptor) / 32] & (1U << (descriptor % 32)))) events |= static_cast<std::uint16_t>(set == 0 ? 1 : set == 1 ? 4 : 2);
            if (events) waits.push_back({descriptor_file(descriptor), descriptor, events});
        }
    } else {
        const auto count = static_cast<std::uint32_t>(state.x[1]); if (count > 4096) throw ServiceError{einval};
        validate(memory, state.x[0], static_cast<std::uint64_t>(count) * 8, false); validate(memory, state.x[0], static_cast<std::uint64_t>(count) * 8, true);
        const auto timeout = static_cast<std::int32_t>(state.x[2]); if (timeout >= 0) deadline = network_deadline(std::chrono::milliseconds(timeout));
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto at = state.x[0] + index * 8;
            const auto descriptor = static_cast<std::int32_t>(memory.read32(at));
            const auto events = static_cast<std::uint16_t>(memory.read8(at + 4) | (memory.read8(at + 5) << 8));
            std::shared_ptr<File> record; std::uint16_t invalid = 0;
            if (descriptor >= 0) try { record = descriptor_file(descriptor); } catch (const ServiceError&) { invalid = 32; }
            waits.push_back({record, descriptor, events, invalid});
        }
    }
    for (;;) {
        if (context.requested_exit()) throw ServiceError{89};
        std::uint64_t count = 0;
        for (auto& wait : waits) {
            if (wait.descriptor < 0) wait.ready = 0;
            else if (!(wait.ready & 32)) wait.ready = network_ready(wait.file, wait.requested);
            if (select_mode && (wait.ready & 32)) throw ServiceError{ebadf};
            if (select_mode) {
                if ((wait.requested & 1) && (wait.ready & (1 | 8 | 16))) ++count;
                if ((wait.requested & 4) && (wait.ready & (4 | 8 | 16))) ++count;
                if ((wait.requested & 2) && (wait.ready & 2)) ++count;
            } else if (wait.ready) ++count;
        }
        if (count || std::chrono::steady_clock::now() >= deadline) {
            if (!select_mode) for (std::size_t index = 0; index < waits.size(); ++index) {
                memory.write8(state.x[0] + index * 8 + 6, static_cast<std::uint8_t>(waits[index].ready));
                memory.write8(state.x[0] + index * 8 + 7, static_cast<std::uint8_t>(waits[index].ready >> 8));
            } else {
                for (auto& mask : masks) std::fill(mask.begin(), mask.end(), 0);
                for (const auto& wait : waits) for (unsigned set = 0; set < 3; ++set) {
                    const auto bit = static_cast<std::uint16_t>(set == 0 ? 1 : set == 1 ? 4 : 2);
                    const auto ready = static_cast<std::uint16_t>(set == 2 ? 2 : bit | 8 | 16);
                    if ((wait.requested & bit) && (wait.ready & ready)) masks[set][static_cast<std::size_t>(wait.descriptor) / 32] |= 1U << (wait.descriptor % 32);
                }
                for (unsigned set = 0; set < 3; ++set) for (std::size_t word = 0; word < masks[set].size(); ++word) memory.write32(state.x[set + 1] + word * 4, masks[set][word]);
            }
            return count;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// struct msghdr {void *msg_name; socklen_t msg_namelen; struct iovec *msg_iov;
// int msg_iovlen; void *msg_control; socklen_t msg_controllen; int msg_flags}.
std::uint64_t DarwinServices::Impl::network_message(transmute::GuestMemory& memory, int descriptor, std::uint64_t message,
                                                    std::uint64_t flags, bool sending) {
    const auto record = network_file(descriptor);
    validate(memory, message, 48, !sending);
    const auto name = memory.read64(message), iov = memory.read64(message + 16), control = memory.read64(message + 32);
    const auto count = static_cast<std::int32_t>(memory.read32(message + 24));
    const auto control_length = memory.read32(message + 40);
    if (count < 0 || count > 1024) throw ServiceError{einval};   // IOV_MAX
    if (sending && control && control_length) throw ServiceError{45};
    validate(memory, iov, static_cast<std::uint64_t>(count) * 16, false);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pieces;
    std::uint64_t total = 0;
    for (std::int32_t index = 0; index < count; ++index) {
        const auto base = memory.read64(iov + index * 16ULL), size = memory.read64(iov + index * 16ULL + 8);
        if (size > transfer_limit - total) throw ServiceError{einval};
        if (size) { validate(memory, base, size, !sending); pieces.emplace_back(base, size); total += size; }
    }
    // One contiguous transfer keeps datagram boundaries.
    const auto scratch = context.allocate_guest(std::max<std::uint64_t>(total, 1));
    struct Release { ProcessContext& context; std::uint64_t address; ~Release() { context.release_guest(address); } } release_scratch{context, scratch};
    if (sending) { std::uint64_t at = 0; for (const auto& [base, size] : pieces) { memory.copy_bytes(scratch + at, base, size); at += size; } }
    std::uint64_t name_length_cell = 0;
    if (!sending && name) { name_length_cell = message + 8; }
    const auto transferred = network_io(memory, record, scratch, total, flags, sending,
        sending ? name : (name ? name : 0), sending ? memory.read32(message + 8) : name_length_cell);
    if (!sending) {
        std::uint64_t at = 0;
        for (const auto& [base, size] : pieces) {
            if (at >= transferred) break;
            const auto part = std::min(size, transferred - at);
            memory.copy_bytes(base, scratch + at, part); at += part;
        }
        if (control) memory.write32(message + 40, 0);
        memory.write32(message + 44, 0);
    }
    return transferred;
}

void DarwinServices::Impl::install_network_services() {
    network_session = std::make_shared<NetworkSession>();
    network_h_errno = context.allocate_guest(4); context.main_engine().write32(network_h_errno, 0);
    context.install_export(std::string(system_library), "_h_errno", {network_h_errno, ExportKind::data});
    install("_socket", [this](auto& s, auto&) { return static_cast<std::uint64_t>(add_socket(create_socket(static_cast<int>(s.x[0]), static_cast<int>(s.x[1]), static_cast<int>(s.x[2])))); });
    install("_bind", [this](auto& s, auto& m) {
        const auto record = network_file(static_cast<int>(s.x[0]));
        if (record->socket->domain == 1) {
            // bind creates the socket file (EADDRINUSE when the path exists, as on Darwin).
            const auto address = unix_address(m, s.x[1], s.x[2], false);
            std::lock_guard lock(record->socket->gate); if (record->closed) throw ServiceError{ebadf};
            if (!record->socket->unix_local.empty()) throw ServiceError{einval};
            // The placeholder claims the pathname (EADDRINUSE when it exists).
            const auto placeholder = host::files::open(path_text(address.host), host::files::Access::write, host::files::Create::create_new);
            if (!placeholder) throw ServiceError{placeholder.error() == host::Error::exists ? 48U : file_failure(placeholder.error())};
            const auto marker = std::as_bytes(std::span(unix_placeholder_marker.data(), unix_placeholder_marker.size()));
            const auto wrote = host::files::write_at(*placeholder, marker, 0);
            host::close(*placeholder);
            std::error_code ignored;
            std::filesystem::remove(utf8_path(address.native.path), ignored);   // a socket file left by an earlier, unlinked-by-hand placeholder
            const auto bound = wrote ? net::bind(record->socket->handle, address.native) : host::Status(wrote.error());
            if (!bound) {
                std::filesystem::remove(address.host, ignored);
                throw ServiceError{wrote ? network_error(bound.error()) : file_failure(bound.error())};
            }
            record->socket->unix_local = address.guest;
            return std::uint64_t{0};
        }
        const auto address = read_network_address(m, s.x[1], s.x[2]);
        std::lock_guard lock(record->socket->gate); if (record->closed) throw ServiceError{ebadf};
        if (const auto bound = net::bind(record->socket->handle, address); !bound) throw ServiceError{network_error(bound.error())};
        return std::uint64_t{0};
    });
    install("_listen", [this](auto& s, auto&) {
        const auto record = network_file(static_cast<int>(s.x[0])); std::lock_guard lock(record->socket->gate);
        if (record->closed) throw ServiceError{ebadf};
        if (const auto listening = net::listen(record->socket->handle, static_cast<int>(s.x[1])); !listening) throw ServiceError{network_error(listening.error())};
        record->socket->listening = true; return std::uint64_t{0};
    });
    install("_connect", [this](auto& s, auto& m) {
        const auto record = network_file(static_cast<int>(s.x[0])); const auto socket = record->socket;
        std::string unix_peer;
        const auto address = socket->domain == 1 ? [&] {
            auto named = unix_address(m, s.x[1], s.x[2], true); unix_peer = named.guest; return named.native;
        }() : read_network_address(m, s.x[1], s.x[2]);
        if (socket->domain != 1 && network_policy::loopback_only() && !network_policy::loopback_address(address)) {
            network_policy::deny("connect", network_policy::address_text(address));
            throw ServiceError{51};   // ENETUNREACH
        }
        if (socket->domain != 1 && network_policy::refuse_listed_address("connect", address)) throw ServiceError{51};   // ALCHEMY_NETWORK_DENY
        if (socket->domain != 1) network_policy::allowed("connect", address);
        std::uint32_t error = 0;
        {
            std::lock_guard lock(socket->gate); if (record->closed) throw ServiceError{ebadf};
            if (socket->domain == 1) {
                // A local connect has no handshake to wait for: it runs blocking,
                // so it completes or fails at once, and a guest nonblocking
                // connect returns 0 immediately (POSIX allows it) instead of
                // EINPROGRESS and a later writability poll.
                if (const auto blocking = net::set_blocking(socket->handle, true); !blocking) throw ServiceError{network_error(blocking.error())};
                const auto status = net::connect(socket->handle, address);
                const auto connect_error = status ? 0U : network_error(status.error());
                if (const auto nonblocking = net::set_blocking(socket->handle, false); !nonblocking) throw ServiceError{network_error(nonblocking.error())};
                if (connect_error && connect_error != 35 && connect_error != 36) throw ServiceError{connect_error};
                socket->unix_peer = unix_peer;
                if (!connect_error) return std::uint64_t{0};
                error = connect_error; socket->connecting = true;
            } else {
                const auto status = net::connect(socket->handle, address);
                if (status) return std::uint64_t{0};
                error = network_error(status.error()); if (error == 35 || error == 36 || error == 37) socket->connecting = true;
            }
        }
        if (error != 35 && error != 36 && error != 37) throw ServiceError{error};
        if (socket->status_flags & 4) throw ServiceError{error == 37 ? 37U : 36U};
        for (;;) {
            if (record->closed) throw ServiceError{ebadf}; if (context.requested_exit()) throw ServiceError{89};
            if (network_ready(record, 4) & (4 | 8 | 16)) {
                std::lock_guard lock(socket->gate); if (record->closed) throw ServiceError{ebadf};
                const auto ended = net::pending_error(socket->handle);
                if (!ended) throw ServiceError{network_error(ended.error())};
                socket->connecting = false; if (ended->error != host::Error::none) throw ServiceError{network_error(ended->error)}; return std::uint64_t{0};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    install("_accept", [this](auto& s, auto& m) {
        const auto record = network_file(static_cast<int>(s.x[0])); validate_network_output(m, s.x[1], s.x[2]);
        for (;;) {
            if (record->closed) throw ServiceError{ebadf}; if (context.requested_exit()) throw ServiceError{89};
            auto accepted = std::make_shared<NetworkSocket>(); net::Address peer;
            accepted->session = network_session; accepted->domain = record->socket->domain; accepted->type = 1;
            accepted->unix_local = record->socket->unix_local;   // AF_UNIX: the listener's path; the peer is unnamed
            std::uint32_t error = 0;
            {
                std::lock_guard lock(record->socket->gate); if (record->closed) throw ServiceError{ebadf};
                auto connection = net::accept(record->socket->handle);
                if (connection) { accepted->handle = connection->socket; peer = connection->peer; } else error = network_error(connection.error());
            }
            if (!error) {
                { std::lock_guard lock(mutex); accepted->identity = ++network_identity; }
                write_network_address(m, s.x[1], s.x[2], peer, accepted->domain);
                return static_cast<std::uint64_t>(add_socket(std::move(accepted)));
            }
            if (error != 35 || (record->socket->status_flags & 4)) throw ServiceError{error};
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    for (const auto peer : {false, true}) install(peer ? "_getpeername" : "_getsockname", [this, peer](auto& s, auto& m) {
        const auto record = network_file(static_cast<int>(s.x[0])); if (!s.x[1]) throw ServiceError{efault};
        validate_network_output(m, s.x[1], s.x[2]);
        if (record->socket->domain == 1) {
            std::string path;
            { std::lock_guard lock(record->socket->gate); if (record->closed) throw ServiceError{ebadf};
                // Unconnected AF_UNIX sockets have no peer (ENOTCONN), as on Darwin.
                if (peer) {
                    if (const auto connected = net::peer_address(record->socket->handle); !connected) throw ServiceError{network_error(connected.error())};
                    path = record->socket->unix_peer;
                } else path = record->socket->unix_local; }
            write_unix_address(m, s.x[1], s.x[2], path); return std::uint64_t{0};
        }
        net::Address result;
        { std::lock_guard lock(record->socket->gate); if (record->closed) throw ServiceError{ebadf};
            auto named = peer ? net::peer_address(record->socket->handle) : net::local_address(record->socket->handle);
            if (!named) throw ServiceError{network_error(named.error())};
            result = std::move(*named); }
        write_network_address(m, s.x[1], s.x[2], result, record->socket->domain); return std::uint64_t{0};
    });
    install("_shutdown", [this](auto& s, auto&) {
        if (s.x[1] > 2) throw ServiceError{einval}; const auto record = network_file(static_cast<int>(s.x[0]));
        std::lock_guard lock(record->socket->gate); if (record->closed) throw ServiceError{ebadf};
        const auto how = s.x[1] == 0 ? net::Shutdown::receive : s.x[1] == 1 ? net::Shutdown::send : net::Shutdown::both;
        if (const auto done = net::shutdown(record->socket->handle, how); !done) throw ServiceError{network_error(done.error())};
        if (s.x[1] != 0) record->socket->send_shut = true;
        return std::uint64_t{0};
    });
    for (const auto sending : {false, true}) for (const auto peer : {false, true})
        install(sending ? (peer ? "_sendto" : "_send") : (peer ? "_recvfrom" : "_recv"), [this, sending, peer](auto& s, auto& m) {
            return network_io(m, network_file(static_cast<int>(s.x[0])), s.x[1], s.x[2], s.x[3], sending, peer ? s.x[4] : 0, peer ? s.x[5] : 0);
        });
    install("_poll", [this](auto& s, auto& m) { return network_poll(s, m, false); });
    for (const auto name : {"_select", "_select$DARWIN_EXTSN"}) install(name, [this](auto& s, auto& m) { return network_poll(s, m, true); });
    install("___darwin_check_fd_set_overflow", [](auto& s, auto& m) {
        const auto descriptor = static_cast<std::int32_t>(s.x[0]);
        if (descriptor < 0 || descriptor >= (s.x[2] ? 4096 : 1024)) return std::uint64_t{0};
        validate(m, add(s.x[1], static_cast<std::uint64_t>(descriptor / 32) * 4), 4, false); return std::uint64_t{1};
    }, 0);
    install("_kqueue", [this](auto&, auto&) {
        auto record = std::make_shared<File>(); record->queue = std::make_shared<NetworkQueue>();
        std::lock_guard lock(mutex);
        for (int descriptor = 3; descriptor < 4096; ++descriptor) if (!files.contains(descriptor)) {
            files.emplace(descriptor, record); return static_cast<std::uint64_t>(descriptor);
        }
        throw ServiceError{emfile};
    });
    // kevent (32-byte struct kevent) and kevent64 (48-byte kevent64_s: the
    // same fields plus ext[2]) share one queue. EVFILT_MACHPORT (-8) watches
    // a Mach receive right or port set through the registered Mach IPC
    // provider (include/alchemy/mach_port_events.hpp); with MACH_RCV_MSG in
    // fflags the message is received into ext[0]/ext[1].
    // EVFILT_TIMER (-7): data is the interval in milliseconds, or in the unit
    // of NOTE_SECONDS/USECONDS/NSECONDS/MACHTIME (mach time is nanoseconds
    // here, timebase 1/1); with NOTE_ABSOLUTE it is a deadline (wall clock, or
    // mach absolute time with NOTE_MACHTIME); a past deadline or a negative
    // interval fires at once, once. The event's data is the number
    // of expirations since the last delivery. Leeway, critical and background
    // hints are accepted; timers fire within about a millisecond (the queue
    // polls).
    // EV_CLEAR (edge triggering): after delivery an event fires again only when
    // its state changes: more (or less) readable data, writability or EOF
    // returning after a lapse, a newer Mach message (detection mode). Timers
    // already report expirations since the last delivery.
    const auto run_kevent = [this](auto& s, auto& m, bool wide, std::uint64_t timeout_address, bool immediate) -> std::uint64_t {
        const auto queue_record = file(static_cast<int>(s.x[0])); if (!queue_record->queue) throw ServiceError{einval};
        const auto queue = queue_record->queue;
        const std::uint64_t stride = wide ? 48 : 32;
        const auto changes = static_cast<std::int32_t>(s.x[2]), capacity = static_cast<std::int32_t>(s.x[4]);
        if (changes < 0 || capacity < 0 || changes > 4096 || capacity > 4096) throw ServiceError{einval};
        validate(m, s.x[1], static_cast<std::uint64_t>(changes) * stride, false);
        validate(m, s.x[3], static_cast<std::uint64_t>(capacity) * stride, true);
        auto deadline = std::chrono::steady_clock::time_point::max();
        if (immediate) deadline = std::chrono::steady_clock::now();
        else if (timeout_address) {
            validate(m, timeout_address, 16, false);
            const auto seconds = std::bit_cast<std::int64_t>(m.read64(timeout_address)); const auto nanos = std::bit_cast<std::int64_t>(m.read64(timeout_address + 8));
            if (seconds < 0 || seconds > INT64_MAX / 1000000 || nanos < 0 || nanos >= 1000000000 || seconds * 1000000 > INT64_MAX - nanos / 1000) throw ServiceError{einval};
            deadline = network_deadline(std::chrono::microseconds(seconds * 1000000 + (nanos + 999) / 1000));
        }
        const auto mach = mach_port_events(context);
        // mach_absolute_time's clock (misc/support.cpp: unbiased interrupt time).
        const auto mach_now = []() -> std::uint64_t { return host::time::monotonic_ns(); };
        std::uint64_t produced = 0;
        const auto emit = [&](const NetworkQueue::Watch& watch, std::uint16_t flags, std::uint64_t data, std::uint32_t fflags = 0, std::uint64_t ext1 = 0) {
            const auto at = s.x[3] + produced * stride; m.fill_bytes(at, 0, stride);
            m.write64(at, watch.ident); const auto filter = static_cast<std::uint16_t>(watch.filter);
            m.write8(at + 8, static_cast<std::uint8_t>(filter)); m.write8(at + 9, static_cast<std::uint8_t>(filter >> 8));
            m.write8(at + 10, static_cast<std::uint8_t>(flags)); m.write8(at + 11, static_cast<std::uint8_t>(flags >> 8));
            m.write32(at + 12, fflags); m.write64(at + 16, data); m.write64(at + 24, watch.udata);
            if (wide) { m.write64(at + 32, watch.ext0); m.write64(at + 40, ext1); }
            ++produced;
        };
        for (int index = 0; index < changes; ++index) {
            const auto at = s.x[1] + static_cast<std::uint64_t>(index) * stride;
            NetworkQueue::Watch change; change.ident = m.read64(at); change.udata = m.read64(at + 24);
            change.filter = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(m.read8(at + 8) | (m.read8(at + 9) << 8)));
            change.flags = static_cast<std::uint16_t>(m.read8(at + 10) | (m.read8(at + 11) << 8));
            const bool machport = change.filter == -8, timer = change.filter == -7, vnode = change.filter == -4, proc = change.filter == -5;
            if (machport) { change.fflags = m.read32(at + 12); if (wide) { change.ext0 = m.read64(at + 32); change.ext1 = m.read64(at + 40); } }
            if (timer || vnode || proc) change.fflags = m.read32(at + 12);
            std::uint32_t error = 0;
            if (change.filter != -1 && change.filter != -2 && !timer && !vnode && !proc && !(machport && mach)) error = 45;
            else if ((change.flags & ~0xffU) || (!machport && !timer && !vnode && !proc && m.read32(at + 12)) || (!timer && m.read64(at + 16)) || ((change.flags & 4) && (change.flags & 8))) error = 45;
            // EVFILT_PROC: NOTE_EXIT (with NOTE_EXITSTATUS) of this process's
            // own children only; fork, exec and signal notes are not tracked.
            else if (proc && (change.fflags & ~(0x80000000U | 0x04000000U))) error = 45;
            else if (proc && (change.flags & 1) && !child_status(static_cast<std::uint32_t>(change.ident))) error = 3;   // ESRCH
            else if (timer && (change.fflags & ~0x1ffU)) error = einval;
            // A negative NOTE_ABSOLUTE deadline is not a past one: XNU reads it
            // unsigned, a far deadline or ERANGE (design); refused here.
            else if (timer && (change.fflags & 0x8) && std::bit_cast<std::int64_t>(m.read64(at + 16)) < 0) error = einval;
            else if (vnode && (change.fflags & ~0x1ffU)) error = einval;   // NOTE_DELETE ... NOTE_FUNLOCK
            else if (!timer && change.ident > (machport ? UINT32_MAX : INT32_MAX)) error = machport ? enoent : ebadf;
            if (!error && (change.flags & 1) && timer) {
                const auto value = m.read64(at + 16);
                const auto unit = (change.fflags & 0x1) ? 1000000000ULL : (change.fflags & 0x2) ? 1000ULL : (change.fflags & (0x4 | 0x100 | 0x80)) ? 1ULL : 1000000ULL;
                const auto nanoseconds = value > UINT64_MAX / unit ? UINT64_MAX : value * unit;
                const auto now = mach_now();
                if (change.fflags & 0x8) {   // NOTE_ABSOLUTE
                    if (change.fflags & (0x100 | 0x80)) change.due = nanoseconds;
                    else {
                        const auto wall = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
                        change.due = now + (nanoseconds > wall ? nanoseconds - wall : 0);
                    }
                    change.period = 0;
                } else if (std::bit_cast<std::int64_t>(value) < 0) {
                    // A negative interval fires at once, once, periodic or not
                    // (XNU's filt_timervalidate, design): MessagePumpKqueue arms
                    // an overdue wake-up as (wakeup - now) in microseconds.
                    change.due = 0; change.period = 0;
                } else {
                    change.due = nanoseconds > UINT64_MAX - now ? UINT64_MAX : now + nanoseconds;
                    change.period = (change.flags & 0x10) ? 0 : std::max<std::uint64_t>(nanoseconds, 1);
                }
            }
            else if (!error && (change.flags & 1) && machport) error = mach->attach(static_cast<std::uint32_t>(change.ident));
            else if (!error && proc) {}
            else if (!error && (change.flags & 1)) {
                try {
                    change.record = descriptor_file(static_cast<int>(change.ident));
                    if (vnode) {
                        // A file or directory (not a socket, pipe, queue or device).
                        if (!change.record || change.record->queue || change.record->socket || change.record->pipe ||
                            change.record->random_device || change.record->null_device || change.record->standard >= 0 ||
                            change.record->path.empty()) error = einval;
                        else change.vnode = vnode_watch(change.record);
                    } else {
                        if (change.record && (change.record->queue || change.record->directory)) error = 45;
                        if (change.filter == -1 && (!change.record || change.record->standard >= 0)) error = 45;
                    }
                }
                catch (const ServiceError& failure) { error = failure.value; }
            }
            if (!error) {
                std::lock_guard lock(queue->gate); if (queue_record->closed) throw ServiceError{ebadf};
                const auto key = std::pair(change.ident, change.filter); auto found = queue->watches.find(key);
                if (change.flags & 2) { if (found == queue->watches.end()) error = enoent; else queue->watches.erase(found); }
                else if (change.flags & 1) {
                    change.enabled = !(change.flags & 8); change.serial = ++queue->serial;
                    queue->watches.insert_or_assign(key, change);
                } else if (found == queue->watches.end()) error = enoent;
                else { if (change.flags & 4) found->second.enabled = true; if (change.flags & 8) found->second.enabled = false; }
            }
            if (error || (change.flags & 0x40)) {
                if (produced < static_cast<std::uint64_t>(capacity)) emit(change, 0x4000, error);
                else if (error) throw ServiceError{error};
                else throw ServiceError{einval};
            }
        }
        if (produced || !capacity) return produced;
        for (;;) {
            if (queue_record->closed) throw ServiceError{ebadf}; if (context.requested_exit()) throw ServiceError{89};
            const auto epoch = mach ? mach->epoch() : 0;
            std::vector<NetworkQueue::Watch> watches;
            { std::lock_guard lock(queue->gate); for (const auto& [key, watch] : queue->watches) { static_cast<void>(key); if (watch.enabled) watches.push_back(watch); } }
            bool watching_ports = false;
            for (const auto& watch : watches) {
                if (watch.filter == -8) {
                    watching_ports = true;
                    if (!mach) continue;
                    {
                        std::lock_guard lock(queue->gate); const auto found = queue->watches.find({watch.ident, watch.filter});
                        if (found == queue->watches.end() || found->second.serial != watch.serial || !found->second.enabled) continue;
                    }
                    // EV_CLEAR in detection mode: only a message newer than the last one reported.
                    const bool edge = (watch.flags & 0x20) && !(watch.fflags & 0x2);
                    if (edge && watch.cleared && !mach->newer(static_cast<std::uint32_t>(watch.ident), watch.cleared_data)) continue;
                    const auto event = mach->poll(m, static_cast<std::uint32_t>(watch.ident), watch.fflags, watch.ext0, watch.ext1);
                    if (!event) continue;
                    {
                        std::lock_guard lock(queue->gate); const auto found = queue->watches.find({watch.ident, watch.filter});
                        if (found != queue->watches.end() && found->second.serial == watch.serial) {
                            if (edge) { found->second.cleared = true; found->second.cleared_data = event->stamp; }
                            if (watch.flags & 0x10) queue->watches.erase(found);
                            else if (watch.flags & 0x80) found->second.enabled = false;
                        }
                    }
                    emit(watch, static_cast<std::uint16_t>(watch.flags & (0x10 | 0x20 | 0x80)), event->data, event->fflags, event->ext1);
                    if (produced == static_cast<std::uint64_t>(capacity)) break;
                    continue;
                }
                if (watch.filter == -4) {
                    // EVFILT_VNODE: notes accumulate until delivered; EV_CLEAR
                    // (and EV_ONESHOT) reset them, otherwise they stay active.
                    if (!watch.vnode) continue;
                    if (watch.record && watch.record->closed) {
                        std::lock_guard lock(queue->gate); const auto found = queue->watches.find({watch.ident, watch.filter});
                        if (found != queue->watches.end() && found->second.serial == watch.serial) queue->watches.erase(found);   // close drops the knote
                        continue;
                    }
                    const auto notes = vnode_poll(*watch.vnode);
                    std::uint32_t delivered = 0;
                    {
                        std::lock_guard lock(queue->gate); const auto found = queue->watches.find({watch.ident, watch.filter});
                        if (found == queue->watches.end() || found->second.serial != watch.serial || !found->second.enabled) continue;
                        std::lock_guard state_lock(watch.vnode->gate);
                        watch.vnode->pending |= notes & watch.fflags;
                        delivered = watch.vnode->pending;
                        if (!delivered) continue;
                        if (watch.flags & (0x20 | 0x10)) watch.vnode->pending = 0;
                        if (watch.flags & 0x10) queue->watches.erase(found);
                        else if (watch.flags & 0x80) found->second.enabled = false;
                    }
                    emit(watch, static_cast<std::uint16_t>(watch.flags & (0x10 | 0x20 | 0x80)), 0, delivered);
                    if (produced == static_cast<std::uint64_t>(capacity)) break;
                    continue;
                }
                if (watch.filter == -5) {
                    // EVFILT_PROC NOTE_EXIT: once, when the child has ended (or
                    // was reaped meanwhile); data is its wait status with
                    // NOTE_EXITSTATUS. The knote ends with EV_EOF | EV_ONESHOT.
                    const auto status = child_status(static_cast<std::uint32_t>(watch.ident));
                    if (status && !*status) continue;
                    {
                        std::lock_guard lock(queue->gate); const auto found = queue->watches.find({watch.ident, watch.filter});
                        if (found == queue->watches.end() || found->second.serial != watch.serial || !found->second.enabled) continue;
                        queue->watches.erase(found);
                    }
                    const auto data = (watch.fflags & 0x04000000U) && status ? **status : 0U;
                    emit(watch, static_cast<std::uint16_t>((watch.flags & (0x20 | 0x80)) | 0x10 | 0x8000), data, watch.fflags & 0x84000000U);
                    if (produced == static_cast<std::uint64_t>(capacity)) break;
                    continue;
                }
                if (watch.filter == -7) {
                    const auto now = mach_now();
                    if (now < watch.due) continue;
                    std::uint64_t expirations = 1;
                    {
                        std::lock_guard lock(queue->gate); const auto found = queue->watches.find({watch.ident, watch.filter});
                        if (found == queue->watches.end() || found->second.serial != watch.serial || !found->second.enabled) continue;
                        if (watch.period) {
                            expirations += (now - watch.due) / watch.period;
                            found->second.due = watch.due + expirations * watch.period;
                        }
                        if ((watch.flags & 0x10) || !watch.period) {
                            if (watch.flags & 0x10) queue->watches.erase(found);
                            else found->second.enabled = false;   // an absolute or overdue timer fires once
                        } else if (watch.flags & 0x80) found->second.enabled = false;
                    }
                    emit(watch, static_cast<std::uint16_t>(watch.flags & (0x10 | 0x20 | 0x80)), expirations, 0);
                    if (produced == static_cast<std::uint64_t>(capacity)) break;
                    continue;
                }
                auto ready = network_ready(watch.record, watch.filter == -1 ? 1 : 4);
                // EVFILT_WRITE's EOF is "this side cannot send any more" (a local
                // SHUT_WR, or an error). Winsock reports POLLHUP as soon as the
                // peer shuts down its sending side (a FIN, e.g. crashpad's
                // one-way socketpair), when this side can still send.
                if (watch.filter == -2 && (ready & 16) && !(ready & 8) && watch.record && watch.record->socket &&
                    !watch.record->socket->send_shut)
                    ready = static_cast<std::uint16_t>(ready & ~16);
                const bool available = (ready & (watch.filter == -1 ? (1 | 8 | 16 | 32) : (4 | 8 | 16 | 32))) != 0;
                // Write data1 is a conservative capacity hint from native
                // writable readiness. This adapter does not claim exact
                // kernel send-buffer accounting.
                const auto data = !available ? 0ULL : (ready & 32) ? ebadf : watch.filter == -1 ? network_read_hint(watch.record) : (ready & 4) ? 1ULL : 0ULL;
                const auto state = data | ((ready & 16) ? (1ULL << 63) : 0);   // EOF is part of the state
                bool deliver = false;
                {
                    std::lock_guard lock(queue->gate); const auto found = queue->watches.find({watch.ident, watch.filter});
                    if (found == queue->watches.end() || found->second.serial != watch.serial || !found->second.enabled) continue;
                    if (!available) found->second.cleared = false;   // EV_CLEAR: the lapse re-arms the edge
                    else if (!((watch.flags & 0x20) && found->second.cleared && found->second.cleared_data == state)) {
                        deliver = true;
                        if (watch.flags & 0x20) { found->second.cleared = true; found->second.cleared_data = state; }
                        if ((watch.flags & 0x10) || (ready & 32)) queue->watches.erase(found);
                        else if (watch.flags & 0x80) found->second.enabled = false;
                    }
                }
                if (deliver) {
                    emit(watch, static_cast<std::uint16_t>((watch.flags & (0x10 | 0x20 | 0x80)) | ((ready & 32) ? 0x4000 : (ready & 16) ? 0x8000 : 0)), data);
                    if (produced == static_cast<std::uint64_t>(capacity)) break;
                }
            }
            if (produced || std::chrono::steady_clock::now() >= deadline) return produced;
            // A queued Mach message ends the wait at once; descriptors are polled.
            if (watching_ports && mach) mach->wait(epoch, std::chrono::milliseconds(1));
            else std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    install("_kevent", [run_kevent](auto& s, auto& m) { return run_kevent(s, m, false, s.x[5], false); });
    // kevent64(kq, changelist, nchanges, eventlist, nevents, flags, timeout):
    // KEVENT_FLAG_IMMEDIATE (1) only.
    install("_kevent64", [run_kevent](auto& s, auto& m) {
        if (static_cast<std::uint32_t>(s.x[5]) & ~1U) throw ServiceError{einval};
        return run_kevent(s, m, true, s.x[6], (s.x[5] & 1U) != 0);
    });
    for (const auto setting : {false, true}) install(setting ? "_setsockopt" : "_getsockopt", [this, setting](auto& s, auto& m) {
        const auto record = network_file(static_cast<int>(s.x[0])); const auto socket = record->socket;
        const auto level = static_cast<int>(s.x[1]), option = static_cast<int>(s.x[2]);
        std::uint32_t size;
        if (setting) size = static_cast<std::uint32_t>(s.x[4]);
        else { validate(m, s.x[4], 4, true); validate(m, s.x[4], 4, false); size = m.read32(s.x[4]); }
        const bool timeout = level == 0xffff && (option == 0x1005 || option == 0x1006);
        const bool no_sigpipe = level == 0xffff && option == 0x1022;
        const bool linger_option = level == 0xffff && option == 0x80;
        const auto needed = timeout ? 16U : linger_option ? 8U : 4U;
        if (setting && size < needed) throw ServiceError{einval};
        validate(m, s.x[3], setting ? needed : std::min(size, needed), !setting);
        std::vector<std::uint8_t> output(needed);
        std::lock_guard lock(socket->gate); if (record->closed) throw ServiceError{ebadf};
        if (timeout) {
            auto& interval = option == 0x1005 ? socket->send_timeout : socket->receive_timeout;
            if (setting) interval = network_timeval(m, s.x[3]);
            else {
                const auto seconds = static_cast<std::uint64_t>(interval.count() / 1000000);
                const auto micros = static_cast<std::uint32_t>(interval.count() % 1000000);
                for (unsigned index = 0; index < 8; ++index) output[index] = static_cast<std::uint8_t>(seconds >> (index * 8));
                for (unsigned index = 0; index < 4; ++index) output[8 + index] = static_cast<std::uint8_t>(micros >> (index * 8));
            }
        } else if (no_sigpipe) {
            if (setting) socket->no_sigpipe = m.read32(s.x[3]) != 0;
            else output[0] = socket->no_sigpipe ? 1 : 0;
        } else if (linger_option) {
            if (setting) {
                const auto seconds = m.read32(s.x[3] + 4); if (seconds > UINT16_MAX) throw ServiceError{einval};
                // Positive linger can make nonblocking Windows close fail
                // without releasing the handle. Only ordinary close and
                // immediate abortive close are supported in this profile.
                if (m.read32(s.x[3]) && seconds) throw ServiceError{45};
                const auto done = net::set_linger(socket->handle, {m.read32(s.x[3]) != 0, static_cast<std::uint16_t>(seconds)});
                if (!done) throw ServiceError{network_error(done.error())};
            } else {
                const auto value = net::get_linger(socket->handle);
                if (!value) throw ServiceError{network_error(value.error())};
                output[0] = value->on ? 1 : 0;
                for (unsigned index = 0; index < 4; ++index) output[4 + index] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(value->seconds) >> (index * 8));
            }
        } else {
            int value = setting ? static_cast<int>(m.read32(s.x[3])) : 0;
            if (level == 0xffff && (option == 0x1007 || option == 0x1008)) {
                // SO_ERROR (the pending error, translated; reading clears it) and SO_TYPE are read-only.
                if (setting) throw ServiceError{42};
                if (option == 0x1007) {
                    const auto pending = net::pending_error(socket->handle);
                    if (!pending) throw ServiceError{network_error(pending.error())};
                    value = pending->error == host::Error::none ? 0 : static_cast<int>(network_error(pending->error));
                    socket->connecting = false;
                } else value = socket->type;
            } else {
                const auto native_option = native_socket_option(level, option);
                if (setting) {
                    if (const auto done = net::set_option(socket->handle, native_option, value); !done) throw ServiceError{network_error(done.error())};
                } else {
                    const auto read = net::get_option(socket->handle, native_option);
                    if (!read) throw ServiceError{network_error(read.error())};
                    value = *read;
                }
            }
            for (unsigned index = 0; index < 4; ++index) output[index] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(value) >> (index * 8));
        }
        if (!setting) { m.write_bytes(s.x[3], std::span(output.data(), std::min<std::size_t>(size, output.size()))); m.write32(s.x[4], std::min(size, needed)); }
        return std::uint64_t{0};
    });
    install("_gethostname", [](auto& s, auto& m) {
        if (!s.x[1]) throw ServiceError{einval}; validate(m, s.x[0], s.x[1], true);
        const auto value = net::host_name(); if (!value) throw ServiceError{network_error(value.error())};
        if (value->size() >= s.x[1]) throw ServiceError{63}; put_text(m, s.x[0], *value); return std::uint64_t{0};
    });
    install("_inet_pton", [](auto& s, auto& m) {
        const auto family = host_family(static_cast<int>(s.x[0])); if (family == net::Family::unspecified) throw ServiceError{47};
        const auto input = text(m, s.x[1]); const auto size = family == net::Family::ipv4 ? 4U : 16U;
        validate(m, s.x[2], size, true);
        // A bare literal of the family: no brackets or zone.
        const auto parsed = input.find_first_of("[]%") == std::string::npos ? net::parse_numeric(input) : std::nullopt;
        if (!parsed || parsed->family != family) return std::uint64_t{0};
        m.write_bytes(s.x[2], std::span(parsed->bytes.data(), size)); return std::uint64_t{1};
    });
    install("_inet_ntop", [](auto& s, auto& m) {
        const auto family = host_family(static_cast<int>(s.x[0])); if (family == net::Family::unspecified) throw ServiceError{47};
        const auto size = family == net::Family::ipv4 ? 4U : 16U; std::array<std::uint8_t, 16> bytes{};
        validate(m, s.x[1], size, false); m.read_bytes(s.x[1], std::span(bytes.data(), size));
        const auto output = net::host_text(family == net::Family::ipv4 ? net::Address::ipv4({bytes[0], bytes[1], bytes[2], bytes[3]}) : net::Address::ipv6(bytes));
        if (output.size() >= s.x[3]) throw ServiceError{enospc};
        validate(m, s.x[2], output.size() + 1, true); put_text(m, s.x[2], output); return s.x[2];
    }, 0);
    // in_addr_t inet_addr(const char*): the address in network order, INADDR_NONE when invalid.
    install("_inet_addr", [](auto& s, auto& m) {
        const auto value = inet_aton_value(text(m, s.x[0]));
        if (!value) return std::uint64_t{UINT32_MAX};
        return static_cast<std::uint64_t>((*value >> 24) | ((*value >> 8) & 0xff00U) | ((*value << 8) & 0xff0000U) | (*value << 24));
    }, UINT32_MAX);
    install("_inet_ntoa", [this](auto& s, auto& m) {
        const auto bits = static_cast<std::uint32_t>(s.x[0]);   // network order in memory
        const auto output = net::host_text(net::Address::ipv4({static_cast<std::uint8_t>(bits), static_cast<std::uint8_t>(bits >> 8),
                                                               static_cast<std::uint8_t>(bits >> 16), static_cast<std::uint8_t>(bits >> 24)}));
        const auto key = "inet_ntoa:" + std::to_string(context.current_thread()->identity);
        std::uint64_t result; { std::lock_guard lock(mutex); auto& slot = network_strings[key]; if (!slot) slot = context.allocate_guest(32); result = slot; }
        put_text(m, result, output.data()); return result;
    }, 0);
    install("_getaddrinfo", [this](auto& s, auto& m) {
        validate(m, s.x[3], 8, true); m.write64(s.x[3], 0);
        const auto node = s.x[0] ? text(m, s.x[0]) : std::string{};
        const auto service = s.x[1] ? text(m, s.x[1]) : std::string{};
        net::Hints hints; std::uint32_t flags = 0;
        if (s.x[2]) {
            validate(m, s.x[2], 48, false); flags = m.read32(s.x[2]);
            try { hints.family = host_family(static_cast<int>(m.read32(s.x[2] + 4))); } catch (const ServiceError&) { return std::uint64_t{5}; }
            const auto type = static_cast<int>(m.read32(s.x[2] + 8)); hints.protocol = static_cast<int>(m.read32(s.x[2] + 12));
            try { hints.flags = resolver_flags(flags); } catch (const ServiceError&) { return std::uint64_t{3}; }
            if (type && type != 1 && type != 2) return std::uint64_t{10};
            hints.type = type == 1 ? net::SocketType::stream : type == 2 ? net::SocketType::datagram : net::SocketType::unspecified;
        }
        const bool has_node = s.x[0] != 0, has_service = s.x[1] != 0;
        if (has_node && network_policy::loopback_only() && !network_policy::loopback_host(node) && !network_policy::numeric_host(node)) {
            network_policy::deny("getaddrinfo", node);
            return std::uint64_t{8};   // EAI_NONAME
        }
        if (has_node && network_policy::refuse_listed_host("getaddrinfo", node)) return std::uint64_t{8};   // ALCHEMY_NETWORK_DENY
        if (has_node) network_policy::allowed("getaddrinfo", node);
        const auto query = query_network<std::vector<net::Resolved>>(context, network_session, [node, service, hints, has_node, has_service](auto& values) {
            auto resolved = net::resolve(has_node ? std::optional<std::string_view>(node) : std::nullopt,
                                         has_service ? std::optional<std::string_view>(service) : std::nullopt, hints);
            if (!resolved) return resolver_error(resolved.error());
            if (resolved->size() > 256) return std::uint32_t{6};
            values = std::move(*resolved);
            return 0U;
        });
        if (!query) return std::uint64_t{2}; if (query->error) return static_cast<std::uint64_t>(query->error);
        std::uint64_t size = query->value.size() * 48;
        for (const auto& entry : query->value) size += 32 + entry.canonical_name.size() + 1;
        const auto result = allocate(size); std::uint64_t cursor = result + query->value.size() * 48;
        try {
            m.fill_bytes(result, 0, size);
            for (std::size_t index = 0; index < query->value.size(); ++index) {
                const auto& entry = query->value[index]; const auto at = result + index * 48;
                const auto address = encode_network_address(entry.address);
                const auto type = entry.type == net::SocketType::stream ? 1U : entry.type == net::SocketType::datagram ? 2U : 0U;
                m.write32(at, flags); m.write32(at + 4, static_cast<std::uint32_t>(guest_family(entry.address.family)));
                m.write32(at + 8, type); m.write32(at + 12, static_cast<std::uint32_t>(entry.protocol));
                m.write32(at + 16, static_cast<std::uint32_t>(address.size())); m.write64(at + 32, cursor); m.write_bytes(cursor, address); cursor += 32;
                if (!entry.canonical_name.empty()) { m.write64(at + 24, cursor); put_text(m, cursor, entry.canonical_name); cursor += entry.canonical_name.size() + 1; }
                if (index + 1 < query->value.size()) m.write64(at + 40, at + 48);
            }
            { std::lock_guard lock(mutex); network_results.insert(result); } m.write64(s.x[3], result);
        } catch (...) { { std::lock_guard lock(mutex); network_results.erase(result); } release(result); throw; }
        return std::uint64_t{0};
    }, 11);
    install("_freeaddrinfo", [this](auto& s, auto&) {
        if (!s.x[0]) return std::uint64_t{0};
        { std::lock_guard lock(mutex); if (!network_results.erase(s.x[0])) throw ServiceError{einval}; }
        release(s.x[0]); return std::uint64_t{0};
    });
    install("_getnameinfo", [this](auto& s, auto& m) {
        const auto address = read_network_address(m, s.x[0], s.x[1]);
        if (s.x[2]) validate(m, s.x[2], s.x[3], true); if (s.x[4]) validate(m, s.x[4], s.x[5], true);
        const auto flags = static_cast<std::uint32_t>(s.x[6]); if (flags & ~31U) return std::uint64_t{3};
        // The Darwin NI_* flags are the host's name_* flags of the same values.
        auto native_flags = flags;
        if (network_policy::loopback_only() && !(flags & 2) && !network_policy::loopback_address(address)) {
            if (flags & 4) {   // NI_NAMEREQD: a name is required and cannot be looked up
                network_policy::deny("getnameinfo", network_policy::address_text(address));
                return std::uint64_t{8};   // EAI_NONAME
            }
            native_flags |= net::name_numeric_host;   // the numeric form, as when a reverse lookup finds nothing
        }
        const auto query = query_network<std::array<std::string, 2>>(context, network_session, [address, native_flags](auto& values) {
            auto name = net::name_info(address, native_flags);
            if (!name) return resolver_error(name.error());
            values[0] = std::move(name->host); values[1] = std::move(name->service); return 0U;
        });
        if (!query) return std::uint64_t{2}; if (query->error) return static_cast<std::uint64_t>(query->error);
        if ((s.x[2] && query->value[0].size() >= s.x[3]) || (s.x[4] && query->value[1].size() >= s.x[5])) return std::uint64_t{14};
        if (s.x[2]) put_text(m, s.x[2], query->value[0]); if (s.x[4]) put_text(m, s.x[4], query->value[1]); return std::uint64_t{0};
    }, 11);
    install("_sethostname", [](auto&, auto&) -> std::uint64_t { throw ServiceError{eperm}; });
    for (const auto host_error_text : {false, true}) install(host_error_text ? "_hstrerror" : "_gai_strerror", [this, host_error_text](auto& s, auto& m) {
        static constexpr std::array<std::string_view, 15> messages{"Success", "Address family not supported", "Temporary name resolution failure", "Invalid resolver flags", "Name resolution failure", "Unsupported address family", "Resolver out of memory", "No address data", "Name or service not known", "Service not supported", "Socket type not supported", "System resolver error", "Invalid resolver hints", "Unsupported protocol", "Resolver output too small"};
        static constexpr std::array<std::string_view, 5> hosts{"Success", "Unknown host", "Temporary resolver failure", "Unrecoverable resolver error", "No address data"};
        const auto error = static_cast<std::uint32_t>(s.x[0]); const auto message = host_error_text ? (error < hosts.size() ? hosts[error] : "Unknown resolver error") : (error < messages.size() ? messages[error] : "Unknown resolver error");
        const auto key = std::string(host_error_text ? "herror:" : "gaierror:") + std::to_string(error); std::uint64_t result;
        { std::lock_guard lock(mutex); auto& entry = network_strings[key]; if (!entry) entry = context.allocate_guest(message.size() + 1); result = entry; }
        put_text(m, result, message); return result;
    }, 0);
    const auto legacy_buffer = [this](std::string_view kind) {
        const auto key = std::string(kind) + ':' + std::to_string(context.current_thread()->identity);
        std::lock_guard lock(mutex); auto& address = network_strings[key]; if (!address) address = context.allocate_guest(8192); return address;
    };
    for (const auto reverse : {false, true}) install(reverse ? "_gethostbyaddr" : "_gethostbyname", [this, reverse, legacy_buffer](auto& s, auto& m) {
        std::string name; net::Address address;
        if (reverse) {
            const auto family = host_family(static_cast<int>(s.x[2])); const auto length = family == net::Family::ipv4 ? 4U : family == net::Family::ipv6 ? 16U : 0U;
            if (!length || s.x[1] != length) throw ServiceError{einval};
            validate(m, s.x[0], length, false);
            std::array<std::uint8_t, 16> bytes{}; m.read_bytes(s.x[0], std::span(bytes.data(), length));
            address = family == net::Family::ipv4 ? net::Address::ipv4({bytes[0], bytes[1], bytes[2], bytes[3]}) : net::Address::ipv6(bytes);
        } else name = text(m, s.x[0]);
        if (network_policy::loopback_only()) {
            bool local = false;
            std::string target = name;
            if (reverse) {
                local = network_policy::loopback_address(address);
                target = network_policy::address_text(address);
            } else local = network_policy::loopback_host(name) || network_policy::numeric_host(name);
            if (!local) {
                network_policy::deny(reverse ? "gethostbyaddr" : "gethostbyname", target);
                m.write32(network_h_errno, 1);   // HOST_NOT_FOUND
                return std::uint64_t{0};
            }
        }
        if (!reverse && network_policy::refuse_listed_host("gethostbyname", name)) {   // ALCHEMY_NETWORK_DENY
            m.write32(network_h_errno, 1);   // HOST_NOT_FOUND
            return std::uint64_t{0};
        }
        if (!reverse) network_policy::allowed("gethostbyname", name);
        const auto query = query_network<LegacyHost>(context, network_session, [reverse, name, address](auto& host) {
            // The result is copied entirely on this resolver thread; no guest
            // state crosses the callback boundary.
            auto value = reverse ? net::host_by_address(address) : net::host_by_name(name);
            if (!value) return legacy_resolver_error(value.error());
            host.name = value->name.empty() ? name : value->name; host.aliases = std::move(value->aliases);
            host.family = guest_family(value->addresses.front().family);
            host.length = value->addresses.front().family == net::Family::ipv4 ? 4 : 16;
            for (const auto& entry : value->addresses)
                host.addresses.emplace_back(entry.bytes.begin(), entry.bytes.begin() + host.length);
            return 0U;
        });
        if (!query || query->error) { m.write32(network_h_errno, query ? (query->error <= 4 ? query->error : 3U) : 2U); return std::uint64_t{0}; }
        const auto& host = query->value; std::uint64_t size = 32 + (host.aliases.size() + host.addresses.size() + 2) * 8 + host.name.size() + 1;
        for (const auto& alias : host.aliases) size += alias.size() + 1;
        size += static_cast<std::uint64_t>(host.length) * host.addresses.size(); if (size > 8192) throw ServiceError{enomem};
        const auto result = legacy_buffer("hostent"); m.fill_bytes(result, 0, size);
        const auto aliases = result + 32, addresses = aliases + (host.aliases.size() + 1) * 8;
        auto cursor = addresses + (host.addresses.size() + 1) * 8;
        m.write64(result, cursor); put_text(m, cursor, host.name); cursor += host.name.size() + 1;
        m.write64(result + 8, aliases); m.write32(result + 16, static_cast<std::uint32_t>(host.family)); m.write32(result + 20, static_cast<std::uint32_t>(host.length)); m.write64(result + 24, addresses);
        for (std::size_t index = 0; index < host.aliases.size(); ++index) { m.write64(aliases + index * 8, cursor); put_text(m, cursor, host.aliases[index]); cursor += host.aliases[index].size() + 1; }
        for (std::size_t index = 0; index < host.addresses.size(); ++index) { m.write64(addresses + index * 8, cursor); m.write_bytes(cursor, host.addresses[index]); cursor += host.addresses[index].size(); }
        m.write32(network_h_errno, 0); return result;
    }, 0);
    for (const auto by_port : {false, true}) install(by_port ? "_getservbyport" : "_getservbyname", [this, by_port, legacy_buffer](auto& s, auto& m) {
        const auto name = by_port ? std::string{} : text(m, s.x[0]); const auto protocol = s.x[1] ? text(m, s.x[1]) : std::string{};
        const auto port = swap16(s.x[0]); const auto has_protocol = s.x[1] != 0;   // the argument is in network order
        const auto query = query_network<LegacyService>(context, network_session, [by_port, name, protocol, port, has_protocol](auto& result) {
            const auto wanted = has_protocol ? std::optional<std::string_view>(protocol) : std::nullopt;
            auto value = by_port ? net::service_by_port(port, wanted) : net::service_by_name(name, wanted);
            if (!value) return 1U; result.name = std::move(value->name); result.protocol = std::move(value->protocol);
            result.port = swap16(value->port); result.aliases = std::move(value->aliases); return 0U;
        });
        if (!query || query->error) return std::uint64_t{0}; const auto& value = query->value;
        std::uint64_t size = 32 + (value.aliases.size() + 1) * 8 + value.name.size() + value.protocol.size() + 2;
        for (const auto& alias : value.aliases) size += alias.size() + 1; if (size > 8192) throw ServiceError{enomem};
        const auto result = legacy_buffer("servent"); m.fill_bytes(result, 0, size); const auto aliases = result + 32; auto cursor = aliases + (value.aliases.size() + 1) * 8;
        m.write64(result, cursor); put_text(m, cursor, value.name); cursor += value.name.size() + 1;
        m.write64(result + 8, aliases); m.write32(result + 16, static_cast<std::uint32_t>(value.port)); m.write64(result + 24, cursor); put_text(m, cursor, value.protocol); cursor += value.protocol.size() + 1;
        for (std::size_t index = 0; index < value.aliases.size(); ++index) { m.write64(aliases + index * 8, cursor); put_text(m, cursor, value.aliases[index]); cursor += value.aliases[index].size() + 1; } return result;
    }, 0);
    install("_getprotobyname", [this, legacy_buffer](auto& s, auto& m) {
        const auto name = text(m, s.x[0]);
        const auto query = query_network<LegacyProtocol>(context, network_session, [name](auto& result) {
            auto value = net::protocol_by_name(name); if (!value) return 1U;
            result.name = std::move(value->name); result.protocol = value->number; result.aliases = std::move(value->aliases); return 0U;
        });
        if (!query || query->error) return std::uint64_t{0}; const auto& value = query->value;
        std::uint64_t size = 24 + (value.aliases.size() + 1) * 8 + value.name.size() + 1;
        for (const auto& alias : value.aliases) size += alias.size() + 1; if (size > 8192) throw ServiceError{enomem};
        const auto result = legacy_buffer("protoent"); m.fill_bytes(result, 0, size); const auto aliases = result + 24; auto cursor = aliases + (value.aliases.size() + 1) * 8;
        m.write64(result, cursor); put_text(m, cursor, value.name); cursor += value.name.size() + 1; m.write64(result + 8, aliases); m.write32(result + 16, static_cast<std::uint32_t>(value.protocol));
        for (std::size_t index = 0; index < value.aliases.size(); ++index) { m.write64(aliases + index * 8, cursor); put_text(m, cursor, value.aliases[index]); cursor += value.aliases[index].size() + 1; } return result;
    }, 0);
    install("_if_nameindex", [this](auto&, auto& m) {
        const auto values = network_interfaces(); const auto size = (values.size() + 1) * 16 + values.size() * 16;
        const auto result = allocate(size); auto cursor = result + (values.size() + 1) * 16;
        try {
            m.fill_bytes(result, 0, size);
            for (std::size_t index = 0; index < values.size(); ++index) {
                if (values[index].name.size() >= 16) throw ServiceError{63};
                m.write32(result + index * 16, values[index].index); m.write64(result + index * 16 + 8, cursor);
                put_text(m, cursor, values[index].name); cursor += 16;
            }
            std::lock_guard lock(mutex); network_interface_results.insert(result);
        } catch (...) { release(result); throw; } return result;
    }, 0);
    install("_if_freenameindex", [this](auto& s, auto&) {
        if (!s.x[0]) return std::uint64_t{0};
        { std::lock_guard lock(mutex); if (!network_interface_results.erase(s.x[0])) throw ServiceError{einval}; }
        release(s.x[0]); return std::uint64_t{0};
    });
    install("_if_nametoindex", [](auto& s, auto& m) {
        const auto name = text(m, s.x[0], 16);
        for (const auto& entry : network_interfaces()) if (entry.name == name) return static_cast<std::uint64_t>(entry.index);
        throw ServiceError{6};
    }, 0);
    install("_if_indextoname", [](auto& s, auto& m) {
        validate(m, s.x[1], 16, true);
        for (const auto& entry : network_interfaces()) if (entry.index == s.x[0]) { put_text(m, s.x[1], entry.name); return s.x[1]; }
        throw ServiceError{6};
    }, 0);
    install("_socketpair", [this](auto& s, auto& m) {
        if (s.x[0] != 1) throw ServiceError{47}; if (s.x[1] != 1 && s.x[1] != 2) throw ServiceError{44};
        if (s.x[2]) throw ServiceError{43}; validate(m, s.x[3], 8, true);
        const auto type = static_cast<int>(s.x[1]);
        std::shared_ptr<NetworkSocket> first, second;
        if (type == 1) std::tie(first, second) = connected_stream_pair();
        else {
            first = create_socket(2, type, 0); second = create_socket(2, type, 0);
            const auto check = [](const host::Status& status) { if (!status) throw ServiceError{network_error(status.error())}; };
            const auto loop = net::Address::ipv4({127, 0, 0, 1});
            check(net::bind(first->handle, loop));
            const auto first_address = net::local_address(first->handle);
            if (!first_address) throw ServiceError{network_error(first_address.error())};
            check(net::bind(second->handle, loop));
            const auto second_address = net::local_address(second->handle);
            if (!second_address) throw ServiceError{network_error(second_address.error())};
            check(net::connect(first->handle, *second_address));
            check(net::connect(second->handle, *first_address));
            first->domain = second->domain = 1;
        }
        const auto first_fd = add_socket(first);
        int second_fd = -1;
        try { second_fd = add_socket(second); m.write32(s.x[3], static_cast<std::uint32_t>(first_fd)); m.write32(s.x[3] + 4, static_cast<std::uint32_t>(second_fd)); }
        catch (...) { if (second_fd >= 0) static_cast<void>(close(second_fd)); static_cast<void>(close(first_fd)); throw; }
        return std::uint64_t{0};
    });
    // sendmsg/recvmsg move the data of msg_iov (and msg_name as sendto/recvfrom
    // do). Ancillary data is outside this profile: sending a control message
    // (SCM_RIGHTS descriptor passing, SCM_CREDS) fails with EOPNOTSUPP, and a
    // receive never returns one (msg_controllen 0).
    for (const auto sending : {false, true}) install(sending ? "_sendmsg" : "_recvmsg", [this, sending](auto& s, auto& m) {
        return network_message(m, static_cast<int>(s.x[0]), s.x[1], s.x[2], sending);
    });
    // int sendfile(int fd, int s, off_t offset, off_t* len, struct sf_hdtr*, int flags):
    // fd's bytes from offset sent on the stream socket s; *len is how many (0:
    // to the end of the file) and, on return, how many were sent (also when
    // the call fails part of the way). A descriptor that is not a socket fails
    // with ENOTSOCK, so callers copy another way (libuv's fs.copyFile does).
    // Headers and trailers are not provided (EOPNOTSUPP when given).
    install("_sendfile", [this](auto& s, auto& m) {
        const auto out = descriptor_file(static_cast<int>(s.x[1]));
        if (!out || !out->socket) throw ServiceError{38};          // ENOTSOCK
        if (out->socket->type != 1) throw ServiceError{45};        // a stream socket only (ENOTSUP)
        if (!s.x[3]) throw ServiceError{einval};
        validate(m, s.x[3], 8, true);
        if (s.x[4]) {
            validate(m, s.x[4], 32, false);
            if (m.read32(s.x[4] + 8) || m.read32(s.x[4] + 24)) throw ServiceError{102};   // EOPNOTSUPP: headers or trailers
        }
        const auto offset = std::bit_cast<std::int64_t>(s.x[2]);
        if (offset < 0) throw ServiceError{einval};
        const auto wanted = m.read64(s.x[3]);
        constexpr std::uint64_t chunk = 65536;
        const auto buffer = context.allocate_guest(chunk);
        std::uint64_t sent = 0;
        try {
            while (!wanted || sent < wanted) {
                const auto size = wanted ? std::min(chunk, wanted - sent) : chunk;
                const auto got = positioned_io(m, static_cast<int>(s.x[0]), buffer, size, static_cast<std::uint64_t>(offset) + sent, false);
                if (!got) break;
                for (std::uint64_t done = 0; done < got;) {
                    const auto written = write(m, static_cast<int>(s.x[1]), buffer + done, got - done);
                    done += written;
                    sent += written;
                }
            }
        } catch (...) {
            m.write64(s.x[3], sent);
            context.release_guest(buffer);
            throw;
        }
        m.write64(s.x[3], sent);
        context.release_guest(buffer);
        return std::uint64_t{0};
    });
}

} // namespace alchemy
