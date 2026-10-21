#include "alchemy/darwin/network/network_internal.hpp"
#include "alchemy/host/net.hpp"
#include "alchemy/host/network_policy.hpp"
#include "alchemy/host/time.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <random>
#include <variant>


// libresolv (/usr/lib/libresolv.9.dylib): the BIND resolver's res_9_n*
// entry points a license server and a cloud client import. Queries are
// real DNS exchanges with the host's DNS servers (host::net::dns_configuration):
// UDP first, TCP when the answer is truncated; res_nsearch applies the
// host's search suffixes like BIND's ndots:1 rule. The raw answer message is
// returned, so callers parse it as on macOS. h_errno is reported in the
// res_state (res_h_errno) the way BIND's res_nquery does.
//
// struct __res_state (LP64 Darwin, 552 bytes) fields written: retrans (0),
// retry (4), options (8, RES_INIT | RES_RECURSE | RES_DEFNAMES | RES_DNSRCH),
// nscount (12), nsaddr_list (16, three sockaddr_in), id (64), ndots (388,
// low bits), res_h_errno (496). Other fields are zeroed by res_ninit.
namespace alchemy::gui::network {
namespace {

constexpr std::string_view lib = "/usr/lib/libresolv.9.dylib";
constexpr std::uint64_t state_size = 552, h_errno_offset = 496;
constexpr int host_not_found = 1, try_again = 2, no_recovery = 3, no_data = 4;

using Configuration = host::net::DnsConfiguration;

Configuration configuration() {
    static_cast<void>(host::net::initialize());
    return host::net::dns_configuration().value_or(Configuration{});
}

std::optional<Bytes> encode_query(std::string_view name, std::uint16_t id, int klass, int type) {
    Bytes query{static_cast<std::uint8_t>(id >> 8), static_cast<std::uint8_t>(id), 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0};
    std::size_t start = 0;
    while (start < name.size()) {
        auto end = name.find('.', start);
        if (end == std::string_view::npos) end = name.size();
        const auto length = end - start;
        if (length == 0 || length > 63) return std::nullopt;
        query.push_back(static_cast<std::uint8_t>(length));
        query.insert(query.end(), name.begin() + static_cast<std::ptrdiff_t>(start), name.begin() + static_cast<std::ptrdiff_t>(end));
        start = end + 1;
    }
    query.push_back(0);
    if (query.size() > 12 + 255) return std::nullopt;
    query.push_back(static_cast<std::uint8_t>(type >> 8)); query.push_back(static_cast<std::uint8_t>(type));
    query.push_back(static_cast<std::uint8_t>(klass >> 8)); query.push_back(static_cast<std::uint8_t>(klass));
    return query;
}

struct Socket {
    host::Handle handle;
    ~Socket() { if (handle) host::net::close_socket(handle); }
};

// Waits until `socket` is ready for `events` (or failed) before `deadline` (monotonic ns).
bool wait_ready(host::Handle socket, std::uint16_t events, std::uint64_t deadline) {
    namespace net = host::net;
    for (;;) {
        const auto now = host::time::monotonic_ns();
        const auto ready = net::ready(socket, events, deadline > now ? deadline - now : 0);
        if (!ready || (*ready & net::ready_invalid)) return false;
        if (*ready & (events | net::ready_error | net::ready_hang_up)) return true;
        if (host::time::monotonic_ns() >= deadline) return false;
    }
}

// receive / send on a non-blocking socket, each call waiting up to `timeout_ns`.
host::Result<std::size_t> receive_within(host::Handle socket, host::Bytes buffer, std::uint64_t timeout_ns) {
    const auto deadline = host::time::monotonic_ns() + timeout_ns;
    for (;;) {
        auto got = host::net::receive(socket, buffer);
        if (got || got.error() != host::Error::would_block) return got;
        if (!wait_ready(socket, host::net::ready_readable, deadline)) return host::Error::timed_out;
    }
}
host::Result<std::size_t> send_within(host::Handle socket, host::ConstBytes data, std::uint64_t timeout_ns) {
    const auto deadline = host::time::monotonic_ns() + timeout_ns;
    for (;;) {
        auto sent = host::net::send(socket, data);
        if (sent || sent.error() != host::Error::would_block) return sent;
        if (!wait_ready(socket, host::net::ready_writable, deadline)) return host::Error::timed_out;
    }
}

std::optional<Bytes> exchange_udp(const host::net::Address& server, const Bytes& query, std::uint16_t id) {
    const auto opened = host::net::open_socket(server.family, host::net::SocketType::datagram);
    if (!opened) return std::nullopt;
    Socket s{*opened};
    const auto sent = host::net::send_to(s.handle, std::as_bytes(std::span(query)), server);
    if (!sent || *sent != query.size()) return std::nullopt;
    Bytes answer(65535);
    for (int attempt = 0; attempt < 4; ++attempt) {
        const auto got = receive_within(s.handle, std::as_writable_bytes(std::span(answer)), 2'500'000'000ULL);
        if (!got || *got < 12) return std::nullopt;
        if (((answer[0] << 8) | answer[1]) != id || !(answer[2] & 0x80)) continue;   // not our response
        answer.resize(*got);
        return answer;
    }
    return std::nullopt;
}

std::optional<Bytes> exchange_tcp(const host::net::Address& server, const Bytes& query, std::uint16_t id) {
    constexpr std::uint64_t timeout = 5'000'000'000ULL;
    const auto opened = host::net::open_socket(server.family, host::net::SocketType::stream);
    if (!opened) return std::nullopt;
    Socket s{*opened};
    if (const auto connected = host::net::connect(s.handle, server); !connected) {
        const auto error = connected.error();
        if (error != host::Error::would_block && error != host::Error::in_progress && error != host::Error::already) return std::nullopt;
        if (!wait_ready(s.handle, host::net::ready_writable, host::time::monotonic_ns() + timeout)) return std::nullopt;
        const auto pending = host::net::pending_error(s.handle);
        if (!pending || pending->error != host::Error::none) return std::nullopt;
    }
    Bytes framed{static_cast<std::uint8_t>(query.size() >> 8), static_cast<std::uint8_t>(query.size())};
    framed.insert(framed.end(), query.begin(), query.end());
    for (std::size_t done = 0; done < framed.size();) {
        const auto sent = send_within(s.handle, std::as_bytes(std::span(framed).subspan(done)), timeout);
        if (!sent || *sent == 0) return std::nullopt;
        done += *sent;
    }
    const auto read_exact = [&](std::uint8_t* to, std::size_t count) {
        for (std::size_t done = 0; done < count;) {
            const auto got = receive_within(s.handle, std::as_writable_bytes(std::span(to + done, count - done)), timeout);
            if (!got || *got == 0) return false;
            done += *got;
        }
        return true;
    };
    std::array<std::uint8_t, 2> prefix{};
    if (!read_exact(prefix.data(), 2)) return std::nullopt;
    Bytes answer(static_cast<std::size_t>((prefix[0] << 8) | prefix[1]));
    if (answer.size() < 12 || !read_exact(answer.data(), answer.size())) return std::nullopt;
    if (((answer[0] << 8) | answer[1]) != id) return std::nullopt;
    return answer;
}

// res_nquery: the answer, or the h_errno value.
std::variant<Bytes, int> query(const Configuration& config, std::string_view name, int klass, int type) {
    // ALCHEMY_NETWORK=loopback: every query goes to an external DNS server.
    if (network_policy::loopback_only()) {
        network_policy::deny("res_query", name);
        return host_not_found;
    }
    if (network_policy::refuse_listed_host("res_query", name)) return host_not_found;   // ALCHEMY_NETWORK_DENY
    network_policy::allowed("res_query", name);
    static std::mt19937 random{std::random_device{}()};
    const auto id = static_cast<std::uint16_t>(random());
    const auto message = encode_query(name, id, klass, type);
    if (!message) return no_recovery;
    if (config.servers.empty()) return try_again;
    for (int round = 0; round < 2; ++round) {
        for (const auto& server : config.servers) {
            auto answer = exchange_udp(server, *message, id);
            if (answer && ((*answer)[2] & 0x02)) {   // truncated: retry over TCP
                if (auto full = exchange_tcp(server, *message, id)) answer = std::move(full);
            }
            if (!answer) continue;
            const auto rcode = (*answer)[3] & 0x0f;
            const auto answers = ((*answer)[6] << 8) | (*answer)[7];
            if (rcode == 2) continue;                // SERVFAIL: try the next server
            if (rcode == 3) return host_not_found;   // NXDOMAIN
            if (rcode != 0) return no_recovery;
            if (answers == 0) return no_data;
            return std::move(*answer);
        }
    }
    return try_again;
}

std::string read_cstring(GuestMemory& m, Address at) {
    std::string text;
    for (; text.size() < 1025; ++at) {
        const auto c = m.read8(at);
        if (!c) break;
        text.push_back(static_cast<char>(c));
    }
    return text;
}

} // namespace

void install_resolver(const std::shared_ptr<Bridge>& b) {
    const auto install = [b](std::string name, transmute::GuestCallHandler handler) {
        if (!b->exported(lib, name)) b->function(lib, name, std::move(handler));
    };
    const auto set_h_errno = [](GuestMemory& m, Address state, int value) { if (state) m.write32(state + h_errno_offset, static_cast<std::uint32_t>(value)); };

    // int res_ninit(res_state statp)
    install("_res_9_ninit", [](CpuState& s, GuestMemory& m) {
        const auto state = s.x[0];
        if (!state) { s.x[0] = static_cast<Address>(-1); return; }
        const auto config = configuration();
        m.fill_bytes(state, 0, state_size);
        m.write32(state, 5);                       // retrans
        m.write32(state + 4, 2);                   // retry
        m.write32(state + 8, 0x1 | 0x40 | 0x80 | 0x200);   // RES_INIT | RES_RECURSE | RES_DEFNAMES | RES_DNSRCH
        std::uint32_t count = 0;
        for (const auto& server : config.servers) {
            if (server.family != host::net::Family::ipv4 || count == 3) continue;
            const auto at = state + 16 + count * 16;
            m.write8(at, 16); m.write8(at + 1, 2);  // sin_len, AF_INET
            m.write8(at + 2, 0); m.write8(at + 3, 53);
            m.write_bytes(at + 4, std::span(server.bytes.data(), 4));
            ++count;
        }
        m.write32(state + 12, count);
        const auto id = static_cast<std::uint16_t>(std::random_device{}());
        m.write8(state + 64, static_cast<std::uint8_t>(id)); m.write8(state + 65, static_cast<std::uint8_t>(id >> 8));
        m.write32(state + 388, 1);                 // ndots:1, nsort:0
        s.x[0] = 0;
    });
    for (const auto* name : {"_res_9_nclose", "_res_9_ndestroy"})
        install(name, [](CpuState& s, GuestMemory& m) {
            if (s.x[0]) m.write32(s.x[0] + 8, m.read32(s.x[0] + 8) & ~1U);   // clears RES_INIT
            s.x[0] = 0;
        });
    // int res_getservers(res_state, union res_sockaddr_union *set, int cnt): 128-byte entries.
    install("_res_9_getservers", [](CpuState& s, GuestMemory& m) {
        const auto set = s.x[1];
        const auto capacity = static_cast<std::int32_t>(s.x[2]);
        const auto config = configuration();
        std::int32_t count = 0;
        for (const auto& server : config.servers) {
            if (count >= capacity || !set) break;
            const auto at = set + static_cast<Address>(count) * 128;
            m.fill_bytes(at, 0, 128);
            if (server.family == host::net::Family::ipv4) {
                m.write8(at, 16); m.write8(at + 1, 2); m.write8(at + 3, 53);
                m.write_bytes(at + 4, std::span(server.bytes.data(), 4));
            } else {
                m.write8(at, 28); m.write8(at + 1, 30); m.write8(at + 3, 53);   // sin6_len, AF_INET6 (Darwin)
                m.write_bytes(at + 8, std::span(server.bytes.data(), 16));
                m.write32(at + 24, server.scope_id);
            }
            ++count;
        }
        s.x[0] = static_cast<Address>(count);
    });
    // int res_nquery(res_state, const char *dname, int class, int type, u_char *answer, int anslen)
    // int res_nsearch(...same...)
    for (const bool search : {false, true}) {
        install(search ? "_res_9_nsearch" : "_res_9_nquery", [search, set_h_errno](CpuState& s, GuestMemory& m) {
            const auto state = s.x[0];
            auto name = read_cstring(m, s.x[1]);
            const auto klass = static_cast<int>(s.x[2]), type = static_cast<int>(s.x[3]);
            const auto answer = s.x[4];
            const auto capacity = static_cast<std::int32_t>(s.x[5]);
            const auto config = configuration();
            std::vector<std::string> candidates;
            const bool absolute = !name.empty() && name.back() == '.';
            if (absolute) name.pop_back();
            if (absolute || !search) candidates.push_back(name);
            else {
                const auto dots = std::count(name.begin(), name.end(), '.');
                if (dots >= 1) candidates.push_back(name);
                for (const auto& suffix : config.search) candidates.push_back(name + "." + suffix);
                if (dots == 0) candidates.push_back(name);
            }
            int error = host_not_found;
            for (const auto& candidate : candidates) {
                auto result = query(config, candidate, klass, type);
                if (auto* bytes = std::get_if<Bytes>(&result)) {
                    if (answer && capacity > 0)
                        m.write_bytes(answer, std::span(bytes->data(), std::min<std::size_t>(bytes->size(), static_cast<std::size_t>(capacity))));
                    set_h_errno(m, state, 0);
                    s.x[0] = static_cast<Address>(bytes->size());
                    return;
                }
                error = std::get<int>(result);
                if (error == try_again || error == no_recovery) break;
            }
            set_h_errno(m, state, error);
            s.x[0] = static_cast<Address>(static_cast<std::int64_t>(-1));
        });
    }
}

} // namespace alchemy::gui::network
