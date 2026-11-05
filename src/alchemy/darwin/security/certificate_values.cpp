#include "alchemy/darwin/security/security_internal.hpp"

#include "alchemy/darwin/security/keys.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

// SecCertificateCopyValues (macOS): the certificate's fields as a dictionary
// keyed by the kSecOID* strings, each value a property dictionary
// (kSecPropertyKeyType / Label / LocalizedLabel / Value) as Apple's
// SecCertificateCopyValues documentation describes. Covers the version,
// serial number, signature and public-key algorithms, issuer and subject
// names (sections of attribute properties labelled by OID), validity (numbers
// of CFAbsoluteTime), the public key and signature (data), the subject's
// common names and email addresses, and every extension (Critical plus the
// raw value, with subjectAltName, basicConstraints and extendedKeyUsage
// decoded). The constant values are the object identifiers published in
// Apple's open-source Security release (APSL 2.0).
namespace alchemy::gui::security {
namespace {

constexpr std::string_view lib = security_library;

struct OidName { const char* symbol; const char* oid; };
constexpr OidName oid_constants[] = {
    {"kSecOIDADC_CERT_POLICY", "1.2.840.113635.100.5.3"},
    {"kSecOIDAPPLE_CERT_POLICY", "1.2.840.113635.100.5.1"},
    {"kSecOIDAPPLE_EKU_CODE_SIGNING", "1.2.840.113635.100.4.1"},
    {"kSecOIDAPPLE_EKU_CODE_SIGNING_DEV", "1.2.840.113635.100.4.1.1"},
    {"kSecOIDAPPLE_EKU_ICHAT_ENCRYPTION", "1.2.840.113635.100.4.3"},
    {"kSecOIDAPPLE_EKU_ICHAT_SIGNING", "1.2.840.113635.100.4.2"},
    {"kSecOIDAPPLE_EKU_RESOURCE_SIGNING", "1.2.840.113635.100.4.1.4"},
    {"kSecOIDAPPLE_EKU_SYSTEM_IDENTITY", "1.2.840.113635.100.4.4"},
    {"kSecOIDAPPLE_EXTENSION", "1.2.840.113635.100.6"},
    {"kSecOIDAPPLE_EXTENSION_AAI_INTERMEDIATE", "1.2.840.113635.100.6.2.3"},
    {"kSecOIDAPPLE_EXTENSION_ADC_APPLE_SIGNING", "1.2.840.113635.100.6.1.2.0.0"},
    {"kSecOIDAPPLE_EXTENSION_ADC_DEV_SIGNING", "1.2.840.113635.100.6.1.2.0"},
    {"kSecOIDAPPLE_EXTENSION_APPLEID_INTERMEDIATE", "1.2.840.113635.100.6.2.7"},
    {"kSecOIDAPPLE_EXTENSION_APPLE_SIGNING", "1.2.840.113635.100.6.1.1"},
    {"kSecOIDAPPLE_EXTENSION_CODE_SIGNING", "1.2.840.113635.100.6.1"},
    {"kSecOIDAPPLE_EXTENSION_INTERMEDIATE_MARKER", "1.2.840.113635.100.6.2"},
    {"kSecOIDAPPLE_EXTENSION_ITMS_INTERMEDIATE", "1.2.840.113635.100.6.2.2"},
    {"kSecOIDAPPLE_EXTENSION_WWDR_INTERMEDIATE", "1.2.840.113635.100.6.2.1"},
    {"kSecOIDAuthorityInfoAccess", "1.3.6.1.5.5.7.1.1"},
    {"kSecOIDAuthorityKeyIdentifier", "2.5.29.35"},
    {"kSecOIDBasicConstraints", "2.5.29.19"},
    {"kSecOIDBiometricInfo", "1.3.6.1.5.5.7.1.2"},
    {"kSecOIDCSSMKeyStruct", "2.16.840.1.113741.2.1.1.1.20"},
    {"kSecOIDCertIssuer", "2.5.29.29"},
    {"kSecOIDCertificatePolicies", "2.5.29.32"},
    {"kSecOIDClientAuth", "1.3.6.1.5.5.7.3.2"},
    {"kSecOIDCollectiveStateProvinceName", "2.5.4.8.1"},
    {"kSecOIDCollectiveStreetAddress", "2.5.4.9.1"},
    {"kSecOIDCommonName", "2.5.4.3"},
    {"kSecOIDCountryName", "2.5.4.6"},
    {"kSecOIDCrlDistributionPoints", "2.5.29.31"},
    {"kSecOIDCrlNumber", "2.5.29.20"},
    {"kSecOIDCrlReason", "2.5.29.21"},
    {"kSecOIDDOTMAC_CERT_EMAIL_ENCRYPT", "1.2.840.113635.100.3.2.3"},
    {"kSecOIDDOTMAC_CERT_EMAIL_SIGN", "1.2.840.113635.100.3.2.2"},
    {"kSecOIDDOTMAC_CERT_EXTENSION", "1.2.840.113635.100.3.2"},
    {"kSecOIDDOTMAC_CERT_IDENTITY", "1.2.840.113635.100.3.2.1"},
    {"kSecOIDDOTMAC_CERT_POLICY", "1.2.840.113635.100.5.2"},
    {"kSecOIDDeltaCrlIndicator", "2.5.29.27"},
    {"kSecOIDDescription", "2.5.4.13"},
    {"kSecOIDEKU_IPSec", "1.3.6.1.5.5.8.2.2"},
    {"kSecOIDEmailAddress", "1.2.840.113549.1.9.1"},
    {"kSecOIDEmailProtection", "1.3.6.1.5.5.7.3.4"},
    {"kSecOIDExtendedKeyUsage", "2.5.29.37"},
    {"kSecOIDExtendedKeyUsageAny", "2.5.29.37.0"},
    {"kSecOIDExtendedUseCodeSigning", "1.3.6.1.5.5.7.3.3"},
    {"kSecOIDGivenName", "2.5.4.42"},
    {"kSecOIDHoldInstructionCode", "2.5.29.23"},
    {"kSecOIDInvalidityDate", "2.5.29.24"},
    {"kSecOIDIssuerAltName", "2.5.29.18"},
    {"kSecOIDIssuingDistributionPoint", "2.5.29.28"},
    {"kSecOIDIssuingDistributionPoints", "2.5.29.28"},
    {"kSecOIDKERBv5_PKINIT_KP_CLIENT_AUTH", "1.3.6.1.5.2.3.4"},
    {"kSecOIDKERBv5_PKINIT_KP_KDC", "1.3.6.1.5.2.3.5"},
    {"kSecOIDKeyUsage", "2.5.29.15"},
    {"kSecOIDLocalityName", "2.5.4.7"},
    {"kSecOIDMS_NTPrincipalName", "1.3.6.1.4.1.311.20.2.3"},
    {"kSecOIDMicrosoftSGC", "1.3.6.1.4.1.311.10.3.3"},
    {"kSecOIDNameConstraints", "2.5.29.30"},
    {"kSecOIDNetscapeCertSequence", "2.16.840.1.113730.2.5"},
    {"kSecOIDNetscapeCertType", "2.16.840.1.113730.1.1"},
    {"kSecOIDNetscapeSGC", "2.16.840.1.113730.4.1"},
    {"kSecOIDOCSPSigning", "1.3.6.1.5.5.7.3.9"},
    {"kSecOIDOrganizationName", "2.5.4.10"},
    {"kSecOIDOrganizationalUnitName", "2.5.4.11"},
    {"kSecOIDPolicyConstraints", "2.5.29.36"},
    {"kSecOIDPolicyMappings", "2.5.29.33"},
    {"kSecOIDPrivateKeyUsagePeriod", "2.5.29.16"},
    {"kSecOIDQC_Statements", "1.3.6.1.5.5.7.1.3"},
    {"kSecOIDSRVName", "1.3.6.1.5.5.7.8.7"},
    {"kSecOIDSerialNumber", "2.5.4.5"},
    {"kSecOIDServerAuth", "1.3.6.1.5.5.7.3.1"},
    {"kSecOIDStateProvinceName", "2.5.4.8"},
    {"kSecOIDStreetAddress", "2.5.4.9"},
    {"kSecOIDSubjectAltName", "2.5.29.17"},
    {"kSecOIDSubjectDirectoryAttributes", "2.5.29.9"},
    {"kSecOIDSubjectEmailAddress", "2.16.840.1.113741.2.1.1.1.50.3"},
    {"kSecOIDSubjectInfoAccess", "1.3.6.1.5.5.7.1.11"},
    {"kSecOIDSubjectKeyIdentifier", "2.5.29.14"},
    {"kSecOIDSubjectPicture", "2.16.840.1.113741.2.1.1.1.50.2"},
    {"kSecOIDSubjectSignatureBitmap", "2.16.840.1.113741.2.1.1.1.50.1"},
    {"kSecOIDSurname", "2.5.4.4"},
    {"kSecOIDTimeStamping", "1.3.6.1.5.5.7.3.8"},
    {"kSecOIDTitle", "2.5.4.12"},
    {"kSecOIDUseExemptions", "2.16.840.1.113741.2.1.1.1.50.4"},
    {"kSecOIDX509V1CertificateIssuerUniqueId", "2.16.840.1.113741.2.1.1.1.11"},
    {"kSecOIDX509V1CertificateSubjectUniqueId", "2.16.840.1.113741.2.1.1.1.12"},
    {"kSecOIDX509V1IssuerName", "2.16.840.1.113741.2.1.1.1.5"},
    {"kSecOIDX509V1IssuerNameCStruct", "2.16.840.1.113741.2.1.1.1.5.1"},
    {"kSecOIDX509V1IssuerNameLDAP", "2.16.840.1.113741.2.1.1.1.5.2"},
    {"kSecOIDX509V1IssuerNameStd", "2.16.840.1.113741.2.1.1.1.23"},
    {"kSecOIDX509V1SerialNumber", "2.16.840.1.113741.2.1.1.1.3"},
    {"kSecOIDX509V1Signature", "2.16.840.1.113741.2.1.3.2.2"},
    {"kSecOIDX509V1SignatureAlgorithm", "2.16.840.1.113741.2.1.3.2.1"},
    {"kSecOIDX509V1SignatureAlgorithmParameters", "2.16.840.1.113741.2.1.3.2.3"},
    {"kSecOIDX509V1SignatureAlgorithmTBS", "2.16.840.1.113741.2.1.3.2.10"},
    {"kSecOIDX509V1SignatureCStruct", "2.16.840.1.113741.2.1.3.2.0.1"},
    {"kSecOIDX509V1SignatureStruct", "2.16.840.1.113741.2.1.3.2.0"},
    {"kSecOIDX509V1SubjectName", "2.16.840.1.113741.2.1.1.1.8"},
    {"kSecOIDX509V1SubjectNameCStruct", "2.16.840.1.113741.2.1.1.1.8.1"},
    {"kSecOIDX509V1SubjectNameLDAP", "2.16.840.1.113741.2.1.1.1.8.2"},
    {"kSecOIDX509V1SubjectNameStd", "2.16.840.1.113741.2.1.1.1.22"},
    {"kSecOIDX509V1SubjectPublicKey", "2.16.840.1.113741.2.1.1.1.10"},
    {"kSecOIDX509V1SubjectPublicKeyAlgorithm", "2.16.840.1.113741.2.1.1.1.9"},
    {"kSecOIDX509V1SubjectPublicKeyAlgorithmParameters", "2.16.840.1.113741.2.1.1.1.18"},
    {"kSecOIDX509V1SubjectPublicKeyCStruct", "2.16.840.1.113741.2.1.1.1.20.1"},
    {"kSecOIDX509V1ValidityNotAfter", "2.16.840.1.113741.2.1.1.1.7"},
    {"kSecOIDX509V1ValidityNotBefore", "2.16.840.1.113741.2.1.1.1.6"},
    {"kSecOIDX509V1Version", "2.16.840.1.113741.2.1.1.1.2"},
    {"kSecOIDX509V3Certificate", "2.16.840.1.113741.2.1.1.1.1"},
    {"kSecOIDX509V3CertificateCStruct", "2.16.840.1.113741.2.1.1.1.1.1"},
    {"kSecOIDX509V3CertificateExtensionCStruct", "2.16.840.1.113741.2.1.1.1.13.1"},
    {"kSecOIDX509V3CertificateExtensionCritical", "2.16.840.1.113741.2.1.1.1.16"},
    {"kSecOIDX509V3CertificateExtensionId", "2.16.840.1.113741.2.1.1.1.15"},
    {"kSecOIDX509V3CertificateExtensionStruct", "2.16.840.1.113741.2.1.1.1.13"},
    {"kSecOIDX509V3CertificateExtensionType", "2.16.840.1.113741.2.1.1.1.19"},
    {"kSecOIDX509V3CertificateExtensionValue", "2.16.840.1.113741.2.1.1.1.17"},
    {"kSecOIDX509V3CertificateExtensionsCStruct", "2.16.840.1.113741.2.1.1.1.21.1"},
    {"kSecOIDX509V3CertificateExtensionsStruct", "2.16.840.1.113741.2.1.1.1.21"},
    {"kSecOIDX509V3CertificateNumberOfExtensions", "2.16.840.1.113741.2.1.1.1.14"},
    {"kSecOIDX509V3SignedCertificate", "2.16.840.1.113741.2.1.1.1.0"},
    {"kSecOIDX509V3SignedCertificateCStruct", "2.16.840.1.113741.2.1.1.1.0.1"},
};

// Display names of the name attributes and extensions this file labels.
const char* localized_name(std::string_view oid) {
    static constexpr std::pair<std::string_view, const char*> names[] = {
        {"2.5.4.3", "Common Name"}, {"2.5.4.4", "Surname"}, {"2.5.4.5", "Serial Number"}, {"2.5.4.6", "Country"},
        {"2.5.4.7", "Locality"}, {"2.5.4.8", "State/Province"}, {"2.5.4.9", "Street Address"}, {"2.5.4.10", "Organization"},
        {"2.5.4.11", "Organizational Unit"}, {"2.5.4.12", "Title"}, {"2.5.4.42", "Given Name"},
        {"1.2.840.113549.1.9.1", "Email Address"}, {"0.9.2342.19200300.100.1.25", "Domain Component"},
        {"0.9.2342.19200300.100.1.1", "User ID"},
        {"2.5.29.14", "Subject Key Identifier"}, {"2.5.29.15", "Key Usage"}, {"2.5.29.17", "Subject Alternative Name"},
        {"2.5.29.18", "Issuer Alternative Name"}, {"2.5.29.19", "Basic Constraints"}, {"2.5.29.31", "CRL Distribution Points"},
        {"2.5.29.32", "Certificate Policies"}, {"2.5.29.35", "Authority Key Identifier"}, {"2.5.29.37", "Extended Key Usage"},
        {"1.3.6.1.5.5.7.1.1", "Authority Information Access"}, {"1.3.6.1.5.5.7.3.1", "Server Authentication"},
        {"1.3.6.1.5.5.7.3.2", "Client Authentication"}, {"1.3.6.1.5.5.7.3.3", "Code Signing"},
        {"1.3.6.1.5.5.7.3.4", "Email Protection"}, {"1.3.6.1.5.5.7.3.8", "Time Stamping"}, {"1.3.6.1.5.5.7.3.9", "OCSP Signing"},
        {"1.2.840.113549.1.1.1", "RSA Encryption"}, {"1.2.840.113549.1.1.5", "SHA-1 with RSA Encryption"},
        {"1.2.840.113549.1.1.11", "SHA-256 with RSA Encryption"}, {"1.2.840.113549.1.1.12", "SHA-384 with RSA Encryption"},
        {"1.2.840.113549.1.1.13", "SHA-512 with RSA Encryption"}, {"1.2.840.10045.2.1", "Elliptic Curve Public Key"},
        {"1.2.840.10045.4.3.2", "ECDSA Signature with SHA-256"}, {"1.2.840.10045.4.3.3", "ECDSA Signature with SHA-384"},
        {"1.2.840.10045.4.3.4", "ECDSA Signature with SHA-512"}};
    for (const auto& [key, name] : names) if (key == oid) return name;
    return nullptr;
}

// The text of a DirectoryString / IA5String / ... value.
std::string string_value(const der::Node& node) {
    if (node.tag == 0x1e) {   // BMPString: UTF-16BE
        std::string text;
        for (std::size_t at = 0; at + 1 < node.value.size(); at += 2) {
            const unsigned unit = (node.value[at] << 8) | node.value[at + 1];
            if (unit < 0x80) text += static_cast<char>(unit);
            else if (unit < 0x800) { text += static_cast<char>(0xc0 | (unit >> 6)); text += static_cast<char>(0x80 | (unit & 0x3f)); }
            else {
                text += static_cast<char>(0xe0 | (unit >> 12));
                text += static_cast<char>(0x80 | ((unit >> 6) & 0x3f));
                text += static_cast<char>(0x80 | (unit & 0x3f));
            }
        }
        return text;
    }
    return std::string(node.value.begin(), node.value.end());
}

std::string spaced_hex(std::span<const std::uint8_t> bytes) {
    std::string text;
    char digits[4];
    for (const auto byte : bytes) {
        std::snprintf(digits, sizeof digits, "%02X", byte);
        if (!text.empty()) text += ' ';
        text += digits;
    }
    return text;
}

// One property dictionary under construction (host values, converted at the end).
struct Property {
    enum class Kind { string, data, number, section, array } kind{Kind::string};
    std::string label, localized;
    std::string text;
    Bytes bytes;
    double number{};
    std::vector<Property> children;        // section
    std::vector<std::string> strings;      // array
};

Property string_property(std::string label, std::string text, std::string localized = {}) {
    Property p;
    p.label = std::move(label);
    p.localized = localized.empty() ? p.label : std::move(localized);
    p.text = std::move(text);
    return p;
}
Property data_property(std::string label, std::span<const std::uint8_t> bytes) {
    Property p;
    p.kind = Property::Kind::data;
    p.label = p.localized = std::move(label);
    p.bytes.assign(bytes.begin(), bytes.end());
    return p;
}
Property section(std::string label, std::vector<Property> children) {
    Property p;
    p.kind = Property::Kind::section;
    p.label = p.localized = std::move(label);
    p.children = std::move(children);
    return p;
}

// A Name as attribute properties labelled by OID ("2.5.4.3", localized "Common Name").
std::vector<Property> name_properties(std::span<const std::uint8_t> name) {
    std::vector<Property> out;
    const auto outer = der::read(name);
    const auto sets = outer ? der::children(*outer) : std::nullopt;
    if (!sets) return out;
    for (const auto& set : *sets) {
        const auto attributes = der::children(set);
        if (!attributes) continue;
        for (const auto& attribute : *attributes) {
            const auto parts = der::children(attribute);
            if (!parts || parts->size() != 2 || (*parts)[0].tag != 0x06) continue;
            const auto oid = der::oid((*parts)[0].value);
            const auto* localized = localized_name(oid);
            out.push_back(string_property(oid, string_value((*parts)[1]), localized ? localized : oid));
        }
    }
    return out;
}

// AlgorithmIdentifier as "Algorithm" (+ "Parameters").
std::vector<Property> algorithm_properties(const der::Node& identifier) {
    std::vector<Property> out;
    const auto parts = der::children(identifier);
    if (!parts || parts->empty() || (*parts)[0].tag != 0x06) return out;
    out.push_back(string_property("Algorithm", der::oid((*parts)[0].value)));
    if (parts->size() > 1 && (*parts)[1].tag != 0x05) {
        if ((*parts)[1].tag == 0x06) out.push_back(string_property("Parameters", der::oid((*parts)[1].value)));
        else out.push_back(data_property("Parameters", (*parts)[1].whole));
    }
    return out;
}

// One extension: Critical, then the decoded values where known, then the raw value.
Property extension_property(std::string_view oid, bool critical, std::span<const std::uint8_t> value) {
    std::vector<Property> children{string_property("Critical", critical ? "Yes" : "No")};
    const auto inner = der::read(value);
    const auto items = inner ? der::children(*inner) : std::nullopt;
    if (oid == "2.5.29.17" || oid == "2.5.29.18") {   // GeneralNames
        if (items) for (const auto& item : *items) {
            const std::string text(item.value.begin(), item.value.end());
            if (item.tag == 0x82) children.push_back(string_property("DNS Name", text));
            else if (item.tag == 0x81) children.push_back(string_property("Email Address", text));
            else if (item.tag == 0x86) children.push_back(string_property("URI", text));
            else if (item.tag == 0x87) children.push_back(data_property("IP Address", item.value));
        }
    } else if (oid == "2.5.29.19") {                  // BasicConstraints
        bool ca = false;
        std::optional<std::uint64_t> path;
        if (items) for (const auto& item : *items) {
            if (item.tag == 0x01) ca = !item.value.empty() && item.value[0];
            if (item.tag == 0x02) { std::uint64_t n = 0; for (const auto byte : item.value) n = (n << 8) | byte; path = n; }
        }
        children.push_back(string_property("Certificate Authority", ca ? "Yes" : "No"));
        if (path) children.push_back(string_property("Path Length Constraint", std::to_string(*path)));
    } else if (oid == "2.5.29.37") {                  // ExtKeyUsageSyntax
        if (items) for (const auto& item : *items)
            if (item.tag == 0x06) {
                const auto purpose = der::oid(item.value);
                const auto* name = localized_name(purpose);
                children.push_back(string_property("Purpose", purpose, name ? std::string("Purpose: ") + name : std::string()));
            }
    }
    children.push_back(data_property("Data", value));
    const auto* name = localized_name(oid);
    auto result = section(std::string(oid), std::move(children));
    if (name) result.localized = name;
    return result;
}

struct Values {
    Security& sec;
    Bridge& b;

