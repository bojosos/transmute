#include "preprocess.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <deque>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <unordered_map>

// In-process MSL preprocessor (owner: A6). See preprocess.hpp. Macro
// expansion follows Prosser's algorithm (hide sets), the model of the C
// standard's rescanning rules that clang implements.
namespace alchemy::msl::detail {
namespace {

struct Decline : std::runtime_error {
    using std::runtime_error::runtime_error;
};
[[noreturn]] void decline(const std::string& why) { throw Decline(why); }

// ---- hide sets ------------------------------------------------------------------------
using HideSet = std::shared_ptr<const std::vector<std::string>>;   // sorted, unique

bool hs_contains(const HideSet& h, const std::string& name) {
    return h && std::binary_search(h->begin(), h->end(), name);
}
HideSet hs_union(const HideSet& a, const HideSet& b) {
    if (!a || a->empty()) return b;
    if (!b || b->empty()) return a;
    auto merged = std::make_shared<std::vector<std::string>>();
    std::set_union(a->begin(), a->end(), b->begin(), b->end(), std::back_inserter(*merged));
    return merged;
}
HideSet hs_intersect(const HideSet& a, const HideSet& b) {
    if (!a || !b) return nullptr;
    auto common = std::make_shared<std::vector<std::string>>();
    std::set_intersection(a->begin(), a->end(), b->begin(), b->end(), std::back_inserter(*common));
    return common;
}
HideSet hs_add(const HideSet& h, const std::string& name) {
    return hs_union(h, std::make_shared<const std::vector<std::string>>(std::vector<std::string>{name}));
}

// ---- tokens ---------------------------------------------------------------------------
struct Tok {
    enum class K : std::uint8_t { ident, number, string, character, punct, other, newline, placemarker };
    K kind{K::other};
    std::string text;
    bool space{};          // whitespace before it
    bool bol{};            // first token of a logical source line
    bool painted{};        // found in its own hide set: never expands again
    bool from_macro{};
    std::uint32_t line{};  // physical line where its logical line starts
    std::uint32_t newlines{};   // newline tokens: physical lines they end
    HideSet hide;
    bool is(std::string_view t) const { return (kind == K::punct || kind == K::ident) && text == t; }
};

bool ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$'; }
bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$'; }

// Lexes preprocessing tokens. Line splices are removed first; a newline
// token carries the number of physical lines its logical line spans
// (splices and newlines inside block comments included).
class Lexer {
public:
    explicit Lexer(std::string_view source) {
        text_.reserve(source.size());
        for (std::size_t i = 0; i < source.size(); ++i) {
            const char c = source[i];
            if (c == '\r' && i + 1 < source.size() && source[i + 1] == '\n') continue;
            if (c == '\\') {
                std::size_t j = i + 1;
                if (j < source.size() && source[j] == '\r') ++j;
                if (j < source.size() && source[j] == '\n') {
                    splices_.push_back(text_.size());
                    i = j;
                    continue;
                }
            }
            text_ += c;
        }
    }

    std::vector<Tok> run() {
        std::vector<Tok> tokens;
        std::size_t pos = 0, line_begin = 0;
        std::uint32_t line = 1, comment_lines = 0;
        bool bol = true, space = false;
        while (pos < text_.size()) {
            const char c = text_[pos];
            if (c == '\n') {
                Tok t;
                t.kind = Tok::K::newline;
                t.text = "\n";
                t.line = line;
                t.newlines = 1 + comment_lines + splices_between(line_begin, pos + 1);
                tokens.push_back(std::move(t));
                line += tokens.back().newlines;
                comment_lines = 0;
                line_begin = pos + 1;
                ++pos;
                bol = true;
                space = false;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
                space = true;
                ++pos;
                continue;
            }
            if (c == '/' && pos + 1 < text_.size() && text_[pos + 1] == '/') {
                while (pos < text_.size() && text_[pos] != '\n') ++pos;
                space = true;
                continue;
            }
            if (c == '/' && pos + 1 < text_.size() && text_[pos + 1] == '*') {
                const auto end = text_.find("*/", pos + 2);
                if (end == std::string::npos) decline("unterminated block comment");
                comment_lines += static_cast<std::uint32_t>(std::count(text_.begin() + static_cast<std::ptrdiff_t>(pos),
                                                                       text_.begin() + static_cast<std::ptrdiff_t>(end), '\n'));
                pos = end + 2;
                space = true;
                continue;
            }
            Tok t;
            t.line = line;
            t.bol = bol;
            t.space = space;
            pos = lex_token(pos, t);
            tokens.push_back(std::move(t));
            bol = false;
            space = false;
        }
        // A final line without a newline still ends.
        Tok end;
        end.kind = Tok::K::newline;
        end.text = "\n";
        end.line = line;
        end.newlines = 0;
        tokens.push_back(end);
        return tokens;
    }

    // One token from `text` (token pasting); nullopt unless it is exactly one.
    static std::optional<Tok> single(const std::string& text) {
        Lexer lexer(text);
        if (lexer.text_.empty() || lexer.text_.size() != text.size()) return std::nullopt;
        const char c = lexer.text_[0];
        if (c == ' ' || c == '\t' || c == '\n' || (c == '/' && text.size() > 1 && (text[1] == '/' || text[1] == '*')))
            return std::nullopt;
        Tok t;
        std::size_t end = 0;
        try {
            end = lexer.lex_token(0, t);
        } catch (const Decline&) {
            return std::nullopt;
        }
        if (end != lexer.text_.size()) return std::nullopt;
        return t;
    }

private:
    std::string text_;
    std::vector<std::size_t> splices_;   // positions in text_ where a splice was removed

