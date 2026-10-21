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
#include <map>
#include <variant>

// Keychain items (SecItemAdd / SecItemCopyMatching / SecItemUpdate /
// SecItemDelete and the SecKeychain generic-password calls) for generic and
// internet passwords. The items live in one file per Windows user, encrypted
// with DPAPI for that user: <Alchemy root>/.alchemy/keychain/<user>.alchemy-keychain
// when an Alchemy root is configured (outside the guest-visible /Applications
// and /Library), otherwise %LOCALAPPDATA%/Alchemy/keychain/login.alchemy-keychain;
// ALCHEMY_KEYCHAIN overrides the path. Keys,
// certificates and identities are not stored: adding them fails with
// errSecUnimplemented and queries for them find nothing.
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

Bytes serialize(const std::vector<Item>& items, std::uint64_t next_id) {
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
    return out;
}

bool deserialize(std::span<const std::uint8_t> data, std::vector<Item>& items, std::uint64_t& next_id) {
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
    return in.ok && in.at == data.size();
}

const std::uint8_t entropy_bytes[] = {'A', 'l', 'c', 'h', 'e', 'm', 'y', ' ', 'k', 'e', 'y', 'c', 'h', 'a', 'i', 'n', ' ', 'v', '1'};

// The decrypted store, held while the process-wide and cross-process locks are held.
class Store {
public:
    Store() : path_(keychain_path()) {
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

    void save() {
        auto plain = serialize(items, next_id);
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
        std::ifstream file(path_, std::ios::binary);
        Bytes content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (content.size() < file_magic.size() || std::memcmp(content.data(), file_magic.data(), file_magic.size()) != 0)
            fail("the keychain file is not an Alchemy keychain: " + narrow(path_.wstring()));
        auto plain = host::crypto::unprotect(std::as_bytes(std::span(content).subspan(file_magic.size())), std::as_bytes(std::span(entropy_bytes)));
        if (!plain) fail("the keychain cannot be decrypted for this user (" + std::string(host::name(plain.error())) + ")");
        const bool ok = deserialize(std::span(*plain), items, next_id);
        host::crypto::secure_zero(std::as_writable_bytes(std::span(*plain)));
        if (!ok) fail("the keychain file is corrupt");
    }
    std::filesystem::path path_;
    host::OwnedHandle mutex_;
};

std::mutex process_mutex;

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

struct KeychainItemState final : ObjectPayload { std::uint64_t id{}; };

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
    Address item_ref(std::uint64_t id) {
        const auto object = b.objc.allocate(item_class);
        auto state = std::make_shared<KeychainItemState>();
        state->id = id;
        static_cast<void>(b.objc.host_state<KeychainItemState>(object, [&] { return state; }));
        return object;
    }
    std::optional<std::uint64_t> item_id(Address object) {
        const auto state = object ? b.objc.find_host_state<KeychainItemState>(object) : nullptr;
        if (!state) return std::nullopt;
        return state->id;
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
        if (!self->default_keychain) self->default_keychain = self->b.objc.allocate(self->keychain_class);
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
        std::uint64_t id{};
        {
            std::lock_guard lock(process_mutex);
            Store store;
            item.id = store.next_id++;
            item.created = item.modified = CoreFoundation::absolute_time_now();
            for (const auto& other : store.items) if (duplicates(item, other)) { s.x[0] = status_value(status::duplicate_item); return; }
            store.items.push_back(item);
            store.save();
            id = item.id;
        }
        if (out) m.write64(out, self->item_ref(id));
        s.x[0] = 0;
    });
    // OSStatus SecKeychainFindGenericPassword(keychainOrArray, serviceNameLength, serviceName,
    //     accountNameLength, accountName, UInt32 *passwordLength, void **passwordData, itemRef *)
    b.function(lib, "_SecKeychainFindGenericPassword", [self](CpuState& s, GuestMemory& m) {
        const auto service = self->guest_text(m, s.x[2], s.x[1]), account = self->guest_text(m, s.x[4], s.x[3]);
        std::optional<Item> found;
        {
            std::lock_guard lock(process_mutex);
            Store store;
            for (const auto& item : store.items) {
                if (item.cls != "genp") continue;
                if (!service.empty() && !(primary_value(item, "svce") == Value{Value::Kind::string, service})) continue;
                if (!account.empty() && !(primary_value(item, "acct") == Value{Value::Kind::string, account})) continue;
                found = item;
                break;
            }
        }
        if (!found) { s.x[0] = status_value(status::item_not_found); return; }
        if (s.x[5]) m.write32(s.x[5], static_cast<std::uint32_t>(found->secret.size()));
        if (s.x[6]) {
            const auto buffer = self->b.process.allocate_guest(std::max<std::size_t>(found->secret.size(), 1));
            if (!found->secret.empty()) m.write_bytes(buffer, found->secret);
            m.write64(s.x[6], buffer);
        }
        if (s.x[7]) m.write64(s.x[7], self->item_ref(found->id));
        s.x[0] = 0;
    });
    b.function(lib, "_SecKeychainItemFreeContent", [self](CpuState& s, GuestMemory&) {
        if (s.x[0]) { s.x[0] = status_value(status::unimplemented); return; }   // attribute lists are never returned
        if (s.x[1]) self->b.process.release_guest(s.x[1]);
        s.x[0] = 0;
    });
    b.function(lib, "_SecKeychainItemDelete", [self](CpuState& s, GuestMemory&) {
        const auto id = self->item_id(s.x[0]);
        if (!id) { s.x[0] = status_value(status::param); return; }
        std::lock_guard lock(process_mutex);
        Store store;
        const auto before = store.items.size();
        std::erase_if(store.items, [&](const Item& item) { return item.id == *id; });
        if (store.items.size() == before) { s.x[0] = status_value(status::item_not_found); return; }
        store.save();
        s.x[0] = 0;
    });
    // Only the password data can be changed (attribute lists are not supported).
    b.function(lib, "_SecKeychainItemModifyAttributesAndData", [self](CpuState& s, GuestMemory& m) {
        const auto id = self->item_id(s.x[0]);
        if (!id) { s.x[0] = status_value(status::param); return; }
        if (s.x[1]) { s.x[0] = status_value(status::unimplemented); return; }
        if (s.x[2] > (1U << 24)) { s.x[0] = status_value(status::param); return; }
        Bytes secret(static_cast<std::size_t>(s.x[2]));
        if (!secret.empty()) m.read_bytes(s.x[3], secret);
        std::lock_guard lock(process_mutex);
        Store store;
        for (auto& item : store.items) {
            if (item.id != *id) continue;
            if (s.x[3]) item.secret = secret;
            item.modified = CoreFoundation::absolute_time_now();
            store.save();
            s.x[0] = 0;
            return;
        }
        s.x[0] = status_value(status::item_not_found);
    });
}

} // namespace alchemy::gui::security
