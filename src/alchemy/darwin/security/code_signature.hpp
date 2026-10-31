#pragma once

// Embedded code signatures of Mach-O files and bundles on disk, host only (no
// guest calls). The layout is the public one: LC_CODE_SIGNATURE points at a
// SuperBlob of CodeDirectories (SHA-1, SHA-256, SHA-384 page hashes and
// special slots), requirements, entitlements (XML and DER) and a CMS
// SignedData blob (BER) over the CodeDirectory. Sealed resources are the
// bundle's _CodeSignature/CodeResources ("files2"). Used by SecStaticCode /
// SecCode (code_signing.cpp).

#include "alchemy/darwin/security/x509.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace alchemy::gui::security::codesign {

// errSecCS* (CSCommon.h).
namespace cs {
inline constexpr std::int32_t unimplemented = -67072, invalid_object = -67071, invalid_flags = -67070, object_required = -67069,
    static_code_not_found = -67068, unsupported_guest_attributes = -67067, invalid_attribute_values = -67066, no_such_code = -67065,
    guest_invalid = -67063, unsigned_code = -67062, signature_failed = -67061, signature_not_verifiable = -67060,
    signature_unsupported = -67059, resources_not_sealed = -67057, resources_not_found = -67056, resources_invalid = -67055,
    bad_resource = -67054, requirement_invalid = -67052, requirement_unsupported = -67051, requirement_failed = -67050,
    bad_object_format = -67049, internal_error = -67048, info_plist_failed = -67030, no_main_executable = -67029,
    bad_bundle_format = -67028, bad_nested_code = -67021, unsupported_digest = -67000;
} // namespace cs

// CodeDirectory flags (cs_blobs.h): CS_ADHOC, CS_LINKER_SIGNED.
inline constexpr std::uint32_t flag_adhoc = 0x2, flag_linker_signed = 0x20000;
// Hash types.
inline constexpr std::uint8_t hash_sha1 = 1, hash_sha256 = 2, hash_sha256_truncated = 3, hash_sha384 = 4;

struct CodeDirectory {
    std::uint32_t version{}, flags{};
    std::string identifier, team;
    std::uint8_t hash_type{}, hash_size{}, platform{}, page_log2{};
    std::uint64_t code_limit{};
    std::vector<Bytes> special;   // special[n - 1]: the hash of special slot n
    std::vector<Bytes> code;      // one hash per page
    std::uint64_t exec_segment_flags{};
    Bytes blob;                   // the CodeDirectory as embedded
    [[nodiscard]] Bytes digest() const;   // the full hash of the blob (its hash type)
    [[nodiscard]] Bytes cdhash() const;   // the first 20 bytes of digest()
    // The hash recorded for special slot `slot`, nullopt when absent (all zero).
    [[nodiscard]] std::optional<Bytes> special_slot(unsigned slot) const;
};

struct Signature {
    std::vector<CodeDirectory> directories;   // the primary (slot 0) first, then the alternates
    Bytes requirements, entitlements, der_entitlements, cms;
    // The strongest CodeDirectory (the one Apple's validation uses).
    [[nodiscard]] const CodeDirectory& best() const;
};

struct Slice {
    std::uint32_t cpu_type{}, cpu_subtype{};
    std::uint64_t offset{}, size{};
    std::optional<Signature> signature;
};

struct StaticCode {
    std::filesystem::path root;                         // the bundle directory, or the file
    std::filesystem::path executable;                   // the main executable
    std::optional<std::filesystem::path> info_plist;    // bundles: Info.plist
    std::optional<std::filesystem::path> resources;     // bundles: _CodeSignature/CodeResources (when present)
    std::filesystem::path contents;                     // where CodeResources paths start (Contents/ or the bundle)
    bool bundle{};
    std::vector<Slice> slices;
    Bytes file;                                         // the main executable's bytes
    [[nodiscard]] const Slice& preferred() const;       // arm64 when present, else the first
    [[nodiscard]] bool is_signed() const;               // the preferred slice has a signature
};

// Opens a bundle directory or a Mach-O file. nullopt with `status` (errSecCS*) when it is not code.
[[nodiscard]] std::optional<StaticCode> open(const std::filesystem::path& path, std::int32_t& status);

struct CmsResult {
    std::vector<CertificatePtr> certificates;   // the signer first, then up its chain as far as the blob carries it
    std::optional<double> signing_time;          // CFAbsoluteTime
    std::int32_t status{};                       // 0 when the CMS signature verifies over the CodeDirectory
    std::string detail;
};
[[nodiscard]] CmsResult check_cms(const Signature& signature);

// Apple Root CA (by the SHA-256 of the certificate, as Apple publishes it).
[[nodiscard]] bool is_apple_root(const Certificate& certificate);

struct Validation {
    std::int32_t status{};
    std::string detail;
    std::string resource;    // a sealed resource that is missing or altered
    bool missing{};          // ... missing (kSecCFErrorResourceMissing), else altered
};
// SecCSFlags for validation: kSecCSCheckAllArchitectures (1), kSecCSDoNotValidateExecutable (2),
// kSecCSDoNotValidateResources (4), kSecCSCheckNestedCode (8).
inline constexpr std::uint32_t check_all_architectures = 1, skip_executable = 2, skip_resources = 4, check_nested = 8;
[[nodiscard]] Validation validate(const StaticCode& code, std::uint32_t flags);

// "Mach-O thin (arm64)", "Mach-O universal (x86_64 arm64)", prefixed "app bundle with " / "bundle with ".
[[nodiscard]] std::string format_of(const StaticCode& code);

} // namespace alchemy::gui::security::codesign
