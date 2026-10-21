#include "alchemy/darwin/appkit/nib_archive.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <stdexcept>
#include <utility>

// Keyed-archive nibs: NSKeyedArchiver binary property lists converted to the
// NIBArchive model of nib_archive.hpp (owner: AppKit).
//
// Binary property list ("bplist00"), project-authored from the format
// description: the header, objects, an offset table and a 32-byte trailer
// (6 unused bytes, offset size, object reference size, then big-endian
// uint64 object count, top object, offset table offset). Object markers
// (high nibble type, low nibble size or 0xF followed by an integer object
// holding the size): 0x00 null, 0x08 false, 0x09 true, 0x1n integer (2^n
// big-endian bytes), 0x2n real (2^n bytes), 0x33 date, 0x4n data, 0x5n ASCII
// string, 0x6n UTF-16BE string (count of code units), 0x7n UTF-8 string,
// 0x8n UID (n + 1 bytes), 0xAn array, 0xCn set, 0xDn dictionary (key
// references then value references).
//
// NSKeyedArchiver: {"$archiver": "NSKeyedArchiver", "$version", "$top":
// {key: UID}, "$objects": [...]}; UID n is $objects[n] ($objects[0] is
// "$null"); objects are dictionaries with "$class" (a UID of {"$classname",
// "$classes"}) plus their keys.
//
// Input is untrusted: every offset, size and reference is bounds-checked and
// any inconsistency throws std::runtime_error (no partial archives).
namespace alchemy::gui::nib {
namespace {

[[noreturn]] void bad(const std::string& what) { throw std::runtime_error("Alchemy keyed nib: " + what); }

struct Node {
    enum class Kind : std::uint8_t { null, boolean, integer, real, date, data, string, uid, array, set, dict } kind{Kind::null};
    bool boolean{};
    std::int64_t integer{};
    double real{};
    std::uint64_t uid{};
    std::vector<std::uint8_t> bytes;              // data
    std::string text;                             // strings (UTF-8)
    std::vector<std::uint64_t> items;             // array / set: object references
    std::vector<std::pair<std::uint64_t, std::uint64_t>> entries;   // dict: key, value references
};

void append_utf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) out += static_cast<char>(code);
    else if (code < 0x800) { out += static_cast<char>(0xc0 | (code >> 6)); out += static_cast<char>(0x80 | (code & 0x3f)); }
    else if (code < 0x10000) {
        out += static_cast<char>(0xe0 | (code >> 12)); out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (code & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (code >> 18)); out += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3f)); out += static_cast<char>(0x80 | (code & 0x3f));
    }
}

class BinaryPlist {
public:
    explicit BinaryPlist(std::span<const std::uint8_t> bytes) : bytes_(bytes) {
        if (bytes.size() < 8 + 32 || std::memcmp(bytes.data(), "bplist00", 8) != 0) bad("not a binary property list");
        const auto* trailer = bytes.data() + bytes.size() - 32;
        offset_size_ = trailer[6];
        ref_size_ = trailer[7];
        const auto count = be(trailer + 8, 8), top = be(trailer + 16, 8), table = be(trailer + 24, 8);
        if (offset_size_ < 1 || offset_size_ > 8 || ref_size_ < 1 || ref_size_ > 8) bad("trailer sizes");
        const auto limit = bytes.size() - 32;
        if (count == 0 || count > limit || table < 8 || table > limit || (limit - table) / offset_size_ < count) bad("offset table");
        if (top >= count) bad("top object");
        top_ = top;
        nodes_.resize(static_cast<std::size_t>(count));
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto offset = be(bytes.data() + table + i * offset_size_, offset_size_);
            if (offset < 8 || offset >= table) bad("object offset");
            nodes_[static_cast<std::size_t>(i)] = parse_node(static_cast<std::size_t>(offset), static_cast<std::size_t>(table));
        }
        for (const auto& node : nodes_) {
            for (const auto ref : node.items) if (ref >= count) bad("object reference");
            for (const auto& [k, v] : node.entries) if (k >= count || v >= count) bad("object reference");
        }
    }
    [[nodiscard]] const Node& node(std::uint64_t index) const {
        if (index >= nodes_.size()) bad("object reference");
        return nodes_[static_cast<std::size_t>(index)];
    }
    [[nodiscard]] const Node& top() const { return node(top_); }
    // The value of `key` in dictionary `dict` (nullptr when absent).
    [[nodiscard]] const Node* lookup(const Node& dict, std::string_view key) const {
        if (dict.kind != Node::Kind::dict) bad("expected a dictionary");
        for (const auto& [k, v] : dict.entries) {
            const auto& name = node(k);
            if (name.kind == Node::Kind::string && name.text == key) return &node(v);
        }
        return nullptr;
    }

