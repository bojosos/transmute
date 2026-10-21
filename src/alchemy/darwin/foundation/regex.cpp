#include "alchemy/darwin/foundation/regex.hpp"
#include "alchemy/darwin/kit/completion/data/text.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace alchemy::gui::completion_kit::regex {
namespace {

using text::fold;
using text::to_lower;
using text::to_upper;

// ---- character predicates ----
enum class Builtin { digit, word, space, hspace, vspace, alpha, alnum, upper, lower, punct, xdigit, blank, cntrl, print, graph, any };

bool line_terminator(char32_t c, bool unix_only) {
    if (unix_only) return c == U'\n';
    return (c >= 0x0a && c <= 0x0d) || c == 0x85 || c == 0x2028 || c == 0x2029;
}
bool builtin_has(Builtin b, char32_t c) {
    switch (b) {
    case Builtin::digit: return text::is_digit(c);
    case Builtin::word: return text::is_word(c);
    case Builtin::space: return text::is_space(c);
    case Builtin::hspace: return c == 9 || c == 0x20 || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x202f || c == 0x205f || c == 0x3000;
    case Builtin::vspace: return (c >= 0x0a && c <= 0x0d) || c == 0x85 || c == 0x2028 || c == 0x2029;
    case Builtin::alpha: return text::is_alpha(c);
    case Builtin::alnum: return text::is_alpha(c) || text::is_digit(c);
    case Builtin::upper: return to_lower(c) != c;
    case Builtin::lower: return to_upper(c) != c;
    case Builtin::punct: return text::is_punct(c);
    case Builtin::xdigit: return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    case Builtin::blank: return c == 9 || c == 0x20 || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x202f || c == 0x205f || c == 0x3000;
    case Builtin::cntrl: return c < 0x20 || (c >= 0x7f && c < 0xa0);
    case Builtin::print: return !(c < 0x20 || (c >= 0x7f && c < 0xa0));
    case Builtin::graph: return !(c < 0x21 || (c >= 0x7f && c <= 0xa0)) && !text::is_space(c);
    case Builtin::any: return true;
    }
    return false;
}

struct CharSet {
    std::vector<std::pair<char32_t, char32_t>> ranges;
    std::vector<std::pair<Builtin, bool>> builtins;   // (class, negated)
    std::vector<CharSet> nested;                      // unions ([a[bc]], \D inside a class)
    bool negated{}, icase{};

    bool has_raw(char32_t c) const {
        for (const auto& [lo, hi] : ranges) if (c >= lo && c <= hi) return true;
        for (const auto& [b, neg] : builtins) if (builtin_has(b, c) != neg) return true;
        for (const auto& n : nested) if (n.contains(c)) return true;
        return false;
    }
    bool contains(char32_t c) const {
        bool in = has_raw(c);
        if (!in && icase) {
            const auto l = to_lower(c), u = to_upper(c);
            in = (l != c && has_raw(l)) || (u != c && has_raw(u));
        }
        return in != negated;
    }
};

// ---- AST ----
enum class Assert { bol, eol, begin_text, end_text, end_text_nl, word, not_word, search_start };

struct Node {
    enum class Kind { empty, literal, any, set, assertion, group, concat, alt, repeat, backref, look, atomic } kind{Kind::empty};
    char32_t ch{};
    bool icase{}, dotall{}, multiline{}, unix_lines{};
    int set{-1};
    Assert assertion{};
    int capture{-1};
    std::vector<std::unique_ptr<Node>> kids;
    int min{}, max{};   // max -1: unbounded
    bool greedy{true}, possessive{};
    bool ahead{true}, negate{};
};
using NodePtr = std::unique_ptr<Node>;

NodePtr make(Node::Kind kind) { auto n = std::make_unique<Node>(); n->kind = kind; return n; }

struct Flags { bool icase{}, dotall{}, multiline{}, comments{}, unix_lines{}, unicode_words{}; };

class Parser {
public:
    Parser(std::u32string pattern, std::uint64_t options, std::vector<CharSet>& sets) : p_(std::move(pattern)), sets_(sets) {
        flags_.icase = options & case_insensitive;
        flags_.comments = options & allow_comments;
        flags_.dotall = options & dot_matches_line_separators;
        flags_.multiline = options & anchors_match_lines;
        flags_.unix_lines = options & unix_line_separators;
        flags_.unicode_words = options & unicode_word_boundaries;
        total_groups_ = count_groups();
    }
    Status status{Status::ok};
    std::string message;
    int groups{0};
    std::vector<std::pair<std::u16string, int>> names;
    bool uses_word_boundary{};

    NodePtr parse() {
        auto flags = flags_;
        auto root = alternation(flags);
        if (status == Status::ok && at_ < p_.size()) error(Status::invalid, "unmatched ')'");
        return root;
    }

private:
    std::u32string p_;
    std::size_t at_{};
    std::vector<CharSet>& sets_;
    Flags flags_;
    int total_groups_{};

