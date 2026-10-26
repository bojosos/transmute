#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/host/net.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// libSystem networking pieces a renderer plug-in's ZeroMQ/HTTP clients import: interface
// enumeration (getifaddrs over GetAdaptersAddresses), pselect, connectx,
// getprotobynumber, inet_net_pton and in6addr_any; disconnectx and the classic
// inet_aton/inet_network/inet_makeaddr/inet_lnaof/inet_netof. Structure layouts follow
// the Darwin headers (<ifaddrs.h>, <net/if_dl.h>, <net/if_var.h>,
// <netinet/in.h>, <sys/socket.h>, <netdb.h>).
namespace alchemy::export_fill {
namespace {
constexpr std::uint32_t byte_swap32(std::uint32_t value) {
    return (value >> 24) | ((value >> 8) & 0xff00U) | ((value << 8) & 0xff0000U) | (value << 24);
}
} // namespace

std::vector<Interface> host_interfaces() {
    std::vector<Interface> result;
    const auto host = host::net::interfaces();
    if (!host) return result;
    unsigned ethernet = 0, tunnel = 0;
    std::vector<Interface> up, down;
    for (const auto& adapter : *host) {
        Interface entry;
        entry.index = adapter.index;
        entry.up = adapter.up;
        entry.loopback = adapter.kind == host::net::InterfaceKind::loopback;
        entry.point_to_point = adapter.kind == host::net::InterfaceKind::tunnel || adapter.kind == host::net::InterfaceKind::ppp;
        entry.multicast = adapter.multicast;
        entry.mtu = adapter.mtu;
        entry.speed = adapter.speed;
        if (adapter.hardware.size() == 6) entry.mac = adapter.hardware;
        for (const auto& assigned : adapter.addresses) {
            Interface::Address value;
            value.prefix = assigned.prefix_length;
            if (assigned.address.family == host::net::Family::ipv4) {
                value.family = 2;
                std::copy_n(assigned.address.bytes.begin(), 4, value.bytes.begin());
            } else if (assigned.address.family == host::net::Family::ipv6) {
                value.family = 30;
                value.bytes = assigned.address.bytes;
                value.scope = assigned.address.scope_id;
            } else continue;
            entry.addresses.push_back(value);
        }
        (entry.up ? up : down).push_back(std::move(entry));
    }
    for (auto* list : {&up, &down}) {
        for (auto& entry : *list) {
            if (entry.loopback) entry.name = "lo0";
            else if (entry.point_to_point) entry.name = "utun" + std::to_string(tunnel++);
            else entry.name = "en" + std::to_string(ethernet++);
            result.push_back(std::move(entry));
        }
    }
    // Darwin lists lo0 first.
    std::stable_partition(result.begin(), result.end(), [](const Interface& entry) { return entry.loopback; });
    return result;
}

namespace {

// <net/if.h>
constexpr std::uint32_t iff_up = 0x1, iff_broadcast = 0x2, iff_loopback = 0x8, iff_pointopoint = 0x10,
    iff_running = 0x40, iff_multicast = 0x8000;
// <net/if_types.h>
constexpr std::uint8_t ift_other = 0x1, ift_ether = 0x6, ift_loop = 0x18;
constexpr std::uint64_t ifaddrs_size = 56, if_data_size = 96;

class Image {
public:
    std::uint64_t reserve(std::uint64_t size) {
        const auto offset = (bytes.size() + 7) & ~std::uint64_t{7};
        bytes.resize(static_cast<std::size_t>(offset + size), 0);
        return offset;
    }
    void put8(std::uint64_t at, std::uint8_t value) { bytes[static_cast<std::size_t>(at)] = value; }
    void put16(std::uint64_t at, std::uint16_t value) { std::memcpy(&bytes[static_cast<std::size_t>(at)], &value, 2); }
    void put32(std::uint64_t at, std::uint32_t value) { std::memcpy(&bytes[static_cast<std::size_t>(at)], &value, 4); }
    // Pointer fields hold offsets until relocate().
    void pointer(std::uint64_t at, std::uint64_t target) { pointers.push_back(at); std::memcpy(&bytes[static_cast<std::size_t>(at)], &target, 8); }
    void relocate(std::uint64_t base) {
        for (const auto at : pointers) {
            std::uint64_t value{};
            std::memcpy(&value, &bytes[static_cast<std::size_t>(at)], 8);
            value += base;
            std::memcpy(&bytes[static_cast<std::size_t>(at)], &value, 8);
        }
    }
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint64_t> pointers;
};

std::uint64_t socket_address(Image& image, std::uint8_t family, std::span<const std::uint8_t> address, std::uint32_t scope = 0) {
    if (family == 2) {
        const auto at = image.reserve(16);
        image.put8(at, 16); image.put8(at + 1, 2);
        std::memcpy(&image.bytes[static_cast<std::size_t>(at + 4)], address.data(), 4);
        return at;
    }
    const auto at = image.reserve(28);
    image.put8(at, 28); image.put8(at + 1, 30);
    std::memcpy(&image.bytes[static_cast<std::size_t>(at + 8)], address.data(), 16);
    image.put32(at + 24, scope);
    return at;
}

std::array<std::uint8_t, 16> mask(unsigned family, unsigned prefix) {
    std::array<std::uint8_t, 16> value{};
    const unsigned bits = std::min(prefix, family == 2 ? 32U : 128U);
    for (unsigned bit = 0; bit < bits; ++bit) value[bit / 8] |= static_cast<std::uint8_t>(0x80U >> (bit % 8));
    return value;
}

// Serializes the list as one guest block: freeifaddrs releases it whole.
std::vector<std::uint8_t> ifaddrs_image(const std::vector<Interface>& interfaces, std::uint64_t base) {
    Image image;
    std::uint64_t previous = UINT64_MAX;
    const auto link = [&](std::uint64_t entry) {
        if (previous != UINT64_MAX) image.pointer(previous, entry);
        previous = entry;
    };
    for (const auto& item : interfaces) {
        std::uint32_t flags = 0;
        if (item.up) flags |= iff_up | iff_running;
        if (item.loopback) flags |= iff_loopback;
        else if (item.point_to_point) flags |= iff_pointopoint;
        else flags |= iff_broadcast;
        if (item.multicast) flags |= iff_multicast;
        // AF_LINK entry: sockaddr_dl and if_data, as Darwin reports first for
        // each interface. The first entry sits at offset 0: the block's base
        // is the list head getifaddrs returns and freeifaddrs releases.
        std::uint64_t name{};
        {
            const auto entry = image.reserve(ifaddrs_size);
            link(entry);
            name = image.reserve(item.name.size() + 1);
            std::memcpy(&image.bytes[static_cast<std::size_t>(name)], item.name.data(), item.name.size());
            const auto name_length = static_cast<std::uint8_t>(item.name.size());
            const auto address_length = static_cast<std::uint8_t>(item.mac.size());
            const auto length = static_cast<std::uint8_t>(std::max<unsigned>(20, 8U + name_length + address_length));
            const auto dl = image.reserve(length);
            const auto type = item.loopback ? ift_loop : item.mac.size() == 6 ? ift_ether : ift_other;
            image.put8(dl, length); image.put8(dl + 1, 18);
            image.put16(dl + 2, static_cast<std::uint16_t>(item.index));
            image.put8(dl + 4, type); image.put8(dl + 5, name_length); image.put8(dl + 6, address_length);
            std::memcpy(&image.bytes[static_cast<std::size_t>(dl + 8)], item.name.data(), name_length);
            if (address_length) std::memcpy(&image.bytes[static_cast<std::size_t>(dl + 8 + name_length)], item.mac.data(), address_length);
            const auto data = image.reserve(if_data_size);
            image.put8(data, type); image.put8(data + 3, address_length);
            image.put8(data + 4, type == ift_ether ? 14 : 0);
            image.put32(data + 8, item.mtu);
            image.put32(data + 16, static_cast<std::uint32_t>(std::min<std::uint64_t>(item.speed, UINT32_MAX)));
            image.pointer(entry + 8, name);
            image.put32(entry + 16, flags);
            image.pointer(entry + 24, dl);
            image.pointer(entry + 48, data);
        }
        for (const auto& address : item.addresses) {
            const auto entry = image.reserve(ifaddrs_size);
            link(entry);
            const auto bytes = std::span<const std::uint8_t>(address.bytes.data(), address.family == 2 ? 4 : 16);
            const auto netmask = mask(address.family, address.prefix);
            image.pointer(entry + 8, name);
            image.put32(entry + 16, flags);
            image.pointer(entry + 24, socket_address(image, address.family, bytes, address.scope));
            image.pointer(entry + 32, socket_address(image, address.family, std::span(netmask.data(), bytes.size())));
            if (address.family == 2 && !item.loopback && !item.point_to_point) {
                std::array<std::uint8_t, 4> broadcast{};
                for (int i = 0; i < 4; ++i) broadcast[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(bytes[static_cast<std::size_t>(i)] | ~netmask[static_cast<std::size_t>(i)]);
                image.pointer(entry + 40, socket_address(image, 2, broadcast));
            }
        }
    }
    image.relocate(base);
    return std::move(image.bytes);
}

std::uint64_t image_size(const std::vector<Interface>& interfaces) {
    return ifaddrs_image(interfaces, 0).size();
}

// inet_net_pton(AF_INET): dotted (possibly short) quad with an optional /bits;
// without one, the classful width extended to the octets given (BIND rules).
std::optional<std::pair<std::array<std::uint8_t, 4>, int>> parse_net4(std::string_view text) {
    std::array<std::uint8_t, 4> value{};
    std::size_t position = 0;
    int octets = 0;
    for (;;) {
        if (position >= text.size() || text[position] < '0' || text[position] > '9') return std::nullopt;
        unsigned number = 0;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
            number = number * 10 + static_cast<unsigned>(text[position++] - '0');
            if (number > 255) return std::nullopt;
        }
        if (octets == 4) return std::nullopt;
        value[static_cast<std::size_t>(octets++)] = static_cast<std::uint8_t>(number);
        if (position == text.size() || text[position] == '/') break;
        if (text[position++] != '.') return std::nullopt;
    }
    int bits = -1;
    if (position < text.size()) {
        ++position;
        if (position >= text.size()) return std::nullopt;
        bits = 0;
        while (position < text.size()) {
            if (text[position] < '0' || text[position] > '9') return std::nullopt;
            bits = bits * 10 + (text[position++] - '0');
            if (bits > 32) return std::nullopt;
        }
    }
    if (bits < 0) {
        const auto first = value[0];
        bits = first >= 240 ? 32 : first >= 224 ? 4 : first >= 192 ? 24 : first >= 128 ? 16 : 8;
        if (bits < octets * 8) bits = octets * 8;
    }
    return std::pair{value, bits};
}

} // namespace

std::vector<std::uint8_t> serialize_ifaddrs(const std::vector<Interface>& interfaces, std::uint64_t base) {
    return ifaddrs_image(interfaces, base);
}

void install_libsystem_net(Provider& p) {
    auto& process = p.process();
    // const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT (all zero).
    static_cast<void>(p.data(libsystem_library, "_in6addr_any", 16));

    // int getifaddrs(struct ifaddrs** ifap)
    auto blocks = std::make_shared<std::pair<std::mutex, std::vector<std::uint64_t>>>();
    p.libsystem("_getifaddrs", [&process, blocks](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[0]) throw Errno{efault};
        const auto interfaces = host_interfaces();
        if (interfaces.empty()) { m.write64(s.x[0], 0); return 0; }
        const auto base = process.allocate_guest(image_size(interfaces), 16);
        m.write_bytes(base, ifaddrs_image(interfaces, base));
        { std::lock_guard lock(blocks->first); blocks->second.push_back(base); }
        m.write64(s.x[0], base);
        return 0;
    });
    // void freeifaddrs(struct ifaddrs*): only lists getifaddrs returned.
    p.libsystem("_freeifaddrs", [&process, blocks](CpuState& s, GuestMemory&) -> std::uint64_t {
        if (!s.x[0]) return 0;
        {
            std::lock_guard lock(blocks->first);
            const auto found = std::find(blocks->second.begin(), blocks->second.end(), s.x[0]);
            if (found == blocks->second.end()) throw std::runtime_error("Alchemy freeifaddrs: pointer was not returned by getifaddrs");
            blocks->second.erase(found);
        }
        process.release_guest(s.x[0]);
        return 0;
    });

