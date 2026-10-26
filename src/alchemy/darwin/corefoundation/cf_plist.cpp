#include "alchemy/darwin/corefoundation/cf_plist.hpp"

#include "alchemy/object_store.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <stdexcept>

// Independent implementation of the documented property list formats: the
// XML plist DTD and the "bplist00" binary layout (trailer, offset table,
// typed object markers).
namespace alchemy::export_fill {

const Plist* Plist::find(std::string_view key) const {
    for (const auto& [name, value] : entries) if (name == key) return &value;
    return nullptr;
}
Plist* Plist::find(std::string_view key) {
    for (auto& [name, value] : entries) if (name == key) return &value;
    return nullptr;
}
void Plist::set(std::string key, Plist value) {
    if (auto* existing = find(key)) { *existing = std::move(value); return; }
    entries.emplace_back(std::move(key), std::move(value));
}
bool Plist::erase(std::string_view key) {
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.first == key; });
    if (found == entries.end()) return false;
    entries.erase(found);
    return true;
}

namespace {

constexpr double unix_to_absolute = 978307200.0;

// ---- dates (ISO 8601 UTC, "2001-01-01T00:00:00Z") ----
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const auto era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const auto doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const auto doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}
std::string format_date(double absolute) {
    auto seconds = static_cast<std::int64_t>(std::floor(absolute + unix_to_absolute));
    auto days = seconds / 86400;
    auto rest = seconds % 86400;
    if (rest < 0) { rest += 86400; --days; }
    // civil_from_days
    days += 719468;
    const auto era = (days >= 0 ? days : days - 146096) / 146097;
    const auto doe = static_cast<unsigned>(days - era * 146097);
    const auto yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    auto year = static_cast<std::int64_t>(yoe) + era * 400;
    const auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const auto mp = (5 * doy + 2) / 153;
    const auto day = doy - (153 * mp + 2) / 5 + 1;
    const auto month = mp < 10 ? mp + 3 : mp - 9;
    year += month <= 2;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02uT%02lld:%02lld:%02lldZ", static_cast<long long>(year), month, day,
                  static_cast<long long>(rest / 3600), static_cast<long long>(rest / 60 % 60), static_cast<long long>(rest % 60));
    return buffer;
}
std::optional<double> parse_date(std::string_view text) {
    int year{}, month{}, day{}, hour{}, minute{}, second{};
    {   // "%d-%d-%dT%d:%d:%dZ"
        const char* at = text.data();
        const char* end = text.data() + text.size();
        const std::pair<int*, char> fields[] = {{&year, '-'}, {&month, '-'}, {&day, 'T'}, {&hour, ':'}, {&minute, ':'}, {&second, 'Z'}};
        for (const auto& [field, separator] : fields) {
            const auto [next, problem] = std::from_chars(at, end, *field);
            if (problem != std::errc{} || next == end || *next != separator) return std::nullopt;
            at = next + 1;
        }
    }
    if (month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;
    const auto days = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    return static_cast<double>(days * 86400 + hour * 3600 + minute * 60 + second) - unix_to_absolute;
}

// ---- base64 ----
std::string base64(std::span<const std::uint8_t> bytes) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t at = 0; at < bytes.size(); at += 3) {
        const std::uint32_t value = (std::uint32_t{bytes[at]} << 16) | (at + 1 < bytes.size() ? std::uint32_t{bytes[at + 1]} << 8 : 0) |
                                    (at + 2 < bytes.size() ? bytes[at + 2] : 0);
        out += table[value >> 18];
        out += table[(value >> 12) & 63];
        out += at + 1 < bytes.size() ? table[(value >> 6) & 63] : '=';
        out += at + 2 < bytes.size() ? table[value & 63] : '=';
    }
    return out;
}
std::vector<std::uint8_t> unbase64(std::string_view text) {
    std::vector<std::uint8_t> out;
    std::uint32_t buffer{};
    int bits{};
    for (const auto c : text) {
        const int value = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 :
                          c == '+' ? 62 : c == '/' ? 63 : -1;
        if (value < 0) continue;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(static_cast<std::uint8_t>(buffer >> bits)); }
    }
    return out;
}

