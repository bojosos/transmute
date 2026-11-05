#include "alchemy/darwin/launchd/plist.hpp"

#include "alchemy/host/files.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <ctime>
#include <limits>

// XML: the plist 1.0 elements (dict, key, array, string, integer, real, true,
// false, date, data), comments, the XML declaration and DOCTYPE, CDATA and the
// five named entities plus numeric references. Binary: the bplist00 trailer,
// offset table and object markers; UIDs and sets are refused (launchd.plist
// never holds them).
namespace alchemy::launchd {
namespace {

// ---- base64 -------------------------------------------------------------------------------------
constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::vector<std::uint8_t> base64_decode(std::string_view text) {
    std::vector<std::uint8_t> out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const auto c : text) {
        if (c == '=' ) break;
        const auto at = alphabet.find(c);
        if (at == std::string_view::npos) {
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
            throw PlistError("plist: bad character in <data>");
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(at);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xff));
        }
    }
    return out;
}

std::string base64_encode(std::span<const std::uint8_t> bytes) {
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const auto n = (std::uint32_t{bytes[i]} << 16) | (std::uint32_t{bytes[i + 1]} << 8) | bytes[i + 2];
        for (int shift = 18; shift >= 0; shift -= 6) out.push_back(alphabet[(n >> shift) & 63]);
    }
    if (i < bytes.size()) {
        auto n = std::uint32_t{bytes[i]} << 16;
        if (i + 1 < bytes.size()) n |= std::uint32_t{bytes[i + 1]} << 8;
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
        out.push_back(i + 1 < bytes.size() ? alphabet[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

// ---- dates (seconds since 2001-01-01 UTC <-> ISO 8601) ------------------------------------------------
constexpr std::int64_t reference_epoch = 978307200;   // 2001-01-01T00:00:00Z in Unix time

std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const auto era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const auto doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const auto doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

std::string iso_date(double reference_seconds) {
    const auto unix_seconds = static_cast<std::int64_t>(std::floor(reference_seconds)) + reference_epoch;
    auto days = unix_seconds / 86400;
    auto rest = unix_seconds % 86400;
    if (rest < 0) { rest += 86400; --days; }
    // civil_from_days
    days += 719468;
    const auto era = (days >= 0 ? days : days - 146096) / 146097;
    const auto doe = static_cast<unsigned>(days - era * 146097);
    const auto yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    auto y = static_cast<std::int64_t>(yoe) + era * 400;
    const auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const auto mp = (5 * doy + 2) / 153;
    const auto d = doy - (153 * mp + 2) / 5 + 1;
    const auto m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    char text[32];
    std::snprintf(text, sizeof text, "%04lld-%02u-%02uT%02lld:%02lld:%02lldZ", static_cast<long long>(y), m, d,
                  static_cast<long long>(rest / 3600), static_cast<long long>(rest / 60 % 60), static_cast<long long>(rest % 60));
    return text;
}

void check_iso_date(std::string_view text) {
    // YYYY-MM-DDTHH:MM:SSZ
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':' || text[19] != 'Z')
        throw PlistError("plist: bad <date> " + std::string(text));
}

// ---- XML ----------------------------------------------------------------------------------------
class XmlReader {
public:
    explicit XmlReader(std::string_view text) : text_(text) {}

    Value document() {
        skip_misc();
        auto tag = open_tag();
        if (tag.name != "plist") throw PlistError("plist: the root element is <" + tag.name + ">, not <plist>");
        if (tag.empty) throw PlistError("plist: empty <plist>");
        skip_misc();
        auto value = element();
        skip_misc();
        close_tag("plist");
        return value;
    }

private:
    struct Tag { std::string name; bool empty{}; };

    [[noreturn]] void fail(const std::string& why) const { throw PlistError("plist: " + why + " (at byte " + std::to_string(at_) + ")"); }
    bool starts(std::string_view prefix) const { return text_.substr(at_).starts_with(prefix); }
    void skip_space() { while (at_ < text_.size() && std::strchr(" \t\r\n", text_[at_])) ++at_; }
    void skip_past(std::string_view end) {
        const auto found = text_.find(end, at_);
        if (found == std::string_view::npos) fail("unterminated markup");
        at_ = found + end.size();
    }
    // Whitespace, comments, processing instructions and the DOCTYPE.
    void skip_misc() {
        for (;;) {
            skip_space();
            if (starts("<!--")) skip_past("-->");
            else if (starts("<?")) skip_past("?>");
            else if (starts("<!DOCTYPE")) skip_past(">");
            else return;
        }
    }
    Tag open_tag() {
        if (at_ >= text_.size() || text_[at_] != '<') fail("expected an element");
        ++at_;
        Tag tag;
        while (at_ < text_.size() && !std::strchr(" \t\r\n/>", text_[at_])) tag.name.push_back(text_[at_++]);
        // Attributes (plist version) are skipped.
        while (at_ < text_.size() && text_[at_] != '>') {
            if (text_[at_] == '/' && at_ + 1 < text_.size() && text_[at_ + 1] == '>') { tag.empty = true; ++at_; break; }
            ++at_;
        }
        if (at_ >= text_.size()) fail("unterminated tag");
        ++at_;
        return tag;
    }
    void close_tag(std::string_view name) {
        skip_space();
        const auto expected = "</" + std::string(name);
        if (!starts(expected)) fail("expected " + expected + ">");
        at_ += expected.size();
        skip_space();
        if (at_ >= text_.size() || text_[at_] != '>') fail("bad closing tag");
        ++at_;
    }
    // Character data up to the next '<' that is not CDATA, entities decoded.
    std::string characters(std::string_view element) {
        std::string out;
        for (;;) {
            if (at_ >= text_.size()) fail("unterminated <" + std::string(element) + ">");
            if (starts("<![CDATA[")) {
                at_ += 9;
                const auto end = text_.find("]]>", at_);
                if (end == std::string_view::npos) fail("unterminated CDATA");
                out.append(text_.substr(at_, end - at_));
                at_ = end + 3;
                continue;
            }
            if (starts("<!--")) { skip_past("-->"); continue; }
            const auto c = text_[at_];
            if (c == '<') break;
            if (c != '&') { out.push_back(c); ++at_; continue; }
            const auto end = text_.find(';', at_);
            if (end == std::string_view::npos) fail("unterminated entity");
            const auto name = text_.substr(at_ + 1, end - at_ - 1);
            at_ = end + 1;
            if (name == "lt") out.push_back('<');
            else if (name == "gt") out.push_back('>');
            else if (name == "amp") out.push_back('&');
            else if (name == "quot") out.push_back('"');
            else if (name == "apos") out.push_back('\'');
            else if (name.starts_with('#')) {
                std::uint32_t code = 0;
                const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
                for (const auto d : name.substr(hex ? 2 : 1)) {
                    const int digit = d >= '0' && d <= '9' ? d - '0' : hex && d >= 'a' && d <= 'f' ? d - 'a' + 10 : hex && d >= 'A' && d <= 'F' ? d - 'A' + 10 : -1;
                    if (digit < 0) fail("bad character reference");
                    code = code * (hex ? 16 : 10) + static_cast<std::uint32_t>(digit);
                    if (code > 0x10ffff) fail("bad character reference");
                }
                // UTF-8
                if (code < 0x80) out.push_back(static_cast<char>(code));
                else if (code < 0x800) { out.push_back(static_cast<char>(0xc0 | (code >> 6))); out.push_back(static_cast<char>(0x80 | (code & 0x3f))); }
                else if (code < 0x10000) {
                    out.push_back(static_cast<char>(0xe0 | (code >> 12))); out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                } else {
                    out.push_back(static_cast<char>(0xf0 | (code >> 18))); out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
                    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f))); out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                }
            } else fail("unknown entity &" + std::string(name) + ";");
        }
        return out;
    }
    std::string text_element(const Tag& tag) {
        if (tag.empty) return {};
        auto value = characters(tag.name);
        close_tag(tag.name);
        return value;
    }
    static std::string trimmed(std::string value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    Value element() {
        if (++depth_ > 256) fail("nesting too deep");
        const auto tag = open_tag();
        Value value;
        if (tag.name == "dict") {
            value.kind = Value::Kind::dictionary;
            if (!tag.empty) {
                for (;;) {
                    skip_misc();
                    if (starts("</")) break;
                    const auto key_tag = open_tag();
                    if (key_tag.name != "key") fail("expected <key> in <dict>, found <" + key_tag.name + ">");
                    auto key = text_element(key_tag);
                    skip_misc();
                    auto member = element();
                    if (value.find(key)) fail("duplicate key " + key);
                    value.dictionary.emplace_back(std::move(key), std::move(member));
                }
                close_tag("dict");
            }
        } else if (tag.name == "array") {
            value.kind = Value::Kind::array;
            if (!tag.empty) {
                for (;;) {
                    skip_misc();
                    if (starts("</")) break;
                    value.array.push_back(element());
                }
                close_tag("array");
            }
        } else if (tag.name == "string") {
            value.text = text_element(tag);
        } else if (tag.name == "integer") {
            const auto text = trimmed(text_element(tag));
            try {
                std::size_t used = 0;
                if (text.starts_with("0x") || text.starts_with("0X")) value.integer = static_cast<std::int64_t>(std::stoull(text.substr(2), &used, 16)), used += 2;
                else if (text.starts_with('-')) value.integer = std::stoll(text, &used, 10);
                else value.integer = static_cast<std::int64_t>(std::stoull(text, &used, 10));
                if (used != text.size()) throw std::invalid_argument("trailing");
            } catch (const std::exception&) { fail("bad <integer> " + text); }
            value.kind = Value::Kind::integer;
        } else if (tag.name == "real") {
            const auto text = trimmed(text_element(tag));
            try {
                std::size_t used = 0;
                value.real = std::stod(text, &used);
                if (used != text.size()) throw std::invalid_argument("trailing");
            } catch (const std::exception&) { fail("bad <real> " + text); }
            value.kind = Value::Kind::real;
        } else if (tag.name == "true" || tag.name == "false") {
            if (!tag.empty) close_tag(tag.name);
            value.kind = Value::Kind::boolean;
            value.boolean = tag.name == "true";
        } else if (tag.name == "date") {
            value.text = trimmed(text_element(tag));
            check_iso_date(value.text);
            value.kind = Value::Kind::date;
        } else if (tag.name == "data") {
            value.data = base64_decode(text_element(tag));
            value.kind = Value::Kind::data;
        } else fail("unknown element <" + tag.name + ">");
        --depth_;
        return value;
    }

    std::string_view text_;
    std::size_t at_{};
    int depth_{};
};