    // int pselect(int, fd_set*, fd_set*, fd_set*, const struct timespec*, const sigset_t*)
    // Alchemy delivers no asynchronous signals, so the temporary mask has no
    // observable effect; the wait is select() with the timeout converted.
    p.libsystem("_pselect", [p, &process](CpuState& s, GuestMemory& m) -> std::uint64_t {
        std::uint64_t timeval = 0;
        if (s.x[4]) {
            const auto seconds = static_cast<std::int64_t>(m.read64(s.x[4]));
            const auto nanos = static_cast<std::int64_t>(m.read64(s.x[4] + 8));
            if (seconds < 0 || nanos < 0 || nanos >= 1'000'000'000) throw Errno{einval};
            auto micros = (nanos + 999) / 1000;
            auto whole = seconds;
            if (micros == 1'000'000) { ++whole; micros = 0; }
            timeval = process.allocate_guest(16);
            m.write64(timeval, static_cast<std::uint64_t>(whole));
            m.write64(timeval + 8, static_cast<std::uint64_t>(micros));
        }
        std::uint64_t result{};
        try {
            result = p.call(libsystem_library, "_select", {s.x[0], s.x[1], s.x[2], s.x[3], timeval});
        } catch (...) {
            if (timeval) process.release_guest(timeval);
            throw;
        }
        if (timeval) process.release_guest(timeval);
        return result;
    });

    // int connectx(int, const sa_endpoints_t*, sae_associd_t, unsigned flags,
    //              const struct iovec*, unsigned iovcnt, size_t* len, sae_connid_t*)
    // One endpoint pair: bind the source (when given), connect the
    // destination, then write the initial data when the connection completed.
    // Data on a connection still in progress is not queued (*len = 0).
    p.libsystem("_connectx", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const std::uint64_t socket = s.x[0], endpoints = s.x[1], iov = s.x[4];
        const std::uint64_t iovcnt = static_cast<std::uint32_t>(s.x[5]);
        const std::uint64_t length_out = s.x[6], connection_out = m.read64(s.sp);
        if (!endpoints) throw Errno{einval};
        const std::uint64_t source = m.read64(endpoints + 8), source_length = m.read32(endpoints + 16);
        const std::uint64_t destination = m.read64(endpoints + 24), destination_length = m.read32(endpoints + 32);
        if (!destination || !destination_length) throw Errno{einval};
        if (length_out) m.write64(length_out, 0);
        if (source && source_length) {
            if (static_cast<std::int32_t>(p.call(libsystem_library, "_bind", {socket, source, source_length})) < 0) throw Errno{p.guest_errno()};
        }
        if (static_cast<std::int32_t>(p.call(libsystem_library, "_connect", {socket, destination, destination_length})) < 0)
            throw Errno{p.guest_errno()};
        if (connection_out) m.write32(connection_out, 1);
        if (iov && iovcnt) {
            const auto written = static_cast<std::int64_t>(p.call(libsystem_library, "_writev", {socket, iov, iovcnt}));
            if (written < 0) throw Errno{p.guest_errno()};
            if (length_out) m.write64(length_out, static_cast<std::uint64_t>(written));
        }
        return 0;
    });

