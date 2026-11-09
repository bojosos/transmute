#include "alchemy/darwin/network/network_internal.hpp"
#include "alchemy/host/net.hpp"
#include "alchemy/host/network_policy.hpp"
#include "alchemy/host/time.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <variant>


// libresolv (/usr/lib/libresolv.9.dylib): the BIND resolver's res_9_n*
// entry points a license server and a cloud client import. Queries are
// real DNS exchanges with the host's DNS servers (host::net::dns_configuration):
// UDP first, TCP when the answer is truncated; res_nsearch applies the
// host's search suffixes like BIND's ndots:1 rule. The raw answer message is
// returned, so callers parse it as on macOS. h_errno is reported in the
// res_state (res_h_errno) the way BIND's res_nquery does. res_nmkquery and
// res_nsend build and send a caller's own message, res_setservers names the
// servers a res_state asks, and dn_expand reads a compressed name.
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

// A presentation-form name ("www.example.com", a trailing dot allowed, "\." and "\DDD" escapes) in wire
// form (RFC 1035 labels, uncompressed); nullopt for an empty label, a label over 63 bytes or a name over 255.
std::optional<Bytes> encode_name(std::string_view name) {
    Bytes wire;
    if (name == ".") { wire.push_back(0); return wire; }
    Bytes label;
    const auto end_label = [&] {
        if (label.empty() || label.size() > 63) return false;
        wire.push_back(static_cast<std::uint8_t>(label.size()));
        wire.insert(wire.end(), label.begin(), label.end());
        label.clear();
        return true;
    };
    for (std::size_t index = 0; index < name.size(); ++index) {
        const auto c = static_cast<std::uint8_t>(name[index]);
        if (c == '\\') {
            if (++index >= name.size()) return std::nullopt;
            if (index + 2 < name.size() && std::isdigit(static_cast<unsigned char>(name[index])) &&
                std::isdigit(static_cast<unsigned char>(name[index + 1])) && std::isdigit(static_cast<unsigned char>(name[index + 2]))) {
                const auto value = (name[index] - '0') * 100 + (name[index + 1] - '0') * 10 + (name[index + 2] - '0');
                if (value > 255) return std::nullopt;
                label.push_back(static_cast<std::uint8_t>(value));
                index += 2;
            } else {
                label.push_back(static_cast<std::uint8_t>(name[index]));
            }
        } else if (c == '.') {
            if (!end_label()) return std::nullopt;
            if (index + 1 == name.size()) break;   // the trailing dot of an absolute name
        } else {
            label.push_back(c);
        }
    }
    if (!label.empty() && !end_label()) return std::nullopt;
    wire.push_back(0);
    if (wire.size() > 255) return std::nullopt;
    return wire;
}

// A standard query (opcode QUERY, recursion desired when `recurse`) for one question.
std::optional<Bytes> encode_query(std::string_view name, std::uint16_t id, int klass, int type, bool recurse = true) {
    const auto wire = encode_name(name);
    if (!wire) return std::nullopt;
    Bytes query{static_cast<std::uint8_t>(id >> 8), static_cast<std::uint8_t>(id), static_cast<std::uint8_t>(recurse ? 0x01 : 0x00), 0x00,
                0, 1, 0, 0, 0, 0, 0, 0};
    query.insert(query.end(), wire->begin(), wire->end());
    query.push_back(static_cast<std::uint8_t>(type >> 8)); query.push_back(static_cast<std::uint8_t>(type));
    query.push_back(static_cast<std::uint8_t>(klass >> 8)); query.push_back(static_cast<std::uint8_t>(klass));
    return query;
}

