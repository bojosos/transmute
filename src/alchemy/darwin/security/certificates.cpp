#include "alchemy/darwin/security/security_internal.hpp"

#include "alchemy/darwin/security/keys.hpp"
#include "alchemy/host/crypto.hpp"

#include <array>
#include <bit>

// SecCertificate, SecPolicy, SecTrust, SecTrustSettings and SecRandom.
namespace alchemy::gui::security {
namespace {

constexpr std::string_view lib = security_library;

double d0(const CpuState& s) { return std::bit_cast<double>(s.v[0][0]); }

// A CFTypeRef that is either one object or a CFArray of them.
std::vector<Address> objects_of(Bridge& b, Address value) {
    if (!value) return {};
    if (b.is_array(value)) return b.cf.array_values(value);
    return {value};
}

struct Certificates {
    Security& sec;
    Bridge& b;

    CertificatePtr require_certificate(Address object) {
        auto certificate = sec.certificate_of(object);
        if (!certificate) fail("expected a SecCertificateRef");
        return certificate;
    }
    std::shared_ptr<TrustState> require_trust(Address object) {
        auto state = sec.trust_of(object);
        if (!state) fail("expected a SecTrustRef");
        return state;
    }
    // Replaces a list of +1 guest objects (and invalidates the evaluation).
    void replace(TrustState& state, std::vector<Address> TrustState::*list, std::vector<Address> values) {
        for (const auto value : values) b.retain(value);
        std::vector<Address> old, chain;
        {
            std::lock_guard lock(state.mutex);
            old.swap(state.*list);
            state.*list = std::move(values);
            state.outcome.reset();
            chain.swap(state.chain);
        }
        b.release_all(old);
        b.release_all(chain);
    }
    void invalidate(TrustState& state) {
        std::vector<Address> chain;
        {
            std::lock_guard lock(state.mutex);
            state.outcome.reset();
            chain.swap(state.chain);
        }
        b.release_all(chain);
    }
    std::vector<TrustPolicy> policies_of(const std::vector<Address>& objects) {
        std::vector<TrustPolicy> result;
        for (const auto object : objects) {
            const auto policy = sec.policy_of(object);
            if (!policy) fail("expected a SecPolicyRef");
            result.push_back(*policy);
        }
        return result;
    }
    std::vector<CertificatePtr> certificates_of(const std::vector<Address>& objects) {
        std::vector<CertificatePtr> result;
        for (const auto object : objects) result.push_back(require_certificate(object));
        return result;
    }
    Address error_for(const TrustOutcome& outcome) {
        return b.os_status_error(outcome.status, outcome.description);
    }
    Address policy_properties(const TrustPolicy& policy) {
        std::vector<std::pair<Address, Address>> entries;
        std::vector<Address> owned;
        const char* oid = policy.kind == TrustPolicy::Kind::ssl ? "kSecPolicyAppleSSL"
            : policy.kind == TrustPolicy::Kind::revocation ? "kSecPolicyAppleRevocation" : "kSecPolicyAppleX509Basic";
        entries.emplace_back(sec.key("kSecPolicyOid"), sec.key(oid));
        if (policy.kind == TrustPolicy::Kind::ssl) {
            if (!policy.hostname.empty()) {
                owned.push_back(b.string(policy.hostname));
                entries.emplace_back(sec.key("kSecPolicyName"), owned.back());
            }
            entries.emplace_back(sec.key("kSecPolicyClient"), b.cf.boolean(!policy.server));
        }
        const auto result = b.dictionary(entries);
        b.release_all(owned);
        return result;
    }
};

} // namespace

void install_certificates(Security& sec) {
    auto& b = *sec.b;
    auto self = std::make_shared<Certificates>(Certificates{sec, b});
    const auto make = [&](std::string name, TypeId id) {
        const auto cls = b.define_class(std::move(name), b.object_class, lib);
        b.register_type_id(cls, id);
        return cls;
    };
    sec.certificate_class = make("_AlchemySecCertificate", TypeId::certificate);
    sec.policy_class = make("_AlchemySecPolicy", TypeId::policy);
    sec.trust_class = make("_AlchemySecTrust", TypeId::trust);
    sec.key_class = make("_AlchemySecKey", TypeId::key);
    sec.identity_class = make("_AlchemySecIdentity", TypeId::identity);

    // Policy constants (Apple's object identifiers) and dictionary keys.
    sec.export_constant("kSecPolicyAppleSSL", "1.2.840.113635.100.1.3");
    sec.export_constant("kSecPolicyAppleX509Basic", "1.2.840.113635.100.1.2");
    sec.export_constant("kSecPolicyAppleRevocation", "1.2.840.113635.100.1.32");
    sec.export_constant("kSecPolicyOid", "SecPolicyOid");
    sec.export_constant("kSecPolicyName", "SecPolicyName");
    sec.export_constant("kSecPolicyClient", "SecPolicyClient");
    sec.export_constant("kSecPolicyRevocationFlags", "SecPolicyRevocationFlags");
    sec.export_constant("kSecPolicyTeamIdentifier", "SecPolicyTeamIdentifier");
    sec.export_constant("kSecTrustEvaluationDate", "TrustEvaluationDate");
    sec.export_constant("kSecTrustExtendedValidation", "TrustExtendedValidation");
    sec.export_constant("kSecTrustOrganizationName", "Organization");
    sec.export_constant("kSecTrustResultValue", "TrustResultValue");
    sec.export_constant("kSecTrustRevocationChecked", "TrustRevocationChecked");
    sec.export_constant("kSecTrustCertificateTransparency", "TrustCertificateTransparency");
    sec.export_constant("kSecPropertyTypeTitle", "title");
    sec.export_constant("kSecPropertyTypeError", "error");
    b.export_value(lib, "_kSecRandomDefault", 0);   // const SecRandomRef kSecRandomDefault = NULL

    // ---- object lifetime --------------------------------------------------------------------
    b.method(sec.trust_class, "dealloc", [self, cls = sec.trust_class](CpuState& s, GuestMemory&) {
        if (const auto state = self->sec.trust_of(s.x[0])) {
            std::vector<Address> objects;
            {
                std::lock_guard lock(state->mutex);
                for (auto* list : {&state->certificates, &state->policies, &state->anchors, &state->chain}) {
                    objects.insert(objects.end(), list->begin(), list->end());
                    list->clear();
                }
            }
            self->b.release_all(objects);
        }
        static_cast<void>(self->b.objc.send_super(s.x[0], cls, self->b.objc.selector("dealloc")));
    }, false, "v@:");
    b.method(sec.identity_class, "dealloc", [self, cls = sec.identity_class](CpuState& s, GuestMemory&) {
        if (const auto state = self->b.objc.find_host_state<IdentityState>(s.x[0]))
            self->b.release(std::exchange(state->certificate, 0));
        static_cast<void>(self->b.objc.send_super(s.x[0], cls, self->b.objc.selector("dealloc")));
    }, false, "v@:");
    b.method(sec.certificate_class, "isEqual:", [self](CpuState& s, GuestMemory&) {
        const auto left = self->sec.certificate_of(s.x[0]), right = self->sec.certificate_of(s.x[2]);
        s.x[0] = left && right && left->der() == right->der();
    }, false, "B@:@");
    b.method(sec.certificate_class, "hash", [self](CpuState& s, GuestMemory&) {
        const auto digest = self->require_certificate(s.x[0])->sha256();
        Address value{};
        for (std::size_t index = 0; index < 8; ++index) value = (value << 8) | digest[index];
        s.x[0] = value;
    }, false, "Q@:");
    b.method(sec.certificate_class, "description", [self](CpuState& s, GuestMemory&) {
        const auto certificate = self->require_certificate(s.x[0]);
        s.x[0] = self->b.autorelease(self->b.string("<cert(" + certificate->subject_summary() + ")>"));
    });

    // ---- SecRandom ------------------------------------------------------------------------------
    b.function(lib, "_SecRandomCopyBytes", [](CpuState& s, GuestMemory& m) {
        if (s.x[0]) { s.x[0] = static_cast<Address>(-1); return; }   // only kSecRandomDefault
        if (s.x[1] > (1ULL << 30)) { s.x[0] = static_cast<Address>(-1); return; }
        Bytes bytes(static_cast<std::size_t>(s.x[1]));
        if (!bytes.empty() && !host::crypto::random(std::as_writable_bytes(std::span(bytes)))) {
            s.x[0] = static_cast<Address>(-1);
            return;
        }
        if (!bytes.empty()) m.write_bytes(s.x[2], bytes);
        s.x[0] = 0;
    });

    // ---- SecCertificate ---------------------------------------------------------------------------
    b.function(lib, "_SecCertificateGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(TypeId::certificate); });
    b.function(lib, "_SecCertificateCreateWithData", [self](CpuState& s, GuestMemory&) {
        if (!self->b.is_data(s.x[1])) { s.x[0] = 0; return; }
        const auto certificate = Certificate::parse(self->b.bytes(s.x[1]));
        s.x[0] = certificate ? self->sec.make_certificate(certificate) : 0;
    });
    b.function(lib, "_SecCertificateCopyData", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->b.data(self->require_certificate(s.x[0])->der());
    });
    b.function(lib, "_SecCertificateCopySubjectSummary", [self](CpuState& s, GuestMemory&) {
        const auto text = self->require_certificate(s.x[0])->subject_summary();
        s.x[0] = text.empty() ? 0 : self->b.string(text);
    });
    b.function(lib, "_SecCertificateCopyCommonName", [self](CpuState& s, GuestMemory& m) {
        const auto certificate = self->sec.certificate_of(s.x[0]);
        if (!certificate || !s.x[1]) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::param)); return; }
        const auto name = certificate->common_name();
        m.write64(s.x[1], name.empty() ? 0 : self->b.string(name));
        s.x[0] = 0;
    });
    b.function(lib, "_SecCertificateCopyEmailAddresses", [self](CpuState& s, GuestMemory& m) {
        const auto certificate = self->sec.certificate_of(s.x[0]);
        if (!certificate || !s.x[1]) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::param)); return; }
        std::vector<Address> values;
        for (const auto& address : certificate->email_addresses()) values.push_back(self->b.string(address));
        m.write64(s.x[1], self->b.array(values));
        self->b.release_all(values);
        s.x[0] = 0;
    });
    for (const auto* name : {"_SecCertificateCopySerialNumberData", "_SecCertificateCopySerialNumber"})
        b.function(lib, name, [self](CpuState& s, GuestMemory&) {
            s.x[0] = self->b.data(self->require_certificate(s.x[0])->serial_number());
        });
    // Apple normalizes the names (case folding, whitespace); the DER Names are
    // returned as encoded, which compares equal for the same CA's encoding.
    b.function(lib, "_SecCertificateCopyNormalizedIssuerSequence", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->b.data(self->require_certificate(s.x[0])->issuer_sequence());
    });
    b.function(lib, "_SecCertificateCopyNormalizedSubjectSequence", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->b.data(self->require_certificate(s.x[0])->subject_sequence());
    });
    b.function(lib, "_SecCertificateCopyKey", [self](CpuState& s, GuestMemory&) {
        const auto key = key_from_spki(self->require_certificate(s.x[0])->subject_public_key_info());
        s.x[0] = key ? self->sec.make_key(key) : 0;
    });
    // macOS signature: OSStatus SecCertificateCopyPublicKey(SecCertificateRef, SecKeyRef *).
    b.function(lib, "_SecCertificateCopyPublicKey", [self](CpuState& s, GuestMemory& m) {
        const auto certificate = self->sec.certificate_of(s.x[0]);
        if (!certificate || !s.x[1]) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::param)); return; }
        const auto key = key_from_spki(certificate->subject_public_key_info());
        if (!key) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::unsupported_algorithm)); return; }
        m.write64(s.x[1], self->sec.make_key(key));
        s.x[0] = 0;
    });
    b.function(lib, "_SecCertificateCopyNotValidBeforeDate", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->b.cf.date(self->require_certificate(s.x[0])->not_before());
    });
    b.function(lib, "_SecCertificateCopyNotValidAfterDate", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->b.cf.date(self->require_certificate(s.x[0])->not_after());
    });

    // ---- SecPolicy -------------------------------------------------------------------------------------
    b.function(lib, "_SecPolicyGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(TypeId::policy); });
    b.function(lib, "_SecPolicyCreateSSL", [self](CpuState& s, GuestMemory&) {
        TrustPolicy policy{TrustPolicy::Kind::ssl, (s.x[0] & 0xff) != 0, {}, 0};
        if (s.x[1]) policy.hostname = self->b.text(s.x[1]);
        s.x[0] = self->sec.make_policy(policy);
    });
    b.function(lib, "_SecPolicyCreateBasicX509", [self](CpuState& s, GuestMemory&) { s.x[0] = self->sec.make_policy({}); });
    b.function(lib, "_SecPolicyCreateRevocation", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->sec.make_policy({TrustPolicy::Kind::revocation, true, {}, s.x[0]});
    });
    b.function(lib, "_SecPolicyCreateWithProperties", [self](CpuState& s, GuestMemory&) {
        auto& sec = self->sec;
        TrustPolicy policy;
        if (sec.is(s.x[0], "kSecPolicyAppleSSL")) {
            policy.kind = TrustPolicy::Kind::ssl;
            if (const auto name = sec.get(s.x[1], "kSecPolicyName")) policy.hostname = self->b.text(name);
            policy.server = !sec.get_bool(s.x[1], "kSecPolicyClient");
        } else if (sec.is(s.x[0], "kSecPolicyAppleRevocation")) {
            policy.kind = TrustPolicy::Kind::revocation;
            if (const auto flags = self->b.integer(sec.get(s.x[1], "kSecPolicyRevocationFlags"))) policy.revocation_flags = static_cast<std::uint64_t>(*flags);
        } else if (!sec.is(s.x[0], "kSecPolicyAppleX509Basic")) {
            s.x[0] = 0;   // policies other than SSL, basic X.509 and revocation are not available
            return;
        }
        s.x[0] = sec.make_policy(policy);
    });
    b.function(lib, "_SecPolicyCopyProperties", [self](CpuState& s, GuestMemory&) {
        const auto policy = self->sec.policy_of(s.x[0]);
        if (!policy) fail("expected a SecPolicyRef");
        s.x[0] = self->policy_properties(*policy);
    });

    // ---- SecTrust ----------------------------------------------------------------------------------------
    b.function(lib, "_SecTrustGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(TypeId::trust); });
    b.function(lib, "_SecTrustCreateWithCertificates", [self](CpuState& s, GuestMemory& m) {
        auto& sec = self->sec;
        const auto out = s.x[2];
        const auto certificates = objects_of(self->b, s.x[0]);
        const auto policies = objects_of(self->b, s.x[1]);
        if (!out || certificates.empty()) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::param)); return; }
        for (const auto object : certificates) if (!sec.certificate_of(object)) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::param)); return; }
        for (const auto object : policies) if (!sec.policy_of(object)) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::param)); return; }
        const auto trust = self->b.objc.allocate(sec.trust_class);
        auto state = std::make_shared<TrustState>();
        for (const auto object : certificates) { state->certificates.push_back(self->b.retain(object)); state->request.certificates.push_back(sec.certificate_of(object)); }
        for (const auto object : policies) { state->policies.push_back(self->b.retain(object)); state->request.policies.push_back(*sec.policy_of(object)); }
        static_cast<void>(self->b.objc.host_state<TrustState>(trust, [&] { return state; }));
        m.write64(out, trust);
        s.x[0] = 0;
    });
    b.function(lib, "_SecTrustSetPolicies", [self](CpuState& s, GuestMemory&) {
        const auto state = self->require_trust(s.x[0]);
        const auto policies = objects_of(self->b, s.x[1]);
        const auto values = self->policies_of(policies);
        self->replace(*state, &TrustState::policies, policies);
        { std::lock_guard lock(state->mutex); state->request.policies = values; }
        s.x[0] = 0;
    });
    b.function(lib, "_SecTrustCopyPolicies", [self](CpuState& s, GuestMemory& m) {
        const auto state = self->require_trust(s.x[0]);
        if (!s.x[1]) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::param)); return; }
        std::vector<Address> values;
        { std::lock_guard lock(state->mutex); values = state->policies; }
        m.write64(s.x[1], self->b.array(values));
        s.x[0] = 0;
    });
    b.function(lib, "_SecTrustSetAnchorCertificates", [self](CpuState& s, GuestMemory&) {
        const auto state = self->require_trust(s.x[0]);
        const auto anchors = s.x[1] ? self->b.cf.array_values(s.x[1]) : std::vector<Address>{};
        const auto values = self->certificates_of(anchors);
        self->replace(*state, &TrustState::anchors, anchors);
        {
            std::lock_guard lock(state->mutex);
            state->request.anchors = values;
            state->request.anchors_only = true;   // setting anchors disables the system anchors
        }
        s.x[0] = 0;
    });
    b.function(lib, "_SecTrustSetAnchorCertificatesOnly", [self](CpuState& s, GuestMemory&) {
        const auto state = self->require_trust(s.x[0]);
        { std::lock_guard lock(state->mutex); state->request.anchors_only = (s.x[1] & 0xff) != 0; }
        self->invalidate(*state);
        s.x[0] = 0;
    });
    b.function(lib, "_SecTrustCopyCustomAnchorCertificates", [self](CpuState& s, GuestMemory& m) {
        const auto state = self->require_trust(s.x[0]);
        if (!s.x[1]) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::param)); return; }
        std::vector<Address> values;
        { std::lock_guard lock(state->mutex); values = state->anchors; }
        m.write64(s.x[1], values.empty() ? 0 : self->b.array(values));
        s.x[0] = 0;
    });
    b.function(lib, "_SecTrustSetVerifyDate", [self](CpuState& s, GuestMemory&) {
        const auto state = self->require_trust(s.x[0]);
        if (!s.x[1]) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::param)); return; }
        const auto when = self->b.cf.date_time(s.x[1]);
        { std::lock_guard lock(state->mutex); state->request.verify_time = when; }
        self->invalidate(*state);
        s.x[0] = 0;
    });
    b.function(lib, "_SecTrustGetVerifyTime", [self](CpuState& s, GuestMemory&) {
        const auto state = self->require_trust(s.x[0]);
        double when;
        { std::lock_guard lock(state->mutex); when = state->request.verify_time.value_or(CoreFoundation::absolute_time_now()); }
        s.v[0] = {std::bit_cast<std::uint64_t>(when), 0};
    });
    b.function(lib, "_SecTrustSetNetworkFetchAllowed", [self](CpuState& s, GuestMemory&) {
        const auto state = self->require_trust(s.x[0]);
        { std::lock_guard lock(state->mutex); state->request.network_fetch = (s.x[1] & 0xff) != 0; }
        self->invalidate(*state);
        s.x[0] = 0;
    });
    b.function(lib, "_SecTrustGetNetworkFetchAllowed", [self](CpuState& s, GuestMemory& m) {
        const auto state = self->require_trust(s.x[0]);
        if (!s.x[1]) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::param)); return; }
        bool allowed;
        { std::lock_guard lock(state->mutex); allowed = state->request.network_fetch; }
        m.write8(s.x[1], allowed ? 1 : 0);
        s.x[0] = 0;
    });
    b.function(lib, "_SecTrustEvaluate", [self](CpuState& s, GuestMemory& m) {
        const auto outcome = self->sec.evaluate(s.x[0]);
        if (s.x[1]) m.write32(s.x[1], static_cast<std::uint32_t>(outcome.result));
        s.x[0] = outcome.result == TrustResult::invalid ? static_cast<Address>(static_cast<std::int64_t>(outcome.status)) : 0;
    });
    b.function(lib, "_SecTrustEvaluateWithError", [self](CpuState& s, GuestMemory& m) {
        const auto outcome = self->sec.evaluate(s.x[0]);
        if (s.x[1]) m.write64(s.x[1], outcome.trusted ? 0 : self->error_for(outcome));
        s.x[0] = outcome.trusted ? 1 : 0;
    });
    b.function(lib, "_SecTrustGetTrustResult", [self](CpuState& s, GuestMemory& m) {
        const auto state = self->require_trust(s.x[0]);
        if (!s.x[1]) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::param)); return; }
        TrustResult result = TrustResult::invalid;
        { std::lock_guard lock(state->mutex); if (state->outcome) result = state->outcome->result; }
        m.write32(s.x[1], static_cast<std::uint32_t>(result));
        s.x[0] = 0;
    });
    // Asynchronous evaluation: the result block runs on `queue`.
    b.function(lib, "_SecTrustEvaluateAsync", [self](CpuState& s, GuestMemory&) {
        self->require_trust(s.x[0]);
        if (!s.x[1] || !s.x[2]) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::param)); return; }
        const auto trust = self->b.retain(s.x[0]), block = self->b.block_copy(s.x[2]);
        self->b.dispatch_async(s.x[1], [self, trust, block] {
            const auto outcome = self->sec.evaluate(trust);
            self->b.invoke_block(block, {trust, static_cast<Address>(outcome.result)});
            self->b.block_release(block);
            self->b.release(trust);
        });
        s.x[0] = 0;
    });
    b.function(lib, "_SecTrustEvaluateAsyncWithError", [self](CpuState& s, GuestMemory&) {
        self->require_trust(s.x[0]);
        if (!s.x[1] || !s.x[2]) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::param)); return; }
        const auto trust = self->b.retain(s.x[0]), block = self->b.block_copy(s.x[2]);
        self->b.dispatch_async(s.x[1], [self, trust, block] {
            const auto outcome = self->sec.evaluate(trust);
            const auto error = outcome.trusted ? 0 : self->error_for(outcome);
            self->b.invoke_block(block, {trust, outcome.trusted ? 1U : 0U, error});
            self->b.release(error);
            self->b.block_release(block);
            self->b.release(trust);
        });
        s.x[0] = 0;
    });
    // The evaluated chain once evaluated, else the input certificates.
    const auto listed = [self](Address trust) {
        const auto state = self->require_trust(trust);
        std::lock_guard lock(state->mutex);
        return state->outcome ? state->chain : state->certificates;
    };
    b.function(lib, "_SecTrustGetCertificateCount", [listed](CpuState& s, GuestMemory&) { s.x[0] = listed(s.x[0]).size(); });
    b.function(lib, "_SecTrustGetCertificateAtIndex", [listed](CpuState& s, GuestMemory&) {
        const auto values = listed(s.x[0]);
        s.x[0] = s.x[1] < values.size() ? values[static_cast<std::size_t>(s.x[1])] : 0;
    });
    b.function(lib, "_SecTrustCopyCertificateChain", [self, listed](CpuState& s, GuestMemory&) {
        static_cast<void>(self->sec.evaluate(s.x[0]));
        s.x[0] = self->b.array(listed(s.x[0]));
    });
    for (const auto* name : {"_SecTrustCopyKey", "_SecTrustCopyPublicKey"})
        b.function(lib, name, [self](CpuState& s, GuestMemory&) {
            const auto state = self->require_trust(s.x[0]);
            CertificatePtr leaf;
            { std::lock_guard lock(state->mutex); if (!state->request.certificates.empty()) leaf = state->request.certificates[0]; }
            const auto key = leaf ? key_from_spki(leaf->subject_public_key_info()) : nullptr;
            s.x[0] = key ? self->sec.make_key(key) : 0;
        });
    b.function(lib, "_SecTrustCopyResult", [self](CpuState& s, GuestMemory&) {
        const auto state = self->require_trust(s.x[0]);
        std::optional<TrustOutcome> outcome;
        { std::lock_guard lock(state->mutex); outcome = state->outcome; }
        if (!outcome) { s.x[0] = 0; return; }
        const auto value = self->b.number(static_cast<std::int64_t>(outcome->result));
        const auto date = self->b.cf.date(CoreFoundation::absolute_time_now());
        const auto result = self->b.dictionary({{self->sec.key("kSecTrustResultValue"), value},
                                                {self->sec.key("kSecTrustEvaluationDate"), date}});
        self->b.release_all({value, date});
        s.x[0] = result;
    });
    b.function(lib, "_SecTrustCopyProperties", [self](CpuState& s, GuestMemory&) {
        const auto outcome = self->sec.evaluate(s.x[0]);
        if (outcome.trusted) { s.x[0] = 0; return; }
        const auto type = self->sec.key("kSecPropertyTypeError");
        const auto type_key = self->b.string("type"), value_key = self->b.string("value"), value = self->b.string(outcome.description);
        const auto entry = self->b.dictionary({{type_key, type}, {value_key, value}});
        s.x[0] = self->b.array({entry});
        self->b.release_all({type_key, value_key, value, entry});
    });
    b.function(lib, "_SecTrustCopyExceptions", [self](CpuState& s, GuestMemory&) {
        const auto state = self->require_trust(s.x[0]);
        CertificatePtr leaf;
        { std::lock_guard lock(state->mutex); if (!state->request.certificates.empty()) leaf = state->request.certificates[0]; }
        s.x[0] = leaf ? self->b.data(trust_exceptions(*leaf)) : 0;
    });
    b.function(lib, "_SecTrustSetExceptions", [self](CpuState& s, GuestMemory&) {
        const auto state = self->require_trust(s.x[0]);
        Bytes exceptions = s.x[1] && self->b.is_data(s.x[1]) ? self->b.bytes(s.x[1]) : Bytes{};
        bool matches = false;
        {
            std::lock_guard lock(state->mutex);
            matches = !exceptions.empty() && !state->request.certificates.empty() &&
                exceptions == trust_exceptions(*state->request.certificates[0]);
            state->request.exceptions = matches ? exceptions : Bytes{};
        }
        self->invalidate(*state);
        s.x[0] = matches ? 1 : 0;
    });

    // ---- anchors and trust settings (the Windows root stores) ---------------------------------------------
    b.function(lib, "_SecTrustCopyAnchorCertificates", [self](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::param)); return; }
        std::vector<Address> values;
        for (const auto& certificate : root_certificates(2)) values.push_back(self->sec.make_certificate(certificate));
        for (const auto& certificate : root_certificates(0)) values.push_back(self->sec.make_certificate(certificate));
        m.write64(s.x[0], self->b.array(values));
        self->b.release_all(values);
        s.x[0] = 0;
    });
    b.function(lib, "_SecTrustSettingsCopyCertificates", [self](CpuState& s, GuestMemory& m) {
        const auto domain = static_cast<int>(s.x[0]);
        if (domain < 0 || domain > 2) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::param)); return; }
        const auto certificates = root_certificates(domain);
        if (certificates.empty()) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::no_trust_settings)); return; }
        if (s.x[1]) {
            std::vector<Address> values;
            for (const auto& certificate : certificates) values.push_back(self->sec.make_certificate(certificate));
            m.write64(s.x[1], self->b.array(values));
            self->b.release_all(values);
        }
        s.x[0] = 0;
    });
    // Root-store membership is the only trust setting Windows records: system
    // roots have no settings (errSecItemNotFound, i.e. trusted as roots), user
    // and admin roots an empty settings array (always trust as root).
    b.function(lib, "_SecTrustSettingsCopyTrustSettings", [self](CpuState& s, GuestMemory& m) {
        const auto certificate = self->sec.certificate_of(s.x[0]);
        const auto domain = static_cast<int>(s.x[1]);
        if (!certificate || !s.x[2] || domain < 0 || domain > 2) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::param)); return; }
        const auto roots = root_certificates(domain);
        const bool member = std::any_of(roots.begin(), roots.end(), [&](const CertificatePtr& root) { return root->der() == certificate->der(); });
        if (!member || domain == 2) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::item_not_found)); return; }
        m.write64(s.x[2], self->b.array({}));
        s.x[0] = 0;
    });
}

} // namespace alchemy::gui::security