    // int disconnectx(int, sae_associd_t, sae_connid_t): ends the connection connectx made (its
    // connection id is 1); SAE_ASSOCID_ANY/ALL and SAE_CONNID_ANY/ALL name it too.
    p.libsystem("_disconnectx", [p](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto association = static_cast<std::uint32_t>(s.x[1]), connection = static_cast<std::uint32_t>(s.x[2]);
        if ((association != 0 && association != UINT32_MAX) || (connection > 1 && connection != UINT32_MAX)) throw Errno{einval};
        if (static_cast<std::int32_t>(p.call(libsystem_library, "_shutdown", {s.x[0], 2})) < 0) throw Errno{p.guest_errno()};
        return 0;
    });

    // The BSD numbers-and-dots forms: a, a.b (8.24 bits), a.b.c (8.8.16), a.b.c.d; each part
    // decimal, octal (leading 0) or hex (0x). Trailing white space ends the address.
    const auto parts = [](const std::string& text) -> std::optional<std::vector<std::uint64_t>> {
        std::vector<std::uint64_t> values;
        std::size_t at = 0;
        for (;;) {
            if (at >= text.size() || !std::isdigit(static_cast<unsigned char>(text[at]))) return std::nullopt;
            unsigned base = 10;
            if (text[at] == '0') {
                base = 8; ++at;
                if (at < text.size() && (text[at] == 'x' || text[at] == 'X')) { base = 16; ++at; }
            }
            std::uint64_t value = 0;
            while (at < text.size()) {
                const auto c = static_cast<unsigned char>(text[at]);
                unsigned digit;
                if (std::isdigit(c) && c - '0' < base) digit = c - '0';
                else if (base == 16 && std::isxdigit(c)) digit = static_cast<unsigned>(std::tolower(c) - 'a' + 10);
                else break;
                value = value * base + digit;
                if (value > UINT32_MAX) return std::nullopt;
                ++at;
            }
            values.push_back(value);
            if (at < text.size() && text[at] == '.') {
                if (values.size() == 4) return std::nullopt;
                ++at;
                continue;
            }
            break;
        }
        if (at < text.size() && !std::isspace(static_cast<unsigned char>(text[at]))) return std::nullopt;
        return values;
    };
    // int inet_aton(const char*, struct in_addr*): 1 and the address (network order), or 0.
    p.libsystem("_inet_aton", [parts](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto values = parts(guest_string(m, s.x[0], 256));
        if (!values) return 0;
        const auto& v = *values;
        std::uint64_t address{};
        if (v.size() == 1) address = v[0];
        else if (v.size() == 2) { if (v[0] > 255 || v[1] > 0xffffff) return 0; address = v[0] << 24 | v[1]; }
        else if (v.size() == 3) { if (v[0] > 255 || v[1] > 255 || v[2] > 0xffff) return 0; address = v[0] << 24 | v[1] << 16 | v[2]; }
        else { for (const auto part : v) if (part > 255) return 0; address = v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3]; }
        if (s.x[1]) m.write32(s.x[1], byte_swap32(static_cast<std::uint32_t>(address)));
        return 1;
    }, 0);
    // in_addr_t inet_network(const char*): each part one byte, in host order; INADDR_NONE on error.
    p.libsystem("_inet_network", [parts](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto values = parts(guest_string(m, s.x[0], 256));
        if (!values) return UINT32_MAX;
        std::uint64_t address{};
        for (const auto part : *values) { if (part > 255) return UINT32_MAX; address = address << 8 | part; }
        return address;
    }, UINT32_MAX);
    // The classful helpers: struct in_addr is a 32-bit value in network order.
    p.libsystem("_inet_makeaddr", [](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto net = static_cast<std::uint32_t>(s.x[0]), host = static_cast<std::uint32_t>(s.x[1]);
        std::uint32_t address;
        if (net < 128) address = net << 24 | (host & 0x00ffffffU);
        else if (net < 65536) address = net << 16 | (host & 0xffffU);
        else if (net < 16777216) address = net << 8 | (host & 0xffU);
        else address = net | host;
        return byte_swap32(address);
    });
    p.libsystem("_inet_lnaof", [](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto address = byte_swap32(static_cast<std::uint32_t>(s.x[0]));
        if (!(address & 0x80000000U)) return address & 0x00ffffffU;
        if ((address & 0xc0000000U) == 0x80000000U) return address & 0xffffU;
        return address & 0xffU;
    });
    p.libsystem("_inet_netof", [](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto address = byte_swap32(static_cast<std::uint32_t>(s.x[0]));
        if (!(address & 0x80000000U)) return address >> 24;
        if ((address & 0xc0000000U) == 0x80000000U) return address >> 16;
        return address >> 8;
    });

    // struct protoent* getprotobynumber(int): the entries of Darwin's /etc/protocols.
    struct Proto { const char* name; const char* alias; std::int32_t number; };
    static constexpr Proto protocols[] = {{"ip", "IP", 0}, {"icmp", "ICMP", 1}, {"igmp", "IGMP", 2}, {"tcp", "TCP", 6},
        {"udp", "UDP", 17}, {"ipv6", "IPv6", 41}, {"ipv6-route", "IPv6-Route", 43}, {"ipv6-frag", "IPv6-Frag", 44},
        {"esp", "ESP", 50}, {"ah", "AH", 51}, {"ipv6-icmp", "IPv6-ICMP", 58}, {"ipv6-nonxt", "IPv6-NoNxt", 59},
        {"ipv6-opts", "IPv6-Opts", 60}, {"sctp", "SCTP", 132}};
    std::vector<std::pair<std::int32_t, std::uint64_t>> entries;
    for (const auto& protocol : protocols) {
        const auto name = process.allocate_guest(32), alias = process.allocate_guest(32), aliases = process.allocate_guest(16),
                   entry = process.allocate_guest(24);
        auto& engine = process.main_engine();
        put_string(engine, name, protocol.name);
        put_string(engine, alias, protocol.alias);
        engine.write64(aliases, alias); engine.write64(aliases + 8, 0);
        engine.write64(entry, name); engine.write64(entry + 8, aliases); engine.write64(entry + 16, static_cast<std::uint32_t>(protocol.number));
        entries.emplace_back(protocol.number, entry);
    }
    p.libsystem("_getprotobynumber", [entries](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto number = static_cast<std::int32_t>(s.x[0]);
        for (const auto& [value, entry] : entries) if (value == number) return entry;
        return 0;
    }, 0);

    // int inet_net_pton(int af, const char* src, void* dst, size_t size): network bits or -1.
    p.libsystem("_inet_net_pton", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto family = static_cast<std::int32_t>(s.x[0]);
        const auto text = guest_string(m, s.x[1], 256);
        if (family == 2) {
            const auto parsed = parse_net4(text);
            if (!parsed) throw Errno{enoent};
            const auto bytes = static_cast<std::uint64_t>((parsed->second + 7) / 8);
            if (s.x[3] < bytes) throw Errno{emsgsize};
            m.write_bytes(s.x[2], std::span(parsed->first.data(), static_cast<std::size_t>(bytes)));
            return static_cast<std::uint64_t>(parsed->second);
        }
        if (family == 30) {
            auto address = text;
            int bits = 128;
            if (const auto slash = text.find('/'); slash != std::string::npos) {
                address = text.substr(0, slash);
                const auto suffix = text.substr(slash + 1);
                if (suffix.empty() || suffix.size() > 3 || suffix.find_first_not_of("0123456789") != std::string::npos) throw Errno{enoent};
                bits = std::stoi(suffix);
                if (bits > 128) throw Errno{enoent};
            }
            // inet_pton(AF_INET6): a bare IPv6 literal, no brackets or zone.
            const auto parsed = address.find_first_of("[]%") == std::string::npos ? host::net::parse_numeric(address) : std::nullopt;
            if (!parsed || parsed->family != host::net::Family::ipv6) throw Errno{enoent};
            const auto bytes = static_cast<std::uint64_t>((bits + 7) / 8);
            if (s.x[3] < bytes) throw Errno{emsgsize};
            m.write_bytes(s.x[2], std::span(parsed->bytes.data(), static_cast<std::size_t>(bytes)));
            return static_cast<std::uint64_t>(bits);
        }
        throw Errno{eafnosupport};
    });
}

} // namespace alchemy::export_fill