// ---- XML ----
std::string escape(std::string_view text) {
    std::string out;
    for (const auto c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out += c;
        }
    }
    return out;
}
std::string real_text(double value) {
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value > 0 ? "+infinity" : "-infinity";
    if (value == std::floor(value) && std::fabs(value) < 1e15) return std::to_string(static_cast<long long>(value));
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    return buffer;
}
void write_xml(std::string& out, const Plist& value, int depth) {
    const std::string indent(static_cast<std::size_t>(depth), '\t');
    switch (value.kind) {
    case Plist::Kind::string: out += indent + "<string>" + escape(value.text) + "</string>\n"; break;
    case Plist::Kind::integer: out += indent + "<integer>" + std::to_string(value.integer) + "</integer>\n"; break;
    case Plist::Kind::real: out += indent + "<real>" + real_text(value.real) + "</real>\n"; break;
    case Plist::Kind::boolean: out += indent + (value.boolean ? "<true/>\n" : "<false/>\n"); break;
    case Plist::Kind::uid:
        out += indent + "<dict>\n" + indent + "\t<key>CF$UID</key>\n" + indent + "\t<integer>" + std::to_string(value.integer) +
               "</integer>\n" + indent + "</dict>\n";
        break;
    case Plist::Kind::date: out += indent + "<date>" + format_date(value.real) + "</date>\n"; break;
    case Plist::Kind::data: out += indent + "<data>\n" + indent + base64(value.bytes) + "\n" + indent + "</data>\n"; break;
    case Plist::Kind::array:
        if (value.items.empty()) { out += indent + "<array/>\n"; break; }
        out += indent + "<array>\n";
        for (const auto& item : value.items) write_xml(out, item, depth + 1);
        out += indent + "</array>\n";
        break;
    case Plist::Kind::dictionary: {
        if (value.entries.empty()) { out += indent + "<dict/>\n"; break; }
        out += indent + "<dict>\n";
        auto sorted = value.entries;
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [key, item] : sorted) {
            out += indent + "\t<key>" + escape(key) + "</key>\n";
            write_xml(out, item, depth + 1);
        }
        out += indent + "</dict>\n";
        break;
    }
    }
}

void append_utf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) out += static_cast<char>(code);
    else if (code < 0x800) { out += static_cast<char>(0xc0 | (code >> 6)); out += static_cast<char>(0x80 | (code & 63)); }
    else if (code < 0x10000) {
        out += static_cast<char>(0xe0 | (code >> 12)); out += static_cast<char>(0x80 | ((code >> 6) & 63)); out += static_cast<char>(0x80 | (code & 63));
    } else {
        out += static_cast<char>(0xf0 | (code >> 18)); out += static_cast<char>(0x80 | ((code >> 12) & 63));
        out += static_cast<char>(0x80 | ((code >> 6) & 63)); out += static_cast<char>(0x80 | (code & 63));
    }
}

struct XmlError {};

class XmlReader {
public:
    explicit XmlReader(std::string_view text) : text_(text) {}
    struct Tag { std::string name; bool closing{}, empty{}; };
    Tag tag() {
        for (;;) {
            skip_space();
            if (at_ >= text_.size() || text_[at_] != '<') throw XmlError{};
            if (text_.substr(at_, 4) == "<!--") { skip_past("-->"); continue; }
            if (text_.substr(at_, 2) == "<?" ) { skip_past("?>"); continue; }
            if (text_.substr(at_, 2) == "<!") { skip_past(">"); continue; }
            break;
        }
        const auto end = text_.find('>', at_);
        if (end == std::string_view::npos) throw XmlError{};
        std::string_view inner = text_.substr(at_ + 1, end - at_ - 1);
        at_ = end + 1;
        Tag tag;
        if (!inner.empty() && inner.front() == '/') { tag.closing = true; inner.remove_prefix(1); }
        if (!inner.empty() && inner.back() == '/') { tag.empty = true; inner.remove_suffix(1); }
        const auto space = inner.find_first_of(" \t\r\n");
        tag.name = std::string(inner.substr(0, space));
        return tag;
    }
    // Text up to </name>, entities decoded.
    std::string content(std::string_view name) {
        const auto close = "</" + std::string(name) + ">";
        const auto end = text_.find(close, at_);
        if (end == std::string_view::npos) throw XmlError{};
        const auto raw = text_.substr(at_, end - at_);
        at_ = end + close.size();
        std::string out;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] != '&') { out += raw[i]; continue; }
            const auto semicolon = raw.find(';', i);
            if (semicolon == std::string_view::npos) throw XmlError{};
            const auto entity = raw.substr(i + 1, semicolon - i - 1);
            if (entity == "amp") out += '&';
            else if (entity == "lt") out += '<';
            else if (entity == "gt") out += '>';
            else if (entity == "quot") out += '"';
            else if (entity == "apos") out += '\'';
            else if (!entity.empty() && entity[0] == '#') {
                std::uint32_t code{};
                const bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
                const auto digits = entity.substr(hex ? 2 : 1);
                if (std::from_chars(digits.data(), digits.data() + digits.size(), code, hex ? 16 : 10).ec != std::errc{}) throw XmlError{};
                append_utf8(out, code);
            } else throw XmlError{};
            i = semicolon;
        }
        return out;
    }

private:
    void skip_space() { while (at_ < text_.size() && std::string_view(" \t\r\n").find(text_[at_]) != std::string_view::npos) ++at_; }
    void skip_past(std::string_view marker) {
        const auto end = text_.find(marker, at_);
        if (end == std::string_view::npos) throw XmlError{};
        at_ = end + marker.size();
    }
    std::string_view text_;
    std::size_t at_{};
};