private:
    static std::uint64_t be(const std::uint8_t* p, std::size_t n) {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < n; ++i) value = (value << 8) | p[i];
        return value;
    }
    Node parse_node(std::size_t at, std::size_t end) const {
        const auto need = [&](std::size_t from, std::size_t n) {
            if (from > end || end - from < n) bad("object runs past the offset table");
        };
        need(at, 1);
        const auto marker = bytes_[at];
        const auto type = marker >> 4, low = marker & 0x0f;
        std::size_t cursor = at + 1;
        // Element / byte count: the low nibble or an integer object after it.
        const auto count = [&]() -> std::size_t {
            if (low != 0x0f) return low;
            need(cursor, 1);
            const auto int_marker = bytes_[cursor];
            if ((int_marker >> 4) != 0x1) bad("malformed length");
            const std::size_t width = std::size_t{1} << (int_marker & 0x0f);
            if (width > 8) bad("length too large");
            need(cursor + 1, width);
            const auto value = be(bytes_.data() + cursor + 1, width);
            cursor += 1 + width;
            if (value > bytes_.size()) bad("length too large");
            return static_cast<std::size_t>(value);
        };
        Node node;
        switch (type) {
        case 0x0:
            if (marker == 0x00) node.kind = Node::Kind::null;
            else if (marker == 0x08 || marker == 0x09) { node.kind = Node::Kind::boolean; node.boolean = marker == 0x09; }
            else bad("unknown marker");
            break;
        case 0x1: {
            const std::size_t width = std::size_t{1} << low;
            if (width > 16) bad("integer width");
            need(cursor, width);
            node.kind = Node::Kind::integer;
            // 16-byte integers keep their low 64 bits; 8-byte ones are signed.
            node.integer = static_cast<std::int64_t>(be(bytes_.data() + cursor + (width > 8 ? width - 8 : 0), std::min<std::size_t>(width, 8)));
            break;
        }
        case 0x2: case 0x3: {
            if (type == 0x3 && marker != 0x33) bad("unknown marker");
            const std::size_t width = type == 0x3 ? 8 : std::size_t{1} << low;
            if (width != 4 && width != 8) bad("real width");
            need(cursor, width);
            const auto raw = be(bytes_.data() + cursor, width);
            if (width == 4) { float f; const auto bits = static_cast<std::uint32_t>(raw); std::memcpy(&f, &bits, 4); node.real = f; }
            else std::memcpy(&node.real, &raw, 8);
            node.kind = type == 0x3 ? Node::Kind::date : Node::Kind::real;
            break;
        }
        case 0x4: {
            const auto n = count();
            need(cursor, n);
            node.kind = Node::Kind::data;
            node.bytes.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(cursor), bytes_.begin() + static_cast<std::ptrdiff_t>(cursor + n));
            break;
        }
        case 0x5: case 0x7: {
            const auto n = count();
            need(cursor, n);
            node.kind = Node::Kind::string;
            node.text.assign(reinterpret_cast<const char*>(bytes_.data() + cursor), n);
            break;
        }
        case 0x6: {
            const auto n = count();
            if (n > (end - cursor) / 2) bad("string runs past the offset table");
            node.kind = Node::Kind::string;
            for (std::size_t i = 0; i < n; ++i) {
                std::uint32_t unit = static_cast<std::uint32_t>(be(bytes_.data() + cursor + i * 2, 2));
                if (unit >= 0xd800 && unit < 0xdc00 && i + 1 < n) {
                    const auto next = static_cast<std::uint32_t>(be(bytes_.data() + cursor + (i + 1) * 2, 2));
                    if (next >= 0xdc00 && next < 0xe000) { unit = 0x10000 + ((unit - 0xd800) << 10) + (next - 0xdc00); ++i; }
                }
                append_utf8(node.text, unit);
            }
            break;
        }
        case 0x8: {
            const std::size_t width = static_cast<std::size_t>(low) + 1;
            if (width > 8) bad("UID width");
            need(cursor, width);
            node.kind = Node::Kind::uid;
            node.uid = be(bytes_.data() + cursor, width);
            break;
        }
        case 0xa: case 0xc: case 0xd: {
            const auto n = count();
            const auto refs = type == 0xd ? n * 2 : n;
            if (refs > (end - cursor) / ref_size_) bad("collection runs past the offset table");
            std::vector<std::uint64_t> all(refs);
            for (std::size_t i = 0; i < refs; ++i) all[i] = be(bytes_.data() + cursor + i * ref_size_, ref_size_);
            if (type == 0xd) {
                node.kind = Node::Kind::dict;
                for (std::size_t i = 0; i < n; ++i) node.entries.emplace_back(all[i], all[n + i]);
            } else {
                node.kind = type == 0xa ? Node::Kind::array : Node::Kind::set;
                node.items = std::move(all);
            }
            break;
        }
        default: bad("unknown marker");
        }
        return node;
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t offset_size_{}, ref_size_{};
    std::uint64_t top_{};
    std::vector<Node> nodes_;
};