    void error(Status s, std::string why) {
        if (status == Status::ok) { status = s; message = std::move(why) + " at offset " + std::to_string(at_); }
        at_ = p_.size();
    }
    bool more() const { return at_ < p_.size(); }
    char32_t peek(std::size_t ahead = 0) const { return at_ + ahead < p_.size() ? p_[at_ + ahead] : 0; }
    int count_groups() const {
        int n = 0;
        bool in_class = false;
        for (std::size_t i = 0; i < p_.size(); ++i) {
            const auto c = p_[i];
            if (c == '\\') { ++i; continue; }
            if (in_class) { if (c == ']') in_class = false; continue; }
            if (c == '[') { in_class = true; if (i + 1 < p_.size() && p_[i + 1] == ']') ++i; continue; }
            if (c == '(') {
                if (i + 1 < p_.size() && p_[i + 1] == '?') {
                    if (i + 2 < p_.size() && p_[i + 2] == '<' && i + 3 < p_.size() && p_[i + 3] != '=' && p_[i + 3] != '!') ++n;
                } else ++n;
            }
        }
        return n;
    }
    void skip_extended(const Flags& f) {
        if (!f.comments) return;
        while (more()) {
            const auto c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == 0x0b) { ++at_; continue; }
            if (c == '#') { while (more() && peek() != '\n') ++at_; continue; }
            break;
        }
    }
    NodePtr alternation(Flags& f) {
        std::vector<NodePtr> options;
        options.push_back(sequence(f));
        while (status == Status::ok && more() && peek() == '|') {
            ++at_;
            options.push_back(sequence(f));
        }
        if (options.size() == 1) return std::move(options[0]);
        auto n = make(Node::Kind::alt);
        n->kids = std::move(options);
        return n;
    }
    NodePtr sequence(Flags& f) {
        auto n = make(Node::Kind::concat);
        for (;;) {
            skip_extended(f);
            if (status != Status::ok || !more() || peek() == '|' || peek() == ')') break;
            auto atom_node = atom(f);
            if (status != Status::ok) break;
            if (!atom_node) continue;   // flag group / comment
            skip_extended(f);
            atom_node = quantified(std::move(atom_node), f);
            n->kids.push_back(std::move(atom_node));
        }
        return n;
    }
    std::optional<int> number() {
        if (!more() || peek() < '0' || peek() > '9') return std::nullopt;
        long long v = 0;
        while (more() && peek() >= '0' && peek() <= '9') { v = v * 10 + (peek() - '0'); if (v > 100000000) { error(Status::invalid, "quantifier too large"); return std::nullopt; } ++at_; }
        return static_cast<int>(v);
    }
    NodePtr quantified(NodePtr atom_node, Flags& f) {
        for (;;) {
            if (!more()) return atom_node;
            int lo = 0, hi = -1;
            const auto c = peek();
            if (c == '*') { lo = 0; hi = -1; ++at_; }
            else if (c == '+') { lo = 1; hi = -1; ++at_; }
            else if (c == '?') { lo = 0; hi = 1; ++at_; }
            else if (c == '{') {
                const auto save = at_;
                ++at_;
                const auto first = number();
                if (!first) { at_ = save; error(Status::invalid, "bad interval"); return atom_node; }
                lo = *first; hi = lo;
                if (more() && peek() == ',') {
                    ++at_;
                    if (more() && peek() == '}') hi = -1;
                    else { const auto second = number(); if (!second) { error(Status::invalid, "bad interval"); return atom_node; } hi = *second; }
                }
                if (!more() || peek() != '}') { error(Status::invalid, "bad interval"); return atom_node; }
                ++at_;
                if (hi >= 0 && hi < lo) { error(Status::invalid, "interval maximum below minimum"); return atom_node; }
            } else return atom_node;
            if (atom_node->kind == Node::Kind::assertion || atom_node->kind == Node::Kind::look) {
                // ICU accepts quantified assertions; {0,..} makes them optional.
            }
            auto r = make(Node::Kind::repeat);
            r->min = lo; r->max = hi;
            if (more() && peek() == '?') { r->greedy = false; ++at_; }
            else if (more() && peek() == '+') { r->possessive = true; ++at_; }
            r->kids.push_back(std::move(atom_node));
            atom_node = std::move(r);
            skip_extended(f);
        }
    }
    NodePtr literal(char32_t c, const Flags& f) {
        auto n = make(Node::Kind::literal);
        n->ch = c;
        n->icase = f.icase && (to_lower(c) != c || to_upper(c) != c);
        return n;
    }
    NodePtr set_node(CharSet set) {
        auto n = make(Node::Kind::set);
        sets_.push_back(std::move(set));
        n->set = static_cast<int>(sets_.size() - 1);
        return n;
    }
    NodePtr assertion(Assert a, const Flags& f) {
        auto n = make(Node::Kind::assertion);
        n->assertion = a;
        n->multiline = f.multiline;
        n->unix_lines = f.unix_lines;
        if (a == Assert::word || a == Assert::not_word) {
            uses_word_boundary = true;
            if (f.unicode_words) error(Status::unsupported, "Unicode (UAX #29) word boundaries");
        }
        return n;
    }
    std::optional<char32_t> hex(std::size_t digits) {
        char32_t v = 0;
        for (std::size_t i = 0; i < digits; ++i) {
            const auto c = peek();
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return std::nullopt;
            v = v * 16 + static_cast<char32_t>(d);
            ++at_;
        }
        return v;
    }
    // A character escape (after the backslash); nullopt when `c` is not one.
    std::optional<char32_t> char_escape(char32_t c) {
        switch (c) {
        case 't': return U'\t';
        case 'n': return U'\n';
        case 'r': return U'\r';
        case 'f': return U'\f';
        case 'a': return 7;
        case 'e': return 0x1b;
        case 'c': { if (!more()) { error(Status::invalid, "\\c at the end"); return 0; } const auto x = peek(); ++at_; return x ^ 0x40; }
        case '0': {
            char32_t v = 0;
            for (int i = 0; i < 3 && more() && peek() >= '0' && peek() <= '7'; ++i) { v = v * 8 + (peek() - '0'); ++at_; }
            return v;
        }
        case 'x': {
            if (more() && peek() == '{') {
                ++at_;
                char32_t v = 0;
                int count = 0;
                while (more() && peek() != '}') { const auto d = hex(1); if (!d) { error(Status::invalid, "bad \\x{...}"); return 0; } v = v * 16 + *d; ++count; }
                if (!more() || count == 0 || count > 6 || v > 0x10ffff) { error(Status::invalid, "bad \\x{...}"); return 0; }
                ++at_;
                return v;
            }
            const auto v = hex(2);
            if (!v) { error(Status::invalid, "bad \\x escape"); return 0; }
            return *v;
        }
        case 'u': { const auto v = hex(4); if (!v) { error(Status::invalid, "bad \\u escape"); return 0; } return *v; }
        case 'U': { const auto v = hex(8); if (!v || *v > 0x10ffff) { error(Status::invalid, "bad \\U escape"); return 0; } return *v; }
        default: return std::nullopt;
        }
    }
    std::optional<std::pair<Builtin, bool>> builtin_escape(char32_t c) {
        switch (c) {
        case 'd': return std::pair{Builtin::digit, false};
        case 'D': return std::pair{Builtin::digit, true};
        case 'w': return std::pair{Builtin::word, false};
        case 'W': return std::pair{Builtin::word, true};
        case 's': return std::pair{Builtin::space, false};
        case 'S': return std::pair{Builtin::space, true};
        case 'h': return std::pair{Builtin::hspace, false};
        case 'H': return std::pair{Builtin::hspace, true};
        case 'v': return std::pair{Builtin::vspace, false};
        case 'V': return std::pair{Builtin::vspace, true};
        default: return std::nullopt;
        }
    }
    bool unsupported_escape(char32_t c) {
        if (c == 'p' || c == 'P' || c == 'N' || c == 'X' || c == 'R' || c == 'O') {
            error(Status::unsupported, std::string("the \\") + static_cast<char>(c) + " construct");
            return true;
        }
        return false;
    }
    NodePtr escape(const Flags& f) {
        if (!more()) { error(Status::invalid, "trailing backslash"); return nullptr; }
        const auto c = peek();
        ++at_;
        if (unsupported_escape(c)) return nullptr;
        if (const auto b = builtin_escape(c)) { CharSet s; s.builtins.push_back(*b); return set_node(std::move(s)); }
        switch (c) {
        case 'b': return assertion(Assert::word, f);
        case 'B': return assertion(Assert::not_word, f);
        case 'A': return assertion(Assert::begin_text, f);
        case 'z': return assertion(Assert::end_text, f);
        case 'Z': return assertion(Assert::end_text_nl, f);
        case 'G': return assertion(Assert::search_start, f);
        case 'Q': {
            auto n = make(Node::Kind::concat);
            while (more() && !(peek() == '\\' && peek(1) == 'E')) { n->kids.push_back(literal(peek(), f)); ++at_; }
            if (more()) at_ += 2;
            return n;
        }
        case 'E': return make(Node::Kind::empty);
        case 'k': {
            if (!more() || peek() != '<') { error(Status::invalid, "bad \\k<name>"); return nullptr; }
            ++at_;
            std::u16string name;
            while (more() && peek() != '>') { name.push_back(static_cast<char16_t>(peek())); ++at_; }
            if (!more()) { error(Status::invalid, "bad \\k<name>"); return nullptr; }
            ++at_;
            for (const auto& [n, index] : names) if (n == name) { auto r = make(Node::Kind::backref); r->capture = index; r->icase = f.icase; return r; }
            error(Status::invalid, "\\k names an undefined group");
            return nullptr;
        }
        default: break;
        }
        if (c >= '1' && c <= '9') {
            int v = static_cast<int>(c - '0');
            while (more() && peek() >= '0' && peek() <= '9' && v * 10 + static_cast<int>(peek() - '0') <= total_groups_) { v = v * 10 + static_cast<int>(peek() - '0'); ++at_; }
            if (v > total_groups_) { error(Status::invalid, "back reference to a nonexistent group"); return nullptr; }
            auto r = make(Node::Kind::backref);
            r->capture = v;
            r->icase = f.icase;
            return r;
        }
        if (const auto ch = char_escape(c)) return status == Status::ok ? literal(*ch, f) : nullptr;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) { error(Status::invalid, std::string("unknown escape \\") + static_cast<char>(c)); return nullptr; }
        return literal(c, f);
    }
    bool posix_class(CharSet& set) {
        // At "[:" inside a class.
        const auto close = p_.find(U":]", at_ + 2);
        if (close == std::u32string::npos) return false;
        std::string name;
        bool negated = false;
        for (auto i = at_ + 2; i < close; ++i) name.push_back(static_cast<char>(p_[i]));
        if (!name.empty() && name[0] == '^') { negated = true; name.erase(0, 1); }
        static const std::pair<const char*, Builtin> table[] = {
            {"alpha", Builtin::alpha}, {"digit", Builtin::digit}, {"alnum", Builtin::alnum}, {"space", Builtin::space}, {"upper", Builtin::upper},
            {"lower", Builtin::lower}, {"punct", Builtin::punct}, {"xdigit", Builtin::xdigit}, {"word", Builtin::word}, {"blank", Builtin::blank},
            {"cntrl", Builtin::cntrl}, {"print", Builtin::print}, {"graph", Builtin::graph}};
        for (const auto& [n, b] : table)
            if (name == n) { set.builtins.emplace_back(b, negated); at_ = close + 2; return true; }
        error(Status::unsupported, "the POSIX class [:" + name + ":]");
        return true;
    }
    std::optional<CharSet> char_class(const Flags& f) {
        // After '['.
        CharSet set;
        set.icase = f.icase;
        if (more() && peek() == '^') { set.negated = true; ++at_; }
        bool first = true;
        std::optional<char32_t> pending;   // last single character (range start candidate)
        const auto flush = [&] { if (pending) { set.ranges.emplace_back(*pending, *pending); pending.reset(); } };
        for (;;) {
            if (!more()) { error(Status::invalid, "missing ']'"); return std::nullopt; }
            auto c = peek();
            if (c == ']' && !first) { ++at_; break; }
            first = false;
            if ((c == '&' && peek(1) == '&') || (c == '-' && peek(1) == '-' && pending)) { error(Status::unsupported, "class set operations (&& / --)"); return std::nullopt; }
            if (f.comments && (c == ' ' || c == '\t' || c == '\n' || c == '\r')) { ++at_; continue; }
            if (c == '[') {
                if (peek(1) == ':') { flush(); if (posix_class(set)) { if (status != Status::ok) return std::nullopt; continue; } }
                ++at_;
                flush();
                auto inner = char_class(f);
                if (!inner) return std::nullopt;
                set.nested.push_back(std::move(*inner));
                continue;
            }
            std::optional<char32_t> single;
            if (c == '\\') {
                ++at_;
                if (!more()) { error(Status::invalid, "trailing backslash"); return std::nullopt; }
                const auto e = peek();
                ++at_;
                if (unsupported_escape(e)) return std::nullopt;
                if (const auto b = builtin_escape(e)) { flush(); set.builtins.push_back(*b); continue; }
                if (e == 'b') single = 8;
                else if (e == 'Q') {
                    flush();
                    while (more() && !(peek() == '\\' && peek(1) == 'E')) { set.ranges.emplace_back(peek(), peek()); ++at_; }
                    if (more()) at_ += 2;
                    continue;
                } else if (const auto ch = char_escape(e)) { if (status != Status::ok) return std::nullopt; single = *ch; }
                else if ((e >= 'a' && e <= 'z') || (e >= 'A' && e <= 'Z') || (e >= '0' && e <= '9')) { error(Status::invalid, std::string("unknown escape \\") + static_cast<char>(e) + " in a class"); return std::nullopt; }
                else single = e;
            } else { single = c; ++at_; }
            // A range "x-y" (a '-' before ']' is literal).
            if (pending && *single == U'-' && c == '-' && more() && peek() != ']') {
                // c was an unescaped '-': read the range end.
                char32_t end{};
                auto e = peek();
                if (e == '[') { error(Status::invalid, "bad class range"); return std::nullopt; }
                if (e == '\\') {
                    ++at_;
                    if (!more()) { error(Status::invalid, "trailing backslash"); return std::nullopt; }
                    const auto x = peek();
                    ++at_;
                    if (const auto ch = char_escape(x)) end = *ch;
                    else if (builtin_escape(x)) { error(Status::invalid, "a class escape as a range end"); return std::nullopt; }
                    else end = x;
                } else { end = e; ++at_; }
                if (end < *pending) { error(Status::invalid, "class range out of order"); return std::nullopt; }
                set.ranges.emplace_back(*pending, end);
                pending.reset();
                continue;
            }
            flush();
            pending = single;
        }
        flush();
        return set;
    }
    NodePtr group(Flags& f) {
        // After '('.
        Flags inner = f;
        if (more() && peek() == '?') {
            ++at_;
            const auto c = peek();
            if (c == '#') { while (more() && peek() != ')') ++at_; if (!more()) { error(Status::invalid, "unterminated comment"); return nullptr; } ++at_; return nullptr; }
            if (c == ':' ) { ++at_; return close(alternation(inner)); }
            if (c == '>') { ++at_; auto n = make(Node::Kind::atomic); n->kids.push_back(alternation(inner)); return close(std::move(n)); }
            if (c == '=' || c == '!') { ++at_; auto n = make(Node::Kind::look); n->ahead = true; n->negate = c == '!'; n->kids.push_back(alternation(inner)); return close(std::move(n)); }
            if (c == '<' && (peek(1) == '=' || peek(1) == '!')) {
                const bool neg = peek(1) == '!';
                at_ += 2;
                auto n = make(Node::Kind::look);
                n->ahead = false; n->negate = neg;
                n->kids.push_back(alternation(inner));
                return close(std::move(n));
            }
            if (c == '<') {
                ++at_;
                std::u16string name;
                while (more() && ((peek() >= 'a' && peek() <= 'z') || (peek() >= 'A' && peek() <= 'Z') || (peek() >= '0' && peek() <= '9'))) { name.push_back(static_cast<char16_t>(peek())); ++at_; }
                if (name.empty() || !more() || peek() != '>' || (name[0] >= '0' && name[0] <= '9')) { error(Status::invalid, "bad group name"); return nullptr; }
                ++at_;
                for (const auto& [n, i] : names) if (n == name) { error(Status::invalid, "duplicate group name"); return nullptr; }
                auto n = make(Node::Kind::group);
                n->capture = ++groups;
                names.emplace_back(name, n->capture);
                n->kids.push_back(alternation(inner));
                return close(std::move(n));
            }
            // Flags: (?ismwx-ismwx) or (?ismwx-ismwx:...).
            bool on = true;
            Flags changed = f;
            while (more() && peek() != ')' && peek() != ':') {
                const auto x = peek();
                ++at_;
                if (x == '-') { on = false; continue; }
                if (x == 'i') changed.icase = on;
                else if (x == 's') changed.dotall = on;
                else if (x == 'm') changed.multiline = on;
                else if (x == 'x') changed.comments = on;
                else if (x == 'w') changed.unicode_words = on;
                else if (x == 'd') changed.unix_lines = on;
                else { error(Status::invalid, "bad (? construct"); return nullptr; }
            }
            if (!more()) { error(Status::invalid, "missing ')'"); return nullptr; }
            if (peek() == ')') { ++at_; f = changed; return nullptr; }
            ++at_;   // ':'
            return close(alternation(changed));
        }
        auto n = make(Node::Kind::group);
        n->capture = ++groups;
        n->kids.push_back(alternation(inner));
        return close(std::move(n));
    }
    NodePtr close(NodePtr n) {
        if (status != Status::ok) return nullptr;
        if (!more() || peek() != ')') { error(Status::invalid, "missing ')'"); return nullptr; }
        ++at_;
        return n;
    }
    NodePtr atom(Flags& f) {
        const auto c = peek();
        ++at_;
        switch (c) {
        case '(': return group(f);
        case '[': { auto s = char_class(f); return s ? set_node(std::move(*s)) : nullptr; }
        case '.': { auto n = make(Node::Kind::any); n->dotall = f.dotall; n->unix_lines = f.unix_lines; return n; }
        case '^': return assertion(Assert::bol, f);
        case '$': return assertion(Assert::eol, f);
        case '\\': return escape(f);
        case '*': case '+': case '?': error(Status::invalid, "a quantifier without an operand"); return nullptr;
        case '{': {
            // ICU: a '{' that does not start a valid interval is an error.
            error(Status::invalid, "a quantifier without an operand");
            return nullptr;
        }
        default: return literal(c, f);
        }
    }
};

