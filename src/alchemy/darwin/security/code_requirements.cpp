#include "alchemy/darwin/security/code_requirements.hpp"

#include "alchemy/darwin/corefoundation/cf_plist.hpp"
#include "alchemy/darwin/security/ber.hpp"
#include "alchemy/darwin/security/keys.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace alchemy::gui::security::codesign {
namespace {

using export_fill::Plist;

constexpr std::uint32_t magic_requirement = 0xfade0c00, magic_requirement_set = 0xfade0c01, expression_form = 1;

// Expression opcodes (the low 24 bits; the high byte holds flags).
enum Op : std::uint32_t {
    op_false = 0, op_true = 1, op_ident = 2, op_apple_anchor = 3, op_anchor_hash = 4, op_info_key_value = 5, op_and = 6,
    op_or = 7, op_cdhash = 8, op_not = 9, op_info_key_field = 10, op_cert_field = 11, op_trusted_cert = 12,
    op_trusted_certs = 13, op_cert_generic = 14, op_apple_generic_anchor = 15, op_entitlement_field = 16,
    op_cert_policy = 17, op_named_anchor = 18, op_named_code = 19, op_platform = 20, op_notarized = 21,
    op_cert_field_date = 22, op_legacy_dev_id = 23,
};
enum Match : std::uint32_t {
    match_exists = 0, match_equal = 1, match_contains = 2, match_begins_with = 3, match_ends_with = 4, match_less_than = 5,
    match_greater_than = 6, match_less_equal = 7, match_greater_equal = 8, match_absent = 14,
};
bool match_has_value(std::uint32_t match) { return match >= match_equal && match <= match_greater_equal; }

constexpr int prec_or = 1, prec_and = 2, prec_unary = 3;

// ---- the binary form -------------------------------------------------------------------------------------

struct Node {
    std::uint32_t op{};
    std::vector<Node> children;
    std::int32_t slot{}, integer{};
    std::string text;         // identifiers, keys, cert field names, named anchors/code
    Bytes data;               // hashes, OIDs
    std::uint32_t match{};
    std::string value;        // the match value
};

struct Writer {
    Bytes out;
    void u32(std::uint32_t value) { for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift)); }
    void bytes(std::span<const std::uint8_t> value) {
        u32(static_cast<std::uint32_t>(value.size()));
        out.insert(out.end(), value.begin(), value.end());
        while (out.size() % 4) out.push_back(0);
    }
    void text(std::string_view value) { bytes(std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size())); }
    void match(const Node& node) {
        u32(node.match);
        if (match_has_value(node.match)) text(node.value);
    }
    void node(const Node& node) {
        u32(node.op);
        switch (node.op) {
        case op_ident: case op_named_anchor: case op_named_code: text(node.text); break;
        case op_anchor_hash: u32(static_cast<std::uint32_t>(node.slot)); bytes(node.data); break;
        case op_and: case op_or: this->node(node.children[0]); this->node(node.children[1]); break;
        case op_not: this->node(node.children[0]); break;
        case op_cdhash: bytes(node.data); break;
        case op_info_key_field: case op_entitlement_field: text(node.text); match(node); break;
        case op_cert_field: u32(static_cast<std::uint32_t>(node.slot)); text(node.text); match(node); break;
        case op_trusted_cert: u32(static_cast<std::uint32_t>(node.slot)); break;
        case op_cert_generic: case op_cert_policy: u32(static_cast<std::uint32_t>(node.slot)); bytes(node.data); match(node); break;
        case op_platform: u32(static_cast<std::uint32_t>(node.integer)); break;
        default: break;
        }
    }
};

