#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/darwin/libsystem/install.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// <netdb.h> and <net/ethernet.h> leftovers (fill-only):
//   gethostbyname2, getipnodebyname, getipnodebyaddr, freehostent  over getaddrinfo and
//       gethostbyaddr (same network policy); gethostbyname2's hostent is per thread,
//       getipnode*'s are malloc'd
//   herror, inet_net_ntop
//   sethostent/gethostent/endhostent, setservent/getservent/endservent,
//   setprotoent/getprotoent/endprotoent, setnetent/getnetent/endnetent, getnetbyname,
//   getnetbyaddr  the flat files /etc/hosts, /etc/services, /etc/protocols and
//       /etc/networks of the guest file system (none when a file is missing)
//   ether_line, ether_ntohost, ether_hostton  over /etc/ethers
//   inet6_opt_* and inet6_rth_* (RFC 3542 extension headers), inet6_option_space (RFC 2292)
// Structure layouts follow <netdb.h>: hostent {h_name, h_aliases, int h_addrtype,
// int h_length, h_addr_list} (32 bytes), servent {s_name, s_aliases, int s_port, s_proto}
// (32), protoent {p_name, p_aliases, int p_proto} (24), netent {n_name, n_aliases,
// int n_addrtype, uint32_t n_net} (24).
namespace alchemy::export_fill {
namespace {

constexpr std::uint32_t af_inet = 2, af_inet6 = 30;
constexpr std::uint32_t host_not_found = 1, try_again = 2, no_recovery = 3, no_data = 4;
constexpr std::uint64_t entry_buffer_size = 8192;

// Bump allocation of a returned entry and the strings, pointer arrays and
// addresses it points at, inside one guest buffer.
class Layout {
public:
    Layout(GuestMemory& m, std::uint64_t base, std::uint64_t capacity, std::uint64_t header)
        : m_(m), base_(base), capacity_(capacity), used_(header) {
        if (header > capacity) throw Errno{erange};
        m.fill_bytes(base, 0, header);
    }
    std::uint64_t string(std::string_view value) {
        const auto at = take(value.size() + 1, 1);
        put_string(m_, at, value);
        return at;
    }
    std::uint64_t bytes(std::span<const std::uint8_t> value) {
        const auto at = take(value.size(), 1);
        m_.write_bytes(at, value);
        return at;
    }
    // A NULL-terminated pointer array.
    std::uint64_t pointers(const std::vector<std::uint64_t>& values) {
        const auto at = take((values.size() + 1) * 8, 8);
        for (std::size_t index = 0; index < values.size(); ++index) m_.write64(at + index * 8, values[index]);
        m_.write64(at + values.size() * 8, 0);
        return at;
    }
    std::uint64_t strings(const std::vector<std::string>& values) {
        std::vector<std::uint64_t> addresses;
        for (const auto& value : values) addresses.push_back(string(value));
        return pointers(addresses);
    }

private:
    std::uint64_t take(std::uint64_t size, std::uint64_t alignment) {
        used_ = (used_ + alignment - 1) / alignment * alignment;
        if (size > capacity_ - std::min(used_, capacity_)) throw Errno{erange};
        const auto at = base_ + used_;
        used_ += size;
        return at;
    }
    GuestMemory& m_;
    std::uint64_t base_, capacity_, used_;
};

struct Host {
    std::string name;
    std::vector<std::string> aliases;
    std::uint32_t family{af_inet};
    std::vector<std::vector<std::uint8_t>> addresses;
};

void write_host(GuestMemory& m, std::uint64_t base, std::uint64_t capacity, const Host& host) {
    Layout layout(m, base, capacity, 32);
    const auto name = layout.string(host.name);
    const auto aliases = layout.strings(host.aliases);
    std::vector<std::uint64_t> addresses;
    for (const auto& address : host.addresses) addresses.push_back(layout.bytes(address));
    const auto list = layout.pointers(addresses);
    m.write64(base, name);
    m.write64(base + 8, aliases);
    m.write32(base + 16, host.family);
    m.write32(base + 20, host.family == af_inet ? 4 : 16);
    m.write64(base + 24, list);
}

std::uint64_t host_size(const Host& host) {
    std::uint64_t size = 32 + host.name.size() + 1 + (host.aliases.size() + host.addresses.size() + 2) * 8 + 16;
    for (const auto& alias : host.aliases) size += alias.size() + 1;
    for (const auto& address : host.addresses) size += address.size();
    return size;
}

std::vector<std::string> fields(std::string_view line) {
    if (const auto hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
    std::vector<std::string> result;
    std::size_t at = 0;
    while (at < line.size()) {
        while (at < line.size() && (line[at] == ' ' || line[at] == '\t' || line[at] == '\r')) ++at;
        const auto start = at;
        while (at < line.size() && line[at] != ' ' && line[at] != '\t' && line[at] != '\r') ++at;
        if (at > start) result.emplace_back(line.substr(start, at - start));
    }
    return result;
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        result.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return result;
}

std::optional<std::array<std::uint8_t, 6>> parse_ether(std::string_view text) {
    std::array<std::uint8_t, 6> result{};
    std::size_t at = 0;
    for (std::size_t octet = 0; octet < 6; ++octet) {
        unsigned value = 0, digits = 0;
        while (at < text.size() && digits < 2) {
            const auto c = text[at];
            unsigned digit;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
            else break;
            value = value * 16 + digit;
            ++digits; ++at;
        }
        if (!digits) return std::nullopt;
        result[octet] = static_cast<std::uint8_t>(value);
        if (octet < 5) { if (at >= text.size() || text[at] != ':') return std::nullopt; ++at; }
    }
    if (at != text.size()) return std::nullopt;
    return result;
}

} // namespace

void install_libsystem_netdb_extra(Provider& p) {
    auto& process = p.process();
    const auto call = [p](std::string_view name, std::initializer_list<std::uint64_t> arguments) {
        return p.call(libsystem_library, name, arguments);
    };
    const auto guest_text = [p](GuestMemory& m, std::string_view value) {
        const auto address = p.malloc(value.size() + 1);
        put_string(m, address, value);
        return address;
    };
    // Per-thread result buffers ("static" storage of the BSD interfaces, per thread as Darwin's).
    struct Buffers { std::mutex mutex; std::map<std::pair<int, std::uint64_t>, std::uint64_t> values; };
    auto buffers = std::make_shared<Buffers>();
    const auto buffer = [&process, buffers](int kind) {
        const auto thread = process.current_thread();
        std::lock_guard lock(buffers->mutex);
        auto& address = buffers->values[{kind, thread ? thread->identity : 0}];
        if (!address) address = process.allocate_guest(entry_buffer_size);
        return address;
    };
    const auto set_h_errno = [&process](GuestMemory& m, std::uint32_t value) {
        if (const auto cell = process.resolve(libsystem_library, "_h_errno")) m.write32(cell->address, value);
    };
    // The text of a guest file, or nullopt when it cannot be read.
    const auto read_file = [call, guest_text](GuestMemory& m, std::string_view path) -> std::optional<std::string> {
        const auto name = guest_text(m, path);
        const auto descriptor = static_cast<std::int32_t>(call("_open", {name, 0}));
        static_cast<void>(call("_free", {name}));
        if (descriptor < 0) return std::nullopt;
        std::string text;
        const auto chunk = call("_malloc", {4096});
        while (chunk && text.size() < (16U << 20)) {
            const auto got = static_cast<std::int64_t>(call("_read", {static_cast<std::uint32_t>(descriptor), chunk, 4096}));
            if (got <= 0) break;
            std::vector<std::uint8_t> data(static_cast<std::size_t>(got));
            m.read_bytes(chunk, data);
            text.append(data.begin(), data.end());
        }
        if (chunk) static_cast<void>(call("_free", {chunk}));
        static_cast<void>(call("_close", {static_cast<std::uint32_t>(descriptor)}));
        return text;
    };
    // inet_pton into host bytes: the family and its address, or nullopt.
    const auto parse_address = [call, guest_text](GuestMemory& m, std::string_view text) -> std::optional<std::pair<std::uint32_t, std::vector<std::uint8_t>>> {
        const auto source = guest_text(m, text);
        const auto target = call("_malloc", {16});
        std::optional<std::pair<std::uint32_t, std::vector<std::uint8_t>>> result;
        for (const auto family : {af_inet, af_inet6}) {
            if (static_cast<std::int32_t>(call("_inet_pton", {family, source, target})) != 1) continue;
            std::vector<std::uint8_t> bytes(family == af_inet ? 4 : 16);
            m.read_bytes(target, bytes);
            result.emplace(family, std::move(bytes));
            break;
        }
        static_cast<void>(call("_free", {target}));
        static_cast<void>(call("_free", {source}));
        return result;
    };

    // ---- name lookups over getaddrinfo ----
    // The addresses of `family` getaddrinfo finds for name (h_errno-style error otherwise).
    struct Lookup { Host host; std::uint32_t error{}; };
    const auto lookup = [call](GuestMemory& m, std::uint64_t name, std::uint32_t family, std::uint32_t flags) {
        Lookup result;
        result.host.family = family;
        const auto hints = call("_calloc", {1, 48}), list = call("_calloc", {1, 8});
        if (!hints || !list) throw Errno{enomem};
        m.write32(hints, 0x2 | flags);       // AI_CANONNAME, plus AI_ADDRCONFIG when asked
        m.write32(hints + 4, family);
        m.write32(hints + 8, 1);             // SOCK_STREAM: one entry per address
        const auto error = static_cast<std::int32_t>(call("_getaddrinfo", {name, 0, hints, list}));
        if (error) {
            result.error = error == 2 ? try_again : error == 8 ? host_not_found : error == 7 ? no_data : no_recovery;
        } else {
            for (auto entry = m.read64(list); entry; entry = m.read64(entry + 40)) {
                if (const auto canonical = m.read64(entry + 24); canonical && result.host.name.empty())
                    result.host.name = guest_string(m, canonical, 1025);
                if (m.read32(entry + 4) != family) continue;
                const auto address = m.read64(entry + 32);
                std::vector<std::uint8_t> bytes(family == af_inet ? 4 : 16);
                m.read_bytes(address + (family == af_inet ? 4 : 8), bytes);
                if (std::find(result.host.addresses.begin(), result.host.addresses.end(), bytes) == result.host.addresses.end())
                    result.host.addresses.push_back(std::move(bytes));
            }
            if (const auto first = m.read64(list)) static_cast<void>(call("_freeaddrinfo", {first}));
            if (result.host.addresses.empty()) result.error = no_data;
        }
        if (result.host.name.empty()) result.host.name = guest_string(m, name, 1025);
        static_cast<void>(call("_free", {hints}));
        static_cast<void>(call("_free", {list}));
        return result;
    };
    // struct hostent* gethostbyname2(const char* name, int af): AF_INET or AF_INET6 only.
    p.libsystem("_gethostbyname2", [lookup, buffer, set_h_errno](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto family = static_cast<std::uint32_t>(s.x[1]);
        if (family != af_inet && family != af_inet6) { set_h_errno(m, no_recovery); throw Errno{eafnosupport}; }
        auto found = lookup(m, s.x[0], family, 0);
        if (found.error) { set_h_errno(m, found.error); return 0; }
        if (host_size(found.host) > entry_buffer_size) { set_h_errno(m, no_recovery); return 0; }
        const auto result = buffer(0);
        write_host(m, result, entry_buffer_size, found.host);
        set_h_errno(m, 0);
        return result;
    }, 0);
    // getipnodebyname(name, af, flags, int* error): a malloc'd hostent (freehostent). With
    // AF_INET6, AI_V4MAPPED (or AI_V4MAPPED_CFG) maps IPv4 answers when there are no IPv6
    // ones (all of them too with AI_ALL); AI_ADDRCONFIG goes to getaddrinfo.
    constexpr std::uint32_t ai_all = 0x100, ai_v4mapped_cfg = 0x200, ai_addrconfig = 0x400, ai_v4mapped = 0x800;
    const auto allocate_host = [p](GuestMemory& m, const Host& host) {
        const auto size = host_size(host);
        const auto result = p.malloc(size);
        write_host(m, result, size, host);
        return result;
    };
    p.libsystem("_getipnodebyname", [lookup, allocate_host](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto family = static_cast<std::uint32_t>(s.x[1]), flags = static_cast<std::uint32_t>(s.x[2]);
        const auto fail = [&](std::uint32_t error) { if (s.x[3]) m.write32(s.x[3], error); return std::uint64_t{0}; };
        if (family != af_inet && family != af_inet6) { if (s.x[3]) m.write32(s.x[3], no_recovery); throw Errno{eafnosupport}; }
        const auto config = flags & ai_addrconfig;
        auto found = lookup(m, s.x[0], family, config);
        const bool mapped = family == af_inet6 && (flags & (ai_v4mapped | ai_v4mapped_cfg));
        if (mapped && (found.error || (flags & ai_all))) {
            const auto ipv4 = lookup(m, s.x[0], af_inet, config);
            if (!ipv4.error) {
                if (found.error) { found.host.name = ipv4.host.name; found.error = 0; }
                for (const auto& address : ipv4.host.addresses) {
                    std::vector<std::uint8_t> bytes(16, 0);
                    bytes[10] = bytes[11] = 0xff;
                    std::copy(address.begin(), address.end(), bytes.begin() + 12);
                    found.host.addresses.push_back(std::move(bytes));
                }
            }
        }
        if (found.error) return fail(found.error);
        found.host.family = family;
        if (s.x[3]) m.write32(s.x[3], 0);
        return allocate_host(m, found.host);
    }, 0);
    // getipnodebyaddr(src, len, af, int* error): gethostbyaddr's entry, copied to malloc'd memory.
    p.libsystem("_getipnodebyaddr", [call, allocate_host, &process](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto entry = call("_gethostbyaddr", {s.x[0], s.x[1] & 0xffffffffULL, s.x[2] & 0xffffffffULL});
        if (!entry) {
            std::uint32_t error = host_not_found;
            if (const auto cell = process.resolve(libsystem_library, "_h_errno")) error = m.read32(cell->address);
            if (s.x[3]) m.write32(s.x[3], error);
            return 0;
        }
        Host host;
        host.name = guest_string(m, m.read64(entry), 1025);
        for (auto alias = m.read64(entry + 8); alias && m.read64(alias); alias += 8) host.aliases.push_back(guest_string(m, m.read64(alias), 1025));
        host.family = m.read32(entry + 16);
        const auto length = m.read32(entry + 20);
        for (auto item = m.read64(entry + 24); item && m.read64(item); item += 8) {
            std::vector<std::uint8_t> bytes(length);
            m.read_bytes(m.read64(item), bytes);
            host.addresses.push_back(std::move(bytes));
        }
        if (s.x[3]) m.write32(s.x[3], 0);
        return allocate_host(m, host);
    }, 0);
    p.libsystem("_freehostent", [call](CpuState& s, GuestMemory&) -> std::uint64_t {
        if (s.x[0]) static_cast<void>(call("_free", {s.x[0]}));
        return 0;
    });

    // void herror(const char* s): "<s>: <hstrerror(h_errno)>\n" on standard error.
    p.libsystem("_herror", [call, guest_text, &process](CpuState& s, GuestMemory& m) -> std::uint64_t {
        std::uint32_t error = 0;
        if (const auto cell = process.resolve(libsystem_library, "_h_errno")) error = m.read32(cell->address);
        std::string line;
        if (s.x[0]) if (auto prefix = guest_string(m, s.x[0], 4096); !prefix.empty()) line = prefix + ": ";
        line += guest_string(m, call("_hstrerror", {error}), 1024) + "\n";
        const auto text = guest_text(m, line);
        static_cast<void>(call("_write", {2, text, line.size()}));
        static_cast<void>(call("_free", {text}));
        return 0;
    });

    // char* inet_net_ntop(int af, const void* src, int bits, char* dst, size_t size):
    // "a[.b[.c[.d]]]/bits" with the octets bits covers (AF_INET); for AF_INET6 the
    // address with the bits past the prefix cleared, then "/bits".
    p.libsystem("_inet_net_ntop", [call](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto family = static_cast<std::uint32_t>(s.x[0]);
        const auto bits = static_cast<std::int32_t>(s.x[2]);
        std::string text;
        if (family == af_inet) {
            if (bits < 0 || bits > 32) throw Errno{einval};
            std::array<std::uint8_t, 4> address{};
            m.read_bytes(s.x[1], address);
            if (bits == 0) text = "0";
            const auto whole = bits / 8;
            for (int index = 0; index < whole; ++index) text += (index ? "." : "") + std::to_string(address[static_cast<std::size_t>(index)]);
            if (bits % 8) {
                const auto mask = static_cast<std::uint8_t>(0xff00U >> (bits % 8));
                text += (whole ? "." : "") + std::to_string(address[static_cast<std::size_t>(whole)] & mask);
            }
        } else if (family == af_inet6) {
            if (bits < 0 || bits > 128) throw Errno{einval};
            std::array<std::uint8_t, 16> address{};
            m.read_bytes(s.x[1], address);
            for (int index = 0; index < 16; ++index) {
                const auto keep = std::clamp(bits - index * 8, 0, 8);
                address[static_cast<std::size_t>(index)] &= static_cast<std::uint8_t>(0xff00U >> keep);
            }
            const auto source = call("_malloc", {16}), target = call("_malloc", {64});
            m.write_bytes(source, address);
            if (call("_inet_ntop", {af_inet6, source, target, 64})) text = guest_string(m, target, 64);
            static_cast<void>(call("_free", {source}));
            static_cast<void>(call("_free", {target}));
            if (text.empty()) throw Errno{einval};
        } else throw Errno{eafnosupport};
        text += "/" + std::to_string(bits);
        if (text.size() + 1 > s.x[4]) throw Errno{emsgsize};
        put_string(m, s.x[3], text);
        return s.x[3];
    }, 0);

    // ---- the /etc databases ----
    struct Database { std::mutex mutex; std::optional<std::vector<std::string>> lines; std::size_t next{}; };
    auto databases = std::make_shared<std::array<Database, 4>>();   // hosts, services, protocols, networks
    constexpr std::array<std::string_view, 4> paths{"/etc/hosts", "/etc/services", "/etc/protocols", "/etc/networks"};
    constexpr std::array<std::string_view, 4> stems{"host", "serv", "proto", "net"};
    const auto load = [read_file, paths](GuestMemory& m, std::size_t kind) {
        const auto text = read_file(m, paths[kind]);
        return text ? lines(*text) : std::vector<std::string>{};
    };
    // Each kind's entry from one line's fields, written to the thread's buffer; 0 when the line is not one.
    using Writer = std::function<std::uint64_t(GuestMemory&, const std::vector<std::string>&)>;
    std::array<Writer, 4> writers;
    writers[0] = [parse_address, buffer](GuestMemory& m, const std::vector<std::string>& f) -> std::uint64_t {
        if (f.size() < 2) return 0;
        const auto address = parse_address(m, f[0]);
        if (!address) return 0;
        Host host;
        host.name = f[1];
        host.aliases.assign(f.begin() + 2, f.end());
        host.family = address->first;
        host.addresses.push_back(address->second);
        if (host_size(host) > entry_buffer_size) return 0;
        const auto result = buffer(1);
        write_host(m, result, entry_buffer_size, host);
        return result;
    };
    writers[1] = [buffer](GuestMemory& m, const std::vector<std::string>& f) -> std::uint64_t {
        if (f.size() < 2) return 0;
        const auto slash = f[1].find('/');
        if (slash == std::string::npos || slash == 0) return 0;
        const auto port_text = f[1].substr(0, slash);
        if (port_text.find_first_not_of("0123456789") != std::string::npos || port_text.size() > 5) return 0;
        const auto port = static_cast<std::uint32_t>(std::stoul(port_text));
        if (port > 65535) return 0;
        const auto result = buffer(2);
        try {
            Layout layout(m, result, entry_buffer_size, 32);
            m.write64(result, layout.string(f[0]));
            m.write64(result + 8, layout.strings({f.begin() + 2, f.end()}));
            m.write32(result + 16, ((port & 0xff) << 8) | (port >> 8));   // network order
            m.write64(result + 24, layout.string(f[1].substr(slash + 1)));
        } catch (const Errno&) { return 0; }
        return result;
    };
    writers[2] = [buffer](GuestMemory& m, const std::vector<std::string>& f) -> std::uint64_t {
        if (f.size() < 2 || f[1].empty() || f[1].size() > 9 || f[1].find_first_not_of("0123456789") != std::string::npos) return 0;
        const auto result = buffer(3);
        try {
            Layout layout(m, result, entry_buffer_size, 24);
            m.write64(result, layout.string(f[0]));
            m.write64(result + 8, layout.strings({f.begin() + 2, f.end()}));
            m.write32(result + 16, static_cast<std::uint32_t>(std::stoul(f[1])));
        } catch (const Errno&) { return 0; }
        return result;
    };
    writers[3] = [buffer, call, guest_text](GuestMemory& m, const std::vector<std::string>& f) -> std::uint64_t {
        if (f.size() < 2) return 0;
        const auto text = guest_text(m, f[1]);
        const auto network = static_cast<std::uint32_t>(call("_inet_network", {text}));
        static_cast<void>(call("_free", {text}));
        if (network == 0xffffffffU) return 0;
        const auto result = buffer(4);
        try {
            Layout layout(m, result, entry_buffer_size, 24);
            m.write64(result, layout.string(f[0]));
            m.write64(result + 8, layout.strings({f.begin() + 2, f.end()}));
            m.write32(result + 16, af_inet);
            m.write32(result + 20, network);
        } catch (const Errno&) { return 0; }
        return result;
    };
    for (std::size_t kind = 0; kind < 4; ++kind) {
        const auto stem = std::string(stems[kind]);
        // set<kind>ent(int stayopen): rewind (reading the file again).
        p.libsystem(("_set" + stem + "ent").c_str(), [databases, load, kind](CpuState&, GuestMemory& m) -> std::uint64_t {
            auto& database = (*databases)[kind];
            auto fresh = load(m, kind);
            std::lock_guard lock(database.mutex);
            database.lines = std::move(fresh);
            database.next = 0;
            return 0;
        });
        p.libsystem(("_end" + stem + "ent").c_str(), [databases, kind](CpuState&, GuestMemory&) -> std::uint64_t {
            auto& database = (*databases)[kind];
            std::lock_guard lock(database.mutex);
            database.lines.reset();
            database.next = 0;
            return 0;
        });
        // get<kind>ent(): the next entry, or NULL at the end (or without the file).
        p.libsystem(("_get" + stem + "ent").c_str(), [databases, load, writers, kind](CpuState&, GuestMemory& m) -> std::uint64_t {
            auto& database = (*databases)[kind];
            std::unique_lock lock(database.mutex);
            if (!database.lines) {
                lock.unlock();
                auto fresh = load(m, kind);
                lock.lock();
                if (!database.lines) { database.lines = std::move(fresh); database.next = 0; }
            }
            while (database.next < database.lines->size()) {
                const auto f = fields((*database.lines)[database.next++]);
                if (f.empty()) continue;
                if (const auto entry = writers[kind](m, f)) return entry;
            }
            return 0;
        }, 0);
    }
    // getnetbyname(name) / getnetbyaddr(net, type): a search of /etc/networks.
    const auto find_network = [load, writers](GuestMemory& m, const std::function<bool(const std::vector<std::string>&)>& match) -> std::uint64_t {
        for (const auto& line : load(m, 3)) {
            const auto f = fields(line);
            if (f.size() >= 2 && match(f))
                if (const auto entry = writers[3](m, f)) return entry;
        }
        return 0;
    };
    p.libsystem("_getnetbyname", [find_network](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto name = guest_string(m, s.x[0], 1025);
        return find_network(m, [&name](const std::vector<std::string>& f) {
            return f[0] == name || std::find(f.begin() + 2, f.end(), name) != f.end();
        });
    }, 0);
    p.libsystem("_getnetbyaddr", [find_network, call, guest_text](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (static_cast<std::uint32_t>(s.x[1]) != af_inet) return 0;
        const auto wanted = static_cast<std::uint32_t>(s.x[0]);
        return find_network(m, [&](const std::vector<std::string>& f) {
            const auto text = guest_text(m, f[1]);
            const auto network = static_cast<std::uint32_t>(call("_inet_network", {text}));
            static_cast<void>(call("_free", {text}));
            return network == wanted;
        });
    }, 0);

    // ---- IPv6 extension headers (RFC 3542 section 10 and 7) ----
    // Options: a header {next header, length in 8-byte units minus 1}, then TLV options, aligned
    // with Pad1 (a 0 byte) and PadN (type 1, length, zeros).
    const auto pad = [](GuestMemory& m, std::uint64_t at, std::uint32_t count) {
        if (count == 1) { m.write8(at, 0); return; }
        if (count < 2) return;
        m.write8(at, 1);
        m.write8(at + 1, static_cast<std::uint8_t>(count - 2));
        if (count > 2) m.fill_bytes(at + 2, 0, count - 2);
    };
    const auto signed_int = [](std::uint64_t value) { return static_cast<std::int32_t>(value); };
    const auto result = [](std::int64_t value) { return static_cast<std::uint64_t>(static_cast<std::uint32_t>(static_cast<std::int32_t>(value))); };
    // int inet6_opt_init(void* extbuf, socklen_t extlen): the header's 2 bytes.
    p.libsystem("_inet6_opt_init", [result](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto length = static_cast<std::uint32_t>(s.x[1]);
        if (s.x[0]) {
            if (!length || length % 8) return result(-1);
            m.write8(s.x[0] + 1, static_cast<std::uint8_t>(length / 8 - 1));
        }
        return 2;
    });
    // int inet6_opt_append(extbuf, extlen, offset, uint8_t type, socklen_t len, uint8_t align, void** databufp)
    p.libsystem("_inet6_opt_append", [pad, signed_int, result](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto offset = signed_int(s.x[2]);
        const auto type = static_cast<std::uint8_t>(s.x[3]);
        const auto length = static_cast<std::uint32_t>(s.x[4]), align = static_cast<std::uint32_t>(s.x[5] & 0xff);
        if (offset < 2 || type < 2 || length > 255 || (align != 1 && align != 2 && align != 4 && align != 8) || align > std::max(length, 1U))
            return result(-1);
        const auto padding = (align - (static_cast<std::uint32_t>(offset) + 2) % align) % align;
        const auto end = static_cast<std::uint32_t>(offset) + padding + 2 + length;
        if (s.x[0]) {
            if (end > static_cast<std::uint32_t>(s.x[1])) return result(-1);
            pad(m, s.x[0] + static_cast<std::uint32_t>(offset), padding);
            const auto option = s.x[0] + static_cast<std::uint32_t>(offset) + padding;
            m.write8(option, type);
            m.write8(option + 1, static_cast<std::uint8_t>(length));
            if (s.x[6]) m.write64(s.x[6], option + 2);
        }
        return end;
    });
    // int inet6_opt_finish(extbuf, extlen, offset): padded to a multiple of 8.
    p.libsystem("_inet6_opt_finish", [pad, signed_int, result](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto offset = signed_int(s.x[2]);
        if (offset < 2) return result(-1);
        const auto end = (static_cast<std::uint32_t>(offset) + 7) & ~7U;
        if (s.x[0]) {
            if (end > static_cast<std::uint32_t>(s.x[1])) return result(-1);
            pad(m, s.x[0] + static_cast<std::uint32_t>(offset), end - static_cast<std::uint32_t>(offset));
        }
        return end;
    });
    // int inet6_opt_set_val(void* databuf, int offset, void* val, socklen_t vallen) / inet6_opt_get_val
    for (const bool get : {false, true})
        p.libsystem(get ? "_inet6_opt_get_val" : "_inet6_opt_set_val", [signed_int, get](CpuState& s, GuestMemory& m) -> std::uint64_t {
            const auto offset = signed_int(s.x[1]);
            const auto length = static_cast<std::uint32_t>(s.x[3]);
            if (length) {
                if (get) m.copy_bytes(s.x[2], s.x[0] + static_cast<std::uint32_t>(offset), length);
                else m.copy_bytes(s.x[0] + static_cast<std::uint32_t>(offset), s.x[2], length);
            }
            return static_cast<std::uint32_t>(offset + static_cast<std::int32_t>(length));
        });
    // inet6_opt_next(extbuf, extlen, offset, uint8_t* typep, socklen_t* lenp, void** databufp) and
    // inet6_opt_find(extbuf, extlen, offset, uint8_t type, socklen_t* lenp, void** databufp): the next
    // option (of that type) after offset (0: the first), skipping padding; -1 at the end.
    for (const bool find : {false, true})
        p.libsystem(find ? "_inet6_opt_find" : "_inet6_opt_next", [signed_int, result, find](CpuState& s, GuestMemory& m) -> std::uint64_t {
            const auto size = static_cast<std::uint32_t>(s.x[1]);
            auto at = static_cast<std::uint32_t>(std::max(signed_int(s.x[2]), 2));
            if (!s.x[0] || signed_int(s.x[2]) < 0) return result(-1);
            while (at < size) {
                const auto type = m.read8(s.x[0] + at);
                if (type == 0) { ++at; continue; }
                if (at + 2 > size) return result(-1);
                const auto length = m.read8(s.x[0] + at + 1);
                if (at + 2 + length > size) return result(-1);
                const auto data = s.x[0] + at + 2;
                at += 2 + length;
                if (type == 1 || (find && type != static_cast<std::uint8_t>(s.x[3]))) continue;
                if (!find && s.x[3]) m.write8(s.x[3], type);
                if (s.x[4]) m.write32(s.x[4], length);
                if (s.x[5]) m.write64(s.x[5], data);
                return at;
            }
            return result(-1);
        });
    // Type 0 routing headers: {next header, length (2 per address), type 0, segments left,
    // reserved}, then the addresses.
    p.libsystem("_inet6_rth_space", [](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto segments = static_cast<std::int32_t>(s.x[1]);
        if (static_cast<std::int32_t>(s.x[0]) != 0 || segments < 0 || segments > 127) return 0;
        return 8 + 16U * static_cast<std::uint32_t>(segments);
    });
    p.libsystem("_inet6_rth_init", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto segments = static_cast<std::int32_t>(s.x[3]);
        if (static_cast<std::int32_t>(s.x[2]) != 0 || segments < 0 || segments > 127) return 0;
        const auto space = 8 + 16U * static_cast<std::uint32_t>(segments);
        if (!s.x[0] || static_cast<std::uint32_t>(s.x[1]) < space) return 0;
        m.fill_bytes(s.x[0], 0, space);
        m.write8(s.x[0] + 1, static_cast<std::uint8_t>(segments * 2));
        return s.x[0];
    }, 0);
    // int inet6_rth_add(void* bp, const struct in6_addr*): the next address (segments left counts them).
    p.libsystem("_inet6_rth_add", [result](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto count = m.read8(s.x[0] + 3), capacity = static_cast<std::uint8_t>(m.read8(s.x[0] + 1) / 2);
        if (m.read8(s.x[0] + 2) != 0 || count >= capacity) return result(-1);
        m.copy_bytes(s.x[0] + 8 + 16ULL * count, s.x[1], 16);
        m.write8(s.x[0] + 3, static_cast<std::uint8_t>(count + 1));
        return 0;
    });
    p.libsystem("_inet6_rth_segments", [result](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (m.read8(s.x[0] + 2) != 0) return result(-1);
        return m.read8(s.x[0] + 1) / 2U;
    });
    p.libsystem("_inet6_rth_getaddr", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto index = static_cast<std::int32_t>(s.x[1]);
        if (m.read8(s.x[0] + 2) != 0 || index < 0 || index >= m.read8(s.x[0] + 1) / 2) return 0;
        return s.x[0] + 8 + 16ULL * static_cast<std::uint32_t>(index);
    }, 0);
    // int inet6_rth_reverse(const void* in, void* out): the addresses in reverse order, all left (in may be out).
    p.libsystem("_inet6_rth_reverse", [result](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (m.read8(s.x[0] + 2) != 0) return result(-1);
        const auto segments = m.read8(s.x[0] + 1) / 2U;
        std::vector<std::uint8_t> addresses(16 * segments);
        if (segments) m.read_bytes(s.x[0] + 8, addresses);
        const auto header = std::array<std::uint8_t, 2>{m.read8(s.x[0]), m.read8(s.x[0] + 1)};
        m.fill_bytes(s.x[1], 0, 8);
        m.write8(s.x[1], header[0]); m.write8(s.x[1] + 1, header[1]);
        m.write8(s.x[1] + 3, static_cast<std::uint8_t>(segments));
        for (std::uint32_t index = 0; index < segments; ++index)
            m.write_bytes(s.x[1] + 8 + 16ULL * index, std::span(addresses.data() + 16 * (segments - 1 - index), 16));
        return 0;
    });
    // int inet6_option_space(int nbytes) (RFC 2292): a cmsghdr (12 bytes, 4-aligned on Darwin) and
    // the option with its type and length bytes, padded to 8.
    p.libsystem("_inet6_option_space", [](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto bytes = (static_cast<std::uint32_t>(s.x[0]) + 2 + 7) & ~7U;
        return 12 + ((bytes + 3) & ~3U);
    });