    std::uint32_t splices_between(std::size_t begin, std::size_t end) const {
        const auto a = std::lower_bound(splices_.begin(), splices_.end(), begin);
        const auto b = std::lower_bound(splices_.begin(), splices_.end(), end);
        return static_cast<std::uint32_t>(b - a);
    }

    std::size_t lex_token(std::size_t pos, Tok& t) const {
        const auto& s = text_;
        const char c = s[pos];
        // String and character literals, with encoding prefixes.
        std::size_t quote = std::string::npos;
        if (c == '"' || c == '\'') quote = pos;
        else if ((c == 'L' || c == 'U') && pos + 1 < s.size() && (s[pos + 1] == '"' || s[pos + 1] == '\'')) quote = pos + 1;
        else if (c == 'u' && pos + 1 < s.size() && (s[pos + 1] == '"' || s[pos + 1] == '\'')) quote = pos + 1;
        else if (c == 'u' && pos + 2 < s.size() && s[pos + 1] == '8' && (s[pos + 2] == '"' || s[pos + 2] == '\'')) quote = pos + 2;
        if (quote == std::string::npos && ident_start(c)) {
            // Raw string literals: R"..." and prefixed forms.
            std::size_t k = pos;
            while (k < s.size() && ident_char(s[k])) ++k;
            const auto word = std::string_view(s).substr(pos, k - pos);
            if (k < s.size() && s[k] == '"' && (word == "R" || word == "LR" || word == "uR" || word == "UR" || word == "u8R"))
                decline("raw string literal");
            t.kind = Tok::K::ident;
            t.text = std::string(word);
            return k;
        }
        if (quote != std::string::npos) {
            const char q = s[quote];
            std::size_t k = quote + 1;
            while (k < s.size() && s[k] != q && s[k] != '\n') {
                if (s[k] == '\\' && k + 1 < s.size() && s[k + 1] != '\n') ++k;
                ++k;
            }
            if (k >= s.size() || s[k] != q) {
                // Unterminated: an "other" token to the end of the line (clang
                // tolerates it in skipped blocks; in active text it is an error).
                t.kind = Tok::K::other;
                t.text = s.substr(pos, k - pos);
                return k;
            }
            ++k;
            // User-defined literal suffix.
            while (k < s.size() && ident_char(s[k])) ++k;
            t.kind = q == '"' ? Tok::K::string : Tok::K::character;
            t.text = s.substr(pos, k - pos);
            return k;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && pos + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[pos + 1])))) {
            std::size_t k = pos + 1;
            while (k < s.size()) {
                const char d = s[k];
                if ((d == '+' || d == '-') && (s[k - 1] == 'e' || s[k - 1] == 'E' || s[k - 1] == 'p' || s[k - 1] == 'P')) {
                    ++k;
                    continue;
                }
                if (d == '\'' && k + 1 < s.size() && ident_char(s[k + 1])) {
                    k += 2;
                    continue;
                }
                if (!ident_char(d) && d != '.') break;
                ++k;
            }
            t.kind = Tok::K::number;
            t.text = s.substr(pos, k - pos);
            return k;
        }
        if ((c == '<' && pos + 1 < s.size() && (s[pos + 1] == '%' || (s[pos + 1] == ':' && !(pos + 2 < s.size() && s[pos + 2] == ':')))) ||
            (c == '%' && pos + 1 < s.size() && (s[pos + 1] == ':' || s[pos + 1] == '>')) ||
            (c == ':' && pos + 1 < s.size() && s[pos + 1] == '>'))
            decline("digraph");
        static const char* const three[] = {"<<=", ">>=", "...", "->*"};
        static const char* const two[] = {"::", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "+=",
            "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##", ".*"};
        for (const auto* p : three)
            if (s.compare(pos, 3, p) == 0) {
                t.kind = Tok::K::punct;
                t.text = p;
                return pos + 3;
            }
        for (const auto* p : two)
            if (s.compare(pos, 2, p) == 0) {
                t.kind = Tok::K::punct;
                t.text = p;
                return pos + 2;
            }
        t.kind = static_cast<unsigned char>(c) < 0x80 && std::ispunct(static_cast<unsigned char>(c)) ? Tok::K::punct
                                                                                                    : Tok::K::other;
        t.text = std::string(1, c);
        return pos + 1;
    }
};

// ---- macros ---------------------------------------------------------------------------
struct Macro {
    bool function{};
    bool variadic{};
    std::vector<std::string> params;   // __VA_ARGS__ last when variadic
    std::vector<Tok> body;
};

bool same_definition(const Macro& a, const Macro& b) {
    if (a.function != b.function || a.variadic != b.variadic || a.params != b.params || a.body.size() != b.body.size())
        return false;
    for (std::size_t i = 0; i < a.body.size(); ++i)
        if (a.body[i].text != b.body[i].text || (i && a.body[i].space != b.body[i].space)) return false;
    return true;
}

