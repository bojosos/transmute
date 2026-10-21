#include "alchemy/darwin/foundation/json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

// JSON parser and writer; see json.hpp.
namespace alchemy::gui::completion_kit::json {
namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    std::variant<Value, ParseError> run(bool allow_fragments) {
        try {
            // A UTF-8 byte order mark is accepted, as Foundation does.
            if (text_.substr(0, 3) == "\xEF\xBB\xBF") at_ = 3;
            space();
            if (at_ >= text_.size()) error("no value");
            if (!allow_fragments && text_[at_] != '{' && text_[at_] != '[') error("the top level is not an array or object (fragments are not allowed)");
            auto value = parse_value(0);
            space();
            if (at_ != text_.size()) error("garbage after the value");
            return value;
        } catch (const ParseError& failure) {
            return failure;
        }
    }

private:
    [[noreturn]] void error(std::string message) { throw ParseError{at_, std::move(message)}; }
    void space() { while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r')) ++at_; }
    bool literal(std::string_view word) {
        if (text_.substr(at_, word.size()) != word) return false;
        at_ += word.size();
        return true;
    }

    Value parse_value(int depth) {
        if (depth > 512) error("nesting is too deep");
        space();
        if (at_ >= text_.size()) error("unexpected end");
        const char c = text_[at_];
        if (c == '{') return parse_object(depth);
        if (c == '[') return parse_array(depth);
        if (c == '"') return Value{parse_string()};
        if (literal("true")) return Value{true};
        if (literal("false")) return Value{false};
        if (literal("null")) return Value{Null{}};
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        error("unexpected character");
    }

    Value parse_object(int depth) {
        ++at_;
        auto object = std::make_shared<Object>();
        space();
        if (at_ < text_.size() && text_[at_] == '}') { ++at_; return Value{object}; }
        for (;;) {
            space();
            if (at_ >= text_.size() || text_[at_] != '"') error("object key is not a string");
            auto key = parse_string();
            space();
            if (at_ >= text_.size() || text_[at_] != ':') error("missing ':' after an object key");
            ++at_;
            auto value = parse_value(depth + 1);
            // A repeated key keeps the last value (as Foundation's dictionaries do).
            const auto found = std::find_if(object->begin(), object->end(), [&](const auto& entry) { return entry.first == key; });
            if (found != object->end()) found->second = std::move(value);
            else object->emplace_back(std::move(key), std::move(value));
            space();
            if (at_ < text_.size() && text_[at_] == ',') { ++at_; continue; }
            if (at_ < text_.size() && text_[at_] == '}') { ++at_; return Value{object}; }
            error("missing ',' or '}' in an object");
        }
    }

    Value parse_array(int depth) {
        ++at_;
        auto array = std::make_shared<Array>();
        space();
        if (at_ < text_.size() && text_[at_] == ']') { ++at_; return Value{array}; }
        for (;;) {
            array->push_back(parse_value(depth + 1));
            space();
            if (at_ < text_.size() && text_[at_] == ',') { ++at_; continue; }
            if (at_ < text_.size() && text_[at_] == ']') { ++at_; return Value{array}; }
            error("missing ',' or ']' in an array");
        }
    }

    unsigned hex4() {
        if (at_ + 4 > text_.size()) error("truncated \\u escape");
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[at_++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned>(c - 'A' + 10);
            else error("bad \\u escape");
        }
        return value;
    }

    std::u16string parse_string() {
        ++at_;   // opening quote
        std::u16string out;
        for (;;) {
            if (at_ >= text_.size()) error("unterminated string");
            const auto c = static_cast<unsigned char>(text_[at_]);
            if (c == '"') { ++at_; return out; }
            if (c < 0x20) error("control character in a string");
            if (c == '\\') {
                ++at_;
                if (at_ >= text_.size()) error("truncated escape");
                const char e = text_[at_++];
                switch (e) {
                case '"': out.push_back(u'"'); break;
                case '\\': out.push_back(u'\\'); break;
                case '/': out.push_back(u'/'); break;
                case 'b': out.push_back(u'\b'); break;
                case 'f': out.push_back(u'\f'); break;
                case 'n': out.push_back(u'\n'); break;
                case 'r': out.push_back(u'\r'); break;
                case 't': out.push_back(u'\t'); break;
                case 'u': out.push_back(static_cast<char16_t>(hex4())); break;   // surrogate pairs pass through as UTF-16
                default: error("bad escape");
                }
                continue;
            }
            // UTF-8 to UTF-16.
            std::uint32_t code{};
            std::size_t length{};
            if (c < 0x80) { code = c; length = 1; }
            else if ((c & 0xE0) == 0xC0) { code = c & 0x1F; length = 2; }
            else if ((c & 0xF0) == 0xE0) { code = c & 0x0F; length = 3; }
            else if ((c & 0xF8) == 0xF0) { code = c & 0x07; length = 4; }
            else error("invalid UTF-8");
            if (at_ + length > text_.size()) error("truncated UTF-8");
            for (std::size_t i = 1; i < length; ++i) {
                const auto next = static_cast<unsigned char>(text_[at_ + i]);
                if ((next & 0xC0) != 0x80) error("invalid UTF-8");
                code = (code << 6) | (next & 0x3F);
            }
            if ((length == 2 && code < 0x80) || (length == 3 && code < 0x800) || (length == 4 && (code < 0x10000 || code > 0x10FFFF)) ||
                (code >= 0xD800 && code <= 0xDFFF))
                error("invalid UTF-8");
            at_ += length;
            if (code >= 0x10000) {
                code -= 0x10000;
                out.push_back(static_cast<char16_t>(0xD800 + (code >> 10)));
                out.push_back(static_cast<char16_t>(0xDC00 + (code & 0x3FF)));
            } else {
                out.push_back(static_cast<char16_t>(code));
            }
        }
    }

    Value parse_number() {
        const auto start = at_;
        if (text_[at_] == '-') ++at_;
        if (at_ >= text_.size() || !(text_[at_] >= '0' && text_[at_] <= '9')) error("bad number");
        if (text_[at_] == '0') ++at_;
        else while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_;
        bool real = false;
        if (at_ < text_.size() && text_[at_] == '.') {
            real = true;
            ++at_;
            if (at_ >= text_.size() || !(text_[at_] >= '0' && text_[at_] <= '9')) error("bad fraction");
            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_;
        }
        if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E')) {
            real = true;
            ++at_;
            if (at_ < text_.size() && (text_[at_] == '+' || text_[at_] == '-')) ++at_;
            if (at_ >= text_.size() || !(text_[at_] >= '0' && text_[at_] <= '9')) error("bad exponent");
            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_;
        }
        const auto token = text_.substr(start, at_ - start);
        if (!real) {
            std::int64_t integer{};
            const auto [end, code] = std::from_chars(token.data(), token.data() + token.size(), integer);
            if (code == std::errc() && end == token.data() + token.size()) return Value{integer};
        }
        double value{};
        const auto [end, code] = std::from_chars(token.data(), token.data() + token.size(), value);
        if (code != std::errc() || end != token.data() + token.size()) error("number out of range");
        return Value{value};
    }