// ---- program ----
enum class Op { character, character_icase, any, any_dotall, set, split, jump, save, assertion, backref, sub, match, mark, check };
struct Instruction {
    Op op{};
    char32_t ch{};
    int a{}, b{};          // split: preferred, other; jump: target; save/mark/check: slot; set: index; sub: index; backref: group
    Assert assertion{};
    bool multiline{}, unix_lines{}, icase{};
};
struct Sub {
    enum class Kind { ahead, behind, atomic } kind{};
    bool negate{};
    int start{};
    int min_length{}, max_length{};
};

int min_length(const Node& n);
std::optional<int> max_length(const Node& n);
int min_length(const Node& n) {
    switch (n.kind) {
    case Node::Kind::literal: return n.ch > 0xffff ? 2 : 1;
    case Node::Kind::any: case Node::Kind::set: return 1;
    case Node::Kind::group: case Node::Kind::atomic: return min_length(*n.kids[0]);
    case Node::Kind::concat: { int t = 0; for (const auto& k : n.kids) t += min_length(*k); return t; }
    case Node::Kind::alt: { int m = INT32_MAX; for (const auto& k : n.kids) m = std::min(m, min_length(*k)); return n.kids.empty() ? 0 : m; }
    case Node::Kind::repeat: return n.min * min_length(*n.kids[0]);
    default: return 0;
    }
}
std::optional<int> max_length(const Node& n) {
    switch (n.kind) {
    case Node::Kind::literal: return n.ch > 0xffff ? 2 : 1;
    case Node::Kind::any: case Node::Kind::set: return 2;   // a surrogate pair
    case Node::Kind::group: case Node::Kind::atomic: return max_length(*n.kids[0]);
    case Node::Kind::concat: { int t = 0; for (const auto& k : n.kids) { const auto m = max_length(*k); if (!m) return std::nullopt; t += *m; } return t; }
    case Node::Kind::alt: { int m = 0; for (const auto& k : n.kids) { const auto x = max_length(*k); if (!x) return std::nullopt; m = std::max(m, *x); } return m; }
    case Node::Kind::repeat: { if (n.max < 0) return std::nullopt; const auto k = max_length(*n.kids[0]); if (!k) return std::nullopt; return *k * n.max; }
    case Node::Kind::backref: return std::nullopt;
    default: return 0;
    }
}
bool nullable(const Node& n) { return min_length(n) == 0; }

} // namespace

