#include "alchemy/darwin/foundation/predicate.hpp"
#include "alchemy/darwin/kit/completion/data/text.hpp"

#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstdlib>

namespace alchemy::gui::completion_kit::predicate {
namespace {

struct Failure { Status status; std::string message; };

bool letter(char16_t c) { return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || c == u'_' || c >= 0x80; }
bool digit(char16_t c) { return c >= u'0' && c <= u'9'; }
char16_t upper_ascii(char16_t c) { return c >= u'a' && c <= u'z' ? static_cast<char16_t>(c - 32) : c; }
bool same_word(std::u16string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) if (upper_ascii(a[i]) != static_cast<char16_t>(b[i])) return false;
    return true;
}
std::string narrow(std::u16string_view v) { std::string s; for (const auto c : v) s.push_back(c < 0x80 ? static_cast<char>(c) : '?'); return s; }

class Parser {
public:
    Parser(std::u16string_view text, const Arguments& arguments) : t_(text), arguments_(arguments) {}

    Node parse() {
        auto node = disjunction();
        space();
        if (at_ < t_.size()) fail(Status::invalid, "unexpected text at offset " + std::to_string(at_) + ": \"" + narrow(t_.substr(at_, 20)) + "\"");
        return node;
    }

private:
    std::u16string_view t_;
    std::size_t at_{};
    const Arguments& arguments_;

    [[noreturn]] void fail(Status status, std::string why) { throw Failure{status, std::move(why)}; }
    void space() { while (at_ < t_.size() && (t_[at_] == u' ' || t_[at_] == u'\t' || t_[at_] == u'\n' || t_[at_] == u'\r')) ++at_; }
    char16_t peek(std::size_t k = 0) const { return at_ + k < t_.size() ? t_[at_ + k] : 0; }
    // The next bare word (letters, digits, '_'), without consuming it.
    std::u16string_view word() const {
        auto e = at_;
        while (e < t_.size() && (letter(t_[e]) || digit(t_[e]))) ++e;
        return t_.substr(at_, e - at_);
    }
    bool keyword(std::string_view k) {
        space();
        const auto w = word();
        if (!same_word(w, k)) return false;
        at_ += w.size();
        return true;
    }
    bool symbol(std::u16string_view s) {
        space();
        if (t_.substr(at_, s.size()) != s) return false;
        at_ += s.size();
        return true;
    }

