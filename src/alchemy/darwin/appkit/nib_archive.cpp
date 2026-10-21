#include "alchemy/darwin/appkit/nib_archive.hpp"

#include <bit>
#include <cstring>
#include <stdexcept>

namespace alchemy::gui::nib {
namespace {
constexpr std::string_view magic = "NIBArchive";
constexpr std::string_view empty_key = "UINibEncoderEmptyKey";

[[noreturn]] void fail(const std::string& why) { throw std::runtime_error("Alchemy nib archive: " + why); }

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    void seek(std::size_t offset) {
        if (offset > bytes_.size()) fail("table offset outside the file");
        at_ = offset;
    }
    std::uint8_t byte() {
        if (at_ >= bytes_.size()) fail("truncated file");
        return bytes_[at_++];
    }
    std::uint64_t varint() {
        std::uint64_t value{};
        for (unsigned shift = 0;; shift += 7) {
            if (shift > 63) fail("variable-length integer too long");
            const auto b = byte();
            value |= static_cast<std::uint64_t>(b & 0x7f) << shift;
            if (b & 0x80) return value;
        }
    }
    template <class T>
    T fixed() {
        if (at_ + sizeof(T) > bytes_.size()) fail("truncated file");
        T value{};
        std::memcpy(&value, bytes_.data() + at_, sizeof(T));
        at_ += sizeof(T);
        return value;
    }
    std::span<const std::uint8_t> take(std::uint64_t length) {
        if (length > bytes_.size() - at_) fail("byte string runs past the end of the file");
        const auto result = bytes_.subspan(at_, static_cast<std::size_t>(length));
        at_ += static_cast<std::size_t>(length);
        return result;
    }
private:
    std::span<const std::uint8_t> bytes_;
    std::size_t at_{};
};

void put_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (;;) {
        const auto group = static_cast<std::uint8_t>(value & 0x7f);
        value >>= 7;
        if (!value) { out.push_back(group | 0x80); return; }
        out.push_back(group);
    }
}
template <class T>
void put_fixed(std::vector<std::uint8_t>& out, T value) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), p, p + sizeof(T));
}
} // namespace

bool Archive::is_nib_archive(std::span<const std::uint8_t> bytes) {
    return bytes.size() >= magic.size() + 40 && std::memcmp(bytes.data(), magic.data(), magic.size()) == 0;
}

Archive Archive::parse(std::span<const std::uint8_t> bytes) {
    if (!is_nib_archive(bytes)) fail("not a NIBArchive file");
    Reader header(bytes);
    header.seek(magic.size());
    Archive archive;
    archive.format_version = header.fixed<std::uint32_t>();
    archive.coder_version = header.fixed<std::uint32_t>();
    const auto object_count = header.fixed<std::uint32_t>(), object_offset = header.fixed<std::uint32_t>();
    const auto key_count = header.fixed<std::uint32_t>(), key_offset = header.fixed<std::uint32_t>();
    const auto value_count = header.fixed<std::uint32_t>(), value_offset = header.fixed<std::uint32_t>();
    const auto class_count = header.fixed<std::uint32_t>(), class_offset = header.fixed<std::uint32_t>();
    if (archive.format_version != 1) fail("unsupported format version " + std::to_string(archive.format_version));
    // Every entry takes at least one byte: counts beyond the file size are corrupt.
    for (const auto count : {object_count, key_count, value_count, class_count})
        if (count > bytes.size()) fail("table count larger than the file");

    Reader r(bytes);
    r.seek(class_offset);
    archive.class_names.reserve(class_count);
    for (std::uint32_t i = 0; i < class_count; ++i) {
        const auto length = r.varint();
        const auto extra = r.varint();
        if (extra > 64) fail("class name with too many fallback classes");
        for (std::uint64_t k = 0; k < extra; ++k) static_cast<void>(r.fixed<std::int32_t>());
        const auto name = r.take(length);
        std::string text(name.begin(), name.end());
        while (!text.empty() && text.back() == '\0') text.pop_back();
        archive.class_names.push_back(std::move(text));
    }
    r.seek(key_offset);
    archive.keys.reserve(key_count);
    for (std::uint32_t i = 0; i < key_count; ++i) {
        const auto name = r.take(r.varint());
        archive.keys.emplace_back(name.begin(), name.end());
    }
    r.seek(value_offset);
    archive.values.reserve(value_count);
    for (std::uint32_t i = 0; i < value_count; ++i) {
        Value value;
        const auto key = r.varint();
        if (key >= key_count) fail("value key index out of range");
        value.key = static_cast<std::uint32_t>(key);
        const auto type = r.byte();
        if (type > 10) fail("unknown value type " + std::to_string(type));
        value.type = static_cast<ValueType>(type);
        switch (value.type) {
        case ValueType::int8: value.integer = r.fixed<std::int8_t>(); break;
        case ValueType::int16: value.integer = r.fixed<std::int16_t>(); break;
        case ValueType::int32: value.integer = r.fixed<std::int32_t>(); break;
        case ValueType::int64: value.integer = r.fixed<std::int64_t>(); break;
        case ValueType::true_: value.integer = 1; break;
        case ValueType::false_: value.integer = 0; break;
        case ValueType::float32: value.real = r.fixed<float>(); break;
        case ValueType::float64: value.real = r.fixed<double>(); break;
        case ValueType::data: {
            const auto data = r.take(r.varint());
            value.data.assign(data.begin(), data.end());
            break;
        }
        case ValueType::nil: break;
        case ValueType::object: {
            const auto target = r.fixed<std::uint32_t>();
            if (target >= object_count) fail("object reference out of range");
            value.integer = target;
            break;
        }
        }
        archive.values.push_back(std::move(value));
    }
    r.seek(object_offset);
    archive.objects.reserve(object_count);
    for (std::uint32_t i = 0; i < object_count; ++i) {
        Object object;
        const auto cls = r.varint(), first = r.varint(), count = r.varint();
        if (cls >= class_count) fail("object class index out of range");
        if (first > value_count || count > value_count - first) fail("object values out of range");
        object.class_index = static_cast<std::uint32_t>(cls);
        object.first_value = static_cast<std::uint32_t>(first);
        object.value_count = static_cast<std::uint32_t>(count);
        archive.objects.push_back(object);
    }
    if (archive.objects.empty()) fail("archive without objects");
    return archive;
}

