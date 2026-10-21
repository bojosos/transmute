#include "alchemy/darwin/foundation/xml.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>

namespace alchemy::gui::completion_kit::xml {

std::string Node::local_name() const {
    const auto colon = name.find(':');
    return colon == std::string::npos ? name : name.substr(colon + 1);
}
std::string Node::prefix() const {
    const auto colon = name.find(':');
    return colon == std::string::npos ? std::string() : name.substr(0, colon);
}
std::string Node::string_value() const {
    if (kind != Kind::element && kind != Kind::document) return value;
    std::string out;
    for (const auto* c : children) if (c->kind == Kind::text || c->kind == Kind::element) out += c->string_value();
    return out;
}
Node* Node::attribute(std::string_view n) const {
    for (auto* a : attributes) if (a->name == n) return a;
    return nullptr;
}
std::size_t Node::index() const {
    if (!parent) return 0;
    const auto& list = kind == Kind::attribute ? parent->attributes : parent->children;
    return static_cast<std::size_t>(std::find(list.begin(), list.end(), this) - list.begin());
}
Node* Node::next_sibling() const {
    if (!parent || kind == Kind::attribute) return nullptr;
    const auto i = index();
    return i + 1 < parent->children.size() ? parent->children[i + 1] : nullptr;
}
Node* Node::previous_sibling() const {
    if (!parent || kind == Kind::attribute) return nullptr;
    const auto i = index();
    return i > 0 ? parent->children[i - 1] : nullptr;
}
Node* Document::root_element() const {
    if (!root()) return nullptr;
    for (auto* c : root()->children) if (c->kind == Kind::element) return c;
    return nullptr;
}

namespace {

struct Failure { std::string message; std::size_t at; };

void append_utf8(std::string& out, std::uint32_t c) {
    if (c < 0x80) out.push_back(static_cast<char>(c));
    else if (c < 0x800) { out.push_back(static_cast<char>(0xc0 | (c >> 6))); out.push_back(static_cast<char>(0x80 | (c & 0x3f))); }
    else if (c < 0x10000) { out.push_back(static_cast<char>(0xe0 | (c >> 12))); out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f))); out.push_back(static_cast<char>(0x80 | (c & 0x3f))); }
    else { out.push_back(static_cast<char>(0xf0 | (c >> 18))); out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3f))); out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f))); out.push_back(static_cast<char>(0x80 | (c & 0x3f))); }
}
bool name_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == ':' || static_cast<unsigned char>(c) >= 0x80; }
bool name_char(char c) { return name_start(c) || std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '.'; }
bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

class Parser {
public:
    Parser(std::string_view text, Document& doc, bool preserve) : t_(text), doc_(doc), preserve_(preserve) {}

    void parse() {
        if (t_.substr(0, 3) == "\xef\xbb\xbf") at_ = 3;
        auto* document = make(Kind::document, nullptr);
        if (t_.substr(at_, 5) == "<?xml" && at_ + 5 < t_.size() && space(t_[at_ + 5])) declaration();
        misc(document);
        if (at_ >= t_.size() || t_[at_] != '<') fail("no root element");
        element(document);
        misc(document);
        if (at_ < t_.size()) fail("content after the root element");
    }

private:
    std::string_view t_;
    std::size_t at_{};
    Document& doc_;
    bool preserve_;

