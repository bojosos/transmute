// Security.framework provider tests (owner: v-netsec): random bytes,
// NSError/CFError, certificates and trust (custom anchors, host names,
// exceptions, system roots), SecKey (RSA / EC sign, verify, encrypt, decrypt,
// ECDH, external representations), SecItemImport, the DPAPI keychain, and a
// SecureTransport client against an in-process SChannel TLS server.
#include "security_internal.hpp"
#include "keys.hpp"
#include "tls.hpp"

#include "alchemy/darwin_dispatch.hpp"
#include "alchemy/alchemy_root.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/darwin_threads.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/gui/install.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {
using Address = std::uint64_t;
using Bytes = std::vector<std::uint8_t>;
namespace sec = alchemy::gui::security;
constexpr std::string_view security_library = sec::security_library;

void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext process{engine, [](unsigned, std::span<const std::uint8_t> bytes) {
        return alchemy::OutputResult{bytes.size(), 0}; }};
    alchemy::DarwinServices services{process};
    alchemy::DarwinThreads threads{(alchemy::install_darwin_dispatch(process), process)};
    alchemy::ObjcRuntime objc{process};
    alchemy::CoreFoundation cf{process};
    alchemy::FoundationBridge foundation{process, objc, cf};

    Fixture() {
        alchemy::install_darwin_frameworks(process, objc, cf);
        alchemy::gui::InstallContext context{process, objc, cf};
        alchemy::gui::install_security(context);
    }
    ~Fixture() {
        try { process.request_exit(0); threads.join_workers(); } catch (...) {}
    }
    Address symbol(std::string_view library, std::string_view name) {
        const auto found = process.resolve(library, name);
        require(found.has_value(), "missing symbol " + std::string(name));
        return found->address;
    }
    Address call(std::string_view library, std::string_view name, std::initializer_list<Address> arguments = {}) {
        return engine.invoke_callback(symbol(library, name), std::span(arguments.begin(), arguments.size()));
    }
    Address s(std::string_view name, std::initializer_list<Address> arguments = {}) { return call(security_library, name, arguments); }
    Address core(std::string_view name, std::initializer_list<Address> arguments = {}) {
        return call(alchemy::core_foundation_library, name, arguments);
    }
    std::int32_t status(std::string_view name, std::initializer_list<Address> arguments = {}) {
        return static_cast<std::int32_t>(s(name, arguments));
    }
    Address constant(std::string_view name) { return engine.read64(symbol(security_library, "_" + std::string(name))); }
    Address cell() { const auto value = process.allocate_guest(8); engine.write64(value, 0); return value; }
    Address data(std::span<const std::uint8_t> bytes) { return sec::bridge()->data(bytes); }
    Address data(std::string_view text) { return data(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())); }
    Bytes bytes(Address object) { return sec::bridge()->bytes(object); }
    Address string(std::string_view text) { return cf.string(text); }
    Address dictionary(std::initializer_list<std::pair<Address, Address>> entries) {
        return cf.dictionary(std::vector<std::pair<Address, Address>>(entries));
    }
    Address array(std::initializer_list<Address> values) { return cf.array(std::vector<Address>(values)); }
    void release(Address object) { if (object) static_cast<void>(core("_CFRelease", {object})); }
    std::int64_t error_code(Address error) { return static_cast<std::int64_t>(core("_CFErrorGetCode", {error})); }
    Address text_buffer(std::string_view text) {
        const auto address = process.allocate_guest(text.size() + 1);
        engine.write_bytes(address, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
        engine.fill_bytes(address + text.size(), 0, 1);
        return address;
    }
};

void test_random_and_errors(Fixture& f) {
    const auto buffer = f.process.allocate_guest(32);
    require(f.status("_SecRandomCopyBytes", {0, 32, buffer}) == 0, "SecRandomCopyBytes failed");
    Bytes bytes(32);
    f.engine.read_bytes(buffer, bytes);
    require(std::any_of(bytes.begin(), bytes.end(), [](auto value) { return value != 0; }), "SecRandomCopyBytes returned zeros");

    const auto domain = f.string("AlchemyTestDomain");
    const auto error = f.core("_CFErrorCreate", {0, domain, 42, 0});
    require(f.error_code(error) == 42, "CFErrorGetCode");
    const auto description = f.core("_CFErrorCopyDescription", {error});
    require(f.cf.utf8(description).find("error 42") != std::string::npos, "CFErrorCopyDescription: " + f.cf.utf8(description));
    require(f.core("_CFGetTypeID", {error}) == f.core("_CFErrorGetTypeID"), "CFGetTypeID(CFError)");
    require(f.core("_CFGetTypeID", {domain}) == f.core("_CFStringGetTypeID"), "CFGetTypeID forwarding for CFString");
    // NSError is the same object.
    const auto code = f.objc.send(error, "code").x0;
    require(code == 42, "-[NSError code]");
    f.release(description);
    f.release(error);
}