struct Pattern::Program {
    std::vector<Instruction> code;
    std::vector<CharSet> sets;
    std::vector<Sub> subs;
    int captures{};
    int marks{};
    std::vector<std::pair<std::u16string, int>> names;

    // ---- compilation ----
    void emit_node(const Node& n) {
        switch (n.kind) {
        case Node::Kind::empty: break;
        case Node::Kind::literal: {
            Instruction i;
            i.op = n.icase ? Op::character_icase : Op::character;
            i.ch = n.icase ? fold(n.ch) : n.ch;
            code.push_back(i);
            break;
        }
        case Node::Kind::any: {
            Instruction i;
            i.op = n.dotall ? Op::any_dotall : Op::any;
            i.unix_lines = n.unix_lines;
            code.push_back(i);
            break;
        }
        case Node::Kind::set: { Instruction i; i.op = Op::set; i.a = n.set; code.push_back(i); break; }
        case Node::Kind::assertion: {
            Instruction i;
            i.op = Op::assertion;
            i.assertion = n.assertion;
            i.multiline = n.multiline;
            i.unix_lines = n.unix_lines;
            code.push_back(i);
            break;
        }
        case Node::Kind::group: {
            Instruction open; open.op = Op::save; open.a = 2 * n.capture;
            code.push_back(open);
            emit_node(*n.kids[0]);
            Instruction shut; shut.op = Op::save; shut.a = 2 * n.capture + 1;
            code.push_back(shut);
            break;
        }
        case Node::Kind::concat: for (const auto& k : n.kids) emit_node(*k); break;
        case Node::Kind::alt: {
            std::vector<std::size_t> jumps;
            for (std::size_t k = 0; k < n.kids.size(); ++k) {
                if (k + 1 < n.kids.size()) {
                    const auto split = code.size();
                    code.push_back({Op::split});
                    code[split].a = static_cast<int>(code.size());
                    emit_node(*n.kids[k]);
                    jumps.push_back(code.size());
                    code.push_back({Op::jump});
                    code[split].b = static_cast<int>(code.size());
                } else emit_node(*n.kids[k]);
            }
            for (const auto j : jumps) code[j].a = static_cast<int>(code.size());
            break;
        }
        case Node::Kind::repeat: {
            if (n.possessive) {
                // X*+ is (?>X*).
                emit_atomic([&] { emit_repeat(*n.kids[0], n.min, n.max, true); });
                break;
            }
            emit_repeat(*n.kids[0], n.min, n.max, n.greedy);
            break;
        }
        case Node::Kind::backref: { Instruction i; i.op = Op::backref; i.a = n.capture; i.icase = n.icase; code.push_back(i); break; }
        case Node::Kind::atomic: emit_atomic([&] { emit_node(*n.kids[0]); }); break;
        case Node::Kind::look: {
            Sub s;
            s.kind = n.ahead ? Sub::Kind::ahead : Sub::Kind::behind;
            s.negate = n.negate;
            if (!n.ahead) {
                s.min_length = min_length(*n.kids[0]);
                const auto m = max_length(*n.kids[0]);
                if (!m) throw std::invalid_argument("look-behind without a bounded maximum length");
                s.max_length = *m;
            }
            emit_sub(s, [&] { emit_node(*n.kids[0]); });
            break;
        }
        }
    }
    template <typename Body> void emit_atomic(Body body) {
        Sub s;
        s.kind = Sub::Kind::atomic;
        emit_sub(s, body);
    }
    // The sub-program is placed inline after a jump over it.
    template <typename Body> void emit_sub(Sub s, Body body) {
        const auto index = subs.size();
        subs.push_back(s);
        Instruction call; call.op = Op::sub; call.a = static_cast<int>(index);
        code.push_back(call);
        const auto skip = code.size();
        code.push_back({Op::jump});
        subs[index].start = static_cast<int>(code.size());
        body();
        code.push_back({Op::match});
        code[skip].a = static_cast<int>(code.size());
    }
    void emit_repeat(const Node& child, int lo, int hi, bool greedy) {
        if (code.size() > 400000) throw std::length_error("pattern too large");
        for (int k = 0; k < lo; ++k) emit_node(child);
        const bool empty_ok = nullable(child);
        if (hi < 0) {
            // L: split(body, exit); [mark] body [check]; jump L
            const auto loop = code.size();
            code.push_back({Op::split});
            const int mark = empty_ok ? marks++ : -1;
            code[loop].a = static_cast<int>(code.size());
            if (empty_ok) { Instruction m; m.op = Op::mark; m.a = mark; code.push_back(m); }
            emit_node(child);
            if (empty_ok) { Instruction c; c.op = Op::check; c.a = mark; code.push_back(c); }
            Instruction back; back.op = Op::jump; back.a = static_cast<int>(loop);
            code.push_back(back);
            code[loop].b = static_cast<int>(code.size());
            if (!greedy) std::swap(code[loop].a, code[loop].b);
            return;
        }
        // Optional copies, nested: split(body, exit) body split(body, exit) ...
        std::vector<std::size_t> splits;
        for (int k = lo; k < hi; ++k) {
            const auto split = code.size();
            code.push_back({Op::split});
            code[split].a = static_cast<int>(code.size());
            splits.push_back(split);
            emit_node(child);
            if (code.size() > 400000) throw std::length_error("pattern too large");
        }
        for (const auto split : splits) {
            code[split].b = static_cast<int>(code.size());
            if (!greedy) std::swap(code[split].a, code[split].b);
        }
    }

