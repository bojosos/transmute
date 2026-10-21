#include "alchemy/darwin/kit/completion/completion_kit.hpp"
#include "alchemy/darwin/corefoundation/cf_plist.hpp"

#include <map>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// NSKeyedArchiver / NSKeyedUnarchiver (QLab archives its license activations
// and settings), added to the classes the other providers declare. The
// archive is Foundation's: {$archiver: "NSKeyedArchiver", $version: 100000,
// $top: {root: UID}, $objects: ["$null", ...]} as a binary (default) or XML
// property list (corefoundation/cf_plist with UIDs).
//   Inline values: NSString (immutable), NSNumber and booleans, NSData
//   (immutable). Foundation collections and values are encoded natively:
//   NSArray / NSMutableArray / NSSet / NSMutableSet / NSOrderedSet (NS.objects),
//   NSDictionary / NSMutableDictionary (NS.keys, NS.objects), NSMutableString
//   (NS.string), NSMutableData (NS.bytes), NSDate (NS.time), NSURL (NS.base,
//   NS.relative), NSNull. Every other object must answer -encodeWithCoder: /
//   -initWithCoder: (with this archiver / unarchiver as the coder); its class
//   chain is recorded in $classes, and +[NSKeyedArchiver classNameForClass:] /
//   +[NSKeyedUnarchiver classForClassName:] apply. Keys beginning with "$"
//   are escaped with another "$".
// Secure coding: archiving requires +supportsSecureCoding; decoding checks
// the allowed classes of -decodeObjectOfClass(es):forKey: and the
// unarchivedObjectOfClass(es) factories (subclasses allowed). Failures give
// NSCocoaErrorDomain errors (4864 corrupt, 4865 not found).
// Not provided (explicit failures): NSValue geometry objects, archiver
// delegates, and the non-keyed coding methods.
namespace alchemy::gui::completion_kit {
namespace {

using export_fill::Plist;

Plist text_plist(std::string value) { Plist p; p.kind = Plist::Kind::string; p.text = std::move(value); return p; }
Plist integer_plist(std::int64_t value) { Plist p; p.kind = Plist::Kind::integer; p.integer = value; return p; }
Plist uid_plist(std::uint64_t value) { Plist p; p.kind = Plist::Kind::uid; p.integer = static_cast<std::int64_t>(value); return p; }
Plist dictionary_plist() { Plist p; p.kind = Plist::Kind::dictionary; return p; }
std::string escape_key(std::string key) { return !key.empty() && key[0] == '$' ? "$" + key : key; }

struct ArchiverState final : ObjectPayload {
    std::vector<Plist> objects{text_plist("$null")};
    std::unordered_map<Address, std::uint64_t> uids;
    std::map<std::string, std::uint64_t> class_uids;
    std::vector<std::size_t> stack;           // object indices being encoded (empty: $top)
    Plist top = dictionary_plist();
    std::vector<std::uint8_t> bytes;
    bool finished{}, secure{}, xml{};
    // +archivedDataWithRootObject:requiringSecureCoding:error: reports a class
    // without NSSecureCoding as an error (the other entry points raise).
    bool collect_errors{};
    std::string error;
    Address output_data{};                    // initForWritingWithMutableData: (not retained by Foundation either)
};

struct UnarchiverState final : ObjectPayload {
    Plist archive;
    std::vector<Plist> objects;
    std::unordered_map<std::uint64_t, Address> decoded;   // +1 each, released at dealloc
    std::vector<std::uint64_t> stack;                      // object uids being decoded (empty: $top)
    std::vector<Address> buffers;                          // decodeBytesForKey: results
    std::vector<Address> allowed;                          // classes for the next decode (secure coding)
    bool secure{}, failed{};
    Address error{};                                       // +1
};

class Archive {
public:
    explicit Archive(Support& support) : s_(support) {}
    Support& s_;
    Address archiver_class{}, unarchiver_class{};

    std::string class_name(Address cls) {
        const auto pointer = s_.call("/usr/lib/libobjc.A.dylib", "_class_getName", {cls});
        std::string name;
        for (auto at = pointer; at && name.size() < 1024; ++at) { const auto c = s_.memory().read8(at); if (!c) break; name.push_back(static_cast<char>(c)); }
        return name;
    }
    Address superclass(Address cls) { return s_.call("/usr/lib/libobjc.A.dylib", "_class_getSuperclass", {cls}); }
    std::string archived_name(Address cls) {
        if (const auto name = s_.send_id(archiver_class, "classNameForClass:", {cls})) return s_.cf.utf8(name);
        return class_name(cls);
    }
    bool is_subclass(Address cls, Address of) {
        for (auto current = cls; current; current = superclass(current)) if (current == of) return true;
        return false;
    }
    Address error(std::int64_t code, const std::string& description) {
        const auto error_class = s_.objc.find_class("NSError");
        if (!error_class) return 0;
        std::vector<std::pair<Address, Address>> entries{{s_.cf.constant_string("NSDebugDescription"), s_.send_id(s_.cf.string(description), "autorelease")}};
        const auto info = s_.send_id(s_.cf.dictionary(entries), "autorelease");
        CallRegs registers;
        registers.x[2] = s_.cf.constant_string("NSCocoaErrorDomain");
        registers.x[3] = static_cast<Address>(code);
        registers.x[4] = info;
        return s_.send(error_class, "errorWithDomain:code:userInfo:", registers).x0;
    }
    Address data_object(std::span<const std::uint8_t> bytes, bool mutable_data) {
        const auto block = s_.process.allocate_guest(std::max<std::size_t>(bytes.size(), 1));
        Address result{};
        try {
            if (!bytes.empty()) s_.memory().write_bytes(block, bytes);
            CallRegs make;
            make.x[2] = block;
            make.x[3] = bytes.size();
            result = s_.send(s_.objc.find_class(mutable_data ? "NSMutableData" : "NSData"), "dataWithBytes:length:", make).x0;
        } catch (...) { s_.process.release_guest(block); throw; }
        s_.process.release_guest(block);
        return result;   // autoreleased
    }
    std::vector<std::uint8_t> data_bytes(Address data) {
        const auto length = s_.cf.length(data);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        if (length) s_.memory().read_bytes(s_.send(data, "bytes").x0, bytes);
        return bytes;
    }