    Address convert(const Property& p) {
        Address value{};
        const char* type = "kSecPropertyTypeString";
        switch (p.kind) {
        case Property::Kind::string: value = b.string(p.text); break;
        case Property::Kind::data: value = b.data(p.bytes); type = "kSecPropertyTypeData"; break;
        case Property::Kind::number: value = b.cf.real(p.number); type = "kSecPropertyTypeNumber"; break;
        case Property::Kind::section:
        case Property::Kind::array: {
            std::vector<Address> items;
            if (p.kind == Property::Kind::section) for (const auto& child : p.children) items.push_back(convert(child));
            else for (const auto& text : p.strings) items.push_back(b.string(text));
            value = b.array(items);
            b.release_all(items);
            type = p.kind == Property::Kind::section ? "kSecPropertyTypeSection" : "kSecPropertyTypeArray";
            break;
        }
        }
        const auto label = b.string(p.label), localized = b.string(p.localized);
        const auto result = b.dictionary({{sec.key("kSecPropertyKeyType"), sec.key(type)}, {sec.key("kSecPropertyKeyLabel"), label},
                                          {sec.key("kSecPropertyKeyLocalizedLabel"), localized}, {sec.key("kSecPropertyKeyValue"), value}});
        b.release_all({label, localized, value});
        return result;
    }