struct Reader {
    std::span<const std::uint8_t> data;
    std::size_t at{};
    bool failed{};
    std::uint32_t u32() {
        if (at + 4 > data.size()) { failed = true; return 0; }
        const auto value = (static_cast<std::uint32_t>(data[at]) << 24) | (static_cast<std::uint32_t>(data[at + 1]) << 16) |
                           (static_cast<std::uint32_t>(data[at + 2]) << 8) | data[at + 3];
        at += 4;
        return value;
    }
    Bytes bytes() {
        const auto size = u32();
        if (failed || at + size > data.size()) { failed = true; return {}; }
        Bytes value(data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + size));
        at += (size + 3) & ~std::size_t{3};
        return value;
    }
    std::string text() { const auto value = bytes(); return std::string(value.begin(), value.end()); }
    void match(Node& node) {
        node.match = u32();
        if (match_has_value(node.match)) node.value = text();
        else if (node.match != match_exists && node.match != match_absent) failed = true;   // date matches: not read
    }
    std::optional<Node> node(int depth = 0) {
        if (depth > 64) { failed = true; return std::nullopt; }
        Node node;
        node.op = u32() & 0x00ffffff;
        switch (node.op) {
        case op_false: case op_true: case op_apple_anchor: case op_trusted_certs: case op_apple_generic_anchor:
        case op_notarized: case op_legacy_dev_id: break;
        case op_ident: case op_named_anchor: case op_named_code: node.text = text(); break;
        case op_anchor_hash: node.slot = static_cast<std::int32_t>(u32()); node.data = bytes(); break;
        case op_info_key_value: node.text = text(); node.value = text(); break;
        case op_and: case op_or: {
            auto left = this->node(depth + 1);
            auto right = left ? this->node(depth + 1) : std::nullopt;
            if (!left || !right) return std::nullopt;
            node.children = {std::move(*left), std::move(*right)};
            break;
        }
        case op_not: {
            auto inner = this->node(depth + 1);
            if (!inner) return std::nullopt;
            node.children = {std::move(*inner)};
            break;
        }
        case op_cdhash: node.data = bytes(); break;
        case op_info_key_field: case op_entitlement_field: node.text = text(); match(node); break;
        case op_cert_field: node.slot = static_cast<std::int32_t>(u32()); node.text = text(); match(node); break;
        case op_trusted_cert: node.slot = static_cast<std::int32_t>(u32()); break;
        case op_cert_generic: case op_cert_policy: case op_cert_field_date:
            node.slot = static_cast<std::int32_t>(u32()); node.data = bytes(); match(node); break;
        case op_platform: node.integer = static_cast<std::int32_t>(u32()); break;
        default: failed = true; return std::nullopt;
        }
        if (failed) return std::nullopt;
        return node;
    }
};

std::optional<Node> read_requirement(std::span<const std::uint8_t> blob) {
    Reader reader{blob};
    if (reader.u32() != magic_requirement) return std::nullopt;
    const auto length = reader.u32();
    if (length > blob.size() || length < 12) return std::nullopt;
    reader.data = blob.first(length);
    if (reader.u32() != expression_form) return std::nullopt;
    auto node = reader.node();
    if (reader.failed) return std::nullopt;
    return node;
}

Bytes write_requirement(const Node& node) {
    Writer writer;
    writer.u32(magic_requirement);
    writer.u32(0);
    writer.u32(expression_form);
    writer.node(node);
    const auto size = static_cast<std::uint32_t>(writer.out.size());
    for (int index = 0; index < 4; ++index) writer.out[4 + index] = static_cast<std::uint8_t>(size >> (24 - 8 * index));
    return writer.out;
}

// ---- text ---------------------------------------------------------------------------------------------------