struct Identity {
    std::unique_ptr<sec::SelfSignedIdentity> identity;
    Address certificate{};
};

Identity make_identity(Fixture& f) {
    Identity result;
    result.identity = sec::SelfSignedIdentity::create("alchemy.test", {"alchemy.test", "localhost"});
    const auto der = result.identity->certificate()->der();
    const auto data = f.data(der);
    result.certificate = f.s("_SecCertificateCreateWithData", {0, data});
    f.release(data);
    require(result.certificate != 0, "SecCertificateCreateWithData failed");
    return result;
}

std::int64_t evaluate(Fixture& f, Address trust) {
    const auto error = f.cell();
    const auto trusted = f.s("_SecTrustEvaluateWithError", {trust, error}) & 0xff;
    const auto object = f.engine.read64(error);
    const auto code = object ? f.error_code(object) : 0;
    f.release(object);
    require(trusted == (object == 0 ? 1U : 0U), "SecTrustEvaluateWithError result and error disagree");
    return code;
}

void test_trust(Fixture& f, Identity& identity) {
    const auto certificate = identity.certificate;
    require(f.core("_CFGetTypeID", {certificate}) == f.s("_SecCertificateGetTypeID"), "CFGetTypeID(SecCertificate)");
    require(f.s("_SecCertificateGetTypeID") != f.s("_SecTrustGetTypeID"), "certificate and trust type ids collide");
    const auto summary = f.s("_SecCertificateCopySubjectSummary", {certificate});
    require(f.cf.utf8(summary) == "alchemy.test", "subject summary: " + f.cf.utf8(summary));
    f.release(summary);
    const auto copied = f.s("_SecCertificateCopyData", {certificate});
    require(f.bytes(copied) == identity.identity->certificate()->der(), "SecCertificateCopyData");
    f.release(copied);

    // A self-signed server certificate: untrusted by default, trusted as a custom anchor.
    const auto host = f.string("localhost");
    const auto policy = f.s("_SecPolicyCreateSSL", {1, host});
    const auto out = f.cell();
    require(f.status("_SecTrustCreateWithCertificates", {certificate, policy, out}) == 0, "SecTrustCreateWithCertificates");
    const auto trust = f.engine.read64(out);
    require(evaluate(f, trust) == sec::status::not_trusted, "self-signed certificate was not rejected with errSecNotTrusted");
    const auto result = f.cell();
    require(f.status("_SecTrustGetTrustResult", {trust, result}) == 0 && f.engine.read32(result) == 5, "kSecTrustResultRecoverableTrustFailure");
    const auto anchors = f.array({certificate});
    require(f.status("_SecTrustSetAnchorCertificates", {trust, anchors}) == 0, "SecTrustSetAnchorCertificates");
    require(evaluate(f, trust) == 0, "custom anchor not trusted");
    require(f.s("_SecTrustGetCertificateCount", {trust}) == 1, "evaluated chain length");
    const auto chain = f.s("_SecTrustCopyCertificateChain", {trust});
    require(f.core("_CFArrayGetCount", {chain}) == 1, "SecTrustCopyCertificateChain");
    f.release(chain);
    // The host name is checked.
    const auto other = f.string("wrong.example");
    const auto wrong = f.s("_SecPolicyCreateSSL", {1, other});
    require(f.status("_SecTrustSetPolicies", {trust, wrong}) == 0, "SecTrustSetPolicies");
    require(evaluate(f, trust) == sec::status::host_name_mismatch, "host name mismatch not reported");
    // Exceptions accept the recoverable failure for this leaf only.
    const auto out2 = f.cell();
    require(f.status("_SecTrustCreateWithCertificates", {certificate, policy, out2}) == 0, "second trust");
    const auto second = f.engine.read64(out2);
    const auto exceptions = f.s("_SecTrustCopyExceptions", {second});
    require((f.s("_SecTrustSetExceptions", {second, exceptions}) & 0xff) == 1, "SecTrustSetExceptions");
    require(evaluate(f, second) == 0, "exceptions did not apply");
    // Public key of the leaf.
    const auto key = f.s("_SecTrustCopyKey", {second});
    require(key != 0 && f.core("_CFGetTypeID", {key}) == f.s("_SecKeyGetTypeID"), "SecTrustCopyKey");
    for (const auto object : {key, exceptions, second, wrong, other, anchors, trust, policy, host}) f.release(object);

    // A Windows machine root validates under the basic X.509 policy.
    const auto roots = sec::root_certificates(2);
    require(!roots.empty(), "no machine root certificates");
    bool found = false;
    const auto now = alchemy::CoreFoundation::absolute_time_now();
    for (const auto& root : roots) {
        if (!root->self_issued() || root->not_after() < now || root->not_before() > now) continue;
        const auto data = f.data(root->der());
        const auto object = f.s("_SecCertificateCreateWithData", {0, data});
        const auto basic = f.s("_SecPolicyCreateBasicX509");
        const auto cell = f.cell();
        require(f.status("_SecTrustCreateWithCertificates", {object, basic, cell}) == 0, "root trust");
        const auto root_trust = f.engine.read64(cell);
        found = evaluate(f, root_trust) == 0;
        for (const auto value : {root_trust, basic, object, data}) f.release(value);
        if (found) break;
    }
    require(found, "no machine root evaluated as trusted");
    const auto anchors_out = f.cell();
    require(f.status("_SecTrustCopyAnchorCertificates", {anchors_out}) == 0 && f.core("_CFArrayGetCount", {f.engine.read64(anchors_out)}) > 0,
            "SecTrustCopyAnchorCertificates");
    f.release(f.engine.read64(anchors_out));
    require(f.status("_SecTrustSettingsCopyCertificates", {2, anchors_out}) == 0, "SecTrustSettingsCopyCertificates(system)");
    f.release(f.engine.read64(anchors_out));
}