// ---- binary ---------------------------------------------------------------------------------------
class BinaryReader {
public:
    explicit BinaryReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {
        if (bytes.size() < 8 + 32) fail("too short");
        const auto trailer = bytes.subspan(bytes.size() - 32);
        offset_size_ = trailer[6];
        ref_size_ = trailer[7];
        count_ = be(trailer.subspan(8, 8));
        top_ = be(trailer.subspan(16, 8));
        table_ = be(trailer.subspan(24, 8));
        if (offset_size_ < 1 || offset_size_ > 8 || ref_size_ < 1 || ref_size_ > 8) fail("bad trailer");
        if (count_ == 0 || top_ >= count_ || table_ < 8 || table_ > bytes.size() - 32 ||
            count_ > (bytes.size() - 32 - table_) / offset_size_) fail("bad offset table");
    }

    Value document() { return object(top_, 0); }

private:
    [[noreturn]] static void fail(const std::string& why) { throw PlistError("binary plist: " + why); }
    static std::uint64_t be(std::span<const std::uint8_t> bytes) {
        std::uint64_t value = 0;
        for (const auto byte : bytes) value = (value << 8) | byte;
        return value;
    }
    std::span<const std::uint8_t> span(std::uint64_t at, std::uint64_t size) const {
        if (at > table_ || size > table_ - at) fail("object outside the object area");
        return bytes_.subspan(static_cast<std::size_t>(at), static_cast<std::size_t>(size));
    }
    std::uint64_t offset_of(std::uint64_t index) const {
        if (index >= count_) fail("object reference out of range");
        return be(bytes_.subspan(static_cast<std::size_t>(table_ + index * offset_size_), offset_size_));
    }
    // The count of a marker's low nibble, or the integer object after it.
    std::uint64_t length(std::uint8_t low, std::uint64_t& at) const {
        if (low != 0xf) return low;
        const auto marker = span(at, 1)[0];
        if ((marker & 0xf0) != 0x10) fail("bad length");
        const auto size = std::uint64_t{1} << (marker & 0x0f);
        if (size > 8) fail("bad length");
        const auto value = be(span(at + 1, size));
        at += 1 + size;
        return value;
    }

