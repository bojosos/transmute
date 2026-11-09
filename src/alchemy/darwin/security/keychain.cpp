#include "alchemy/darwin/security/security_internal.hpp"

#include "alchemy/darwin/security/keys.hpp"

#include "alchemy/alchemy_root.hpp"
#include "alchemy/host/crypto.hpp"
#include "alchemy/host/files.hpp"
#include "alchemy/host/sync.hpp"
#include "alchemy/host/system.hpp"
#include "alchemy/path.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <map>
#include <set>
#include <variant>

// Keychain items (SecItemAdd / SecItemCopyMatching / SecItemUpdate /
// SecItemDelete) and the legacy SecKeychain API for generic and internet
// passwords. The default keychain is one file per Windows user, encrypted
// with DPAPI for that user: <Alchemy root>/.alchemy/keychain/<user>.alchemy-keychain
// when an Alchemy root is configured (outside the guest-visible /Applications
// and /Library), otherwise %LOCALAPPDATA%/Alchemy/keychain/login.alchemy-keychain;
// ALCHEMY_KEYCHAIN overrides the path. Keys and certificates are
// stored in the same file (nothing goes to the Windows certificate store):
// a key item holds the SecKey external representation, a certificate item
// its DER, each with the attributes macOS gives them. An identity is a
// stored certificate whose public key hash (pkhh) is a stored private key's
// application label (klbl), as on macOS; kSecAttrIsExtractable false keeps
// a key's bytes from being returned (errSecDataNotAvailable). Identity
// preferences (SecIdentitySetPreferred) are records of the same file.
// Legacy API: generic and internet passwords (add / find with their stack
// arguments), attribute lists (SecKeychainItemCopyAttributesAndData,
// CopyContent, ModifyAttributesAndData, CreateFromContent and the Free calls),
// searches (SecKeychainSearchCreateFromAttributes / CopyNext), and keychain
// files: SecKeychainCreate makes another file in the same encrypted format
// that also holds a salted PBKDF2 hash of its password, SecKeychainUnlock
// checks it (errSecAuthFailed), Lock / LockAll lock keychains for this process
// (items of a locked keychain answer errSecInteractionNotAllowed, as there is
// no UI to ask for the password), Open, Delete, GetPath, GetStatus,
// Set / CopySearchList and Set / CopyDefault. The default keychain is
// protected by the Windows logon, so it unlocks without a password. SecItem
// calls use the default keychain.
namespace alchemy::gui::security {
namespace {

constexpr std::string_view lib = security_library;
constexpr std::string_view file_magic = "ALCHEMYKC1\n";

struct Value {
    enum class Kind : std::uint8_t { string, data, integer, real, boolean, date } kind{Kind::string};
    std::string text;
    Bytes bytes;
    std::int64_t integer{};
    double real{};
    bool operator==(const Value& other) const {
        if (kind != other.kind) {
            // Numbers compare by value across integer / boolean.
            const auto numeric = [](const Value& v) -> std::optional<double> {
                if (v.kind == Kind::integer || v.kind == Kind::boolean) return static_cast<double>(v.integer);
                if (v.kind == Kind::real) return v.real;
                return std::nullopt;
            };
            const auto left = numeric(*this), right = numeric(other);
            return left && right && *left == *right;
        }
        switch (kind) {
        case Kind::string: return text == other.text;
        case Kind::data: return bytes == other.bytes;
        case Kind::integer: case Kind::boolean: return integer == other.integer;
        case Kind::real: case Kind::date: return real == other.real;
        }
        return false;
    }
};

Value text_value(std::string text) { Value value; value.kind = Value::Kind::string; value.text = std::move(text); return value; }
Value data_value(Bytes bytes) { Value value; value.kind = Value::Kind::data; value.bytes = std::move(bytes); return value; }
Value integer_value(std::int64_t number) { Value value; value.kind = Value::Kind::integer; value.integer = number; return value; }
Value boolean_value(bool on) { Value value; value.kind = Value::Kind::boolean; value.integer = on ? 1 : 0; return value; }

struct Item {
    std::uint64_t id{};
    std::string cls;                          // "genp" / "inet"
    std::map<std::string, Value> attributes;  // keyed by attribute name ("acct", "svce", ...)
    Bytes secret;
    double created{}, modified{};
    Bytes access;                             // a serialized Access (SecKeychainItemSetAccess / initialAccess); empty: none set
};

// ---- access control (legacy SecAccess / SecACL / SecTrustedApplication) -----------------------------
// Recorded and reported: one guest process and no prompt UI, so nothing is enforced and nothing prompts.

struct Acl {
    std::string role;                                   // "owner" (changes the access) or "use" (reads the secret)
    std::optional<std::vector<std::string>> applications;   // trusted application paths; nullopt: any application
    std::string description;
    std::uint32_t prompt{};                             // SecKeychainPromptSelector
};
struct Access {
    std::string descriptor;
    std::vector<std::shared_ptr<Acl>> acls;
};

// ---- storage ----------------------------------------------------------------------------------------

std::filesystem::path keychain_path() {
    if (const auto value = host_environment("ALCHEMY_KEYCHAIN"); !value.empty()) return utf8_path(value);
    if (const auto root = configured_alchemy_root()) {
        const auto user = host::system::user_name();
        if (!user) fail("cannot read the user name for the keychain location");
        return *root / ".alchemy" / "keychain" / utf8_path(*user + ".alchemy-keychain");
    }
    const auto local = host_environment("LOCALAPPDATA");
    if (local.empty()) fail("LOCALAPPDATA is not set; the keychain has no location");
    return utf8_path(local) / "Alchemy" / "keychain" / "login.alchemy-keychain";
}

void put32(Bytes& out, std::uint32_t value) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i))); }
void put64(Bytes& out, std::uint64_t value) { for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i))); }
void put_bytes(Bytes& out, std::span<const std::uint8_t> value) { put32(out, static_cast<std::uint32_t>(value.size())); out.insert(out.end(), value.begin(), value.end()); }
void put_text(Bytes& out, std::string_view value) { put_bytes(out, std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size())); }

struct Reader {
    std::span<const std::uint8_t> data;
    std::size_t at{};
    bool ok{true};
    std::uint64_t number(int size) {
        if (at + static_cast<std::size_t>(size) > data.size()) { ok = false; return 0; }
        std::uint64_t value = 0;
        for (int i = 0; i < size; ++i) value |= static_cast<std::uint64_t>(data[at + static_cast<std::size_t>(i)]) << (8 * i);
        at += static_cast<std::size_t>(size);
        return value;
    }
    Bytes bytes() {
        const auto size = number(4);
        if (!ok || at + size > data.size()) { ok = false; return {}; }
        Bytes value(data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + size));
        at += size;
        return value;
    }
    std::string text() { const auto value = bytes(); return std::string(value.begin(), value.end()); }
};

constexpr std::uint32_t password_trailer = 0x5750434b;   // "KCPW": a created keychain's password hash
constexpr std::uint32_t access_trailer = 0x4341434b;     // "KCAC": item access (id, serialized Access)

Bytes serialize_access(const Access& access) {
    Bytes out;
    put_text(out, access.descriptor);
    put32(out, static_cast<std::uint32_t>(access.acls.size()));
    for (const auto& acl : access.acls) {
        put_text(out, acl->role);
        out.push_back(acl->applications ? 1 : 0);
        if (acl->applications) {
            put32(out, static_cast<std::uint32_t>(acl->applications->size()));
            for (const auto& path : *acl->applications) put_text(out, path);
        }
        put_text(out, acl->description);
        put32(out, acl->prompt);
    }
    return out;
}
std::optional<Access> deserialize_access(std::span<const std::uint8_t> data) {
    Reader in{data};
    Access access;
    access.descriptor = in.text();
    const auto count = in.number(4);
    for (std::uint64_t index = 0; in.ok && index < count && index < 64; ++index) {
        auto acl = std::make_shared<Acl>();
        acl->role = in.text();
        if (in.number(1)) {
            std::vector<std::string> paths;
            const auto apps = in.number(4);
            for (std::uint64_t a = 0; in.ok && a < apps && a < 1024; ++a) paths.push_back(in.text());
            acl->applications = std::move(paths);
        }
        acl->description = in.text();
        acl->prompt = static_cast<std::uint32_t>(in.number(4));
        access.acls.push_back(std::move(acl));
    }
    if (!in.ok || in.at != data.size()) return std::nullopt;
    return access;
}

Bytes serialize(const std::vector<Item>& items, std::uint64_t next_id, const std::optional<Bytes>& password) {
    Bytes out;
    put64(out, next_id);
    put32(out, static_cast<std::uint32_t>(items.size()));
    for (const auto& item : items) {
        put64(out, item.id);
        put_text(out, item.cls);
        put64(out, std::bit_cast<std::uint64_t>(item.created));
        put64(out, std::bit_cast<std::uint64_t>(item.modified));
        put_bytes(out, item.secret);
        put32(out, static_cast<std::uint32_t>(item.attributes.size()));
        for (const auto& [name, value] : item.attributes) {
            put_text(out, name);
            out.push_back(static_cast<std::uint8_t>(value.kind));
            switch (value.kind) {
            case Value::Kind::string: put_text(out, value.text); break;
            case Value::Kind::data: put_bytes(out, value.bytes); break;
            case Value::Kind::integer: case Value::Kind::boolean: put64(out, static_cast<std::uint64_t>(value.integer)); break;
            case Value::Kind::real: case Value::Kind::date: put64(out, std::bit_cast<std::uint64_t>(value.real)); break;
            }
        }
    }
    if (password) {
        put32(out, password_trailer);
        put_bytes(out, *password);
    }
    std::size_t with_access = 0;
    for (const auto& item : items) with_access += item.access.empty() ? 0 : 1;
    if (with_access) {
        put32(out, access_trailer);
        put32(out, static_cast<std::uint32_t>(with_access));
        for (const auto& item : items)
            if (!item.access.empty()) { put64(out, item.id); put_bytes(out, item.access); }
    }
    return out;
}

bool deserialize(std::span<const std::uint8_t> data, std::vector<Item>& items, std::uint64_t& next_id, std::optional<Bytes>& password) {
    Reader in{data};
    next_id = in.number(8);
    const auto count = in.number(4);
    for (std::uint64_t index = 0; in.ok && index < count; ++index) {
        Item item;
        item.id = in.number(8);
        item.cls = in.text();
        item.created = std::bit_cast<double>(in.number(8));
        item.modified = std::bit_cast<double>(in.number(8));
        item.secret = in.bytes();
        const auto attributes = in.number(4);
        for (std::uint64_t a = 0; in.ok && a < attributes; ++a) {
            const auto name = in.text();
            Value value;
            value.kind = static_cast<Value::Kind>(in.number(1));
            switch (value.kind) {
            case Value::Kind::string: value.text = in.text(); break;
            case Value::Kind::data: value.bytes = in.bytes(); break;
            case Value::Kind::integer: case Value::Kind::boolean: value.integer = static_cast<std::int64_t>(in.number(8)); break;
            case Value::Kind::real: case Value::Kind::date: value.real = std::bit_cast<double>(in.number(8)); break;
            default: in.ok = false; break;
            }
            item.attributes.emplace(name, std::move(value));
        }
        items.push_back(std::move(item));
    }
    while (in.ok && in.at < data.size()) {
        const auto tag = in.number(4);
        if (tag == password_trailer) {
            password = in.bytes();
        } else if (tag == access_trailer) {
            const auto count = in.number(4);
            for (std::uint64_t index = 0; in.ok && index < count; ++index) {
                const auto id = in.number(8);
                auto access = in.bytes();
                for (auto& item : items) if (item.id == id) item.access = std::move(access);
            }
        } else {
            return false;
        }
    }
    return in.ok && in.at == data.size();
}

const std::uint8_t entropy_bytes[] = {'A', 'l', 'c', 'h', 'e', 'm', 'y', ' ', 'k', 'e', 'y', 'c', 'h', 'a', 'i', 'n', ' ', 'v', '1'};