Address key_parameters(Fixture& f, bool rsa, std::int64_t bits) {
    const auto size = f.cf.integer(bits, 4);
    const auto result = f.dictionary({{f.constant("kSecAttrKeyType"), f.constant(rsa ? "kSecAttrKeyTypeRSA" : "kSecAttrKeyTypeECSECPrimeRandom")},
                                      {f.constant("kSecAttrKeySizeInBits"), size}});
    f.release(size);
    return result;
}

void test_keys(Fixture& f, Identity& identity) {
    const auto error = f.cell();
    // RSA.
    const auto rsa_parameters = key_parameters(f, true, 2048);
    const auto rsa = f.s("_SecKeyCreateRandomKey", {rsa_parameters, error});
    require(rsa != 0, "SecKeyCreateRandomKey(RSA)");
    const auto rsa_public = f.s("_SecKeyCopyPublicKey", {rsa});
    require(f.s("_SecKeyGetBlockSize", {rsa}) == 256, "RSA block size");
    const auto message = f.data("Alchemy signs this");
    const auto sign = f.constant("kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256");
    const auto signature = f.s("_SecKeyCreateSignature", {rsa, sign, message, error});
    require(signature != 0 && f.core("_CFDataGetLength", {signature}) == 256, "RSA signature");
    require((f.s("_SecKeyVerifySignature", {rsa_public, sign, message, signature, error}) & 0xff) == 1, "RSA verify");
    const auto tampered = f.data("Alchemy signs that");
    require((f.s("_SecKeyVerifySignature", {rsa_public, sign, tampered, signature, error}) & 0xff) == 0, "RSA verify accepted a tampered message");
    f.release(f.engine.read64(error));
    f.engine.write64(error, 0);
    const auto oaep = f.constant("kSecKeyAlgorithmRSAEncryptionOAEPSHA256");
    const auto encrypted = f.s("_SecKeyCreateEncryptedData", {rsa_public, oaep, message, error});
    const auto decrypted = f.s("_SecKeyCreateDecryptedData", {rsa, oaep, encrypted, error});
    require(decrypted && f.bytes(decrypted) == f.bytes(message), "RSA OAEP round trip");
    // Raw RSA (QLab decrypts with kSecKeyAlgorithmRSAEncryptionRaw).
    const auto raw = f.constant("kSecKeyAlgorithmRSAEncryptionRaw");
    Bytes block(256, 0);
    block[255] = 7; block[200] = 0x42;
    const auto block_data = f.data(block);
    const auto raw_encrypted = f.s("_SecKeyCreateEncryptedData", {rsa_public, raw, block_data, error});
    const auto raw_decrypted = f.s("_SecKeyCreateDecryptedData", {rsa, raw, raw_encrypted, error});
    require(raw_decrypted && f.bytes(raw_decrypted) == block, "RSA raw round trip");
    // External representation round trip.
    const auto external = f.s("_SecKeyCopyExternalRepresentation", {rsa, error});
    const auto private_attributes = f.dictionary({{f.constant("kSecAttrKeyType"), f.constant("kSecAttrKeyTypeRSA")},
                                                  {f.constant("kSecAttrKeyClass"), f.constant("kSecAttrKeyClassPrivate")}});
    const auto imported = f.s("_SecKeyCreateWithData", {external, private_attributes, error});
    require(imported != 0, "SecKeyCreateWithData(RSA private)");
    const auto signature2 = f.s("_SecKeyCreateSignature", {imported, sign, message, error});
    require((f.s("_SecKeyVerifySignature", {rsa_public, sign, message, signature2, error}) & 0xff) == 1, "imported RSA key signature");
    const auto attributes = f.s("_SecKeyCopyAttributes", {rsa});
    require(f.core("_CFDictionaryGetValue", {attributes, f.constant("kSecAttrKeyClass")}) == f.constant("kSecAttrKeyClassPrivate"),
            "SecKeyCopyAttributes class");

    // EC P-256: ECDSA and ECDH.
    const auto ec_parameters = key_parameters(f, false, 256);
    const auto ec = f.s("_SecKeyCreateRandomKey", {ec_parameters, error});
    const auto ec2 = f.s("_SecKeyCreateRandomKey", {ec_parameters, error});
    require(ec && ec2, "SecKeyCreateRandomKey(EC)");
    const auto ec_public = f.s("_SecKeyCopyPublicKey", {ec}), ec2_public = f.s("_SecKeyCopyPublicKey", {ec2});
    const auto ecdsa = f.constant("kSecKeyAlgorithmECDSASignatureMessageX962SHA256");
    const auto ec_signature = f.s("_SecKeyCreateSignature", {ec, ecdsa, message, error});
    require(ec_signature && (f.s("_SecKeyVerifySignature", {ec_public, ecdsa, message, ec_signature, error}) & 0xff) == 1, "ECDSA");
    const auto ecdh = f.constant("kSecKeyAlgorithmECDHKeyExchangeStandard");
    const auto shared1 = f.s("_SecKeyCopyKeyExchangeResult", {ec, ecdh, ec2_public, 0, error});
    const auto shared2 = f.s("_SecKeyCopyKeyExchangeResult", {ec2, ecdh, ec_public, 0, error});
    require(shared1 && shared2 && f.bytes(shared1) == f.bytes(shared2) && f.bytes(shared1).size() == 32, "ECDH secrets differ");
    const auto ec_external = f.s("_SecKeyCopyExternalRepresentation", {ec_public, error});
    require(f.bytes(ec_external).size() == 65 && f.bytes(ec_external)[0] == 4, "EC public external representation");

    // SecItemImport: a PEM certificate and a PEM public key.
    const auto rsa_material = sec::security()->key_of(rsa_public);
    const auto pem = "-----BEGIN CERTIFICATE-----\n" + sec::base64_encode(identity.identity->certificate()->der()) +
        "\n-----END CERTIFICATE-----\n-----BEGIN PUBLIC KEY-----\n" + sec::base64_encode(rsa_material->spki()) + "\n-----END PUBLIC KEY-----\n";
    const auto pem_data = f.data(pem);
    const auto items = f.cell();
    const auto format = f.process.allocate_guest(4), type = f.process.allocate_guest(4);
    f.engine.write32(format, 0); f.engine.write32(type, 0);
    require(f.status("_SecItemImport", {pem_data, 0, format, type, 0, 0, 0, items}) == 0, "SecItemImport(PEM)");
    const auto list = f.engine.read64(items);
    require(f.core("_CFArrayGetCount", {list}) == 2, "SecItemImport item count");
    require(f.core("_CFGetTypeID", {f.core("_CFArrayGetValueAtIndex", {list, 0})}) == f.s("_SecCertificateGetTypeID"), "imported certificate");
    require(f.core("_CFGetTypeID", {f.core("_CFArrayGetValueAtIndex", {list, 1})}) == f.s("_SecKeyGetTypeID"), "imported key");
    require(f.engine.read32(format) == 10 && f.engine.read32(type) == 5, "SecItemImport format / item type");
    for (const auto object : {list, pem_data, ec_external, shared2, shared1, ec_signature, ec2_public, ec_public, ec2, ec, ec_parameters,
                              attributes, signature2, imported, private_attributes, external, raw_decrypted, raw_encrypted, block_data,
                              decrypted, encrypted, tampered, signature, message, rsa_public, rsa, rsa_parameters})
        f.release(object);
}