std::string hex(std::span<const std::uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (const auto byte : bytes) { text += digits[byte >> 4]; text += digits[byte & 15]; }
    return text;
}
std::string requirement_string(std::string_view value) {
    const bool bare = !value.empty() && (std::isalpha(static_cast<unsigned char>(value[0])) || value[0] == '_') &&
        std::all_of(value.begin(), value.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }) &&
        value != "and" && value != "or" && value != "exists" && value != "absent";
    if (bare) return std::string(value);
    std::string text = "\"";
    for (const auto c : value) {
        if (c == '"' || c == '\\') text += '\\';
        text += c;
    }
    return text + "\"";
}
std::string slot_name(std::int32_t slot) {
    if (slot == 0) return "leaf";
    if (slot == -1) return "root";
    return std::to_string(slot);
}
std::string match_text(const Node& node) {
    switch (node.match) {
    case match_exists: return " /* exists */";
    case match_absent: return " absent ";
    case match_equal: return " = " + requirement_string(node.value);
    case match_contains: return " = *" + requirement_string(node.value) + "*";
    case match_begins_with: return " = " + requirement_string(node.value) + "*";
    case match_ends_with: return " = *" + requirement_string(node.value);
    case match_less_than: return " < " + requirement_string(node.value);
    case match_greater_than: return " > " + requirement_string(node.value);
    case match_less_equal: return " <= " + requirement_string(node.value);
    case match_greater_equal: return " >= " + requirement_string(node.value);
    default: return " /* unsupported match */";
    }
}
std::pair<std::string, int> text_of(const Node& node) {
    const auto wrap = [](const std::pair<std::string, int>& inner, int at_least) {
        return inner.second < at_least ? "(" + inner.first + ")" : inner.first;
    };
    switch (node.op) {
    case op_false: return {"never", prec_unary};
    case op_true: return {"always", prec_unary};
    case op_ident: return {"identifier " + requirement_string(node.text), prec_unary};
    case op_apple_anchor: return {"anchor apple", prec_unary};
    case op_apple_generic_anchor: return {"anchor apple generic", prec_unary};
    case op_anchor_hash: return {"certificate " + slot_name(node.slot) + " = H\"" + hex(node.data) + "\"", prec_unary};
    case op_info_key_value: return {"info[" + node.text + "] = " + requirement_string(node.value), prec_unary};
    case op_and: return {wrap(text_of(node.children[0]), prec_and) + " and " + wrap(text_of(node.children[1]), prec_and), prec_and};
    case op_or: return {wrap(text_of(node.children[0]), prec_or) + " or " + wrap(text_of(node.children[1]), prec_or), prec_or};
    case op_not: return {"! " + wrap(text_of(node.children[0]), prec_unary), prec_unary};
    case op_cdhash: return {"cdhash H\"" + hex(node.data) + "\"", prec_unary};
    case op_info_key_field: return {"info[" + node.text + "]" + match_text(node), prec_unary};
    case op_entitlement_field: return {"entitlement[" + node.text + "]" + match_text(node), prec_unary};
    case op_cert_field: return {"certificate " + slot_name(node.slot) + "[" + node.text + "]" + match_text(node), prec_unary};
    case op_trusted_cert: return {"certificate " + slot_name(node.slot) + " trusted", prec_unary};
    case op_trusted_certs: return {"anchor trusted", prec_unary};
    case op_cert_generic: return {"certificate " + slot_name(node.slot) + "[field." + ber::oid_text(node.data) + "]" + match_text(node), prec_unary};
    case op_cert_policy: return {"certificate " + slot_name(node.slot) + "[policy." + ber::oid_text(node.data) + "]" + match_text(node), prec_unary};
    case op_cert_field_date: return {"certificate " + slot_name(node.slot) + "[timestamp." + ber::oid_text(node.data) + "]" + match_text(node), prec_unary};
    case op_named_anchor: return {"anchor " + node.text, prec_unary};
    case op_named_code: return {"(" + node.text + ")", prec_unary};
    case op_platform: return {"platform = " + std::to_string(node.integer), prec_unary};
    case op_notarized: return {"notarized", prec_unary};
    case op_legacy_dev_id: return {"legacy", prec_unary};
    default: return {"/* unknown */", prec_unary};
    }
}

// ---- parsing ------------------------------------------------------------------------------------------------

struct Token {
    enum class Kind { end, word, string, hash, symbol } kind{Kind::end};
    std::string text;
};