    [[noreturn]] void fail(const std::string& why) { throw Failure{why, at_}; }
    Node* make(Kind kind, Node* parent) {
        doc_.nodes.push_back(std::make_unique<Node>());
        auto* n = doc_.nodes.back().get();
        n->kind = kind;
        n->parent = parent;
        n->order = doc_.nodes.size() - 1;
        return n;
    }
    void skip_space() { while (at_ < t_.size() && space(t_[at_])) ++at_; }
    bool starts(std::string_view s) const { return t_.substr(at_, s.size()) == s; }
    void expect(std::string_view s) { if (!starts(s)) fail("expected \"" + std::string(s) + "\""); at_ += s.size(); }
    std::string name() {
        if (at_ >= t_.size() || !name_start(t_[at_])) fail("expected a name");
        const auto start = at_;
        while (at_ < t_.size() && name_char(t_[at_])) ++at_;
        return std::string(t_.substr(start, at_ - start));
    }
    std::string decode(std::string_view raw) {
        std::string out;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] != '&') { out.push_back(raw[i]); continue; }
            const auto end = raw.find(';', i);
            if (end == std::string_view::npos) fail("unterminated entity reference");
            const auto entity = raw.substr(i + 1, end - i - 1);
            if (entity == "lt") out.push_back('<');
            else if (entity == "gt") out.push_back('>');
            else if (entity == "amp") out.push_back('&');
            else if (entity == "quot") out.push_back('"');
            else if (entity == "apos") out.push_back('\'');
            else if (!entity.empty() && entity[0] == '#') {
                const bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
                const std::string digits(entity.substr(hex ? 2 : 1));
                char* stop = nullptr;
                const auto code = std::strtoul(digits.c_str(), &stop, hex ? 16 : 10);
                if (digits.empty() || *stop || code == 0 || code > 0x10ffff) fail("bad character reference");
                append_utf8(out, static_cast<std::uint32_t>(code));
            } else fail("undefined entity &" + std::string(entity) + ";");
            i = end;
        }
        return out;
    }
    void declaration() {
        at_ += 5;
        const auto end = t_.find("?>", at_);
        if (end == std::string_view::npos) fail("unterminated XML declaration");
        const auto body = std::string(t_.substr(at_, end - at_));
        const auto pseudo = [&](const char* key) -> std::string {
            const auto k = body.find(key);
            if (k == std::string::npos) return {};
            auto q = body.find_first_of("\"'", k);
            if (q == std::string::npos) return {};
            const auto close = body.find(body[q], q + 1);
            return close == std::string::npos ? std::string() : body.substr(q + 1, close - q - 1);
        };
        if (const auto v = pseudo("version"); !v.empty()) doc_.version = v;
        doc_.encoding = pseudo("encoding");
        doc_.standalone = pseudo("standalone") == "yes";
        at_ = end + 2;
    }
    void misc(Node* parent) {
        for (;;) {
            skip_space();
            if (starts("<!--")) comment(parent);
            else if (starts("<?")) processing_instruction(parent);
            else if (starts("<!DOCTYPE")) doctype();
            else return;
        }
    }
    void doctype() {
        // Skipped, internal subset included (its entities are not provided).
        int depth = 0;
        for (; at_ < t_.size(); ++at_) {
            if (t_[at_] == '[') ++depth;
            else if (t_[at_] == ']') --depth;
            else if (t_[at_] == '>' && depth == 0) { ++at_; return; }
        }
        fail("unterminated DOCTYPE");
    }
    void comment(Node* parent) {
        at_ += 4;
        const auto end = t_.find("-->", at_);
        if (end == std::string_view::npos) fail("unterminated comment");
        auto* n = make(Kind::comment, parent);
        n->value = std::string(t_.substr(at_, end - at_));
        parent->children.push_back(n);
        at_ = end + 3;
    }
    void processing_instruction(Node* parent) {
        at_ += 2;
        auto* n = make(Kind::processing_instruction, parent);
        n->name = name();
        const auto end = t_.find("?>", at_);
        if (end == std::string_view::npos) fail("unterminated processing instruction");
        auto data = t_.substr(at_, end - at_);
        while (!data.empty() && space(data.front())) data.remove_prefix(1);
        n->value = std::string(data);
        parent->children.push_back(n);
        at_ = end + 2;
    }
    std::string resolve(Node* element, const std::string& prefix) {
        if (prefix == "xml") return "http://www.w3.org/XML/1998/namespace";
        for (auto* n = element; n; n = n->parent)
            for (const auto& [p, uri] : n->namespaces) if (p == prefix) return uri;
        if (!prefix.empty()) fail("undeclared namespace prefix \"" + prefix + "\"");
        return {};
    }
    void element(Node* parent) {
        expect("<");
        auto* n = make(Kind::element, parent);
        n->name = name();
        parent->children.push_back(n);
        for (;;) {
            const auto before = at_;
            skip_space();
            if (starts("/>") || starts(">")) break;
            if (at_ == before) fail("expected white space between attributes");
            const auto attribute_name = name();
            skip_space();
            expect("=");
            skip_space();
            if (at_ >= t_.size() || (t_[at_] != '"' && t_[at_] != '\'')) fail("expected a quoted attribute value");
            const auto quote = t_[at_++];
            const auto end = t_.find(quote, at_);
            if (end == std::string_view::npos) fail("unterminated attribute value");
            const auto raw = t_.substr(at_, end - at_);
            if (raw.find('<') != std::string_view::npos) fail("'<' in an attribute value");
            auto value = decode(raw);
            at_ = end + 1;
            if (attribute_name == "xmlns") { n->namespaces.emplace_back("", value); continue; }
            if (attribute_name.rfind("xmlns:", 0) == 0) { n->namespaces.emplace_back(attribute_name.substr(6), value); continue; }
            if (n->attribute(attribute_name)) fail("duplicate attribute " + attribute_name);
            auto* a = make(Kind::attribute, n);
            a->name = attribute_name;
            a->value = std::move(value);
            n->attributes.push_back(a);
        }
        n->uri = resolve(n, n->prefix());
        for (auto* a : n->attributes) if (!a->prefix().empty()) a->uri = resolve(n, a->prefix());
        if (starts("/>")) { at_ += 2; return; }
        expect(">");
        content(n);
        expect("</");
        const auto closing = name();
        if (closing != n->name) fail("mismatched end tag </" + closing + "> for <" + n->name + ">");
        skip_space();
        expect(">");
    }
    void content(Node* element_node) {
        std::string text;
        const auto flush = [&] {
            if (text.empty()) return;
            const bool blank = std::all_of(text.begin(), text.end(), space);
            if (!blank || preserve_) {
                auto* n = make(Kind::text, element_node);
                n->value = std::move(text);
                element_node->children.push_back(n);
            }
            text.clear();
        };
        for (;;) {
            if (at_ >= t_.size()) fail("unterminated element <" + element_node->name + ">");
            if (starts("</")) { flush(); return; }
            if (starts("<!--")) { flush(); comment(element_node); continue; }
            if (starts("<![CDATA[")) {
                at_ += 9;
                const auto end = t_.find("]]>", at_);
                if (end == std::string_view::npos) fail("unterminated CDATA section");
                text += t_.substr(at_, end - at_);
                at_ = end + 3;
                continue;
            }
            if (starts("<?")) { flush(); processing_instruction(element_node); continue; }
            if (t_[at_] == '<') { flush(); element(element_node); continue; }
            const auto end = std::min(t_.find('<', at_), t_.size());
            text += decode(t_.substr(at_, end - at_));
            at_ = end;
        }
    }
};