void test_keychain(Fixture& f) {
    const auto path = std::filesystem::temp_directory_path() / ("alchemy-keychain-test-" + std::to_string(GetCurrentProcessId()) + ".db");
    std::filesystem::remove(path);
    SetEnvironmentVariableW(L"ALCHEMY_KEYCHAIN", path.c_str());
    const auto service = f.string("com.alchemy.test"), account = f.string("artist@example.com");
    const std::string secret_text = "s3cret-Alchemy-token-01234567";
    const auto secret = f.data(secret_text);
    const auto add = f.dictionary({{f.constant("kSecClass"), f.constant("kSecClassGenericPassword")},
                                   {f.constant("kSecAttrService"), service}, {f.constant("kSecAttrAccount"), account},
                                   {f.constant("kSecValueData"), secret}});
    require(f.status("_SecItemAdd", {add, 0}) == 0, "SecItemAdd");
    require(f.status("_SecItemAdd", {add, 0}) == sec::status::duplicate_item, "duplicate SecItemAdd");
    // The file never holds the secret in plain text.
    {
        std::ifstream file(path, std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        require(!content.empty() && content.find(secret_text) == std::string::npos && content.find("artist@example.com") == std::string::npos,
                "keychain file is not encrypted");
    }
    const auto query = f.dictionary({{f.constant("kSecClass"), f.constant("kSecClassGenericPassword")},
                                     {f.constant("kSecAttrService"), service}, {f.constant("kSecReturnData"), f.cf.boolean(true)}});
    const auto result = f.cell();
    require(f.status("_SecItemCopyMatching", {query, result}) == 0, "SecItemCopyMatching");
    require(f.bytes(f.engine.read64(result)) == Bytes(secret_text.begin(), secret_text.end()), "keychain data differs");
    f.release(f.engine.read64(result));
    const auto attributes_query = f.dictionary({{f.constant("kSecClass"), f.constant("kSecClassGenericPassword")},
                                                {f.constant("kSecReturnAttributes"), f.cf.boolean(true)},
                                                {f.constant("kSecMatchLimit"), f.constant("kSecMatchLimitAll")}});
    require(f.status("_SecItemCopyMatching", {attributes_query, result}) == 0, "SecItemCopyMatching(attributes)");
    const auto list = f.engine.read64(result);
    require(f.core("_CFArrayGetCount", {list}) == 1, "attribute query count");
    const auto first = f.core("_CFArrayGetValueAtIndex", {list, 0});
    require(f.cf.utf8(f.core("_CFDictionaryGetValue", {first, f.constant("kSecAttrAccount")})) == "artist@example.com", "account attribute");
    f.release(list);
    // Update, then the legacy API sees the new password.
    const auto changed = f.data(std::string_view("rotated"));
    const auto update = f.dictionary({{f.constant("kSecValueData"), changed}});
    const auto match = f.dictionary({{f.constant("kSecClass"), f.constant("kSecClassGenericPassword")}, {f.constant("kSecAttrService"), service}});
    require(f.status("_SecItemUpdate", {match, update}) == 0, "SecItemUpdate");
    const auto service_text = f.text_buffer("com.alchemy.test"), account_text = f.text_buffer("artist@example.com");
    const auto length = f.process.allocate_guest(4), data = f.cell(), item = f.cell();
    require(f.status("_SecKeychainFindGenericPassword", {0, 16, service_text, 18, account_text, length, data, item}) == 0,
            "SecKeychainFindGenericPassword");
    Bytes found(f.engine.read32(length));
    f.engine.read_bytes(f.engine.read64(data), found);
    require(std::string(found.begin(), found.end()) == "rotated", "legacy keychain read");
    require(f.status("_SecKeychainItemFreeContent", {0, f.engine.read64(data)}) == 0, "SecKeychainItemFreeContent");
    require(f.status("_SecKeychainItemDelete", {f.engine.read64(item)}) == 0, "SecKeychainItemDelete");
    f.release(f.engine.read64(item));
    require(f.status("_SecItemCopyMatching", {query, result}) == sec::status::item_not_found, "item survived deletion");
    // Legacy add, then SecItemDelete.
    const auto password = f.text_buffer("pw");
    require(f.status("_SecKeychainAddGenericPassword", {0, 16, service_text, 18, account_text, 2, password, 0}) == 0, "SecKeychainAddGenericPassword");
    require(f.status("_SecItemDelete", {match}) == 0, "SecItemDelete");
    require(f.status("_SecItemDelete", {match}) == sec::status::item_not_found, "second SecItemDelete");
    // Keys are not stored in the keychain.
    const auto key_query = f.dictionary({{f.constant("kSecClass"), f.constant("kSecClassKey")}});
    require(f.status("_SecItemCopyMatching", {key_query, result}) == sec::status::item_not_found, "key query");
    SetEnvironmentVariableW(L"ALCHEMY_KEYCHAIN", nullptr);
    std::filesystem::remove(path);

    // With an Alchemy root configured, the keychain lives under <root>/.alchemy/keychain.
    const auto root = std::filesystem::temp_directory_path() / ("alchemy-keychain-root-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(root);
    alchemy::create_alchemy_root(root);
    alchemy::configure_alchemy_root(root);
    struct Clear { std::filesystem::path root; ~Clear() { alchemy::clear_alchemy_root(); std::error_code e; std::filesystem::remove_all(root, e); } } clear{root};
    require(f.status("_SecItemAdd", {add, 0}) == 0, "SecItemAdd in the Alchemy root");
    wchar_t user[257]{};
    DWORD user_size = 257;
    require(GetUserNameW(user, &user_size) != 0, "host user name");
    const auto stored = root / ".alchemy" / "keychain" / (std::wstring(user, user_size - 1) + L".alchemy-keychain");
    require(std::filesystem::exists(stored), "the keychain is not in the Alchemy root");
    require(f.status("_SecItemDelete", {match}) == 0, "SecItemDelete in the Alchemy root");
    for (const auto object : {key_query, match, update, changed, attributes_query, query, add, secret, account, service}) f.release(object);
}

// ---- SecureTransport against an SChannel server -------------------------------------------------------------

struct Server {
    SOCKET listener{INVALID_SOCKET};
    std::uint16_t port{};
    std::thread thread;
    std::atomic<bool> ok{};
    std::string error;

    explicit Server(const alchemy::host::crypto::Identity* certificate, int connections) {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener, 4) == 0, "listen failed");
        int size = sizeof(address);
        getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size);
        port = ntohs(address.sin_port);
        thread = std::thread([this, certificate, connections] {
            try {
                for (int index = 0; index < connections; ++index) serve(certificate, index == connections - 1);
                ok = true;
            } catch (const std::exception& e) { error = e.what(); }
        });
    }
    void serve(const alchemy::host::crypto::Identity* certificate, bool expect_data) {
        const auto client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) throw std::runtime_error("accept failed");
        struct Close { SOCKET s; ~Close() { closesocket(s); } } close{client};
        sec::TlsOptions options;
        options.server = true;
        options.certificate = certificate;
        sec::TlsSession tls(options);
        const sec::TlsRead read = [client](std::uint8_t* buffer, std::size_t size) {
            const auto got = recv(client, reinterpret_cast<char*>(buffer), static_cast<int>(size), 0);
            if (got < 0) return sec::IoResult{sec::IoResult::Kind::error, 0};
            return sec::IoResult{got == 0 ? sec::IoResult::Kind::closed : sec::IoResult::Kind::ok, static_cast<std::size_t>(got)};
        };
        const sec::TlsWrite write = [client](const std::uint8_t* data, std::size_t size) {
            const auto sent = send(client, reinterpret_cast<const char*>(data), static_cast<int>(size), 0);
            if (sent < 0) return sec::IoResult{sec::IoResult::Kind::error, 0};
            return sec::IoResult{sec::IoResult::Kind::ok, static_cast<std::size_t>(sent)};
        };
        const auto status = tls.handshake(read, write);
        if (!expect_data) return;   // the client rejects the certificate
        if (status != sec::TlsSession::Status::ok) throw std::runtime_error("server handshake: " + tls.error());
        std::uint8_t buffer[16];
        std::size_t got{};
        if (tls.read(read, write, buffer, 4, got) != sec::TlsSession::Status::ok || std::string(reinterpret_cast<char*>(buffer), got) != "ping")
            throw std::runtime_error("server did not receive ping");
        std::size_t accepted{};
        if (tls.write(write, reinterpret_cast<const std::uint8_t*>("pong"), 4, accepted) != sec::TlsSession::Status::ok)
            throw std::runtime_error("server write failed");
        static_cast<void>(tls.shutdown(write));
    }
    ~Server() {
        if (thread.joinable()) thread.join();
        closesocket(listener);
    }
};