    // ---- matching ----
    struct Context {
        std::u16string_view text;
        std::int64_t lo{}, hi{};             // the search range
        std::int64_t look_lo{}, look_hi{};   // visible to look-around and \b
        std::int64_t anchor_lo{}, anchor_hi{};
        std::int64_t search_start{};
        mutable std::uint64_t steps{};
    };
    char32_t at(const Context& c, std::int64_t pos, std::int64_t limit, int& width) const {
        const auto u = static_cast<char32_t>(c.text[static_cast<std::size_t>(pos)]);
        width = 1;
        if (u >= 0xd800 && u <= 0xdbff && pos + 1 < limit) {
            const auto v = static_cast<char32_t>(c.text[static_cast<std::size_t>(pos + 1)]);
            if (v >= 0xdc00 && v <= 0xdfff) { width = 2; return 0x10000 + ((u - 0xd800) << 10) + (v - 0xdc00); }
        }
        return u;
    }
    char32_t before(const Context& c, std::int64_t pos, std::int64_t limit_lo) const {
        const auto u = static_cast<char32_t>(c.text[static_cast<std::size_t>(pos - 1)]);
        if (u >= 0xdc00 && u <= 0xdfff && pos - 2 >= limit_lo) {
            const auto h = static_cast<char32_t>(c.text[static_cast<std::size_t>(pos - 2)]);
            if (h >= 0xd800 && h <= 0xdbff) return 0x10000 + ((h - 0xd800) << 10) + (u - 0xdc00);
        }
        return u;
    }
    bool word_at(const Context& c, std::int64_t pos) const {
        if (pos < c.look_lo || pos >= c.look_hi) return false;
        int w;
        return text::is_word(at(c, pos, c.look_hi, w));
    }
    bool word_before(const Context& c, std::int64_t pos) const {
        if (pos <= c.look_lo || pos > c.look_hi) return false;
        return text::is_word(before(c, pos, c.look_lo));
    }
    bool terminator_at(const Context& c, std::int64_t pos, bool unix_lines) const {
        return pos < c.look_hi && line_terminator(c.text[static_cast<std::size_t>(pos)], unix_lines);
    }
    bool check_assertion(const Context& c, const Instruction& i, std::int64_t pos) const {
        switch (i.assertion) {
        case Assert::begin_text: return pos == c.anchor_lo;
        case Assert::end_text: return pos == c.anchor_hi;
        case Assert::search_start: return pos == c.search_start;
        case Assert::end_text_nl: case Assert::eol: {
            if (pos == c.anchor_hi) return true;
            if (pos > c.anchor_hi || !terminator_at(c, pos, i.unix_lines)) return false;
            const bool inside_crlf = pos > c.look_lo && c.text[static_cast<std::size_t>(pos - 1)] == u'\r' && c.text[static_cast<std::size_t>(pos)] == u'\n';
            if (i.assertion == Assert::eol && i.multiline) return !inside_crlf;
            // Before the final line terminator only (\r\n counts as one).
            if (pos + 1 == c.anchor_hi) return true;
            return !i.unix_lines && pos + 2 == c.anchor_hi && c.text[static_cast<std::size_t>(pos)] == u'\r' && c.text[static_cast<std::size_t>(pos + 1)] == u'\n';
        }
        case Assert::bol: {
            if (pos == c.anchor_lo) return true;
            if (!i.multiline) return false;
            if (pos >= c.anchor_hi || pos <= c.look_lo) return false;
            const auto prev = c.text[static_cast<std::size_t>(pos - 1)];
            if (!line_terminator(prev, i.unix_lines)) return false;
            return !(prev == u'\r' && c.text[static_cast<std::size_t>(pos)] == u'\n');
        }
        case Assert::word: return word_before(c, pos) != word_at(c, pos);
        case Assert::not_word: return word_before(c, pos) == word_at(c, pos);
        }
        return false;
    }