    Value object(std::uint64_t index, int depth) {
        if (depth > 256) fail("nesting too deep");
        auto at = offset_of(index);
        const auto marker = span(at, 1)[0];
        ++at;
        const auto high = static_cast<std::uint8_t>(marker >> 4), low = static_cast<std::uint8_t>(marker & 0x0f);
        Value value;
        switch (high) {
        case 0x0:
            if (marker == 0x08 || marker == 0x09) { value.kind = Value::Kind::boolean; value.boolean = marker == 0x09; return value; }
            fail("unsupported marker");
        case 0x1: {
            const auto size = std::uint64_t{1} << low;
            if (size > 16) fail("bad integer");
            const auto bytes = span(at, size);
            // 16-byte integers keep their low 8 bytes.
            value.integer = static_cast<std::int64_t>(be(size == 16 ? bytes.subspan(8) : bytes));
            value.kind = Value::Kind::integer;
            return value;
        }
        case 0x2: {
            const auto size = std::uint64_t{1} << low;
            const auto raw = be(span(at, size));
            if (size == 4) { std::uint32_t bits = static_cast<std::uint32_t>(raw); float f; std::memcpy(&f, &bits, 4); value.real = f; }
            else if (size == 8) std::memcpy(&value.real, &raw, 8);
            else fail("bad real");
            value.kind = Value::Kind::real;
            return value;
        }
        case 0x3: {
            if (marker != 0x33) fail("bad date");
            const auto raw = be(span(at, 8));
            double seconds;
            std::memcpy(&seconds, &raw, 8);
            value.kind = Value::Kind::date;
            value.text = iso_date(seconds);
            return value;
        }
        case 0x4: {
            const auto size = length(low, at);
            const auto bytes = span(at, size);
            value.kind = Value::Kind::data;
            value.data.assign(bytes.begin(), bytes.end());
            return value;
        }
        case 0x5: {
            const auto size = length(low, at);
            const auto bytes = span(at, size);
            value.text.assign(bytes.begin(), bytes.end());   // ASCII
            return value;
        }
        case 0x6: {
            const auto units = length(low, at);
            if (units > std::numeric_limits<std::uint64_t>::max() / 2) fail("bad string");
            const auto bytes = span(at, units * 2);
            for (std::size_t i = 0; i < bytes.size(); i += 2) {
                std::uint32_t code = (std::uint32_t{bytes[i]} << 8) | bytes[i + 1];
                if (code >= 0xd800 && code < 0xdc00 && i + 3 < bytes.size()) {
                    const std::uint32_t low_unit = (std::uint32_t{bytes[i + 2]} << 8) | bytes[i + 3];
                    if (low_unit >= 0xdc00 && low_unit < 0xe000) { code = 0x10000 + ((code - 0xd800) << 10) + (low_unit - 0xdc00); i += 2; }
                }
                auto& out = value.text;
                if (code < 0x80) out.push_back(static_cast<char>(code));
                else if (code < 0x800) { out.push_back(static_cast<char>(0xc0 | (code >> 6))); out.push_back(static_cast<char>(0x80 | (code & 0x3f))); }
                else if (code < 0x10000) {
                    out.push_back(static_cast<char>(0xe0 | (code >> 12))); out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                } else {
                    out.push_back(static_cast<char>(0xf0 | (code >> 18))); out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
                    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f))); out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                }
            }
            return value;
        }
        case 0xa: {
            const auto count = length(low, at);
            const auto refs = span(at, count * ref_size_);
            value.kind = Value::Kind::array;
            for (std::uint64_t i = 0; i < count; ++i) value.array.push_back(object(be(refs.subspan(static_cast<std::size_t>(i * ref_size_), ref_size_)), depth + 1));
            return value;
        }
        case 0xd: {
            const auto count = length(low, at);
            const auto refs = span(at, count * 2 * ref_size_);
            value.kind = Value::Kind::dictionary;
            for (std::uint64_t i = 0; i < count; ++i) {
                auto key = object(be(refs.subspan(static_cast<std::size_t>(i * ref_size_), ref_size_)), depth + 1);
                if (key.kind != Value::Kind::string) fail("a dictionary key is not a string");
                auto member = object(be(refs.subspan(static_cast<std::size_t>((count + i) * ref_size_), ref_size_)), depth + 1);
                if (value.find(key.text)) fail("duplicate key " + key.text);
                value.dictionary.emplace_back(std::move(key.text), std::move(member));
            }
            return value;
        }
        default:
            fail("unsupported marker");
        }
    }

    std::span<const std::uint8_t> bytes_;
    std::uint64_t count_{}, top_{}, table_{};
    std::size_t offset_size_{}, ref_size_{};
};