Plist read_xml_value(XmlReader& reader, const XmlReader::Tag& tag, int depth) {
    if (tag.closing || depth > 512) throw XmlError{};
    Plist value;
    if (tag.name == "string") { value.kind = Plist::Kind::string; if (!tag.empty) value.text = reader.content("string"); return value; }
    if (tag.name == "integer") {
        value.kind = Plist::Kind::integer;
        auto text = reader.content("integer");
        text.erase(0, text.find_first_not_of(" \t\r\n"));
        text.erase(text.find_last_not_of(" \t\r\n") + 1);
        const bool hex = text.starts_with("0x") || text.starts_with("-0x");
        const auto digits = hex ? std::string_view(text).substr(text[0] == '-' ? 3 : 2) : std::string_view(text);
        if (hex) {
            std::uint64_t magnitude{};
            if (std::from_chars(digits.data(), digits.data() + digits.size(), magnitude, 16).ec != std::errc{}) throw XmlError{};
            value.integer = text[0] == '-' ? -static_cast<std::int64_t>(magnitude) : static_cast<std::int64_t>(magnitude);
        } else if (std::from_chars(digits.data(), digits.data() + digits.size(), value.integer).ec != std::errc{}) {
            // Unsigned 64-bit values above INT64_MAX keep their bit pattern.
            std::uint64_t magnitude{};
            if (std::from_chars(digits.data(), digits.data() + digits.size(), magnitude).ec != std::errc{}) throw XmlError{};
            value.integer = static_cast<std::int64_t>(magnitude);
        }
        return value;
    }
    if (tag.name == "real") {
        value.kind = Plist::Kind::real;
        const auto text = reader.content("real");
        value.real = text == "nan" ? NAN : text == "+infinity" || text == "infinity" ? INFINITY : text == "-infinity" ? -INFINITY : std::strtod(text.c_str(), nullptr);
        return value;
    }
    if (tag.name == "true" || tag.name == "false") {
        value.kind = Plist::Kind::boolean;
        value.boolean = tag.name == "true";
        if (!tag.empty) static_cast<void>(reader.content(tag.name));
        return value;
    }
    if (tag.name == "date") {
        value.kind = Plist::Kind::date;
        const auto parsed = parse_date(reader.content("date"));
        if (!parsed) throw XmlError{};
        value.real = *parsed;
        return value;
    }
    if (tag.name == "data") { value.kind = Plist::Kind::data; if (!tag.empty) value.bytes = unbase64(reader.content("data")); return value; }
    if (tag.name == "array") {
        value.kind = Plist::Kind::array;
        if (tag.empty) return value;
        for (;;) {
            const auto next = reader.tag();
            if (next.closing) { if (next.name != "array") throw XmlError{}; break; }
            value.items.push_back(read_xml_value(reader, next, depth + 1));
        }
        return value;
    }
    if (tag.name == "dict") {
        value.kind = Plist::Kind::dictionary;
        if (tag.empty) return value;
        for (;;) {
            const auto next = reader.tag();
            if (next.closing) { if (next.name != "dict") throw XmlError{}; break; }
            if (next.name != "key") throw XmlError{};
            auto key = next.empty ? std::string{} : reader.content("key");
            value.set(std::move(key), read_xml_value(reader, reader.tag(), depth + 1));
        }
        return value;
    }
    throw XmlError{};
}

// ---- binary ----
struct BinaryError {};