    Node disjunction() {
        auto left = conjunction();
        std::vector<Node> parts;
        while (keyword("OR") || symbol(u"||")) {
            if (parts.empty()) parts.push_back(std::move(left));
            parts.push_back(conjunction());
        }
        if (parts.empty()) return left;
        Node n;
        n.kind = Node::Kind::disjunction;
        n.children = std::move(parts);
        return n;
    }
    Node conjunction() {
        auto left = negation();
        std::vector<Node> parts;
        while (keyword("AND") || symbol(u"&&")) {
            if (parts.empty()) parts.push_back(std::move(left));
            parts.push_back(negation());
        }
        if (parts.empty()) return left;
        Node n;
        n.kind = Node::Kind::conjunction;
        n.children = std::move(parts);
        return n;
    }
    Node negation() {
        space();
        if (keyword("NOT") || (peek() == u'!' && peek(1) != u'=' && symbol(u"!"))) {
            Node n;
            n.kind = Node::Kind::negation;
            n.children.push_back(negation());
            return n;
        }
        return primary();
    }
    Node primary() {
        space();
        if (peek() == u'(') {
            ++at_;
            auto inner = disjunction();
            if (!symbol(u")")) fail(Status::invalid, "missing ')'");
            return inner;
        }
        if (keyword("TRUEPREDICATE")) { Node n; n.kind = Node::Kind::truth; return n; }
        if (keyword("FALSEPREDICATE")) { Node n; n.kind = Node::Kind::falsity; return n; }
        return comparison();
    }
    Node comparison() {
        Node n;
        n.kind = Node::Kind::comparison;
        if (keyword("ANY") || keyword("SOME")) n.modifier = Modifier::any;
        else if (keyword("ALL")) n.modifier = Modifier::all;
        else if (keyword("NONE")) { n.modifier = Modifier::any; n.none = true; }
        n.left = expression();
        space();
        bool negate_op = false;
        if (symbol(u"==") || symbol(u"=")) n.op = Operator::equal;
        else if (symbol(u"!=") || symbol(u"<>")) n.op = Operator::not_equal;
        else if (symbol(u"<=") || symbol(u"=<")) n.op = Operator::less_or_equal;
        else if (symbol(u">=") || symbol(u"=>")) n.op = Operator::greater_or_equal;
        else if (symbol(u"<")) n.op = Operator::less;
        else if (symbol(u">")) n.op = Operator::greater;
        else {
            if (keyword("NOT")) negate_op = true;   // "a NOT IN b" etc.
            if (keyword("BETWEEN")) n.op = Operator::between;
            else if (keyword("IN")) n.op = Operator::in;
            else if (keyword("CONTAINS")) n.op = Operator::contains;
            else if (keyword("BEGINSWITH")) n.op = Operator::begins_with;
            else if (keyword("ENDSWITH")) n.op = Operator::ends_with;
            else if (keyword("LIKE")) n.op = Operator::like;
            else if (keyword("MATCHES")) n.op = Operator::matches;
            else fail(Status::invalid, "expected a comparison operator at offset " + std::to_string(at_));
        }
        if (peek() == u'[') {
            ++at_;
            while (at_ < t_.size() && t_[at_] != u']') {
                const auto c = upper_ascii(t_[at_]);
                if (c == u'C') n.options |= case_insensitive;
                else if (c == u'D') n.options |= diacritic_insensitive;
                else if (c == u'N') n.options |= normalized;
                else if (c == u'L') {}   // locale-sensitive: no locale collation here
                else fail(Status::invalid, "bad comparison option");
                ++at_;
            }
            if (at_ >= t_.size()) fail(Status::invalid, "missing ']'");
            ++at_;
        }
        n.right = expression();
        if (negate_op) {
            Node outer;
            outer.kind = Node::Kind::negation;
            outer.children.push_back(std::move(n));
            return outer;
        }
        return n;
    }
    Expression expression() {
        space();
        auto e = term();
        space();
        if (peek() == u'+' || peek() == u'*' || peek() == u'/' || (peek() == u'-' && !digit(peek(1))))
            fail(Status::unsupported, "arithmetic expressions");
        if (peek() == u'[') fail(Status::unsupported, "index subscripts in key paths");
        return e;
    }
    Expression term() {
        space();
        const auto c = peek();
        if (c == u'"' || c == u'\'') return string_literal();
        if (digit(c) || ((c == u'-' || c == u'+') && (digit(peek(1)) || peek(1) == u'.')) || (c == u'.' && digit(peek(1)))) return number();
        if (c == u'{') {
            ++at_;
            Expression list;
            list.kind = Expression::Kind::aggregate;
            space();
            if (peek() != u'}') {
                for (;;) {
                    list.items.push_back(expression());
                    if (symbol(u",")) continue;
                    break;
                }
            }
            if (!symbol(u"}")) fail(Status::invalid, "missing '}'");
            return list;
        }
        if (c == u'%') return argument();
        if (c == u'$') {
            ++at_;
            const auto w = word();
            if (w.empty()) fail(Status::invalid, "a $ without a variable name");
            at_ += w.size();
            Expression v;
            v.kind = Expression::Kind::variable;
            v.text = std::u16string(w);
            return v;
        }
        const auto w = word();
        if (same_word(w, "TRUE") || same_word(w, "YES")) { at_ += w.size(); Expression e; e.kind = Expression::Kind::boolean; e.boolean = true; return e; }
        if (same_word(w, "FALSE") || same_word(w, "NO")) { at_ += w.size(); Expression e; e.kind = Expression::Kind::boolean; return e; }
        if (same_word(w, "NULL") || same_word(w, "NIL")) { at_ += w.size(); return Expression{}; }
        if (same_word(w, "SUBQUERY") || same_word(w, "FUNCTION") || same_word(w, "CAST")) fail(Status::unsupported, narrow(w) + " expressions");
        return key_path();
    }
    Expression key_path() {
        std::u16string path;
        for (;;) {
            space();
            if (peek() == u'#') ++at_;   // a keyword used as a key
            if (peek() == u'@') { path.push_back(u'@'); ++at_; }
            const auto w = word();
            if (w.empty()) {
                if (peek() == u'%' && (peek(1) == u'K')) {
                    at_ += 2;
                    const auto k = arguments_('K', "");
                    if (k.kind != Expression::Kind::key_path) fail(Status::invalid, "%K needs a key path string");
                    path += k.text;
                } else fail(Status::invalid, "expected an expression at offset " + std::to_string(at_));
            } else {
                path += w;
                at_ += w.size();
            }
            if (peek() == u'.' && !digit(peek(1))) { path.push_back(u'.'); ++at_; continue; }
            break;
        }
        Expression e;
        if (same_word(path, "SELF")) { e.kind = Expression::Kind::self; return e; }
        if (path.size() > 5 && same_word(std::u16string_view(path).substr(0, 5), "SELF.")) path.erase(0, 5);
        e.kind = Expression::Kind::key_path;
        e.text = std::move(path);
        return e;
    }
    Expression string_literal() {
        const auto quote = t_[at_++];
        std::u16string out;
        while (at_ < t_.size() && t_[at_] != quote) {
            auto c = t_[at_++];
            if (c == u'\\' && at_ < t_.size()) {
                c = t_[at_++];
                if (c == u'n') c = u'\n';
                else if (c == u't') c = u'\t';
                else if (c == u'r') c = u'\r';
                else if (c == u'u' && at_ + 4 <= t_.size()) {
                    c = static_cast<char16_t>(std::strtoul(narrow(t_.substr(at_, 4)).c_str(), nullptr, 16));
                    at_ += 4;
                }
            }
            out.push_back(c);
        }
        if (at_ >= t_.size()) fail(Status::invalid, "unterminated string");
        ++at_;
        Expression e;
        e.kind = Expression::Kind::string;
        e.text = std::move(out);
        return e;
    }
    Expression number() {
        std::string ascii;
        if (peek() == u'-' || peek() == u'+') ascii.push_back(static_cast<char>(t_[at_++]));
        if (peek() == u'0' && (peek(1) == u'x' || peek(1) == u'X')) {
            at_ += 2;
            std::string hex;
            while (at_ < t_.size() && std::isxdigit(static_cast<unsigned char>(t_[at_] < 0x80 ? t_[at_] : 'g'))) hex.push_back(static_cast<char>(t_[at_++]));
            Expression e;
            e.kind = Expression::Kind::integer;
            e.integer = static_cast<std::int64_t>(std::strtoull(hex.c_str(), nullptr, 16)) * (ascii == "-" ? -1 : 1);
            return e;
        }
        bool real = false;
        while (at_ < t_.size() && (digit(t_[at_]) || t_[at_] == u'.' || t_[at_] == u'e' || t_[at_] == u'E' ||
                                   ((t_[at_] == u'-' || t_[at_] == u'+') && (t_[at_ - 1] == u'e' || t_[at_ - 1] == u'E')))) {
            if (!digit(t_[at_])) real = true;
            ascii.push_back(static_cast<char>(t_[at_++]));
        }
        Expression e;
        if (real) { e.kind = Expression::Kind::real; e.real = std::strtod(ascii.c_str(), nullptr); }
        else { e.kind = Expression::Kind::integer; e.integer = std::strtoll(ascii.c_str(), nullptr, 10); }
        return e;
    }
    Expression argument() {
        ++at_;   // '%'
        std::string length;
        while (at_ < t_.size() && (t_[at_] == u'l' || t_[at_] == u'h' || t_[at_] == u'q' || t_[at_] == u'z')) length.push_back(static_cast<char>(t_[at_++]));
        if (at_ >= t_.size()) fail(Status::invalid, "a '%' at the end of the format");
        const auto c = static_cast<char>(t_[at_++]);
        switch (c) {
        case '@': case 'K': case 'd': case 'i': case 'D': case 'u': case 'U': case 'x': case 'X': case 'o': case 'O':
        case 'c': case 'C': case 'f': case 'e': case 'E': case 'g': case 'G': {
            auto e = arguments_(c, length);
            if (c == 'K') {
                // A key followed by more path components ("%K.name").
                if (peek() == u'.' && letter(peek(1))) { ++at_; auto rest = key_path(); e.text += u"." + rest.text; }
            }
            return e;
        }
        default: fail(Status::unsupported, std::string("format conversion %") + c);
        }
    }
};

std::u16string from_ascii(std::string_view s) { return std::u16string(s.begin(), s.end()); }

const char* operator_text(Operator op) {
    switch (op) {
    case Operator::less: return "<";
    case Operator::less_or_equal: return "<=";
    case Operator::greater: return ">";
    case Operator::greater_or_equal: return ">=";
    case Operator::equal: return "==";
    case Operator::not_equal: return "!=";
    case Operator::matches: return "MATCHES";
    case Operator::like: return "LIKE";
    case Operator::begins_with: return "BEGINSWITH";
    case Operator::ends_with: return "ENDSWITH";
    case Operator::in: return "IN";
    case Operator::contains: return "CONTAINS";
    case Operator::between: return "BETWEEN";
    }
    return "?";
}

} // namespace