// The decrypted store, held while the process-wide and cross-process locks are held.
class Store {
public:
    explicit Store(std::filesystem::path path = keychain_path()) : path_(std::move(path)) {
        // Shared by name with other Alchemy processes of this login session.
        auto mutex = host::sync::create_mutex("Alchemy.Keychain." + std::to_string(std::hash<std::wstring>{}(path_.wstring())));
        if (!mutex) fail("cannot create the keychain lock");
        mutex_.reset(*mutex);
        const std::array handles{mutex_.get()};
        if (!host::sync::wait_any(handles, 30'000'000'000ULL)) { mutex_.reset(); fail("timed out waiting for the keychain lock"); }
        load();
    }
    ~Store() { static_cast<void>(host::sync::unlock_mutex(mutex_.get())); }
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    std::vector<Item> items;
    std::uint64_t next_id{1};
    std::optional<Bytes> password;   // salt (16 bytes) and PBKDF2-SHA256 (32 bytes); none for the default keychain
    bool existed{};

    void save() {
        auto plain = serialize(items, next_id, password);
        const auto sealed = host::crypto::protect(std::as_bytes(std::span(plain)), std::as_bytes(std::span(entropy_bytes)), "Alchemy keychain");
        host::crypto::secure_zero(std::as_writable_bytes(std::span(plain)));
        if (!sealed) fail("the keychain cannot be encrypted (" + std::string(host::name(sealed.error())) + ")");
        std::error_code error;
        std::filesystem::create_directories(path_.parent_path(), error);
        auto temporary = path_;
        temporary += L".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file.write(file_magic.data(), static_cast<std::streamsize>(file_magic.size()));
            file.write(reinterpret_cast<const char*>(sealed->data()), static_cast<std::streamsize>(sealed->size()));
            if (!file) fail("cannot write the keychain file");
        }
        const auto text = [](const std::filesystem::path& path) {
            const auto bytes = path.u8string();
            return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        };
        if (const auto replaced = host::files::replace(text(temporary), text(path_)); !replaced)
            fail("cannot replace the keychain file (" + std::string(host::name(replaced.error())) + ")");
    }

private:
    void load() {
        std::error_code error;
        if (!std::filesystem::exists(path_, error)) return;
        existed = true;
        std::ifstream file(path_, std::ios::binary);
        Bytes content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (content.size() < file_magic.size() || std::memcmp(content.data(), file_magic.data(), file_magic.size()) != 0)
            fail("the keychain file is not an Alchemy keychain: " + narrow(path_.wstring()));
        auto plain = host::crypto::unprotect(std::as_bytes(std::span(content).subspan(file_magic.size())), std::as_bytes(std::span(entropy_bytes)));
        if (!plain) fail("the keychain cannot be decrypted for this user (" + std::string(host::name(plain.error())) + ")");
        const bool ok = deserialize(std::span(*plain), items, next_id, password);
        host::crypto::secure_zero(std::as_writable_bytes(std::span(*plain)));
        if (!ok) fail("the keychain file is corrupt");
    }
    std::filesystem::path path_;
    host::OwnedHandle mutex_;
};

std::mutex process_mutex;

// Keychains this process has locked (by host path); the default keychain unlocks without a password.
std::mutex lock_mutex;
std::set<std::wstring> locked_keychains;
bool is_locked(const std::filesystem::path& path) {
    std::lock_guard lock(lock_mutex);
    return locked_keychains.contains(path.wstring());
}

constexpr std::int32_t err_auth_failed = -25293, err_no_such_keychain = -25294, err_duplicate_keychain = -25296,
    err_buffer_too_small = -25301, err_no_such_attribute = -25303, err_no_such_class = -25306;
constexpr std::uint32_t pbkdf2_rounds = 20000;

// A keychain password record: a random salt and PBKDF2-SHA256 of the password.
std::optional<Bytes> password_record(std::span<const std::uint8_t> password) {
    Bytes record(16 + 32);
    if (!host::crypto::random(std::as_writable_bytes(std::span(record).first(16)))) return std::nullopt;
    if (!host::crypto::pbkdf2(host::crypto::Digest::sha256, std::as_bytes(password), std::as_bytes(std::span(record).first(16)),
                              pbkdf2_rounds, std::as_writable_bytes(std::span(record).subspan(16))))
        return std::nullopt;
    return record;
}
bool password_matches(const Bytes& record, std::span<const std::uint8_t> password) {
    if (record.size() != 48) return false;
    Bytes hash(32);
    if (!host::crypto::pbkdf2(host::crypto::Digest::sha256, std::as_bytes(password), std::as_bytes(std::span(record).first(16)),
                              pbkdf2_rounds, std::as_writable_bytes(std::span(hash))))
        return false;
    return std::equal(hash.begin(), hash.end(), record.begin() + 16);
}

// Legacy FourCharCode tags and the store's attribute names / value kinds.
std::string four_cc(std::uint32_t code) {
    return {static_cast<char>(code >> 24), static_cast<char>(code >> 16), static_cast<char>(code >> 8), static_cast<char>(code)};
}
std::uint32_t four_cc(std::string_view text) {
    std::uint32_t code = 0;
    for (std::size_t index = 0; index < 4; ++index) code = (code << 8) | static_cast<std::uint8_t>(index < text.size() ? text[index] : ' ');
    return code;
}
enum class TagKind { text, blob, number, four_char_text, boolean, date };
TagKind tag_kind(std::string_view tag) {
    if (tag == "port" || tag == "crtr" || tag == "type" || tag == "scrp") return TagKind::number;
    if (tag == "ptcl" || tag == "atyp") return TagKind::four_char_text;
    if (tag == "invi" || tag == "nega" || tag == "cusi") return TagKind::boolean;
    if (tag == "cdat" || tag == "mdat") return TagKind::date;
    if (tag == "gena" || tag == "alis" || tag == "sign" || tag == "addr") return TagKind::blob;
    return TagKind::text;
}
// "YYYYMMDDhhmmssZ" (the legacy date attribute) for a CFAbsoluteTime.
std::string legacy_date(double absolute) {
    auto seconds = static_cast<std::int64_t>(absolute) + 978307200;
    auto days = seconds / 86400;
    seconds -= days * 86400;
    if (seconds < 0) { seconds += 86400; --days; }
    days += 719468;
    const auto era = (days >= 0 ? days : days - 146096) / 146097;
    const auto day_of_era = days - era * 146097;
    const auto year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const auto day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const auto mp = (5 * day_of_year + 2) / 153;
    const auto day = day_of_year - (153 * mp + 2) / 5 + 1;
    const auto month = mp < 10 ? mp + 3 : mp - 9;
    const auto year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);
    char text[32];
    std::snprintf(text, sizeof(text), "%04lld%02lld%02lld%02lld%02lld%02lldZ", static_cast<long long>(year), static_cast<long long>(month),
                  static_cast<long long>(day), static_cast<long long>(seconds / 3600), static_cast<long long>(seconds / 60 % 60),
                  static_cast<long long>(seconds % 60));
    return text;
}

// Primary-key attributes (duplicate detection).
const std::vector<std::string>& primary_keys(const std::string& cls) {
    static const std::vector<std::string> generic{"acct", "svce", "agrp", "sync"};
    static const std::vector<std::string> internet{"acct", "sdmn", "srvr", "ptcl", "atyp", "port", "path", "agrp", "sync"};
    static const std::vector<std::string> key{"kcls", "klbl", "atag", "type", "bsiz", "esiz", "agrp"};
    static const std::vector<std::string> certificate{"ctyp", "issr", "slnr", "agrp"};
    static const std::vector<std::string> preference{"svce"};
    if (cls == "keys") return key;
    if (cls == "cert") return certificate;
    if (cls == "iprf") return preference;
    return cls == "inet" ? internet : generic;
}

struct Query {
    std::string cls;
    std::map<std::string, Value> attributes;
    std::optional<Bytes> secret;
    std::optional<std::uint64_t> persistent;   // v_PersistentRef
    Address ref{};                             // v_Ref: the item to add, or a match value
    bool sync_any{};
    bool return_data{}, return_attributes{}, return_ref{}, return_persistent{};
    std::optional<bool> limit_all;             // m_Limit
    std::int32_t status{};                     // a parse error
};

bool matches(const Item& item, const Query& query) {
    if (item.cls != query.cls) return false;
    if (query.persistent && item.id != *query.persistent) return false;
    for (const auto& [name, value] : query.attributes) {
        const auto found = item.attributes.find(name);
        if (found == item.attributes.end() || !(found->second == value)) return false;
    }
    if (!query.sync_any && !query.attributes.contains("sync")) {
        const auto found = item.attributes.find("sync");
        if (found != item.attributes.end() && found->second.integer != 0) return false;
    }
    return true;
}

Value primary_value(const Item& item, const std::string& name) {
    if (const auto found = item.attributes.find(name); found != item.attributes.end()) return found->second;
    if (name == "port" || name == "sync") { Value zero; zero.kind = Value::Kind::integer; return zero; }
    return Value{};
}

bool duplicates(const Item& left, const Item& right) {
    if (left.cls != right.cls || left.id == right.id) return false;
    for (const auto& name : primary_keys(left.cls))
        if (!(primary_value(left, name) == primary_value(right, name))) return false;
    return true;
}

Bytes persistent_ref(std::uint64_t id) {
    Bytes value{'a', 'k', 'c', '1'};
    for (int i = 0; i < 8; ++i) value.push_back(static_cast<std::uint8_t>(id >> (8 * i)));
    return value;
}
std::optional<std::uint64_t> persistent_id(const Bytes& value) {
    if (value.size() != 12 || std::memcmp(value.data(), "akc1", 4) != 0) return std::nullopt;
    std::uint64_t id = 0;
    for (int i = 0; i < 8; ++i) id |= static_cast<std::uint64_t>(value[4 + static_cast<std::size_t>(i)]) << (8 * i);
    return id;
}

struct KeychainItemState final : ObjectPayload { std::uint64_t id{}; std::filesystem::path keychain; };   // empty: the default keychain
struct AccessState final : ObjectPayload { std::shared_ptr<Access> access; };
struct AclState final : ObjectPayload { std::shared_ptr<Access> access; std::shared_ptr<Acl> acl; };
struct TrustedApplicationState final : ObjectPayload { std::string path; };
struct KeychainState final : ObjectPayload { std::filesystem::path path; };                          // empty: the default keychain
struct SearchState final : ObjectPayload {
    std::vector<std::filesystem::path> keychains;
    std::string cls;
    std::map<std::string, Value> attributes;
    std::size_t keychain{};      // the keychain being searched
    std::uint64_t after{};       // the last item returned from it
};

struct Keychain {
    Security& sec;
    Bridge& b;
    Address item_class{}, keychain_class{}, default_keychain{};
    Address access_class{}, acl_class{}, trusted_class{};

    // ---- access control ----
    std::shared_ptr<Access> access_of(Address object) {
        const auto state = object ? b.objc.find_host_state<AccessState>(object) : nullptr;
        return state ? state->access : nullptr;
    }
    Address make_access(std::shared_ptr<Access> access) {
        const auto object = b.objc.allocate(access_class);
        b.objc.host_state<AccessState>(object)->access = std::move(access);
        return object;
    }
    Address make_trusted(std::string path) {
        const auto object = b.objc.allocate(trusted_class);
        b.objc.host_state<TrustedApplicationState>(object)->path = std::move(path);
        return object;
    }
    // The calling application's executable path (its main bundle's).
    std::string calling_application() {
        const auto bundle = b.call(alchemy::core_foundation_library, "_CFBundleGetMainBundle", {});
        const auto url = bundle ? b.call(alchemy::core_foundation_library, "_CFBundleCopyExecutableURL", {bundle}) : 0;
        if (!url) return {};
        auto path = b.cf.url_path(url);
        b.release(url);
        return path;
    }
    // A CFArray of SecTrustedApplicationRefs as paths; nullopt for NULL (any application). false when an entry is not one.
    bool application_paths(Address list, std::optional<std::vector<std::string>>& out) {
        out.reset();
        if (!list) return true;
        if (!b.is_array(list)) return false;
        std::vector<std::string> paths;
        for (const auto entry : b.cf.array_values(list)) {
            const auto state = entry ? b.objc.find_host_state<TrustedApplicationState>(entry) : nullptr;
            if (!state) return false;
            paths.push_back(state->path);
        }
        out = std::move(paths);
        return true;
    }
    // SecAccessCreate's lists: an owner ACL (no application changes the access without confirmation)
    // and the use ACL of the trusted applications.
    std::shared_ptr<Access> new_access(std::string descriptor, std::optional<std::vector<std::string>> trusted) {
        auto access = std::make_shared<Access>();
        access->descriptor = descriptor;
        auto owner = std::make_shared<Acl>();
        owner->role = "owner";
        owner->applications = std::vector<std::string>{};
        owner->description = descriptor;
        auto use = std::make_shared<Acl>();
        use->role = "use";
        use->applications = std::move(trusted);
        use->description = std::move(descriptor);
        access->acls = {std::move(owner), std::move(use)};
        return access;
    }

    std::optional<Value> value_of(Address object) {
        if (!object) return std::nullopt;
        const auto kind = b.cf.kind(object);
        Value value;
        if (kind == ObjectKind::string) { value.kind = Value::Kind::string; value.text = b.text(object); return value; }
        if (kind == ObjectKind::data) { value.kind = Value::Kind::data; value.bytes = b.bytes(object); return value; }
        if (kind == ObjectKind::boolean) { value.kind = Value::Kind::boolean; value.integer = *b.integer(object); return value; }
        if (kind == ObjectKind::date) { value.kind = Value::Kind::date; value.real = b.cf.date_time(object); return value; }
        if (kind == ObjectKind::number) {
            const auto integer = b.integer(object);
            const auto real = b.real(object);
            if (real && integer && static_cast<double>(*integer) != *real) { value.kind = Value::Kind::real; value.real = *real; }
            else { value.kind = Value::Kind::integer; value.integer = integer.value_or(0); }
            return value;
        }
        return std::nullopt;
    }
    Address object_of(const Value& value) {
        switch (value.kind) {
        case Value::Kind::string: return b.string(value.text);
        case Value::Kind::data: return b.data(value.bytes);
        case Value::Kind::integer: return b.number(value.integer);
        case Value::Kind::real: return b.cf.real(value.real);
        case Value::Kind::boolean: return b.retain(b.cf.boolean(value.integer != 0));
        case Value::Kind::date: return b.cf.date(value.real);
        }
        return 0;
    }