SOCKET connect_to(std::uint16_t port) {
    const auto socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    require(connect(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect failed");
    return socket_handle;
}

void test_secure_transport(Fixture& f, Identity& identity) {
    // SSLReadFunc / SSLWriteFunc over a blocking socket (the connection ref is the SOCKET).
    const auto read_function = f.process.install_function("test_ssl_read", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        const auto wanted = m.read64(s.x[2]);
        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(wanted));
        std::size_t total = 0;
        while (total < buffer.size()) {
            const auto got = recv(static_cast<SOCKET>(s.x[0]), reinterpret_cast<char*>(buffer.data() + total), static_cast<int>(buffer.size() - total), 0);
            if (got <= 0) break;
            total += static_cast<std::size_t>(got);
            break;   // return what one recv delivered
        }
        if (total) m.write_bytes(s.x[1], std::span(buffer.data(), total));
        m.write64(s.x[2], total);
        s.x[0] = total ? 0 : static_cast<std::uint64_t>(static_cast<std::int64_t>(-9805));
    }, "alchemy.test");
    const auto write_function = f.process.install_function("test_ssl_write", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(m.read64(s.x[2])));
        m.read_bytes(s.x[1], buffer);
        const auto sent = send(static_cast<SOCKET>(s.x[0]), reinterpret_cast<const char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
        m.write64(s.x[2], sent > 0 ? static_cast<std::uint64_t>(sent) : 0);
        s.x[0] = sent > 0 ? 0 : static_cast<std::uint64_t>(static_cast<std::int64_t>(-9806));
    }, "alchemy.test");
    const auto name = f.text_buffer("localhost");
    Server server(identity.identity->context(), 2);

    // 1. Default validation rejects the self-signed server certificate.
    {
        const auto socket_handle = connect_to(server.port);
        const auto context = f.s("_SSLCreateContext", {0, 1, 0});
        require(context != 0, "SSLCreateContext");
        require(f.status("_SSLSetIOFuncs", {context, read_function, write_function}) == 0 &&
                f.status("_SSLSetConnection", {context, static_cast<Address>(socket_handle)}) == 0 &&
                f.status("_SSLSetPeerDomainName", {context, name, 9}) == 0, "SSL configuration");
        const auto status = f.status("_SSLHandshake", {context});
        require(status == -9807, "untrusted server certificate gave " + std::to_string(status));
        f.release(context);
        closesocket(socket_handle);
    }
    // 2. Break on server auth, trust the anchor, exchange data.
    const auto socket_handle = connect_to(server.port);
    const auto context = f.s("_SSLCreateContext", {0, 1, 0});
    require(f.status("_SSLSetIOFuncs", {context, read_function, write_function}) == 0 &&
            f.status("_SSLSetConnection", {context, static_cast<Address>(socket_handle)}) == 0 &&
            f.status("_SSLSetPeerDomainName", {context, name, 9}) == 0 &&
            f.status("_SSLSetSessionOption", {context, 0, 1}) == 0, "SSL configuration");
    require(f.status("_SSLHandshake", {context}) == -9841, "errSSLServerAuthCompleted expected");
    const auto trust_out = f.cell();
    require(f.status("_SSLCopyPeerTrust", {context, trust_out}) == 0 && f.engine.read64(trust_out), "SSLCopyPeerTrust");
    const auto trust = f.engine.read64(trust_out);
    require(evaluate(f, trust) == sec::status::not_trusted, "peer trust without anchors");
    const auto anchors = f.array({identity.certificate});
    static_cast<void>(f.s("_SecTrustSetAnchorCertificates", {trust, anchors}));
    require(evaluate(f, trust) == 0, "peer trust with the anchor");
    require(f.status("_SSLHandshake", {context}) == 0, "SSLHandshake after server auth");
    const auto state = f.process.allocate_guest(4);
    require(f.status("_SSLGetSessionState", {context, state}) == 0 && f.engine.read32(state) == 2, "kSSLConnected");
    const auto processed = f.cell();
    const auto ping = f.text_buffer("ping");
    require(f.status("_SSLWrite", {context, ping, 4, processed}) == 0 && f.engine.read64(processed) == 4, "SSLWrite");
    const auto buffer = f.process.allocate_guest(16);
    std::string received;
    while (received.size() < 4) {
        const auto status = f.status("_SSLRead", {context, buffer, 4 - received.size(), processed});
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(f.engine.read64(processed)));
        f.engine.read_bytes(buffer, bytes);
        received.append(bytes.begin(), bytes.end());
        require(status == 0 || (status == -9805 && received.size() == 4), "SSLRead status " + std::to_string(status));
    }
    require(received == "pong", "SSLRead data: " + received);
    const auto version = f.process.allocate_guest(4);
    require(f.status("_SSLGetNegotiatedProtocolVersion", {context, version}) == 0 && f.engine.read32(version) >= 8, "TLS 1.2+ negotiated");
    require(f.status("_SSLClose", {context}) == 0, "SSLClose");
    for (const auto object : {anchors, trust, context}) f.release(object);
    closesocket(socket_handle);
    server.thread.join();
    require(server.ok, "TLS server failed: " + server.error);
}