std::optional<std::vector<Token>> tokenize(std::string_view source, std::string& error) {
    std::vector<Token> tokens;
    std::size_t at = 0;
    const auto word_char = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-' || c == '/' || c == '$'; };
    while (at < source.size()) {
        const auto c = source[at];
        if (std::isspace(static_cast<unsigned char>(c))) { ++at; continue; }
        if (source.substr(at, 2) == "/*") {
            const auto end = source.find("*/", at + 2);
            if (end == std::string_view::npos) { error = "unterminated comment"; return std::nullopt; }
            at = end + 2;
            continue;
        }
        if (source.substr(at, 2) == "//") { at = std::min(source.find('\n', at), source.size()); continue; }
        if (c == '"' || (c == 'H' && at + 1 < source.size() && source[at + 1] == '"')) {
            const bool hash = c == 'H';
            at += hash ? 2 : 1;
            std::string text;
            while (at < source.size() && source[at] != '"') {
                if (source[at] == '\\' && at + 1 < source.size()) ++at;
                text += source[at++];
            }
            if (at >= source.size()) { error = "unterminated string"; return std::nullopt; }
            ++at;
            tokens.push_back({hash ? Token::Kind::hash : Token::Kind::string, text});
            continue;
        }
        if (source.substr(at, 2) == "<=" || source.substr(at, 2) == ">=" || source.substr(at, 2) == "&&" || source.substr(at, 2) == "||") {
            tokens.push_back({Token::Kind::symbol, std::string(source.substr(at, 2))});
            at += 2;
            continue;
        }
        if (std::string_view("()[]!=<>*").find(c) != std::string_view::npos) {
            tokens.push_back({Token::Kind::symbol, std::string(1, c)});
            ++at;
            continue;
        }
        if (word_char(c)) {
            const auto start = at;
            while (at < source.size() && word_char(source[at])) ++at;
            tokens.push_back({Token::Kind::word, std::string(source.substr(start, at - start))});
            continue;
        }
        error = std::string("unexpected character '") + c + "'";
        return std::nullopt;
    }
    tokens.push_back({Token::Kind::end, {}});
    return tokens;
}

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}
    std::optional<Node> parse(std::string& error) {
        try {
            auto node = expression();
            if (peek().kind != Token::Kind::end) fail("unexpected \"" + peek().text + "\"");
            return node;
        } catch (const std::invalid_argument& failure) {
            error = failure.what();
            return std::nullopt;
        }
    }

