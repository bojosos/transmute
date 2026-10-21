#include "alchemy/host/dns_sd.hpp"

#include "alchemy/host/win32/win32.hpp"

#include <ws2tcpip.h>
#include <windns.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>

// DNS-SD over the Windows DNS-SD client in dnsapi.dll (DnsServiceBrowse,
// DnsServiceResolve, DnsServiceRegister; Windows 10 1809+), resolved at run
// time so an older dnsapi reports not_running instead of failing to load.
//
// Lifetime. Every Windows request carries a context id, not a pointer; the
// callbacks look the id up in a registry that holds the request state until
// Windows reports its final callback (ERROR_CANCELLED after a cancel, the
// completion of a deregistration), because the request structures must stay
// valid until then. Callbacks run under the state's mutex, and cancel takes
// that mutex after clearing them, so none runs once cancel() has returned.
namespace alchemy::host::dns_sd {
namespace {

// ---- dnsapi entry points (resolved at run time) ---------------------------------------------
struct Api {
    decltype(&::DnsServiceBrowse) browse{};
    decltype(&::DnsServiceBrowseCancel) browse_cancel{};
    decltype(&::DnsServiceResolve) resolve{};
    decltype(&::DnsServiceResolveCancel) resolve_cancel{};
    decltype(&::DnsServiceRegister) register_service{};
    decltype(&::DnsServiceDeRegister) deregister{};
    decltype(&::DnsServiceConstructInstance) construct{};
    decltype(&::DnsServiceFreeInstance) free_instance{};
    decltype(&::DnsFree) free_records{};
    bool ok{};
};

const Api& api() {
    static const Api value = [] {
        Api result;
        const auto module = LoadLibraryExW(L"dnsapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return result;
        const auto load = [module](auto& target, const char* name) {
            target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(reinterpret_cast<void*>(GetProcAddress(module, name)));
            return target != nullptr;
        };
        result.ok = load(result.browse, "DnsServiceBrowse") && load(result.browse_cancel, "DnsServiceBrowseCancel")
            && load(result.resolve, "DnsServiceResolve") && load(result.resolve_cancel, "DnsServiceResolveCancel")
            && load(result.register_service, "DnsServiceRegister") && load(result.deregister, "DnsServiceDeRegister")
            && load(result.construct, "DnsServiceConstructInstance") && load(result.free_instance, "DnsServiceFreeInstance")
            && load(result.free_records, "DnsFree");
        return result;
    }();
    return value;
}

std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

std::string without_dot(std::string_view text) {
    return std::string(!text.empty() && text.back() == '.' ? text.substr(0, text.size() - 1) : text);
}

Failure failure_of(DWORD status) {
    switch (status) {
    case ERROR_SUCCESS: return Failure::none;
    case DNS_ERROR_RCODE_NAME_ERROR: case DNS_INFO_NO_RECORDS: return Failure::no_such_record;
    case ERROR_TIMEOUT: case WAIT_TIMEOUT: case DNS_ERROR_RCODE_SERVER_FAILURE: return Failure::timeout;
    case ERROR_INVALID_PARAMETER: return Failure::bad_param;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return Failure::no_memory;
    case ERROR_ACCESS_DENIED: return Failure::refused;
    case ERROR_ALREADY_EXISTS: case DNS_ERROR_RCODE_YXDOMAIN: return Failure::name_conflict;
    default: return Failure::unknown;
    }
}

// Runs `work` on the Windows thread pool.
bool submit(std::function<void()> work) {
    auto* heap = new std::function<void()>(std::move(work));
    const auto ok = TrySubmitThreadpoolCallback([](PTP_CALLBACK_INSTANCE, PVOID context) {
        std::unique_ptr<std::function<void()>> owned(static_cast<std::function<void()>*>(context));
        (*owned)();
    }, heap, nullptr);
    if (!ok) delete heap;
    return ok != FALSE;
}

} // namespace

// ---- request registry ----------------------------------------------------------------------
struct Operation::State {
    enum class Kind { browse, resolve, registration, immediate };
    Kind kind{Kind::immediate};
    std::mutex mutex;
    // Cleared by cancel.
    BrowseCallback on_browse;
    ResolveCallback on_resolve;
    RegisterCallback on_register;
    std::function<void()> on_domain;
    AddressCallback on_addresses;
    std::uint64_t id{};
    // Request structures and strings Windows reads until the final callback.
    std::wstring query;
    DNS_SERVICE_CANCEL cancel_handle{};
    DNS_SERVICE_BROWSE_REQUEST browse{};
    DNS_SERVICE_RESOLVE_REQUEST resolve{};
    DNS_SERVICE_REGISTER_REQUEST registration{};
    PDNS_SERVICE_INSTANCE instance{};
    bool completed{};                // resolve: the one result arrived
    bool registered{}, withdrawing{}, withdraw_when_registered{};
    ~State() { if (instance && api().free_instance) api().free_instance(instance); }
    void clear() {
        on_browse = nullptr;
        on_resolve = nullptr;
        on_register = nullptr;
        on_domain = nullptr;
        on_addresses = nullptr;
    }
};

namespace {

using State = Operation::State;

std::mutex registry_mutex;
std::map<std::uint64_t, std::shared_ptr<State>> registry;
std::atomic<std::uint64_t> next_id{1};

std::shared_ptr<State> make_state(State::Kind kind) {
    auto state = std::make_shared<State>();
    state->kind = kind;
    state->id = next_id++;
    std::lock_guard lock(registry_mutex);
    registry[state->id] = state;
    return state;
}
std::shared_ptr<State> find_state(void* context) {
    std::lock_guard lock(registry_mutex);
    const auto found = registry.find(reinterpret_cast<std::uintptr_t>(context));
    return found == registry.end() ? nullptr : found->second;
}
void forget_state(std::uint64_t id) {
    std::shared_ptr<State> keep;   // destroyed outside the registry lock
    std::lock_guard lock(registry_mutex);
    if (const auto found = registry.find(id); found != registry.end()) { keep = std::move(found->second); registry.erase(found); }
}
PVOID context_of(const State& state) { return reinterpret_cast<PVOID>(static_cast<std::uintptr_t>(state.id)); }

void WINAPI on_browse(DWORD status, PVOID context, PDNS_RECORD records) {
    const auto state = find_state(context);
    struct Free { PDNS_RECORD r; ~Free() { if (r && api().free_records) api().free_records(r, DnsFreeRecordList); } } release{records};
    if (!state) return;
    if (status == ERROR_CANCELLED) { forget_state(state->id); return; }
    std::lock_guard lock(state->mutex);
    if (!state->on_browse) return;
    if (status != ERROR_SUCCESS) {
        state->on_browse(failure_of(status), {});
        return;
    }
    std::vector<Pointer> pointers;
    const auto query = lower(without_dot(win32::narrow(state->query)));
    for (auto record = reinterpret_cast<DNS_RECORDW*>(records); record; record = record->pNext) {
        if (record->wType != DNS_TYPE_PTR || !record->pName || !record->Data.PTR.pNameHost) continue;
        if (lower(without_dot(win32::narrow(record->pName))) != query) continue;
        pointers.push_back({win32::narrow(record->Data.PTR.pNameHost), record->dwTtl == 0});
    }
    if (!pointers.empty()) state->on_browse(Failure::none, pointers);
}

void WINAPI on_resolve(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance) {
    const auto state = find_state(context);
    struct Free { PDNS_SERVICE_INSTANCE i; ~Free() { if (i && api().free_instance) api().free_instance(i); } } release{instance};
    if (!state) return;
    {
        std::lock_guard lock(state->mutex);
        state->completed = true;
        if (state->on_resolve && status != ERROR_CANCELLED) {
            if (status == ERROR_SUCCESS && instance) {
                Instance result;
                result.host = instance->pszHostName ? without_dot(win32::narrow(instance->pszHostName)) : std::string{};
                result.port = instance->wPort;
                result.interface_index = instance->dwInterfaceIndex;
                for (DWORD index = 0; index < instance->dwPropertyCount; ++index) {
                    if (!instance->keys || !instance->keys[index]) continue;
                    std::optional<std::string> value;
                    if (instance->values && instance->values[index]) value = win32::narrow(instance->values[index]);
                    result.properties.emplace_back(win32::narrow(instance->keys[index]), std::move(value));
                }
                if (instance->ip4Address) {
                    std::array<std::uint8_t, 4> bytes{};
                    std::memcpy(bytes.data(), instance->ip4Address, 4);   // IP4_ADDRESS is in network order
                    result.ipv4 = bytes;
                }
                if (instance->ip6Address) {
                    std::array<std::uint8_t, 16> bytes{};
                    std::memcpy(bytes.data(), instance->ip6Address, 16);
                    result.ipv6 = bytes;
                }
                state->on_resolve(Failure::none, &result);
            } else {
                state->on_resolve(status == ERROR_SUCCESS ? Failure::no_such_record : failure_of(status), nullptr);
            }
        }
    }
    forget_state(state->id);   // a resolve calls back exactly once
}

void WINAPI on_register(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance) {
    const auto state = find_state(context);
    struct Free { PDNS_SERVICE_INSTANCE i; ~Free() { if (i && api().free_instance) api().free_instance(i); } } release{instance};
    if (!state) return;
    bool withdraw_now{}, done{};
    {
        std::lock_guard lock(state->mutex);
        if (state->withdrawing) {
            done = true;   // the deregistration finished
        } else if (state->withdraw_when_registered) {
            withdraw_now = status == ERROR_SUCCESS;
            done = !withdraw_now;
            state->withdrawing = withdraw_now;
        } else {
            state->registered = status == ERROR_SUCCESS;
            if (status != ERROR_SUCCESS) done = true;
            if (state->on_register) {
                if (status == ERROR_SUCCESS)
                    state->on_register(Failure::none, instance && instance->pszInstanceName ? win32::narrow(instance->pszInstanceName)
                                                                                          : win32::narrow(state->query));
                else
                    state->on_register(failure_of(status), {});
            }
        }
    }
    if (withdraw_now && api().deregister(&state->registration, nullptr) != DNS_REQUEST_PENDING) done = true;
    if (done) forget_state(state->id);
}

} // namespace

Operation::Operation() = default;
Operation::Operation(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}
Operation::Operation(Operation&&) noexcept = default;
Operation& Operation::operator=(Operation&& other) noexcept {
    if (this != &other) {
        cancel();
        state_ = std::move(other.state_);
    }
    return *this;
}
Operation::~Operation() { cancel(); }

void Operation::cancel() noexcept {
    if (!state_) return;
    const auto state = std::exchange(state_, nullptr);
    bool forget{}, deregister{};
    {
        std::lock_guard lock(state->mutex);
        state->clear();
        switch (state->kind) {
        case State::Kind::browse: break;
        case State::Kind::resolve: forget = state->completed; break;
        case State::Kind::registration:
            if (state->withdrawing || state->withdraw_when_registered) break;
            if (state->registered) { state->withdrawing = true; deregister = true; }
            else state->withdraw_when_registered = true;   // withdrawn when the registration completes
            break;
        case State::Kind::immediate: forget = true; break;
        }
    }
    switch (state->kind) {
    case State::Kind::browse:
        if (api().browse_cancel(&state->cancel_handle) != ERROR_SUCCESS) forget = true;
        break;
    case State::Kind::resolve:
        if (!forget && api().resolve_cancel(&state->cancel_handle) != ERROR_SUCCESS) forget = true;
        break;
    case State::Kind::registration:
        if (deregister && api().deregister(&state->registration, nullptr) != DNS_REQUEST_PENDING) forget = true;
        break;
    case State::Kind::immediate: break;
    }
    if (forget) forget_state(state->id);
}

Failure available() { return api().ok ? Failure::none : Failure::not_running; }

Operation browse(std::string_view query, std::uint32_t interface_index, BrowseCallback callback, Failure& failure) {
    if ((failure = available()) != Failure::none) return {};
    auto state = make_state(State::Kind::browse);
    state->on_browse = std::move(callback);
    state->query = win32::widen(query);
    state->browse.Version = DNS_QUERY_REQUEST_VERSION1;
    state->browse.InterfaceIndex = interface_index;
    state->browse.QueryName = state->query.c_str();
    state->browse.pBrowseCallback = on_browse;
    state->browse.pQueryContext = context_of(*state);
    const auto result = api().browse(&state->browse, &state->cancel_handle);
    if (result != DNS_REQUEST_PENDING) {
        forget_state(state->id);
        failure = failure_of(result == ERROR_SUCCESS ? ERROR_INVALID_PARAMETER : result);
        return {};
    }
    return Operation(state);
}

Operation browse_domains(std::function<void()> callback, Failure& failure) {
    if ((failure = available()) != Failure::none) return {};
    auto state = make_state(State::Kind::immediate);
    state->on_domain = std::move(callback);
    const std::weak_ptr<State> weak = state;
    const auto queued = submit([weak] {
        const auto alive = weak.lock();
        if (!alive) return;
        std::lock_guard lock(alive->mutex);
        if (alive->on_domain) alive->on_domain();
    });
    if (!queued) { forget_state(state->id); failure = Failure::no_memory; return {}; }
    return Operation(state);
}

Operation resolve(std::string_view full_name, std::uint32_t interface_index, ResolveCallback callback, Failure& failure) {
    if ((failure = available()) != Failure::none) return {};
    auto state = make_state(State::Kind::resolve);
    state->on_resolve = std::move(callback);
    state->query = win32::widen(full_name);
    state->resolve.Version = DNS_QUERY_REQUEST_VERSION1;
    state->resolve.InterfaceIndex = interface_index;
    state->resolve.QueryName = state->query.data();
    state->resolve.pResolveCompletionCallback = on_resolve;
    state->resolve.pQueryContext = context_of(*state);
    const auto result = api().resolve(&state->resolve, &state->cancel_handle);
    if (result != DNS_REQUEST_PENDING) {
        forget_state(state->id);
        failure = failure_of(result == ERROR_SUCCESS ? ERROR_INVALID_PARAMETER : result);
        return {};
    }
    return Operation(state);
}

Operation register_service(const Registration& registration, RegisterCallback callback, Failure& failure) {
    if ((failure = available()) != Failure::none) return {};
    auto state = make_state(State::Kind::registration);
    state->on_register = std::move(callback);
    state->query = win32::widen(registration.full_name);
    const auto host = win32::widen(registration.host);
    std::vector<std::wstring> keys, values;
    for (const auto& [key, value] : registration.properties) {
        keys.push_back(win32::widen(key));
        values.push_back(value ? win32::widen(*value) : std::wstring{});
    }
    std::vector<PCWSTR> key_pointers, value_pointers;
    for (std::size_t index = 0; index < keys.size(); ++index) {
        key_pointers.push_back(keys[index].c_str());
        value_pointers.push_back(values[index].c_str());
    }
    state->instance = api().construct(state->query.c_str(), host.c_str(), nullptr, nullptr, registration.port, 0, 0,
                                      static_cast<DWORD>(keys.size()), keys.empty() ? nullptr : key_pointers.data(),
                                      values.empty() ? nullptr : value_pointers.data());
    if (!state->instance) { forget_state(state->id); failure = Failure::no_memory; return {}; }
    state->instance->dwInterfaceIndex = registration.interface_index;
    state->registration.Version = DNS_QUERY_REQUEST_VERSION1;
    state->registration.InterfaceIndex = registration.interface_index;
    state->registration.pServiceInstance = state->instance;
    state->registration.pRegisterCompletionCallback = on_register;
    state->registration.pQueryContext = context_of(*state);
    state->registration.unicastEnabled = FALSE;
    const auto result = api().register_service(&state->registration, nullptr);
    if (result != DNS_REQUEST_PENDING) {
        forget_state(state->id);
        failure = failure_of(result == ERROR_SUCCESS ? ERROR_INVALID_PARAMETER : result);
        return {};
    }
    return Operation(state);
}

Operation get_addr_info(std::string_view host, std::uint32_t protocols, AddressCallback callback, Failure& failure) {
    failure = Failure::none;
    auto state = make_state(State::Kind::immediate);
    state->on_addresses = std::move(callback);
    const std::weak_ptr<State> weak = state;
    const auto queued = submit([weak, query = win32::widen(without_dot(host)), protocols] {
        ADDRINFOW hints{};
        hints.ai_family = (protocols & 3) == 1 ? AF_INET : (protocols & 3) == 2 ? AF_INET6 : AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        ADDRINFOW* list{};
        const auto error = GetAddrInfoW(query.c_str(), nullptr, &hints, &list);
        std::vector<net::Address> addresses;
        for (auto* item = error == 0 ? list : nullptr; item; item = item->ai_next) {
            net::Address address;
            if (item->ai_family == AF_INET) {
                std::array<std::uint8_t, 4> bytes{};
                std::memcpy(bytes.data(), &reinterpret_cast<const sockaddr_in*>(item->ai_addr)->sin_addr, 4);
                address = net::Address::ipv4(bytes);
            } else if (item->ai_family == AF_INET6) {
                const auto* v6 = reinterpret_cast<const sockaddr_in6*>(item->ai_addr);
                std::array<std::uint8_t, 16> bytes{};
                std::memcpy(bytes.data(), &v6->sin6_addr, 16);
                address = net::Address::ipv6(bytes, 0, v6->sin6_scope_id);
            } else {
                continue;
            }
            if (std::find(addresses.begin(), addresses.end(), address) == addresses.end()) addresses.push_back(std::move(address));
        }
        if (list) FreeAddrInfoW(list);
        const auto alive = weak.lock();
        if (!alive) return;
        std::lock_guard lock(alive->mutex);
        if (alive->on_addresses) alive->on_addresses(addresses);
    });
    if (!queued) { forget_state(state->id); failure = Failure::no_memory; return {}; }
    return Operation(state);
}

} // namespace alchemy::host::dns_sd