// Parses `NAME[(params)] body` (the tokens after #define).
std::pair<std::string, Macro> parse_define(const std::vector<Tok>& line) {
    if (line.empty() || line[0].kind != Tok::K::ident) decline("malformed #define");
    const auto name = line[0].text;
    if (name == "defined" || name == "__VA_ARGS__" || name == "__VA_OPT__") decline("#define of a reserved name");
    Macro macro;
    std::size_t i = 1;
    if (i < line.size() && line[i].is("(") && !line[i].space) {
        macro.function = true;
        ++i;
        bool expect_param = true;
        while (true) {
            if (i >= line.size()) decline("malformed macro parameter list");
            const auto& t = line[i];
            if (t.is(")") && (!expect_param || macro.params.empty())) {
                ++i;
                break;
            }
            if (expect_param && t.is("...")) {
                macro.variadic = true;
                macro.params.push_back("__VA_ARGS__");
                ++i;
                if (i >= line.size() || !line[i].is(")")) decline("malformed variadic macro");
                ++i;
                break;
            }
            if (expect_param && t.kind == Tok::K::ident) {
                if (i + 1 < line.size() && line[i + 1].is("...")) decline("named variadic macro parameter");
                if (std::find(macro.params.begin(), macro.params.end(), t.text) != macro.params.end())
                    decline("duplicate macro parameter");
                macro.params.push_back(t.text);
                expect_param = false;
                ++i;
                continue;
            }
            if (!expect_param && t.is(",")) {
                expect_param = true;
                ++i;
                continue;
            }
            decline("malformed macro parameter list");
        }
    }
    for (; i < line.size(); ++i) macro.body.push_back(line[i]);
    if (!macro.body.empty()) macro.body.front().space = false;
    for (auto& t : macro.body) {
        t.bol = false;
        if (t.kind == Tok::K::ident && (t.text == "__VA_OPT__")) decline("__VA_OPT__");
    }
    if (!macro.body.empty() && (macro.body.front().is("##") || macro.body.back().is("##")))
        decline("'##' at the edge of a macro body");
    if (macro.function) {
        for (std::size_t k = 0; k < macro.body.size(); ++k) {
            if (!macro.body[k].is("#")) continue;
            const bool param = k + 1 < macro.body.size() && macro.body[k + 1].kind == Tok::K::ident &&
                std::find(macro.params.begin(), macro.params.end(), macro.body[k + 1].text) != macro.params.end();
            if (!param) decline("'#' not followed by a macro parameter");
        }
    }
    return {name, std::move(macro)};
}

bool is_unsupported_builtin(const std::string& name) {
    static const char* const names[] = {"__has_include", "__has_include_next", "__has_feature", "__has_extension",
        "__has_attribute", "__has_builtin", "__has_cpp_attribute", "__has_c_attribute", "__has_declspec_attribute",
        "__has_warning", "__has_constexpr_builtin", "__has_embed", "__is_identifier", "__is_target_arch",
        "__is_target_vendor", "__is_target_os", "__is_target_environment", "__is_target_variant_os",
        "__is_target_variant_environment", "__building_module", "__FILE_NAME__", "__BASE_FILE__", "__TIMESTAMP__",
        "__DATE__", "__TIME__", "__COUNTER__", "__INCLUDE_LEVEL__", "_Pragma", "__VA_OPT__", "__pragma"};
    for (const auto* n : names)
        if (name == n) return true;
    return false;
}

} // namespace

class MacroSet {
public:
    std::vector<std::pair<std::string, Macro>> macros;
};

std::size_t macro_count(const MacroSet& set) { return set.macros.size(); }

std::shared_ptr<const MacroSet> parse_macro_set(std::string_view defines) {
    auto set = std::make_shared<MacroSet>();
    try {
        const auto tokens = Lexer(defines).run();
        std::vector<Tok> line;
        for (const auto& t : tokens) {
            if (t.kind != Tok::K::newline) {
                line.push_back(t);
                continue;
            }
            if (line.size() >= 2 && line[0].is("#") && line[1].is("define")) {
                std::vector<Tok> rest(line.begin() + 2, line.end());
                set->macros.push_back(parse_define(rest));
            }
            line.clear();
        }
    } catch (const Decline&) {
        return nullptr;
    }
    return set;
}

namespace {

// ---- #if expressions ------------------------------------------------------------------
struct Value {
    std::uint64_t bits{};
    bool is_unsigned{};
    bool truth() const { return bits != 0; }
};

class Expression {
public:
    explicit Expression(const std::vector<Tok>& tokens) : t_(tokens) {}
    Value run() {
        const auto v = conditional(true);
        if (p_ != t_.size()) decline("unexpected token in #if: " + t_[p_].text);
        return v;
    }

private:
    const std::vector<Tok>& t_;
    std::size_t p_{};

