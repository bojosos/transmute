#include "alchemy/darwin/security/code_signature.hpp"
#include "alchemy/darwin/security/ber.hpp"
#include "alchemy/darwin/security/code_requirements.hpp"

#include "alchemy/darwin/corefoundation/cf_plist.hpp"
#include "alchemy/darwin/security/keys.hpp"
#include "alchemy/path.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <stdexcept>
#include <fstream>
#include <span>

namespace alchemy::gui::security::codesign {
namespace {

using export_fill::Plist;
namespace fs = std::filesystem;

constexpr std::uint32_t cpu_arm64 = 0x0100000c, cpu_x86_64 = 0x01000007;
constexpr std::uint32_t magic_superblob = 0xfade0cc0, magic_directory = 0xfade0c02, magic_requirements = 0xfade0c01,
    magic_entitlements = 0xfade7171, magic_der_entitlements = 0xfade7172, magic_cms = 0xfade0b01;
constexpr double unix_to_reference = 978307200.0;

std::uint32_t be32(std::span<const std::uint8_t> bytes, std::size_t at) {
    if (at + 4 > bytes.size()) throw std::out_of_range("code signature: truncated");
    return (static_cast<std::uint32_t>(bytes[at]) << 24) | (static_cast<std::uint32_t>(bytes[at + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 8) | bytes[at + 3];
}
std::uint64_t be64(std::span<const std::uint8_t> bytes, std::size_t at) {
    return (static_cast<std::uint64_t>(be32(bytes, at)) << 32) | be32(bytes, at + 4);
}
std::uint32_t le32(std::span<const std::uint8_t> bytes, std::size_t at) {
    if (at + 4 > bytes.size()) throw std::out_of_range("Mach-O: truncated");
    return bytes[at] | (static_cast<std::uint32_t>(bytes[at + 1]) << 8) | (static_cast<std::uint32_t>(bytes[at + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
}
std::string c_string_at(std::span<const std::uint8_t> bytes, std::size_t at) {
    std::string value;
    while (at < bytes.size() && bytes[at]) value += static_cast<char>(bytes[at++]);
    return value;
}

std::optional<Bytes> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string_view digest_name(std::uint8_t type) {
    switch (type) {
    case hash_sha1: return "SHA1";
    case hash_sha256: case hash_sha256_truncated: return "SHA256";
    case hash_sha384: return "SHA384";
    default: return {};
    }
}
std::optional<Bytes> hash_of(std::uint8_t type, std::span<const std::uint8_t> data, std::size_t size) {
    const auto name = digest_name(type);
    if (name.empty()) return std::nullopt;
    auto value = digest(name, data);
    if (value && value->size() > size) value->resize(size);
    return value;
}

// ---- the signature --------------------------------------------------------------------------------

CodeDirectory parse_directory(std::span<const std::uint8_t> blob) {
    CodeDirectory cd;
    cd.blob.assign(blob.begin(), blob.end());
    cd.version = be32(blob, 8);
    cd.flags = be32(blob, 12);
    const auto hash_offset = be32(blob, 16), ident_offset = be32(blob, 20), special = be32(blob, 24), code_slots = be32(blob, 28);
    cd.code_limit = be32(blob, 32);
    if (blob.size() < 40) throw std::out_of_range("CodeDirectory: truncated");
    cd.hash_size = blob[36];
    cd.hash_type = blob[37];
    cd.platform = blob[38];
    cd.page_log2 = blob[39];
    cd.identifier = c_string_at(blob, ident_offset);
    if (cd.version >= 0x20200 && blob.size() >= 52)
        if (const auto team_offset = be32(blob, 48); team_offset) cd.team = c_string_at(blob, team_offset);
    if (cd.version >= 0x20300 && blob.size() >= 64)
        if (const auto limit64 = be64(blob, 56); limit64) cd.code_limit = limit64;
    if (cd.version >= 0x20400 && blob.size() >= 88) cd.exec_segment_flags = be64(blob, 80);
    const std::size_t size = cd.hash_size;
    if (!size || static_cast<std::uint64_t>(hash_offset) < static_cast<std::uint64_t>(special) * size ||
        hash_offset + static_cast<std::uint64_t>(code_slots) * size > blob.size())
        throw std::out_of_range("CodeDirectory: hashes outside the blob");
    for (std::uint32_t slot = 1; slot <= special; ++slot) {
        const auto at = hash_offset - slot * size;
        cd.special.emplace_back(blob.begin() + static_cast<std::ptrdiff_t>(at), blob.begin() + static_cast<std::ptrdiff_t>(at + size));
    }
    for (std::uint32_t page = 0; page < code_slots; ++page) {
        const auto at = hash_offset + static_cast<std::size_t>(page) * size;
        cd.code.emplace_back(blob.begin() + static_cast<std::ptrdiff_t>(at), blob.begin() + static_cast<std::ptrdiff_t>(at + size));
    }
    return cd;
}

std::optional<Signature> parse_signature(std::span<const std::uint8_t> blob) {
    if (blob.size() < 12 || be32(blob, 0) != magic_superblob) return std::nullopt;
    const auto length = std::min<std::size_t>(be32(blob, 4), blob.size());
    blob = blob.first(length);
    const auto count = be32(blob, 8);
    Signature signature;
    std::optional<CodeDirectory> primary;
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto type = be32(blob, 12 + index * 8), offset = be32(blob, 16 + index * 8);
        if (offset + 8 > blob.size()) throw std::out_of_range("SuperBlob: an entry outside it");
        const auto magic = be32(blob, offset), size = be32(blob, offset + 4);
        if (size < 8 || offset + size > blob.size()) throw std::out_of_range("SuperBlob: a blob outside it");
        const auto body = blob.subspan(offset, size);
        if (magic == magic_directory) {
            auto cd = parse_directory(body);
            if (type == 0) primary = std::move(cd);
            else signature.directories.push_back(std::move(cd));
        } else if (magic == magic_requirements) {
            signature.requirements.assign(body.begin(), body.end());
        } else if (magic == magic_entitlements) {
            signature.entitlements.assign(body.begin(), body.end());
        } else if (magic == magic_der_entitlements) {
            signature.der_entitlements.assign(body.begin(), body.end());
        } else if (magic == magic_cms) {
            signature.cms.assign(body.begin() + 8, body.end());
        }
    }
    if (!primary) return std::nullopt;
    signature.directories.insert(signature.directories.begin(), std::move(*primary));
    return signature;
}

// The slices of a Mach-O or universal file, with their signatures.
std::vector<Slice> parse_slices(std::span<const std::uint8_t> file, std::int32_t& status) {
    std::vector<Slice> slices;
    if (file.size() < 8) { status = cs::bad_object_format; return slices; }
    const auto fat_magic = be32(file, 0);
    if (fat_magic == 0xcafebabe || fat_magic == 0xcafebabf) {
        const auto count = be32(file, 4);
        const bool wide = fat_magic == 0xcafebabf;
        if (count > 64) { status = cs::bad_object_format; return slices; }
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto at = 8 + static_cast<std::size_t>(index) * (wide ? 32 : 20);
            Slice slice;
            slice.cpu_type = be32(file, at);
            slice.cpu_subtype = be32(file, at + 4);
            slice.offset = wide ? be64(file, at + 8) : be32(file, at + 8);
            slice.size = wide ? be64(file, at + 16) : be32(file, at + 12);
            if (slice.offset + slice.size > file.size()) { status = cs::bad_object_format; return {}; }
            slices.push_back(slice);
        }
    } else {
        Slice slice;
        slice.size = file.size();
        slices.push_back(slice);
    }
    for (auto& slice : slices) {
        const auto image = file.subspan(static_cast<std::size_t>(slice.offset), static_cast<std::size_t>(slice.size));
        const auto magic = le32(image, 0);
        if (magic != 0xfeedfacf && magic != 0xfeedface) { status = cs::bad_object_format; return {}; }
        const bool is64 = magic == 0xfeedfacf;
        if (!slice.cpu_type) { slice.cpu_type = le32(image, 4); slice.cpu_subtype = le32(image, 8); }
        const auto commands = le32(image, 16);
        std::size_t at = is64 ? 32 : 28;
        for (std::uint32_t index = 0; index < commands; ++index) {
            const auto command = le32(image, at), size = le32(image, at + 4);
            if (size < 8) { status = cs::bad_object_format; return {}; }
            if (command == 0x1d) {   // LC_CODE_SIGNATURE
                const auto offset = le32(image, at + 8), length = le32(image, at + 12);
                if (static_cast<std::uint64_t>(offset) + length > image.size()) { status = cs::signature_failed; return {}; }
                slice.signature = parse_signature(image.subspan(offset, length));
                if (!slice.signature) { status = cs::signature_failed; return {}; }
            }
            at += size;
        }
    }
    status = 0;
    return slices;
}

std::string_view hash_name_of(std::string_view oid) {
    if (oid == "1.3.14.3.2.26") return "SHA1";
    if (oid == "2.16.840.1.101.3.4.2.1") return "SHA256";
    if (oid == "2.16.840.1.101.3.4.2.2") return "SHA384";
    if (oid == "2.16.840.1.101.3.4.2.3") return "SHA512";
    return {};
}

// UTCTime YYMMDDHHMMSSZ / GeneralizedTime YYYYMMDDHHMMSSZ as CFAbsoluteTime.
std::optional<double> parse_time(std::span<const std::uint8_t> data, const ber::Element& element) {
    std::string text(data.begin() + static_cast<std::ptrdiff_t>(element.content), data.begin() + static_cast<std::ptrdiff_t>(element.end));
    std::tm tm{};
    std::size_t at = 0;
    const auto digits = [&](std::size_t count) {
        int value = 0;
        for (std::size_t index = 0; index < count; ++index, ++at) {
            if (at >= text.size() || !std::isdigit(static_cast<unsigned char>(text[at]))) throw std::invalid_argument("time");
            value = value * 10 + (text[at] - '0');
        }
        return value;
    };
    try {
        if (element.tag == 0x17) { const auto year = digits(2); tm.tm_year = (year < 50 ? 2000 + year : 1900 + year) - 1900; }
        else if (element.tag == 0x18) tm.tm_year = digits(4) - 1900;
        else return std::nullopt;
        tm.tm_mon = digits(2) - 1;
        tm.tm_mday = digits(2);
        tm.tm_hour = digits(2);
        tm.tm_min = digits(2);
        tm.tm_sec = digits(2);
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
    // Days from the civil date (proleptic Gregorian), then seconds since the Unix epoch.
    const int year = tm.tm_year + 1900 - (tm.tm_mon < 2 ? 1 : 0);
    const int era = (year >= 0 ? year : year - 399) / 400;
    const int year_of_era = year - era * 400;
    const int month = tm.tm_mon + 1;
    const int day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + tm.tm_mday - 1;
    const int day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    const auto days = static_cast<std::int64_t>(era) * 146097 + day_of_era - 719468;
    const auto unix_seconds = days * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    return static_cast<double>(unix_seconds) - unix_to_reference;
}

// ---- resources ----------------------------------------------------------------------------------------

fs::path joined(const fs::path& base, std::string_view relative) {
    auto path = base;
    std::size_t at = 0;
    while (at <= relative.size()) {
        const auto end = std::min(relative.find('/', at), relative.size());
        if (end > at) path /= utf8_path(relative.substr(at, end - at));
        at = end + 1;
    }
    return path;
}

// The cdhashes of every CodeDirectory of every slice of nested code (a bundle or a Mach-O). CodeResources records the
// one of the signing machine's architecture (its strongest CodeDirectory), so any of them may be the recorded one.
std::vector<Bytes> nested_cdhashes(const fs::path& path) {
    std::vector<Bytes> hashes;
    std::int32_t status{};
    const auto code = open(path, status);
    if (!code) return hashes;
    for (const auto& slice : code->slices)
        if (slice.signature)
            for (const auto& cd : slice.signature->directories) hashes.push_back(cd.cdhash());
    return hashes;
}

Validation check_resources(const StaticCode& code, std::uint32_t flags) {
    Validation result;
    if (!code.resources) return result;
    const auto bytes = read_file(*code.resources);
    const auto plist = bytes ? export_fill::plist_parse(*bytes) : std::nullopt;
    if (!plist || plist->kind != Plist::Kind::dictionary) return {cs::resources_invalid, "CodeResources is not a property list", {}, false};
    const auto* files = plist->find("files2");
    if (!files) files = plist->find("files");
    if (!files || files->kind != Plist::Kind::dictionary) return {cs::resources_invalid, "CodeResources has no files", {}, false};
    for (const auto& [name, entry] : files->entries) {
        const auto path = joined(code.contents, name);
        std::error_code error;
        const auto status = fs::symlink_status(path, error);
        const bool present = !error && fs::exists(status);
        bool optional = false;
        const Plist* hash = nullptr;
        const Plist* hash2 = nullptr;
        const Plist* symlink = nullptr;
        const Plist* cdhash = nullptr;
        if (entry.kind == Plist::Kind::data) {
            hash = &entry;
        } else if (entry.kind == Plist::Kind::dictionary) {
            if (const auto* value = entry.find("optional"); value && value->kind == Plist::Kind::boolean) optional = value->boolean;
            hash = entry.find("hash");
            hash2 = entry.find("hash2");
            symlink = entry.find("symlink");
            cdhash = entry.find("cdhash");
        }
        if (!present) {
            if (optional) continue;
            return {cs::bad_resource, "a sealed resource is missing", name, true};
        }
        if (symlink && symlink->kind == Plist::Kind::string) {
            // The link must lead where the recorded target leads (hosts may store the link with another spelling).
            const auto expected = fs::weakly_canonical(joined(path.parent_path(), symlink->text), error);
            const auto actual = fs::weakly_canonical(path, error);
            if (expected != actual) return {cs::bad_resource, "a sealed symbolic link was altered", name, false};
            continue;
        }
        if (cdhash && cdhash->kind == Plist::Kind::data) {
            if (flags & check_nested) {
                std::int32_t nested_status{};
                const auto nested = open(path, nested_status);
                if (!nested) return {cs::bad_nested_code, "nested code is not code", name, false};
                if (const auto check = validate(*nested, flags); check.status)
                    return {cs::bad_nested_code, "nested code is invalid: " + check.detail, name, false};
                // ... and must satisfy the requirement CodeResources records for it.
                if (const auto* requirement = entry.find("requirement"); requirement && requirement->kind == Plist::Kind::string) {
                    std::string error;
                    const auto blob = compile(requirement->text, error);
                    if (!blob) return {cs::resources_invalid, "a nested code requirement does not parse: " + error, name, false};
                    if (evaluate(*blob, *nested)) return {cs::bad_nested_code, "nested code does not satisfy its requirement", name, false};
                }
            }
            const auto actual = nested_cdhashes(path);
            if (std::find(actual.begin(), actual.end(), cdhash->bytes) == actual.end())
                return {cs::bad_resource, "nested code was modified", name, false};
            continue;
        }
        if (fs::is_directory(path, error)) continue;
        const auto data = read_file(path);
        if (!data) return {cs::bad_resource, "a sealed resource cannot be read", name, false};
        bool checked = false;
        if (hash2 && hash2->kind == Plist::Kind::data) {
            const auto value = digest("SHA256", *data);
            if (!value || *value != hash2->bytes) return {cs::bad_resource, "a sealed resource was modified", name, false};
            checked = true;
        } else if (hash && hash->kind == Plist::Kind::data) {
            const auto value = digest("SHA1", *data);
            if (!value || *value != hash->bytes) return {cs::bad_resource, "a sealed resource was modified", name, false};
            checked = true;
        }
        if (!checked) return {cs::resources_invalid, "a CodeResources entry has no hash", name, false};
    }
    return result;
}

Validation check_slice(const StaticCode& code, const Slice& slice, std::uint32_t flags) {
    if (!slice.signature) return {cs::unsigned_code, "code object is not signed at all", {}, false};
    const auto& signature = *slice.signature;
    const std::span<const std::uint8_t> image(code.file.data() + slice.offset, static_cast<std::size_t>(slice.size));
    for (const auto& cd : signature.directories) {
        if (digest_name(cd.hash_type).empty()) return {cs::unsupported_digest, "unsupported CodeDirectory hash", {}, false};
        // The executable's pages.
        if (!(flags & skip_executable)) {
            if (cd.code_limit > image.size()) return {cs::signature_failed, "the code limit is past the end of the file", {}, false};
            const std::uint64_t page = cd.page_log2 ? (1ULL << cd.page_log2) : cd.code_limit;
            for (std::size_t index = 0; index < cd.code.size(); ++index) {
                const auto start = index * page;
                if (start >= cd.code_limit) return {cs::signature_failed, "more page hashes than pages", {}, false};
                const auto size = static_cast<std::size_t>(std::min(page, cd.code_limit - start));
                const auto value = hash_of(cd.hash_type, image.subspan(static_cast<std::size_t>(start), size), cd.hash_size);
                if (!value || *value != cd.code[index]) return {cs::signature_failed, "code or signature have been modified", {}, false};
            }
        }
        // Special slots: Info.plist (1), requirements (2), CodeResources (3), entitlements (5), DER entitlements (7).
        const auto matches = [&](unsigned slot, std::span<const std::uint8_t> data) {
            const auto expected = cd.special_slot(slot);
            if (!expected) return true;
            const auto value = hash_of(cd.hash_type, data, cd.hash_size);
            return value && *value == *expected;
        };
        if (cd.special_slot(1)) {
            const auto info = code.info_plist ? read_file(*code.info_plist) : std::nullopt;
            if (!info || !matches(1, *info)) return {cs::info_plist_failed, "Info.plist does not match the signature", {}, false};
        }
        if (cd.special_slot(2) && (signature.requirements.empty() || !matches(2, signature.requirements)))
            return {cs::signature_failed, "the requirements do not match the signature", {}, false};
        if (cd.special_slot(3)) {
            const auto resources = code.resources ? read_file(*code.resources) : std::nullopt;
            if (!resources) return {cs::resources_not_found, "the sealed resources are missing", {}, false};
            if (!matches(3, *resources)) return {cs::signature_failed, "CodeResources does not match the signature", {}, false};
        }
        if (cd.special_slot(5) && (signature.entitlements.empty() || !matches(5, signature.entitlements)))
            return {cs::signature_failed, "the entitlements do not match the signature", {}, false};
        if (cd.special_slot(7) && (signature.der_entitlements.empty() || !matches(7, signature.der_entitlements)))
            return {cs::signature_failed, "the DER entitlements do not match the signature", {}, false};
    }
    // The CMS signature (none for ad-hoc signatures).
    if (!(signature.directories.front().flags & flag_adhoc)) {
        if (signature.cms.empty()) return {cs::signature_failed, "no CMS signature", {}, false};
        const auto cms = check_cms(signature);
        if (cms.status) return {cms.status, cms.detail, {}, false};
    }
    return {};
}

} // namespace

Bytes CodeDirectory::digest() const { return hash_of(hash_type, blob, 64).value_or(Bytes{}); }
Bytes CodeDirectory::cdhash() const {
    auto value = digest();
    if (value.size() > 20) value.resize(20);
    return value;
}
std::optional<Bytes> CodeDirectory::special_slot(unsigned slot) const {
    if (slot == 0 || slot > special.size()) return std::nullopt;
    const auto& value = special[slot - 1];
    if (std::all_of(value.begin(), value.end(), [](std::uint8_t b) { return b == 0; })) return std::nullopt;
    return value;
}

const CodeDirectory& Signature::best() const {
    const auto rank = [](std::uint8_t type) { return type == hash_sha384 ? 4 : type == hash_sha256 ? 3 : type == hash_sha256_truncated ? 2 : 1; };
    return *std::max_element(directories.begin(), directories.end(),
                             [&](const CodeDirectory& a, const CodeDirectory& b) { return rank(a.hash_type) < rank(b.hash_type); });
}

const Slice& StaticCode::preferred() const {
    for (const auto& slice : slices) if (slice.cpu_type == cpu_arm64) return slice;
    return slices.front();
}
bool StaticCode::is_signed() const { return !slices.empty() && preferred().signature.has_value(); }

std::optional<StaticCode> open(const fs::path& path, std::int32_t& status) {
    std::error_code error;
    if (!fs::exists(path, error)) { status = cs::static_code_not_found; return std::nullopt; }
    StaticCode code;
    code.root = path;
    if (fs::is_directory(path, error)) {
        code.bundle = true;
        fs::path contents;
        fs::path info;
        if (fs::exists(path / "Contents" / "Info.plist", error)) {
            contents = path / "Contents";
            info = contents / "Info.plist";
        } else if (fs::exists(path / "Versions" / "Current", error)) {
            contents = path / "Versions" / "Current";
            info = contents / "Resources" / "Info.plist";
        } else if (fs::exists(path / "Info.plist", error)) {
            contents = path;
            info = path / "Info.plist";
        } else {
            status = cs::bad_bundle_format;
            return std::nullopt;
        }
        code.contents = contents;
        code.info_plist = info;
        const auto bytes = read_file(info);
        const auto plist = bytes ? export_fill::plist_parse(*bytes) : std::nullopt;
        if (!plist) { status = cs::info_plist_failed; return std::nullopt; }
        const auto* name = plist->find("CFBundleExecutable");
        if (!name || name->kind != Plist::Kind::string || name->text.empty()) { status = cs::no_main_executable; return std::nullopt; }
        code.executable = fs::exists(contents / "MacOS", error) ? contents / "MacOS" / utf8_path(name->text) : contents / utf8_path(name->text);
        if (!fs::exists(code.executable, error)) { status = cs::no_main_executable; return std::nullopt; }
        if (const auto resources = contents / "_CodeSignature" / "CodeResources"; fs::exists(resources, error)) code.resources = resources;
    } else {
        code.executable = path;
        code.contents = path.parent_path();
    }
    auto file = read_file(code.executable);
    if (!file) { status = cs::static_code_not_found; return std::nullopt; }
    code.file = std::move(*file);
    try {
        code.slices = parse_slices(code.file, status);
    } catch (const std::out_of_range&) {
        status = cs::signature_failed;
        return std::nullopt;
    }
    if (status) return std::nullopt;
    return code;
}

bool is_apple_root(const Certificate& certificate) {
    // Apple Root CA, SHA-256 fingerprint as published at apple.com/certificateauthority.
    static constexpr std::array<std::uint8_t, 32> apple_root{
        0xb0, 0xb1, 0x73, 0x0e, 0xcb, 0xc7, 0xff, 0x45, 0x05, 0x14, 0x2c, 0x49, 0xf1, 0x29, 0x5e, 0x6e,
        0xda, 0x6b, 0xca, 0xed, 0x7e, 0x2c, 0x68, 0xc5, 0xbe, 0x91, 0xb5, 0xa1, 0x10, 0x01, 0xf0, 0x24};
    const auto value = certificate.sha256();
    return value.size() == apple_root.size() && std::equal(value.begin(), value.end(), apple_root.begin());
}

CmsResult check_cms(const Signature& signature) {
    CmsResult result;
    const std::span<const std::uint8_t> data(signature.cms);
    const auto fail = [&](std::int32_t status, std::string detail) {
        result.status = status;
        result.detail = std::move(detail);
        return result;
    };
    // ContentInfo { contentType, [0] EXPLICIT SignedData }
    const auto info = ber::at(data, 0);
    if (!info) return fail(cs::signature_failed, "the CMS blob is not BER");
    const auto info_items = ber::children(data, *info);
    if (info_items.size() < 2 || ber::oid_text(data, info_items[0]) != "1.2.840.113549.1.7.2") return fail(cs::signature_failed, "not CMS SignedData");
    const auto explicit_items = ber::children(data, info_items[1]);
    if (explicit_items.empty()) return fail(cs::signature_failed, "empty SignedData");
    const auto signed_items = ber::children(data, explicit_items[0]);
    std::vector<Bytes> certificate_ders;
    const ber::Element* signer_infos = nullptr;
    for (const auto& item : signed_items) {
        if (item.tag == 0xa0) for (const auto& certificate : ber::children(data, item)) certificate_ders.push_back(ber::whole(data, certificate));
        if (item.tag == 0x31) signer_infos = &item;   // the last SET is signerInfos (the first is digestAlgorithms)
    }
    if (!signer_infos) return fail(cs::signature_failed, "no SignerInfo");
    std::vector<CertificatePtr> certificates;
    for (const auto& der : certificate_ders)
        if (auto certificate = Certificate::parse(der)) certificates.push_back(std::move(certificate));
    const auto signers = ber::children(data, *signer_infos);
    if (signers.size() != 1) return fail(cs::signature_failed, "not exactly one CMS signer");
    const auto fields = ber::children(data, signers[0]);
    // SignerInfo { version, sid, digestAlgorithm, [0] signedAttrs, signatureAlgorithm, signature, [1] unsignedAttrs }
    if (fields.size() < 5) return fail(cs::signature_failed, "a short SignerInfo");
    const auto sid = ber::children(data, fields[1]);
    CertificatePtr signer;
    if (sid.size() == 2) {
        const auto issuer = ber::whole(data, sid[0]);
        const auto serial = ber::contents(data, sid[1]);
        for (const auto& certificate : certificates)
            if (certificate->issuer_sequence() == issuer && certificate->serial_number() == serial) signer = certificate;
    }
    if (!signer) return fail(cs::signature_failed, "the CMS signer certificate is not in the blob");
    const auto digest_algorithm = ber::children(data, fields[2]);
    const auto hash_name = digest_algorithm.empty() ? std::string_view{} : hash_name_of(ber::oid_text(data, digest_algorithm[0]));
    if (hash_name.empty()) return fail(cs::signature_unsupported, "an unsupported CMS digest");
    std::size_t index = 3;
    if (fields[index].tag != 0xa0) return fail(cs::signature_unsupported, "a CMS signature without signed attributes");
    const auto& attributes = fields[index++];
    if (data[attributes.start + 1] == 0x80) return fail(cs::signature_unsupported, "BER-encoded signed attributes");
    const auto signature_algorithm = ber::children(data, fields[index++]);
    if (index >= fields.size() || fields[index].tag != 0x04) return fail(cs::signature_failed, "no CMS signature value");
    const auto signature_value = ber::contents(data, fields[index]);

    // The signed attributes: the message digest must be the primary CodeDirectory's; the CDHashes list (when present)
    // must name each CodeDirectory.
    std::optional<Bytes> message_digest;
    std::optional<Bytes> cdhashes_plist;
    for (const auto& attribute : ber::children(data, attributes)) {
        const auto parts = ber::children(data, attribute);
        if (parts.size() != 2) continue;
        const auto oid = ber::oid_text(data, parts[0]);
        const auto values = ber::children(data, parts[1]);
        if (values.empty()) continue;
        if (oid == "1.2.840.113549.1.9.4") message_digest = ber::contents(data, values[0]);
        else if (oid == "1.2.840.113549.1.9.5") result.signing_time = parse_time(data, values[0]);
        else if (oid == "1.2.840.113635.100.9.1") cdhashes_plist = ber::contents(data, values[0]);
    }
    const auto& primary = signature.directories.front();
    const auto expected = digest(hash_name, primary.blob);
    if (!message_digest || !expected || *message_digest != *expected) return fail(cs::signature_failed, "the CMS digest does not match the CodeDirectory");
    if (cdhashes_plist) {
        const auto plist = export_fill::plist_parse(*cdhashes_plist);
        const auto* list = plist ? plist->find("cdhashes") : nullptr;
        if (!list || list->kind != Plist::Kind::array) return fail(cs::signature_failed, "a malformed CDHashes attribute");
        for (const auto& cd : signature.directories) {
            const auto hash = cd.cdhash();
            if (std::none_of(list->items.begin(), list->items.end(), [&](const Plist& item) { return item.bytes == hash; }))
                return fail(cs::signature_failed, "a CodeDirectory is not in the signed CDHashes");
        }
    }
    // The signature over the DER SET OF the signed attributes.
    auto signed_bytes = ber::whole(data, attributes);
    signed_bytes[0] = 0x31;
    const auto key = key_from_spki(signer->subject_public_key_info());
    if (!key) return fail(cs::signature_unsupported, "an unsupported signer key");
    const auto algorithm_oid = signature_algorithm.empty() ? std::string{} : ber::oid_text(data, signature_algorithm[0]);
    std::string algorithm;
    if (key->type == KeyMaterial::Type::rsa) algorithm = "algid:sign:RSA:message-PKCS1v15:" + std::string(hash_name);
    else algorithm = "algid:sign:ECDSA:message-X962:" + std::string(hash_name);
    if (algorithm_oid == "1.2.840.10045.4.3.3") algorithm = "algid:sign:ECDSA:message-X962:SHA384";
    else if (algorithm_oid == "1.2.840.113549.1.1.12") algorithm = "algid:sign:RSA:message-PKCS1v15:SHA384";
    if (const auto verified = key_verify(*key, algorithm, signed_bytes, signature_value); verified.status)
        return fail(cs::signature_failed, "the CMS signature does not verify");

    // The chain the blob carries, signer first; each certificate must be signed by the next.
    result.certificates.push_back(signer);
    auto current = signer;
    while (!current->self_issued() && result.certificates.size() < certificates.size()) {
        CertificatePtr issuer;
        for (const auto& certificate : certificates)
            if (certificate != current && certificate->subject_sequence() == current->issuer_sequence()) issuer = certificate;
        if (!issuer) break;
        result.certificates.push_back(issuer);
        current = issuer;
    }
    if (result.certificates.size() > 1 && result.certificates.back()->self_issued()) {
        host::crypto::ChainRequest request;
        request.leaf = signer->host();
        for (std::size_t at = 1; at + 1 < result.certificates.size(); ++at) request.intermediates.push_back(result.certificates[at]->host());
        request.anchors.push_back(result.certificates.back()->host());
        if (result.signing_time) request.time_ns = static_cast<std::int64_t>((*result.signing_time + unix_to_reference) * 1e9);
        request.network_fetch = false;
        const auto chain = host::crypto::evaluate_chain(request);
        if (chain && chain->failure == host::crypto::ChainFailure::bad_signature)
            return fail(cs::signature_failed, "a certificate in the signature chain does not verify");
    }
    return result;
}

Validation validate(const StaticCode& code, std::uint32_t flags) {
    if (code.slices.empty()) return {cs::bad_object_format, "not a Mach-O file", {}, false};
    if (!code.is_signed()) return {cs::unsigned_code, "code object is not signed at all", {}, false};
    if (flags & check_all_architectures) {
        for (const auto& slice : code.slices)
            if (auto result = check_slice(code, slice, flags); result.status) return result;
    } else if (auto result = check_slice(code, code.preferred(), flags); result.status) {
        return result;
    }
    if (code.bundle && !(flags & skip_resources)) return check_resources(code, flags);
    return {};
}

std::string format_of(const StaticCode& code) {
    const auto architecture = [](const Slice& slice) -> std::string {
        if (slice.cpu_type == cpu_arm64) return slice.cpu_subtype == 2 ? "arm64e" : "arm64";
        if (slice.cpu_type == cpu_x86_64) return "x86_64";
        return "cpu " + std::to_string(slice.cpu_type);
    };
    std::string text;
    if (code.slices.size() == 1) {
        text = "Mach-O thin (" + architecture(code.slices.front()) + ")";
    } else {
        text = "Mach-O universal (";
        for (std::size_t index = 0; index < code.slices.size(); ++index) text += (index ? " " : "") + architecture(code.slices[index]);
        text += ")";
    }
    if (code.bundle) {
        const auto extension = code.root.extension().string();
        text = (extension == ".app" ? "app bundle with " : extension == ".framework" ? "framework bundle with " : "bundle with ") + text;
    }
    return text;
}

} // namespace alchemy::gui::security::codesign