// dn_expand (BIND's ns_name_uncompress): the name at `at` of the message [message, end), following
// compression pointers, in presentation form ("." for the root; '.', '\\', '"', ';', '(', ')', '@' and '$'
// escaped with '\', other unprintable bytes as \DDD). Returns the bytes the name takes at `at` (a pointer
// ends it), or nullopt for a malformed name, a pointer loop or a pointer out of the message.
std::optional<std::pair<std::string, std::size_t>> expand_name(std::span<const std::uint8_t> message, std::size_t at) {
    std::string text;
    std::optional<std::size_t> taken;
    std::size_t wire = 0, followed = 0;
    for (auto position = at;;) {
        if (position >= message.size()) return std::nullopt;
        const auto length = message[position];
        if ((length & 0xc0) == 0xc0) {
            if (position + 1 >= message.size()) return std::nullopt;
            if (!taken) taken = position + 2 - at;
            position = static_cast<std::size_t>(((length & 0x3f) << 8) | message[position + 1]);
            followed += 2;
            if (followed >= message.size()) return std::nullopt;   // a loop
            continue;
        }
        if (length & 0xc0) return std::nullopt;                      // the extended label types
        if (length == 0) {
            if (!taken) taken = position + 1 - at;
            break;
        }
        if (position + 1 + length > message.size()) return std::nullopt;
        wire += 1 + length;
        if (wire + 1 > 255) return std::nullopt;
        if (!text.empty()) text.push_back('.');
        for (std::size_t index = 0; index < length; ++index) {
            const auto c = message[position + 1 + index];
            if (c == '.' || c == '\\' || c == '"' || c == ';' || c == '(' || c == ')' || c == '@' || c == '$') {
                text.push_back('\\'); text.push_back(static_cast<char>(c));
            } else if (c > 0x20 && c < 0x7f) {
                text.push_back(static_cast<char>(c));
            } else {
                const char digits[] = {'\\', static_cast<char>('0' + c / 100), static_cast<char>('0' + c / 10 % 10), static_cast<char>('0' + c % 10)};
                text.append(digits, sizeof(digits));
            }
        }
        position += 1 + length;
    }
    if (text.empty()) text = ".";
    return std::pair{std::move(text), *taken};
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

// The servers res_setservers gave a res_state (by its guest address); a state without a list uses the host's.
struct StateServers {
    std::mutex mutex;
    std::map<Address, std::vector<host::net::Address>> lists;
    std::vector<host::net::Address> of(Address state, const Configuration& config) {
        std::lock_guard lock(mutex);
        const auto found = state ? lists.find(state) : lists.end();
        if (found == lists.end()) return config.servers;
        return found->second;
    }
    void forget(Address state) { std::lock_guard lock(mutex); lists.erase(state); }
};

std::uint16_t random_id() {
    static std::mutex mutex;
    static std::mt19937 random{std::random_device{}()};
    std::lock_guard lock(mutex);
    return static_cast<std::uint16_t>(random());
}

// One exchange of `message` (its id in its first two bytes) with each server in turn, two rounds: UDP, then
// TCP when the answer is truncated. An answer with SERVFAIL, NOTIMP or REFUSED moves on to the next server,
// as BIND's res_nsend does. Under ALCHEMY_NETWORK=loopback only loopback servers are asked (others print the
// denial line); ALCHEMY_NETWORK_DENY addresses are skipped. nullopt: no server answered.
std::optional<Bytes> send_message(std::string_view api, const std::vector<host::net::Address>& servers, const Bytes& message) {
    const auto id = static_cast<std::uint16_t>((message[0] << 8) | message[1]);
    std::optional<Bytes> refused;
    for (int round = 0; round < 2; ++round) {
        for (const auto& server : servers) {
            if (network_policy::loopback_only() && !network_policy::loopback_address(server)) {
                if (round == 0) network_policy::deny(api, network_policy::address_text(server));
                continue;
            }
            if (network_policy::refuse_listed_address(api, server)) continue;
            network_policy::allowed(api, server);
            auto answer = exchange_udp(server, message, id);
            if (answer && ((*answer)[2] & 0x02)) {   // truncated: retry over TCP
                if (auto full = exchange_tcp(server, message, id)) answer = std::move(full);
            }
            if (!answer) continue;
            const auto rcode = (*answer)[3] & 0x0f;
            if (rcode == 2 || rcode == 4 || rcode == 5) { refused = std::move(answer); continue; }
            return answer;
        }
    }
    return refused;
}

// res_nquery: the answer, or the h_errno value.
std::variant<Bytes, int> query(const std::vector<host::net::Address>& servers, std::string_view name, int klass, int type) {
    // ALCHEMY_NETWORK=loopback: every query goes to an external DNS server.
    if (network_policy::loopback_only()) {
        network_policy::deny("res_query", name);
        return host_not_found;
    }
    if (network_policy::refuse_listed_host("res_query", name)) return host_not_found;   // ALCHEMY_NETWORK_DENY
    network_policy::allowed("res_query", name);
    const auto message = encode_query(name, random_id(), klass, type);
    if (!message) return no_recovery;
    if (servers.empty()) return try_again;
    const auto answer = send_message("res_query", servers, *message);
    if (!answer) return try_again;
    const auto rcode = (*answer)[3] & 0x0f;
    const auto answers = ((*answer)[6] << 8) | (*answer)[7];
    if (rcode == 2) return try_again;        // SERVFAIL from every server
    if (rcode == 3) return host_not_found;   // NXDOMAIN
    if (rcode != 0) return no_recovery;
    if (answers == 0) return no_data;
    return std::move(*answer);
}

// A res_sockaddr_union entry (128 bytes: sockaddr_in or Darwin's sockaddr_in6), or nullopt for another family.
std::optional<host::net::Address> server_entry(GuestMemory& m, Address at) {
    const auto family = m.read8(at + 1);
    const auto port = static_cast<std::uint16_t>((m.read8(at + 2) << 8) | m.read8(at + 3));
    if (family == 2) {
        std::array<std::uint8_t, 4> bytes{};
        m.read_bytes(at + 4, bytes);
        return host::net::Address::ipv4(bytes, port ? port : 53);
    }
    if (family == 30) {
        std::array<std::uint8_t, 16> bytes{};
        m.read_bytes(at + 8, bytes);
        return host::net::Address::ipv6(bytes, port ? port : 53, m.read32(at + 24));
    }
    return std::nullopt;
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
    const auto servers = std::make_shared<StateServers>();

    // int res_ninit(res_state statp)
    install("_res_9_ninit", [servers](CpuState& s, GuestMemory& m) {
        const auto state = s.x[0];
        if (!state) { s.x[0] = static_cast<Address>(-1); return; }
        servers->forget(state);
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
    for (const bool destroy : {false, true})
        install(destroy ? "_res_9_ndestroy" : "_res_9_nclose", [servers, destroy](CpuState& s, GuestMemory& m) {
            if (s.x[0]) m.write32(s.x[0] + 8, m.read32(s.x[0] + 8) & ~1U);   // clears RES_INIT
            if (destroy) servers->forget(s.x[0]);
            s.x[0] = 0;
        });
    // void res_setservers(res_state, const union res_sockaddr_union *set, int cnt): 128-byte entries (sockaddr_in
    // or sockaddr_in6; port 0 is 53). The state's nsaddr_list and nscount take the first three IPv4 servers, as
    // BIND keeps them; res_nquery, res_nsearch, res_nsend and res_getservers use the whole list.
    install("_res_9_setservers", [servers](CpuState& s, GuestMemory& m) {
        const auto state = s.x[0], set = s.x[1];
        const auto count = static_cast<std::int32_t>(s.x[2]);
        if (!state) return;
        std::vector<host::net::Address> list;
        for (std::int32_t index = 0; set && index < count; ++index)
            if (auto entry = server_entry(m, set + static_cast<Address>(index) * 128)) list.push_back(std::move(*entry));
        std::uint32_t kept = 0;
        for (const auto& server : list) {
            if (server.family != host::net::Family::ipv4 || kept == 3) continue;
            const auto at = state + 16 + kept * 16;
            m.fill_bytes(at, 0, 16);
            m.write8(at, 16); m.write8(at + 1, 2);
            m.write8(at + 2, static_cast<std::uint8_t>(server.port >> 8)); m.write8(at + 3, static_cast<std::uint8_t>(server.port));
            m.write_bytes(at + 4, std::span(server.bytes.data(), 4));
            ++kept;
        }
        m.write32(state + 12, kept);
        std::lock_guard lock(servers->mutex);
        servers->lists[state] = std::move(list);
    });
    // int res_getservers(res_state, union res_sockaddr_union *set, int cnt): 128-byte entries.
    install("_res_9_getservers", [servers](CpuState& s, GuestMemory& m) {
        const auto set = s.x[1];
        const auto capacity = static_cast<std::int32_t>(s.x[2]);
        const auto config = configuration();
        std::int32_t count = 0;
        for (const auto& server : servers->of(s.x[0], config)) {
            if (count >= capacity || !set) break;
            const auto at = set + static_cast<Address>(count) * 128;
            m.fill_bytes(at, 0, 128);
            const auto port = server.port ? server.port : std::uint16_t{53};
            m.write8(at + 2, static_cast<std::uint8_t>(port >> 8)); m.write8(at + 3, static_cast<std::uint8_t>(port));
            if (server.family == host::net::Family::ipv4) {
                m.write8(at, 16); m.write8(at + 1, 2);
                m.write_bytes(at + 4, std::span(server.bytes.data(), 4));
            } else {
                m.write8(at, 28); m.write8(at + 1, 30);   // sin6_len, AF_INET6 (Darwin)
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
        install(search ? "_res_9_nsearch" : "_res_9_nquery", [search, set_h_errno, servers](CpuState& s, GuestMemory& m) {
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
            const auto list = servers->of(state, config);
            for (const auto& candidate : candidates) {
                auto result = query(list, candidate, klass, type);
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
    // int res_nmkquery(res_state, int op, const char *dname, int class, int type, const u_char *data, int datalen,
    // const u_char *newrr, u_char *buf, int buflen): a standard query (opcode QUERY) with a new id (also left in
    // the state's id) and RD when the state has RES_RECURSE. Other opcodes (IQUERY, NOTIFY), a malformed name or
    // a buffer too small: -1.
    install("_res_9_nmkquery", [](CpuState& s, GuestMemory& m) {
        const auto state = s.x[0];
        const auto op = static_cast<std::int32_t>(s.x[1]);
        // buf and buflen are the ninth and tenth arguments: on the stack, packed (Darwin arm64).
        const auto buffer = m.read64(s.sp);
        const auto capacity = static_cast<std::int32_t>(m.read32(s.sp + 8));
        const auto failure = static_cast<Address>(static_cast<std::int64_t>(-1));
        if (op != 0 || !s.x[2] || !buffer || capacity <= 0) { s.x[0] = failure; return; }
        const auto id = random_id();
        const bool recurse = !state || (m.read32(state + 8) & 0x40);
        const auto message = encode_query(read_cstring(m, s.x[2]), id, static_cast<int>(s.x[3]), static_cast<int>(s.x[4]), recurse);
        if (!message || message->size() > static_cast<std::size_t>(capacity)) { s.x[0] = failure; return; }
        if (state) { m.write8(state + 64, static_cast<std::uint8_t>(id)); m.write8(state + 65, static_cast<std::uint8_t>(id >> 8)); }
        m.write_bytes(buffer, *message);
        s.x[0] = static_cast<Address>(message->size());
    });
    // int res_nsend(res_state, const u_char *msg, int msglen, u_char *answer, int anslen): sends a prepared
    // message to the state's servers and returns the whole answer's length (the answer is cut to anslen), whatever
    // its RCODE. No server answered: -1 with ETIMEDOUT (ECONNREFUSED when none could be asked: loopback-only
    // runs, ALCHEMY_NETWORK_DENY, no servers).
    install("_res_9_nsend", [b, servers](CpuState& s, GuestMemory& m) {
        const auto state = s.x[0], answer = s.x[3];
        const auto length = static_cast<std::int32_t>(s.x[2]), capacity = static_cast<std::int32_t>(s.x[4]);
        const auto failure = static_cast<Address>(static_cast<std::int64_t>(-1));
        if (!s.x[1] || length < 12 || length > 65535 || !answer || capacity <= 0) { b->process.set_errno(22); s.x[0] = failure; return; }
        Bytes message(static_cast<std::size_t>(length));
        m.read_bytes(s.x[1], message);
        const auto list = servers->of(state, configuration());
        const bool askable = std::any_of(list.begin(), list.end(), [](const auto& server) {
            return (!network_policy::loopback_only() || network_policy::loopback_address(server)) && !network_policy::listed_address(server);
        });
        const auto reply = send_message("res_send", list, message);
        if (!reply) { b->process.set_errno(askable ? 60 : 61); s.x[0] = failure; return; }   // ETIMEDOUT, ECONNREFUSED
        m.write_bytes(answer, std::span(reply->data(), std::min<std::size_t>(reply->size(), static_cast<std::size_t>(capacity))));
        s.x[0] = static_cast<Address>(reply->size());
    });
    // int dn_expand(const u_char *msg, const u_char *eom, const u_char *src, char *dst, int dstsiz): the
    // compressed name at src in presentation form ("" for the root, as BIND); the bytes it takes at src, or -1.
    install("_res_9_dn_expand", [](CpuState& s, GuestMemory& m) {
        const auto message = s.x[0], end = s.x[1], source = s.x[2], destination = s.x[3];
        const auto capacity = static_cast<std::int32_t>(s.x[4]);
        const auto failure = static_cast<Address>(static_cast<std::int64_t>(-1));
        if (!message || end <= message || source < message || source >= end || end - message > 65535 || !destination || capacity <= 0) {
            s.x[0] = failure; return;
        }
        Bytes bytes(static_cast<std::size_t>(end - message));
        m.read_bytes(message, bytes);
        auto expanded = expand_name(bytes, static_cast<std::size_t>(source - message));
        if (!expanded) { s.x[0] = failure; return; }
        if (expanded->first == ".") expanded->first.clear();
        if (expanded->first.size() + 1 > static_cast<std::size_t>(capacity)) { s.x[0] = failure; return; }
        m.write_bytes(destination, std::span(reinterpret_cast<const std::uint8_t*>(expanded->first.data()), expanded->first.size()));
        m.write8(destination + expanded->first.size(), 0);
        s.x[0] = static_cast<Address>(expanded->second);
    });
}

} // namespace alchemy::gui::network