private:
    [[noreturn]] static void fail(const std::string& what) { throw std::invalid_argument(what); }
    const Token& peek() const { return tokens_[pos_]; }
    const Token& take() { return tokens_[pos_ < tokens_.size() - 1 ? pos_++ : pos_]; }
    bool accept_word(std::string_view word) {
        if (peek().kind == Token::Kind::word && peek().text == word) { ++pos_; return true; }
        return false;
    }
    bool accept_symbol(std::string_view symbol) {
        if (peek().kind == Token::Kind::symbol && peek().text == symbol) { ++pos_; return true; }
        return false;
    }
    void expect_symbol(std::string_view symbol) { if (!accept_symbol(symbol)) fail("expected \"" + std::string(symbol) + "\""); }
    std::string string_value() {
        const auto& token = take();
        if (token.kind != Token::Kind::string && token.kind != Token::Kind::word) fail("expected a string");
        if (token.kind == Token::Kind::word && (token.text == "and" || token.text == "or")) fail("expected a string before \"" + token.text + "\"");
        return token.text;
    }
    Bytes hash_value() {
        const auto& token = take();
        if (token.kind != Token::Kind::hash) fail("expected a hash (H\"...\")");
        Bytes value;
        if (token.text.size() % 2) fail("a hash with an odd number of digits");
        for (std::size_t index = 0; index < token.text.size(); index += 2) {
            const auto digit = [&](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                fail("a hash with a non-hex digit");
            };
            value.push_back(static_cast<std::uint8_t>(digit(token.text[index]) * 16 + digit(token.text[index + 1])));
        }
        return value;
    }
    Node binary(std::uint32_t op, Node left, Node right) {
        Node node;
        node.op = op;
        node.children = {std::move(left), std::move(right)};
        return node;
    }
    Node expression() {
        auto node = term();
        while (accept_word("or") || accept_symbol("||")) node = binary(op_or, std::move(node), term());
        return node;
    }
    Node term() {
        auto node = factor();
        while (accept_word("and") || accept_symbol("&&")) node = binary(op_and, std::move(node), factor());
        return node;
    }
    Node factor() {
        if (accept_symbol("!")) {
            Node node;
            node.op = op_not;
            node.children = {factor()};
            return node;
        }
        if (accept_symbol("(")) {
            auto node = expression();
            expect_symbol(")");
            return node;
        }
        return primary();
    }
    void match(Node& node) {
        if (accept_word("exists")) { node.match = match_exists; return; }
        if (accept_word("absent")) { node.match = match_absent; return; }
        const std::pair<std::string_view, std::uint32_t> relations[] = {
            {"<=", match_less_equal}, {">=", match_greater_equal}, {"<", match_less_than}, {">", match_greater_than}};
        for (const auto& [symbol, op] : relations)
            if (accept_symbol(symbol)) { node.match = op; node.value = string_value(); return; }
        // A field with no match is "exists" (as Apple writes it: "[field.OID] /* exists */").
        if (!accept_symbol("=")) { node.match = match_exists; return; }
        const bool leading = accept_symbol("*");
        node.value = string_value();
        const bool trailing = accept_symbol("*");
        node.match = leading && trailing ? match_contains : leading ? match_ends_with : trailing ? match_begins_with : match_equal;
    }
    std::int32_t slot() {
        if (accept_word("leaf")) return 0;
        if (accept_word("root") || accept_word("anchor")) return -1;
        const auto& token = take();
        if (token.kind != Token::Kind::word) fail("expected a certificate slot");
        try { return std::stoi(token.text); } catch (...) { fail("expected a certificate slot"); }
    }
    // After "certificate N" / "anchor": = H"..." | trusted | [field] match.
    Node certificate(std::int32_t index) {
        Node node;
        node.slot = index;
        if (accept_symbol("=")) { node.op = op_anchor_hash; node.data = hash_value(); return node; }
        if (accept_word("trusted")) { node.op = op_trusted_cert; return node; }
        expect_symbol("[");
        const auto field = string_value();
        expect_symbol("]");
        const auto dot = field.find('.');
        const auto kind = field.substr(0, dot), rest = dot == std::string::npos ? std::string{} : field.substr(dot + 1);
        if (kind == "subject" || kind == "issuer") {
            node.op = op_cert_field;
            node.text = field;
        } else if (kind == "field" || kind == "policy" || kind == "timestamp") {
            const auto oid = ber::oid_bytes(rest);
            if (!oid) fail("a malformed OID in [" + field + "]");
            node.op = kind == "field" ? op_cert_generic : kind == "policy" ? op_cert_policy : op_cert_field_date;
            node.data = *oid;
        } else {
            fail("an unknown certificate field [" + field + "]");
        }
        match(node);
        return node;
    }
    Node primary() {
        Node node;
        if (accept_word("always") || accept_word("true")) { node.op = op_true; return node; }
        if (accept_word("never") || accept_word("false")) { node.op = op_false; return node; }
        if (accept_word("identifier")) { accept_symbol("="); node.op = op_ident; node.text = string_value(); return node; }
        if (accept_word("cdhash")) { node.op = op_cdhash; node.data = hash_value(); return node; }
        if (accept_word("platform")) {
            expect_symbol("=");
            node.op = op_platform;
            try { node.integer = std::stoi(string_value()); } catch (...) { fail("expected a platform number"); }
            return node;
        }
        if (accept_word("notarized")) { node.op = op_notarized; return node; }
        if (accept_word("legacy")) { node.op = op_legacy_dev_id; return node; }
        if (accept_word("anchor")) {
            if (accept_word("apple")) { node.op = accept_word("generic") ? op_apple_generic_anchor : op_apple_anchor; return node; }
            if (accept_word("trusted")) { node.op = op_trusted_certs; return node; }
            if (peek().kind == Token::Kind::word) { node.op = op_named_anchor; node.text = take().text; return node; }
            return certificate(-1);
        }
        if (accept_word("certificate") || accept_word("cert")) return certificate(slot());
        for (const auto& [word, op] : {std::pair{"info", op_info_key_field}, std::pair{"entitlement", op_entitlement_field}}) {
            if (!accept_word(word)) continue;
            expect_symbol("[");
            node.op = op;
            node.text = string_value();
            expect_symbol("]");
            match(node);
            return node;
        }
        fail(peek().kind == Token::Kind::end ? "an incomplete requirement" : "unexpected \"" + peek().text + "\"");
    }
    std::vector<Token> tokens_;
    std::size_t pos_{};
};

// ---- evaluation ---------------------------------------------------------------------------------------------