    std::string_view text_;
    std::size_t at_{};
};

void append_utf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) out.push_back(static_cast<char>(code));
    else if (code < 0x800) { out.push_back(static_cast<char>(0xC0 | (code >> 6))); out.push_back(static_cast<char>(0x80 | (code & 0x3F))); }
    else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

void write_string(std::string& out, const std::u16string& value, const WriteOptions& options) {
    out.push_back('"');
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto c = value[i];
        switch (c) {
        case u'"': out += "\\\""; continue;
        case u'\\': out += "\\\\"; continue;
        case u'\n': out += "\\n"; continue;
        case u'\r': out += "\\r"; continue;
        case u'\t': out += "\\t"; continue;
        case u'\b': out += "\\b"; continue;
        case u'\f': out += "\\f"; continue;
        case u'/': out += options.escape_slashes ? "\\/" : "/"; continue;
        default: break;
        }
        if (c < 0x20) {
            char buffer[8];
            std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(c));
            out += buffer;
            continue;
        }
        std::uint32_t code = c;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < value.size() && value[i + 1] >= 0xDC00 && value[i + 1] <= 0xDFFF) {
            code = 0x10000 + ((static_cast<std::uint32_t>(c) - 0xD800) << 10) + (value[i + 1] - 0xDC00);
            ++i;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            code = 0xFFFD;   // a lone surrogate
        }
        append_utf8(out, code);
    }
    out.push_back('"');
}

bool write_value(std::string& out, const Value& value, const WriteOptions& options, int depth) {
    const auto newline = [&](int level) {
        if (!options.pretty) return;
        out.push_back('\n');
        out.append(static_cast<std::size_t>(level) * 2, ' ');
    };
    if (std::holds_alternative<Null>(value.data)) { out += "null"; return true; }
    if (const auto* flag = std::get_if<bool>(&value.data)) { out += *flag ? "true" : "false"; return true; }
    if (const auto* integer = std::get_if<std::int64_t>(&value.data)) { out += std::to_string(*integer); return true; }
    if (const auto* real = std::get_if<double>(&value.data)) {
        if (!std::isfinite(*real)) return false;
        if (*real == std::floor(*real) && std::fabs(*real) < 1e15) { out += std::to_string(static_cast<std::int64_t>(*real)); return true; }
        char buffer[64];
        const auto [end, code] = std::to_chars(buffer, buffer + sizeof buffer, *real);
        out.append(buffer, end);
        return true;
    }
    if (const auto* text = std::get_if<std::u16string>(&value.data)) { write_string(out, *text, options); return true; }
    if (const auto* array = std::get_if<std::shared_ptr<Array>>(&value.data)) {
        out.push_back('[');
        bool first = true;
        for (const auto& element : **array) {
            if (!first) out.push_back(',');
            first = false;
            newline(depth + 1);
            if (!write_value(out, element, options, depth + 1)) return false;
        }
        if (!(*array)->empty()) newline(depth);
        out.push_back(']');
        return true;
    }
    const auto& object = **std::get_if<std::shared_ptr<Object>>(&value.data);
    std::vector<const std::pair<std::u16string, Value>*> entries;
    for (const auto& entry : object) entries.push_back(&entry);
    if (options.sorted_keys) std::stable_sort(entries.begin(), entries.end(), [](auto* a, auto* b) { return a->first < b->first; });
    out.push_back('{');
    bool first = true;
    for (const auto* entry : entries) {
        if (!first) out.push_back(',');
        first = false;
        newline(depth + 1);
        write_string(out, entry->first, options);
        out += options.pretty ? " : " : ":";
        if (!write_value(out, entry->second, options, depth + 1)) return false;
    }
    if (!entries.empty()) newline(depth);
    out.push_back('}');
    return true;
}

} // namespace

std::variant<Value, ParseError> parse(std::string_view utf8, bool allow_fragments) { return Parser(utf8).run(allow_fragments); }

std::optional<std::string> write(const Value& value, const WriteOptions& options) {
    std::string out;
    if (!write_value(out, value, options, 0)) return std::nullopt;
    return out;
}

} // namespace alchemy::gui::completion_kit::json