    // ---- archiving ----
    Plist& current(ArchiverState& a) { return a.stack.empty() ? a.top : a.objects[a.stack.back()]; }
    void put(ArchiverState& a, Address key, Plist value) {
        if (!key) fail("NSKeyedArchiver: nil key");
        current(a).set(escape_key(s_.cf.utf8(key)), std::move(value));
    }
    std::uint64_t class_uid(ArchiverState& a, Address cls, const std::string& name) {
        if (const auto found = a.class_uids.find(name); found != a.class_uids.end()) return found->second;
        Plist entry = dictionary_plist();
        entry.set("$classname", text_plist(name));
        Plist chain;
        chain.kind = Plist::Kind::array;
        chain.items.push_back(text_plist(name));
        for (auto parent = cls ? superclass(cls) : 0; parent; parent = superclass(parent)) chain.items.push_back(text_plist(class_name(parent)));
        entry.set("$classes", std::move(chain));
        const auto uid = a.objects.size();
        a.objects.push_back(std::move(entry));
        a.class_uids.emplace(name, uid);
        return uid;
    }
    std::uint64_t new_object(ArchiverState& a, Address object) {
        const auto uid = a.objects.size();
        a.objects.push_back(dictionary_plist());
        if (object) a.uids.emplace(object, uid);
        return uid;
    }
    Plist uid_array(ArchiverState& a, const std::vector<Address>& values) {
        Plist array;
        array.kind = Plist::Kind::array;
        for (const auto value : values) array.items.push_back(uid_plist(encode(a, value)));
        return array;
    }
    std::vector<Address> elements(Address collection) {
        const auto all = s_.cf.kind(collection) == ObjectKind::array ? collection : s_.send_id(collection, "allObjects");
        if (!all || s_.cf.kind(all) != ObjectKind::array) fail("NSKeyedArchiver: a collection without -allObjects");
        return s_.cf.array_values(all);
    }
    std::uint64_t encode(ArchiverState& a, Address object) {
        if (!object) return 0;
        if (const auto found = a.uids.find(object); found != a.uids.end()) return found->second;
        const auto kind = s_.cf.kind(object);
        const auto cls = s_.objc.object_class(object);
        const auto mutable_object = kind && s_.cf.is_mutable(object);
        // Inline values.
        if (kind == ObjectKind::string && !mutable_object) { const auto uid = a.objects.size(); a.objects.push_back(text_plist(s_.cf.utf8(object))); a.uids.emplace(object, uid); return uid; }
        if (kind == ObjectKind::boolean) { Plist p; p.kind = Plist::Kind::boolean; p.boolean = s_.send(object, "boolValue").x0 & 0xff; const auto uid = a.objects.size(); a.objects.push_back(p); return uid; }
        if (kind == ObjectKind::number) {
            const auto type = s_.send(object, "objCType").x0;
            const char code = type ? static_cast<char>(s_.memory().read8(type)) : 'q';
            Plist p;
            if (code == 'd' || code == 'f') { p.kind = Plist::Kind::real; p.real = s_.cf.number_value(object).value_or(0); }
            else { p.kind = Plist::Kind::integer; p.integer = static_cast<std::int64_t>(s_.send(object, "longLongValue").x0); }
            const auto uid = a.objects.size();
            a.objects.push_back(p);
            return uid;
        }
        if (kind == ObjectKind::data && !mutable_object) {
            Plist p;
            p.kind = Plist::Kind::data;
            p.bytes = data_bytes(object);
            const auto uid = a.objects.size();
            a.objects.push_back(std::move(p));
            a.uids.emplace(object, uid);
            return uid;
        }
        const auto uid = new_object(a, object);
        std::string name;
        if (kind == ObjectKind::string) { a.objects[uid].set("NS.string", text_plist(s_.cf.utf8(object))); name = "NSMutableString"; }
        else if (kind == ObjectKind::data) { Plist p; p.kind = Plist::Kind::data; p.bytes = data_bytes(object); a.objects[uid].set("NS.bytes", std::move(p)); name = "NSMutableData"; }
        else if (kind == ObjectKind::date) { Plist p; p.kind = Plist::Kind::real; p.real = s_.cf.date_time(object); a.objects[uid].set("NS.time", p); name = "NSDate"; }
        else if (kind == ObjectKind::url) {
            const auto relative = encode(a, s_.send_id(object, "absoluteString"));
            a.objects[uid].set("NS.base", uid_plist(0));
            a.objects[uid].set("NS.relative", uid_plist(relative));
            name = "NSURL";
        } else if (kind == ObjectKind::array) {
            auto array = uid_array(a, s_.cf.array_values(object));
            a.objects[uid].set("NS.objects", std::move(array));
            name = mutable_object ? "NSMutableArray" : "NSArray";
        } else if (kind == ObjectKind::dictionary) {
            std::vector<Address> keys, values;
            for (const auto& [key, value] : s_.cf.dictionary_entries(object)) { keys.push_back(key); values.push_back(value); }
            auto key_array = uid_array(a, keys);
            auto value_array = uid_array(a, values);
            a.objects[uid].set("NS.keys", std::move(key_array));
            a.objects[uid].set("NS.objects", std::move(value_array));
            name = mutable_object ? "NSMutableDictionary" : "NSDictionary";
        } else {
            const auto plain = class_name(cls);
            const bool null_object = plain == "NSNull";
            const bool set_like = plain == "NSSet" || plain == "NSMutableSet" || plain == "NSOrderedSet" || plain == "NSMutableOrderedSet";
            if (null_object) name = "NSNull";
            else if (set_like) {
                auto array = uid_array(a, plain.find("Ordered") != std::string::npos ? s_.cf.array_values(s_.send_id(object, "array")) : elements(object));
                a.objects[uid].set("NS.objects", std::move(array));
                name = plain;
            } else {
                // The object's own coding.
                if (!s_.objc.responds(object, s_.objc.selector("encodeWithCoder:")))
                    fail("NSInvalidArgumentException: " + plain + " does not implement -encodeWithCoder: (NSCoding)");
                auto archive_class = cls;
                if (s_.objc.responds(object, s_.objc.selector("classForKeyedArchiver")))
                    if (const auto replacement = s_.send_id(object, "classForKeyedArchiver")) archive_class = replacement;
                if (a.secure && !(s_.send(archive_class, "supportsSecureCoding").x0 & 0xff)) {
                    const auto why = "Class '" + class_name(archive_class) + "' does not adopt NSSecureCoding";
                    if (!a.collect_errors) fail("NSInvalidArgumentException: " + why + " (archiving securely)");
                    if (a.error.empty()) a.error = why;
                    a.objects[uid] = text_plist("$null");   // the archive is discarded
                    return uid;
                }
                a.stack.push_back(uid);
                try { s_.send_id(object, "encodeWithCoder:", {current_coder}); } catch (...) { a.stack.pop_back(); throw; }
                a.stack.pop_back();
                // class_uid may grow a.objects: take the reference only afterwards.
                const auto class_ref = class_uid(a, archive_class, archived_name(archive_class));
                a.objects[uid].set("$class", uid_plist(class_ref));
                return uid;
            }
        }
        const auto class_ref = class_uid(a, s_.objc.find_class(name), name);   // may grow a.objects
        a.objects[uid].set("$class", uid_plist(class_ref));
        return uid;
    }
    Address current_coder{};   // the archiver whose state is being encoded (single-threaded per archiver)

