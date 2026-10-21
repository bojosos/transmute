#include "alchemy/darwin/network/mdns.hpp"
#include "alchemy/host/dns_sd.hpp"
#include "alchemy/host/network_policy.hpp"
#include "alchemy/host/system.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

// Host mDNS engine over host::dns_sd (see mdns.hpp): the dns_sd names, TXT
// records, Darwin sockaddrs, kDNSServiceErr_* codes and the network policy
// are here; the requests and their lifetimes are the host's.
namespace alchemy::gui::network::mdns {
namespace {

using host::dns_sd::Failure;

std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

std::string without_dot(std::string_view text) {
    return std::string(!text.empty() && text.back() == '.' ? text.substr(0, text.size() - 1) : text);
}

// dns_sd label unescaping ("\." "\\" "\DDD").
std::string unescape_label(std::string_view text) {
    std::string result;
    for (std::size_t at = 0; at < text.size(); ++at) {
        if (text[at] != '\\' || at + 1 >= text.size()) { result += text[at]; continue; }
        const auto digit = [&](std::size_t index) { return std::isdigit(static_cast<unsigned char>(text[index])) != 0; };
        if (at + 3 < text.size() && digit(at + 1) && digit(at + 2) && digit(at + 3)) {
            const int value = (text[at + 1] - '0') * 100 + (text[at + 2] - '0') * 10 + (text[at + 3] - '0');
            if (value < 256) { result += static_cast<char>(value); at += 3; continue; }
        }
        result += text[++at];
    }
    return result;
}

std::int32_t error_of(Failure failure) {
    switch (failure) {
    case Failure::none: return error::none;
    case Failure::not_running: return error::service_not_running;
    case Failure::no_such_record: return error::no_such_record;
    case Failure::timeout: return error::timeout;
    case Failure::bad_param: return error::bad_param;
    case Failure::no_memory: return error::no_memory;
    case Failure::refused: return error::refused;
    case Failure::name_conflict: return error::name_conflict;
    case Failure::unknown: return error::unknown;
    }
    return error::unknown;
}

// Darwin sockaddr_in / sockaddr_in6 (BSD layout with sin_len; AF_INET6 = 30).
Bytes darwin_sockaddr4(const std::uint8_t (&address)[4], std::uint16_t port) {
    Bytes result(16, 0);
    result[0] = 16; result[1] = 2;
    result[2] = static_cast<std::uint8_t>(port >> 8); result[3] = static_cast<std::uint8_t>(port);
    std::memcpy(result.data() + 4, address, 4);
    return result;
}
Bytes darwin_sockaddr6(const std::uint8_t (&address)[16], std::uint16_t port, std::uint32_t scope) {
    Bytes result(28, 0);
    result[0] = 28; result[1] = 30;
    result[2] = static_cast<std::uint8_t>(port >> 8); result[3] = static_cast<std::uint8_t>(port);
    std::memcpy(result.data() + 8, address, 16);
    std::memcpy(result.data() + 24, &scope, 4);   // host (little-endian) order, as Darwin stores sin6_scope_id
    return result;
}

std::string instance_label(std::string_view full, const std::string& type) {
    // "<instance>.<type>local" where type is "_x._tcp." (case-insensitive suffix).
    const auto suffix = "." + type + "local";
    auto body = without_dot(full);
    if (body.size() > suffix.size() && lower(body.substr(body.size() - suffix.size())) == lower(suffix))
        return unescape_label(body.substr(0, body.size() - suffix.size()));
    const auto first = body.find("._");
    return unescape_label(first == std::string::npos ? body : body.substr(0, first));
}

class Request final : public Operation {
public:
    explicit Request(host::dns_sd::Operation operation) : operation_(std::move(operation)) {}
    void cancel() override { operation_.cancel(); }
private:
    host::dns_sd::Operation operation_;
};

std::unique_ptr<Operation> started(host::dns_sd::Operation operation, Failure failure, std::int32_t& status) {
    status = error_of(failure);
    if (!operation) {
        if (status == error::none) status = error::unknown;
        return nullptr;
    }
    return std::make_unique<Request>(std::move(operation));
}

std::string full_name(const std::string& name, const std::string& type) {
    return escape_label(name) + "." + type + "local";
}

} // namespace

std::int32_t available() { return error_of(host::dns_sd::available()); }

std::optional<std::string> normalize_type(std::string_view type) {
    std::string text = without_dot(type);
    const auto dot = text.find('.');
    if (dot == std::string::npos || text.find(',') != std::string::npos) return std::nullopt;
    const auto service = text.substr(0, dot), protocol = lower(text.substr(dot + 1));
    if (service.size() < 2 || service[0] != '_' || service.size() > 16) return std::nullopt;
    if (protocol != "_tcp" && protocol != "_udp") return std::nullopt;
    for (const auto c : service.substr(1))
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') return std::nullopt;
    return service + "." + protocol + ".";
}

bool is_local_domain(std::string_view domain) {
    const auto text = lower(without_dot(domain));
    return text.empty() || text == "local";
}

std::string escape_label(std::string_view label) {
    std::string result;
    for (const auto c : label) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '.' || c == '\\') { result += '\\'; result += c; }
        else if (byte < 0x20 || byte == 0x7f) {
            result += '\\';
            result += static_cast<char>('0' + byte / 100);
            result += static_cast<char>('0' + byte / 10 % 10);
            result += static_cast<char>('0' + byte % 10);
        } else result += c;
    }
    return result;
}