    Query parse(Address dictionary, bool for_add) {
        Query query;
        if (!b.is_dictionary(dictionary)) { query.status = status::param; return query; }
        for (const auto& [key_object, value_object] : b.cf.dictionary_entries(dictionary)) {
            if (!b.is_string(key_object)) { query.status = status::param; return query; }
            const auto name = b.text(key_object);
            const auto truthy = [&] { const auto v = b.integer(value_object); return v && *v != 0; };
            if (name == "class") {
                if (!b.is_string(value_object)) { query.status = status::param; return query; }
                query.cls = b.text(value_object);
            } else if (name == "v_Data") {
                if (!b.is_data(value_object)) { query.status = status::param; return query; }
                query.secret = b.bytes(value_object);
            } else if (name == "v_PersistentRef") {
                const auto id = b.is_data(value_object) ? persistent_id(b.bytes(value_object)) : std::nullopt;
                if (!id) { query.status = status::param; return query; }
                query.persistent = *id;
            } else if (name == "v_Ref") {
                query.ref = value_object;
            } else if (name == "r_Data") query.return_data = truthy();
            else if (name == "r_Attributes") query.return_attributes = truthy();
            else if (name == "r_Ref") query.return_ref = truthy();
            else if (name == "r_PersistentRef") query.return_persistent = truthy();
            else if (name == "m_Limit") {
                if (const auto text = b.maybe_text(value_object)) query.limit_all = *text == "m_LimitAll";
                else if (const auto number = b.integer(value_object)) query.limit_all = *number != 1;
            } else if (name == "sync" && b.maybe_text(value_object) == std::optional<std::string>("syna")) {
                query.sync_any = true;
            } else if (name == "m_SearchList" || name == "u_Keychain" || name == "nleg" || name == "u_AuthUI" ||
                       name == "u_OperationPrompt" || name == "u_NoAuthUI" || name == "u_AuthCtx" || name == "pdmn" ||
                       name == "accc" || (name == "m_CaseInsensitive" && !truthy())) {
                // One keychain, no access control lists and no authentication UI: these
                // options select nothing different here.
            } else if (name.starts_with("m_") || name.starts_with("u_") || name.starts_with("r_")) {
                query.status = status::unimplemented;   // other match / use options
                return query;
            } else {
                auto value = value_of(value_object);
                if (!value) { query.status = status::param; return query; }
                query.attributes[name] = std::move(*value);
            }
        }
        if (query.cls.empty() && query.ref) query.cls = ref_class(query.ref);
        if (query.cls.empty()) query.status = status::param;
        static_cast<void>(for_add);
        return query;
    }

    // ---- keys, certificates and identities ----
    // A stored key's own attributes: kSecAttrKeyClass and kSecAttrKeyType hold their constants'
    // values, kSecAttrApplicationLabel the SHA-1 of the public key (as SecKeyCopyAttributes reports).
    static std::map<std::string, Value> key_attributes(const KeyMaterial& key) {
        std::map<std::string, Value> out;
        const bool rsa = key.type == KeyMaterial::Type::rsa;
        out["kcls"] = text_value(key.is_private ? "1" : "0");
        out["type"] = text_value(rsa ? "42" : "73");
        out["bsiz"] = integer_value(key.bits);
        out["esiz"] = integer_value(key.bits);
        out["perm"] = boolean_value(true);
        out["encr"] = boolean_value(rsa && !key.is_private);
        out["decr"] = boolean_value(rsa && key.is_private);
        out["sign"] = boolean_value(key.is_private);
        out["vrfy"] = boolean_value(!key.is_private);
        out["drve"] = boolean_value(!rsa && key.is_private);
        if (const auto label = digest("SHA1", key.public_external())) out["klbl"] = data_value(*label);
        return out;
    }
    // A certificate's attributes: CSSM_CERT_X_509v3 (3), CSSM_CERT_ENCODING_DER (3), the DER names
    // and serial number, and the SHA-1 of the public key (pkhh, its private key's klbl).
    static std::map<std::string, Value> certificate_attributes(const Certificate& certificate) {
        std::map<std::string, Value> out;
        out["ctyp"] = integer_value(3);
        out["cenc"] = integer_value(3);
        out["subj"] = data_value(certificate.subject_sequence());
        out["issr"] = data_value(certificate.issuer_sequence());
        out["slnr"] = data_value(certificate.serial_number());
        out["labl"] = text_value(certificate.subject_summary());
        if (const auto key = key_from_spki(certificate.subject_public_key_info()))
            if (const auto hash = digest("SHA1", key->public_external())) out["pkhh"] = data_value(*hash);
        return out;
    }
    static bool is_text(const Item& item, const char* name, std::string_view value) {
        const auto found = item.attributes.find(name);
        return found != item.attributes.end() && found->second.kind == Value::Kind::string && found->second.text == value;
    }
    static bool extractable(const Item& item) {
        const auto found = item.attributes.find("extr");
        return found == item.attributes.end() || found->second.integer != 0;
    }
    // The key of a key item (nullptr when the stored bytes are not one).
    static KeyPtr item_key(const Item& item) {
        const auto type = is_text(item, "type", "42") ? KeyMaterial::Type::rsa : KeyMaterial::Type::ec;
        return key_from_external(type, is_text(item, "kcls", "1"), item.secret);
    }
    // A key item for `key`: the caller's attributes, then the key's own.
    static Item key_item(const KeyMaterial& key, bool can_extract, std::map<std::string, Value> attributes) {
        Item item;
        item.cls = "keys";
        item.attributes = std::move(attributes);
        if (const auto found = item.attributes.find("extr"); found != item.attributes.end()) can_extract = found->second.integer != 0;
        for (auto& [name, value] : key_attributes(key)) item.attributes[name] = std::move(value);
        item.attributes["extr"] = boolean_value(can_extract);
        item.secret = key.is_private ? key.private_external() : key.public_external();
        return item;
    }
    // A certificate item: its own attributes, the caller's label (and other attributes) over them.
    static Item certificate_item(const Certificate& certificate, const std::map<std::string, Value>& attributes) {
        Item item;
        item.cls = "cert";
        item.attributes = certificate_attributes(certificate);
        for (const auto& [name, value] : attributes)
            if (name != "ctyp" && name != "cenc" && name != "subj" && name != "issr" && name != "slnr" && name != "pkhh") item.attributes[name] = value;
        item.secret = certificate.der();
        return item;
    }
    // The stored private key of a certificate item (its klbl is the certificate's pkhh), or nullptr.
    static const Item* private_key_for(const std::vector<Item>& items, const Item& certificate) {
        const auto hash = certificate.attributes.find("pkhh");
        if (hash == certificate.attributes.end()) return nullptr;
        for (const auto& item : items)
            if (item.cls == "keys" && is_text(item, "kcls", "1"))
                if (const auto label = item.attributes.find("klbl"); label != item.attributes.end() && label->second == hash->second) return &item;
        return nullptr;
    }
    // Adds `item` with a fresh id unless it duplicates a stored one.
    static std::int32_t insert(Store& store, Item& item) {
        item.id = store.next_id;
        item.created = item.modified = CoreFoundation::absolute_time_now();
        for (const auto& other : store.items) if (duplicates(item, other)) return status::duplicate_item;
        ++store.next_id;
        store.items.push_back(item);
        return status::success;
    }

    // The class of an item reference (kSecValueRef): keys, cert or idnt ("" for anything else).
    std::string ref_class(Address ref) {
        if (sec.key_state(ref)) return "keys";
        if (sec.certificate_of(ref)) return "cert";
        if (b.objc.find_host_state<IdentityState>(ref)) return "idnt";
        return {};
    }
    // kSecValueRef as a match value: the reference's identifying attributes.
    std::int32_t match_ref(Query& query) {
        if (!query.ref) return status::success;
        if (const auto key = sec.key_state(query.ref)) {
            auto own = key_attributes(*key->key);
            query.attributes["kcls"] = own["kcls"];
            query.attributes["klbl"] = own["klbl"];
            return query.cls == "keys" ? status::success : status::param;
        }
        auto certificate = sec.certificate_of(query.ref);
        if (const auto identity = certificate ? nullptr : b.objc.find_host_state<IdentityState>(query.ref))
            certificate = sec.certificate_of(identity->certificate);
        if (!certificate || (query.cls != "cert" && query.cls != "idnt")) return status::param;
        query.attributes["issr"] = data_value(certificate->issuer_sequence());
        query.attributes["slnr"] = data_value(certificate->serial_number());
        return status::success;
    }
    Address key_ref(const Item& item) {
        const auto key = item_key(item);
        return key ? sec.make_key(key, extractable(item), true) : 0;
    }
    Address certificate_ref(const Item& item) {
        const auto certificate = Certificate::parse(item.secret);
        return certificate ? sec.make_certificate(certificate) : 0;
    }
    // A +1 SecIdentity of a stored certificate and private key.
    Address identity_ref(const CertificatePtr& certificate, const Item& key) {
        const auto material = item_key(key);
        if (!certificate || !material) return 0;
        const auto object = b.objc.allocate(sec.identity_class);
        auto state = std::make_shared<IdentityState>();
        state->certificate = sec.make_certificate(certificate);
        state->private_key = material;
        state->private_key_extractable = extractable(key);
        static_cast<void>(b.objc.host_state<IdentityState>(object, [&] { return state; }));
        return object;
    }

    std::int32_t add_identity(const Query& query, Address result_out) {
        const auto state = query.ref ? b.objc.find_host_state<IdentityState>(query.ref) : nullptr;
        if (!state) return status::param;
        if (!state->private_key) return status::unimplemented;   // a PKCS#12 import: the host holds its key, which cannot be stored
        const auto certificate = sec.certificate_of(state->certificate);
        if (!certificate) return status::param;
        auto certificate_entry = certificate_item(*certificate, query.attributes);
        auto key_entry = key_item(*state->private_key, state->private_key_extractable, {});
        std::lock_guard lock(process_mutex);
        Store store;
        const auto certificate_status = insert(store, certificate_entry);
        const auto key_status = insert(store, key_entry);
        if (certificate_status && key_status) return status::duplicate_item;
        store.save();
        if (result_out && returns_anything(query)) {
            // The stored halves (a half that was already there is the stored one).
            const auto stored = std::find_if(store.items.begin(), store.items.end(), [&](const Item& item) {
                return item.cls == "cert" && item.secret == certificate_entry.secret;
            });
            const auto stored_key = stored != store.items.end() ? private_key_for(store.items, *stored) : nullptr;
            const auto result = stored_key ? identity_result(*stored, *stored_key, query) : std::nullopt;
            b.memory().write64(result_out, result.value_or(0));
        }
        return status::success;
    }
    bool identity_matches(const Item& certificate, const Item& key, const Query& query) {
        if (query.persistent && certificate.id != *query.persistent) return false;
        for (const auto& [name, value] : query.attributes) {
            const auto mine = certificate.attributes.find(name);
            if (mine != certificate.attributes.end() && mine->second == value) continue;
            const auto theirs = key.attributes.find(name);
            if (theirs != key.attributes.end() && theirs->second == value) continue;
            return false;
        }
        return true;
    }
    // One identity result (+1): its reference, the certificate's attributes or persistent reference;
    // nullopt for data (an identity has none of its own).
    std::optional<Address> identity_result(const Item& certificate, const Item& key, const Query& query) {
        if (query.return_data) return std::nullopt;
        Address ref{};
        if (query.return_ref) {
            ref = identity_ref(Certificate::parse(certificate.secret), key);
            if (!ref) return std::nullopt;
        }
        const int kinds = (query.return_ref ? 1 : 0) + (query.return_attributes ? 1 : 0) + (query.return_persistent ? 1 : 0);
        if (kinds == 1 && query.return_ref) return ref;
        if (kinds == 1 && query.return_persistent) return b.data(persistent_ref(certificate.id));
        std::vector<std::pair<Address, Address>> entries;
        std::vector<Address> owned;
        const auto put = [&](const std::string& name, Address value) {
            const auto name_object = b.string(name);
            owned.push_back(name_object);
            owned.push_back(value);
            entries.emplace_back(name_object, value);
        };
        if (query.return_attributes) {
            put("class", b.string("idnt"));
            for (const auto& [name, value] : certificate.attributes) put(name, object_of(value));
        }
        if (ref) put("v_Ref", ref);
        if (query.return_persistent) put("v_PersistentRef", b.data(persistent_ref(certificate.id)));
        const auto result = b.dictionary(entries);
        b.release_all(owned);
        return result;
    }
    std::int32_t copy_identities(const Query& query, Address result_out) {
        std::vector<std::pair<Item, Item>> found;
        {
            std::lock_guard lock(process_mutex);
            Store store;
            for (const auto& certificate : store.items) {
                if (certificate.cls != "cert") continue;
                const auto key = private_key_for(store.items, certificate);
                if (!key || !identity_matches(certificate, *key, query)) continue;
                found.emplace_back(certificate, *key);
                if (!query.limit_all.value_or(false)) break;
            }
        }
        if (found.empty()) return status::item_not_found;
        if (!result_out || !returns_anything(query)) return status::success;
        std::vector<Address> values;
        for (const auto& [certificate, key] : found) {
            const auto value = identity_result(certificate, key, query);
            if (!value) { b.release_all(values); return status::unimplemented; }
            values.push_back(*value);
        }
        if (!query.limit_all.value_or(false)) { b.memory().write64(result_out, values[0]); return status::success; }
        b.memory().write64(result_out, b.array(values));
        b.release_all(values);
        return status::success;
    }