    bool at(std::string_view s) const { return p_ < t_.size() && t_[p_].kind == Tok::K::punct && t_[p_].text == s; }
    static Value arith(Value a, Value b) {
        const bool u = a.is_unsigned || b.is_unsigned;
        return {0, u};
    }
    Value conditional(bool live) {
        auto c = logical_or(live);
        if (!at("?")) return c;
        ++p_;
        auto a = conditional(live && c.truth());
        if (!at(":")) decline("malformed ?: in #if");
        ++p_;
        auto b = conditional(live && !c.truth());
        const bool u = a.is_unsigned || b.is_unsigned;
        auto r = c.truth() ? a : b;
        r.is_unsigned = u;
        return r;
    }
    Value logical_or(bool live) {
        auto a = logical_and(live);
        while (at("||")) {
            ++p_;
            auto b = logical_and(live && !a.truth());
            a = {a.truth() || b.truth() ? 1U : 0U, false};
        }
        return a;
    }
    Value logical_and(bool live) {
        auto a = bit_or(live);
        while (at("&&")) {
            ++p_;
            auto b = bit_or(live && a.truth());
            a = {a.truth() && b.truth() ? 1U : 0U, false};
        }
        return a;
    }
    Value bit_or(bool live) {
        auto a = bit_xor(live);
        while (at("|")) { ++p_; auto b = bit_xor(live); auto r = arith(a, b); r.bits = a.bits | b.bits; a = r; }
        return a;
    }
    Value bit_xor(bool live) {
        auto a = bit_and(live);
        while (at("^")) { ++p_; auto b = bit_and(live); auto r = arith(a, b); r.bits = a.bits ^ b.bits; a = r; }
        return a;
    }
    Value bit_and(bool live) {
        auto a = equality(live);
        while (at("&")) { ++p_; auto b = equality(live); auto r = arith(a, b); r.bits = a.bits & b.bits; a = r; }
        return a;
    }
    Value equality(bool live) {
        auto a = relational(live);
        while (at("==") || at("!=")) {
            const bool eq = at("==");
            ++p_;
            auto b = relational(live);
            a = {(a.bits == b.bits) == eq ? 1U : 0U, false};
        }
        return a;
    }
    static bool less(Value a, Value b) {
        if (a.is_unsigned || b.is_unsigned) return a.bits < b.bits;
        return static_cast<std::int64_t>(a.bits) < static_cast<std::int64_t>(b.bits);
    }
    Value relational(bool live) {
        auto a = shift(live);
        while (at("<") || at(">") || at("<=") || at(">=")) {
            const auto op = t_[p_].text;
            ++p_;
            auto b = shift(live);
            bool r = false;
            if (op == "<") r = less(a, b);
            else if (op == ">") r = less(b, a);
            else if (op == "<=") r = !less(b, a);
            else r = !less(a, b);
            a = {r ? 1U : 0U, false};
        }
        return a;
    }
    Value shift(bool live) {
        auto a = additive(live);
        while (at("<<") || at(">>")) {
            const bool left = at("<<");
            ++p_;
            auto b = additive(live);
            const auto count = b.bits;
            if (live && (count >= 64 || (!b.is_unsigned && static_cast<std::int64_t>(count) < 0)))
                decline("shift count out of range in #if");
            Value r{0, a.is_unsigned};
            if (count < 64) {
                if (left) r.bits = a.bits << count;
                else if (a.is_unsigned) r.bits = a.bits >> count;
                else r.bits = static_cast<std::uint64_t>(static_cast<std::int64_t>(a.bits) >> count);
            }
            a = r;
        }
        return a;
    }
    Value additive(bool live) {
        auto a = multiplicative(live);
        while (at("+") || at("-")) {
            const bool plus = at("+");
            ++p_;
            auto b = multiplicative(live);
            auto r = arith(a, b);
            r.bits = plus ? a.bits + b.bits : a.bits - b.bits;
            a = r;
        }
        return a;
    }
    Value multiplicative(bool live) {
        auto a = unary(live);
        while (at("*") || at("/") || at("%")) {
            const auto op = t_[p_].text;
            ++p_;
            auto b = unary(live);
            auto r = arith(a, b);
            if (op == "*") r.bits = a.bits * b.bits;
            else if (b.bits == 0) {
                if (live) decline("division by zero in #if");
                r.bits = 0;
            } else if (r.is_unsigned) r.bits = op == "/" ? a.bits / b.bits : a.bits % b.bits;
            else {
                const auto x = static_cast<std::int64_t>(a.bits), y = static_cast<std::int64_t>(b.bits);
                if (x == INT64_MIN && y == -1) decline("overflow in #if");
                r.bits = static_cast<std::uint64_t>(op == "/" ? x / y : x % y);
            }
            a = r;
        }
        return a;
    }
    Value unary(bool live) {
        if (at("+")) { ++p_; return unary(live); }
        if (at("-")) { ++p_; auto v = unary(live); v.bits = 0 - v.bits; return v; }
        if (at("~")) { ++p_; auto v = unary(live); v.bits = ~v.bits; return v; }
        if (at("!")) { ++p_; auto v = unary(live); return {v.truth() ? 0U : 1U, false}; }
        return primary(live);
    }
    Value primary(bool live) {
        if (p_ >= t_.size()) decline("truncated #if expression");
        if (at("(")) {
            ++p_;
            auto v = conditional(live);
            if (!at(")")) decline("missing ')' in #if");
            ++p_;
            return v;
        }
        const auto& t = t_[p_++];
        if (t.kind == Tok::K::number) return number(t.text);
        if (t.kind == Tok::K::character) return character(t.text);
        decline("unexpected token in #if: " + t.text);
    }
    static Value number(std::string text) {
        text.erase(std::remove(text.begin(), text.end(), '\''), text.end());
        bool is_unsigned = false;
        std::size_t end = text.size();
        while (end > 0) {
            const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(text[end - 1])));
            if (c == 'u') is_unsigned = true;
            else if (c != 'l') break;
            --end;
        }
        const auto digits = text.substr(0, end);
        int base = 10;
        std::size_t start = 0;
        if (digits.size() > 1 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) { base = 16; start = 2; }
        else if (digits.size() > 1 && digits[0] == '0' && (digits[1] == 'b' || digits[1] == 'B')) { base = 2; start = 2; }
        else if (digits.size() > 1 && digits[0] == '0') { base = 8; start = 1; }
        if (start >= digits.size() && base != 8) decline("malformed number in #if");
        std::uint64_t value = 0;
        for (std::size_t i = start; i < digits.size(); ++i) {
            const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(digits[i])));
            int d = -1;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            if (d < 0 || d >= base) decline("non-integer number in #if: " + text);
            if (value > (UINT64_MAX - static_cast<std::uint64_t>(d)) / static_cast<std::uint64_t>(base))
                decline("integer overflow in #if");
            value = value * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(d);
        }
        if (value > static_cast<std::uint64_t>(INT64_MAX)) is_unsigned = true;
        return {value, is_unsigned};
    }
    static Value character(const std::string& text) {
        if (text.size() == 3 && text[0] == '\'' && text[2] == '\'' && text[1] != '\\')
            return {static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<signed char>(text[1]))), false};
        if (text.size() == 4 && text[0] == '\'' && text[1] == '\\' && text[3] == '\'') {
            switch (text[2]) {
            case 'n': return {'\n', false};
            case 't': return {'\t', false};
            case 'r': return {'\r', false};
            case '0': return {0, false};
            case '\\': return {'\\', false};
            case '\'': return {'\'', false};
            case '"': return {'"', false};
            default: break;
            }
        }
        decline("character literal in #if: " + text);
    }
};