std::string default_service_name() {
    const auto name = host::system::host_name(false);
    return name && !name->empty() ? *name : "Alchemy";
}

std::string local_host_name() { return default_service_name() + ".local."; }

std::optional<std::vector<TxtEntry>> parse_txt(std::span<const std::uint8_t> wire) {
    std::vector<TxtEntry> result;
    std::size_t at = 0;
    while (at < wire.size()) {
        const std::size_t length = wire[at++];
        if (at + length > wire.size()) return std::nullopt;
        const std::string_view item(reinterpret_cast<const char*>(wire.data() + at), length);
        at += length;
        if (item.empty()) continue;   // the empty TXT record is one zero-length string
        const auto equals = item.find('=');
        if (equals == 0) continue;    // a string without a key is ignored (RFC 6763 6.4)
        TxtEntry entry{std::string(item.substr(0, equals)), std::nullopt};
        if (equals != std::string_view::npos) {
            const auto value = item.substr(equals + 1);
            entry.value = Bytes(value.begin(), value.end());
        }
        result.push_back(std::move(entry));
    }
    return result;
}

Bytes build_txt(const std::vector<TxtEntry>& entries) {
    Bytes result;
    for (const auto& entry : entries) {
        std::size_t length = entry.key.size() + (entry.value ? 1 + entry.value->size() : 0);
        if (length > 255 || entry.key.empty()) continue;
        result.push_back(static_cast<std::uint8_t>(length));
        result.insert(result.end(), entry.key.begin(), entry.key.end());
        if (entry.value) {
            result.push_back('=');
            result.insert(result.end(), entry.value->begin(), entry.value->end());
        }
    }
    if (result.empty()) result.push_back(0);   // the empty TXT record
    return result;
}