class BinaryReader {
public:
    explicit BinaryReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {
        if (bytes.size() < 8 + 32 || std::memcmp(bytes.data(), "bplist00", 8) != 0) throw BinaryError{};
        const auto trailer = bytes.subspan(bytes.size() - 32);
        offset_size_ = trailer[6];
        reference_size_ = trailer[7];
        count_ = big(trailer.subspan(8, 8));
        top_ = big(trailer.subspan(16, 8));
        table_ = big(trailer.subspan(24, 8));
        if (offset_size_ < 1 || offset_size_ > 8 || reference_size_ < 1 || reference_size_ > 8 || top_ >= count_ ||
            count_ > bytes.size() || table_ + count_ * offset_size_ > bytes.size() - 32) throw BinaryError{};
    }
    Plist top() { return object(top_, 0); }

private:
    static std::uint64_t big(std::span<const std::uint8_t> bytes) {
        std::uint64_t value{};
        for (const auto byte : bytes) value = (value << 8) | byte;
        return value;
    }
    std::span<const std::uint8_t> range(std::uint64_t at, std::uint64_t size) const {
        if (at > bytes_.size() || size > bytes_.size() - at) throw BinaryError{};
        return bytes_.subspan(static_cast<std::size_t>(at), static_cast<std::size_t>(size));
    }
    std::uint64_t offset(std::uint64_t index) const { return big(range(table_ + index * offset_size_, offset_size_)); }
    // The count after a marker: the low nibble, or a following integer object when it is 0xF.
    std::uint64_t count(std::uint64_t& at, std::uint8_t marker) const {
        if ((marker & 15) != 15) return marker & 15;
        const auto next = range(at, 1)[0];
        if ((next >> 4) != 1) throw BinaryError{};
        const auto size = std::uint64_t{1} << (next & 15);
        const auto value = big(range(at + 1, size));
        at += 1 + size;
        return value;
    }
    Plist object(std::uint64_t index, int depth) {
        if (index >= count_ || depth > 512) throw BinaryError{};
        auto at = offset(index);
        const auto marker = range(at, 1)[0];
        ++at;
        Plist value;
        switch (marker >> 4) {
        case 0:
            if (marker == 8 || marker == 9) { value.kind = Plist::Kind::boolean; value.boolean = marker == 9; return value; }
            throw BinaryError{};
        case 1: {
            const auto size = std::uint64_t{1} << (marker & 15);
            if (size > 16) throw BinaryError{};
            const auto bytes = range(at, size);
            value.kind = Plist::Kind::integer;
            // 16-byte integers keep their low 64 bits; 8-byte ones are signed.
            value.integer = static_cast<std::int64_t>(big(size == 16 ? bytes.subspan(8) : bytes));
            return value;
        }
        case 2: {
            const auto size = std::uint64_t{1} << (marker & 15);
            value.kind = Plist::Kind::real;
            if (size == 4) value.real = std::bit_cast<float>(static_cast<std::uint32_t>(big(range(at, 4))));
            else if (size == 8) value.real = std::bit_cast<double>(big(range(at, 8)));
            else throw BinaryError{};
            return value;
        }
        case 3:
            if (marker != 0x33) throw BinaryError{};
            value.kind = Plist::Kind::date;
            value.real = std::bit_cast<double>(big(range(at, 8)));
            return value;
        case 8: {
            const auto size = static_cast<std::uint64_t>(marker & 15) + 1;
            if (size > 8) throw BinaryError{};
            value.kind = Plist::Kind::uid;
            value.integer = static_cast<std::int64_t>(big(range(at, size)));
            return value;
        }
        case 4: {
            const auto size = count(at, marker);
            const auto bytes = range(at, size);
            value.kind = Plist::Kind::data;
            value.bytes.assign(bytes.begin(), bytes.end());
            return value;
        }
        case 5: {
            const auto size = count(at, marker);
            const auto bytes = range(at, size);
            value.kind = Plist::Kind::string;
            for (const auto byte : bytes) append_utf8(value.text, byte);
            return value;
        }
        case 6: {
            const auto size = count(at, marker);
            const auto bytes = range(at, size * 2);
            value.kind = Plist::Kind::string;
            for (std::size_t i = 0; i < bytes.size(); i += 2) {
                std::uint32_t code = (std::uint32_t{bytes[i]} << 8) | bytes[i + 1];
                if (code >= 0xd800 && code < 0xdc00 && i + 3 < bytes.size()) {
                    const std::uint32_t low = (std::uint32_t{bytes[i + 2]} << 8) | bytes[i + 3];
                    if (low >= 0xdc00 && low < 0xe000) { code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00); i += 2; }
                }
                append_utf8(value.text, code);
            }
            return value;
        }
        case 10: {
            const auto size = count(at, marker);
            value.kind = Plist::Kind::array;
            for (std::uint64_t i = 0; i < size; ++i) value.items.push_back(object(big(range(at + i * reference_size_, reference_size_)), depth + 1));
            return value;
        }
        case 13: {
            const auto size = count(at, marker);
            value.kind = Plist::Kind::dictionary;
            for (std::uint64_t i = 0; i < size; ++i) {
                auto key = object(big(range(at + i * reference_size_, reference_size_)), depth + 1);
                if (key.kind != Plist::Kind::string) throw BinaryError{};
                value.set(std::move(key.text), object(big(range(at + (size + i) * reference_size_, reference_size_)), depth + 1));
            }
            return value;
        }
        default: throw BinaryError{};
        }
    }

    std::span<const std::uint8_t> bytes_;
    unsigned offset_size_{}, reference_size_{};
    std::uint64_t count_{}, top_{}, table_{};
};

// UTF-8 to UTF-16 code units (binary plist strings that are not ASCII).
std::vector<std::uint16_t> utf16(std::string_view text) {
    std::vector<std::uint16_t> out;
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::uint32_t code{};
        std::size_t length = 1;
        if (c < 0x80) code = c;
        else if ((c >> 5) == 6 && i + 1 < text.size()) { code = ((c & 31U) << 6) | (text[i + 1] & 63U); length = 2; }
        else if ((c >> 4) == 14 && i + 2 < text.size()) { code = ((c & 15U) << 12) | ((text[i + 1] & 63U) << 6) | (text[i + 2] & 63U); length = 3; }
        else if ((c >> 3) == 30 && i + 3 < text.size()) {
            code = ((c & 7U) << 18) | ((text[i + 1] & 63U) << 12) | ((text[i + 2] & 63U) << 6) | (text[i + 3] & 63U);
            length = 4;
        } else code = 0xfffd;
        i += length;
        if (code >= 0x10000) {
            code -= 0x10000;
            out.push_back(static_cast<std::uint16_t>(0xd800 + (code >> 10)));
            out.push_back(static_cast<std::uint16_t>(0xdc00 + (code & 1023)));
        } else out.push_back(static_cast<std::uint16_t>(code));
    }
    return out;
}