// ---- the preprocessor -------------------------------------------------------------------
bool stdlib_header(const std::string& name) {
    if (name == "metal_stdlib" || name == "simd/simd.h" || name == "simd/vector_types.h" || name == "simd/matrix_types.h")
        return true;
    static const char* const names[] = {"metal_math", "metal_common", "metal_geometric", "metal_integer",
        "metal_relational", "metal_matrix", "metal_pack", "metal_texture", "metal_atomic", "metal_compute",
        "metal_graphics", "metal_simdgroup", "metal_quadgroup", "metal_types", "metal_numeric", "metal_limits",
        "metal_type_traits", "metal_array", "metal_utility", "metal_functional", "metal_initializer_list",
        "metal_tensor", "metal_simdgroup_matrix", "metal_uniform"};
    for (const auto* n : names)
        if (name == n) return true;
    return false;
}

class Preprocessor {
public:
    Preprocessor(std::string_view source, const MacroSet& builtins, const MacroSet& stdlib,
                 const std::vector<std::pair<std::string, std::string>>& user)
        : src_(Lexer(source).run()), stdlib_(stdlib) {
        for (const auto& [name, macro] : builtins.macros) macros_[name] = macro;
        for (const auto& [name, value] : user) {
            const auto line = Lexer(name + " " + value).run();
            std::vector<Tok> tokens(line.begin(), line.end() - 1);
            auto [parsed_name, macro] = parse_define(tokens);
            macros_[parsed_name] = std::move(macro);
        }
    }

    std::string run() {
        out_ = "# 1 \"<stdin>\"\n";
        line_empty_ = true;
        while (true) {
            if (pending_.empty()) {
                if (i_ >= src_.size()) break;
                const auto& t = src_[i_];
                if (t.bol && t.is("#")) {
                    directive();
                    continue;
                }
                if (!active()) {
                    while (i_ < src_.size() && src_[i_].kind != Tok::K::newline) ++i_;
                    if (i_ < src_.size()) newline(src_[i_++].newlines);
                    continue;
                }
                pending_.push_back(src_[i_++]);
            }
            auto t = std::move(pending_.front());
            pending_.pop_front();
            if (t.kind == Tok::K::newline) {
                newline(t.newlines + deferred_newlines_);
                deferred_newlines_ = 0;
                continue;
            }
            if (t.kind == Tok::K::other && (t.text.front() == '"' || t.text.front() == '\''))
                decline("unterminated literal");
            if (t.kind == Tok::K::ident && expand(t, pending_, true)) continue;
            emit(t);
        }
        if (!conditions_.empty()) decline("unterminated #if");
        return std::move(out_);
    }

private:
    std::vector<Tok> src_;
    std::size_t i_{};
    std::deque<Tok> pending_;
    std::unordered_map<std::string, Macro> macros_;
    const MacroSet& stdlib_;
    bool stdlib_loaded_{};
    std::string out_;
    bool line_empty_{true};
    bool last_from_macro_{};
    Tok::K last_kind_{Tok::K::other};
    std::string file_ = "<stdin>";
    std::int64_t line_offset_{};   // presumed line = physical + offset
    std::uint32_t deferred_newlines_{};   // lines a multi-line macro invocation consumed

    struct Condition {
        bool parent_active{};
        bool taken{};      // some branch already taken
        bool active{};
        bool seen_else{};
    };
    std::vector<Condition> conditions_;

    bool active() const { return conditions_.empty() || conditions_.back().active; }

    // ---- output ---------------------------------------------------------------------
    void emit(const Tok& t) {
        if (!line_empty_) {
            const bool word = [](Tok::K k) { return k == Tok::K::ident || k == Tok::K::number; }(t.kind);
            const bool last_word = last_kind_ == Tok::K::ident || last_kind_ == Tok::K::number;
            if (t.space || t.from_macro || last_from_macro_ || (word && last_word) ||
                (t.kind == Tok::K::punct && last_kind_ == Tok::K::punct))
                out_ += ' ';
        }
        out_ += t.text;
        line_empty_ = false;
        last_from_macro_ = t.from_macro;
        last_kind_ = t.kind;
    }
    void newline(std::uint32_t count) {
        out_.append(count, '\n');
        if (count) line_empty_ = true;
    }