    // One result value per the r_* flags (+1); nullopt when a return type is unsupported.
    std::optional<Address> result_of(const Item& item, const Query& query) {
        Address ref{};
        if (query.return_ref) {
            ref = item.cls == "keys" ? key_ref(item) : item.cls == "cert" ? certificate_ref(item) : 0;
            if (!ref) return std::nullopt;   // passwords have no reference
        }
        const int kinds = (query.return_data ? 1 : 0) + (query.return_attributes ? 1 : 0) + (query.return_persistent ? 1 : 0) +
            (query.return_ref ? 1 : 0);
        if (kinds == 1 && query.return_ref) return ref;
        if (kinds == 1 && query.return_data) return b.data(item.secret);
        if (kinds == 1 && query.return_persistent) return b.data(persistent_ref(item.id));
        std::vector<std::pair<Address, Address>> entries;
        std::vector<Address> owned;
        const auto put = [&](const std::string& name, Address value) {
            const auto key = b.string(name);
            owned.push_back(key);
            owned.push_back(value);
            entries.emplace_back(key, value);
        };
        if (query.return_attributes) {
            put("class", b.string(item.cls));
            for (const auto& [name, value] : item.attributes) put(name, object_of(value));
            put("cdat", b.cf.date(item.created));
            put("mdat", b.cf.date(item.modified));
        }
        if (query.return_data) put("v_Data", b.data(item.secret));
        if (ref) put("v_Ref", ref);
        if (query.return_persistent) put("v_PersistentRef", b.data(persistent_ref(item.id)));
        const auto result = b.dictionary(entries);
        b.release_all(owned);
        return result;
    }
    bool returns_anything(const Query& query) {
        return query.return_data || query.return_attributes || query.return_ref || query.return_persistent;
    }
    bool supported_class(const std::string& cls) { return cls == "genp" || cls == "inet" || cls == "keys" || cls == "cert"; }

    std::int32_t add(Address attributes, Address result_out) {
        auto query = parse(attributes, true);
        if (query.status) return query.status;
        if (is_locked(keychain_path())) return status::interaction_not_allowed;
        if (query.persistent) return status::param;
        if (query.cls == "idnt") return add_identity(query, result_out);
        if (!supported_class(query.cls)) return status::param;
        Item item;
        if (query.cls == "keys") {
            // kSecValueRef (a SecKey), or kSecValueData with the key's type and class.
            KeyPtr key;
            bool can_extract = true;
            if (const auto state = query.ref ? sec.key_state(query.ref) : nullptr) {
                key = state->key;
                can_extract = state->extractable;
            } else if (query.ref || !query.secret) {
                return status::param;
            } else {
                const auto type = query.attributes.find("type");
                const auto cls = query.attributes.find("kcls");
                const auto is = [](const auto& found, const auto& end, std::string_view text, std::int64_t number) {
                    return found != end && (found->second.text == text || (found->second.kind == Value::Kind::integer && found->second.integer == number));
                };
                const bool rsa = is(type, query.attributes.end(), "42", 42), ec = is(type, query.attributes.end(), "73", 73);
                const bool is_private = is(cls, query.attributes.end(), "1", 1), is_public = is(cls, query.attributes.end(), "0", 0);
                if ((!rsa && !ec) || (!is_private && !is_public)) return status::param;
                key = key_from_external(rsa ? KeyMaterial::Type::rsa : KeyMaterial::Type::ec, is_private, *query.secret);
                if (!key) return status::decode;
            }
            item = key_item(*key, can_extract, query.attributes);
        } else if (query.cls == "cert") {
            // kSecValueRef (a SecCertificate) or kSecValueData (its DER).
            const auto certificate = query.ref ? sec.certificate_of(query.ref) : query.secret ? Certificate::parse(*query.secret) : nullptr;
            if (!certificate) return query.secret && !query.ref ? status::decode : status::param;
            item = certificate_item(*certificate, query.attributes);
        } else {
            if (query.ref) return status::param;
            item.cls = query.cls;
            item.attributes = query.attributes;
            item.secret = query.secret.value_or(Bytes{});
        }
        std::lock_guard lock(process_mutex);
        Store store;
        item.id = store.next_id++;
        item.created = item.modified = CoreFoundation::absolute_time_now();
        for (const auto& other : store.items) if (duplicates(item, other)) return status::duplicate_item;
        std::optional<Address> result;
        if (result_out && returns_anything(query)) {
            result = result_of(item, query);
            if (!result) return status::unimplemented;
        }
        store.items.push_back(item);
        try { store.save(); } catch (...) { if (result) b.release(*result); throw; }
        if (result_out) b.memory().write64(result_out, result.value_or(0));
        return status::success;
    }

    std::int32_t copy_matching(Address dictionary, Address result_out) {
        auto query = parse(dictionary, false);
        if (query.status) return query.status;
        if (is_locked(keychain_path())) return status::interaction_not_allowed;
        if (const auto error = match_ref(query)) return error;
        if (query.cls == "idnt") return copy_identities(query, result_out);
        if (!supported_class(query.cls)) return status::item_not_found;
        std::vector<Item> found;
        {
            std::lock_guard lock(process_mutex);
            Store store;
            for (const auto& item : store.items) {
                if (!matches(item, query)) continue;
                if (query.secret && item.secret != *query.secret) continue;
                found.push_back(item);
                if (!query.limit_all.value_or(false)) break;
            }
        }
        if (found.empty()) return status::item_not_found;
        if (!result_out || !returns_anything(query)) return status::success;
        if (query.return_data)
            for (const auto& item : found)
                if (item.cls == "keys" && !extractable(item)) return status::data_not_available;   // kSecAttrIsExtractable false
        if (!query.limit_all.value_or(false)) {
            const auto value = result_of(found[0], query);
            if (!value) return status::unimplemented;
            b.memory().write64(result_out, *value);
            return status::success;
        }
        std::vector<Address> values;
        for (const auto& item : found) {
            const auto value = result_of(item, query);
            if (!value) { b.release_all(values); return status::unimplemented; }
            values.push_back(*value);
        }
        b.memory().write64(result_out, b.array(values));
        b.release_all(values);
        return status::success;
    }

    std::int32_t update(Address dictionary, Address changes) {
        auto query = parse(dictionary, false);
        if (query.status) return query.status;
        auto change = parse_changes(changes);
        if (change.status) return change.status;
        if (const auto error = match_ref(query)) return error;
        if (query.cls == "idnt") return status::unimplemented;   // an identity is its certificate and key: update them
        if (!supported_class(query.cls)) return status::item_not_found;
        if ((query.cls == "keys" || query.cls == "cert") && change.secret) return status::param;   // their bytes are what they are
        std::lock_guard lock(process_mutex);
        Store store;
        bool any = false;
        const auto now = CoreFoundation::absolute_time_now();
        for (auto& item : store.items) {
            if (!matches(item, query)) continue;
            any = true;
            for (const auto& [name, value] : change.attributes) item.attributes[name] = value;
            if (change.secret) item.secret = *change.secret;
            item.modified = now;
        }
        if (!any) return status::item_not_found;
        for (std::size_t i = 0; i < store.items.size(); ++i)
            for (std::size_t j = i + 1; j < store.items.size(); ++j)
                if (duplicates(store.items[i], store.items[j])) return status::duplicate_item;
        store.save();
        return status::success;
    }
    Query parse_changes(Address changes) {
        Query change;
        if (!b.is_dictionary(changes)) { change.status = status::param; return change; }
        for (const auto& [key_object, value_object] : b.cf.dictionary_entries(changes)) {
            if (!b.is_string(key_object)) { change.status = status::param; return change; }
            const auto name = b.text(key_object);
            if (name == "class" || name.starts_with("r_") || name.starts_with("m_") || name == "v_PersistentRef" || name == "v_Ref") {
                change.status = status::param;
                return change;
            }
            if (name == "v_Data") {
                if (!b.is_data(value_object)) { change.status = status::param; return change; }
                change.secret = b.bytes(value_object);
                continue;
            }
            if (name.starts_with("u_") || name == "nleg") continue;
            auto value = value_of(value_object);
            if (!value) { change.status = status::param; return change; }
            change.attributes[name] = std::move(*value);
        }
        return change;
    }

    std::int32_t remove(Address dictionary) {
        auto query = parse(dictionary, false);
        if (query.status) return query.status;
        if (const auto error = match_ref(query)) return error;
        if (query.cls == "idnt") {
            // An identity goes with its certificate and private key.
            std::lock_guard lock(process_mutex);
            Store store;
            std::set<std::uint64_t> doomed;
            for (const auto& certificate : store.items) {
                if (certificate.cls != "cert") continue;
                const auto key = private_key_for(store.items, certificate);
                if (!key || !identity_matches(certificate, *key, query)) continue;
                doomed.insert(certificate.id);
                doomed.insert(key->id);
            }
            if (doomed.empty()) return status::item_not_found;
            std::erase_if(store.items, [&](const Item& item) { return doomed.contains(item.id); });
            store.save();
            return status::success;
        }
        if (!supported_class(query.cls)) return status::item_not_found;
        std::lock_guard lock(process_mutex);
        Store store;
        const auto before = store.items.size();
        std::erase_if(store.items, [&](const Item& item) { return matches(item, query); });
        if (store.items.size() == before) return status::item_not_found;
        store.save();
        return status::success;
    }

    // ---- SecKeychain generic passwords -------------------------------------------------------------
    std::string guest_text(GuestMemory& m, Address pointer, Address length) {
        if (!pointer || !length) return {};
        if (length > (1U << 20)) fail("keychain attribute too long");
        Bytes bytes(static_cast<std::size_t>(length));
        m.read_bytes(pointer, bytes);
        return std::string(bytes.begin(), bytes.end());
    }
    Address item_ref(std::uint64_t id, const std::filesystem::path& keychain = {}) {
        const auto object = b.objc.allocate(item_class);
        auto state = std::make_shared<KeychainItemState>();
        state->id = id;
        state->keychain = keychain;
        static_cast<void>(b.objc.host_state<KeychainItemState>(object, [&] { return state; }));
        return object;
    }
    std::optional<std::uint64_t> item_id(Address object) {
        const auto state = object ? b.objc.find_host_state<KeychainItemState>(object) : nullptr;
        if (!state) return std::nullopt;
        return state->id;
    }
    std::filesystem::path item_keychain(Address object) {
        const auto state = object ? b.objc.find_host_state<KeychainItemState>(object) : nullptr;
        return state && !state->keychain.empty() ? state->keychain : keychain_path();
    }

    // ---- keychains ---------------------------------------------------------------------------------------
    std::optional<std::filesystem::path> default_override;
    std::vector<std::filesystem::path> search_list;   // empty: the default keychain alone

    std::filesystem::path default_path() { return default_override.value_or(keychain_path()); }
    // The host path of a SecKeychainRef (nullopt when it is not one).
    std::optional<std::filesystem::path> path_of(Address object) {
        const auto state = object ? b.objc.find_host_state<KeychainState>(object) : nullptr;
        if (!state) return std::nullopt;
        return state->path.empty() ? default_path() : state->path;
    }
    Address keychain_object(const std::filesystem::path& path) {   // +1
        const auto object = b.objc.allocate(keychain_class);
        auto state = std::make_shared<KeychainState>();
        state->path = path;
        static_cast<void>(b.objc.host_state<KeychainState>(object, [&] { return state; }));
        return object;
    }
    std::vector<std::filesystem::path> searched() { return search_list.empty() ? std::vector{default_path()} : search_list; }
    // keychainOrArray: NULL (the search list), a keychain, or an array of keychains. nullopt for anything else.
    std::optional<std::vector<std::filesystem::path>> keychains_of(Address value) {
        if (!value) return searched();
        if (const auto path = path_of(value)) return std::vector{*path};
        if (!b.is_array(value)) return std::nullopt;
        std::vector<std::filesystem::path> paths;
        for (const auto item : b.cf.array_values(value)) {
            const auto path = path_of(item);
            if (!path) return std::nullopt;
            paths.push_back(*path);
        }
        return paths;
    }
    // A guest path to the host (the Alchemy root, /Volumes/<drive>/, else as given).
    static std::filesystem::path host_of(const std::string& guest) {
        if (guest.size() > 10 && guest.starts_with("/Volumes/") && guest[10] == '/') return utf8_path(std::string(1, guest[9]) + ":" + guest.substr(10));
        if (auto mapped = guest_root_host_path(guest)) return *mapped;
        return utf8_path(guest);
    }
    std::string guest_c_string(GuestMemory& m, Address pointer) {
        std::string text;
        for (std::size_t index = 0; pointer && index < 4096; ++index) {
            const auto c = static_cast<char>(m.read8(pointer + index));
            if (!c) break;
            text += c;
        }
        return text;
    }

