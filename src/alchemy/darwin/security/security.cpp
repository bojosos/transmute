#include "alchemy/darwin/security/security_internal.hpp"

#include "alchemy/darwin/security/keys.hpp"

// Security.framework provider entry point and shared object helpers.
// Identity: /System/Library/Frameworks/Security.framework/Versions/A/Security
// (listed in darwin_framework_libraries). Behaviour follows Apple's public
// Security documentation over Windows CryptoAPI / CNG / SChannel / DPAPI;
// no Apple code is used.
namespace alchemy::gui::security {
namespace {
std::mutex instance_mutex;
std::shared_ptr<Security> instance;
} // namespace

std::shared_ptr<Security> security() {
    std::lock_guard lock(instance_mutex);
    if (!instance) fail("the Security provider is not installed");
    return instance;
}

Address Security::export_constant(std::string_view name, std::string_view value, std::string_view library) {
    std::lock_guard lock(mutex_);
    if (const auto found = constants_.find(name); found != constants_.end()) return found->second;
    Address object{};
    if (const auto existing = by_value_.find(value); existing != by_value_.end()) object = existing->second;
    else {
        object = b->cf.constant_string(value);
        by_value_.emplace(std::string(value), object);
    }
    b->export_value(library, "_" + std::string(name), object);
    constants_.emplace(std::string(name), object);
    return object;
}

Address Security::key(std::string_view name) const {
    std::lock_guard lock(mutex_);
    const auto found = constants_.find(name);
    if (found == constants_.end()) fail("Security constant not registered: " + std::string(name));
    return found->second;
}

std::optional<std::string> Security::key_name(Address value) const {
    if (!value) return std::nullopt;
    std::vector<std::pair<std::string, Address>> entries;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [name, object] : constants_) {
            if (object == value) return name;
            entries.emplace_back(name, object);
        }
    }
    if (!b->is_string(value)) return std::nullopt;
    for (const auto& [name, object] : entries) if (b->cf.equal(object, value)) return name;
    return std::nullopt;
}

Address Security::get(Address dictionary, std::string_view name) { return b->lookup(dictionary, key(name)); }

bool Security::get_bool(Address dictionary, std::string_view name, bool fallback) {
    const auto value = get(dictionary, name);
    if (!value) return fallback;
    const auto number = b->integer(value);
    return number ? *number != 0 : fallback;
}

bool Security::is(Address value, std::string_view name) {
    if (!value) return false;
    const auto constant = key(name);
    return value == constant || (b->is_string(value) && b->cf.equal(value, constant));
}

Address Security::make_certificate(const CertificatePtr& certificate) {
    if (!certificate) fail("null certificate");
    const auto object = b->objc.allocate(certificate_class);
    auto state = std::make_shared<CertificateState>();
    state->certificate = certificate;
    static_cast<void>(b->objc.host_state<CertificateState>(object, [&] { return state; }));
    return object;
}

CertificatePtr Security::certificate_of(Address object) const {
    if (!object) return nullptr;
    const auto state = b->objc.find_host_state<CertificateState>(object);
    return state ? state->certificate : nullptr;
}

Address Security::make_policy(const TrustPolicy& policy) {
    const auto object = b->objc.allocate(policy_class);
    auto state = std::make_shared<PolicyState>();
    state->policy = policy;
    static_cast<void>(b->objc.host_state<PolicyState>(object, [&] { return state; }));
    return object;
}

std::optional<TrustPolicy> Security::policy_of(Address object) const {
    if (!object) return std::nullopt;
    const auto state = b->objc.find_host_state<PolicyState>(object);
    if (!state) return std::nullopt;
    return state->policy;
}

std::shared_ptr<TrustState> Security::trust_of(Address object) const {
    return object ? b->objc.find_host_state<TrustState>(object) : nullptr;
}

Address Security::make_trust(const std::vector<CertificatePtr>& certificates, const std::vector<TrustPolicy>& policies) {
    const auto object = b->objc.allocate(trust_class);
    auto state = std::make_shared<TrustState>();
    try {
        for (const auto& certificate : certificates) {
            state->certificates.push_back(make_certificate(certificate));
            state->request.certificates.push_back(certificate);
        }
        for (const auto& policy : policies) {
            state->policies.push_back(make_policy(policy));
            state->request.policies.push_back(policy);
        }
    } catch (...) {
        b->release_all(state->certificates);
        b->release_all(state->policies);
        b->process.objects().release(object);
        throw;
    }
    static_cast<void>(b->objc.host_state<TrustState>(object, [&] { return state; }));
    return object;
}

TrustRequest Security::trust_request(Address trust) const {
    const auto state = trust_of(trust);
    if (!state) fail("not a SecTrust");
    std::lock_guard lock(state->mutex);
    return state->request;
}

TrustOutcome Security::evaluate(Address trust) {
    const auto state = trust_of(trust);
    if (!state) fail("not a SecTrust");
    TrustRequest request;
    {
        std::lock_guard lock(state->mutex);
        if (state->outcome) return *state->outcome;
        request = state->request;
    }
    auto outcome = evaluate_trust(request);
    std::vector<Address> chain, old;
    try {
        for (const auto& certificate : outcome.chain) chain.push_back(make_certificate(certificate));
    } catch (...) { b->release_all(chain); throw; }
    {
        std::lock_guard lock(state->mutex);
        old.swap(state->chain);
        state->chain = std::move(chain);
        state->outcome = outcome;
    }
    b->release_all(old);
    return outcome;
}

Address Security::make_key(std::shared_ptr<const KeyMaterial> material) {
    if (!material) fail("null key");
    const auto object = b->objc.allocate(key_class);
    auto state = std::make_shared<KeyState>();
    state->key = std::move(material);
    static_cast<void>(b->objc.host_state<KeyState>(object, [&] { return state; }));
    return object;
}

std::shared_ptr<const KeyMaterial> Security::key_of(Address object) const {
    if (!object) return nullptr;
    const auto state = b->objc.find_host_state<KeyState>(object);
    return state ? state->key : nullptr;
}

void Security::set_error(Address out, std::int32_t status_code, std::string_view description) {
    if (!out) return;
    b->store_error(out, b->os_status_error(status_code, description), false);
}

// Installs the Security exports (and NSError/CFError when nothing provides
// them yet). Creates only classes and constant strings; no host resources.
void install_security(InstallContext& context) {
    const auto bridge = create_bridge(context);
    install_errors(bridge);
    auto provider = std::make_shared<Security>(bridge);
    {
        std::lock_guard lock(instance_mutex);
        instance = provider;
    }
    install_certificates(*provider);
    install_keys(*provider);
    install_keychain(*provider);
    install_secure_transport(*provider);
    install_code_signing(*provider);
    install_common_crypto(*provider);
}

} // namespace alchemy::gui::security