    // ---- directives -------------------------------------------------------------------
    // The current directive's tokens (after '#') and its newline count.
    std::vector<Tok> directive_line(std::uint32_t& newlines) {
        std::vector<Tok> line;
        ++i_;   // '#'
        while (i_ < src_.size() && src_[i_].kind != Tok::K::newline) line.push_back(src_[i_++]);
        newlines = i_ < src_.size() ? src_[i_++].newlines : 0;
        return line;
    }

    void directive() {
        const auto directive_line_number = src_[i_].line;
        std::uint32_t newlines = 0;
        auto line = directive_line(newlines);
        if (line.empty()) {   // null directive
            newline(newlines);
            return;
        }
        const auto name = line[0].kind == Tok::K::ident ? line[0].text : std::string{};
        std::vector<Tok> rest(line.begin() + 1, line.end());
        if (name == "if" || name == "ifdef" || name == "ifndef") {
            Condition c;
            c.parent_active = active();
            if (c.parent_active) {
                bool value;
                if (name == "if") value = evaluate(rest);
                else {
                    if (rest.empty() || rest[0].kind != Tok::K::ident) decline("#" + name + " without a name");
                    value = defined(rest[0].text) == (name == "ifdef");
                }
                c.active = value;
                c.taken = value;
            }
            conditions_.push_back(c);
        } else if (name == "elif") {
            if (conditions_.empty() || conditions_.back().seen_else) decline("#elif without #if");
            auto& c = conditions_.back();
            if (c.parent_active && !c.taken) {
                c.active = evaluate(rest);
                c.taken = c.active;
            } else {
                c.active = false;
            }
        } else if (name == "else") {
            if (conditions_.empty() || conditions_.back().seen_else) decline("#else without #if");
            auto& c = conditions_.back();
            c.seen_else = true;
            c.active = c.parent_active && !c.taken;
            c.taken = true;
        } else if (name == "endif") {
            if (conditions_.empty()) decline("#endif without #if");
            conditions_.pop_back();
        } else if (!active()) {
            // Other directives in skipped blocks are ignored.
        } else if (name == "define") {
            auto [macro_name, macro] = parse_define(rest);
            if (is_unsupported_builtin(macro_name) || macro_name == "__LINE__" || macro_name == "__FILE__")
                decline("#define of a builtin macro");
            macros_[macro_name] = std::move(macro);
        } else if (name == "undef") {
            if (rest.empty() || rest[0].kind != Tok::K::ident) decline("malformed #undef");
            macros_.erase(rest[0].text);
        } else if (name == "line") {
            line_directive(rest, directive_line_number + newlines);
            return;   // the marker replaces the directive's line
        } else if (name == "pragma") {
            std::string text = "#pragma";
            for (const auto& t : rest) text += " " + t.text;
            if (!line_empty_) out_ += '\n';
            out_ += text;
            line_empty_ = false;
            last_kind_ = Tok::K::other;
        } else if (name == "include") {
            include(rest);
        } else {
            decline("#" + (name.empty() ? line[0].text : name) + " directive");
        }
        newline(newlines);
    }

    bool defined(const std::string& name) const {
        return macros_.count(name) != 0 || name == "__LINE__" || name == "__FILE__" || is_unsupported_builtin(name);
    }

    void include(const std::vector<Tok>& rest) {
        std::string header;
        if (rest.size() == 1 && rest[0].kind == Tok::K::string && rest[0].text.size() >= 2 && rest[0].text[0] == '"')
            header = rest[0].text.substr(1, rest[0].text.size() - 2);
        else if (rest.size() >= 3 && rest.front().is("<") && rest.back().is(">")) {
            for (std::size_t k = 1; k + 1 < rest.size(); ++k) header += rest[k].text;
        } else {
            decline("#include form");
        }
        if (!stdlib_header(header)) decline("#include <" + header + ">");
        if (stdlib_loaded_) return;
        stdlib_loaded_ = true;
        for (const auto& [name, macro] : stdlib_.macros) macros_[name] = macro;
    }

    void line_directive(std::vector<Tok> rest, std::uint32_t next_physical) {
        std::deque<Tok> q(rest.begin(), rest.end());
        const auto expanded = expand_all(q);
        if (expanded.empty() || expanded[0].kind != Tok::K::number) decline("malformed #line");
        for (const char c : expanded[0].text)
            if (!std::isdigit(static_cast<unsigned char>(c))) decline("malformed #line");
        const auto number = std::stoll(expanded[0].text);
        if (expanded.size() >= 2) {
            if (expanded[1].kind != Tok::K::string || expanded[1].text.front() != '"' || expanded.size() > 2)
                decline("malformed #line");
            file_ = expanded[1].text.substr(1, expanded[1].text.size() - 2);
        }
        line_offset_ = number - static_cast<std::int64_t>(next_physical);
        if (!line_empty_) out_ += '\n';
        out_ += "# " + std::to_string(number) + " \"" + file_ + "\"\n";
        line_empty_ = true;
    }