// ---- XPath subset ----
struct Step {
    enum class Axis { child, attribute, self, parent, descendant_or_self } axis{Axis::child};
    enum class Test { name, any, text, node } test{Test::name};
    std::string name;
    std::vector<std::string> predicates;
};

std::vector<Node*> descendants_or_self(Node* n) {
    std::vector<Node*> out{n};
    for (std::size_t i = 0; i < out.size(); ++i) for (auto* c : out[i]->children) out.push_back(c);
    return out;
}

bool matches(const Step& s, const Node* n) {
    switch (s.test) {
    case Step::Test::node: return true;
    case Step::Test::text: return n->kind == Kind::text;
    case Step::Test::any: return n->kind == (s.axis == Step::Axis::attribute ? Kind::attribute : Kind::element);
    case Step::Test::name:
        if (n->kind != (s.axis == Step::Axis::attribute ? Kind::attribute : Kind::element)) return false;
        return n->name == s.name;
    }
    return false;
}

std::string unquote(std::string_view v, Status& status) {
    if (v.size() >= 2 && (v.front() == '\'' || v.front() == '"') && v.back() == v.front()) return std::string(v.substr(1, v.size() - 2));
    status = Status::unsupported;
    return {};
}
std::string trim(std::string_view v) {
    while (!v.empty() && space(v.front())) v.remove_prefix(1);
    while (!v.empty() && space(v.back())) v.remove_suffix(1);
    return std::string(v);
}