std::optional<Node> parse(std::u16string_view format_text, const Arguments& arguments, Status& status, std::string& message) {
    status = Status::ok;
    message.clear();
    try {
        Parser parser(format_text, arguments);
        return parser.parse();
    } catch (const Failure& f) {
        status = f.status;
        message = f.message;
        return std::nullopt;
    }
}

std::u16string format(const Expression& e, const std::function<std::u16string(std::uint64_t)>& describe) {
    switch (e.kind) {
    case Expression::Kind::null: return u"nil";
    case Expression::Kind::boolean: return e.boolean ? u"1" : u"0";
    case Expression::Kind::integer: return from_ascii(std::to_string(e.integer));
    case Expression::Kind::real: {
        char buffer[40];
        std::snprintf(buffer, sizeof buffer, "%.17g", e.real);
        for (int p = 1; p <= 17; ++p) { std::snprintf(buffer, sizeof buffer, "%.*g", p, e.real); if (std::strtod(buffer, nullptr) == e.real) break; }
        return from_ascii(buffer);
    }
    case Expression::Kind::string: {
        std::u16string out = u"\"";
        for (const auto c : e.text) {
            if (c == u'"' || c == u'\\') out.push_back(u'\\');
            out.push_back(c);
        }
        return out + u"\"";
    }
    case Expression::Kind::object: return describe(e.object);
    case Expression::Kind::key_path: return e.text;
    case Expression::Kind::self: return u"SELF";
    case Expression::Kind::variable: return u"$" + e.text;
    case Expression::Kind::aggregate: {
        std::u16string out = u"{";
        for (std::size_t i = 0; i < e.items.size(); ++i) { if (i) out += u", "; out += format(e.items[i], describe); }
        return out + u"}";
    }
    }
    return {};
}