    bool evaluate(const std::vector<Tok>& rest) {
        // `defined X` / `defined(X)` first, then macro expansion.
        std::vector<Tok> tokens;
        for (std::size_t k = 0; k < rest.size(); ++k) {
            if (rest[k].kind == Tok::K::ident && rest[k].text == "defined") {
                std::string name;
                if (k + 1 < rest.size() && rest[k + 1].kind == Tok::K::ident) {
                    name = rest[k + 1].text;
                    k += 1;
                } else if (k + 3 < rest.size() && rest[k + 1].is("(") && rest[k + 2].kind == Tok::K::ident &&
                           rest[k + 3].is(")")) {
                    name = rest[k + 2].text;
                    k += 3;
                } else {
                    decline("malformed defined()");
                }
                Tok value;
                value.kind = Tok::K::number;
                value.text = defined(name) ? "1" : "0";
                tokens.push_back(value);
                continue;
            }
            tokens.push_back(rest[k]);
        }
        std::deque<Tok> q(tokens.begin(), tokens.end());
        auto expanded = expand_all(q);
        for (auto& t : expanded) {
            if (t.kind != Tok::K::ident) continue;
            if (t.text == "defined" || is_unsupported_builtin(t.text) || t.text.rfind("__has_", 0) == 0 ||
                t.text.rfind("__is_", 0) == 0)
                decline("#if uses '" + t.text + "'");
            t.kind = Tok::K::number;
            t.text = t.text == "true" ? "1" : "0";
        }
        if (expanded.empty()) decline("empty #if");
        return Expression(expanded).run().truth();
    }

    // ---- macro expansion ------------------------------------------------------------
    std::vector<Tok> expand_all(std::deque<Tok>& q) {
        std::vector<Tok> out;
        while (!q.empty()) {
            auto t = std::move(q.front());
            q.pop_front();
            if (t.kind == Tok::K::ident && expand(t, q, false)) continue;
            if (t.kind != Tok::K::newline) out.push_back(std::move(t));
        }
        return out;
    }

    // Next token for argument collection: the queue, then (top level) the source.
    bool next_raw(std::deque<Tok>& q, bool use_source, Tok& t, std::uint32_t& skipped_newlines) {
        while (true) {
            if (!q.empty()) {
                t = std::move(q.front());
                q.pop_front();
            } else if (use_source && i_ < src_.size()) {
                if (src_[i_].bol && src_[i_].is("#")) decline("directive inside macro arguments");
                t = src_[i_++];
            } else {
                return false;
            }
            if (t.kind == Tok::K::newline) {
                skipped_newlines += t.newlines;
                continue;
            }
            return true;
        }
    }

    // True when the next significant token is '(' (function-like invocation).
    bool next_is_paren(const std::deque<Tok>& q, bool use_source) const {
        for (const auto& t : q) {
            if (t.kind == Tok::K::newline) continue;
            return t.is("(");
        }
        if (!use_source) return false;
        for (auto j = i_; j < src_.size(); ++j) {
            const auto& t = src_[j];
            if (t.kind == Tok::K::newline) continue;
            if (t.bol && t.is("#")) return false;
            return t.is("(");
        }
        return false;
    }

    // Expands `t` if it names a macro: the replacement goes to the front of
    // `q` for rescanning. False when `t` is not expanded (emit it).
    bool expand(Tok& t, std::deque<Tok>& q, bool top) {
        if (t.painted) return false;
        if (t.text == "__LINE__" || t.text == "__FILE__") {
            if (macros_.count(t.text)) decline("redefined " + t.text);
            Tok r = t;
            r.from_macro = true;
            if (t.text == "__LINE__") {
                r.kind = Tok::K::number;
                r.text = std::to_string(static_cast<std::int64_t>(t.line) + line_offset_);
            } else {
                r.kind = Tok::K::string;
                r.text = "\"" + file_ + "\"";
            }
            q.push_front(std::move(r));
            return true;
        }
        if (is_unsupported_builtin(t.text) && !macros_.count(t.text)) decline("builtin '" + t.text + "'");
        const auto found = macros_.find(t.text);
        if (found == macros_.end()) return false;
        if (hs_contains(t.hide, t.text)) {
            t.painted = true;
            return false;
        }
        const Macro& macro = found->second;
        if (!macro.function) {
            auto result = substitute(macro, {}, hs_add(t.hide, t.text), t);
            for (auto it = result.rbegin(); it != result.rend(); ++it) q.push_front(std::move(*it));
            return true;
        }
        if (!next_is_paren(q, top)) return false;
        // Collect the arguments.
        std::uint32_t skipped = 0;
        Tok token;
        next_raw(q, top, token, skipped);   // '('
        std::vector<std::vector<Tok>> args(1);
        int depth = 0;
        HideSet close_hide;
        const std::size_t named = macro.variadic ? macro.params.size() - 1 : macro.params.size();
        while (true) {
            const auto before = skipped;
            if (!next_raw(q, top, token, skipped)) decline("unterminated macro invocation of '" + t.text + "'");
            if (skipped != before) token.space = true;
            if (depth == 0 && token.is(")")) {
                close_hide = token.hide;
                break;
            }
            if (token.is("(")) ++depth;
            else if (token.is(")")) --depth;
            // A top-level comma separates arguments, except inside __VA_ARGS__.
            if (depth == 0 && token.is(",") && (!macro.variadic || args.size() < macro.params.size())) {
                args.emplace_back();
                continue;
            }
            token.bol = false;
            args.back().push_back(std::move(token));
        }
        if (macro.params.empty()) {
            if (args.size() != 1 || !args[0].empty()) decline("arguments to macro '" + t.text + "' without parameters");
            args.clear();
        }
        if (macro.variadic && args.size() == named) args.emplace_back();   // empty __VA_ARGS__
        if (args.size() != macro.params.size()) decline("wrong argument count for macro '" + t.text + "'");
        for (auto& arg : args)
            if (!arg.empty()) arg.front().space = false;
        const auto hide = hs_add(hs_intersect(t.hide, close_hide), t.text);
        auto result = substitute(macro, args, hide, t);
        // As clang -E: the rest of the invocation's last line continues the
        // current output line; the lines the invocation spanned are added at
        // the next line end.
        if (top) deferred_newlines_ += skipped;
        for (auto it = result.rbegin(); it != result.rend(); ++it) q.push_front(std::move(*it));
        return true;
    }