std::vector<std::uint8_t> Archive::encode() const {
    std::vector<std::uint8_t> classes, key_table, value_table, object_table;
    for (const auto& name : class_names) {
        put_varint(classes, name.size() + 1);
        put_varint(classes, 0);
        classes.insert(classes.end(), name.begin(), name.end());
        classes.push_back(0);
    }
    for (const auto& key : keys) {
        put_varint(key_table, key.size());
        key_table.insert(key_table.end(), key.begin(), key.end());
    }
    for (const auto& value : values) {
        put_varint(value_table, value.key);
        value_table.push_back(static_cast<std::uint8_t>(value.type));
        switch (value.type) {
        case ValueType::int8: put_fixed(value_table, static_cast<std::int8_t>(value.integer)); break;
        case ValueType::int16: put_fixed(value_table, static_cast<std::int16_t>(value.integer)); break;
        case ValueType::int32: put_fixed(value_table, static_cast<std::int32_t>(value.integer)); break;
        case ValueType::int64: put_fixed(value_table, value.integer); break;
        case ValueType::float32: put_fixed(value_table, static_cast<float>(value.real)); break;
        case ValueType::float64: put_fixed(value_table, value.real); break;
        case ValueType::data:
            put_varint(value_table, value.data.size());
            value_table.insert(value_table.end(), value.data.begin(), value.data.end());
            break;
        case ValueType::object: put_fixed(value_table, static_cast<std::uint32_t>(value.integer)); break;
        default: break;
        }
    }
    for (const auto& object : objects) {
        put_varint(object_table, object.class_index);
        put_varint(object_table, object.first_value);
        put_varint(object_table, object.value_count);
    }
    std::vector<std::uint8_t> out(magic.begin(), magic.end());
    const std::uint32_t header_end = static_cast<std::uint32_t>(magic.size() + 40);
    const auto object_offset = header_end;
    const auto key_offset = object_offset + static_cast<std::uint32_t>(object_table.size());
    const auto value_offset = key_offset + static_cast<std::uint32_t>(key_table.size());
    const auto class_offset = value_offset + static_cast<std::uint32_t>(value_table.size());
    for (const auto field : {std::uint32_t{1}, coder_version ? coder_version : 10u, static_cast<std::uint32_t>(objects.size()), object_offset,
             static_cast<std::uint32_t>(keys.size()), key_offset, static_cast<std::uint32_t>(values.size()), value_offset,
             static_cast<std::uint32_t>(class_names.size()), class_offset})
        put_fixed(out, field);
    out.insert(out.end(), object_table.begin(), object_table.end());
    out.insert(out.end(), key_table.begin(), key_table.end());
    out.insert(out.end(), value_table.begin(), value_table.end());
    out.insert(out.end(), classes.begin(), classes.end());
    return out;
}