    // ---- attribute lists ---------------------------------------------------------------------------------
    // SecKeychainAttributeList {UInt32 count; SecKeychainAttribute *attr}; SecKeychainAttribute {tag; UInt32 length; void *data}.
    std::optional<std::map<std::string, Value>> read_attributes(GuestMemory& m, Address list) {
        std::map<std::string, Value> attributes;
        if (!list) return attributes;
        const auto count = m.read32(list);
        const auto array = m.read64(list + 8);
        if (count > 256 || (count && !array)) return std::nullopt;
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto entry = array + index * 16ULL;
            const auto tag = four_cc(m.read32(entry));
            const auto length = m.read32(entry + 4);
            const auto data = m.read64(entry + 8);
            if (length > (1U << 20) || (length && !data)) return std::nullopt;
            Bytes bytes(length);
            if (length) m.read_bytes(data, bytes);
            Value value;
            switch (tag_kind(tag)) {
            case TagKind::text: case TagKind::date:
                value.kind = Value::Kind::string;
                value.text.assign(bytes.begin(), bytes.end());
                while (!value.text.empty() && value.text.back() == 0) value.text.pop_back();
                if (tag_kind(tag) == TagKind::date) continue;   // creation / modification dates are the store's own
                break;
            case TagKind::blob: value.kind = Value::Kind::data; value.bytes = bytes; break;
            case TagKind::number: case TagKind::boolean: case TagKind::four_char_text: {
                std::uint32_t number = 0;
                for (std::size_t at = 0; at < std::min<std::size_t>(4, bytes.size()); ++at) number |= static_cast<std::uint32_t>(bytes[at]) << (8 * at);
                if (tag_kind(tag) == TagKind::four_char_text) { value.kind = Value::Kind::string; value.text = four_cc(number); }
                else { value.kind = tag_kind(tag) == TagKind::boolean ? Value::Kind::boolean : Value::Kind::integer; value.integer = number; }
                break;
            }
            }
            attributes[tag] = std::move(value);
        }
        return attributes;
    }
    // The legacy bytes of one attribute of an item (nullopt when the item has none).
    std::optional<Bytes> attribute_bytes(const Item& item, const std::string& tag) {
        if (tag == "cdat" || tag == "mdat") {
            const auto text = legacy_date(tag == "cdat" ? item.created : item.modified);
            Bytes bytes(text.begin(), text.end());
            bytes.push_back(0);
            return bytes;
        }
        const auto found = item.attributes.find(tag);
        if (found == item.attributes.end()) return std::nullopt;
        const auto& value = found->second;
        const auto number_bytes = [](std::uint32_t number) {
            return Bytes{static_cast<std::uint8_t>(number), static_cast<std::uint8_t>(number >> 8), static_cast<std::uint8_t>(number >> 16),
                         static_cast<std::uint8_t>(number >> 24)};
        };
        switch (tag_kind(tag)) {
        case TagKind::four_char_text: return number_bytes(four_cc(value.text));
        case TagKind::number: case TagKind::boolean: return number_bytes(static_cast<std::uint32_t>(value.integer));
        default:
            if (value.kind == Value::Kind::data) return value.bytes;
            return Bytes(value.text.begin(), value.text.end());
        }
    }
    // Guest allocations handed out for attribute lists, freed by SecKeychainItemFreeAttributesAndData.
    std::shared_ptr<std::mutex> allocations_mutex = std::make_shared<std::mutex>();   // (the struct is copied once)
    std::map<Address, std::vector<Address>> list_allocations;
    Address guest_copy(std::span<const std::uint8_t> bytes) {
        const auto address = b.process.allocate_guest(std::max<std::size_t>(bytes.size(), 1));
        if (!bytes.empty()) b.memory().write_bytes(address, bytes);
        return address;
    }

    // ---- store access --------------------------------------------------------------------------------------
    // Runs `work` on the store of `path`; errSecNoSuchKeychain when the file does not exist (other than the
    // default keychain), errSecInteractionNotAllowed when this process locked it.
    template <class Work>
    std::int32_t with_store(const std::filesystem::path& path, bool must_exist, Work work) {
        if (is_locked(path)) return status::interaction_not_allowed;
        std::lock_guard lock(process_mutex);
        Store store(path);
        if (must_exist && !store.existed && path != default_path() && path != keychain_path()) return err_no_such_keychain;
        return work(store);
    }
    bool legacy_class(std::uint32_t code, std::string& cls) {
        cls = four_cc(code);
        return cls == "genp" || cls == "inet";
    }
    bool matches_all(const Item& item, const std::string& cls, const std::map<std::string, Value>& attributes) {
        if (item.cls != cls) return false;
        for (const auto& [name, value] : attributes)
            if (!(primary_value(item, name) == value)) return false;
        return true;
    }
    // The first item (by id after `after`) in `paths` that matches; nullopt when none does.
    std::optional<std::pair<Item, std::filesystem::path>> find_first(const std::vector<std::filesystem::path>& paths, const std::string& cls,
                                                                    const std::map<std::string, Value>& attributes, std::int32_t& error) {
        error = 0;
        for (const auto& path : paths) {
            std::optional<Item> found;
            const auto status = with_store(path, true, [&](Store& store) {
                for (const auto& item : store.items)
                    if (matches_all(item, cls, attributes)) { found = item; break; }
                return status::success;
            });
            if (status == status::interaction_not_allowed) { error = status; return std::nullopt; }
            if (found) return std::make_pair(*found, path);
        }
        return std::nullopt;
    }
    // Adds an item to `path`'s keychain; its id, or the error.
    std::int32_t add_item(const std::filesystem::path& path, Item item, std::uint64_t& id) {
        return with_store(path, true, [&](Store& store) {
            item.id = store.next_id++;
            item.created = item.modified = CoreFoundation::absolute_time_now();
            for (const auto& other : store.items) if (duplicates(item, other)) return status::duplicate_item;
            store.items.push_back(item);
            store.save();
            id = item.id;
            return status::success;
        });
    }
    std::optional<Item> item_of(Address object, std::int32_t& error) {
        const auto id = item_id(object);
        if (!id) { error = status::param; return std::nullopt; }
        std::optional<Item> found;
        error = with_store(item_keychain(object), true, [&](Store& store) {
            for (const auto& item : store.items) if (item.id == *id) { found = item; return status::success; }
            return status::item_not_found;
        });
        return found;
    }
    // Writes a found password: *length, *data (a guest copy, freed by SecKeychainItemFreeContent).
    void write_secret(GuestMemory& m, const Item& item, Address length_out, Address data_out) {
        if (length_out) m.write32(length_out, static_cast<std::uint32_t>(item.secret.size()));
        if (data_out) m.write64(data_out, guest_copy(item.secret));
    }
};

Address status_value(std::int32_t value) { return static_cast<Address>(static_cast<std::int64_t>(value)); }

} // namespace

