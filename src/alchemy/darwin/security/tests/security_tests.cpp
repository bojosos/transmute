// Security.framework provider tests (owner: v-netsec): random bytes,
// NSError/CFError, certificates and trust (custom anchors, host names,
// exceptions, system roots), SecKey (RSA / EC sign, verify, encrypt, decrypt,
// ECDH, external representations), SecItemImport, the DPAPI keychain, and a
// SecureTransport client against an in-process SChannel TLS server. The legacy
// SecKey calls, SecItemExport and SecCertificateCopyValues.
#include "security_internal.hpp"
#include "code_signature.hpp"
#include "keys.hpp"
#include "tls.hpp"

#include "alchemy/darwin_dispatch.hpp"
#include "alchemy/alchemy_root.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/darwin_threads.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/gui/install.hpp"
#include "alchemy/path.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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
namespace codesign = alchemy::gui::security::codesign;
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

// The legacy SecKey calls, SecItemExport and SecCertificateCopyValues.
void test_legacy_keys_and_export(Fixture& f, Identity& identity) {
    const auto error = f.cell();
    const auto buffer = [&](const Bytes& bytes) {
        const auto address = f.process.allocate_guest(bytes.size() + 1);
        f.engine.write_bytes(address, bytes);
        return address;
    };
    const auto read = [&](Address address, std::size_t size) {
        Bytes bytes(size);
        f.engine.read_bytes(address, bytes);
        return bytes;
    };
    const auto rsa_parameters = key_parameters(f, true, 2048);
    const auto rsa = f.s("_SecKeyCreateRandomKey", {rsa_parameters, error});
    const auto rsa_public = f.s("_SecKeyCopyPublicKey", {rsa});
    require(rsa && rsa_public, "RSA key for the legacy calls");
    // SecKeyRawSign with kSecPaddingPKCS1SHA256 over a digest is PKCS#1 v1.5 (deterministic): it equals
    // SecKeyCreateSignature with kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA256.
    Bytes digest(32);
    for (std::size_t index = 0; index < digest.size(); ++index) digest[index] = static_cast<std::uint8_t>(index * 7);
    const auto digest_buffer = buffer(digest);
    const auto signature_buffer = f.process.allocate_guest(512), length = f.cell();
    f.engine.write64(length, 512);
    require(f.status("_SecKeyRawSign", {rsa, 0x8004, digest_buffer, 32, signature_buffer, length}) == 0 && f.engine.read64(length) == 256,
            "SecKeyRawSign(PKCS1SHA256)");
    const auto digest_data = f.data(digest);
    const auto signature = f.s("_SecKeyCreateSignature", {rsa, f.constant("kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA256"), digest_data, error});
    require(signature && f.bytes(signature) == read(signature_buffer, 256), "SecKeyRawSign equals SecKeyCreateSignature");
    require(f.status("_SecKeyRawVerify", {rsa_public, 0x8004, digest_buffer, 32, signature_buffer, 256}) == 0, "SecKeyRawVerify");
    f.engine.write_bytes(digest_buffer + 3, Bytes{static_cast<std::uint8_t>(digest[3] ^ 1)});
    require(f.status("_SecKeyRawVerify", {rsa_public, 0x8004, digest_buffer, 32, signature_buffer, 256}) != 0, "SecKeyRawVerify accepted a changed digest");
    f.engine.write_bytes(digest_buffer + 3, Bytes{digest[3]});
    f.engine.write64(length, 100);
    require(f.status("_SecKeyRawSign", {rsa, 0x8004, digest_buffer, 32, signature_buffer, length}) == -50, "SecKeyRawSign with a short buffer");
    require(f.status("_SecKeyRawSign", {rsa, 0x8000, digest_buffer, 16, signature_buffer, length}) == -4, "kSecPaddingPKCS1MD2 is unimplemented");
    // SecKeyEncrypt / SecKeyDecrypt with kSecPaddingPKCS1 and kSecPaddingOAEP.
    const Bytes secret{'s', 'e', 's', 's', 'i', 'o', 'n', ' ', 'k', 'e', 'y'};
    for (const Address padding : {Address{1}, Address{2}}) {
        const auto cipher = f.process.allocate_guest(256), cipher_length = f.cell(), plain = f.process.allocate_guest(256), plain_length = f.cell();
        f.engine.write64(cipher_length, 256);
        f.engine.write64(plain_length, 256);
        require(f.status("_SecKeyEncrypt", {rsa_public, padding, buffer(secret), secret.size(), cipher, cipher_length}) == 0 &&
                f.engine.read64(cipher_length) == 256, "SecKeyEncrypt");
        require(f.status("_SecKeyDecrypt", {rsa, padding, cipher, 256, plain, plain_length}) == 0 &&
                read(plain, static_cast<std::size_t>(f.engine.read64(plain_length))) == secret, "SecKeyDecrypt");
    }
    // ECDSA over a digest (DER signatures), whatever the padding.
    const auto ec_parameters = key_parameters(f, false, 256);
    const auto ec = f.s("_SecKeyCreateRandomKey", {ec_parameters, error});
    const auto ec_public = f.s("_SecKeyCopyPublicKey", {ec});
    f.engine.write64(length, 512);
    require(f.status("_SecKeyRawSign", {ec, 1, digest_buffer, 32, signature_buffer, length}) == 0, "SecKeyRawSign(ECDSA)");
    require(f.status("_SecKeyRawVerify", {ec_public, 1, digest_buffer, 32, signature_buffer, f.engine.read64(length)}) == 0, "SecKeyRawVerify(ECDSA)");

    // SecItemExport.
    const auto out = f.cell();
    const auto der = identity.identity->certificate()->der();
    require(f.status("_SecItemExport", {identity.certificate, 0, 0, 0, out}) == 0 && f.bytes(f.engine.read64(out)) == der, "SecItemExport(X.509 DER)");
    f.release(f.engine.read64(out));
    require(f.status("_SecItemExport", {identity.certificate, 9, 1, 0, out}) == 0, "SecItemExport(PEM armour)");
    const auto armoured = f.bytes(f.engine.read64(out));
    const std::string armoured_text(armoured.begin(), armoured.end());
    require(armoured_text.starts_with("-----BEGIN CERTIFICATE-----\n") && armoured_text.ends_with("-----END CERTIFICATE-----\n"), "PEM certificate");
    f.release(f.engine.read64(out));
    const auto pair = f.array({identity.certificate, rsa_public});
    require(f.status("_SecItemExport", {pair, 0, 0, 0, out}) == 0, "SecItemExport(PEM sequence)");
    const auto sequence = f.engine.read64(out);
    const auto items = f.cell();
    const auto format = f.process.allocate_guest(4), type = f.process.allocate_guest(4);
    f.engine.write32(format, 0);
    f.engine.write32(type, 0);
    require(f.status("_SecItemImport", {sequence, 0, format, type, 0, 0, 0, items}) == 0 && f.engine.read32(format) == 10 &&
            f.core("_CFArrayGetCount", {f.engine.read64(items)}) == 2, "SecItemImport of the exported sequence");
    const auto imported_public = f.core("_CFArrayGetValueAtIndex", {f.engine.read64(items), 1});
    require(sec::security()->key_of(imported_public)->public_external() == sec::security()->key_of(rsa_public)->public_external(),
            "exported public key round trip");
    f.release(f.engine.read64(items));
    f.release(sequence);
    for (const auto key : {ec, rsa}) {
        require(f.status("_SecItemExport", {key, 1, 1, 0, out}) == 0, "SecItemExport(private key)");
        const auto exported = f.engine.read64(out);
        const auto text = f.bytes(exported);
        require(std::string(text.begin(), text.end()).starts_with(key == ec ? "-----BEGIN EC PRIVATE KEY-----" : "-----BEGIN RSA PRIVATE KEY-----"),
                "private key PEM label");
        require(f.status("_SecItemImport", {exported, 0, 0, 0, 0, 0, 0, items}) == 0, "SecItemImport of an exported private key");
        const auto back = f.core("_CFArrayGetValueAtIndex", {f.engine.read64(items), 0});
        require(sec::security()->key_of(back)->private_external() == sec::security()->key_of(key)->private_external(), "private key round trip");
        f.release(f.engine.read64(items));
        f.release(exported);
    }
    require(f.status("_SecItemExport", {identity.certificate, 12, 0, 0, out}) == -4, "PKCS#12 export is unimplemented");
    require(f.status("_SecItemExport", {rsa_public, 9, 0, 0, out}) == -50, "a key is not X.509");

    // SecCertificateCopyValues.
    const auto values = f.s("_SecCertificateCopyValues", {identity.certificate, 0, error});
    require(values != 0, "SecCertificateCopyValues");
    const auto field = [&](Address dictionary, std::string_view oid) { return f.core("_CFDictionaryGetValue", {dictionary, f.constant(oid)}); };
    const auto value_of = [&](Address property) { return f.core("_CFDictionaryGetValue", {property, f.constant("kSecPropertyKeyValue")}); };
    const auto subject = field(values, "kSecOIDX509V1SubjectName");
    require(subject && f.core("_CFDictionaryGetValue", {subject, f.constant("kSecPropertyKeyType")}) == f.constant("kSecPropertyTypeSection"),
            "subject name section");
    bool found_name = false;
    for (const auto entry : f.cf.array_values(value_of(subject))) {
        const auto label = f.core("_CFDictionaryGetValue", {entry, f.constant("kSecPropertyKeyLabel")});
        if (sec::bridge()->text(label) == "2.5.4.3") found_name = sec::bridge()->text(value_of(entry)) == "alchemy.test";
    }
    require(found_name, "subject common name property");
    const auto not_after = field(values, "kSecOIDX509V1ValidityNotAfter");
    require(not_after && sec::bridge()->real(value_of(not_after)) == identity.identity->certificate()->not_after(), "not-after number");
    require(sec::bridge()->text(value_of(field(values, "kSecOIDX509V1Version"))) == "3", "version");
    require(field(values, "kSecOIDX509V1SubjectPublicKey") && field(values, "kSecOIDX509V1Signature"), "public key and signature data");
    const auto names = f.core("_CFDictionaryGetValue", {values, f.string("2.5.29.17")});
    require(names != 0, "subjectAltName extension");
    std::vector<std::string> dns;
    for (const auto entry : f.cf.array_values(value_of(names)))
        if (sec::bridge()->text(f.core("_CFDictionaryGetValue", {entry, f.constant("kSecPropertyKeyLabel")})) == "DNS Name")
            dns.push_back(sec::bridge()->text(value_of(entry)));
    require(dns == std::vector<std::string>{"alchemy.test", "localhost"}, "subjectAltName DNS names");
    const auto common_names = field(values, "kSecOIDCommonName");
    require(common_names && f.core("_CFArrayGetCount", {value_of(common_names)}) == 1, "kSecOIDCommonName");
    const auto keys = f.array({f.constant("kSecOIDX509V1SubjectName"), f.constant("kSecOIDX509V1SerialNumber")});
    const auto filtered = f.s("_SecCertificateCopyValues", {identity.certificate, keys, error});
    require(filtered && f.core("_CFDictionaryGetCount", {filtered}) == 2, "SecCertificateCopyValues with keys");
    for (const auto object : {filtered, keys, values, pair, ec_public, ec, ec_parameters, signature, digest_data, rsa_public, rsa, rsa_parameters})
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

// The legacy keychain API: internet passwords, attribute lists, searches and keychain files.
void test_legacy_keychain(Fixture& f) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / ("alchemy-legacy-keychain-" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto default_path = dir / "login.keychain";
    SetEnvironmentVariableW(L"ALCHEMY_KEYCHAIN", default_path.c_str());
    const auto call_regs = [&](std::string_view name, alchemy::CallRegs registers) {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(f.engine.invoke_callback_regs(f.symbol(security_library, name), registers).x0));
    };
    const auto fourcc = [](const char* text) {
        return (static_cast<std::uint32_t>(text[0]) << 24) | (static_cast<std::uint32_t>(text[1]) << 16) | (static_cast<std::uint32_t>(text[2]) << 8) |
               static_cast<std::uint32_t>(text[3]);
    };
    const auto read_text = [&](Address data, std::uint32_t length) {
        std::string text(length, '\0');
        auto& memory = static_cast<transmute::GuestMemory&>(f.engine);
        for (std::uint32_t index = 0; index < length; ++index) text[index] = static_cast<char>(memory.read8(data + index));
        return text;
    };
    // A SecKeychainAttributeList of (tag, text) pairs.
    const auto attribute_list = [&](std::vector<std::pair<std::uint32_t, std::string>> attributes) {
        const auto array = f.process.allocate_guest(std::max<std::size_t>(attributes.size() * 16, 16)), list = f.process.allocate_guest(16);
        for (std::size_t index = 0; index < attributes.size(); ++index) {
            f.engine.write32(array + index * 16, attributes[index].first);
            f.engine.write32(array + index * 16 + 4, static_cast<std::uint32_t>(attributes[index].second.size()));
            f.engine.write64(array + index * 16 + 8, attributes[index].second.empty() ? 0 : f.text_buffer(attributes[index].second));
        }
        f.engine.write32(list, static_cast<std::uint32_t>(attributes.size()));
        f.engine.write64(list + 8, array);
        return list;
    };

    // 1. An internet password through the legacy API (stack arguments), seen by SecItem too.
    const auto server = f.text_buffer("example.com"), account = f.text_buffer("artist"), login_path = f.text_buffer("/login");
    const auto password = f.text_buffer("inet-pw"), item = f.cell();
    alchemy::CallRegs add{};
    add.x = {0, 11, server, 0, 0, 6, account, 6};
    add.push_stack(std::uint64_t{login_path});
    add.push_stack(std::uint16_t{443});
    add.push_stack(fourcc("htps"));
    add.push_stack(fourcc("dflt"));
    add.push_stack(std::uint32_t{7});
    add.push_stack(std::uint64_t{password});
    add.push_stack(std::uint64_t{item});
    require(call_regs("_SecKeychainAddInternetPassword", add) == 0 && f.engine.read64(item), "SecKeychainAddInternetPassword");
    require(call_regs("_SecKeychainAddInternetPassword", add) == sec::status::duplicate_item, "a duplicate internet password");
    const auto attributes_out = f.cell();
    const auto query = f.dictionary({{f.constant("kSecClass"), f.constant("kSecClassInternetPassword")},
                                     {f.constant("kSecAttrServer"), f.string("example.com")}, {f.constant("kSecAttrProtocol"), f.constant("kSecAttrProtocolHTTPS")},
                                     {f.constant("kSecReturnAttributes"), f.cf.boolean(true)}});
    require(f.status("_SecItemCopyMatching", {query, attributes_out}) == 0, "SecItemCopyMatching sees the legacy internet password");
    const auto found_attributes = f.engine.read64(attributes_out);
    require(f.cf.number_value(f.core("_CFDictionaryGetValue", {found_attributes, f.constant("kSecAttrPort")})).value_or(0) == 443 &&
            f.cf.utf8(f.core("_CFDictionaryGetValue", {found_attributes, f.constant("kSecAttrPath")})) == "/login", "port and path");
    f.release(found_attributes);
    // Find with wildcards (empty strings, port 0, protocol 0, any authentication type).
    const auto length = f.process.allocate_guest(4), data = f.cell(), found_item = f.cell();
    alchemy::CallRegs find{};
    find.x = {0, 11, server, 0, 0, 0, 0, 0};
    find.push_stack(std::uint64_t{0});
    find.push_stack(std::uint16_t{0});
    find.push_stack(std::uint32_t{0});
    find.push_stack(std::uint32_t{0});
    find.push_stack(std::uint64_t{length});
    find.push_stack(std::uint64_t{data});
    find.push_stack(std::uint64_t{found_item});
    require(call_regs("_SecKeychainFindInternetPassword", find) == 0 && read_text(f.engine.read64(data), f.engine.read32(length)) == "inet-pw",
            "SecKeychainFindInternetPassword");
    require(f.status("_SecKeychainItemFreeContent", {0, f.engine.read64(data)}) == 0, "free the password");
    const auto inet = f.engine.read64(found_item);

    // 2. Attribute lists: CopyAttributesAndData (chosen tags), CopyContent (the caller's list), ModifyAttributesAndData.
    const auto tags = f.process.allocate_guest(12), info = f.process.allocate_guest(24);
    f.engine.write32(tags, fourcc("srvr")); f.engine.write32(tags + 4, fourcc("port")); f.engine.write32(tags + 8, fourcc("ptcl"));
    f.engine.write32(info, 3); f.engine.write64(info + 8, tags); f.engine.write64(info + 16, 0);
    const auto item_class = f.process.allocate_guest(4), list_out = f.cell();
    require(f.status("_SecKeychainItemCopyAttributesAndData", {inet, info, item_class, list_out, length, data}) == 0, "SecKeychainItemCopyAttributesAndData");
    require(f.engine.read32(item_class) == fourcc("inet"), "the item class");
    const auto list = f.engine.read64(list_out), entries = f.engine.read64(list + 8);
    require(f.engine.read32(list) == 3 && read_text(f.engine.read64(entries + 8), f.engine.read32(entries + 4)) == "example.com" &&
            f.engine.read32(f.engine.read64(entries + 16 + 8)) == 443 && f.engine.read32(f.engine.read64(entries + 32 + 8)) == fourcc("htps"),
            "server, port and protocol attributes");
    require(read_text(f.engine.read64(data), f.engine.read32(length)) == "inet-pw", "the data with the attributes");
    require(f.status("_SecKeychainItemFreeAttributesAndData", {list, f.engine.read64(data)}) == 0, "SecKeychainItemFreeAttributesAndData");
    const auto label = attribute_list({{fourcc("labl"), "Login"}});
    require(f.status("_SecKeychainItemModifyAttributesAndData", {inet, label, 0, 0}) == 0, "SecKeychainItemModifyAttributesAndData (a label)");
    const auto content = attribute_list({{fourcc("labl"), ""}, {fourcc("acct"), ""}});
    require(f.status("_SecKeychainItemCopyContent", {inet, 0, content, 0, 0}) == 0, "SecKeychainItemCopyContent");
    const auto content_entries = f.engine.read64(content + 8);
    require(read_text(f.engine.read64(content_entries + 8), f.engine.read32(content_entries + 4)) == "Login" &&
            read_text(f.engine.read64(content_entries + 24), f.engine.read32(content_entries + 20)) == "artist", "the label and account");
    require(f.status("_SecKeychainItemFreeContent", {content, 0}) == 0, "free the content");

    // 3. CreateFromContent and a search.
    const auto generic = attribute_list({{fourcc("svce"), "com.alchemy.legacy"}, {fourcc("acct"), "searcher"}});
    const auto secret = f.text_buffer("created"), created = f.cell();
    require(f.status("_SecKeychainItemCreateFromContent", {fourcc("genp"), generic, 7, secret, 0, 0, created}) == 0, "SecKeychainItemCreateFromContent");
    const auto search = f.cell(), next = f.cell();
    const auto by_service = attribute_list({{fourcc("svce"), "com.alchemy.legacy"}});
    require(f.status("_SecKeychainSearchCreateFromAttributes", {0, fourcc("genp"), by_service, search}) == 0, "SecKeychainSearchCreateFromAttributes");
    require(f.status("_SecKeychainSearchCopyNext", {f.engine.read64(search), next}) == 0 && f.engine.read64(next), "the search finds it");
    f.release(f.engine.read64(next));
    require(f.status("_SecKeychainSearchCopyNext", {f.engine.read64(search), next}) == sec::status::item_not_found, "and only it");
    f.release(f.engine.read64(search));

    // 4. Another keychain file: create, path, status, lock and unlock with its password, the search list, delete.
    const auto other = dir / "other.keychain";
    const auto other_guest = f.text_buffer(alchemy::guest_path_string(other));
    const auto keychain_out = f.cell(), key = f.text_buffer("open sesame"), wrong = f.text_buffer("wrong");
    require(f.status("_SecKeychainCreate", {other_guest, 11, key, 0, 0, keychain_out}) == 0 && fs::exists(other), "SecKeychainCreate");
    const auto keychain = f.engine.read64(keychain_out);
    require(f.status("_SecKeychainCreate", {other_guest, 11, key, 0, 0, keychain_out}) == -25296, "an existing keychain is errSecDuplicateKeychain");
    const auto path_length = f.process.allocate_guest(4), path_text = f.process.allocate_guest(1024);
    f.engine.write32(path_length, 1024);
    require(f.status("_SecKeychainGetPath", {keychain, path_length, path_text}) == 0 &&
            read_text(path_text, f.engine.read32(path_length)) == alchemy::guest_path_string(other), "SecKeychainGetPath");
    const auto status_out = f.process.allocate_guest(4);
    require(f.status("_SecKeychainGetStatus", {keychain, status_out}) == 0 && f.engine.read32(status_out) == 7, "a new keychain is unlocked");
    const auto service_text = f.text_buffer("com.alchemy.other"), account_text = f.text_buffer("someone"), pw = f.text_buffer("pw2");
    require(f.status("_SecKeychainAddGenericPassword", {keychain, 17, service_text, 7, account_text, 3, pw, 0}) == 0, "an item in the other keychain");
    require(f.status("_SecKeychainLock", {keychain}) == 0 && f.status("_SecKeychainGetStatus", {keychain, status_out}) == 0 &&
            f.engine.read32(status_out) == 6, "SecKeychainLock");
    require(f.status("_SecKeychainFindGenericPassword", {keychain, 17, service_text, 0, 0, length, data, 0}) == sec::status::interaction_not_allowed,
            "a locked keychain answers errSecInteractionNotAllowed");
    require(f.status("_SecKeychainUnlock", {keychain, 5, wrong, 1}) == -25293, "the wrong password is errSecAuthFailed");
    require(f.status("_SecKeychainUnlock", {keychain, 11, key, 1}) == 0, "SecKeychainUnlock");
    require(f.status("_SecKeychainFindGenericPassword", {keychain, 17, service_text, 0, 0, length, data, found_item}) == 0 &&
            read_text(f.engine.read64(data), f.engine.read32(length)) == "pw2", "found after unlocking");
    require(f.status("_SecKeychainItemFreeContent", {0, f.engine.read64(data)}) == 0, "free");
    const auto item_keychain = f.cell();
    require(f.status("_SecKeychainItemCopyKeychain", {f.engine.read64(found_item), item_keychain}) == 0 &&
            f.status("_SecKeychainGetPath", {f.engine.read64(item_keychain), (f.engine.write32(path_length, 1024), path_length), path_text}) == 0 &&
            read_text(path_text, f.engine.read32(path_length)) == alchemy::guest_path_string(other), "SecKeychainItemCopyKeychain");
    require(f.status("_SecKeychainFindGenericPassword", {0, 17, service_text, 0, 0, 0, 0, 0}) == sec::status::item_not_found,
            "the default search list does not hold the other keychain");
    const auto default_out = f.cell();
    require(f.status("_SecKeychainCopyDefault", {default_out}) == 0, "SecKeychainCopyDefault");
    const auto search_list = sec::bridge()->array({f.engine.read64(default_out), keychain});
    require(f.status("_SecKeychainSetSearchList", {search_list}) == 0 &&
            f.status("_SecKeychainFindGenericPassword", {0, 17, service_text, 0, 0, 0, 0, 0}) == 0, "the search list reaches it");
    const auto list_out2 = f.cell();
    require(f.status("_SecKeychainCopySearchList", {list_out2}) == 0 && f.cf.length(f.engine.read64(list_out2)) == 2, "SecKeychainCopySearchList");
    require(f.status("_SecKeychainDelete", {keychain}) == 0 && !fs::exists(other), "SecKeychainDelete");
    require(f.status("_SecKeychainGetStatus", {keychain, status_out}) == -25294, "a deleted keychain is errSecNoSuchKeychain");
    const auto reset = sec::bridge()->array({f.engine.read64(default_out)});
    require(f.status("_SecKeychainSetSearchList", {reset}) == 0, "reset the search list");

    // 5. Access control: recorded with an item and reported (nothing prompts).
    const auto app = f.cell(), app_data = f.cell();
    require(f.status("_SecTrustedApplicationCreateFromPath", {f.text_buffer("/Applications/Example.app"), app}) == 0 &&
            f.status("_SecTrustedApplicationCopyData", {f.engine.read64(app), app_data}) == 0 &&
            f.bytes(f.engine.read64(app_data)) == Bytes{'/', 'A', 'p', 'p', 'l', 'i', 'c', 'a', 't', 'i', 'o', 'n', 's', '/', 'E', 'x', 'a', 'm', 'p', 'l', 'e', '.',
                                                       'a', 'p', 'p'}, "SecTrustedApplicationCreateFromPath / CopyData");
    const auto trusted = sec::bridge()->array({f.engine.read64(app)});
    const auto access_out = f.cell();
    require(f.status("_SecAccessCreate", {f.string("Example access"), trusted, access_out}) == 0 &&
            f.core("_CFGetTypeID", {f.engine.read64(access_out)}) == f.s("_SecAccessGetTypeID"), "SecAccessCreate");
    const auto acls_out = f.cell();
    require(f.status("_SecAccessCopyACLList", {f.engine.read64(access_out), acls_out}) == 0 && f.cf.length(f.engine.read64(acls_out)) == 2,
            "SecAccessCopyACLList: an owner and a use ACL");
    const auto use_acl = f.core("_CFArrayGetValueAtIndex", {f.engine.read64(acls_out), 1});
    const auto apps_out = f.cell(), description_out = f.cell(), prompt_out = f.process.allocate_guest(2);
    require(f.status("_SecACLCopyContents", {use_acl, apps_out, description_out, prompt_out}) == 0 && f.engine.read64(apps_out) &&
            f.cf.length(f.engine.read64(apps_out)) == 1 && f.cf.utf8(f.engine.read64(description_out)) == "Example access", "SecACLCopyContents");
    // Any application, as Chrome sets its item's ACLs.
    require(f.status("_SecACLSetContents", {use_acl, 0, f.string("Any application"), 0}) == 0 &&
            f.status("_SecACLCopyContents", {use_acl, apps_out, description_out, prompt_out}) == 0 && f.engine.read64(apps_out) == 0,
            "SecACLSetContents (any application)");
    const auto acl_attributes = attribute_list({{fourcc("svce"), "com.alchemy.acl"}, {fourcc("acct"), "acl"}});
    const auto acl_item = f.cell();
    require(f.status("_SecKeychainItemCreateFromContent", {fourcc("genp"), acl_attributes, 3, f.text_buffer("abc"), 0, f.engine.read64(access_out), acl_item}) == 0,
            "SecKeychainItemCreateFromContent with an initial access");
    const auto item_access = f.cell(), item_acls = f.cell();
    require(f.status("_SecKeychainItemCopyAccess", {f.engine.read64(acl_item), item_access}) == 0 &&
            f.status("_SecAccessCopyACLList", {f.engine.read64(item_access), item_acls}) == 0 && f.cf.length(f.engine.read64(item_acls)) == 2,
            "SecKeychainItemCopyAccess");
    const auto stored_use = f.core("_CFArrayGetValueAtIndex", {f.engine.read64(item_acls), 1});
    require(f.status("_SecACLCopyContents", {stored_use, apps_out, description_out, prompt_out}) == 0 && f.engine.read64(apps_out) == 0 &&
            f.cf.utf8(f.engine.read64(description_out)) == "Any application", "the item keeps the access it was created with");
    const auto none_out = f.cell();
    require(f.status("_SecAccessCreate", {f.string("No application"), sec::bridge()->array({}), none_out}) == 0 &&
            f.status("_SecKeychainItemSetAccess", {f.engine.read64(acl_item), f.engine.read64(none_out)}) == 0 &&
            f.status("_SecKeychainItemCopyAccess", {f.engine.read64(acl_item), item_access}) == 0 &&
            f.status("_SecAccessCopyACLList", {f.engine.read64(item_access), item_acls}) == 0 &&
            f.status("_SecACLCopyContents", {f.core("_CFArrayGetValueAtIndex", {f.engine.read64(item_acls), 1}), apps_out, 0, 0}) == 0 &&
            f.engine.read64(apps_out) && f.cf.length(f.engine.read64(apps_out)) == 0, "SecKeychainItemSetAccess (an empty trusted list)");
    require(f.status("_SecKeychainItemDelete", {f.engine.read64(acl_item)}) == 0, "delete the access-controlled item");
    for (const auto object : {reset, search_list, f.engine.read64(list_out2), f.engine.read64(default_out), f.engine.read64(item_keychain), keychain,
                              f.engine.read64(created), inet, f.engine.read64(item), query})
        f.release(object);
    SetEnvironmentVariableW(L"ALCHEMY_KEYCHAIN", nullptr);
    fs::remove_all(dir);
}

