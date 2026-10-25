// JSON document model (json.hpp).
#include "alchemy/darwin/metal/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace alchemy::metal::json {

Value::Value(std::int64_t value) : kind_(Kind::number), integer_(true), negative_(value < 0),
    bits_(value < 0 ? 0 - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value)),
    number_(static_cast<double>(value)) {}

Value::Value(std::uint64_t value) : kind_(Kind::number), integer_(true), bits_(value), number_(static_cast<double>(value)) {}

std::optional<bool> Value::as_bool() const {
    if (kind_ != Kind::boolean) return std::nullopt;
    return boolean_;
}

std::optional<std::uint64_t> Value::as_u64() const {
    if (kind_ != Kind::number || !integer_ || negative_) return std::nullopt;
    return bits_;
}

std::optional<std::int64_t> Value::as_i64() const {
    if (kind_ != Kind::number || !integer_) return std::nullopt;
    if (!negative_) {
        if (bits_ > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
        return static_cast<std::int64_t>(bits_);
    }
    if (bits_ > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1) return std::nullopt;
    return static_cast<std::int64_t>(0 - bits_);
}

std::optional<double> Value::as_double() const {
    if (kind_ != Kind::number) return std::nullopt;
    return number_;
}

const Value* Value::find(std::string_view name) const {
    if (kind_ != Kind::object) return nullptr;
    for (std::size_t i = names_.size(); i-- > 0;)
        if (names_[i] == name) return &items_[i];
    return nullptr;
}

namespace {
const Value& null_value() {
    static const Value value;
    return value;
}
} // namespace

const Value& Value::operator[](std::string_view name) const {
    const auto* found = find(name);
    return found ? *found : null_value();
}

const Value& Value::operator[](std::size_t index) const {
    return kind_ == Kind::array && index < items_.size() ? items_[index] : null_value();
}

Value& Value::set(std::string_view name, Value value) {
    if (kind_ != Kind::object) throw std::logic_error("json: set on a non-object");
    for (std::size_t i = 0; i < names_.size(); ++i)
        if (names_[i] == name) return items_[i] = std::move(value);
    names_.emplace_back(name);
    return items_.emplace_back(std::move(value));
}

Value& Value::push(Value value) {
    if (kind_ != Kind::array) throw std::logic_error("json: push on a non-array");
    return items_.emplace_back(std::move(value));
}

namespace {
void dump_string(std::string_view s, std::string& out) {
    out += '"';
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char escape[8];
                std::snprintf(escape, sizeof escape, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += escape;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}
} // namespace

void Value::dump(std::string& out) const {
    switch (kind_) {
    case Kind::null: out += "null"; break;
    case Kind::boolean: out += boolean_ ? "true" : "false"; break;
    case Kind::number:
        if (integer_) {
            if (negative_) out += '-';
            out += std::to_string(bits_);
        } else if (std::isfinite(number_)) {
            char buffer[32];
            const auto [end, error] = std::to_chars(buffer, buffer + sizeof buffer, number_);
            out.append(buffer, error == std::errc{} ? end : buffer);
        } else {
            out += "null";   // JSON has no infinities or NaN
        }
        break;
    case Kind::string: dump_string(string_, out); break;
    case Kind::array:
        out += '[';
        for (std::size_t i = 0; i < items_.size(); ++i) {
            if (i) out += ',';
            items_[i].dump(out);
        }
        out += ']';
        break;
    case Kind::object:
        out += '{';
        for (std::size_t i = 0; i < items_.size(); ++i) {
            if (i) out += ',';
            dump_string(names_[i], out);
            out += ':';
            items_[i].dump(out);
        }
        out += '}';
        break;
    }
}

std::string Value::dump() const {
    std::string out;
    dump(out);
    return out;
}

bool operator==(const Value& a, const Value& b) {
    if (a.kind_ != b.kind_) return false;
    switch (a.kind_) {
    case Value::Kind::null: return true;
    case Value::Kind::boolean: return a.boolean_ == b.boolean_;
    case Value::Kind::number:
        if (a.integer_ && b.integer_) return a.negative_ == b.negative_ && a.bits_ == b.bits_;
        return a.number_ == b.number_;
    case Value::Kind::string: return a.string_ == b.string_;
    case Value::Kind::array: return a.items_ == b.items_;
    case Value::Kind::object: return a.names_ == b.names_ && a.items_ == b.items_;
    }
    return false;
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Value document() {
        auto value = parse_value(0);
        skip_space();
        if (at_ != text_.size()) fail("trailing characters");
        return value;
    }

private:
    std::string_view text_;
    std::size_t at_{};

    [[noreturn]] void fail(const char* reason) const {
        throw std::runtime_error("malformed JSON at offset " + std::to_string(at_) + ": " + reason);
    }
    void skip_space() {
        while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\n' || text_[at_] == '\r' || text_[at_] == '\t')) ++at_;
    }
    bool take(std::string_view word) {
        if (text_.substr(at_, word.size()) != word) return false;
        at_ += word.size();
        return true;
    }
    void expect(char c) {
        skip_space();
        if (at_ >= text_.size() || text_[at_] != c) fail("unexpected character");
        ++at_;
    }

    Value parse_value(int depth) {
        if (depth > 256) fail("nested too deeply");
        skip_space();
        if (at_ >= text_.size()) fail("unexpected end");
        const char c = text_[at_];
        if (c == '{') return parse_object(depth);
        if (c == '[') return parse_array(depth);
        if (c == '"') return Value(parse_string());
        if (take("true")) return Value(true);
        if (take("false")) return Value(false);
        if (take("null")) return Value();
        return parse_number();
    }

    Value parse_object(int depth) {
        ++at_;
        auto value = Value::object();
        skip_space();
        if (at_ < text_.size() && text_[at_] == '}') { ++at_; return value; }
        while (true) {
            skip_space();
            if (at_ >= text_.size() || text_[at_] != '"') fail("expected a member name");
            auto name = parse_string();
            expect(':');
            value.names_.push_back(std::move(name));
            value.items_.push_back(parse_value(depth + 1));
            skip_space();
            if (at_ < text_.size() && text_[at_] == ',') { ++at_; continue; }
            expect('}');
            return value;
        }
    }

    Value parse_array(int depth) {
        ++at_;
        auto value = Value::array();
        skip_space();
        if (at_ < text_.size() && text_[at_] == ']') { ++at_; return value; }
        while (true) {
            value.items_.push_back(parse_value(depth + 1));
            skip_space();
            if (at_ < text_.size() && text_[at_] == ',') { ++at_; continue; }
            expect(']');
            return value;
        }
    }

    static void append_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::uint32_t parse_hex4() {
        if (at_ + 4 > text_.size()) fail("short \\u escape");
        std::uint32_t value{};
        const auto [end, error] = std::from_chars(text_.data() + at_, text_.data() + at_ + 4, value, 16);
        if (error != std::errc{} || end != text_.data() + at_ + 4) fail("bad \\u escape");
        at_ += 4;
        return value;
    }

    std::string parse_string() {
        ++at_;   // the opening quote
        std::string out;
        while (true) {
            if (at_ >= text_.size()) fail("unterminated string");
            const char c = text_[at_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) fail("control character in a string");
            if (c != '\\') { out += c; continue; }
            if (at_ >= text_.size()) fail("unterminated escape");
            switch (text_[at_++]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                auto cp = parse_hex4();
                if (cp >= 0xD800 && cp < 0xDC00 && take("\\u")) {
                    const auto low = parse_hex4();
                    if (low < 0xDC00 || low >= 0xE000) fail("bad surrogate pair");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                append_utf8(out, cp);
                break;
            }
            default: fail("unknown escape");
            }
        }
    }

    Value parse_number() {
        const auto start = at_;
        if (at_ < text_.size() && text_[at_] == '-') ++at_;
        bool fraction = false;
        while (at_ < text_.size()) {
            const char c = text_[at_];
            if (c >= '0' && c <= '9') { ++at_; continue; }
            if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') { fraction = true; ++at_; continue; }
            break;
        }
        const auto token = text_.substr(start, at_ - start);
        if (token.empty() || token == "-") fail("expected a value");
        Value value;
        value.kind_ = Value::Kind::number;
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value.number_);
        if (error != std::errc{} || end != token.data() + token.size()) fail("bad number");
        if (!fraction) {
            const bool negative = token.front() == '-';
            const auto digits = negative ? token.substr(1) : token;
            std::uint64_t bits{};
            const auto [iend, ierror] = std::from_chars(digits.data(), digits.data() + digits.size(), bits);
            if (ierror == std::errc{} && iend == digits.data() + digits.size()) {
                value.integer_ = true;
                value.negative_ = negative && bits != 0;
                value.bits_ = bits;
            }
        }
        return value;
    }
};

Value Value::parse(std::string_view text) { return Parser(text).document(); }

} // namespace alchemy::metal::json
