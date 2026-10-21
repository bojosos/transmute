// Host area "net", the parts every backend shares: socket addresses as values
// (include/alchemy/host/net.hpp). Plain C++, no OS calls.
#include "alchemy/host/net.hpp"

#include <algorithm>
#include <charconv>

namespace alchemy::host::net {
namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return out;
}

// A dotted-quad IPv4 literal (four decimal parts, each 0..255, no leading zeros).
bool parse_ipv4(std::string_view text, std::uint8_t* out) {
    for (int part = 0; part < 4; ++part) {
        if (part) {
            if (text.empty() || text.front() != '.') return false;
            text.remove_prefix(1);
        }
        std::size_t digits = 0;
        while (digits < text.size() && digits < 4 && text[digits] >= '0' && text[digits] <= '9') ++digits;
        if (digits == 0 || digits > 3 || (digits > 1 && text.front() == '0')) return false;
        unsigned value = 0;
        std::from_chars(text.data(), text.data() + digits, value);
        if (value > 255) return false;
        out[part] = static_cast<std::uint8_t>(value);
        text.remove_prefix(digits);
    }
    return text.empty();
}

// An IPv6 literal (RFC 4291 2.2: "::" compression, a trailing dotted quad).
bool parse_ipv6(std::string_view text, std::uint8_t* out) {
    std::uint16_t groups[8]{};
    int count = 0, gap = -1;
    if (text.starts_with("::")) {
        gap = 0;
        text.remove_prefix(2);
        if (text.empty()) { std::fill_n(out, 16, std::uint8_t{0}); return true; }
    }
    while (!text.empty()) {
        if (count == 8) return false;
        const auto colon = text.find(':');
        const auto part = text.substr(0, colon);
        if (part.find('.') != std::string_view::npos) {
            if (colon != std::string_view::npos || count > 6) return false;
            std::uint8_t quad[4];
            if (!parse_ipv4(part, quad)) return false;
            groups[count++] = static_cast<std::uint16_t>(quad[0] << 8 | quad[1]);
            groups[count++] = static_cast<std::uint16_t>(quad[2] << 8 | quad[3]);
            text = {};
            break;
        }
        if (part.empty() || part.size() > 4) return false;
        unsigned value = 0;
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value, 16);
        if (error != std::errc{} || end != part.data() + part.size()) return false;
        groups[count++] = static_cast<std::uint16_t>(value);
        if (colon == std::string_view::npos) { text = {}; break; }
        text.remove_prefix(colon + 1);
        if (text.starts_with(":")) {
            if (gap >= 0) return false;
            gap = count;
            text.remove_prefix(1);
        } else if (text.empty()) {
            return false;   // a trailing single colon
        }
    }
    if (gap < 0 ? count != 8 : count > 7) return false;
    std::uint16_t full[8]{};
    if (gap < 0) {
        std::copy_n(groups, 8, full);
    } else {
        std::copy_n(groups, gap, full);
        std::copy(groups + gap, groups + count, full + 8 - (count - gap));
    }
    for (int index = 0; index < 8; ++index) {
        out[index * 2] = static_cast<std::uint8_t>(full[index] >> 8);
        out[index * 2 + 1] = static_cast<std::uint8_t>(full[index]);
    }
    return true;
}

std::string format_ipv4(const std::uint8_t* bytes) {
    std::string text;
    for (int index = 0; index < 4; ++index) {
        if (index) text += '.';
        text += std::to_string(bytes[index]);
    }
    return text;
}