constexpr std::uint8_t test_certificate_der[] = {
    0x30, 0x82, 0x01, 0x98, 0x30, 0x82, 0x01, 0x3d, 0xa0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x14, 0x24, 0x0b, 0x47,
    0x4f, 0x0c, 0x02, 0x4f, 0xc3, 0x15, 0x59, 0xc1, 0x5f, 0x06, 0xf2, 0x9f, 0xee, 0xab, 0xd9, 0x5d, 0xb0, 0x30,
    0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x30, 0x20, 0x31, 0x1e, 0x30, 0x1c, 0x06,
    0x03, 0x55, 0x04, 0x03, 0x0c, 0x15, 0x41, 0x6c, 0x63, 0x68, 0x65, 0x6d, 0x79, 0x20, 0x4b, 0x65, 0x79, 0x63,
    0x68, 0x61, 0x69, 0x6e, 0x20, 0x54, 0x65, 0x73, 0x74, 0x30, 0x20, 0x17, 0x0d, 0x32, 0x36, 0x31, 0x31, 0x30,
    0x36, 0x30, 0x34, 0x34, 0x30, 0x32, 0x39, 0x5a, 0x18, 0x0f, 0x32, 0x31, 0x32, 0x36, 0x31, 0x30, 0x31, 0x33,
    0x30, 0x34, 0x34, 0x30, 0x32, 0x39, 0x5a, 0x30, 0x20, 0x31, 0x1e, 0x30, 0x1c, 0x06, 0x03, 0x55, 0x04, 0x03,
    0x0c, 0x15, 0x41, 0x6c, 0x63, 0x68, 0x65, 0x6d, 0x79, 0x20, 0x4b, 0x65, 0x79, 0x63, 0x68, 0x61, 0x69, 0x6e,
    0x20, 0x54, 0x65, 0x73, 0x74, 0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
    0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04, 0x18, 0x59, 0x3c, 0xa6,
    0x92, 0xb3, 0x81, 0x45, 0x28, 0x5b, 0x4d, 0xc7, 0x29, 0x72, 0xa1, 0xb7, 0xf1, 0x25, 0x85, 0x4f, 0xc6, 0xa3,
    0x78, 0xe5, 0xa3, 0xda, 0x54, 0x97, 0x13, 0x20, 0x0e, 0xbb, 0x30, 0x59, 0x37, 0x61, 0x63, 0x32, 0x38, 0xd3,
    0x50, 0xb4, 0x6d, 0xd3, 0x7e, 0xb0, 0x30, 0xe8, 0x91, 0xde, 0xbf, 0x2f, 0x47, 0x82, 0x76, 0x60, 0x78, 0xe9,
    0xf1, 0xa4, 0xe5, 0xf6, 0x26, 0x90, 0xa3, 0x53, 0x30, 0x51, 0x30, 0x1d, 0x06, 0x03, 0x55, 0x1d, 0x0e, 0x04,
    0x16, 0x04, 0x14, 0x51, 0x0b, 0xd7, 0xe4, 0xb9, 0xbe, 0x7c, 0x5d, 0xed, 0x13, 0x04, 0x11, 0x09, 0x05, 0xa7,
    0x6b, 0x8c, 0xec, 0x07, 0x54, 0x30, 0x1f, 0x06, 0x03, 0x55, 0x1d, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80, 0x14,
    0x51, 0x0b, 0xd7, 0xe4, 0xb9, 0xbe, 0x7c, 0x5d, 0xed, 0x13, 0x04, 0x11, 0x09, 0x05, 0xa7, 0x6b, 0x8c, 0xec,
    0x07, 0x54, 0x30, 0x0f, 0x06, 0x03, 0x55, 0x1d, 0x13, 0x01, 0x01, 0xff, 0x04, 0x05, 0x30, 0x03, 0x01, 0x01,
    0xff, 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x03, 0x49, 0x00, 0x30, 0x46,
    0x02, 0x21, 0x00, 0xa9, 0x27, 0xaa, 0x80, 0x6e, 0x33, 0x9e, 0xf3, 0xe1, 0x8b, 0xb7, 0xde, 0xd1, 0xed, 0x25,
    0xdf, 0x6c, 0xe6, 0x81, 0x57, 0xa6, 0xca, 0x9d, 0xc4, 0xf3, 0x29, 0xba, 0xc1, 0xde, 0x92, 0xc2, 0xf7, 0x02,
    0x21, 0x00, 0xcb, 0x2a, 0x83, 0x56, 0x8a, 0x67, 0x67, 0xdf, 0xfc, 0x45, 0x27, 0x7c, 0x01, 0x91, 0x2e, 0xbc,
    0xb1, 0xb7, 0x57, 0x88, 0x10, 0x6a, 0xce, 0x83, 0x6b, 0xe4, 0x20, 0x2b, 0xe3, 0x8f, 0x87, 0xb9,
};
constexpr std::uint8_t test_private_key[] = {
    0x04, 0x18, 0x59, 0x3c, 0xa6, 0x92, 0xb3, 0x81, 0x45, 0x28, 0x5b, 0x4d, 0xc7, 0x29, 0x72, 0xa1, 0xb7, 0xf1,
    0x25, 0x85, 0x4f, 0xc6, 0xa3, 0x78, 0xe5, 0xa3, 0xda, 0x54, 0x97, 0x13, 0x20, 0x0e, 0xbb, 0x30, 0x59, 0x37,
    0x61, 0x63, 0x32, 0x38, 0xd3, 0x50, 0xb4, 0x6d, 0xd3, 0x7e, 0xb0, 0x30, 0xe8, 0x91, 0xde, 0xbf, 0x2f, 0x47,
    0x82, 0x76, 0x60, 0x78, 0xe9, 0xf1, 0xa4, 0xe5, 0xf6, 0x26, 0x90, 0x21, 0x8b, 0xf4, 0xb8, 0x1b, 0x3c, 0xf2,
    0xda, 0x4d, 0xa3, 0x5e, 0x36, 0x22, 0x5a, 0x97, 0xb3, 0x1b, 0x8c, 0x00, 0x25, 0xc3, 0xac, 0x7b, 0xb2, 0x8a,
    0xdc, 0x37, 0x92, 0x7b, 0xab, 0x44, 0xab,
};