std::u16string format(const Node& n, const std::function<std::u16string(std::uint64_t)>& describe) {
    const auto child = [&](const Node& c) {
        const bool compound = c.kind == Node::Kind::conjunction || c.kind == Node::Kind::disjunction;
        return compound ? u"(" + format(c, describe) + u")" : format(c, describe);
    };
    switch (n.kind) {
    case Node::Kind::truth: return u"TRUEPREDICATE";
    case Node::Kind::falsity: return u"FALSEPREDICATE";
    case Node::Kind::foreign: return describe(n.object);
    case Node::Kind::negation: return u"NOT " + child(n.children.at(0));
    case Node::Kind::conjunction: case Node::Kind::disjunction: {
        std::u16string out;
        for (std::size_t i = 0; i < n.children.size(); ++i) {
            if (i) out += n.kind == Node::Kind::conjunction ? u" AND " : u" OR ";
            out += child(n.children[i]);
        }
        return out;
    }
    case Node::Kind::comparison: {
        std::u16string out;
        if (n.none) out += u"NONE ";
        else if (n.modifier == Modifier::any) out += u"ANY ";
        else if (n.modifier == Modifier::all) out += u"ALL ";
        out += format(n.left, describe) + u" " + from_ascii(operator_text(n.op));
        if (n.options) {
            out += u"[";
            if (n.options & case_insensitive) out += u"c";
            if (n.options & diacritic_insensitive) out += u"d";
            if (n.options & normalized) out += u"n";
            out += u"]";
        }
        return out + u" " + format(n.right, describe);
    }
    }
    return {};
}

bool like(std::u16string_view value, std::u16string_view pattern, unsigned options) {
    const auto fold = [options](char16_t c) -> char32_t {
        char32_t x = c;
        if (options & diacritic_insensitive) { const auto s = text::strip_diacritic(x); if (s) x = s; }
        if (options & case_insensitive) x = text::fold(x);
        return x;
    };
    // Iterative wildcard match with single-star backtracking.
    std::size_t v = 0, p = 0, star = std::u16string_view::npos, mark = 0;
    while (v < value.size()) {
        if (p < pattern.size() && pattern[p] == u'*') { star = p++; mark = v; continue; }
        if (p < pattern.size() && pattern[p] == u'?') { ++p; ++v; continue; }
        if (p < pattern.size()) {
            auto pc = pattern[p];
            auto step = 1;
            if (pc == u'\\' && p + 1 < pattern.size()) { pc = pattern[p + 1]; step = 2; }
            if (fold(pc) == fold(value[v])) { p += static_cast<std::size_t>(step); ++v; continue; }
        }
        if (star == std::u16string_view::npos) return false;
        p = star + 1;
        v = ++mark;
    }
    while (p < pattern.size() && pattern[p] == u'*') ++p;
    return p == pattern.size();
}

void substitute(Node& node, const std::function<std::optional<Expression>(std::u16string_view)>& lookup) {
    const std::function<void(Expression&)> replace = [&](Expression& e) {
        if (e.kind == Expression::Kind::variable) { if (auto v = lookup(e.text)) e = *v; }
        for (auto& item : e.items) replace(item);
    };
    replace(node.left);
    replace(node.right);
    for (auto& c : node.children) substitute(c, lookup);
}

} // namespace alchemy::gui::completion_kit::predicate
