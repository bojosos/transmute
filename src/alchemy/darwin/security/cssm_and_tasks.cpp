#include "alchemy/gui/fill_provider.hpp"

#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <vector>

// Security: the deprecated CSSM interfaces answer errSecUnimplemented;
// SecTask reports an unsigned process with no entitlements; access-control
// objects are real values; the keychain search list is the default keychain.
// The Qt WebEngine images first imported them (moved from
// darwin/identities/system_gaps.cpp); installed by the fill-only kit
// (kit/fwgaps/install.cpp) at the old position.
namespace alchemy::gui::fill {
namespace {

constexpr std::int32_t sec_unimplemented = -4, sec_param = -50;

struct AccessControl final : ObjectPayload { Address protection{}; std::uint64_t flags{}; };
struct Task final : ObjectPayload {};

class CssmAndTasks final : public Provider {
public:
    using Provider::Provider;
    std::mutex mutex;
    bool user_interaction{true};
    std::map<Address, Address> certificate_data;   // SecCertificate -> retained CFData (SecCertificateGetData)

    void install();
};

void CssmAndTasks::install() {
    // CSSM OIDs (CSSM_OID = {size_t Length; uint8 *Data}).
    const auto oid = [this](const char* symbol, std::vector<std::uint8_t> bytes) {
        if (has(security, symbol)) return;
        const auto storage = process.allocate_guest(bytes.size(), 1);
        process.main_engine().write_bytes(storage, bytes);
        std::vector<std::uint8_t> record(16);
        const std::uint64_t length = bytes.size();
        std::memcpy(record.data(), &length, 8);
        std::memcpy(record.data() + 8, &storage, 8);
        static_cast<void>(data(security, symbol, record, 8));
    };
    const std::vector<std::uint8_t> pkcs1{0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01};
    const auto with = [](std::vector<std::uint8_t> base, std::initializer_list<std::uint8_t> tail) { base.insert(base.end(), tail); return base; };
    oid("_CSSMOID_MD2WithRSA", with(pkcs1, {0x02}));
    oid("_CSSMOID_MD4WithRSA", with(pkcs1, {0x03}));
    oid("_CSSMOID_MD5WithRSA", with(pkcs1, {0x04}));
    oid("_CSSMOID_SHA1WithRSA", with(pkcs1, {0x05}));
    oid("_CSSMOID_ECDSA_WithSHA1", {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x01});   // 1.2.840.10045.4.1
    oid("_CSSMOID_SHA1WithDSA", {0x2a, 0x86, 0x48, 0xce, 0x38, 0x04, 0x03});      // 1.2.840.10040.4.3
    oid("_CSSMOID_SHA1WithDSA_CMS", {0x2b, 0x0e, 0x03, 0x02, 0x1b});              // 1.3.14.3.2.27
    oid("_CSSMOID_SHA1WithDSA_JDK", {0x2b, 0x0e, 0x03, 0x02, 0x0d});              // 1.3.14.3.2.13
    oid("_CSSMOID_SHA1WithRSA_OIW", {0x2b, 0x0e, 0x03, 0x02, 0x1d});              // 1.3.14.3.2.29
    // Certificate-library field identifiers under the Intel CDSA X.509 arc
    // (2.16.840.1.113741.2.1.1.1). No CSSM certificate library exists, so they
    // are only compared, never looked up.
    const std::vector<std::uint8_t> cdsa{0x60, 0x86, 0x48, 0x01, 0x86, 0xf8, 0x4d, 0x02, 0x01, 0x01, 0x01};
    oid("_CSSMOID_X509V1SerialNumber", with(cdsa, {0x03}));
    oid("_CSSMOID_X509V1SignatureAlgorithm", with(cdsa, {0x04}));
    for (const auto* name : {"_CSSM_CL_CertAbortCache", "_CSSM_CL_CertAbortQuery", "_CSSM_CL_CertCache",
                             "_CSSM_CL_CertGetFirstCachedFieldValue", "_CSSM_CL_FreeFieldValue", "_SecCertificateGetCLHandle",
                             "_SecTrustGetCssmResultCode", "_SecTrustGetResult", "_SecTrustSetParameters",
                             "_SecTrustSetOCSPResponse", "_SecTrustSetSignedCertificateTimestamps", "_SecKeychainOpen"})
        failure(security, name, sec_unimplemented);
    // Keychain event callbacks: registered and removed, never called, because
    // the keychain (the mock keychain) never changes.
    // errSecDuplicateCallback -25297, errSecInvalidCallback -25298 (from memory, unverified).
    auto keychain_callbacks = std::make_shared<std::set<Address>>();
    raw(security, "_SecKeychainAddCallback", [this, keychain_callbacks](CpuState& s, GuestMemory&) {
        // OSStatus SecKeychainAddCallback(SecKeychainCallback, SecKeychainEventMask, void *userContext)
        if (!s.x[0]) { s.x[0] = status(sec_param); return; }
        std::lock_guard lock(mutex);
        s.x[0] = keychain_callbacks->insert(s.x[0]).second ? 0 : status(-25297);
    });
    raw(security, "_SecKeychainRemoveCallback", [this, keychain_callbacks](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        s.x[0] = keychain_callbacks->erase(s.x[0]) ? 0 : status(-25298);
    });
    failure(security, "_SecIdentitySearchCreate", sec_unimplemented, {2});
    failure(security, "_SecIdentitySearchCopyNext", sec_param, {1});
    constant(security, "_SecIdentityCopyPreferred", 0);
    // SecTrustSetKeychains: certificate evaluation never searches keychains, so
    // only an empty search list (no keychains) is honoured.
    raw(security, "_SecTrustSetKeychains", [this](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[1] && bridge->is_array(s.x[1]) && cf.length(s.x[1]) == 0 ? 0 : status(sec_unimplemented);
    });
    // OSStatus SecCertificateGetData(SecCertificateRef, CSSM_DATA *): the DER bytes,
    // valid while the certificate lives.
    raw(security, "_SecCertificateGetData", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) { s.x[0] = status(sec_param); return; }
        Address bytes{};
        {
            std::lock_guard lock(mutex);
            auto& slot = certificate_data[s.x[0]];
            if (!slot) slot = call(security, "_SecCertificateCopyData", {s.x[0]});
            bytes = slot;
        }
        if (!bytes) { s.x[0] = status(sec_param); return; }
        m.write64(s.x[1], call(core_foundation, "_CFDataGetLength", {bytes}));
        m.write64(s.x[1] + 8, call(core_foundation, "_CFDataGetBytePtr", {bytes}));
        s.x[0] = 0;
    });
    // Keychain search list and user-interaction state.
    raw(security, "_SecKeychainCopySearchList", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) { s.x[0] = status(sec_param); return; }
        const auto slot = process.allocate_guest(8);
        std::vector<Address> keychains;
        if (call(security, "_SecKeychainCopyDefault", {slot}) == 0) keychains.push_back(m.read64(slot));
        m.write64(s.x[0], cf.array(keychains));
        for (const auto keychain : keychains) release(keychain);
        s.x[0] = 0;
    });
    raw(security, "_SecKeychainGetUserInteractionAllowed", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) { s.x[0] = status(sec_param); return; }
        std::lock_guard lock(mutex);
        m.write8(s.x[0], user_interaction ? 1 : 0);
        s.x[0] = 0;
    });
    raw(security, "_SecKeychainSetUserInteractionAllowed", [this](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        user_interaction = (s.x[0] & 0xff) != 0;
        s.x[0] = 0;
    });
    // SecAccessControl: a value object (protection class + flags).
    auto access_class = objc.find_class("__AlchemySecAccessControl");
    if (!access_class) access_class = objc.register_native_class("__AlchemySecAccessControl", objc.find_class("NSObject"), 8);
    register_type(access_class, TypeId::sec_access_control);
    constant(security, "_SecAccessControlGetTypeID", static_cast<Address>(TypeId::sec_access_control));
    raw(security, "_SecAccessControlCreateWithFlags", [this, access_class](CpuState& s, GuestMemory& m) {
        // (allocator, CFTypeRef protection, SecAccessControlCreateFlags, CFErrorRef *)
        if (!s.x[1]) {
            if (s.x[3]) m.write64(s.x[3], bridge->os_status_error(sec_param, "SecAccessControlCreateWithFlags: protection is required"));
            s.x[0] = 0;
            return;
        }
        auto value = std::make_shared<AccessControl>();
        value->protection = retain(s.x[1]);
        value->flags = s.x[2];
        const auto object = objc.allocate(access_class);
        static_cast<void>(objc.host_state<AccessControl>(object, [&] { return value; }));
        s.x[0] = object;
    });
    // SecTask: the process is not code-signed and has no entitlements.
    auto task_class = objc.find_class("__AlchemySecTask");
    if (!task_class) task_class = objc.register_native_class("__AlchemySecTask", objc.find_class("NSObject"), 8);
    bridge->register_type_id(task_class, security::TypeId::task);
    raw(security, "_SecTaskCreateFromSelf", [this, task_class](CpuState& s, GuestMemory&) {
        const auto object = objc.allocate(task_class);
        static_cast<void>(objc.host_state<Task>(object));
        s.x[0] = object;
    });
    raw(security, "_SecTaskCopyValueForEntitlement", [](CpuState& s, GuestMemory& m) { if (s.x[2]) m.write64(s.x[2], 0); s.x[0] = 0; });
    raw(security, "_SecTaskCopySigningIdentifier", [](CpuState& s, GuestMemory& m) { if (s.x[1]) m.write64(s.x[1], 0); s.x[0] = 0; });
    constant(security, "_SecTaskGetCodeSignStatus", 0);
    string(security, "_kSecAttrSubject", "subj");
    string(security, "_kSecUseAuthenticationContext", "u_AuthCtx");
}

} // namespace

void install_security_cssm_and_tasks(InstallContext& context) { std::make_shared<CssmAndTasks>(context)->install(); }

} // namespace alchemy::gui::fill