// $objects -> archive objects (a worklist: references are declared first and
// filled later, so cycles and depth are no problem).
class Converter {
public:
    explicit Converter(const BinaryPlist& plist) : plist_(plist) {
        const auto& top = plist.top();
        if (top.kind != Node::Kind::dict) bad("the archive is not a dictionary");
        const auto* archiver = plist.lookup(top, "$archiver");
        if (!archiver || archiver->kind != Node::Kind::string || archiver->text != "NSKeyedArchiver") bad("not an NSKeyedArchiver archive");
        const auto* objects = plist.lookup(top, "$objects");
        const auto* roots = plist.lookup(top, "$top");
        if (!objects || objects->kind != Node::Kind::array || objects->items.empty()) bad("missing $objects");
        if (!roots || roots->kind != Node::Kind::dict) bad("missing $top");
        objects_ = &objects->items;
        // Object 0: the archive root holding $top's keys ("IB.objectdata").
        const auto root = declare("NSObject");
        for (const auto& [k, v] : roots->entries) put_value(root, key_text(k), plist.node(v));
        while (!work_.empty()) {
            const auto [index, uid] = work_.back();
            work_.pop_back();
            fill(index, plist.node((*objects_)[static_cast<std::size_t>(uid)]));
        }
    }
    Archive finish() {
        archive_.values.clear();
        for (std::size_t i = 0; i < archive_.objects.size(); ++i) {
            archive_.objects[i].first_value = static_cast<std::uint32_t>(archive_.values.size());
            archive_.objects[i].value_count = static_cast<std::uint32_t>(pending_[i].size());
            archive_.values.insert(archive_.values.end(), pending_[i].begin(), pending_[i].end());
        }
        archive_.format_version = 1;
        archive_.coder_version = 10;
        return std::move(archive_);
    }

private:
    std::string key_text(std::uint64_t ref) const {
        const auto& node = plist_.node(ref);
        if (node.kind != Node::Kind::string) bad("non-string dictionary key");
        return node.text;
    }
    std::uint32_t class_index(const std::string& name) {
        if (const auto found = classes_.find(name); found != classes_.end()) return found->second;
        archive_.class_names.push_back(name);
        return classes_[name] = static_cast<std::uint32_t>(archive_.class_names.size() - 1);
    }
    std::uint32_t key_index(const std::string& name) {
        if (const auto found = keys_.find(name); found != keys_.end()) return found->second;
        archive_.keys.push_back(name);
        return keys_[name] = static_cast<std::uint32_t>(archive_.keys.size() - 1);
    }
    std::uint32_t declare(const std::string& cls) {
        if (archive_.objects.size() >= 4u * objects_->size() + 16) bad("too many objects");
        archive_.objects.push_back({class_index(cls), 0, 0});
        pending_.emplace_back();
        return static_cast<std::uint32_t>(archive_.objects.size() - 1);
    }
    void put(std::uint32_t object, const std::string& key, Value value) {
        value.key = key_index(key);
        pending_.at(object).push_back(std::move(value));
    }
    void put_ref(std::uint32_t object, const std::string& key, std::optional<std::uint32_t> target) {
        Value v;
        if (target) { v.type = ValueType::object; v.integer = *target; }
        else v.type = ValueType::nil;
        put(object, key, std::move(v));
    }
    std::uint32_t string_object(const std::string& text) {
        const auto object = declare("NSString");
        Value v;
        v.type = ValueType::data;
        v.data.assign(text.begin(), text.end());
        put(object, "NS.bytes", std::move(v));
        return object;
    }
    // The archive object of $objects[uid] (nullopt: $null).
    std::optional<std::uint32_t> object_for(std::uint64_t uid) {
        if (uid == 0) return std::nullopt;
        if (uid >= objects_->size()) bad("UID out of range");
        if (const auto found = mapped_.find(uid); found != mapped_.end()) return found->second;
        const auto& node = plist_.node((*objects_)[static_cast<std::size_t>(uid)]);
        std::string cls;
        switch (node.kind) {
        case Node::Kind::dict: cls = class_name(node); break;
        case Node::Kind::string: cls = "NSString"; break;
        case Node::Kind::integer: case Node::Kind::real: case Node::Kind::boolean: cls = "NSNumber"; break;
        case Node::Kind::data: cls = "NSData"; break;
        case Node::Kind::null: return std::nullopt;
        default: bad("unsupported $objects entry");
        }
        // Strings, collections and dictionaries with NS.string are stored the
        // NIBArchive way (see fill).
        if (cls == "NSMutableString") cls = "NSString";
        const auto index = declare(cls);
        mapped_.emplace(uid, index);
        work_.emplace_back(index, uid);
        return index;
    }
    std::string class_name(const Node& dict) {
        const auto* ref = plist_.lookup(dict, "$class");
        if (!ref || ref->kind != Node::Kind::uid || ref->uid == 0 || ref->uid >= objects_->size()) bad("object without a $class");
        const auto& cls = plist_.node((*objects_)[static_cast<std::size_t>(ref->uid)]);
        if (cls.kind != Node::Kind::dict) bad("malformed $class");
        const auto* name = plist_.lookup(cls, "$classname");
        if (!name || name->kind != Node::Kind::string || name->text.empty()) bad("$class without $classname");
        return name->text;
    }
    // One keyed value of `object`.
    void put_value(std::uint32_t object, const std::string& key, const Node& node) {
        Value v;
        switch (node.kind) {
        case Node::Kind::uid: put_ref(object, key, object_for(node.uid)); return;
        case Node::Kind::null: v.type = ValueType::nil; break;
        case Node::Kind::boolean: v.type = node.boolean ? ValueType::true_ : ValueType::false_; v.integer = node.boolean; break;
        case Node::Kind::integer: v.type = ValueType::int64; v.integer = node.integer; break;
        case Node::Kind::real: case Node::Kind::date: v.type = ValueType::float64; v.real = node.real; break;
        case Node::Kind::data: v.type = ValueType::data; v.data = node.bytes; break;
        // Encoded geometry (encodeRect: / encodePoint: / encodeSize:) and other inline strings.
        case Node::Kind::string: put_ref(object, key, string_object(node.text)); return;
        case Node::Kind::array: case Node::Kind::set: {
            const auto array = declare(node.kind == Node::Kind::array ? "NSArray" : "NSSet");
            put_elements(array, node.items);
            put_ref(object, key, array);
            return;
        }
        case Node::Kind::dict: bad("inline dictionary value for key " + key);
        }
        put(object, key, std::move(v));
    }
    // Collection elements: UID references (or inline values) as UINibEncoderEmptyKey.
    void put_elements(std::uint32_t object, const std::vector<std::uint64_t>& refs) {
        Value inlined;
        inlined.type = ValueType::false_;
        put(object, "NSInlinedValue", std::move(inlined));
        for (const auto ref : refs) put_value(object, "UINibEncoderEmptyKey", plist_.node(ref));
    }
    const Node& member(const Node& dict, std::string_view key) {
        const auto* found = plist_.lookup(dict, key);
        if (!found) bad("missing " + std::string(key));
        return *found;
    }
    void fill(std::uint32_t index, const Node& node) {
        Value v;
        switch (node.kind) {
        case Node::Kind::string:
            v.type = ValueType::data;
            v.data.assign(node.text.begin(), node.text.end());
            put(index, "NS.bytes", std::move(v));
            return;
        case Node::Kind::integer: v.type = ValueType::int64; v.integer = node.integer; put(index, "NS.intval", std::move(v)); return;
        case Node::Kind::real: v.type = ValueType::float64; v.real = node.real; put(index, "NS.dblval", std::move(v)); return;
        case Node::Kind::boolean:
            v.type = node.boolean ? ValueType::true_ : ValueType::false_; v.integer = node.boolean;
            put(index, "NS.boolval", std::move(v));
            return;
        case Node::Kind::data: v.type = ValueType::data; v.data = node.bytes; put(index, "NS.bytes", std::move(v)); return;
        case Node::Kind::dict: break;
        default: bad("unsupported $objects entry");
        }
        const auto cls = class_name(node);
        const auto collection_items = [&](std::string_view key) -> const std::vector<std::uint64_t>& {
            const auto& items = member(node, key);
            if (items.kind != Node::Kind::array) bad(std::string(key) + " is not an array");
            return items.items;
        };
        if (cls == "NSString" || cls == "NSMutableString") {
            const auto& text = member(node, "NS.string");
            if (text.kind != Node::Kind::string) bad("NS.string is not a string");
            v.type = ValueType::data;
            v.data.assign(text.text.begin(), text.text.end());
            put(index, "NS.bytes", std::move(v));
            return;
        }
        if (cls == "NSArray" || cls == "NSMutableArray" || cls == "NSSet" || cls == "NSMutableSet" || cls == "NSOrderedSet" ||
            cls == "NSMutableOrderedSet") {
            put_elements(index, collection_items("NS.objects"));
            return;
        }
        if (cls == "NSDictionary" || cls == "NSMutableDictionary") {
            const auto& keys = collection_items("NS.keys");
            const auto& values = collection_items("NS.objects");
            if (keys.size() != values.size()) bad("dictionary key and value counts differ");
            std::vector<std::uint64_t> interleaved;
            for (std::size_t i = 0; i < keys.size(); ++i) { interleaved.push_back(keys[i]); interleaved.push_back(values[i]); }
            put_elements(index, interleaved);
            return;
        }
        for (const auto& [k, value] : node.entries) {
            const auto name = key_text(k);
            if (name == "$class") continue;
            put_value(index, name, plist_.node(value));
        }
    }

    const BinaryPlist& plist_;
    const std::vector<std::uint64_t>* objects_{};
    Archive archive_;
    std::vector<std::vector<Value>> pending_;
    std::map<std::string, std::uint32_t> classes_, keys_;
    std::map<std::uint64_t, std::uint32_t> mapped_;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> work_;
};

} // namespace

bool Archive::is_keyed_archive(std::span<const std::uint8_t> bytes) {
    return bytes.size() >= 8 && std::memcmp(bytes.data(), "bplist00", 8) == 0;
}

Archive Archive::from_keyed_archive(std::span<const std::uint8_t> bytes) {
    const BinaryPlist plist(bytes);
    Converter converter(plist);
    return converter.finish();
}

} // namespace alchemy::gui::nib