void test_code_signing(Fixture& f) {
    const auto url = f.cf.file_url("/Applications/QLab.app", true);
    const auto code = f.cell();
    require(f.status("_SecStaticCodeCreateWithPath", {url, 0, code}) == 0, "SecStaticCodeCreateWithPath");
    require(f.status("_SecStaticCodeCheckValidity", {f.engine.read64(code), 0, 0}) == sec::status::cs_unimplemented,
            "code signature checks must fail honestly");
    const auto errors = f.cell();
    require(f.status("_SecStaticCodeCheckValidityWithErrors", {f.engine.read64(code), 0, 0, errors}) == sec::status::cs_unimplemented &&
            f.error_code(f.engine.read64(errors)) == sec::status::cs_unimplemented, "SecStaticCodeCheckValidityWithErrors");
    f.release(f.engine.read64(errors));
    f.release(f.engine.read64(code));
    f.release(url);
    const auto requirement_data = f.cell();
    require(f.status("_SecRequirementCopyData", {0, 0, requirement_data}) == sec::status::cs_unimplemented, "SecRequirementCopyData");
    const auto message = f.s("_SecCopyErrorMessageString", {static_cast<Address>(static_cast<std::uint32_t>(sec::status::item_not_found)), 0});
    require(message && f.cf.utf8(message).find("could not be found") != std::string::npos, "SecCopyErrorMessageString(errSecItemNotFound)");
    f.release(message);
    require(f.s("_SecCopyErrorMessageString", {12345, 0}) == 0, "SecCopyErrorMessageString of an unknown status");
    const auto authorization = f.cell();
    require(f.status("_AuthorizationCreate", {0, 0, 0, authorization}) == 0, "AuthorizationCreate(empty)");
    // AuthorizationRights {count = 1, items}; AuthorizationItem {name, valueLength, value, flags}.
    const auto item = f.process.allocate_guest(32), rights = f.process.allocate_guest(16);
    f.engine.write64(item, f.text_buffer("system.privilege.admin")); f.engine.write64(item + 8, 0);
    f.engine.write64(item + 16, 0); f.engine.write32(item + 24, 0);
    f.engine.write32(rights, 1); f.engine.write64(rights + 8, item);
    require(f.status("_AuthorizationCopyRights", {f.engine.read64(authorization), rights, 0, 3, 0}) == sec::status::authorization_denied,
            "admin rights must be denied");
    require(f.status("_AuthorizationFree", {f.engine.read64(authorization), 0}) == 0, "AuthorizationFree");
}