// Keys, certificates and identities in the keychain file (keychain.cpp). The fixture is a P-256 key
// and a self-signed certificate of it ("CN=Alchemy Keychain Test"), made for this test.
void test_keychain_keys(Fixture& f) {
    const auto path = std::filesystem::temp_directory_path() / ("alchemy-keychain-keys-" + std::to_string(GetCurrentProcessId()) + ".db");
    std::filesystem::remove(path);
    SetEnvironmentVariableW(L"ALCHEMY_KEYCHAIN", path.c_str());
    const auto k = [&](std::string_view name) { return f.constant(name); };
    const auto yes = f.cf.boolean(true), no = f.cf.boolean(false);
    const auto error = f.cell(), out = f.cell(), other = f.cell();
    const auto key_data = f.data(std::span(test_private_key));
    const auto key_type = f.dictionary({{k("kSecAttrKeyType"), k("kSecAttrKeyTypeECSECPrimeRandom")}, {k("kSecAttrKeyClass"), k("kSecAttrKeyClassPrivate")}});
    const auto key = f.s("_SecKeyCreateWithData", {key_data, key_type, error});
    const auto certificate_data = f.data(std::span(test_certificate_der));
    const auto certificate = f.s("_SecCertificateCreateWithData", {0, certificate_data});
    require(key && certificate, "the test key and certificate");

    // 1. A certificate alone is no identity.
    require(f.status("_SecIdentityCreateWithCertificate", {0, certificate, out}) == sec::status::item_not_found, "no stored key: no identity");
    const auto add_certificate = f.dictionary({{k("kSecClass"), k("kSecClassCertificate")}, {k("kSecValueRef"), certificate}});
    require(f.status("_SecItemAdd", {add_certificate, 0}) == 0, "SecItemAdd of a certificate");
    require(f.status("_SecItemAdd", {add_certificate, 0}) == sec::status::duplicate_item, "the same certificate again");
    const auto identities = f.dictionary({{k("kSecClass"), k("kSecClassIdentity")}, {k("kSecReturnRef"), yes}});
    require(f.status("_SecItemCopyMatching", {identities, out}) == sec::status::item_not_found, "a certificate without its key is no identity");
    const auto certificate_query = f.dictionary({{k("kSecClass"), k("kSecClassCertificate")}, {k("kSecReturnAttributes"), yes}});
    require(f.status("_SecItemCopyMatching", {certificate_query, out}) == 0, "the certificate is stored");
    const auto certificate_attributes = f.engine.read64(out);
    require(f.cf.utf8(f.core("_CFDictionaryGetValue", {certificate_attributes, k("kSecAttrLabel")})) == "Alchemy Keychain Test" &&
            f.core("_CFDictionaryGetValue", {certificate_attributes, k("kSecAttrPublicKeyHash")}) != 0,
            "a certificate's label and public key hash");

    // 2. The private key, stored non-extractable: its bytes stay in the keychain, it still signs.
    const auto add_key = f.dictionary({{k("kSecClass"), k("kSecClassKey")}, {k("kSecValueRef"), key}, {k("kSecAttrLabel"), f.string("Test key")},
                                       {k("kSecAttrIsExtractable"), no}, {k("kSecReturnRef"), yes}});
    require(f.status("_SecItemAdd", {add_key, out}) == 0 && f.engine.read64(out), "SecItemAdd of a key, returning its reference");
    const auto stored_key = f.engine.read64(out);
    require(f.s("_SecKeyCopyExternalRepresentation", {stored_key, error}) == 0 && f.error_code(f.engine.read64(error)) == sec::status::data_not_available,
            "a non-extractable key refuses its external representation");
    const auto message = f.data("keychain message");
    const auto ecdsa = k("kSecKeyAlgorithmECDSASignatureMessageX962SHA256");
    const auto signature = f.s("_SecKeyCreateSignature", {stored_key, ecdsa, message, error});
    const auto public_key = f.s("_SecKeyCopyPublicKey", {key});
    require(signature && (f.s("_SecKeyVerifySignature", {public_key, ecdsa, message, signature, error}) & 0xff) == 1, "the stored key signs");
    const auto key_data_query = f.dictionary({{k("kSecClass"), k("kSecClassKey")}, {k("kSecAttrLabel"), f.string("Test key")}, {k("kSecReturnData"), yes}});
    require(f.status("_SecItemCopyMatching", {key_data_query, out}) == sec::status::data_not_available, "no data for a non-extractable key");
    const auto key_query = f.dictionary({{k("kSecClass"), k("kSecClassKey")}, {k("kSecAttrKeyClass"), k("kSecAttrKeyClassPrivate")},
                                         {k("kSecReturnAttributes"), yes}});
    require(f.status("_SecItemCopyMatching", {key_query, out}) == 0, "the key is found by its class");
    const auto key_attributes = f.engine.read64(out);
    require(f.core("_CFDictionaryGetValue", {key_attributes, k("kSecAttrIsExtractable")}) == no &&
            f.core("_CFDictionaryGetValue", {key_attributes, k("kSecAttrKeyType")}) != 0 &&
            f.core("_CFEqual", {f.core("_CFDictionaryGetValue", {key_attributes, k("kSecAttrApplicationLabel")}),
                                f.core("_CFDictionaryGetValue", {certificate_attributes, k("kSecAttrPublicKeyHash")})}) == 1,
            "a key's attributes (its application label is the certificate's public key hash)");
    // The file is encrypted: the private scalar does not appear in it.
    const auto stored = [&] {   // read and closed (an open file would keep the next save from replacing it)
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }();
    const std::string scalar(reinterpret_cast<const char*>(test_private_key) + 65, 32);
    require(!stored.empty() && stored.find(scalar) == std::string::npos, "the keychain file does not hold the key in the clear");

    // 3. The identity: the certificate with its stored key.
    require(f.status("_SecIdentityCreateWithCertificate", {0, certificate, out}) == 0, "SecIdentityCreateWithCertificate");
    const auto identity = f.engine.read64(out);
    require(f.status("_SecIdentityCopyPrivateKey", {identity, other}) == 0 &&
            f.s("_SecKeyCopyExternalRepresentation", {f.engine.read64(other), error}) == 0, "the identity's private key (still not extractable)");
    require(f.status("_SecIdentityCopyCertificate", {identity, other}) == 0 &&
            f.bytes(f.s("_SecCertificateCopyData", {f.engine.read64(other)})) == Bytes(std::begin(test_certificate_der), std::end(test_certificate_der)),
            "the identity's certificate");
    require(f.status("_SecItemCopyMatching", {identities, out}) == 0 && f.engine.read64(out), "SecItemCopyMatching(kSecClassIdentity)");
    const auto all_identities = f.dictionary({{k("kSecClass"), k("kSecClassIdentity")}, {k("kSecReturnRef"), yes}, {k("kSecMatchLimit"), k("kSecMatchLimitAll")}});
    require(f.status("_SecItemCopyMatching", {all_identities, out}) == 0 && f.core("_CFArrayGetCount", {f.engine.read64(out)}) == 1, "one identity");
    const auto by_reference = f.dictionary({{k("kSecValueRef"), identity}, {k("kSecReturnAttributes"), yes}});
    require(f.status("_SecItemCopyMatching", {by_reference, out}) == 0, "an identity found by its reference");

    // 4. Preferences.
    const auto name = f.string("https://alchemy.test/");
    require(f.status("_SecIdentitySetPreferred", {identity, name, 0}) == 0 && f.s("_SecIdentityCopyPreferred", {name, 0, 0}) != 0,
            "SecIdentitySetPreferred / CopyPreferred");
    const auto unrelated = f.array({f.data("not an issuer")});
    require(f.s("_SecIdentityCopyPreferred", {name, 0, unrelated}) == 0, "a preferred identity from another issuer is not offered");
    require(f.s("_SecIdentityCopyPreferred", {f.string("https://other.test/"), 0, 0}) == 0, "no preference under another name");

    // A keychain identity cannot serve TLS yet (SecureTransport needs a host identity): refused explicitly.
    const auto context = f.s("_SSLCreateContext", {0, 1, 0});
    const auto chain = f.array({identity});
    require(context && f.status("_SSLSetCertificate", {context, chain}) == sec::status::unimplemented, "SSLSetCertificate(keychain identity)");
    f.release(context);

    // 5. Deleting the identity deletes its certificate and key; the preference then names nothing.
    const auto delete_identity = f.dictionary({{k("kSecClass"), k("kSecClassIdentity")}, {k("kSecValueRef"), identity}});
    require(f.status("_SecItemDelete", {delete_identity}) == 0, "SecItemDelete of the identity");
    require(f.status("_SecItemCopyMatching", {certificate_query, out}) == sec::status::item_not_found &&
            f.status("_SecItemCopyMatching", {key_query, out}) == sec::status::item_not_found, "its certificate and key are gone");
    require(f.s("_SecIdentityCopyPreferred", {name, 0, 0}) == 0, "a preference for a deleted identity offers nothing");
    require(f.status("_SecIdentitySetPreferred", {0, name, 0}) == 0, "removing the preference");

    // 6. SecItemAdd of an identity stores both halves.
    const auto identity_add = f.dictionary({{k("kSecValueRef"), identity}, {k("kSecReturnRef"), yes}});
    require(f.status("_SecItemAdd", {identity_add, out}) == 0 && f.engine.read64(out), "SecItemAdd of an identity");
    require(f.status("_SecItemAdd", {identity_add, out}) == sec::status::duplicate_item, "the same identity again");
    require(f.status("_SecItemDelete", {delete_identity}) == 0, "and its deletion");

    // 7. Permanent generated keys: both halves are stored with the label.
    const auto size = f.cf.integer(256, 4);
    const auto generated_label = f.string("Generated key");
    const auto parameters = f.dictionary({{k("kSecAttrKeyType"), k("kSecAttrKeyTypeECSECPrimeRandom")}, {k("kSecAttrKeySizeInBits"), size},
                                          {k("kSecAttrIsPermanent"), yes}, {k("kSecAttrLabel"), generated_label}});
    const auto generated = f.s("_SecKeyCreateRandomKey", {parameters, error});
    require(generated && f.core("_CFDictionaryGetValue", {f.s("_SecKeyCopyAttributes", {generated}), k("kSecAttrIsPermanent")}) == yes,
            "SecKeyCreateRandomKey(kSecAttrIsPermanent)");
    const auto generated_query = f.dictionary({{k("kSecClass"), k("kSecClassKey")}, {k("kSecAttrLabel"), generated_label},
                                               {k("kSecReturnRef"), yes}, {k("kSecMatchLimit"), k("kSecMatchLimitAll")}});
    require(f.status("_SecItemCopyMatching", {generated_query, out}) == 0 && f.core("_CFArrayGetCount", {f.engine.read64(out)}) == 2,
            "the private and public keys are stored");
    const auto generated_delete = f.dictionary({{k("kSecClass"), k("kSecClassKey")}, {k("kSecAttrLabel"), generated_label}});
    require(f.status("_SecItemDelete", {generated_delete}) == 0 && f.status("_SecItemCopyMatching", {generated_query, out}) == sec::status::item_not_found,
            "deleting them");
    // Token (Secure Enclave) keys are still refused.
    const auto token = f.dictionary({{k("kSecAttrKeyType"), k("kSecAttrKeyTypeECSECPrimeRandom")}, {k("kSecAttrKeySizeInBits"), size},
                                     {k("kSecAttrTokenID"), k("kSecAttrTokenIDSecureEnclave")}});
    require(f.s("_SecKeyCreateRandomKey", {token, error}) == 0 && f.error_code(f.engine.read64(error)) == sec::status::unimplemented,
            "Secure Enclave keys are refused");

    SetEnvironmentVariableW(L"ALCHEMY_KEYCHAIN", nullptr);
    std::filesystem::remove(path);
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

// An ad-hoc signed arm64 Mach-O: two pages of code, then a SuperBlob with one SHA-256 CodeDirectory whose special
// slots carry `info` (slot 1) and `resources` (slot 3) when given. Built here from the public layout.
std::vector<std::uint8_t> adhoc_macho(const std::string& identifier, const std::vector<std::uint8_t>& info = {},
                                      const std::vector<std::uint8_t>& resources = {}) {
    const auto put32le = [](std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) {
        for (int index = 0; index < 4; ++index) out[at + index] = static_cast<std::uint8_t>(value >> (8 * index));
    };
    const auto put32 = [](std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) {
        for (int index = 0; index < 4; ++index) out[at + index] = static_cast<std::uint8_t>(value >> (24 - 8 * index));
    };
    constexpr std::uint32_t code_limit = 8192;
    std::vector<std::uint8_t> file(code_limit);
    for (std::size_t index = 48; index < file.size(); ++index) file[index] = static_cast<std::uint8_t>(index * 31);
    put32le(file, 0, 0xfeedfacf); put32le(file, 4, 0x0100000c); put32le(file, 8, 0); put32le(file, 12, 2);
    put32le(file, 16, 1); put32le(file, 20, 16); put32le(file, 24, 0); put32le(file, 28, 0);
    const std::uint32_t special = resources.empty() ? (info.empty() ? 0 : 1) : 3;
    const std::uint32_t header = 88, ident_offset = header;
    const auto ident_size = static_cast<std::uint32_t>(identifier.size() + 1);
    const std::uint32_t hash_offset = ident_offset + ident_size + special * 32;
    const std::uint32_t cd_size = hash_offset + 2 * 32;
    std::vector<std::uint8_t> cd(cd_size);
    put32(cd, 0, 0xfade0c02); put32(cd, 4, cd_size); put32(cd, 8, 0x20400); put32(cd, 12, 0x2);
    put32(cd, 16, hash_offset); put32(cd, 20, ident_offset); put32(cd, 24, special); put32(cd, 28, 2); put32(cd, 32, code_limit);
    cd[36] = 32; cd[37] = 2; cd[38] = 0; cd[39] = 12;
    std::copy(identifier.begin(), identifier.end(), cd.begin() + ident_offset);
    const auto sha256 = [](std::span<const std::uint8_t> data) { return *sec::digest("SHA256", data); };
    if (!info.empty()) { const auto h = sha256(info); std::copy(h.begin(), h.end(), cd.begin() + hash_offset - 32); }
    if (!resources.empty()) { const auto h = sha256(resources); std::copy(h.begin(), h.end(), cd.begin() + hash_offset - 96); }
    const std::uint32_t superblob = 20 + cd_size;
    put32le(file, 32, 0x1d); put32le(file, 36, 16); put32le(file, 40, code_limit); put32le(file, 44, superblob);
    for (int page = 0; page < 2; ++page) {
        const auto h = sha256(std::span(file).subspan(static_cast<std::size_t>(page) * 4096, 4096));
        std::copy(h.begin(), h.end(), cd.begin() + hash_offset + page * 32);
    }
    std::vector<std::uint8_t> blob(20);
    put32(blob, 0, 0xfade0cc0); put32(blob, 4, superblob); put32(blob, 8, 1); put32(blob, 12, 0); put32(blob, 16, 20);
    blob.insert(blob.end(), cd.begin(), cd.end());
    file.insert(file.end(), blob.begin(), blob.end());
    return file;
}

void test_code_signing(Fixture& f) {
    namespace fs = std::filesystem;
    const auto temp = fs::temp_directory_path() / ("alchemy-codesign-" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(temp);
    fs::create_directories(temp);
    const auto write = [](const fs::path& path, const std::vector<std::uint8_t>& bytes) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    };
    const auto static_code = [&](const fs::path& path) {
        const auto url = f.cf.file_url(alchemy::guest_path_string(path), fs::is_directory(path));
        const auto out = f.cell();
        require(f.status("_SecStaticCodeCreateWithPath", {url, 0, out}) == 0, "SecStaticCodeCreateWithPath");
        f.release(url);
        return f.engine.read64(out);
    };
    const auto info_of = [&](Address code, std::uint32_t flags) {
        const auto out = f.cell();
        require(f.status("_SecCodeCopySigningInformation", {code, flags, out}) == 0, "SecCodeCopySigningInformation");
        return f.engine.read64(out);
    };
    const auto value = [&](Address dictionary, std::string_view key) {
        const auto name = f.cf.string(key);
        const auto found = f.core("_CFDictionaryGetValue", {dictionary, name});
        f.release(name);
        return found;
    };
    const auto validity = [&](Address code, std::uint32_t flags) { return f.status("_SecStaticCodeCheckValidity", {code, flags, 0}); };
    const auto requirement = [&](std::string_view text) {
        const auto source = f.cf.string(text), out = f.cell();
        const auto status = f.status("_SecRequirementCreateWithString", {source, 0, out});
        f.release(source);
        require(status == 0, "SecRequirementCreateWithString: " + std::string(text));
        return f.engine.read64(out);
    };
    const auto requirement_text = [&](Address object) {
        const auto out = f.cell();
        require(f.status("_SecRequirementCopyString", {object, 0, out}) == 0, "SecRequirementCopyString");
        const auto text = f.cf.utf8(f.engine.read64(out));
        f.release(f.engine.read64(out));
        return text;
    };
    const auto satisfies = [&](Address code, std::string_view text) {
        const auto object = requirement(text);
        const auto status = f.status("_SecStaticCodeCheckValidity", {code, 0, object});
        f.release(object);
        return status;
    };

    // 0. The requirement language: Apple's text round-trips through the binary form; bad text is errSecCSReqInvalid.
    const std::string developer_id = "anchor apple generic and identifier \"com.snoize.SnoizeMIDI\" and (certificate "
        "leaf[field.1.2.840.113635.100.6.1.9] /* exists */ or certificate 1[field.1.2.840.113635.100.6.2.6] /* exists */ and "
        "certificate leaf[field.1.2.840.113635.100.6.1.13] /* exists */ and certificate leaf[subject.OU] = \"7672N4CCJM\")";
    const auto parsed_requirement = requirement(developer_id);
    require(requirement_text(parsed_requirement) == developer_id, "a Developer ID requirement round-trips: " + requirement_text(parsed_requirement));
    const auto data_out = f.cell(), copy_out = f.cell();
    require(f.status("_SecRequirementCopyData", {parsed_requirement, 0, data_out}) == 0 &&
                f.status("_SecRequirementCreateWithData", {f.engine.read64(data_out), 0, copy_out}) == 0 &&
                requirement_text(f.engine.read64(copy_out)) == developer_id, "SecRequirementCopyData / CreateWithData");
    for (const auto object : {parsed_requirement, f.engine.read64(data_out), f.engine.read64(copy_out)}) f.release(object);
    require(requirement_text(requirement("identifier libndi or ! (info[CFBundleVersion] >= \"5\" and cdhash H\"0a0b\")")) ==
            "identifier libndi or ! (info[CFBundleVersion] >= \"5\" and cdhash H\"0a0b\")", "or / not / info / cdhash text");
    const auto bad = f.cf.string("identifier and"), bad_errors = f.cell(), bad_out = f.cell();
    require(f.status("_SecRequirementCreateWithStringAndErrors", {bad, 0, bad_errors, bad_out}) == codesign::cs::requirement_invalid &&
            f.engine.read64(bad_errors) != 0, "an unparsable requirement");
    f.release(f.engine.read64(bad_errors));
    f.release(bad);

    // 1. An ad-hoc signed Mach-O: valid; its signing information; a modified page and an unsigned file fail.
    const auto binary = temp / "adhoc";
    auto bytes = adhoc_macho("com.alchemy.adhoc");
    write(binary, bytes);
    auto code = static_code(binary);
    require(validity(code, 0) == 0, "an ad-hoc signature verifies: " + std::to_string(validity(code, 0)));
    const auto info = info_of(code, 0);
    require(f.cf.utf8(value(info, "identifier")) == "com.alchemy.adhoc", "the identifier");
    require(f.cf.number_value(value(info, "flags")).value_or(0) == 2 && f.cf.number_value(value(info, "digest-algorithm")).value_or(0) == 2,
            "the CodeDirectory flags (ad-hoc) and hash type (SHA-256)");
    require(f.cf.length(value(info, "unique")) == 20 && f.cf.utf8(value(info, "format")) == "Mach-O thin (arm64)", "the cdhash and format");
    require(f.cf.utf8(f.cf.description(value(info, "main-executable"))).find("adhoc") != std::string::npos, "the main executable");
    f.release(info);
    const auto path_out = f.cell();
    require(f.status("_SecCodeCopyPath", {code, 0, path_out}) == 0 && f.cf.url_path(f.engine.read64(path_out)).ends_with("/adhoc"), "SecCodeCopyPath");
    f.release(f.engine.read64(path_out));
    const auto designated_out = f.cell();
    require(f.status("_SecCodeCopyDesignatedRequirement", {code, 0, designated_out}) == 0 &&
            requirement_text(f.engine.read64(designated_out)).starts_with("cdhash H\""), "an ad-hoc designated requirement is its cdhash");
    require(f.status("_SecStaticCodeCheckValidity", {code, 0, f.engine.read64(designated_out)}) == 0, "it satisfies its designated requirement");
    f.release(f.engine.read64(designated_out));
    require(satisfies(code, "identifier \"com.alchemy.adhoc\"") == 0, "identifier requirement");
    require(satisfies(code, "identifier other") == codesign::cs::requirement_failed, "a failing requirement is errSecCSReqFailed");
    require(satisfies(code, "anchor apple generic") == codesign::cs::requirement_failed, "ad-hoc code has no anchor");
    f.release(code);
    bytes[5000] ^= 0xff;
    write(binary, bytes);
    code = static_code(binary);
    const auto errors = f.cell();
    require(f.status("_SecStaticCodeCheckValidityWithErrors", {code, 0, 0, errors}) == codesign::cs::signature_failed &&
            f.error_code(f.engine.read64(errors)) == codesign::cs::signature_failed, "a modified page fails the signature");
    f.release(f.engine.read64(errors));
    require(validity(code, 2) == 0, "kSecCSDoNotValidateExecutable skips the pages");
    f.release(code);
    std::vector<std::uint8_t> plain(4096);
    for (const auto [at, word] : {std::pair{0, 0xfeedfacfU}, std::pair{4, 0x0100000cU}, std::pair{12, 2U}})
        for (int index = 0; index < 4; ++index) plain[at + index] = static_cast<std::uint8_t>(word >> (8 * index));
    write(temp / "plain", plain);
    code = static_code(temp / "plain");
    require(validity(code, 0) == codesign::cs::unsigned_code, "an unsigned Mach-O is errSecCSUnsigned");
    const auto plain_info = info_of(code, 0);
    require(value(plain_info, "identifier") == 0 && value(plain_info, "main-executable") != 0, "unsigned code has no identifier");
    f.release(plain_info);
    f.release(code);

    // 2. A bundle with Info.plist and a sealed resource: valid; an altered or missing resource fails with its path.
    const auto bundle = temp / "Sealed.app";
    const std::string plist_text = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist version=\"1.0\"><dict>"
        "<key>CFBundleExecutable</key><string>Sealed</string><key>CFBundleIdentifier</key><string>com.alchemy.sealed</string></dict></plist>";
    const std::vector<std::uint8_t> plist(plist_text.begin(), plist_text.end());
    const std::string resource_text = "sealed resource";
    const auto resource_hash = *sec::digest("SHA256", std::span(reinterpret_cast<const std::uint8_t*>(resource_text.data()), resource_text.size()));
    std::string hash_base64 = sec::base64_encode(resource_hash);
    const std::string resources_text = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist version=\"1.0\"><dict><key>files2</key><dict>"
        "<key>Resources/note.txt</key><dict><key>hash2</key><data>" + hash_base64 + "</data></dict></dict></dict></plist>";
    const std::vector<std::uint8_t> resources(resources_text.begin(), resources_text.end());
    write(bundle / "Contents" / "Info.plist", plist);
    write(bundle / "Contents" / "_CodeSignature" / "CodeResources", resources);
    write(bundle / "Contents" / "Resources" / "note.txt", std::vector<std::uint8_t>(resource_text.begin(), resource_text.end()));
    write(bundle / "Contents" / "MacOS" / "Sealed", adhoc_macho("com.alchemy.sealed", plist, resources));
    code = static_code(bundle);
    require(validity(code, 0) == 0, "a sealed bundle verifies: " + std::to_string(validity(code, 0)));
    const auto bundle_info = info_of(code, 0);
    require(value(bundle_info, "info-plist") != 0 && f.cf.utf8(value(bundle_info, "format")).starts_with("app bundle with "),
            "a bundle's Info.plist and format");
    f.release(bundle_info);
    f.release(code);
    write(bundle / "Contents" / "Resources" / "note.txt", std::vector<std::uint8_t>{'x'});
    code = static_code(bundle);
    require(f.status("_SecStaticCodeCheckValidityWithErrors", {code, 0, 0, errors}) == codesign::cs::bad_resource, "an altered resource");
    const auto user_info = f.core("_CFErrorCopyUserInfo", {f.engine.read64(errors)});
    require(value(user_info, "SecCSResourceAltered") != 0, "kSecCFErrorResourceAltered names it");
    f.release(user_info);
    f.release(f.engine.read64(errors));
    require(validity(code, 4) == 0, "kSecCSDoNotValidateResources skips them");
    f.release(code);
    fs::remove(bundle / "Contents" / "Resources" / "note.txt");
    code = static_code(bundle);
    require(validity(code, 0) == codesign::cs::bad_resource, "a missing resource");
    f.release(code);
    write(bundle / "Contents" / "Info.plist", std::vector<std::uint8_t>(plist.begin(), plist.end() - 1));
    code = static_code(bundle);
    require(validity(code, 4) == codesign::cs::info_plist_failed, "a modified Info.plist");
    f.release(code);

    // 3. A Developer ID signed application (QLab in the Alchemy root, when present): the CMS signature, the chain to
    //    Apple Root CA, the sealed resources and nested code.
    auto qlab = alchemy::default_alchemy_root() / "Applications" / "QLab.app";
    for (auto dir = fs::current_path(); !fs::exists(qlab) && dir.has_relative_path(); dir = dir.parent_path())
        qlab = dir / "artifacts" / "alchemy-root" / "Applications" / "QLab.app";
    if (fs::exists(qlab)) {
        code = static_code(qlab);
        require(validity(code, 0) == 0, "QLab's signature verifies: " + std::to_string(validity(code, 0)));
        const auto signed_info = info_of(code, 2);
        require(f.cf.utf8(value(signed_info, "identifier")) == "com.figure53.QLab.5" && f.cf.utf8(value(signed_info, "teamid")) == "7672N4CCJM",
                "QLab's identifier and team");
        const auto certificates = value(signed_info, "certificates");
        require(certificates && f.cf.length(certificates) == 3, "the signing chain");
        const auto root = f.s("_SecCertificateCopyData", {f.cf.element(certificates, 2)});
        const auto root_sha = sec::digest("SHA256", sec::bridge()->bytes(root));
        require(root_sha && codesign::is_apple_root(*sec::Certificate::parse(sec::bridge()->bytes(root))), "the chain ends at Apple Root CA");
        require(value(signed_info, "signing-time") != 0, "the signing time");
        f.release(root);
        f.release(signed_info);
        const auto requirement_info = info_of(code, 4);
        require(f.cf.utf8(value(requirement_info, "requirements")).starts_with("designated => ") &&
                value(requirement_info, "designated-requirement") != 0 && value(requirement_info, "entitlements-dict") != 0,
                "QLab's requirements and entitlements");
        f.release(requirement_info);
        const auto qlab_designated = f.cell();
        require(f.status("_SecCodeCopyDesignatedRequirement", {code, 0, qlab_designated}) == 0 &&
                f.status("_SecStaticCodeCheckValidity", {code, 0, f.engine.read64(qlab_designated)}) == 0, "QLab satisfies its designated requirement");
        f.release(f.engine.read64(qlab_designated));
        require(satisfies(code, "anchor apple generic and certificate leaf[subject.OU] = \"7672N4CCJM\"") == 0, "the Developer ID team");
        require(satisfies(code, "anchor apple") == codesign::cs::requirement_failed, "QLab is not Apple's own code");
        require(satisfies(code, "certificate leaf[subject.OU] = OTHERTEAM") == codesign::cs::requirement_failed, "another team");
        require(validity(code, 8) == 0, "nested code verifies deeply against its recorded requirements");
        f.release(code);
        std::cout << "code signing: QLab verified (CMS, Apple Root CA, resources, nested code)\n";
    } else {
        std::cout << "code signing: no QLab in the Alchemy root, the Developer ID check skipped\n";
    }

    // 4. Running code and guests.
    const auto self = f.cell();
    require(f.status("_SecCodeCopySelf", {0, self}) == 0 && f.engine.read64(self), "SecCodeCopySelf");
    require(f.status("_SecCodeCheckValidity", {f.engine.read64(self), 0, 0}) == codesign::cs::unimplemented, "running code is not validated");
    f.release(f.engine.read64(self));
    const auto attributes = [&](std::string_view key, Address object) {
        const auto name = f.cf.string(key);
        const auto dictionary = sec::bridge()->dictionary({{name, object}});
        f.release(name);
        return dictionary;
    };
    const auto guest = f.cell();
    const auto missing_pid = attributes("pid", sec::bridge()->number(999999));
    require(f.status("_SecCodeCopyGuestWithAttributes", {0, missing_pid, 0, guest}) == codesign::cs::no_such_code, "no such guest");
    const auto odd = attributes("color", sec::bridge()->number(1));
    require(f.status("_SecCodeCopyGuestWithAttributes", {0, odd, 0, guest}) == codesign::cs::unsupported_guest_attributes, "unsupported attributes");
    const auto dynamic = attributes("dynamicCode", sec::bridge()->data(std::vector<std::uint8_t>{1, 2, 3}));
    require(f.status("_SecCodeCopyGuestWithAttributes", {0, dynamic, 0, guest}) == codesign::cs::unimplemented,
            "dynamic code is not provided (errSecCSUnimplemented)");
    f.release(dynamic);
    f.release(missing_pid);
    f.release(odd);
    fs::remove_all(temp);
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
        test_legacy_keys_and_export(fixture, identity);
        test_keychain(fixture);
        test_keychain_keys(fixture);
        test_legacy_keychain(fixture);
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