    static int param_index(const Macro& macro, const Tok& t) {
        if (!macro.function || t.kind != Tok::K::ident) return -1;
        const auto at = std::find(macro.params.begin(), macro.params.end(), t.text);
        return at == macro.params.end() ? -1 : static_cast<int>(at - macro.params.begin());
    }

    static Tok stringize(const std::vector<Tok>& arg) {
        std::string s = "\"";
        for (std::size_t k = 0; k < arg.size(); ++k) {
            if (k && arg[k].space) s += ' ';
            if (arg[k].kind == Tok::K::string || arg[k].kind == Tok::K::character) {
                for (const char c : arg[k].text) {
                    if (c == '"' || c == '\\') s += '\\';
                    s += c;
                }
            } else {
                s += arg[k].text;
            }
        }
        s += '"';
        Tok t;
        t.kind = Tok::K::string;
        t.text = std::move(s);
        return t;
    }

    static Tok paste(const Tok& a, const Tok& b) {
        if (a.kind == Tok::K::placemarker) return b;
        if (b.kind == Tok::K::placemarker) return a;
        auto pasted = Lexer::single(a.text + b.text);
        if (!pasted) decline("pasting '" + a.text + "' and '" + b.text + "' does not give a token");
        pasted->space = a.space;
        pasted->hide = a.hide;
        return *pasted;
    }

    std::vector<Tok> substitute(const Macro& macro, const std::vector<std::vector<Tok>>& args, const HideSet& hide,
                                const Tok& invocation) {
        std::vector<Tok> out;
        std::vector<std::optional<std::vector<Tok>>> expanded(args.size());
        const auto& body = macro.body;
        const int va = macro.variadic ? static_cast<int>(macro.params.size()) - 1 : -1;
        auto placemarker = [] {
            Tok p;
            p.kind = Tok::K::placemarker;
            return p;
        };
        for (std::size_t k = 0; k < body.size(); ++k) {
            const auto& b = body[k];
            if (macro.function && b.is("#") && k + 1 < body.size() && param_index(macro, body[k + 1]) >= 0) {
                auto s = stringize(args[static_cast<std::size_t>(param_index(macro, body[k + 1]))]);
                s.space = b.space;
                out.push_back(std::move(s));
                ++k;
                continue;
            }
            if (b.is("##") && k + 1 < body.size()) {
                const auto& next = body[k + 1];
                const int p = param_index(macro, next);
                std::vector<Tok> rhs;
                if (p >= 0) rhs = args[static_cast<std::size_t>(p)];
                else rhs.push_back(next);
                if (out.empty()) decline("'##' without a left operand");
                // GNU comma swallowing: `, ## __VA_ARGS__`.
                if (p >= 0 && p == va && out.back().is(",")) {
                    if (rhs.empty()) out.pop_back();
                    else for (auto& r : rhs) out.push_back(r);
                    ++k;
                    continue;
                }
                if (rhs.empty()) rhs.push_back(placemarker());
                auto lhs = out.back();
                out.pop_back();
                out.push_back(paste(lhs, rhs.front()));
                for (std::size_t r = 1; r < rhs.size(); ++r) out.push_back(rhs[r]);
                ++k;
                continue;
            }
            const int p = param_index(macro, b);
            if (p >= 0) {
                const auto index = static_cast<std::size_t>(p);
                std::vector<Tok> replacement;
                if (k + 1 < body.size() && body[k + 1].is("##")) {
                    replacement = args[index];
                    if (replacement.empty()) replacement.push_back(placemarker());
                } else {
                    if (!expanded[index]) {
                        std::deque<Tok> q(args[index].begin(), args[index].end());
                        expanded[index] = expand_all(q);
                    }
                    replacement = *expanded[index];
                }
                if (!replacement.empty()) replacement.front().space = b.space;
                for (auto& r : replacement) out.push_back(std::move(r));
                continue;
            }
            out.push_back(b);
        }
        std::vector<Tok> result;
        result.reserve(out.size());
        for (auto& t : out) {
            if (t.kind == Tok::K::placemarker) continue;
            t.hide = hs_union(t.hide, hide);
            t.from_macro = true;
            t.bol = false;
            t.line = invocation.line;
            result.push_back(std::move(t));
        }
        if (!result.empty()) result.front().space = invocation.space;
        return result;
    }
};

} // namespace

InHouseOutput preprocess_in_house(std::string_view source, const MacroSet& builtins, const MacroSet& stdlib,
                                  const std::vector<std::pair<std::string, std::string>>& user) {
    InHouseOutput result;
    try {
        Preprocessor preprocessor(source, builtins, stdlib, user);
        result.output = preprocessor.run();
        result.handled = true;
    } catch (const Decline& why) {
        result.reason = why.what();
    }
    return result;
}

} // namespace alchemy::msl::detail