void test_common_crypto(Fixture& f) {
    const auto lib = sec::libsystem_library;
    const auto digest = [&](std::string_view name, std::string_view text, std::size_t size) {
        const auto input = f.text_buffer(text), out = f.process.allocate_guest(64);
        require(f.call(lib, "_CC_" + std::string(name), {input, text.size(), out}) == out, "CC_" + std::string(name) + " return value");
        Bytes bytes(size);
        f.engine.read_bytes(out, bytes);
        return sec::hex(bytes);
    };
    require(digest("MD5", "abc", 16) == "900150983cd24fb0d6963f7d28e17f72", "CC_MD5");
    require(digest("SHA1", "abc", 20) == "a9993e364706816aba3e25717850c26c9cd0d89d", "CC_SHA1");
    require(digest("SHA224", "abc", 28) == "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7", "CC_SHA224");
    require(digest("SHA256", "abc", 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "CC_SHA256");
    require(digest("SHA256", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 32) ==
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "CC_SHA256 two blocks");
    require(digest("SHA384", "abc", 48) ==
            "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7", "CC_SHA384");
    require(digest("SHA512", "abc", 64) ==
            "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
            "CC_SHA512");
    // Init/Update/Final over uneven chunks equals the one-shot digest (contexts are guest memory).
    std::string message;
    for (int i = 0; i < 1000; ++i) message.push_back(static_cast<char>('a' + i % 26));
    for (const auto& [name, context, size] : {std::tuple{"MD5", 92, 16}, std::tuple{"SHA1", 96, 20}, std::tuple{"SHA256", 104, 32},
                                              std::tuple{"SHA512", 208, 64}}) {
        const auto ctx = f.process.allocate_guest(static_cast<std::uint64_t>(context)), out = f.process.allocate_guest(64);
        const auto input = f.text_buffer(message);
        const std::string prefix = std::string("_CC_") + name;
        require(f.call(lib, prefix + "_Init", {ctx}) == 1, prefix + "_Init");
        std::size_t offset = 0;
        for (const std::size_t chunk : {1, 63, 200, 0, 129, 607}) {
            require(f.call(lib, prefix + "_Update", {ctx, input + offset, chunk}) == 1, prefix + "_Update");
            offset += chunk;
        }
        require(offset == message.size() && f.call(lib, prefix + "_Final", {out, ctx}) == 1, prefix + "_Final");
        Bytes bytes(static_cast<std::size_t>(size));
        f.engine.read_bytes(out, bytes);
        require(sec::hex(bytes) == digest(name, message, static_cast<std::size_t>(size)), prefix + " incremental digest");
    }
    // HMAC (RFC 2104 / RFC 4231 "Jefe" vectors), one-shot and incremental.
    const auto key = f.text_buffer("Jefe"), data = f.text_buffer("what do ya want for nothing?"), mac = f.process.allocate_guest(64);
    static_cast<void>(f.call(lib, "_CCHmac", {2, key, 4, data, 28, mac}));
    Bytes value(32);
    f.engine.read_bytes(mac, value);
    require(sec::hex(value) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", "CCHmac SHA256");
    const auto ctx = f.process.allocate_guest(384);
    static_cast<void>(f.call(lib, "_CCHmacInit", {ctx, 1, key, 4}));
    static_cast<void>(f.call(lib, "_CCHmacUpdate", {ctx, data, 10}));
    static_cast<void>(f.call(lib, "_CCHmacUpdate", {ctx, data + 10, 18}));
    static_cast<void>(f.call(lib, "_CCHmacFinal", {ctx, mac}));
    value.resize(16);
    f.engine.read_bytes(mac, value);
    require(sec::hex(value) == "750c783e6ab0b503eaa86e310a5db738", "CCHmac MD5 incremental");
    const auto random = f.process.allocate_guest(32);
    require(f.call(lib, "_CCRandomGenerateBytes", {random, 32}) == 0, "CCRandomGenerateBytes");
}

} // namespace

int main() {
    try {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        Fixture fixture;
        test_random_and_errors(fixture);
        test_common_crypto(fixture);
        auto identity = make_identity(fixture);
        test_trust(fixture, identity);
        test_keys(fixture, identity);
        test_keychain(fixture);
        test_secure_transport(fixture, identity);
        test_code_signing(fixture);
        fixture.release(identity.certificate);
        std::cout << "Alchemy Security tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
