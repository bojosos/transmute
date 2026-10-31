#include "alchemy/darwin/security/security_internal.hpp"

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
// ALCHEMY_KEYCHAIN overrides the path. Keys,
// certificates and identities are not stored: adding them fails with
// errSecUnimplemented and queries for them find nothing.
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

struct Item {
    std::uint64_t id{};
    std::string cls;                          // "genp" / "inet"
    std::map<std::string, Value> attributes;  // keyed by attribute name ("acct", "svce", ...)
    Bytes secret;
    double created{}, modified{};
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
    if (in.ok && in.at < data.size()) {
        if (in.number(4) != password_trailer) return false;
        password = in.bytes();
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
    return cls == "inet" ? internet : generic;
}

struct Query {
    std::string cls;
    std::map<std::string, Value> attributes;
    std::optional<Bytes> secret;
    std::optional<std::uint64_t> persistent;   // v_PersistentRef
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
                query.status = status::unimplemented;   // item references as match values
                return query;
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
        if (query.cls.empty()) query.status = status::param;
        static_cast<void>(for_add);
        return query;
    }

    // One result value per the r_* flags (+1); nullopt when a return type is unsupported.
    std::optional<Address> result_of(const Item& item, const Query& query) {
        if (query.return_ref) return std::nullopt;
        const int kinds = (query.return_data ? 1 : 0) + (query.return_attributes ? 1 : 0) + (query.return_persistent ? 1 : 0);
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
        if (query.return_persistent) put("v_PersistentRef", b.data(persistent_ref(item.id)));
        const auto result = b.dictionary(entries);
        b.release_all(owned);
        return result;
    }
    bool returns_anything(const Query& query) {
        return query.return_data || query.return_attributes || query.return_ref || query.return_persistent;
    }
    bool supported_class(const std::string& cls) { return cls == "genp" || cls == "inet"; }

    std::int32_t add(Address attributes, Address result_out) {
        auto query = parse(attributes, true);
        if (query.status) return query.status;
        if (is_locked(keychain_path())) return status::interaction_not_allowed;
        if (!supported_class(query.cls)) return query.cls == "keys" || query.cls == "cert" || query.cls == "idnt" ? status::unimplemented : status::param;
        if (query.persistent) return status::param;
        std::lock_guard lock(process_mutex);
        Store store;
        Item item;
        item.id = store.next_id++;
        item.cls = query.cls;
        item.attributes = query.attributes;
        item.secret = query.secret.value_or(Bytes{});
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
        if (!supported_class(query.cls)) return status::item_not_found;   // no keys / certificates / identities are stored
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
        if (!supported_class(query.cls)) return status::item_not_found;
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
        {"kSecUseAuthenticationUISkip", "u_AuthUIS"}};
    for (const auto& [name, value] : constants) sec.export_constant(name, value);

    b.function(lib, "_SecItemAdd", [self](CpuState& s, GuestMemory&) { s.x[0] = status_value(self->add(s.x[0], s.x[1])); });
    b.function(lib, "_SecItemCopyMatching", [self](CpuState& s, GuestMemory& m) {
        if (s.x[1]) m.write64(s.x[1], 0);
        s.x[0] = status_value(self->copy_matching(s.x[0], s.x[1]));
    });
    b.function(lib, "_SecItemUpdate", [self](CpuState& s, GuestMemory&) { s.x[0] = status_value(self->update(s.x[0], s.x[1])); });
    b.function(lib, "_SecItemDelete", [self](CpuState& s, GuestMemory&) { s.x[0] = status_value(self->remove(s.x[0])); });

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
        const auto path = s.x[4] ? self->path_of(s.x[4]) : std::optional(self->default_path());
        if (!path) { s.x[0] = status_value(status::param); return; }
        std::uint64_t id{};
        if (const auto status = self->add_item(*path, std::move(item), id)) { s.x[0] = status_value(status); return; }
        if (s.x[6]) m.write64(s.x[6], self->item_ref(id, *path));
        s.x[0] = 0;
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