std::string escaped(std::string_view text) {
    std::string out;
    for (const auto c : text) {
        if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '&') out += "&amp;";
        else out.push_back(c);
    }
    return out;
}

void write_xml(const Value& value, std::string& out, int depth) {
    const std::string pad(static_cast<std::size_t>(depth), '\t');
    switch (value.kind) {
    case Value::Kind::string: out += pad + "<string>" + escaped(value.text) + "</string>\n"; break;
    case Value::Kind::integer: out += pad + "<integer>" + std::to_string(value.integer) + "</integer>\n"; break;
    case Value::Kind::real: {
        char text[40];
        std::snprintf(text, sizeof text, "%.17g", value.real);
        out += pad + "<real>" + text + "</real>\n";
        break;
    }
    case Value::Kind::boolean: out += pad + (value.boolean ? "<true/>\n" : "<false/>\n"); break;
    case Value::Kind::date: out += pad + "<date>" + value.text + "</date>\n"; break;
    case Value::Kind::data: out += pad + "<data>" + base64_encode(value.data) + "</data>\n"; break;
    case Value::Kind::array:
        if (value.array.empty()) { out += pad + "<array/>\n"; break; }
        out += pad + "<array>\n";
        for (const auto& item : value.array) write_xml(item, out, depth + 1);
        out += pad + "</array>\n";
        break;
    case Value::Kind::dictionary:
        if (value.dictionary.empty()) { out += pad + "<dict/>\n"; break; }
        out += pad + "<dict>\n";
        for (const auto& [key, member] : value.dictionary) {
            out += pad + "\t<key>" + escaped(key) + "</key>\n";
            write_xml(member, out, depth + 1);
        }
        out += pad + "</dict>\n";
        break;
    }
}

} // namespace