std::optional<Plist> plist_file(const std::optional<std::filesystem::path>& path) {
    if (!path) return std::nullopt;
    std::ifstream in(*path, std::ios::binary);
    if (!in) return std::nullopt;
    const Bytes bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return export_fill::plist_parse(bytes);
}

std::optional<std::string> plist_text(const Plist& value) {
    switch (value.kind) {
    case Plist::Kind::string: return value.text;
    case Plist::Kind::integer: return std::to_string(value.integer);
    case Plist::Kind::boolean: return value.boolean ? "true" : "false";
    default: return std::nullopt;
    }
}

bool matches(const Node& node, const std::optional<std::string>& actual) {
    if (node.match == match_absent) return !actual;
    if (!actual) return false;
    const auto& value = node.value;
    switch (node.match) {
    case match_exists: return true;
    case match_equal: return *actual == value;
    case match_contains: return actual->find(value) != std::string::npos;
    case match_begins_with: return actual->starts_with(value);
    case match_ends_with: return actual->ends_with(value);
    case match_less_than: return *actual < value;
    case match_greater_than: return *actual > value;
    case match_less_equal: return *actual <= value;
    case match_greater_equal: return *actual >= value;
    default: return false;
    }
}

// (OID, text) pairs of a DER Name.
std::vector<std::pair<std::string, std::string>> name_entries(const Bytes& name) {
    std::vector<std::pair<std::string, std::string>> entries;
    const std::span<const std::uint8_t> data(name);
    const auto sequence = ber::at(data, 0);
    if (!sequence) return entries;
    for (const auto& set : ber::children(data, *sequence))
        for (const auto& pair : ber::children(data, set)) {
            const auto parts = ber::children(data, pair);
            if (parts.size() == 2) {
                const auto value = ber::contents(data, parts[1]);
                entries.emplace_back(ber::oid_text(data, parts[0]), std::string(value.begin(), value.end()));
            }
        }
    return entries;
}

// (OID, value) pairs of a certificate's extensions.
std::vector<std::pair<std::string, Bytes>> extensions_of(const Certificate& certificate) {
    std::vector<std::pair<std::string, Bytes>> extensions;
    const std::span<const std::uint8_t> data(certificate.der());
    const auto top = ber::at(data, 0);
    if (!top) return extensions;
    const auto parts = ber::children(data, *top);
    if (parts.empty()) return extensions;
    for (const auto& field : ber::children(data, parts[0])) {
        if (field.tag != 0xa3) continue;
        const auto inner = ber::children(data, field);
        if (inner.empty()) break;
        for (const auto& extension : ber::children(data, inner[0])) {
            const auto items = ber::children(data, extension);
            if (items.size() >= 2) extensions.emplace_back(ber::oid_text(data, items[0]), ber::contents(data, items.back()));
        }
    }
    return extensions;
}

std::string_view name_oid(std::string_view name) {
    static const std::pair<std::string_view, std::string_view> names[] = {
        {"CN", "2.5.4.3"}, {"C", "2.5.4.6"}, {"L", "2.5.4.7"}, {"ST", "2.5.4.8"}, {"STREET", "2.5.4.9"}, {"O", "2.5.4.10"},
        {"OU", "2.5.4.11"}, {"DESCRIPTION", "2.5.4.13"}, {"EMAIL", "1.2.840.113549.1.9.1"}, {"UID", "0.9.2342.19200300.100.1.1"}};
    for (const auto& [key, oid] : names) if (key == name) return oid;
    return {};
}

struct Context {
    const StaticCode& code;
    const Signature& signature;
    std::vector<CertificatePtr> certificates;   // leaf first
    std::optional<Plist> info, entitlements;

    CertificatePtr certificate(std::int32_t slot) const {
        const auto count = static_cast<std::int32_t>(certificates.size());
        const auto index = slot >= 0 ? slot : count + slot;
        return index >= 0 && index < count ? certificates[static_cast<std::size_t>(index)] : nullptr;
    }
    bool apple_rooted() const { return !certificates.empty() && is_apple_root(*certificates.back()); }