    // ---- /etc/ethers ----
    // int ether_line(const char* line, struct ether_addr* e, char* hostname): 0, or -1.
    p.libsystem("_ether_line", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto f = fields(guest_string(m, s.x[0], 4096));
        if (f.size() < 2) return static_cast<std::uint64_t>(-1);
        const auto address = parse_ether(f[0]);
        if (!address || f[1].size() > 255) return static_cast<std::uint64_t>(-1);
        m.write_bytes(s.x[1], *address);
        put_string(m, s.x[2], f[1]);
        return 0;
    });
    const auto ethers = [read_file](GuestMemory& m) {
        std::vector<std::pair<std::array<std::uint8_t, 6>, std::string>> result;
        if (const auto text = read_file(m, "/etc/ethers"))
            for (const auto& line : lines(*text)) {
                const auto f = fields(line);
                if (f.size() < 2 || f[1].size() > 255) continue;
                if (const auto address = parse_ether(f[0])) result.emplace_back(*address, f[1]);
            }
        return result;
    };
    // ether_ntohost(char* hostname, const struct ether_addr*) / ether_hostton(const char*, struct ether_addr*): 0, or -1.
    p.libsystem("_ether_ntohost", [ethers](CpuState& s, GuestMemory& m) -> std::uint64_t {
        std::array<std::uint8_t, 6> wanted{};
        m.read_bytes(s.x[1], wanted);
        for (const auto& [address, name] : ethers(m))
            if (address == wanted) { put_string(m, s.x[0], name); return 0; }
        return static_cast<std::uint64_t>(-1);
    });
    p.libsystem("_ether_hostton", [ethers](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto wanted = guest_string(m, s.x[0], 1025);
        for (const auto& [address, name] : ethers(m))
            if (name == wanted) { m.write_bytes(s.x[1], address); return 0; }
        return static_cast<std::uint64_t>(-1);
    });
}

} // namespace alchemy::export_fill