    // Runs from `pc` at `pos`; on success returns the end position and leaves
    // the captures in `slots`. `required_end` >= 0: the match must end there.
    std::optional<std::int64_t> run(const Context& c, int pc, std::int64_t pos, std::vector<std::int64_t>& slots, std::int64_t required_end, std::int64_t limit_lo, std::int64_t limit_hi) const {
        struct Entry { int pc; std::int64_t pos; int slot; std::int64_t old; };   // slot >= 0: a restore
        std::vector<Entry> stack;
        stack.push_back({pc, pos, -1, 0});
        while (!stack.empty()) {
            auto e = stack.back();
            stack.pop_back();
            if (e.slot >= 0) { slots[static_cast<std::size_t>(e.slot)] = e.old; continue; }
            pc = e.pc;
            pos = e.pos;
            for (;;) {
                if (++c.steps > 200000000ull) throw Exhausted("regular expression backtracking limit reached");
                const auto& i = code[static_cast<std::size_t>(pc)];
                bool ok = true;
                switch (i.op) {
                case Op::character: case Op::character_icase: {
                    if (pos >= limit_hi) { ok = false; break; }
                    int w;
                    const auto ch = at(c, pos, limit_hi, w);
                    if (i.op == Op::character ? ch != i.ch : fold(ch) != i.ch) { ok = false; break; }
                    pos += w; ++pc;
                    break;
                }
                case Op::any: case Op::any_dotall: {
                    if (pos >= limit_hi) { ok = false; break; }
                    int w;
                    const auto ch = at(c, pos, limit_hi, w);
                    if (i.op == Op::any && line_terminator(ch, i.unix_lines)) { ok = false; break; }
                    // \r\n is one character for a dot that matches line terminators.
                    if (i.op == Op::any_dotall && ch == U'\r' && pos + 1 < limit_hi && c.text[static_cast<std::size_t>(pos + 1)] == u'\n' && !i.unix_lines) w = 2;
                    pos += w; ++pc;
                    break;
                }
                case Op::set: {
                    if (pos >= limit_hi) { ok = false; break; }
                    int w;
                    const auto ch = at(c, pos, limit_hi, w);
                    if (!sets[static_cast<std::size_t>(i.a)].contains(ch)) { ok = false; break; }
                    pos += w; ++pc;
                    break;
                }
                case Op::split: stack.push_back({i.b, pos, -1, 0}); pc = i.a; break;
                case Op::jump: pc = i.a; break;
                case Op::save: case Op::mark:
                    stack.push_back({0, 0, i.op == Op::save ? i.a : 2 * (captures + 1) + i.a, slots[static_cast<std::size_t>(i.op == Op::save ? i.a : 2 * (captures + 1) + i.a)]});
                    slots[static_cast<std::size_t>(i.op == Op::save ? i.a : 2 * (captures + 1) + i.a)] = pos;
                    ++pc;
                    break;
                case Op::check: if (slots[static_cast<std::size_t>(2 * (captures + 1) + i.a)] == pos) ok = false; else ++pc; break;
                case Op::assertion: if (!check_assertion(c, i, pos)) ok = false; else ++pc; break;
                case Op::backref: {
                    const auto s = slots[static_cast<std::size_t>(2 * i.a)], t = slots[static_cast<std::size_t>(2 * i.a + 1)];
                    if (s < 0 || t < 0) { ok = false; break; }
                    const auto n = t - s;
                    if (pos + n > limit_hi) { ok = false; break; }
                    for (std::int64_t k = 0; k < n && ok; ++k) {
                        const auto x = static_cast<char32_t>(c.text[static_cast<std::size_t>(s + k)]), y = static_cast<char32_t>(c.text[static_cast<std::size_t>(pos + k)]);
                        ok = i.icase ? fold(x) == fold(y) : x == y;
                    }
                    if (ok) { pos += n; ++pc; }
                    break;
                }
                case Op::sub: {
                    const auto& s = subs[static_cast<std::size_t>(i.a)];
                    auto trial = slots;
                    std::optional<std::int64_t> end;
                    if (s.kind == Sub::Kind::ahead) end = run(c, s.start, pos, trial, -1, limit_lo, c.look_hi);
                    else if (s.kind == Sub::Kind::atomic) end = run(c, s.start, pos, trial, -1, limit_lo, limit_hi);
                    else {
                        for (int length = s.min_length; length <= s.max_length && !end; ++length) {
                            if (pos - length < c.look_lo) break;
                            auto attempt = slots;
                            if (run(c, s.start, pos - length, attempt, pos, c.look_lo, pos)) { end = pos; trial = std::move(attempt); }
                        }
                    }
                    if (s.negate) { if (end) ok = false; else ++pc; break; }
                    if (!end) { ok = false; break; }
                    // Keep the sub-match's captures (restored on backtracking).
                    for (std::size_t k = 0; k < slots.size(); ++k)
                        if (slots[k] != trial[k]) { stack.push_back({0, 0, static_cast<int>(k), slots[k]}); slots[k] = trial[k]; }
                    if (s.kind == Sub::Kind::atomic) pos = *end;
                    ++pc;
                    break;
                }
                case Op::match:
                    if (required_end >= 0 && pos != required_end) { ok = false; break; }
                    return pos;
                }
                if (!ok) break;
            }
        }
        return std::nullopt;
    }
};