    bool evaluate(const Node& node) const {
        switch (node.op) {
        case op_false: return false;
        case op_true: return true;
        case op_ident: return signature.directories.front().identifier == node.text;
        case op_apple_anchor: return apple_rooted() && certificates.front()->common_name() == "Software Signing";
        case op_apple_generic_anchor: return apple_rooted();
        case op_anchor_hash: {
            const auto certificate = this->certificate(node.slot);
            const auto hash = certificate ? digest("SHA1", certificate->der()) : std::nullopt;
            return hash && *hash == node.data;
        }
        case op_info_key_value: {
            const auto* value = info ? info->find(node.text) : nullptr;
            return value && plist_text(*value) == node.value;
        }
        case op_and: return evaluate(node.children[0]) && evaluate(node.children[1]);
        case op_or: return evaluate(node.children[0]) || evaluate(node.children[1]);
        case op_not: return !evaluate(node.children[0]);
        case op_cdhash:
            return std::any_of(signature.directories.begin(), signature.directories.end(), [&](const CodeDirectory& cd) {
                const auto hash = cd.cdhash();
                return node.data.size() <= hash.size() && std::equal(node.data.begin(), node.data.end(), hash.begin());
            });
        case op_info_key_field: case op_entitlement_field: {
            const auto& source = node.op == op_info_key_field ? info : entitlements;
            const auto* value = source ? source->find(node.text) : nullptr;
            std::optional<std::string> text;
            if (value) text = plist_text(*value).value_or(std::string{});
            if (value && value->kind == Plist::Kind::boolean && node.match == match_exists) return value->boolean;
            return matches(node, text);
        }
        case op_cert_field: {
            const auto certificate = this->certificate(node.slot);
            if (!certificate) return node.match == match_absent;
            const auto dot = node.text.find('.');
            const bool issuer = node.text.starts_with("issuer");
            const auto oid = name_oid(node.text.substr(dot + 1));
            if (oid.empty()) return false;
            std::optional<std::string> found;
            for (const auto& [key, value] : name_entries(issuer ? certificate->issuer_sequence() : certificate->subject_sequence()))
                if (key == oid) {
                    if (matches(node, value)) return true;
                    found = value;
                }
            return node.match == match_absent && !found;
        }
        case op_cert_generic: case op_cert_policy: {
            const auto certificate = this->certificate(node.slot);
            if (!certificate) return node.match == match_absent;
            const auto oid = ber::oid_text(node.data);
            std::optional<std::string> found;
            for (const auto& [key, value] : extensions_of(*certificate)) {
                if (node.op == op_cert_generic && key == oid) found = std::string(value.begin(), value.end());
                if (node.op == op_cert_policy && key == "2.5.29.32") {
                    const std::span<const std::uint8_t> data(value);
                    if (const auto list = ber::at(data, 0))
                        for (const auto& policy : ber::children(data, *list)) {
                            const auto parts = ber::children(data, policy);
                            if (!parts.empty() && ber::oid_text(data, parts[0]) == oid) found = std::string{};
                        }
                }
            }
            return matches(node, found);
        }
        case op_platform: return signature.directories.front().platform == static_cast<std::uint8_t>(node.integer);
        // No user trust settings, no stapled-ticket checks, no named anchors or code, no certificate dates.
        default: return false;
        }
    }
};

} // namespace

std::optional<Bytes> compile(std::string_view text, std::string& error) {
    auto tokens = tokenize(text, error);
    if (!tokens) return std::nullopt;
    Parser parser(std::move(*tokens));
    const auto node = parser.parse(error);
    if (!node) return std::nullopt;
    return write_requirement(*node);
}

std::optional<std::string> requirement_text(std::span<const std::uint8_t> blob) {
    const auto node = read_requirement(blob);
    if (!node) return std::nullopt;
    return text_of(*node).first;
}

std::map<std::uint32_t, Bytes> requirement_set(std::span<const std::uint8_t> blob) {
    std::map<std::uint32_t, Bytes> set;
    Reader reader{blob};
    if (reader.u32() != magic_requirement_set) return set;
    const auto length = reader.u32();
    if (length > blob.size()) return set;
    const auto count = reader.u32();
    for (std::uint32_t index = 0; index < count && !reader.failed; ++index) {
        const auto type = reader.u32(), offset = reader.u32();
        if (offset + 8 > length) break;
        Reader inner{blob, offset};
        inner.u32();
        const auto size = inner.u32();
        if (offset + size > length) break;
        set[type] = Bytes(blob.begin() + offset, blob.begin() + offset + size);
    }
    return set;
}