class BinaryWriter {
public:
    std::vector<std::uint8_t> write(const Plist& top) {
        flatten(top);
        std::vector<std::uint8_t> out{'b', 'p', 'l', 'i', 's', 't', '0', '0'};
        reference_size_ = objects_.size() < 256 ? 1 : objects_.size() < 65536 ? 2 : 4;
        std::vector<std::uint64_t> offsets;
        for (std::size_t index = 0; index < objects_.size(); ++index) {
            offsets.push_back(out.size());
            encode(out, *objects_[index]);
        }
        const auto table = out.size();
        const unsigned offset_size = table < 256 ? 1 : table < 65536 ? 2 : table < (1ULL << 32) ? 4 : 8;
        for (const auto offset : offsets) put_big(out, offset, offset_size);
        std::array<std::uint8_t, 32> trailer{};
        trailer[6] = static_cast<std::uint8_t>(offset_size);
        trailer[7] = static_cast<std::uint8_t>(reference_size_);
        for (int i = 0; i < 8; ++i) {
            trailer[8 + i] = static_cast<std::uint8_t>(objects_.size() >> (56 - 8 * i));
            trailer[24 + i] = static_cast<std::uint8_t>(table >> (56 - 8 * i));
        }
        out.insert(out.end(), trailer.begin(), trailer.end());
        return out;
    }

private:
    static void put_big(std::vector<std::uint8_t>& out, std::uint64_t value, unsigned size) {
        for (unsigned i = 0; i < size; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * (size - 1 - i))));
    }
    // Objects in depth-first order; containers refer to children by index.
    std::uint64_t flatten(const Plist& value) {
        const auto index = objects_.size();
        objects_.push_back(&value);
        references_.emplace_back();
        std::vector<std::uint64_t> children;
        if (value.kind == Plist::Kind::array) {
            for (const auto& item : value.items) children.push_back(flatten(item));
        } else if (value.kind == Plist::Kind::dictionary) {
            std::vector<std::uint64_t> keys, values;
            for (const auto& [key, item] : value.entries) {
                key_storage_.push_back(std::make_unique<Plist>());
                key_storage_.back()->text = key;
                keys.push_back(flatten(*key_storage_.back()));
                values.push_back(flatten(item));
            }
            children = keys;
            children.insert(children.end(), values.begin(), values.end());
        }
        references_[index] = std::move(children);
        return index;
    }
    void marker(std::vector<std::uint8_t>& out, std::uint8_t kind, std::uint64_t count) {
        if (count < 15) { out.push_back(static_cast<std::uint8_t>((kind << 4) | count)); return; }
        out.push_back(static_cast<std::uint8_t>((kind << 4) | 15));
        integer(out, static_cast<std::int64_t>(count));
    }
    static void integer(std::vector<std::uint8_t>& out, std::int64_t value) {
        if (value >= 0 && value < 256) { out.push_back(0x10); put_big(out, static_cast<std::uint64_t>(value), 1); }
        else if (value >= 0 && value < 65536) { out.push_back(0x11); put_big(out, static_cast<std::uint64_t>(value), 2); }
        else if (value >= 0 && value < (1LL << 32)) { out.push_back(0x12); put_big(out, static_cast<std::uint64_t>(value), 4); }
        else { out.push_back(0x13); put_big(out, static_cast<std::uint64_t>(value), 8); }
    }
    void encode(std::vector<std::uint8_t>& out, const Plist& value) {
        switch (value.kind) {
        case Plist::Kind::boolean: out.push_back(value.boolean ? 9 : 8); break;
        case Plist::Kind::uid: {
            const auto uid = static_cast<std::uint64_t>(value.integer);
            unsigned size = 1;
            while (size < 8 && (uid >> (8 * size)) != 0) ++size;
            out.push_back(static_cast<std::uint8_t>(0x80 | (size - 1)));
            put_big(out, uid, size);
            break;
        }
        case Plist::Kind::integer: integer(out, value.integer); break;
        case Plist::Kind::real: out.push_back(0x23); put_big(out, std::bit_cast<std::uint64_t>(value.real), 8); break;
        case Plist::Kind::date: out.push_back(0x33); put_big(out, std::bit_cast<std::uint64_t>(value.real), 8); break;
        case Plist::Kind::data: marker(out, 4, value.bytes.size()); out.insert(out.end(), value.bytes.begin(), value.bytes.end()); break;
        case Plist::Kind::string: {
            const bool ascii = std::all_of(value.text.begin(), value.text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
            if (ascii) { marker(out, 5, value.text.size()); out.insert(out.end(), value.text.begin(), value.text.end()); break; }
            const auto units = utf16(value.text);
            marker(out, 6, units.size());
            for (const auto unit : units) put_big(out, unit, 2);
            break;
        }
        case Plist::Kind::array:
        case Plist::Kind::dictionary: {
            const auto& children = references_[current_];
            marker(out, value.kind == Plist::Kind::array ? 10 : 13, value.kind == Plist::Kind::array ? children.size() : children.size() / 2);
            for (const auto child : children) put_big(out, child, reference_size_);
            break;
        }
        }
        ++current_;
    }

    std::vector<const Plist*> objects_;
    std::vector<std::vector<std::uint64_t>> references_;
    std::vector<std::unique_ptr<Plist>> key_storage_;
    unsigned reference_size_{1};
    std::size_t current_{};
};