    std::vector<std::uint8_t> finish(ArchiverState& a) {
        if (a.finished) return a.bytes;
        Plist root = dictionary_plist();
        root.set("$version", integer_plist(100000));
        root.set("$archiver", text_plist("NSKeyedArchiver"));
        root.set("$top", a.top);
        Plist objects;
        objects.kind = Plist::Kind::array;
        objects.items = a.objects;
        root.set("$objects", std::move(objects));
        if (a.xml) { const auto text = export_fill::plist_to_xml(root); a.bytes.assign(text.begin(), text.end()); }
        else a.bytes = export_fill::plist_to_binary(root);
        a.finished = true;
        return a.bytes;
    }

    // ---- unarchiving ----
    static std::optional<std::uint64_t> uid_of(const Plist* p) {
        if (!p) return std::nullopt;
        if (p->kind == Plist::Kind::uid) return static_cast<std::uint64_t>(p->integer);
        // XML archives: {CF$UID: n}.
        if (p->kind == Plist::Kind::dictionary && p->entries.size() == 1 && p->entries[0].first == "CF$UID" && p->entries[0].second.kind == Plist::Kind::integer)
            return static_cast<std::uint64_t>(p->entries[0].second.integer);
        return std::nullopt;
    }
    const Plist& current(UnarchiverState& u) {
        static const Plist empty = dictionary_plist();
        if (u.stack.empty()) { const auto top = u.archive.find("$top"); return top ? *top : empty; }
        return u.objects[u.stack.back()];
    }
    const Plist* value_for(UnarchiverState& u, Address key) {
        if (!key) return nullptr;
        return current(u).find(escape_key(s_.cf.utf8(key)));
    }
    bool allowed(UnarchiverState& u, Address cls) {
        if (!u.secure || u.allowed.empty()) return true;
        for (const auto permitted : u.allowed) if (is_subclass(cls, permitted)) return true;
        return false;
    }
    void corrupt(UnarchiverState& u, const std::string& why) {
        u.failed = true;
        if (!u.error) u.error = s_.send_id(error(4864, why), "retain");
    }
    // +1 object for `uid` (0: nil).
    Address decode(UnarchiverState& u, std::uint64_t uid, Address coder) {
        if (uid == 0) return 0;
        if (uid >= u.objects.size()) { corrupt(u, "object reference out of range"); return 0; }
        if (const auto found = u.decoded.find(uid); found != u.decoded.end()) return s_.send_id(found->second, "retain");
        const auto& p = u.objects[uid];
        Address made{};
        switch (p.kind) {
        case Plist::Kind::string: made = s_.cf.string(p.text); break;
        case Plist::Kind::integer: made = s_.cf.integer(p.integer); break;
        case Plist::Kind::real: made = s_.cf.real(p.real); break;
        case Plist::Kind::boolean: made = s_.cf.boolean(p.boolean); break;
        case Plist::Kind::data: made = s_.send_id(data_object(p.bytes, false), "retain"); break;
        case Plist::Kind::dictionary: made = decode_object(u, uid, p, coder); break;
        default: corrupt(u, "unexpected archive value"); return 0;
        }
        if (made && p.kind != Plist::Kind::dictionary) u.decoded.emplace(uid, s_.send_id(made, "retain"));
        return made;
    }
    std::vector<Address> decode_list(UnarchiverState& u, const Plist* list, Address coder) {
        std::vector<Address> out;
        if (!list || list->kind != Plist::Kind::array) { corrupt(u, "missing NS.objects / NS.keys"); return out; }
        for (const auto& item : list->items) {
            const auto uid = uid_of(&item);
            const auto value = uid ? decode(u, *uid, coder) : 0;
            if (!value) { for (const auto made : out) s_.release(made); corrupt(u, "nil collection element"); return {}; }
            out.push_back(value);
        }
        return out;
    }
    Address decode_object(UnarchiverState& u, std::uint64_t uid, const Plist& p, Address coder) {
        const auto class_ref = uid_of(p.find("$class"));
        if (!class_ref || *class_ref >= u.objects.size()) { corrupt(u, "object without $class"); return 0; }
        const auto& class_entry = u.objects[*class_ref];
        const auto* name_value = class_entry.find("$classname");
        if (!name_value || name_value->kind != Plist::Kind::string) { corrupt(u, "class without $classname"); return 0; }
        const auto& name = name_value->text;
        // The class: the unarchiver's mapping, else by name, else along $classes.
        Address cls{};
        if (const auto mapped = s_.send_id(unarchiver_class, "classForClassName:", {s_.send_id(s_.cf.string(name), "autorelease")})) cls = mapped;
        if (!cls) cls = s_.objc.find_class(name);
        if (!cls)
            if (const auto* chain = class_entry.find("$classes"); chain && chain->kind == Plist::Kind::array)
                for (const auto& entry : chain->items) if (entry.kind == Plist::Kind::string && (cls = s_.objc.find_class(entry.text))) break;
        if (!cls) { corrupt(u, "cannot decode object of class (" + name + ")"); return 0; }
        if (!allowed(u, cls)) {
            u.failed = true;
            if (!u.error) u.error = s_.send_id(error(4864, "value for key of class '" + name + "' was not among the allowed classes"), "retain");
            return 0;
        }
        const auto saved_allowed = u.allowed;
        Address made{};
        const auto list = [&](const char* key) { return decode_list(u, p.find(key), coder); };
        const auto plain = class_name(cls);
        if (name == "NSArray" || name == "NSMutableArray" || plain == "NSArray" || plain == "NSMutableArray") {
            auto values = list("NS.objects");
            made = s_.cf.array(values, name == "NSMutableArray");
            for (const auto value : values) s_.release(value);
        } else if (name == "NSDictionary" || name == "NSMutableDictionary") {
            auto keys = list("NS.keys");
            auto values = list("NS.objects");
            if (keys.size() != values.size()) corrupt(u, "NS.keys and NS.objects differ in length");
            std::vector<std::pair<Address, Address>> entries;
            for (std::size_t i = 0; i < std::min(keys.size(), values.size()); ++i) entries.emplace_back(keys[i], values[i]);
            made = s_.cf.dictionary(entries, name == "NSMutableDictionary");
            for (const auto value : keys) s_.release(value);
            for (const auto value : values) s_.release(value);
        } else if (name == "NSSet" || name == "NSMutableSet" || name == "NSOrderedSet" || name == "NSMutableOrderedSet") {
            auto values = list("NS.objects");
            const auto array = s_.send_id(s_.cf.array(values), "autorelease");
            for (const auto value : values) s_.release(value);
            const bool ordered = name.find("Ordered") != std::string::npos;
            made = s_.send_id(s_.send_id(cls, "alloc"), ordered ? "initWithArray:" : "initWithArray:", {array});
        } else if (name == "NSString" || name == "NSMutableString") {
            const auto* text = p.find("NS.string");
            const auto string = s_.cf.string(text && text->kind == Plist::Kind::string ? text->text : std::string());
            made = name == "NSMutableString" ? s_.cf.mutable_copy(string) : s_.retain(string);
            s_.release(string);
        } else if (name == "NSData" || name == "NSMutableData") {
            const auto* bytes = p.find("NS.bytes");
            if (!bytes) bytes = p.find("NS.data");
            const std::vector<std::uint8_t> empty;
            made = s_.send_id(data_object(bytes && bytes->kind == Plist::Kind::data ? bytes->bytes : empty, name == "NSMutableData"), "retain");
        } else if (name == "NSDate") {
            const auto* time = p.find("NS.time");
            made = s_.cf.date(time && (time->kind == Plist::Kind::real || time->kind == Plist::Kind::date) ? time->real : 0);
        } else if (name == "NSNull") {
            made = s_.send_id(s_.send_id(cls, "null"), "retain");
        } else if (name == "NSURL") {
            const auto relative_uid = uid_of(p.find("NS.relative"));
            const auto relative = relative_uid ? decode(u, *relative_uid, coder) : 0;
            const auto base_uid = uid_of(p.find("NS.base"));
            const auto base = base_uid ? decode(u, *base_uid, coder) : 0;
            made = relative ? s_.send_id(s_.send_id(s_.objc.find_class("NSURL"), "URLWithString:relativeToURL:", {relative, base}), "retain") : 0;
            s_.release(relative);
            s_.release(base);
        } else if (name == "NSValue") {
            unsupported("NSKeyedUnarchiver: NSValue objects");
        } else {
            // Secure decoding: the class must adopt NSSecureCoding, and every
            // class must decode itself (otherwise: nil and 4864, as Foundation).
            if (u.secure && !(s_.objc.responds(cls, s_.objc.selector("supportsSecureCoding")) && (s_.send(cls, "supportsSecureCoding").x0 & 0xff))) {
                u.allowed = saved_allowed;
                corrupt(u, "class '" + plain + "' does not adopt NSSecureCoding");
                return 0;
            }
            if (!(s_.send(cls, "instancesRespondToSelector:", [&] { CallRegs r; r.x[2] = s_.objc.selector("initWithCoder:"); return r; }()).x0 & 0xff)) {
                u.allowed = saved_allowed;
                corrupt(u, "class '" + plain + "' does not implement -initWithCoder:");
                return 0;
            }
            const auto allocated = s_.send_id(cls, "alloc");
            u.decoded.emplace(uid, s_.send_id(allocated, "retain"));   // cycles see the allocated object
            u.stack.push_back(uid);
            u.allowed.clear();
            Address result{};
            try { result = s_.send_id(allocated, "initWithCoder:", {coder}); } catch (...) { u.stack.pop_back(); u.allowed = saved_allowed; throw; }
            u.stack.pop_back();
            u.allowed = saved_allowed;
            if (result && s_.objc.responds(result, s_.objc.selector("awakeAfterUsingCoder:"))) {
                const auto replaced = s_.send_id(result, "awakeAfterUsingCoder:", {coder});
                if (replaced != result) { if (replaced) s_.send_id(replaced, "retain"); s_.release(result); result = replaced; }
            }
            if (const auto found = u.decoded.find(uid); found != u.decoded.end()) {
                s_.release(found->second);
                if (result) found->second = s_.send_id(result, "retain"); else u.decoded.erase(found);
            }
            return result;
        }
        u.allowed = saved_allowed;
        if (made) u.decoded.emplace(uid, s_.send_id(made, "retain"));
        return made;
    }
    bool load(UnarchiverState& u, Address data) {
        const auto bytes = data_bytes(data);
        auto parsed = export_fill::plist_parse(bytes);
        if (!parsed || parsed->kind != Plist::Kind::dictionary) { corrupt(u, "the data is not a property list"); return false; }
        const auto* objects = parsed->find("$objects");
        const auto* archiver = parsed->find("$archiver");
        if (!objects || objects->kind != Plist::Kind::array || !archiver || archiver->kind != Plist::Kind::string || !parsed->find("$top")) {
            corrupt(u, "the data is not a keyed archive");
            return false;
        }
        u.objects = objects->items;
        u.archive = std::move(*parsed);
        return true;
    }
};

} // namespace

void install_keyed_archive(Support& support) {
    auto archive = std::make_shared<Archive>(support);
    const auto coder = support.objc.find_class("NSCoder") ? "NSCoder" : "NSObject";
    const auto ka = support.declare_class("NSKeyedArchiver", coder, foundation_library);
    const auto ku = support.declare_class("NSKeyedUnarchiver", coder, foundation_library);
    archive->archiver_class = ka;
    archive->unarchiver_class = ku;
    auto* sp = &support;
    const auto archiver_of = [sp](Address self) { return sp->objc.host_state<ArchiverState>(self); };
    const auto unarchiver_of = [sp](Address self) { return sp->objc.host_state<UnarchiverState>(self); };
    for (const auto* string : {"NSKeyedArchiveRootObjectKey"})
        if (!support.process.resolve(foundation_library, std::string("_") + string)) support.string_constant(foundation_library, std::string("_") + string, "root");

    // ---- NSKeyedArchiver ----
    support.method(ka, "initRequiringSecureCoding:", [sp, ka, archiver_of](CpuState& s, GuestMemory&) {
        const auto self = sp->objc.send_super(s.x[0], ka, sp->objc.selector("init")).x0;
        if (self) archiver_of(self)->secure = (s.x[2] & 0xff) != 0;
        s.x[0] = self;
    }, "@@:B");
    support.method(ka, "init", [sp](CpuState& s, GuestMemory&) { CallRegs r; r.x[2] = 1; s.x[0] = sp->send(s.x[0], "initRequiringSecureCoding:", r).x0; }, "@@:");
    support.method(ka, "initForWritingWithMutableData:", [sp, archiver_of](CpuState& s, GuestMemory&) {
        CallRegs r; r.x[2] = 0;
        const auto self = sp->send(s.x[0], "initRequiringSecureCoding:", r).x0;
        if (self) archiver_of(self)->output_data = s.x[2];
        s.x[0] = self;
    }, "@@:@");
    support.method(ka, "allowsKeyedCoding", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    support.method(ka, "requiresSecureCoding", [archiver_of](CpuState& s, GuestMemory&) { s.x[0] = archiver_of(s.x[0])->secure; }, "B@:");
    support.method(ka, "setRequiresSecureCoding:", [archiver_of](CpuState& s, GuestMemory&) { archiver_of(s.x[0])->secure = (s.x[2] & 0xff) != 0; }, "v@:B");
    support.method(ka, "outputFormat", [archiver_of](CpuState& s, GuestMemory&) { s.x[0] = archiver_of(s.x[0])->xml ? 100 : 200; }, "Q@:");
    support.method(ka, "setOutputFormat:", [archiver_of](CpuState& s, GuestMemory&) {
        if (s.x[2] != 100 && s.x[2] != 200) unsupported("NSKeyedArchiver output format " + std::to_string(s.x[2]));
        archiver_of(s.x[0])->xml = s.x[2] == 100;
    }, "v@:Q");
    const auto encode_value = [archive, archiver_of](Address self, Address key, Plist value) {
        const auto state = archiver_of(self);
        if (state->finished) fail("NSKeyedArchiver: encoding after -finishEncoding");
        archive->put(*state, key, std::move(value));
    };
    support.method(ka, "encodeObject:forKey:", [archive, archiver_of](CpuState& s, GuestMemory&) {
        const auto state = archiver_of(s.x[0]);
        if (state->finished) fail("NSKeyedArchiver: encoding after -finishEncoding");
        const auto saved = archive->current_coder;
        archive->current_coder = s.x[0];
        std::uint64_t uid{};
        try { uid = archive->encode(*state, s.x[2]); } catch (...) { archive->current_coder = saved; throw; }
        archive->current_coder = saved;
        archive->put(*state, s.x[3], uid_plist(uid));
    }, "v@:@@");
    // Conditional: the object if it is (or will have been) encoded unconditionally; here: when already encoded.
    support.method(ka, "encodeConditionalObject:forKey:", [archive, archiver_of](CpuState& s, GuestMemory&) {
        const auto state = archiver_of(s.x[0]);
        const auto found = s.x[2] ? state->uids.find(s.x[2]) : state->uids.end();
        archive->put(*state, s.x[3], uid_plist(found != state->uids.end() ? found->second : 0));
    }, "v@:@@");
    support.method(ka, "encodeBool:forKey:", [encode_value](CpuState& s, GuestMemory&) {
        Plist p; p.kind = Plist::Kind::boolean; p.boolean = (s.x[2] & 0xff) != 0;
        encode_value(s.x[0], s.x[3], p);
    }, "v@:B@");
    for (const auto& [name, bits] : std::initializer_list<std::pair<const char*, int>>{{"encodeInt:forKey:", 32}, {"encodeInt32:forKey:", 32},
             {"encodeInt64:forKey:", 64}, {"encodeInteger:forKey:", 64}}) {
        const int width = bits;
        support.method(ka, name, [encode_value, width](CpuState& s, GuestMemory&) {
            const auto value = width == 32 ? static_cast<std::int64_t>(static_cast<std::int32_t>(s.x[2])) : static_cast<std::int64_t>(s.x[2]);
            encode_value(s.x[0], s.x[3], integer_plist(value));
        }, width == 32 ? "v@:i@" : "v@:q@");
    }
    support.method(ka, "encodeDouble:forKey:", [encode_value](CpuState& s, GuestMemory&) {
        Plist p; p.kind = Plist::Kind::real; p.real = std::bit_cast<double>(s.v[0][0]);
        encode_value(s.x[0], s.x[2], p);
    }, "v@:d@");
    support.method(ka, "encodeFloat:forKey:", [encode_value](CpuState& s, GuestMemory&) {
        Plist p; p.kind = Plist::Kind::real; p.real = std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0]));
        encode_value(s.x[0], s.x[2], p);
    }, "v@:f@");
    support.method(ka, "encodeBytes:length:forKey:", [sp, encode_value](CpuState& s, GuestMemory& m) {
        Plist p; p.kind = Plist::Kind::data;
        p.bytes.resize(static_cast<std::size_t>(s.x[3]));
        if (s.x[3]) m.read_bytes(s.x[2], p.bytes);
        encode_value(s.x[0], s.x[4], std::move(p));
        static_cast<void>(sp);
    }, "v@:r*Q@");
    const auto number_text = [](double value) {
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, "%.17g", value);
        return std::string(buffer);
    };
    support.method(ka, "encodePoint:forKey:", [encode_value, number_text](CpuState& s, GuestMemory&) {
        encode_value(s.x[0], s.x[2], text_plist("{" + number_text(std::bit_cast<double>(s.v[0][0])) + ", " + number_text(std::bit_cast<double>(s.v[1][0])) + "}"));
    }, "v@:{CGPoint=dd}@");
    support.method(ka, "encodeSize:forKey:", [encode_value, number_text](CpuState& s, GuestMemory&) {
        encode_value(s.x[0], s.x[2], text_plist("{" + number_text(std::bit_cast<double>(s.v[0][0])) + ", " + number_text(std::bit_cast<double>(s.v[1][0])) + "}"));
    }, "v@:{CGSize=dd}@");
    support.method(ka, "encodeRect:forKey:", [encode_value, number_text](CpuState& s, GuestMemory&) {
        encode_value(s.x[0], s.x[2], text_plist("{{" + number_text(std::bit_cast<double>(s.v[0][0])) + ", " + number_text(std::bit_cast<double>(s.v[1][0])) + "}, {" +
                                                 number_text(std::bit_cast<double>(s.v[2][0])) + ", " + number_text(std::bit_cast<double>(s.v[3][0])) + "}}"));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    support.method(ka, "finishEncoding", [archive, archiver_of](CpuState& s, GuestMemory&) {
        const auto state = archiver_of(s.x[0]);
        const bool first = !state->finished;
        const auto bytes = archive->finish(*state);
        if (first && state->output_data) {
            CallRegs append;
            const auto data = archive->data_object(bytes, false);
            append.x[2] = data;
            static_cast<void>(archive->s_.send(state->output_data, "appendData:", append));
        }
    }, "v@:");
    support.method(ka, "encodedData", [archive, archiver_of](CpuState& s, GuestMemory&) {
        s.x[0] = archive->data_object(archive->finish(*archiver_of(s.x[0])), false);
    }, "@@:");
    // Autoreleased data, or 0 with `error` set (collect_errors).
    const auto archive_root = [sp, ka, archiver_of](Address root, bool secure, bool collect_errors, std::string* error) {
        CallRegs r;
        r.x[2] = secure;
        const auto archiver = sp->send(sp->send_id(ka, "alloc"), "initRequiringSecureCoding:", r).x0;
        Address data{};
        try {
            archiver_of(archiver)->collect_errors = collect_errors;
            sp->send_id(archiver, "encodeObject:forKey:", {root, sp->cf.constant_string("root")});
            if (const auto& why = archiver_of(archiver)->error; !why.empty()) {
                if (error) *error = why;
                sp->send_id(archiver, "release");
                return Address{0};
            }
            data = sp->send_id(sp->send_id(archiver, "encodedData"), "retain");
        } catch (...) { sp->send_id(archiver, "release"); throw; }
        sp->send_id(archiver, "release");
        return sp->send_id(data, "autorelease");
    };
    support.method(ka, "archivedDataWithRootObject:requiringSecureCoding:error:", [archive, archive_root](CpuState& s, GuestMemory& m) {
        if (s.x[4]) m.write64(s.x[4], 0);
        std::string why;
        s.x[0] = archive_root(s.x[2], (s.x[3] & 0xff) != 0, true, &why);
        // NSCoderInvalidValueError (4866).
        if (!s.x[0] && s.x[4]) m.write64(s.x[4], archive->error(4866, why));
    }, "@@:@B^@", true);
    support.method(ka, "archivedDataWithRootObject:", [archive_root](CpuState& s, GuestMemory&) { s.x[0] = archive_root(s.x[2], false, false, nullptr); }, "@@:@", true);
    support.method(ka, "archiveRootObject:toFile:", [sp, archive_root](CpuState& s, GuestMemory&) {
        const auto data = archive_root(s.x[2], false, false, nullptr);
        CallRegs write;
        write.x[2] = s.x[3];
        write.x[3] = 1;
        s.x[0] = sp->send(data, "writeToFile:atomically:", write).x0 & 0xff;
    }, "B@:@@", true);
    support.method(ka, "dealloc", [sp, ka](CpuState& s, GuestMemory&) {
        static_cast<void>(sp->objc.send_super(s.x[0], ka, sp->objc.selector("dealloc")));
    }, "v@:");

    // ---- NSKeyedUnarchiver ----
    support.method(ku, "initForReadingFromData:error:", [sp, ku, archive, unarchiver_of](CpuState& s, GuestMemory& m) {
        const auto self = sp->objc.send_super(s.x[0], ku, sp->objc.selector("init")).x0;
        s.x[0] = self;
        if (!self) return;
        const auto state = unarchiver_of(self);
        state->secure = true;
        if (!s.x[2] || !archive->load(*state, s.x[2])) {
            if (s.x[3]) m.write64(s.x[3], state->error ? sp->send_id(sp->send_id(state->error, "retain"), "autorelease") : 0);
            sp->send_id(self, "release");
            s.x[0] = 0;
        }
    }, "@@:@^@");
    support.method(ku, "initForReadingWithData:", [sp](CpuState& s, GuestMemory&) {
        CallRegs r;
        r.x[2] = s.x[2];
        r.x[3] = 0;
        const auto self = sp->send(s.x[0], "initForReadingFromData:error:", r).x0;
        if (!self) fail("NSInvalidArgumentException: -[NSKeyedUnarchiver initForReadingWithData:] incomprehensible archive");
        sp->objc.host_state<UnarchiverState>(self)->secure = false;
        s.x[0] = self;
    }, "@@:@");
    support.method(ku, "allowsKeyedCoding", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    support.method(ku, "requiresSecureCoding", [unarchiver_of](CpuState& s, GuestMemory&) { s.x[0] = unarchiver_of(s.x[0])->secure; }, "B@:");
    support.method(ku, "setRequiresSecureCoding:", [unarchiver_of](CpuState& s, GuestMemory&) { unarchiver_of(s.x[0])->secure = (s.x[2] & 0xff) != 0; }, "v@:B");
    support.method(ku, "decodingFailurePolicy", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "q@:");   // NSDecodingFailurePolicySetErrorAndReturn
    support.method(ku, "setDecodingFailurePolicy:", [](CpuState&, GuestMemory&) {}, "v@:q");
    support.method(ku, "error", [unarchiver_of](CpuState& s, GuestMemory&) { s.x[0] = unarchiver_of(s.x[0])->error; }, "@@:");
    support.method(ku, "finishDecoding", [](CpuState&, GuestMemory&) {}, "v@:");
    support.method(ku, "containsValueForKey:", [archive, unarchiver_of](CpuState& s, GuestMemory&) {
        s.x[0] = archive->value_for(*unarchiver_of(s.x[0]), s.x[2]) != nullptr;
    }, "B@:@");
    // Objects (autoreleased); `classes` (an NSArray / NSSet of classes, or one class) are the allowed ones.
    const auto decode_object_for = [sp, archive, unarchiver_of](Address self, Address key, std::vector<Address> classes) -> Address {
        const auto state = unarchiver_of(self);
        const auto* value = archive->value_for(*state, key);
        if (!value) return 0;
        const auto uid = Archive::uid_of(value);
        if (!uid) { archive->corrupt(*state, "a value that is not an object reference"); return 0; }
        const auto saved = state->allowed;
        if (!classes.empty()) state->allowed = classes;
        Address made{};
        try { made = archive->decode(*state, *uid, self); } catch (...) { state->allowed = saved; throw; }
        state->allowed = saved;
        return made ? sp->send_id(made, "autorelease") : 0;
    };
    // The classes of an NSArray / NSSet (by fast enumeration: CF arrays cannot
    // hold class objects, so -allObjects of a set of classes fails), or one class.
    const auto class_list = [sp](Address value) {
        std::vector<Address> out;
        if (!value) return out;
        if (!sp->objc.responds(value, sp->objc.selector("countByEnumeratingWithState:objects:count:"))) { out.push_back(value); return out; }
        const auto state = sp->process.allocate_guest(64 + 16 * 8);   // NSFastEnumerationState + a 16-object buffer
        const auto buffer = state + 64;
        try {
            for (std::size_t i = 0; i < 8; ++i) sp->memory().write64(state + i * 8, 0);
            for (;;) {
                CallRegs call;
                call.x[2] = state;
                call.x[3] = buffer;
                call.x[4] = 16;
                const auto count = sp->send(value, "countByEnumeratingWithState:objects:count:", call).x0;
                if (!count) break;
                const auto items = sp->memory().read64(state + 8);
                for (Address i = 0; i < count; ++i) out.push_back(sp->memory().read64(items + i * 8));
            }
        } catch (...) { sp->process.release_guest(state); throw; }
        sp->process.release_guest(state);
        return out;
    };
    support.method(ku, "decodeObjectForKey:", [decode_object_for](CpuState& s, GuestMemory&) { s.x[0] = decode_object_for(s.x[0], s.x[2], {}); }, "@@:@");
    support.method(ku, "decodeObjectOfClass:forKey:", [decode_object_for](CpuState& s, GuestMemory&) { s.x[0] = decode_object_for(s.x[0], s.x[3], {s.x[2]}); }, "@@:#@");
    support.method(ku, "decodeObjectOfClasses:forKey:", [decode_object_for, class_list](CpuState& s, GuestMemory&) { s.x[0] = decode_object_for(s.x[0], s.x[3], class_list(s.x[2])); }, "@@:@@");
    support.method(ku, "decodeArrayOfObjectsOfClass:forKey:", [sp, decode_object_for](CpuState& s, GuestMemory&) {
        s.x[0] = decode_object_for(s.x[0], s.x[3], {sp->objc.find_class("NSArray"), s.x[2]});
    }, "@@:#@");
    support.method(ku, "decodeDictionaryWithKeysOfClass:objectsOfClass:forKey:", [sp, decode_object_for](CpuState& s, GuestMemory&) {
        s.x[0] = decode_object_for(s.x[0], s.x[4], {sp->objc.find_class("NSDictionary"), s.x[2], s.x[3]});
    }, "@@:##@");
    support.method(ku, "decodeTopLevelObjectForKey:error:", [decode_object_for, unarchiver_of](CpuState& s, GuestMemory& m) {
        s.x[0] = decode_object_for(s.x[0], s.x[2], {});
        if (s.x[3]) m.write64(s.x[3], s.x[0] ? 0 : unarchiver_of(s.x[0])->error);
    }, "@@:@^@");
    const auto scalar = [archive, unarchiver_of](Address self, Address key) -> const Plist* {
        return archive->value_for(*unarchiver_of(self), key);
    };
    support.method(ku, "decodeBoolForKey:", [scalar](CpuState& s, GuestMemory&) {
        const auto* p = scalar(s.x[0], s.x[2]);
        s.x[0] = p && ((p->kind == Plist::Kind::boolean && p->boolean) || (p->kind == Plist::Kind::integer && p->integer != 0));
    }, "B@:@");
    for (const auto* name : {"decodeIntForKey:", "decodeInt32ForKey:", "decodeInt64ForKey:", "decodeIntegerForKey:"})
        support.method(ku, name, [scalar](CpuState& s, GuestMemory&) {
            const auto* p = scalar(s.x[0], s.x[2]);
            s.x[0] = !p ? 0 : p->kind == Plist::Kind::integer ? static_cast<Address>(p->integer)
                         : p->kind == Plist::Kind::real ? static_cast<Address>(static_cast<std::int64_t>(p->real))
                         : p->kind == Plist::Kind::boolean ? static_cast<Address>(p->boolean) : 0;
        }, "q@:@");
    const auto real_of = [scalar](Address self, Address key) {
        const auto* p = scalar(self, key);
        return !p ? 0.0 : p->kind == Plist::Kind::real ? p->real : p->kind == Plist::Kind::integer ? static_cast<double>(p->integer) : 0.0;
    };
    support.method(ku, "decodeDoubleForKey:", [real_of](CpuState& s, GuestMemory&) { s.v[0] = {std::bit_cast<std::uint64_t>(real_of(s.x[0], s.x[2])), 0}; }, "d@:@");
    support.method(ku, "decodeFloatForKey:", [real_of](CpuState& s, GuestMemory&) {
        s.v[0] = {std::bit_cast<std::uint32_t>(static_cast<float>(real_of(s.x[0], s.x[2]))), 0};
    }, "f@:@");
    support.method(ku, "decodeBytesForKey:returnedLength:", [sp, scalar, unarchiver_of](CpuState& s, GuestMemory& m) {
        const auto* p = scalar(s.x[0], s.x[2]);
        if (!p || p->kind != Plist::Kind::data) { if (s.x[3]) m.write64(s.x[3], 0); s.x[0] = 0; return; }
        const auto block = sp->process.allocate_guest(std::max<std::size_t>(p->bytes.size(), 1));
        if (!p->bytes.empty()) m.write_bytes(block, p->bytes);
        unarchiver_of(s.x[0])->buffers.push_back(block);
        if (s.x[3]) m.write64(s.x[3], p->bytes.size());
        s.x[0] = block;
    }, "r*@:@^Q");
    // Geometry strings "{x, y}" / "{{x, y}, {w, h}}".
    const auto numbers_in = [scalar](Address self, Address key) {
        std::vector<double> out;
        const auto* p = scalar(self, key);
        if (!p || p->kind != Plist::Kind::string) return out;
        std::string token;
        for (const char c : p->text + ",") {
            if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == 'e' || c == 'E' || c == '+') token.push_back(c);
            else if (!token.empty()) { try { out.push_back(std::stod(token)); } catch (...) {} token.clear(); }
        }
        return out;
    };
    for (const auto* name : {"decodePointForKey:", "decodeSizeForKey:"})
        support.method(ku, name, [numbers_in](CpuState& s, GuestMemory&) {
            const auto n = numbers_in(s.x[0], s.x[2]);
            for (unsigned i = 0; i < 2; ++i) s.v[i] = {std::bit_cast<std::uint64_t>(i < n.size() ? n[i] : 0.0), 0};
        }, "{CGPoint=dd}@:@");
    support.method(ku, "decodeRectForKey:", [numbers_in](CpuState& s, GuestMemory&) {
        const auto n = numbers_in(s.x[0], s.x[2]);
        for (unsigned i = 0; i < 4; ++i) s.v[i] = {std::bit_cast<std::uint64_t>(i < n.size() ? n[i] : 0.0), 0};
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:@");
    // Factories.
    const auto unarchive_root = [sp, ku](Address data, std::vector<Address> classes, bool secure, Address error_out) -> Address {
        if (error_out) sp->memory().write64(error_out, 0);
        CallRegs init;
        init.x[2] = data;
        init.x[3] = error_out;
        const auto unarchiver = sp->send(sp->send_id(ku, "alloc"), "initForReadingFromData:error:", init).x0;
        if (!unarchiver) return 0;
        auto& state = *sp->objc.host_state<UnarchiverState>(unarchiver);
        state.secure = secure;
        Address result{};
        try {
            const auto key = sp->cf.constant_string("root");
            if (classes.empty()) result = sp->send_id(unarchiver, "decodeObjectForKey:", {key});
            else {
                state.allowed = classes;
                result = sp->send_id(unarchiver, "decodeObjectForKey:", {key});
            }
            if (result) result = sp->send_id(result, "retain");
            if (!result && error_out && state.error) sp->memory().write64(error_out, sp->send_id(sp->send_id(state.error, "retain"), "autorelease"));
        } catch (...) { sp->send_id(unarchiver, "release"); throw; }
        sp->send_id(unarchiver, "release");
        return result ? sp->send_id(result, "autorelease") : 0;
    };
    support.method(ku, "unarchivedObjectOfClass:fromData:error:", [unarchive_root](CpuState& s, GuestMemory&) {
        s.x[0] = unarchive_root(s.x[3], {s.x[2]}, true, s.x[4]);
    }, "@@:#@^@", true);
    support.method(ku, "unarchivedObjectOfClasses:fromData:error:", [unarchive_root, class_list](CpuState& s, GuestMemory&) {
        s.x[0] = unarchive_root(s.x[3], class_list(s.x[2]), true, s.x[4]);
    }, "@@:@@^@", true);
    support.method(ku, "unarchivedArrayOfObjectsOfClass:fromData:error:", [sp, unarchive_root](CpuState& s, GuestMemory&) {
        s.x[0] = unarchive_root(s.x[3], {sp->objc.find_class("NSArray"), s.x[2]}, true, s.x[4]);
    }, "@@:#@^@", true);
    support.method(ku, "unarchiveObjectWithData:", [unarchive_root](CpuState& s, GuestMemory&) { s.x[0] = unarchive_root(s.x[2], {}, false, 0); }, "@@:@", true);
    support.method(ku, "unarchiveTopLevelObjectWithData:error:", [unarchive_root](CpuState& s, GuestMemory&) {
        s.x[0] = unarchive_root(s.x[2], {}, false, s.x[3]);
    }, "@@:@^@", true);
    support.method(ku, "unarchiveObjectWithFile:", [sp, unarchive_root](CpuState& s, GuestMemory&) {
        const auto data = sp->send_id(sp->objc.find_class("NSData"), "dataWithContentsOfFile:", {s.x[2]});
        s.x[0] = data ? unarchive_root(data, {}, false, 0) : 0;
    }, "@@:@", true);
    support.method(ku, "dealloc", [sp, ku](CpuState& s, GuestMemory&) {
        if (const auto state = sp->objc.find_host_state<UnarchiverState>(s.x[0])) {
            for (const auto& [uid, object] : std::exchange(state->decoded, {})) sp->send_id(object, "release");
            for (const auto buffer : std::exchange(state->buffers, {})) sp->process.release_guest(buffer);
            if (state->error) sp->send_id(std::exchange(state->error, 0), "release");
        }
        static_cast<void>(sp->objc.send_super(s.x[0], ku, sp->objc.selector("dealloc")));
    }, "v@:");
}

} // namespace alchemy::gui::completion_kit