const std::string& Archive::class_name(std::uint32_t object) const { return class_names.at(objects.at(object).class_index); }
std::span<const Value> Archive::values_of(std::uint32_t object) const {
    const auto& o = objects.at(object);
    return std::span<const Value>(values).subspan(o.first_value, o.value_count);
}
const Value* Archive::find(std::uint32_t object, std::string_view key) const {
    for (const auto& value : values_of(object)) if (keys[value.key] == key) return &value;
    return nullptr;
}
std::vector<const Value*> Archive::all(std::uint32_t object, std::string_view key) const {
    std::vector<const Value*> result;
    for (const auto& value : values_of(object)) if (keys[value.key] == key) result.push_back(&value);
    return result;
}
std::vector<std::uint32_t> Archive::elements(std::uint32_t object) const {
    std::vector<std::uint32_t> result;
    for (const auto* value : all(object, empty_key)) {
        if (!value->is_object()) fail("collection element that is not an object reference");
        result.push_back(static_cast<std::uint32_t>(value->integer));
    }
    return result;
}
std::optional<std::string> Archive::string_value(std::uint32_t object) const {
    const auto& name = class_name(object);
    if (name != "NSString" && name != "NSMutableString" && name != "NSLocalizableString") return std::nullopt;
    const auto* bytes = find(object, "NS.bytes");
    if (!bytes) return std::string();
    if (bytes->type != ValueType::data) fail("NSString bytes that are not data");
    return std::string(bytes->data.begin(), bytes->data.end());
}
std::optional<std::uint32_t> Archive::object_at(std::uint32_t object, std::string_view key) const {
    const auto* value = find(object, key);
    if (!value || !value->is_object()) return std::nullopt;
    return static_cast<std::uint32_t>(value->integer);
}
std::optional<std::string> Archive::string_at(std::uint32_t object, std::string_view key) const {
    const auto target = object_at(object, key);
    return target ? string_value(*target) : std::nullopt;
}

Graph read_graph(const Archive& archive) {
    Graph graph;
    const auto data = archive.object_at(0, "IB.objectdata");
    if (!data || archive.class_name(*data) != "NSIBObjectData") fail("the root has no NSIBObjectData");
    graph.object_data = *data;
    const auto root = archive.object_at(*data, "NSRoot");
    if (!root) fail("NSIBObjectData without NSRoot");
    graph.root = *root;
    if (const auto visible = archive.object_at(*data, "NSVisibleWindows")) graph.visible_windows = archive.elements(*visible);
    if (const auto connections = archive.object_at(*data, "NSConnections"))
        for (const auto connector : archive.elements(*connections)) {
            Connection connection;
            connection.object = connector;
            connection.connector_class = archive.class_name(connector);
            if (connection.connector_class == "NSNibOutletConnector") connection.kind = Connection::Kind::outlet;
            else if (connection.connector_class == "NSNibControlConnector") connection.kind = Connection::Kind::action;
            connection.source = archive.object_at(connector, "NSSource");
            connection.destination = archive.object_at(connector, "NSDestination");
            connection.label = archive.string_at(connector, "NSLabel").value_or("");
            graph.connections.push_back(std::move(connection));
        }
    // NSOidsKeys / NSOidsValues and NSAccessibilityOidsKeys / Values map
    // objects to Interface Builder's object ids; nothing at run time uses them.
    static_cast<void>(archive.find(*data, "NSOidsKeys"));
    static_cast<void>(archive.find(*data, "NSOidsValues"));
    static_cast<void>(archive.find(*data, "NSAccessibilityOidsKeys"));
    static_cast<void>(archive.find(*data, "NSAccessibilityOidsValues"));
    if (const auto connections = archive.object_at(*data, "NSAccessibilityConnectors"))
        for (const auto connector : archive.elements(*connections)) {
            Connection connection;
            connection.object = connector;
            connection.connector_class = archive.class_name(connector);
            graph.accessibility_connections.push_back(std::move(connection));
        }
    const auto keys = archive.object_at(*data, "NSObjectsKeys");
    const auto parents = archive.object_at(*data, "NSObjectsValues");
    if (keys && parents) {
        const auto children = archive.elements(*keys);
        const auto owners = archive.elements(*parents);
        if (children.size() != owners.size()) fail("NSObjectsKeys and NSObjectsValues differ in length");
        for (std::size_t i = 0; i < children.size(); ++i) {
            graph.parents.emplace_back(children[i], owners[i]);
            if (owners[i] == graph.root && children[i] != graph.root) graph.top_level.push_back(children[i]);
        }
    }
    return graph;
}

std::optional<std::vector<double>> parse_numbers(std::string_view text) {
    std::vector<double> numbers;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = text[i];
        if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9')) {
            std::size_t end = i + 1;
            while (end < text.size() && (std::string_view("0123456789.eE+-").find(text[end]) != std::string_view::npos)) ++end;
            try { numbers.push_back(std::stod(std::string(text.substr(i, end - i)))); }
            catch (...) { return std::nullopt; }
            i = end;
        } else {
            if (std::string_view("{}, ").find(c) == std::string_view::npos) return std::nullopt;
            ++i;
        }
    }
    return numbers;
}

} // namespace alchemy::gui::nib