std::unique_ptr<Operation> browse(const std::string& type, std::uint32_t interface_index, Sink sink, std::int32_t& status) {
    if (network_policy::loopback_only()) { network_policy::deny("DNSServiceBrowse", type); status = error::refused; return nullptr; }
    if ((status = available()) != error::none) return nullptr;
    const auto normalized = normalize_type(type);
    if (!normalized) { status = error::bad_param; return nullptr; }
    // Known instances (lower case) and their display names; batches arrive one at a time.
    auto known = std::make_shared<std::map<std::string, std::string>>();
    auto callback = [sink = std::move(sink), known, type = *normalized, interface_index](Failure failure,
                                                                                         const std::vector<host::dns_sd::Pointer>& pointers) {
        if (failure != Failure::none) {
            Event event;
            event.error = error_of(failure);
            sink(std::move(event));
            return;
        }
        std::vector<Event> batch;
        for (const auto& pointer : pointers) {
            const auto name = instance_label(pointer.target, type);
            const auto key = lower(name);
            Event event;
            event.name = name; event.type = type; event.domain = "local."; event.interface_index = interface_index;
            if (pointer.goodbye) {
                if (!known->erase(key)) continue;
                event.kind = Event::Kind::lost;
            } else {
                if (!known->emplace(key, name).second) continue;
                event.kind = Event::Kind::found;
            }
            batch.push_back(std::move(event));
        }
        for (std::size_t index = 0; index < batch.size(); ++index) {
            batch[index].more_coming = index + 1 < batch.size();
            sink(std::move(batch[index]));
        }
    };
    Failure failure{};
    auto operation = host::dns_sd::browse(*normalized + "local", interface_index, std::move(callback), failure);
    return started(std::move(operation), failure, status);
}

std::unique_ptr<Operation> browse_domains(Sink sink, std::int32_t& status) {
    if (network_policy::loopback_only()) { network_policy::deny("DNSServiceEnumerateDomains", "local."); status = error::refused; return nullptr; }
    if ((status = available()) != error::none) return nullptr;
    Failure failure{};
    auto operation = host::dns_sd::browse_domains([sink = std::move(sink)] {
        Event event;
        event.kind = Event::Kind::domain;
        event.name = event.domain = "local.";
        sink(std::move(event));
    }, failure);
    return started(std::move(operation), failure, status);
}

std::unique_ptr<Operation> resolve(const std::string& name, const std::string& type, std::uint32_t interface_index, Sink sink,
                                   std::int32_t& status) {
    if (network_policy::loopback_only()) { network_policy::deny("DNSServiceResolve", name + "." + type); status = error::no_such_name; return nullptr; }
    if ((status = available()) != error::none) return nullptr;
    const auto normalized = normalize_type(type);
    if (!normalized || name.empty()) { status = error::bad_param; return nullptr; }
    const auto query = full_name(name, *normalized);
    auto callback = [sink = std::move(sink), label = instance_label(query, *normalized), type = *normalized, interface_index](
                        Failure failure, const host::dns_sd::Instance* instance) {
        if (!instance) {
            Event event;
            event.error = error_of(failure == Failure::none ? Failure::no_such_record : failure);
            sink(std::move(event));
            return;
        }
        Event event;
        event.kind = Event::Kind::resolved;
        event.name = label;
        event.type = type;
        event.domain = "local.";
        event.interface_index = instance->interface_index ? instance->interface_index : interface_index;
        event.host = instance->host.empty() ? std::string{} : instance->host + ".";
        event.port = instance->port;
        std::vector<TxtEntry> entries;
        for (const auto& [key, value] : instance->properties) {
            TxtEntry entry{key, std::nullopt};
            if (value) entry.value = Bytes(value->begin(), value->end());
            entries.push_back(std::move(entry));
        }
        event.txt = build_txt(entries);
        if (instance->ipv4) {
            std::uint8_t bytes[4];
            std::memcpy(bytes, instance->ipv4->data(), 4);
            event.addresses.push_back(darwin_sockaddr4(bytes, event.port));
        }
        if (instance->ipv6) {
            std::uint8_t bytes[16];
            std::memcpy(bytes, instance->ipv6->data(), 16);
            const bool link_local = bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80;
            event.addresses.push_back(darwin_sockaddr6(bytes, event.port, link_local ? event.interface_index : 0));
        }
        sink(std::move(event));
    };
    Failure failure{};
    auto operation = host::dns_sd::resolve(query, interface_index, std::move(callback), failure);
    return started(std::move(operation), failure, status);
}