void install_keychain(Security& sec) {
    auto& b = *sec.b;
    auto self = std::make_shared<Keychain>(Keychain{sec, b});
    self->item_class = b.define_class("_AlchemySecKeychainItem", b.object_class, lib);
    self->keychain_class = b.define_class("_AlchemySecKeychain", b.object_class, lib);
    b.register_type_id(self->item_class, TypeId::keychain_item);
    b.register_type_id(self->keychain_class, TypeId::keychain);

    const std::pair<const char*, const char*> constants[] = {
        {"kSecClass", "class"}, {"kSecClassGenericPassword", "genp"}, {"kSecClassInternetPassword", "inet"},
        {"kSecClassCertificate", "cert"}, {"kSecClassKey", "keys"}, {"kSecClassIdentity", "idnt"},
        {"kSecAttrService", "svce"}, {"kSecAttrAccount", "acct"}, {"kSecAttrServer", "srvr"}, {"kSecAttrProtocol", "ptcl"},
        {"kSecAttrPort", "port"}, {"kSecAttrPath", "path"}, {"kSecAttrAuthenticationType", "atyp"},
        {"kSecAttrSecurityDomain", "sdmn"}, {"kSecAttrLabel", "labl"}, {"kSecAttrDescription", "desc"},
        {"kSecAttrComment", "icmt"}, {"kSecAttrCreator", "crtr"}, {"kSecAttrType", "type"}, {"kSecAttrGeneric", "gena"},
        {"kSecAttrCreationDate", "cdat"}, {"kSecAttrModificationDate", "mdat"}, {"kSecAttrAccessGroup", "agrp"},
        {"kSecAttrAccessible", "pdmn"}, {"kSecAttrAccessControl", "accc"}, {"kSecAttrSynchronizable", "sync"},
        {"kSecAttrSynchronizableAny", "syna"}, {"kSecAttrIsInvisible", "invi"}, {"kSecAttrIsNegative", "nega"},
        {"kSecAttrAccessibleWhenUnlocked", "ak"}, {"kSecAttrAccessibleAfterFirstUnlock", "ck"},
        {"kSecAttrAccessibleAlways", "dk"}, {"kSecAttrAccessibleWhenUnlockedThisDeviceOnly", "aku"},
        {"kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly", "cku"}, {"kSecAttrAccessibleWhenPasscodeSetThisDeviceOnly", "akpu"},
        {"kSecAttrProtocolHTTP", "http"}, {"kSecAttrProtocolHTTPS", "htps"}, {"kSecAttrProtocolFTP", "ftp "},
        {"kSecAttrProtocolHTTPProxy", "htpx"}, {"kSecAttrProtocolHTTPSProxy", "htsx"}, {"kSecAttrProtocolFTPProxy", "ftpx"},
        {"kSecAttrProtocolSOCKS", "sox "},
        {"kSecAttrAuthenticationTypeDefault", "dflt"}, {"kSecAttrAuthenticationTypeHTTPBasic", "http"},
        {"kSecAttrAuthenticationTypeHTMLForm", "form"}, {"kSecReturnData", "r_Data"}, {"kSecReturnAttributes", "r_Attributes"},
        {"kSecReturnRef", "r_Ref"}, {"kSecReturnPersistentRef", "r_PersistentRef"}, {"kSecMatchLimit", "m_Limit"},
        {"kSecMatchLimitOne", "m_LimitOne"}, {"kSecMatchLimitAll", "m_LimitAll"}, {"kSecMatchSearchList", "m_SearchList"},
        {"kSecMatchCaseInsensitive", "m_CaseInsensitive"}, {"kSecUseDataProtectionKeychain", "nleg"},
        {"kSecUseKeychain", "u_Keychain"}, {"kSecUseAuthenticationUI", "u_AuthUI"}, {"kSecUseOperationPrompt", "u_OperationPrompt"},
        {"kSecUseAuthenticationUIAllow", "u_AuthUIA"}, {"kSecUseAuthenticationUIFail", "u_AuthUIF"},
        {"kSecUseAuthenticationUISkip", "u_AuthUIS"},
        // Certificate attributes (Apple's open-source SecItemConstants values).
        {"kSecAttrCertificateType", "ctyp"}, {"kSecAttrCertificateEncoding", "cenc"}, {"kSecAttrSubject", "subj"},
        {"kSecAttrIssuer", "issr"}, {"kSecAttrSerialNumber", "slnr"}, {"kSecAttrPublicKeyHash", "pkhh"}};
    for (const auto& [name, value] : constants) sec.export_constant(name, value);

    b.function(lib, "_SecItemAdd", [self](CpuState& s, GuestMemory&) { s.x[0] = status_value(self->add(s.x[0], s.x[1])); });
    b.function(lib, "_SecItemCopyMatching", [self](CpuState& s, GuestMemory& m) {
        if (s.x[1]) m.write64(s.x[1], 0);
        s.x[0] = status_value(self->copy_matching(s.x[0], s.x[1]));
    });
    b.function(lib, "_SecItemUpdate", [self](CpuState& s, GuestMemory&) { s.x[0] = status_value(self->update(s.x[0], s.x[1])); });
    b.function(lib, "_SecItemDelete", [self](CpuState& s, GuestMemory&) { s.x[0] = status_value(self->remove(s.x[0])); });

    // Keychain storage of generated keys (SecKeyCreateRandomKey / SecKeyGeneratePair with kSecAttrIsPermanent).
    sec.store_key = [self](const KeyPtr& key, Address parameters, Address nested) -> std::int32_t {
        if (is_locked(keychain_path())) return status::interaction_not_allowed;
        constexpr std::pair<const char*, const char*> kept[] = {{"kSecAttrLabel", "labl"}, {"kSecAttrApplicationTag", "atag"},
            {"kSecAttrIsExtractable", "extr"}, {"kSecAttrIsSensitive", "sens"}, {"kSecAttrAccessGroup", "agrp"}};
        std::map<std::string, Value> attributes;
        for (const auto dictionary : {parameters, nested})   // the nested dictionary's last: it wins
            if (dictionary)
                for (const auto& [name, tag] : kept)
                    if (const auto value = self->value_of(self->sec.get(dictionary, name))) attributes[tag] = *value;
        auto item = Keychain::key_item(*key, true, std::move(attributes));
        std::lock_guard lock(process_mutex);
        Store store;
        if (const auto error = Keychain::insert(store, item)) return error;
        store.save();
        return status::success;
    };
    // OSStatus SecIdentityCreateWithCertificate(CFTypeRef keychainOrArray, SecCertificateRef, SecIdentityRef *):
    // the certificate with its stored private key (there is one keychain).
    b.function(lib, "_SecIdentityCreateWithCertificate", [self](CpuState& s, GuestMemory& m) {
        const auto certificate = self->sec.certificate_of(s.x[1]);
        if (!certificate || !s.x[2]) { s.x[0] = status_value(status::param); return; }
        if (is_locked(keychain_path())) { s.x[0] = status_value(status::interaction_not_allowed); return; }
        const auto entry = Keychain::certificate_item(*certificate, {});
        std::lock_guard lock(process_mutex);
        Store store;
        const auto key = Keychain::private_key_for(store.items, entry);
        if (!key) { s.x[0] = status_value(status::item_not_found); return; }
        m.write64(s.x[2], self->identity_ref(certificate, *key));
        s.x[0] = 0;
    });
    // SecIdentityRef SecIdentityCreate(CFAllocatorRef, SecCertificateRef, SecKeyRef privateKey): an identity of the
    // certificate and the private key of its public key; NULL when the key is not a private key or not the
    // certificate's (compared as the keychain pairs them, by the public key).
    b.function(lib, "_SecIdentityCreate", [self](CpuState& s, GuestMemory&) {
        const auto certificate = self->sec.certificate_of(s.x[1]);
        const auto key = s.x[2] ? self->sec.key_of(s.x[2]) : nullptr;
        const auto certified = certificate ? key_from_spki(certificate->subject_public_key_info()) : nullptr;
        if (!certificate || !key || !key->is_private || !certified || certified->public_external() != key->public_external()) {
            s.x[0] = 0;
            return;
        }
        const auto object = self->b.objc.allocate(self->sec.identity_class);
        auto state = std::make_shared<IdentityState>();
        state->certificate = self->sec.make_certificate(certificate);
        state->private_key = key;
        const auto key_state = self->b.objc.find_host_state<KeyState>(s.x[2]);
        state->private_key_extractable = !key_state || key_state->extractable;
        static_cast<void>(self->b.objc.host_state<IdentityState>(object, [&] { return state; }));
        s.x[0] = object;
    });
    // Identity preferences: a record of the keychain naming the identity's certificate (issuer and
    // serial number) under `name`; keyUsage is not distinguished.
    // OSStatus SecIdentitySetPreferred(SecIdentityRef identity, CFStringRef name, CFArrayRef keyUsage): NULL removes it.
    b.function(lib, "_SecIdentitySetPreferred", [self](CpuState& s, GuestMemory&) {
        if (!s.x[1] || !self->b.is_string(s.x[1])) { s.x[0] = status_value(status::param); return; }
        CertificatePtr certificate;
        if (s.x[0]) {
            const auto state = self->b.objc.find_host_state<IdentityState>(s.x[0]);
            certificate = state ? self->sec.certificate_of(state->certificate) : nullptr;
            if (!certificate) { s.x[0] = status_value(status::param); return; }
        }
        if (is_locked(keychain_path())) { s.x[0] = status_value(status::interaction_not_allowed); return; }
        const auto name = self->b.text(s.x[1]);
        std::lock_guard lock(process_mutex);
        Store store;
        std::erase_if(store.items, [&](const Item& item) { return item.cls == "iprf" && Keychain::is_text(item, "svce", name); });
        if (certificate) {
            Item preference;
            preference.cls = "iprf";
            preference.attributes["svce"] = text_value(name);
            preference.attributes["issr"] = data_value(certificate->issuer_sequence());
            preference.attributes["slnr"] = data_value(certificate->serial_number());
            if (const auto error = Keychain::insert(store, preference)) { s.x[0] = status_value(error); return; }
        }
        store.save();
        s.x[0] = 0;
    });
    // SecIdentityRef SecIdentityCopyPreferred(CFStringRef name, CFArrayRef keyUsage, CFArrayRef validIssuers):
    // the preferred identity while its certificate and private key are stored (with validIssuers, only when
    // its issuer's DER name is one of them); NULL otherwise.
    b.function(lib, "_SecIdentityCopyPreferred", [self](CpuState& s, GuestMemory&) {
        const auto name_object = s.x[0], issuers = s.x[2];
        s.x[0] = 0;
        if (!name_object || !self->b.is_string(name_object) || is_locked(keychain_path())) return;
        const auto name = self->b.text(name_object);
        std::lock_guard lock(process_mutex);
        Store store;
        const auto preference = std::find_if(store.items.begin(), store.items.end(), [&](const Item& item) {
            return item.cls == "iprf" && Keychain::is_text(item, "svce", name);
        });
        if (preference == store.items.end()) return;
        const auto certificate = std::find_if(store.items.begin(), store.items.end(), [&](const Item& item) {
            return item.cls == "cert" && item.attributes.contains("issr") && item.attributes.contains("slnr") &&
                item.attributes.at("issr") == preference->attributes.at("issr") && item.attributes.at("slnr") == preference->attributes.at("slnr");
        });
        if (certificate == store.items.end()) return;
        const auto key = Keychain::private_key_for(store.items, *certificate);
        if (!key) return;
        if (issuers && self->b.is_array(issuers) && !self->b.cf.array_values(issuers).empty()) {
            const auto& issuer = certificate->attributes.at("issr").bytes;
            const auto values = self->b.cf.array_values(issuers);
            if (std::none_of(values.begin(), values.end(), [&](Address value) { return self->b.is_data(value) && self->b.bytes(value) == issuer; })) return;
        }
        s.x[0] = self->identity_ref(Certificate::parse(certificate->secret), *key);
    });

    // Legacy keychain API (generic passwords in the one keychain).
    b.function(lib, "_SecKeychainGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(TypeId::keychain); });
    b.function(lib, "_SecKeychainItemGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(TypeId::keychain_item); });
    b.function(lib, "_SecKeychainCopyDefault", [self](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) { s.x[0] = status_value(status::param); return; }
        if (self->default_override) { m.write64(s.x[0], self->keychain_object(*self->default_override)); s.x[0] = 0; return; }
        if (!self->default_keychain) self->default_keychain = self->keychain_object({});
        m.write64(s.x[0], self->b.retain(self->default_keychain));
        s.x[0] = 0;
    });
    // OSStatus SecKeychainAddGenericPassword(keychain, serviceNameLength, serviceName,
    //     accountNameLength, accountName, passwordLength, passwordData, itemRef *)
    b.function(lib, "_SecKeychainAddGenericPassword", [self](CpuState& s, GuestMemory& m) {
        Item item;
        item.cls = "genp";
        item.attributes["svce"] = Value{Value::Kind::string, self->guest_text(m, s.x[2], s.x[1])};
        item.attributes["acct"] = Value{Value::Kind::string, self->guest_text(m, s.x[4], s.x[3])};
        if (s.x[5] > (1U << 24)) { s.x[0] = status_value(status::param); return; }
        item.secret.resize(static_cast<std::size_t>(s.x[5]));
        if (!item.secret.empty()) m.read_bytes(s.x[6], item.secret);
        const auto out = s.x[7];
        const auto path = s.x[0] ? self->path_of(s.x[0]) : std::optional(self->default_path());
        if (!path) { s.x[0] = status_value(status::param); return; }
        std::uint64_t id{};
        if (const auto status = self->add_item(*path, std::move(item), id)) { s.x[0] = status_value(status); return; }
        if (out) m.write64(out, self->item_ref(id, *path));
        s.x[0] = 0;
    });
    // OSStatus SecKeychainFindGenericPassword(keychainOrArray, serviceNameLength, serviceName,
    //     accountNameLength, accountName, UInt32 *passwordLength, void **passwordData, itemRef *)
    b.function(lib, "_SecKeychainFindGenericPassword", [self](CpuState& s, GuestMemory& m) {
        const auto keychains = self->keychains_of(s.x[0]);
        if (!keychains) { s.x[0] = status_value(status::param); return; }
        std::map<std::string, Value> attributes;
        if (const auto service = self->guest_text(m, s.x[2], s.x[1]); !service.empty()) attributes["svce"] = Value{Value::Kind::string, service};
        if (const auto account = self->guest_text(m, s.x[4], s.x[3]); !account.empty()) attributes["acct"] = Value{Value::Kind::string, account};
        std::int32_t error{};
        const auto found = self->find_first(*keychains, "genp", attributes, error);
        if (!found) { s.x[0] = status_value(error ? error : status::item_not_found); return; }
        self->write_secret(m, found->first, s.x[5], s.x[6]);
        if (s.x[7]) m.write64(s.x[7], self->item_ref(found->first.id, found->second));
        s.x[0] = 0;
    });
    // (SecKeychainAttributeList *attrList, void *data): the attribute data SecKeychainItemCopyContent filled in, and the password.
    b.function(lib, "_SecKeychainItemFreeContent", [self](CpuState& s, GuestMemory& m) {
        if (s.x[0]) {
            const auto count = m.read32(s.x[0]);
            const auto array = m.read64(s.x[0] + 8);
            for (std::uint32_t index = 0; index < count && array; ++index)
                if (const auto data = m.read64(array + index * 16ULL + 8)) {
                    self->b.process.release_guest(data);
                    m.write64(array + index * 16ULL + 8, 0);
                }
        }
        if (s.x[1]) self->b.process.release_guest(s.x[1]);
        s.x[0] = 0;
    });
    b.function(lib, "_SecKeychainItemDelete", [self](CpuState& s, GuestMemory&) {
        const auto id = self->item_id(s.x[0]);
        if (!id) { s.x[0] = status_value(status::param); return; }
        s.x[0] = status_value(self->with_store(self->item_keychain(s.x[0]), true, [&](Store& store) {
            const auto before = store.items.size();
            std::erase_if(store.items, [&](const Item& item) { return item.id == *id; });
            if (store.items.size() == before) return status::item_not_found;
            store.save();
            return status::success;
        }));
    });
    // (itemRef, const SecKeychainAttributeList *attrList, UInt32 length, const void *data)
    b.function(lib, "_SecKeychainItemModifyAttributesAndData", [self](CpuState& s, GuestMemory& m) {
        const auto id = self->item_id(s.x[0]);
        if (!id) { s.x[0] = status_value(status::param); return; }
        const auto attributes = self->read_attributes(m, s.x[1]);
        if (!attributes || s.x[2] > (1U << 24)) { s.x[0] = status_value(status::param); return; }
        Bytes secret(static_cast<std::size_t>(s.x[2]));
        if (!secret.empty()) m.read_bytes(s.x[3], secret);
        const bool set_secret = s.x[3] != 0;
        s.x[0] = status_value(self->with_store(self->item_keychain(s.x[0]), true, [&](Store& store) {
            for (auto& item : store.items) {
                if (item.id != *id) continue;
                const auto before = item;
                for (const auto& [name, value] : *attributes) item.attributes[name] = value;
                if (set_secret) item.secret = secret;
                item.modified = CoreFoundation::absolute_time_now();
                for (const auto& other : store.items)
                    if (duplicates(item, other)) { item = before; return status::duplicate_item; }
                store.save();
                return status::success;
            }
            return status::item_not_found;
        }));
    });

    // ---- internet passwords ----
    // (keychain, serverNameLength, serverName, securityDomainLength, securityDomain, accountNameLength, accountName,
    //  pathLength, [stack] path, UInt16 port, SecProtocolType, SecAuthenticationType, UInt32 passwordLength,
    //  const void *passwordData, SecKeychainItemRef *): the stack arguments at their natural alignment.
    const auto internet_attributes = [self](CpuState& s, GuestMemory& m) {
        std::map<std::string, Value> attributes;
        const auto text = [&](const char* tag, Address pointer, Address length) {
            if (const auto value = self->guest_text(m, pointer, length); !value.empty()) attributes[tag] = Value{Value::Kind::string, value};
        };
        text("srvr", s.x[2], s.x[1]);
        text("sdmn", s.x[4], s.x[3]);
        text("acct", s.x[6], s.x[5]);
        text("path", m.read64(s.sp), s.x[7]);
        if (const auto port = m.read32(s.sp + 8) & 0xffff) { Value value; value.kind = Value::Kind::integer; value.integer = port; attributes["port"] = value; }
        if (const auto protocol = m.read32(s.sp + 12)) attributes["ptcl"] = Value{Value::Kind::string, four_cc(protocol)};
        if (const auto type = m.read32(s.sp + 16)) attributes["atyp"] = Value{Value::Kind::string, four_cc(type)};
        return attributes;
    };
    b.function(lib, "_SecKeychainAddInternetPassword", [self, internet_attributes](CpuState& s, GuestMemory& m) {
        Item item;
        item.cls = "inet";
        item.attributes = internet_attributes(s, m);
        const auto length = m.read32(s.sp + 20);
        const auto data = m.read64(s.sp + 24), out = m.read64(s.sp + 32);
        if (length > (1U << 24) || (length && !data)) { s.x[0] = status_value(status::param); return; }
        item.secret.resize(length);
        if (length) m.read_bytes(data, item.secret);
        const auto path = s.x[0] ? self->path_of(s.x[0]) : std::optional(self->default_path());
        if (!path) { s.x[0] = status_value(status::param); return; }
        std::uint64_t id{};
        if (const auto status = self->add_item(*path, std::move(item), id)) { s.x[0] = status_value(status); return; }
        if (out) m.write64(out, self->item_ref(id, *path));
        s.x[0] = 0;
    });
    // As SecKeychainAddInternetPassword, with [stack] UInt32 *passwordLength (sp + 24), void **passwordData, itemRef *.
    // Empty strings, port 0, protocol 0 and authentication type 0 match any value.
    b.function(lib, "_SecKeychainFindInternetPassword", [self, internet_attributes](CpuState& s, GuestMemory& m) {
        const auto keychains = self->keychains_of(s.x[0]);
        if (!keychains) { s.x[0] = status_value(status::param); return; }
        std::int32_t error{};
        const auto found = self->find_first(*keychains, "inet", internet_attributes(s, m), error);
        if (!found) { s.x[0] = status_value(error ? error : status::item_not_found); return; }
        self->write_secret(m, found->first, m.read64(s.sp + 24), m.read64(s.sp + 32));
        if (const auto out = m.read64(s.sp + 40)) m.write64(out, self->item_ref(found->first.id, found->second));
        s.x[0] = 0;
    });

    // ---- attribute lists ----
    // (itemRef, SecKeychainAttributeInfo *info, SecItemClass *itemClass, SecKeychainAttributeList **attrList,
    //  UInt32 *length, void **outData). info {UInt32 count; UInt32 *tag; UInt32 *format}; NULL: every attribute.
    b.function(lib, "_SecKeychainItemCopyAttributesAndData", [self](CpuState& s, GuestMemory& m) {
        std::int32_t error{};
        const auto item = self->item_of(s.x[0], error);
        if (!item) { s.x[0] = status_value(error); return; }
        std::vector<std::string> tags;
        if (s.x[1]) {
            const auto count = m.read32(s.x[1]);
            const auto list = m.read64(s.x[1] + 8);
            if (count > 256 || (count && !list)) { s.x[0] = status_value(status::param); return; }
            for (std::uint32_t index = 0; index < count; ++index) tags.push_back(four_cc(m.read32(list + index * 4ULL)));
        } else {
            for (const auto& [name, value] : item->attributes) if (name.size() == 4) tags.push_back(name);
            tags.push_back("cdat");
            tags.push_back("mdat");
        }
        if (s.x[2]) m.write32(s.x[2], four_cc(item->cls));
        if (s.x[3]) {
            std::vector<Address> allocations;
            const auto array = self->b.process.allocate_guest(std::max<std::size_t>(tags.size() * 16, 16));
            allocations.push_back(array);
            for (std::size_t index = 0; index < tags.size(); ++index) {
                const auto entry = array + index * 16;
                const auto bytes = self->attribute_bytes(*item, tags[index]);
                m.write32(entry, four_cc(tags[index]));
                m.write32(entry + 4, bytes ? static_cast<std::uint32_t>(bytes->size()) : 0);
                Address data{};
                if (bytes && !bytes->empty()) { data = self->guest_copy(*bytes); allocations.push_back(data); }
                m.write64(entry + 8, data);
            }
            const auto list = self->b.process.allocate_guest(16);
            m.write32(list, static_cast<std::uint32_t>(tags.size()));
            m.write64(list + 8, array);
            {
                std::lock_guard lock(*self->allocations_mutex);
                self->list_allocations[list] = std::move(allocations);
            }
            m.write64(s.x[3], list);
        }
        self->write_secret(m, *item, s.x[4], s.x[5]);
        s.x[0] = 0;
    });
    // (SecKeychainAttributeList *attrList, void *data)
    b.function(lib, "_SecKeychainItemFreeAttributesAndData", [self](CpuState& s, GuestMemory&) {
        if (s.x[0]) {
            std::vector<Address> allocations;
            {
                std::lock_guard lock(*self->allocations_mutex);
                const auto found = self->list_allocations.find(s.x[0]);
                if (found == self->list_allocations.end()) { s.x[0] = status_value(status::param); return; }
                allocations = std::move(found->second);
                self->list_allocations.erase(found);
            }
            for (const auto address : allocations) self->b.process.release_guest(address);
            self->b.process.release_guest(s.x[0]);
        }
        if (s.x[1]) self->b.process.release_guest(s.x[1]);
        s.x[0] = 0;
    });
    // (itemRef, SecItemClass *itemClass, SecKeychainAttributeList *attrList (the caller's tags, filled in),
    //  UInt32 *length, void **outData)
    b.function(lib, "_SecKeychainItemCopyContent", [self](CpuState& s, GuestMemory& m) {
        std::int32_t error{};
        const auto item = self->item_of(s.x[0], error);
        if (!item) { s.x[0] = status_value(error); return; }
        if (s.x[1]) m.write32(s.x[1], four_cc(item->cls));
        if (s.x[2]) {
            const auto count = m.read32(s.x[2]);
            const auto array = m.read64(s.x[2] + 8);
            if (count > 256 || (count && !array)) { s.x[0] = status_value(status::param); return; }
            for (std::uint32_t index = 0; index < count; ++index) {
                const auto entry = array + index * 16ULL;
                const auto bytes = self->attribute_bytes(*item, four_cc(m.read32(entry)));
                m.write32(entry + 4, bytes ? static_cast<std::uint32_t>(bytes->size()) : 0);
                m.write64(entry + 8, bytes && !bytes->empty() ? self->guest_copy(*bytes) : 0);
            }
        }
        self->write_secret(m, *item, s.x[3], s.x[4]);
        s.x[0] = 0;
    });
    // (SecItemClass, SecKeychainAttributeList *attrList, UInt32 length, const void *data, keychain,
    //  SecAccessRef initialAccess, SecKeychainItemRef *itemRef)
    b.function(lib, "_SecKeychainItemCreateFromContent", [self](CpuState& s, GuestMemory& m) {
        Item item;
        if (!self->legacy_class(static_cast<std::uint32_t>(s.x[0]), item.cls)) {
            s.x[0] = status_value(s.x[0] == 0x80001000 || s.x[0] == 0x0f || s.x[0] == 0x10 || s.x[0] == 0x11 ? status::unimplemented : err_no_such_class);
            return;
        }
        auto attributes = self->read_attributes(m, s.x[1]);
        if (!attributes || s.x[2] > (1U << 24) || (s.x[2] && !s.x[3])) { s.x[0] = status_value(status::param); return; }
        item.attributes = std::move(*attributes);
        item.secret.resize(static_cast<std::size_t>(s.x[2]));
        if (!item.secret.empty()) m.read_bytes(s.x[3], item.secret);
        if (s.x[5]) {   // initialAccess
            const auto access = self->access_of(s.x[5]);
            if (!access) { s.x[0] = status_value(status::param); return; }
            item.access = serialize_access(*access);
        }
        const auto path = s.x[4] ? self->path_of(s.x[4]) : std::optional(self->default_path());
        if (!path) { s.x[0] = status_value(status::param); return; }
        std::uint64_t id{};
        if (const auto status = self->add_item(*path, std::move(item), id)) { s.x[0] = status_value(status); return; }
        if (s.x[6]) m.write64(s.x[6], self->item_ref(id, *path));
        s.x[0] = 0;
    });
    // ---- access control: recorded with the items and reported; nothing prompts or is enforced ----
    self->access_class = b.define_class("_AlchemySecAccess", b.object_class, lib);
    self->acl_class = b.define_class("_AlchemySecACL", b.object_class, lib);
    self->trusted_class = b.define_class("_AlchemySecTrustedApplication", b.object_class, lib);
    b.register_type_id(self->access_class, TypeId::sec_access);
    b.register_type_id(self->acl_class, TypeId::sec_acl);
    b.register_type_id(self->trusted_class, TypeId::sec_trusted_application);
    b.function(lib, "_SecAccessGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(TypeId::sec_access); });
    b.function(lib, "_SecACLGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(TypeId::sec_acl); });
    b.function(lib, "_SecTrustedApplicationGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(TypeId::sec_trusted_application); });
    // OSStatus SecTrustedApplicationCreateFromPath(const char *path, SecTrustedApplicationRef *): NULL is the calling application.
    b.function(lib, "_SecTrustedApplicationCreateFromPath", [self](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) { s.x[0] = status_value(status::param); return; }
        auto path = s.x[0] ? self->guest_c_string(m, s.x[0]) : self->calling_application();
        if (path.empty()) { s.x[0] = status_value(status::param); return; }
        m.write64(s.x[1], self->make_trusted(std::move(path)));
        s.x[0] = 0;
    });
    // SecTrustedApplicationCopyData / SetData: the application's identifying data (here, its path).
    b.function(lib, "_SecTrustedApplicationCopyData", [self](CpuState& s, GuestMemory& m) {
        const auto state = s.x[0] ? self->b.objc.find_host_state<TrustedApplicationState>(s.x[0]) : nullptr;
        if (!state || !s.x[1]) { s.x[0] = status_value(status::param); return; }
        m.write64(s.x[1], self->b.data(std::span(reinterpret_cast<const std::uint8_t*>(state->path.data()), state->path.size())));
        s.x[0] = 0;
    });
    b.function(lib, "_SecTrustedApplicationSetData", [self](CpuState& s, GuestMemory&) {
        const auto state = s.x[0] ? self->b.objc.find_host_state<TrustedApplicationState>(s.x[0]) : nullptr;
        if (!state || !s.x[1] || !self->b.is_data(s.x[1])) { s.x[0] = status_value(status::param); return; }
        const auto bytes = self->b.bytes(s.x[1]);
        state->path.assign(bytes.begin(), bytes.end());
        s.x[0] = 0;
    });
    // OSStatus SecAccessCreate(CFStringRef descriptor, CFArrayRef trustedlist, SecAccessRef *accessRef):
    // NULL trusts the calling application, an empty array none.
    b.function(lib, "_SecAccessCreate", [self](CpuState& s, GuestMemory& m) {
        if (!s.x[2] || !s.x[0] || !self->b.is_string(s.x[0])) { s.x[0] = status_value(status::param); return; }
        std::optional<std::vector<std::string>> trusted;
        if (!self->application_paths(s.x[1], trusted)) { s.x[0] = status_value(status::param); return; }
        if (!s.x[1]) trusted = std::vector<std::string>{self->calling_application()};
        m.write64(s.x[2], self->make_access(self->new_access(self->b.text(s.x[0]), std::move(trusted))));
        s.x[0] = 0;
    });
    const auto make_acl = [self](const std::shared_ptr<Access>& access, std::shared_ptr<Acl> acl) {
        const auto object = self->b.objc.allocate(self->acl_class);
        auto state = self->b.objc.host_state<AclState>(object);
        state->access = access;
        state->acl = std::move(acl);
        return object;
    };
    // OSStatus SecAccessCopyACLList(SecAccessRef, CFArrayRef *aclList)
    b.function(lib, "_SecAccessCopyACLList", [self, make_acl](CpuState& s, GuestMemory& m) {
        const auto access = self->access_of(s.x[0]);
        if (!access || !s.x[1]) { s.x[0] = status_value(status::param); return; }
        std::vector<Address> acls;
        for (const auto& acl : access->acls) acls.push_back(make_acl(access, acl));
        m.write64(s.x[1], self->b.array(acls));
        self->b.release_all(acls);
        s.x[0] = 0;
    });
    const auto acl_of = [self](Address object) {
        const auto state = object ? self->b.objc.find_host_state<AclState>(object) : nullptr;
        return state && state->acl ? state : nullptr;
    };
    // OSStatus SecACLCopyContents(SecACLRef, CFArrayRef *applicationList, CFStringRef *description,
    //     SecKeychainPromptSelector *promptSelector): a NULL list is any application.
    b.function(lib, "_SecACLCopyContents", [self, acl_of](CpuState& s, GuestMemory& m) {
        const auto state = acl_of(s.x[0]);
        if (!state) { s.x[0] = status_value(status::param); return; }
        if (s.x[1]) {
            Address list{};
            if (state->acl->applications) {
                std::vector<Address> apps;
                for (const auto& path : *state->acl->applications) apps.push_back(self->make_trusted(path));
                list = self->b.array(apps);
                self->b.release_all(apps);
            }
            m.write64(s.x[1], list);
        }
        if (s.x[2]) m.write64(s.x[2], self->b.string(state->acl->description));
        if (s.x[3]) m.write_bytes(s.x[3], std::vector<std::uint8_t>{static_cast<std::uint8_t>(state->acl->prompt), static_cast<std::uint8_t>(state->acl->prompt >> 8)});
        s.x[0] = 0;
    });
    // OSStatus SecACLSetContents(SecACLRef, CFArrayRef applicationList, CFStringRef description, SecKeychainPromptSelector)
    b.function(lib, "_SecACLSetContents", [self, acl_of](CpuState& s, GuestMemory&) {
        const auto state = acl_of(s.x[0]);
        std::optional<std::vector<std::string>> apps;
        if (!state || !self->application_paths(s.x[1], apps) || (s.x[2] && !self->b.is_string(s.x[2]))) { s.x[0] = status_value(status::param); return; }
        state->acl->applications = std::move(apps);
        state->acl->description = s.x[2] ? self->b.text(s.x[2]) : std::string();
        state->acl->prompt = static_cast<std::uint16_t>(s.x[3]);
        s.x[0] = 0;
    });
    // OSStatus SecACLCreateWithSimpleContents(SecAccessRef, CFArrayRef applicationList, CFStringRef description,
    //     SecKeychainPromptSelector, SecACLRef *newAcl): another use ACL.
    b.function(lib, "_SecACLCreateWithSimpleContents", [self, make_acl](CpuState& s, GuestMemory& m) {
        const auto access = self->access_of(s.x[0]);
        auto acl = std::make_shared<Acl>();
        if (!access || !s.x[4] || !self->application_paths(s.x[1], acl->applications) || (s.x[2] && !self->b.is_string(s.x[2]))) {
            s.x[0] = status_value(status::param);
            return;
        }
        acl->role = "use";
        acl->description = s.x[2] ? self->b.text(s.x[2]) : std::string();
        acl->prompt = static_cast<std::uint16_t>(s.x[3]);
        access->acls.push_back(acl);
        m.write64(s.x[4], make_acl(access, acl));
        s.x[0] = 0;
    });
    b.function(lib, "_SecACLRemove", [acl_of](CpuState& s, GuestMemory&) {
        const auto state = acl_of(s.x[0]);
        if (!state) { s.x[0] = status_value(status::param); return; }
        std::erase(state->access->acls, state->acl);
        s.x[0] = 0;
    });
    // OSStatus SecKeychainItemCopyAccess(SecKeychainItemRef, SecAccessRef *): the item's recorded access, else
    // the access it was created with by default (the calling application trusted).
    b.function(lib, "_SecKeychainItemCopyAccess", [self](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) { s.x[0] = status_value(status::param); return; }
        std::int32_t error{};
        const auto item = self->item_of(s.x[0], error);
        if (!item) { s.x[0] = status_value(error); return; }
        std::optional<Access> access;
        if (!item->access.empty()) access = deserialize_access(item->access);
        if (!access) {
            std::string label;
            for (const auto* name : {"labl", "svce", "srvr"})
                if (const auto found = item->attributes.find(name); label.empty() && found != item->attributes.end()) label = found->second.text;
            access = *self->new_access(label, std::vector<std::string>{self->calling_application()});
        }
        m.write64(s.x[1], self->make_access(std::make_shared<Access>(std::move(*access))));
        s.x[0] = 0;
    });
    // OSStatus SecKeychainItemSetAccess(SecKeychainItemRef, SecAccessRef)
    b.function(lib, "_SecKeychainItemSetAccess", [self](CpuState& s, GuestMemory&) {
        const auto access = self->access_of(s.x[1]);
        const auto id = self->item_id(s.x[0]);
        if (!access || !id) { s.x[0] = status_value(status::param); return; }
        const auto serialized = serialize_access(*access);
        s.x[0] = status_value(self->with_store(self->item_keychain(s.x[0]), true, [&](Store& store) {
            for (auto& item : store.items)
                if (item.id == *id) {
                    item.access = serialized;
                    item.modified = CoreFoundation::absolute_time_now();
                    store.save();
                    return status::success;
                }
            return status::item_not_found;
        }));
    });

    b.function(lib, "_SecKeychainItemCopyKeychain", [self](CpuState& s, GuestMemory& m) {
        if (!self->item_id(s.x[0]) || !s.x[1]) { s.x[0] = status_value(status::param); return; }
        m.write64(s.x[1], self->keychain_object(self->item_keychain(s.x[0])));
        s.x[0] = 0;
    });

    // ---- searches ----
    const auto search_class = b.define_class("_AlchemySecKeychainSearch", b.object_class, lib);
    b.function(lib, "_SecKeychainSearchGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(TypeId::keychain_search); });
    b.register_type_id(search_class, TypeId::keychain_search);
    // (keychainOrArray, SecItemClass, const SecKeychainAttributeList *, SecKeychainSearchRef *)
    b.function(lib, "_SecKeychainSearchCreateFromAttributes", [self, search_class](CpuState& s, GuestMemory& m) {
        const auto keychains = self->keychains_of(s.x[0]);
        auto attributes = self->read_attributes(m, s.x[2]);
        if (!keychains || !attributes || !s.x[3]) { s.x[0] = status_value(status::param); return; }
        auto state = std::make_shared<SearchState>();
        state->keychains = *keychains;
        state->attributes = std::move(*attributes);
        if (!self->legacy_class(static_cast<std::uint32_t>(s.x[1]), state->cls)) state->cls.clear();   // no other class is stored
        const auto object = self->b.objc.allocate(search_class);
        static_cast<void>(self->b.objc.host_state<SearchState>(object, [&] { return state; }));
        m.write64(s.x[3], object);
        s.x[0] = 0;
    });
    b.function(lib, "_SecKeychainSearchCopyNext", [self](CpuState& s, GuestMemory& m) {
        const auto state = s.x[0] ? self->b.objc.find_host_state<SearchState>(s.x[0]) : nullptr;
        if (!state || !s.x[1]) { s.x[0] = status_value(status::param); return; }
        while (!state->cls.empty() && state->keychain < state->keychains.size()) {
            const auto path = state->keychains[state->keychain];
            std::optional<std::uint64_t> next;
            const auto status = self->with_store(path, true, [&](Store& store) {
                for (const auto& item : store.items)
                    if (item.id > state->after && self->matches_all(item, state->cls, state->attributes) && (!next || item.id < *next)) next = item.id;
                return status::success;
            });
            if (status == status::interaction_not_allowed) { s.x[0] = status_value(status); return; }
            if (next) {
                state->after = *next;
                m.write64(s.x[1], self->item_ref(*next, path));
                s.x[0] = 0;
                return;
            }
            ++state->keychain;
            state->after = 0;
        }
        s.x[0] = status_value(status::item_not_found);
    });

    // ---- keychains ----
    // (const char *pathName, UInt32 passwordLength, const void *password, Boolean promptUser, SecAccessRef, SecKeychainRef *)
    b.function(lib, "_SecKeychainCreate", [self](CpuState& s, GuestMemory& m) {
        const auto guest = self->guest_c_string(m, s.x[0]);
        if (guest.empty() || !s.x[5] || s.x[1] > 4096) { s.x[0] = status_value(status::param); return; }
        if (!s.x[2] && (s.x[3] & 0xff)) { s.x[0] = status_value(status::interaction_not_allowed); return; }   // no UI to ask
        Bytes password(static_cast<std::size_t>(s.x[1]));
        if (!password.empty()) m.read_bytes(s.x[2], password);
        const auto path = Keychain::host_of(guest);
        std::error_code error;
        if (std::filesystem::exists(path, error)) { s.x[0] = status_value(err_duplicate_keychain); return; }
        const auto record = password_record(password);
        host::crypto::secure_zero(std::as_writable_bytes(std::span(password)));
        if (!record) { s.x[0] = status_value(status::allocate); return; }
        {
            std::lock_guard lock(process_mutex);
            Store store(path);
            store.password = *record;
            store.save();
        }
        { std::lock_guard lock(lock_mutex); locked_keychains.erase(path.wstring()); }
        m.write64(s.x[5], self->keychain_object(path));
        s.x[0] = 0;
    });
    // (const char *pathName, SecKeychainRef *): a reference, whether or not the file exists yet (as on macOS).
    b.function(lib, "_SecKeychainOpen", [self](CpuState& s, GuestMemory& m) {
        const auto guest = self->guest_c_string(m, s.x[0]);
        if (guest.empty() || !s.x[1]) { s.x[0] = status_value(status::param); return; }
        m.write64(s.x[1], self->keychain_object(Keychain::host_of(guest)));
        s.x[0] = 0;
    });
    b.function(lib, "_SecKeychainDelete", [self](CpuState& s, GuestMemory&) {
        const auto path = self->path_of(s.x[0]);
        if (!path) { s.x[0] = status_value(status::param); return; }
        std::lock_guard lock(process_mutex);
        std::error_code error;
        if (!std::filesystem::remove(*path, error)) { s.x[0] = status_value(err_no_such_keychain); return; }
        std::erase(self->search_list, *path);
        if (self->default_override == *path) self->default_override.reset();
        s.x[0] = 0;
    });
    // (keychain, UInt32 *ioPathLength, char *pathName)
    b.function(lib, "_SecKeychainGetPath", [self](CpuState& s, GuestMemory& m) {
        const auto path = s.x[0] ? self->path_of(s.x[0]) : std::optional(self->default_path());
        if (!path || !s.x[1] || !s.x[2]) { s.x[0] = status_value(status::param); return; }
        const auto text = guest_path_string(*path);
        const auto room = m.read32(s.x[1]);
        m.write32(s.x[1], static_cast<std::uint32_t>(text.size()));
        if (text.size() + 1 > room) { s.x[0] = status_value(err_buffer_too_small); return; }
        m.write_bytes(s.x[2], std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
        m.write8(s.x[2] + text.size(), 0);
        s.x[0] = 0;
    });
    // (keychain, SecKeychainStatus *): unlocked (1), readable (2), writable (4).
    b.function(lib, "_SecKeychainGetStatus", [self](CpuState& s, GuestMemory& m) {
        const auto path = s.x[0] ? self->path_of(s.x[0]) : std::optional(self->default_path());
        if (!path || !s.x[1]) { s.x[0] = status_value(status::param); return; }
        std::error_code error;
        if (*path != keychain_path() && !std::filesystem::exists(*path, error)) { s.x[0] = status_value(err_no_such_keychain); return; }
        m.write32(s.x[1], (is_locked(*path) ? 0 : 1) | 2 | 4);
        s.x[0] = 0;
    });
    b.function(lib, "_SecKeychainLock", [self](CpuState& s, GuestMemory&) {
        const auto path = s.x[0] ? self->path_of(s.x[0]) : std::optional(self->default_path());
        if (!path) { s.x[0] = status_value(status::param); return; }
        std::lock_guard lock(lock_mutex);
        locked_keychains.insert(path->wstring());
        s.x[0] = 0;
    });
    b.function(lib, "_SecKeychainLockAll", [self](CpuState& s, GuestMemory&) {
        std::lock_guard lock(lock_mutex);
        for (const auto& path : self->searched()) locked_keychains.insert(path.wstring());
        s.x[0] = 0;
    });
    // (keychain, UInt32 passwordLength, const void *password, Boolean usePassword)
    b.function(lib, "_SecKeychainUnlock", [self](CpuState& s, GuestMemory& m) {
        const auto path = s.x[0] ? self->path_of(s.x[0]) : std::optional(self->default_path());
        if (!path || s.x[1] > 4096) { s.x[0] = status_value(status::param); return; }
        std::optional<Bytes> record;
        bool existed = false;
        {
            std::lock_guard lock(process_mutex);
            Store store(*path);
            record = store.password;
            existed = store.existed;
        }
        if (!existed && *path != keychain_path()) { s.x[0] = status_value(err_no_such_keychain); return; }
        if (record) {
            if (!(s.x[3] & 0xff)) { s.x[0] = status_value(status::interaction_not_allowed); return; }   // no UI to ask
            Bytes password(static_cast<std::size_t>(s.x[1]));
            if (!password.empty()) m.read_bytes(s.x[2], password);
            const bool ok = password_matches(*record, password);
            host::crypto::secure_zero(std::as_writable_bytes(std::span(password)));
            if (!ok) { s.x[0] = status_value(err_auth_failed); return; }
        }
        std::lock_guard lock(lock_mutex);
        locked_keychains.erase(path->wstring());
        s.x[0] = 0;
    });
    b.function(lib, "_SecKeychainCopySearchList", [self](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) { s.x[0] = status_value(status::param); return; }
        std::vector<Address> keychains;
        for (const auto& path : self->searched()) keychains.push_back(self->keychain_object(path));
        m.write64(s.x[0], self->b.array(keychains));
        self->b.release_all(keychains);
        s.x[0] = 0;
    });
    b.function(lib, "_SecKeychainSetSearchList", [self](CpuState& s, GuestMemory&) {
        if (!s.x[0] || !self->b.is_array(s.x[0])) { s.x[0] = status_value(status::param); return; }
        std::vector<std::filesystem::path> paths;
        for (const auto value : self->b.cf.array_values(s.x[0])) {
            const auto path = self->path_of(value);
            if (!path) { s.x[0] = status_value(status::param); return; }
            paths.push_back(*path);
        }
        self->search_list = std::move(paths);
        s.x[0] = 0;
    });
    b.function(lib, "_SecKeychainSetDefault", [self](CpuState& s, GuestMemory&) {
        const auto path = self->path_of(s.x[0]);
        if (!path) { s.x[0] = status_value(status::param); return; }
        self->default_override = *path;
        s.x[0] = 0;
    });
}

} // namespace alchemy::gui::security