Value Value::of_string(std::string value) { Value v; v.text = std::move(value); return v; }
Value Value::of_integer(std::int64_t value) { Value v; v.kind = Kind::integer; v.integer = value; return v; }
Value Value::of_boolean(bool value) { Value v; v.kind = Kind::boolean; v.boolean = value; return v; }
Value Value::of_array(std::vector<Value> values) { Value v; v.kind = Kind::array; v.array = std::move(values); return v; }
Value Value::of_dictionary() { Value v; v.kind = Kind::dictionary; return v; }

const Value* Value::find(std::string_view key) const noexcept {
    if (kind != Kind::dictionary) return nullptr;
    for (const auto& [name, member] : dictionary)
        if (name == key) return &member;
    return nullptr;
}

void Value::set(std::string key, Value value) {
    for (auto& [name, member] : dictionary)
        if (name == key) { member = std::move(value); return; }
    dictionary.emplace_back(std::move(key), std::move(value));
}

void Value::erase(std::string_view key) {
    std::erase_if(dictionary, [&](const auto& item) { return item.first == key; });
}

Value parse_plist(std::span<const std::uint8_t> bytes) {
    if (bytes.size() >= 8 && std::memcmp(bytes.data(), "bplist00", 8) == 0) return BinaryReader(bytes).document();
    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    return XmlReader(text).document();
}

Value read_plist_file(const std::string& host_path) {
    auto file = host::files::open(host_path, host::files::Access::read, host::files::Create::open_existing);
    if (!file) throw PlistError(host_path + ": cannot open (" + std::string(host::name(file.error())) + ")");
    host::OwnedHandle owned(*file);
    std::vector<std::uint8_t> bytes;
    std::array<std::uint8_t, 65536> buffer{};
    for (;;) {
        const auto count = host::files::read(owned.get(), std::as_writable_bytes(std::span(buffer)));
        if (!count) throw PlistError(host_path + ": cannot read (" + std::string(host::name(count.error())) + ")");
        if (!*count) break;
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*count));
        if (bytes.size() > 16 * 1024 * 1024) throw PlistError(host_path + ": too large for a property list");
    }
    return parse_plist(bytes);
}

std::string to_xml(const Value& value) {
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                      "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                      "<plist version=\"1.0\">\n";
    write_xml(value, out, 0);
    out += "</plist>\n";
    return out;
}

std::string to_text(const Value& value, int indent) {
    const std::string pad(static_cast<std::size_t>(indent) * 4, ' ');
    switch (value.kind) {
    case Value::Kind::string: return "\"" + value.text + "\"";
    case Value::Kind::integer: return std::to_string(value.integer);
    case Value::Kind::real: { char text[40]; std::snprintf(text, sizeof text, "%g", value.real); return text; }
    case Value::Kind::boolean: return value.boolean ? "true" : "false";
    case Value::Kind::date: return value.text;
    case Value::Kind::data: return "<" + std::to_string(value.data.size()) + " bytes>";
    case Value::Kind::array: {
        std::string out = "(\n";
        for (const auto& item : value.array) out += pad + "    " + to_text(item, indent + 1) + ";\n";
        return out + pad + ")";
    }
    case Value::Kind::dictionary: {
        std::string out = "{\n";
        for (const auto& [key, member] : value.dictionary) out += pad + "    \"" + key + "\" = " + to_text(member, indent + 1) + ";\n";
        return out + pad + "}";
    }
    }
    return {};
}

} // namespace alchemy::launchd