std::unique_ptr<Operation> register_service(const std::string& name, const std::string& type, std::uint16_t port,
                                            std::span<const std::uint8_t> txt, std::uint32_t interface_index, Sink sink,
                                            std::int32_t& status) {
    if (network_policy::loopback_only()) { network_policy::deny("DNSServiceRegister", name + "." + type); status = error::refused; return nullptr; }
    if ((status = available()) != error::none) return nullptr;
    const auto normalized = normalize_type(type);
    const auto entries = parse_txt(txt);
    if (!normalized || !entries) { status = error::bad_param; return nullptr; }
    host::dns_sd::Registration registration;
    registration.full_name = full_name(name.empty() ? default_service_name() : name, *normalized);
    registration.host = without_dot(local_host_name());
    registration.port = port;
    registration.interface_index = interface_index;
    for (const auto& entry : *entries) {
        std::optional<std::string> value;
        if (entry.value) value = std::string(entry.value->begin(), entry.value->end());
        registration.properties.emplace_back(entry.key, std::move(value));
    }
    auto callback = [sink = std::move(sink), type = *normalized](Failure failure, std::string_view registered) {
        Event event;
        if (failure == Failure::none) {
            event.kind = Event::Kind::registered;
            event.name = instance_label(registered, type);
            event.type = type;
            event.domain = "local.";
        } else {
            event.error = error_of(failure);
        }
        sink(std::move(event));
    };
    Failure failure{};
    auto operation = host::dns_sd::register_service(registration, std::move(callback), failure);
    return started(std::move(operation), failure, status);
}

std::unique_ptr<Operation> get_addr_info(const std::string& host_name, std::uint32_t protocols, std::uint32_t interface_index,
                                         Sink sink, std::int32_t& status) {
    if (host_name.empty()) { status = error::bad_param; return nullptr; }
    if (network_policy::loopback_only() && !network_policy::loopback_host(host_name) && !network_policy::numeric_host(host_name)) {
        network_policy::deny("DNSServiceGetAddrInfo", host_name);
        status = error::no_such_name;
        return nullptr;
    }
    if (network_policy::refuse_listed_host("DNSServiceGetAddrInfo", host_name)) {   // ALCHEMY_NETWORK_DENY
        status = error::no_such_name;
        return nullptr;
    }
    network_policy::allowed("DNSServiceGetAddrInfo", host_name);
    auto callback = [sink = std::move(sink), host_name, interface_index](const std::vector<host::net::Address>& addresses) {
        std::vector<Event> events;
        for (const auto& address : addresses) {
            Event event;
            event.kind = Event::Kind::address;
            event.host = host_name;
            event.ttl = 120;
            event.interface_index = interface_index;
            if (address.family == host::net::Family::ipv4) {
                std::uint8_t bytes[4];
                std::memcpy(bytes, address.bytes.data(), 4);
                event.addresses.push_back(darwin_sockaddr4(bytes, 0));
            } else if (address.family == host::net::Family::ipv6) {
                std::uint8_t bytes[16];
                std::memcpy(bytes, address.bytes.data(), 16);
                event.addresses.push_back(darwin_sockaddr6(bytes, 0, address.scope_id));
            } else {
                continue;
            }
            events.push_back(std::move(event));
        }
        if (events.empty()) {
            Event event;
            event.error = error::no_such_record;
            event.host = host_name;
            sink(std::move(event));
            return;
        }
        for (std::size_t index = 0; index < events.size(); ++index) {
            events[index].more_coming = index + 1 < events.size();
            sink(std::move(events[index]));
        }
    };
    Failure failure{};
    auto operation = host::dns_sd::get_addr_info(host_name, protocols, std::move(callback), failure);
    return started(std::move(operation), failure, status);
}

} // namespace alchemy::gui::network::mdns