// RFC 5952: lower-case, the longest run (two or more) of zero groups as "::",
// IPv4-mapped addresses with a dotted quad.
std::string format_ipv6(const std::uint8_t* bytes) {
    std::uint16_t groups[8];
    for (int index = 0; index < 8; ++index) groups[index] = static_cast<std::uint16_t>(bytes[index * 2] << 8 | bytes[index * 2 + 1]);
    int best = -1, best_length = 1;
    for (int index = 0; index < 8;) {
        if (groups[index]) { ++index; continue; }
        int end = index;
        while (end < 8 && !groups[end]) ++end;
        if (end - index > best_length) { best = index; best_length = end - index; }
        index = end;
    }
    const bool mapped = best == 0 && best_length == 5 && groups[5] == 0xffff;
    std::string text;
    char digits[8];
    for (int index = 0; index < 8; ++index) {
        if (index == best) {
            text += "::";
            index += best_length - 1;
            if (mapped) return text + "ffff:" + format_ipv4(bytes + 12);
            continue;
        }
        if (!text.empty() && text.back() != ':') text += ':';
        const auto [end, error] = std::to_chars(digits, digits + sizeof(digits), groups[index], 16);
        text.append(digits, end);
    }
    return text;
}

} // namespace

Address Address::ipv4(std::array<std::uint8_t, 4> bytes, std::uint16_t port) noexcept {
    Address address;
    address.family = Family::ipv4;
    address.port = port;
    std::copy(bytes.begin(), bytes.end(), address.bytes.begin());
    return address;
}

Address Address::ipv6(std::array<std::uint8_t, 16> bytes, std::uint16_t port, std::uint32_t scope_id) noexcept {
    Address address;
    address.family = Family::ipv6;
    address.port = port;
    address.bytes = bytes;
    address.scope_id = scope_id;
    return address;
}

Address Address::local(std::string_view path) {
    Address address;
    address.family = Family::local;
    address.path = path;
    return address;
}

bool is_loopback(const Address& address) noexcept {
    switch (address.family) {
    case Family::ipv4: return address.bytes[0] == 127;
    case Family::ipv6: {
        static constexpr std::uint8_t one[16]{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        static constexpr std::uint8_t mapped[12]{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        return std::equal(one, one + 16, address.bytes.begin()) ||
               (std::equal(mapped, mapped + 12, address.bytes.begin()) && address.bytes[12] == 127);
    }
    case Family::local: return true;
    case Family::unspecified: return false;
    }
    return false;
}

std::optional<Address> parse_numeric(std::string_view host, std::uint16_t port) {
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    std::array<std::uint8_t, 4> v4{};
    if (parse_ipv4(host, v4.data())) return Address::ipv4(v4, port);
    std::uint32_t scope = 0;
    if (const auto percent = host.find('%'); percent != std::string_view::npos) {
        // fe80::1%7: a numeric zone is the scope id; a named zone (fe80::1%en0) is kept as 0.
        const auto zone = host.substr(percent + 1);
        if (zone.empty()) return std::nullopt;
        std::from_chars(zone.data(), zone.data() + zone.size(), scope);
        host = host.substr(0, percent);
    }
    std::array<std::uint8_t, 16> v6{};
    if (parse_ipv6(host, v6.data())) return Address::ipv6(v6, port, scope);
    return std::nullopt;
}

bool loopback_host(std::string_view host) {
    auto text = lower(host);
    while (!text.empty() && text.back() == '.') text.pop_back();
    if (text == "localhost" || (text.size() > 10 && text.ends_with(".localhost"))) return true;
    const auto address = parse_numeric(text);
    return address && is_loopback(*address);
}

std::string host_text(const Address& address) {
    switch (address.family) {
    case Family::ipv4: return format_ipv4(address.bytes.data());
    case Family::ipv6: return format_ipv6(address.bytes.data());
    case Family::local: return address.path;
    case Family::unspecified: return {};
    }
    return {};
}

std::string to_string(const Address& address) {
    switch (address.family) {
    case Family::ipv4: return host_text(address) + ":" + std::to_string(address.port);
    case Family::ipv6: return "[" + host_text(address) + "]:" + std::to_string(address.port);
    case Family::local: return address.path.empty() ? "(unnamed local socket)" : address.path;
    case Family::unspecified: return "(none)";
    }
    return "(none)";
}

} // namespace alchemy::host::net