std::vector<Node*> filter(std::vector<Node*> nodes, const std::string& predicate, Status& status, std::string& message) {
    const auto p = trim(predicate);
    if (!p.empty() && std::all_of(p.begin(), p.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        const auto n = std::strtoull(p.c_str(), nullptr, 10);
        if (n >= 1 && n <= nodes.size()) return {nodes[n - 1]};
        return {};
    }
    if (p == "last()") return nodes.empty() ? std::vector<Node*>{} : std::vector<Node*>{nodes.back()};
    const auto eq = p.find('=');
    const auto lhs = trim(p.substr(0, eq == std::string::npos ? p.size() : eq));
    const bool attribute = !lhs.empty() && lhs[0] == '@';
    const auto key = attribute ? lhs.substr(1) : lhs;
    if (key.empty() || !std::all_of(key.begin(), key.end(), [](char c) { return name_char(c); }) || (eq != std::string::npos && p[eq - 1] == '!')) {
        status = Status::unsupported;
        message = "XPath predicate [" + p + "]";
        return {};
    }
    std::optional<std::string> wanted;
    if (eq != std::string::npos) {
        wanted = unquote(trim(p.substr(eq + 1)), status);
        if (status != Status::ok) { message = "XPath predicate [" + p + "]"; return {}; }
    }
    std::vector<Node*> out;
    for (auto* n : nodes) {
        std::optional<std::string> value;
        if (attribute) { if (auto* a = n->attribute(key)) value = a->value; }
        else for (auto* c : n->children) if (c->kind == Kind::element && c->name == key) { value = c->string_value(); if (!wanted || *value == *wanted) break; }
        if (value && (!wanted || *value == *wanted)) out.push_back(n);
    }
    return out;
}

} // namespace

std::shared_ptr<Document> parse(std::string_view text, ParseError& error, bool preserve_whitespace) {
    auto doc = std::make_shared<Document>();
    try {
        Parser(text, *doc, preserve_whitespace).parse();
    } catch (const Failure& f) {
        error.message = f.message;
        error.line = 1 + static_cast<std::size_t>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(std::min(f.at, text.size())), '\n'));
        const auto line_start = text.rfind('\n', f.at == 0 ? 0 : f.at - 1);
        error.column = f.at - (line_start == std::string_view::npos ? 0 : line_start + 1) + 1;
        return nullptr;
    }
    return doc;
}