Pattern::Pattern(std::unique_ptr<Program> program) : program_(std::move(program)) {}
Pattern::~Pattern() = default;
Pattern::Pattern(Pattern&&) noexcept = default;
Pattern& Pattern::operator=(Pattern&&) noexcept = default;

std::optional<Pattern> Pattern::compile(std::u16string_view pattern, std::uint64_t options, Status& status, std::string& message) {
    status = Status::ok;
    message.clear();
    if (options & ~std::uint64_t{127}) { status = Status::invalid; message = "unknown options"; return std::nullopt; }
    auto program = std::make_unique<Program>();
    const auto code_points = text::code_points(pattern);
    NodePtr root;
    if (options & ignore_metacharacters) {
        root = make(Node::Kind::concat);
        for (const auto c : code_points) {
            auto n = make(Node::Kind::literal);
            n->ch = c;
            n->icase = (options & case_insensitive) && (to_lower(c) != c || to_upper(c) != c);
            root->kids.push_back(std::move(n));
        }
    } else {
        Parser parser(code_points, options, program->sets);
        root = parser.parse();
        if (parser.status != Status::ok) { status = parser.status; message = parser.message; return std::nullopt; }
        program->captures = parser.groups;
        program->names = parser.names;
    }
    try {
        Instruction open; open.op = Op::save; open.a = 0;
        program->code.push_back(open);
        program->emit_node(*root);
        Instruction shut; shut.op = Op::save; shut.a = 1;
        program->code.push_back(shut);
        program->code.push_back({Op::match});
    } catch (const std::invalid_argument& e) {
        status = Status::invalid; message = e.what(); return std::nullopt;
    } catch (const std::length_error& e) {
        status = Status::unsupported; message = e.what(); return std::nullopt;
    }
    return Pattern(std::move(program));
}