    // (kSecOID constant name or a dotted extension OID, property) pairs of `certificate`.
    std::vector<std::pair<std::string, Property>> fields(const Certificate& certificate) {
        std::vector<std::pair<std::string, Property>> out;
        const auto root = der::read(certificate.der());
        const auto top = root ? der::children(*root) : std::nullopt;
        if (!top || top->size() != 3) return out;
        const auto tbs = der::children((*top)[0]);
        if (!tbs) return out;
        std::size_t at = 0;
        unsigned version = 1;
        if (at < tbs->size() && (*tbs)[at].tag == 0xa0) {
            if (const auto inner = der::read((*tbs)[at].value); inner && inner->tag == 0x02 && inner->value.size() == 1)
                version = inner->value[0] + 1u;
            ++at;
        }
        out.emplace_back("kSecOIDX509V1Version", string_property("Version", std::to_string(version)));
        if (at < tbs->size()) out.emplace_back("kSecOIDX509V1SerialNumber", string_property("Serial Number", spaced_hex((*tbs)[at++].value)));
        if (at < tbs->size()) out.emplace_back("kSecOIDX509V1SignatureAlgorithm", section("Signature Algorithm", algorithm_properties((*tbs)[at++])));
        if (at < tbs->size()) out.emplace_back("kSecOIDX509V1IssuerName", section("Issuer Name", name_properties((*tbs)[at++].whole)));
        ++at;   // validity: the host's parsed times
        Property before, after;
        before.kind = after.kind = Property::Kind::number;
        before.label = before.localized = "Not Valid Before";
        after.label = after.localized = "Not Valid After";
        before.number = certificate.not_before();
        after.number = certificate.not_after();
        out.emplace_back("kSecOIDX509V1ValidityNotBefore", before);
        out.emplace_back("kSecOIDX509V1ValidityNotAfter", after);
        if (at < tbs->size()) out.emplace_back("kSecOIDX509V1SubjectName", section("Subject Name", name_properties((*tbs)[at++].whole)));
        if (at < tbs->size()) {
            if (const auto spki = der::children((*tbs)[at++]); spki && spki->size() == 2) {
                out.emplace_back("kSecOIDX509V1SubjectPublicKeyAlgorithm", section("Public Key Algorithm", algorithm_properties((*spki)[0])));
                const auto bits = (*spki)[1].value;
                out.emplace_back("kSecOIDX509V1SubjectPublicKey", data_property("Public Key Data", bits.empty() ? bits : bits.subspan(1)));
            }
        }
        for (; at < tbs->size(); ++at) {
            const auto& node = (*tbs)[at];
            if (node.tag == 0xa1) out.emplace_back("kSecOIDX509V1CertificateIssuerUniqueId", data_property("Issuer Unique ID", node.value));
            if (node.tag == 0xa2) out.emplace_back("kSecOIDX509V1CertificateSubjectUniqueId", data_property("Subject Unique ID", node.value));
            if (node.tag != 0xa3) continue;
            const auto list = der::read(node.value);
            const auto extensions = list ? der::children(*list) : std::nullopt;
            if (!extensions) continue;
            for (const auto& extension : *extensions) {
                const auto parts = der::children(extension);
                if (!parts || parts->size() < 2 || (*parts)[0].tag != 0x06) continue;
                const bool critical = parts->size() == 3 && (*parts)[1].tag == 0x01 && !(*parts)[1].value.empty() && (*parts)[1].value[0];
                const auto& value = (*parts)[parts->size() - 1];
                if (value.tag != 0x04) continue;
                const auto oid = der::oid((*parts)[0].value);
                out.emplace_back(oid, extension_property(oid, critical, value.value));
            }
        }
        const auto bits = (*top)[2].value;
        out.emplace_back("kSecOIDX509V1Signature", data_property("Signature", bits.empty() ? bits : bits.subspan(1)));
        const auto names = [](std::string label, std::vector<std::string> values) {
            Property p;
            p.kind = Property::Kind::array;
            p.label = p.localized = std::move(label);
            p.strings = std::move(values);
            return p;
        };
        std::vector<std::string> common_names;
        for (const auto& attribute : name_properties(certificate.subject_sequence()))
            if (attribute.label == "2.5.4.3") common_names.push_back(attribute.text);
        if (!common_names.empty()) out.emplace_back("kSecOIDCommonName", names("CommonName", std::move(common_names)));
        if (auto emails = certificate.email_addresses(); !emails.empty()) out.emplace_back("kSecOIDEmailAddress", names("Email Addresses", std::move(emails)));
        return out;
    }
};

} // namespace

void install_certificate_values(Security& sec) {
    auto& b = *sec.b;
    for (const auto& constant : oid_constants) sec.export_constant(constant.symbol, constant.oid);
    const std::pair<const char*, const char*> property_constants[] = {
        {"kSecPropertyKeyType", "type"}, {"kSecPropertyKeyLabel", "label"}, {"kSecPropertyKeyLocalizedLabel", "localized label"},
        {"kSecPropertyKeyValue", "value"}, {"kSecPropertyTypeWarning", "warning"}, {"kSecPropertyTypeSuccess", "success"},
        {"kSecPropertyTypeSection", "section"}, {"kSecPropertyTypeData", "data"}, {"kSecPropertyTypeString", "string"},
        {"kSecPropertyTypeURL", "url"}, {"kSecPropertyTypeDate", "date"}, {"kSecPropertyTypeArray", "array"},
        {"kSecPropertyTypeNumber", "number"}};
    for (const auto& [name, value] : property_constants) sec.export_constant(name, value);
    auto self = std::make_shared<Values>(Values{sec, b});

    // CFDictionaryRef SecCertificateCopyValues(SecCertificateRef, CFArrayRef keys, CFErrorRef *error): keys, when
    // given, are the OID strings to keep.
    b.function(lib, "_SecCertificateCopyValues", [self](CpuState& s, GuestMemory&) {
        auto& sec = self->sec;
        auto& b = self->b;
        const auto certificate = sec.certificate_of(s.x[0]);
        if (!certificate) { sec.set_error(s.x[2], status::param, "not a SecCertificateRef"); s.x[0] = 0; return; }
        std::optional<std::vector<std::string>> wanted;
        if (s.x[1]) {
            if (!b.is_array(s.x[1])) { sec.set_error(s.x[2], status::param, "keys is not a CFArray"); s.x[0] = 0; return; }
            wanted.emplace();
            for (const auto key : b.cf.array_values(s.x[1])) {
                const auto text = b.maybe_text(key);
                if (!text) { sec.set_error(s.x[2], status::param, "a key is not a CFString"); s.x[0] = 0; return; }
                wanted->push_back(*text);
            }
        }
        std::vector<std::pair<Address, Address>> entries;
        std::vector<Address> owned;
        for (const auto& [name, property] : self->fields(*certificate)) {
            const bool constant = name.starts_with("kSecOID");
            const std::string oid = constant ? b.text(sec.key(name)) : name;
            if (wanted && std::find(wanted->begin(), wanted->end(), oid) == wanted->end()) continue;
            if (std::any_of(entries.begin(), entries.end(), [&](const auto& entry) { return b.text(entry.first) == oid; })) continue;
            const auto key = constant ? sec.key(name) : b.string(oid);
            if (!constant) owned.push_back(key);
            const auto value = self->convert(property);
            owned.push_back(value);
            entries.emplace_back(key, value);
        }
        s.x[0] = b.dictionary(entries);
        b.release_all(owned);
    });
}

} // namespace alchemy::gui::security