std::string requirement_set_text(std::span<const std::uint8_t> blob) {
    static const std::map<std::uint32_t, std::string_view> names{
        {requirement_host, "host"}, {requirement_guest, "guest"}, {requirement_designated, "designated"},
        {requirement_library, "library"}, {requirement_plugin, "plugin"}};
    std::string text;
    for (const auto& [type, requirement] : requirement_set(blob)) {
        const auto name = names.contains(type) ? std::string(names.at(type)) : std::to_string(type);
        text += name + " => " + requirement_text(requirement).value_or("/* malformed */") + "\n";
    }
    return text;
}

std::int32_t evaluate(std::span<const std::uint8_t> blob, const StaticCode& code, std::string* detail) {
    const auto node = read_requirement(blob);
    if (!node) {
        if (detail) *detail = "a malformed code requirement";
        return cs::requirement_invalid;
    }
    if (!code.is_signed()) {
        if (detail) *detail = "code object is not signed at all";
        return cs::unsigned_code;
    }
    const auto& signature = *code.preferred().signature;
    Context context{code, signature, {}, plist_file(code.info_plist), std::nullopt};
    if (!(signature.directories.front().flags & flag_adhoc) && !signature.cms.empty())
        context.certificates = check_cms(signature).certificates;
    if (signature.entitlements.size() > 8)
        context.entitlements = export_fill::plist_parse(std::span(signature.entitlements).subspan(8));
    if (context.evaluate(*node)) return 0;
    if (detail) *detail = "code failed to satisfy specified code requirement(s)";
    return cs::requirement_failed;
}

std::optional<Bytes> designated_requirement(const StaticCode& code, bool* implicit) {
    if (!code.is_signed()) return std::nullopt;
    const auto& signature = *code.preferred().signature;
    if (implicit) *implicit = false;
    if (!signature.requirements.empty()) {
        const auto set = requirement_set(signature.requirements);
        if (const auto found = set.find(requirement_designated); found != set.end()) return found->second;
    }
    if (implicit) *implicit = true;
    const auto& primary = signature.directories.front();
    std::string text;
    if (primary.flags & flag_adhoc) {
        const auto hash = signature.best().cdhash();
        text = "cdhash H\"" + hex(hash) + "\"";
    } else {
        const auto certificates = check_cms(signature).certificates;
        if (certificates.empty()) return std::nullopt;
        const auto& leaf = *certificates.front();
        const auto ident = "identifier " + requirement_string(primary.identifier);
        const auto extensions = extensions_of(leaf);
        const auto has = [&](std::string_view oid) {
            return std::any_of(extensions.begin(), extensions.end(), [&](const auto& extension) { return extension.first == oid; });
        };
        if (is_apple_root(*certificates.back())) {
            std::string team;
            for (const auto& [key, value] : name_entries(leaf.subject_sequence())) if (key == "2.5.4.11") team = value;
            if (has("1.2.840.113635.100.6.1.13")) {
                text = "anchor apple generic and " + ident + " and (certificate leaf[field.1.2.840.113635.100.6.1.9] /* exists */ or "
                       "certificate 1[field.1.2.840.113635.100.6.2.6] /* exists */ and certificate leaf[field.1.2.840.113635.100.6.1.13] "
                       "/* exists */ and certificate leaf[subject.OU] = " + requirement_string(team) + ")";
            } else if (leaf.common_name() == "Software Signing") {
                text = ident + " and anchor apple";
            } else {
                text = ident + " and anchor apple generic and certificate leaf[subject.CN] = " + requirement_string(leaf.common_name()) +
                       " and certificate 1[field.1.2.840.113635.100.6.2.1] /* exists */";
            }
        } else {
            const auto hash = digest("SHA1", certificates.back()->der());
            if (!hash) return std::nullopt;
            text = ident + " and certificate root = H\"" + hex(*hash) + "\"";
        }
    }
    std::string error;
    return compile(text, error);
}

} // namespace alchemy::gui::security::codesign