// ---- OpenStep (old-style) property lists, read only ----
// The text format of .strings files and old Info.plists: strings (quoted with
// C escapes, \Uxxxx UTF-16 units and \ooo octal, or unquoted [A-Za-z0-9_$+/:.-]),
// dictionaries { key = value; }, arrays ( a, b ), data <hex>, and /* */ and //
// comments. A text whose top level is not one value is read as a strings file:
// "key" = "value"; pairs without the braces, as CF reads it. The bytes are UTF-8,
// or UTF-16 with a byte-order mark.
struct OpenStepError {};

class OpenStepReader {
public:
    explicit OpenStepReader(std::string text) : text_(std::move(text)) {}

    Plist top() {
        skip();
        if (at_end()) return dictionary();   // an empty strings file
        const auto start = position_;
        try {
            auto value = this->value(0);
            skip();
            if (at_end()) return value;
        } catch (const OpenStepError&) {
        }
        position_ = start;   // a strings file: dictionary entries to the end
        auto result = dictionary();
        entries(result, 0, false);
        return result;
    }

private:
    std::string text_;
    std::size_t position_{};

    static Plist dictionary() { Plist value; value.kind = Plist::Kind::dictionary; return value; }
    bool at_end() const { return position_ >= text_.size(); }
    char peek() const { return at_end() ? '\0' : text_[position_]; }
    [[noreturn]] static void fail() { throw OpenStepError{}; }
    void expect(char c) {
        skip();
        if (peek() != c) fail();
        ++position_;
    }
    void skip() {
        while (!at_end()) {
            const auto c = text_[position_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') { ++position_; continue; }
            if (c == '/' && position_ + 1 < text_.size() && text_[position_ + 1] == '/') {
                while (!at_end() && text_[position_] != '\n') ++position_;
                continue;
            }
            if (c == '/' && position_ + 1 < text_.size() && text_[position_ + 1] == '*') {
                const auto end = text_.find("*/", position_ + 2);
                if (end == std::string::npos) fail();
                position_ = end + 2;
                continue;
            }
            break;
        }
    }
    static bool unquoted(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$' || c == '+' ||
               c == '/' || c == ':' || c == '.' || c == '-';
    }
    static void append_utf8(std::string& out, char32_t code) {
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
    std::string string() {
        skip();
        std::string out;
        if (peek() != '"' && peek() != '\'') {
            while (!at_end() && unquoted(peek())) out += text_[position_++];
            if (out.empty()) fail();
            return out;
        }
        const char quote = text_[position_++];
        char16_t pending_high{};
        while (true) {
            if (at_end()) fail();
            const char c = text_[position_++];
            if (c == quote) break;
            if (c != '\\') {
                if (pending_high) { append_utf8(out, 0xfffd); pending_high = 0; }
                out += c;
                continue;
            }
            if (at_end()) fail();
            const char e = text_[position_++];
            char32_t code{};
            switch (e) {
            case 'a': code = '\a'; break;
            case 'b': code = '\b'; break;
            case 'f': code = '\f'; break;
            case 'n': code = '\n'; break;
            case 'r': code = '\r'; break;
            case 't': code = '\t'; break;
            case 'v': code = '\v'; break;
            case 'U': case 'u': {
                char32_t unit{};
                for (int digit = 0; digit < 4; ++digit) {
                    if (at_end() || !std::isxdigit(static_cast<unsigned char>(peek()))) fail();
                    const char h = text_[position_++];
                    unit = unit * 16 + static_cast<char32_t>(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
                }
                if (unit >= 0xd800 && unit < 0xdc00) { pending_high = static_cast<char16_t>(unit); continue; }
                if (unit >= 0xdc00 && unit < 0xe000 && pending_high) {
                    append_utf8(out, 0x10000 + ((static_cast<char32_t>(pending_high) - 0xd800) << 10) + (unit - 0xdc00));
                    pending_high = 0;
                    continue;
                }
                code = unit;
                break;
            }
            default:
                if (e >= '0' && e <= '7') {
                    code = static_cast<char32_t>(e - '0');
                    for (int digit = 1; digit < 3 && !at_end() && peek() >= '0' && peek() <= '7'; ++digit)
                        code = code * 8 + static_cast<char32_t>(text_[position_++] - '0');
                } else code = static_cast<unsigned char>(e);   // \" \' \\ and any other character as itself
            }
            if (pending_high) { append_utf8(out, 0xfffd); pending_high = 0; }
            append_utf8(out, code);
        }
        if (pending_high) append_utf8(out, 0xfffd);
        return out;
    }
    // Dictionary entries up to `close` ('}'), or to the end of the text when braceless.
    void entries(Plist& result, int depth, bool braced) {
        while (true) {
            skip();
            if (braced && peek() == '}') { ++position_; return; }
            if (!braced && at_end()) return;
            auto key = string();
            skip();
            Plist item;
            if (peek() == '=') {
                ++position_;
                item = value(depth + 1);
            } else {   // "key"; is "key" = "key";
                item.kind = Plist::Kind::string;
                item.text = key;
            }
            expect(';');
            result.set(std::move(key), std::move(item));
        }
    }
    Plist value(int depth) {
        if (depth > 256) fail();
        skip();
        const char c = peek();
        if (c == '{') {
            ++position_;
            auto result = dictionary();
            entries(result, depth, true);
            return result;
        }
        if (c == '(') {
            ++position_;
            Plist result;
            result.kind = Plist::Kind::array;
            skip();
            if (peek() == ')') { ++position_; return result; }
            while (true) {
                result.items.push_back(value(depth + 1));
                skip();
                if (peek() == ',') {
                    ++position_;
                    skip();
                    if (peek() == ')') { ++position_; return result; }
                    continue;
                }
                if (peek() == ')') { ++position_; return result; }
                fail();
            }
        }
        if (c == '<') {
            ++position_;
            Plist result;
            result.kind = Plist::Kind::data;
            int pending = -1;
            while (true) {
                skip();
                if (at_end()) fail();
                const char h = text_[position_++];
                if (h == '>') break;
                if (!std::isxdigit(static_cast<unsigned char>(h))) fail();
                const int digit = h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10;
                if (pending < 0) pending = digit;
                else { result.bytes.push_back(static_cast<std::uint8_t>(pending * 16 + digit)); pending = -1; }
            }
            if (pending >= 0) fail();
            return result;
        }
        Plist result;
        result.kind = Plist::Kind::string;
        result.text = string();
        return result;
    }
};

// The text of OpenStep bytes as UTF-8: UTF-16 with a byte-order mark, else UTF-8 (a UTF-8 mark is dropped).
std::optional<std::string> openstep_text(std::span<const std::uint8_t> bytes) {
    if (bytes.size() >= 2 && ((bytes[0] == 0xff && bytes[1] == 0xfe) || (bytes[0] == 0xfe && bytes[1] == 0xff))) {
        const bool little = bytes[0] == 0xff;
        std::string out;
        char16_t high{};
        for (std::size_t at = 2; at + 1 < bytes.size(); at += 2) {
            const char16_t unit = little ? static_cast<char16_t>(bytes[at] | (bytes[at + 1] << 8)) : static_cast<char16_t>((bytes[at] << 8) | bytes[at + 1]);
            char32_t code = unit;
            if (unit >= 0xd800 && unit < 0xdc00) { high = unit; continue; }
            if (unit >= 0xdc00 && unit < 0xe000) {
                if (!high) return std::nullopt;
                code = 0x10000 + ((static_cast<char32_t>(high) - 0xd800) << 10) + (unit - 0xdc00);
                high = 0;
            } else if (high) return std::nullopt;
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
        return out;
    }
    std::size_t start = bytes.size() >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf ? 3 : 0;
    return std::string(reinterpret_cast<const char*>(bytes.data()) + start, bytes.size() - start);
}

} // namespace

std::string plist_to_xml(const Plist& value) {
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                      "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                      "<plist version=\"1.0\">\n";
    write_xml(out, value, 0);
    out += "</plist>\n";
    return out;
}

std::vector<std::uint8_t> plist_to_binary(const Plist& value) { return BinaryWriter{}.write(value); }

std::optional<Plist> plist_parse(std::span<const std::uint8_t> bytes, std::uint64_t* format) {
    try {
        if (bytes.size() >= 8 && std::memcmp(bytes.data(), "bplist00", 8) == 0) {
            BinaryReader reader(bytes);
            auto value = reader.top();
            if (format) *format = plist_binary_format;
            return value;
        }
        // XML starts with "<?", "<!" or "<plist" after any whitespace; other text is OpenStep.
        std::size_t first = 0;
        if (bytes.size() >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf) first = 3;
        while (first < bytes.size() && (bytes[first] == ' ' || bytes[first] == '\t' || bytes[first] == '\r' || bytes[first] == '\n')) ++first;
        const bool utf16 = bytes.size() >= 2 && ((bytes[0] == 0xff && bytes[1] == 0xfe) || (bytes[0] == 0xfe && bytes[1] == 0xff));
        const bool xml = !utf16 && first + 1 < bytes.size() && bytes[first] == '<' && (bytes[first + 1] == '?' || bytes[first + 1] == '!' || bytes[first + 1] == 'p');
        if (!xml) {
            const auto text = openstep_text(bytes);
            if (!text) return std::nullopt;
            auto value = OpenStepReader(*text).top();
            if (format) *format = plist_openstep_format;
            return value;
        }
        XmlReader reader(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        auto root = reader.tag();
        if (root.name != "plist" || root.closing) return std::nullopt;
        auto value = read_xml_value(reader, reader.tag(), 0);
        if (format) *format = plist_xml_format;
        return value;
    } catch (const XmlError&) {
        return std::nullopt;
    } catch (const BinaryError&) {
        return std::nullopt;
    } catch (const OpenStepError&) {
        return std::nullopt;
    }
}

Plist plist_from_guest(ProcessContext& process, CoreFoundation& cf, std::uint64_t object) {
    const auto kind = cf.kind(object);
    if (!kind) throw std::runtime_error("Alchemy property list: object is not a property list type");
    Plist value;
    switch (*kind) {
    case ObjectKind::string: value.kind = Plist::Kind::string; value.text = cf.utf8(object); return value;
    case ObjectKind::boolean: value.kind = Plist::Kind::boolean; value.boolean = object == cf.boolean(true); return value;
    case ObjectKind::number: {
        const bool floating = cf.invoke("CFNumberIsFloatType", std::array<std::uint64_t, 1>{object}) != 0;
        if (floating) { value.kind = Plist::Kind::real; value.real = cf.number_value(object).value_or(0); return value; }
        value.kind = Plist::Kind::integer;
        const auto cell = process.allocate_guest(8);
        static_cast<void>(cf.invoke("CFNumberGetValue", std::array<std::uint64_t, 3>{object, 4, cell}));   // kCFNumberSInt64Type
        value.integer = static_cast<std::int64_t>(process.main_engine().read64(cell));
        process.release_guest(cell);
        return value;
    }
    case ObjectKind::date: value.kind = Plist::Kind::date; value.real = cf.date_time(object); return value;
    case ObjectKind::data: {
        value.kind = Plist::Kind::data;
        const auto length = cf.invoke("CFDataGetLength", std::array<std::uint64_t, 1>{object});
        value.bytes.resize(static_cast<std::size_t>(length));
        if (length) process.main_engine().read_bytes(cf.invoke("CFDataGetBytePtr", std::array<std::uint64_t, 1>{object}), value.bytes);
        return value;
    }
    case ObjectKind::array:
        value.kind = Plist::Kind::array;
        for (const auto item : cf.array_values(object)) value.items.push_back(plist_from_guest(process, cf, item));
        return value;
    case ObjectKind::dictionary:
        value.kind = Plist::Kind::dictionary;
        for (const auto& [key, item] : cf.dictionary_entries(object)) {
            if (cf.kind(key) != ObjectKind::string) throw std::runtime_error("Alchemy property list: dictionary key is not a string");
            value.set(cf.utf8(key), plist_from_guest(process, cf, item));
        }
        return value;
    default:
        throw std::runtime_error("Alchemy property list: object is not a property list type");
    }
}

std::uint64_t plist_to_guest(ProcessContext& process, CoreFoundation& cf, const Plist& value, std::uint64_t mutability) {
    switch (value.kind) {
    case Plist::Kind::uid: throw std::runtime_error("property list: a keyed-archive UID is not a property-list object");
    case Plist::Kind::string: {
        const auto string = cf.string(value.text);
        if (mutability < 2) return string;
        const auto copy = cf.mutable_copy(string);
        process.objects().release(string);
        return copy;
    }
    case Plist::Kind::integer: return cf.integer(value.integer, 4);
    case Plist::Kind::real: return cf.real(value.real);
    case Plist::Kind::boolean: return process.objects().retain(cf.boolean(value.boolean));
    case Plist::Kind::date: return cf.date(value.real);
    case Plist::Kind::data: {
        const auto storage = process.allocate_guest(std::max<std::size_t>(value.bytes.size(), 1));
        if (!value.bytes.empty()) process.main_engine().write_bytes(storage, value.bytes);
        std::uint64_t data{};
        try {
            if (mutability >= 2) {
                data = cf.invoke("CFDataCreateMutable", std::array<std::uint64_t, 2>{0, 0});
                static_cast<void>(cf.invoke("CFDataAppendBytes", std::array<std::uint64_t, 3>{data, storage, value.bytes.size()}));
            } else data = cf.invoke("CFDataCreate", std::array<std::uint64_t, 3>{0, storage, value.bytes.size()});
        } catch (...) { process.release_guest(storage); throw; }
        process.release_guest(storage);
        return data;
    }
    case Plist::Kind::array: {
        std::vector<std::uint64_t> items;
        try {
            for (const auto& item : value.items) items.push_back(plist_to_guest(process, cf, item, mutability));
            const auto array = cf.array(items, mutability >= 1);
            for (const auto item : items) process.objects().release(item);
            return array;
        } catch (...) { for (const auto item : items) process.objects().release(item); throw; }
    }
    case Plist::Kind::dictionary: {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> entries;
        std::vector<std::uint64_t> owned;
        try {
            for (const auto& [key, item] : value.entries) {
                const auto k = cf.string(key);
                owned.push_back(k);
                const auto v = plist_to_guest(process, cf, item, mutability);
                owned.push_back(v);
                entries.emplace_back(k, v);
            }
            const auto dictionary = cf.dictionary(entries, mutability >= 1);
            for (const auto item : owned) process.objects().release(item);
            return dictionary;
        } catch (...) { for (const auto item : owned) process.objects().release(item); throw; }
    }
    }
    throw std::runtime_error("Alchemy property list: unknown value kind");
}

} // namespace alchemy::export_fill