std::vector<Node*> select(Node* context, std::string_view path, Status& status, std::string& message) {
    status = Status::ok;
    message.clear();
    if (!context) return {};
    // Split into steps; "//" adds a descendant-or-self step.
    std::vector<Step> steps;
    std::size_t at = 0;
    Node* start = context;
    if (!path.empty() && path[0] == '/') { while (start->parent) start = start->parent; }
    const auto unsupported = [&](const std::string& what) { status = Status::unsupported; message = "XPath " + what + " in \"" + std::string(path) + "\""; };
    while (at < path.size()) {
        if (path[at] == '/') {
            if (at + 1 < path.size() && path[at + 1] == '/') { Step d; d.axis = Step::Axis::descendant_or_self; d.test = Step::Test::node; steps.push_back(d); at += 2; }
            else ++at;
            continue;
        }
        // One step up to the next '/' outside brackets and quotes.
        std::size_t end = at;
        int depth = 0;
        char quote = 0;
        for (; end < path.size(); ++end) {
            const auto c = path[end];
            if (quote) { if (c == quote) quote = 0; continue; }
            if (c == '\'' || c == '"') quote = c;
            else if (c == '[') ++depth;
            else if (c == ']') --depth;
            else if (c == '/' && depth == 0) break;
        }
        auto text = trim(path.substr(at, end - at));
        at = end;
        Step s;
        while (!text.empty() && text.back() == ']') {
            const auto open = text.rfind('[');
            if (open == std::string::npos) { status = Status::invalid; message = "unbalanced '[' in XPath \"" + std::string(path) + "\""; return {}; }
            s.predicates.insert(s.predicates.begin(), text.substr(open + 1, text.size() - open - 2));
            text = trim(std::string_view(text).substr(0, open));
        }
        if (text.find("::") != std::string::npos || text.find('|') != std::string::npos || text.find('(') != std::string::npos && text != "text()" && text != "node()") {
            unsupported("step \"" + text + "\"");
            return {};
        }
        if (text == ".") { s.axis = Step::Axis::self; s.test = Step::Test::node; }
        else if (text == "..") { s.axis = Step::Axis::parent; s.test = Step::Test::node; }
        else if (text == "text()") s.test = Step::Test::text;
        else if (text == "node()") s.test = Step::Test::node;
        else {
            if (!text.empty() && text[0] == '@') { s.axis = Step::Axis::attribute; text.erase(0, 1); }
            if (text == "*") s.test = Step::Test::any;
            else {
                if (text.empty() || !std::all_of(text.begin(), text.end(), [](char c) { return name_char(c); })) { status = Status::invalid; message = "bad XPath step in \"" + std::string(path) + "\""; return {}; }
                s.name = text;
            }
        }
        steps.push_back(std::move(s));
    }
    std::vector<Node*> current{start};
    for (const auto& s : steps) {
        std::vector<Node*> next;
        for (auto* n : current) {
            std::vector<Node*> found;
            switch (s.axis) {
            case Step::Axis::self: found = {n}; break;
            case Step::Axis::parent: if (n->parent) found = {n->parent}; break;
            case Step::Axis::descendant_or_self: found = descendants_or_self(n); break;
            case Step::Axis::attribute: for (auto* a : n->attributes) if (matches(s, a)) found.push_back(a); break;
            case Step::Axis::child: for (auto* c : n->children) if (matches(s, c)) found.push_back(c); break;
            }
            for (const auto& p : s.predicates) {
                found = filter(std::move(found), p, status, message);
                if (status != Status::ok) return {};
            }
            next.insert(next.end(), found.begin(), found.end());
        }
        std::sort(next.begin(), next.end(), [](const Node* a, const Node* b) { return a->order < b->order; });
        next.erase(std::unique(next.begin(), next.end()), next.end());
        current = std::move(next);
    }
    return current;
}

std::string serialize(const Node& n) {
    const auto escape = [](const std::string& v, bool attribute) {
        std::string out;
        for (const auto c : v) {
            if (c == '<') out += "&lt;";
            else if (c == '>') out += "&gt;";
            else if (c == '&') out += "&amp;";
            else if (c == '"' && attribute) out += "&quot;";
            else out.push_back(c);
        }
        return out;
    };
    switch (n.kind) {
    case Kind::text: return escape(n.value, false);
    case Kind::comment: return "<!--" + n.value + "-->";
    case Kind::processing_instruction: return "<?" + n.name + (n.value.empty() ? "" : " " + n.value) + "?>";
    case Kind::attribute: return n.name + "=\"" + escape(n.value, true) + "\"";
    case Kind::document: {
        std::string out;   // the XML declaration is the document object's (-XMLString adds it)
        for (const auto* c : n.children) out += serialize(*c);
        return out;
    }
    case Kind::element: {
        std::string out = "<" + n.name;
        for (const auto& [prefix, uri] : n.namespaces) out += " xmlns" + (prefix.empty() ? std::string() : ":" + prefix) + "=\"" + escape(uri, true) + "\"";
        for (const auto* a : n.attributes) out += " " + serialize(*a);
        if (n.children.empty()) return out + "/>";
        out += ">";
        for (const auto* c : n.children) out += serialize(*c);
        return out + "</" + n.name + ">";
    }
    default: return {};
    }
}

} // namespace alchemy::gui::completion_kit::xml