std::size_t Pattern::capture_count() const { return static_cast<std::size_t>(program_->captures); }
std::optional<std::size_t> Pattern::group_named(std::u16string_view name) const {
    for (const auto& [n, index] : program_->names) if (n == name) return static_cast<std::size_t>(index);
    return std::nullopt;
}

std::optional<Match> Pattern::search(std::u16string_view text, Range range, std::int64_t from, std::uint64_t match_options) const {
    const auto& p = *program_;
    Program::Context c;
    c.text = text;
    c.lo = range.location;
    c.hi = range.location + range.length;
    if (c.lo < 0 || c.hi > static_cast<std::int64_t>(text.size()) || c.lo > c.hi) throw std::out_of_range("regular expression range beyond the string");
    const bool transparent = match_options & transparent_bounds;
    c.look_lo = transparent ? 0 : c.lo;
    c.look_hi = transparent ? static_cast<std::int64_t>(text.size()) : c.hi;
    const bool anchoring = !(match_options & without_anchoring_bounds);
    c.anchor_lo = anchoring ? c.lo : 0;
    c.anchor_hi = anchoring ? c.hi : static_cast<std::int64_t>(text.size());
    c.search_start = from;
    const auto slot_count = static_cast<std::size_t>(2 * (p.captures + 1) + p.marks);
    for (auto start = from; start <= c.hi; ++start) {
        std::vector<std::int64_t> slots(slot_count, -1);
        if (const auto end = p.run(c, 0, start, slots, -1, c.lo, c.hi)) {
            Match m;
            for (int g = 0; g <= p.captures; ++g) {
                const auto s = slots[static_cast<std::size_t>(2 * g)], t = slots[static_cast<std::size_t>(2 * g + 1)];
                m.groups.push_back(s >= 0 && t >= s ? Range{s, t - s} : Range{});
            }
            return m;
        }
        if (match_options & anchored) break;
        // Do not start inside a surrogate pair.
        if (start + 1 < c.hi) {
            const auto u = text[static_cast<std::size_t>(start)];
            const auto v = text[static_cast<std::size_t>(start + 1)];
            if (u >= 0xd800 && u <= 0xdbff && v >= 0xdc00 && v <= 0xdfff) ++start;
        }
    }
    return std::nullopt;
}

std::vector<Match> Pattern::all(std::u16string_view text, Range range, std::uint64_t match_options) const {
    std::vector<Match> out;
    auto from = range.location;
    const auto end = range.location + range.length;
    while (from <= end) {
        auto m = search(text, range, from, match_options);
        if (!m) break;
        const auto whole = m->groups[0];
        out.push_back(std::move(*m));
        if (match_options & anchored) {
            // Anchored: later matches must start where the previous ended.
            if (whole.length == 0) break;
            from = whole.location + whole.length;
            continue;
        }
        from = whole.length == 0 ? whole.location + 1 : whole.location + whole.length;
    }
    return out;
}

std::u16string escape_pattern(std::u16string_view value) {
    static constexpr std::u16string_view meta = u"\\*?+[](){}^$|./";
    std::u16string out;
    for (const auto c : value) {
        if (meta.find(c) != std::u16string_view::npos) out.push_back(u'\\');
        out.push_back(c);
    }
    return out;
}

std::u16string escape_template(std::u16string_view value) {
    std::u16string out;
    for (const auto c : value) {
        if (c == u'\\' || c == u'$') out.push_back(u'\\');
        out.push_back(c);
    }
    return out;
}

std::u16string expand_template(const Pattern& pattern, std::u16string_view value, const Match& match, std::u16string_view replacement) {
    std::u16string out;
    const auto group_text = [&](std::size_t g) {
        if (g >= match.groups.size() || match.groups[g].location < 0) return std::u16string_view{};
        return value.substr(static_cast<std::size_t>(match.groups[g].location), static_cast<std::size_t>(match.groups[g].length));
    };
    for (std::size_t i = 0; i < replacement.size(); ++i) {
        const auto c = replacement[i];
        if (c == u'\\' && i + 1 < replacement.size()) { out.push_back(replacement[++i]); continue; }
        if (c == u'$' && i + 1 < replacement.size()) {
            if (replacement[i + 1] == u'{') {
                const auto close = replacement.find(u'}', i + 2);
                if (close != std::u16string_view::npos) {
                    const auto name = replacement.substr(i + 2, close - i - 2);
                    if (const auto g = pattern.group_named(name)) { out += group_text(*g); i = close; continue; }
                }
            } else if (replacement[i + 1] >= u'0' && replacement[i + 1] <= u'9') {
                std::size_t g = static_cast<std::size_t>(replacement[++i] - u'0');
                while (i + 1 < replacement.size() && replacement[i + 1] >= u'0' && replacement[i + 1] <= u'9' &&
                       g * 10 + static_cast<std::size_t>(replacement[i + 1] - u'0') <= pattern.capture_count())
                    g = g * 10 + static_cast<std::size_t>(replacement[++i] - u'0');
                out += group_text(g);
                continue;
            }
        }
        out.push_back(c);
    }
    return out;
}

} // namespace alchemy::gui::completion_kit::regex
