#include "alchemy/darwin/kit/completion/data/providers.hpp"
#include "alchemy/darwin/foundation/xml.hpp"

#include <cctype>

// NSXMLDocument / NSXMLElement / NSXMLNode, read-only, over the parser and
// XPath subset of xml.cpp (Sparkle reads appcasts with them). Documents are
// UTF-8, US-ASCII or ISO-8859-1; UTF-16 and other declared encodings, and the
// HTML / XML tidying options, fail explicitly. Parse errors give nil and an
// NSXMLParserErrorDomain error. Node objects are made on demand (equal
// nodes are -isEqual:, not necessarily identical). The mutation API
// (-addChild:, -setStringValue:, ...) is not provided.
namespace alchemy::gui::completion_kit::data {
namespace {

constexpr std::uint64_t preserve_whitespace = 1u << 25, tidy_html = 1u << 9, tidy_xml = 1u << 10;

struct XmlState final : ObjectPayload {
    std::shared_ptr<xml::Document> doc;
    xml::Node* node{};
};

struct Xml {
    Support& s;
    Guest g{s};
    Address node_class{}, element_class{}, document_class{};

    std::shared_ptr<XmlState> state(Address self) {
        auto st = s.objc.find_host_state<XmlState>(self);
        if (!st || !st->node) fail("NSXMLNode without a node (the mutable XML API is not provided)");
        return st;
    }
    Address wrap(const std::shared_ptr<xml::Document>& doc, xml::Node* node) {
        if (!node) return 0;
        const auto cls = node->kind == xml::Kind::element ? element_class : node->kind == xml::Kind::document ? document_class : node_class;
        const auto made = s.objc.allocate(cls);
        auto st = s.objc.host_state<XmlState>(made);
        st->doc = doc;
        st->node = node;
        return g.autorelease(made);
    }
    Address string(const std::string& utf8) { return g.auto_string(std::string_view(utf8)); }
    Address node_list(const std::shared_ptr<xml::Document>& doc, const std::vector<xml::Node*>& nodes) {
        std::vector<Address> objects;
        for (auto* n : nodes) objects.push_back(wrap(doc, n));
        return g.auto_array(objects);
    }
    std::string text_of(std::vector<std::uint8_t> bytes, Address error_out, bool& ok) {
        ok = true;
        if ((bytes.size() >= 2 && ((bytes[0] == 0xfe && bytes[1] == 0xff) || (bytes[0] == 0xff && bytes[1] == 0xfe))) ||
            (bytes.size() >= 2 && (bytes[0] == 0 || bytes[1] == 0)))
            unsupported("NSXMLDocument with UTF-16 data");
        std::string text(bytes.begin(), bytes.end());
        // The declared encoding.
        const auto declaration_end = text.find("?>");
        if (text.rfind("<?xml", 0) == 0 && declaration_end != std::string::npos) {
            auto declaration = text.substr(0, declaration_end);
            for (auto& c : declaration) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const auto key = declaration.find("encoding");
            if (key != std::string::npos) {
                const auto q = declaration.find_first_of("\"'", key);
                const auto close = q == std::string::npos ? std::string::npos : declaration.find(declaration[q], q + 1);
                const auto encoding = close == std::string::npos ? std::string() : declaration.substr(q + 1, close - q - 1);
                if (encoding == "iso-8859-1" || encoding == "latin1" || encoding == "iso_8859-1") {
                    std::string utf8;
                    for (const auto b : bytes) {
                        if (b < 0x80) utf8.push_back(static_cast<char>(b));
                        else { utf8.push_back(static_cast<char>(0xc0 | (b >> 6))); utf8.push_back(static_cast<char>(0x80 | (b & 0x3f))); }
                    }
                    return utf8;
                }
                if (!encoding.empty() && encoding != "utf-8" && encoding != "utf8" && encoding != "us-ascii" && encoding != "ascii")
                    unsupported("NSXMLDocument encoding \"" + encoding + "\"");
            }
        }
        static_cast<void>(error_out);
        return text;
    }
    // +1 document object, or 0 with the error stored.
    Address document(Address self, const std::string& text, std::uint64_t options, Address error_out) {
        g.set_error(error_out, 0);
        if (options & (tidy_html | tidy_xml)) unsupported("NSXMLDocument tidying options (NSXMLDocumentTidyHTML / NSXMLDocumentTidyXML)");
        xml::ParseError error;
        auto doc = xml::parse(text, error, (options & preserve_whitespace) != 0);
        if (!doc) {
            g.set_error(error_out, g.error("NSXMLParserErrorDomain", 1, "line " + std::to_string(error.line) + " column " + std::to_string(error.column) + ": " + error.message));
            g.release(self);
            return 0;
        }
        auto st = s.objc.host_state<XmlState>(self);
        st->doc = doc;
        st->node = doc->root();
        return self;
    }
};

} // namespace

void install_xml(Support& support) {
    auto xm = std::make_shared<Xml>(Xml{support});
    const auto nn = support.declare_class("NSXMLNode", "NSObject", foundation_library);
    const auto ne = support.declare_class("NSXMLElement", "NSXMLNode", foundation_library);
    const auto nd = support.declare_class("NSXMLDocument", "NSXMLNode", foundation_library);
    xm->node_class = nn;
    xm->element_class = ne;
    xm->document_class = nd;

    // ---- NSXMLDocument ----
    support.method(nd, "initWithData:options:error:", [xm](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("NSInvalidArgumentException: -[NSXMLDocument initWithData:options:error:]: nil data");
        const auto length = xm->s.send(s.x[2], "length").x0;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        if (length) xm->g.memory().read_bytes(xm->s.send(s.x[2], "bytes").x0, bytes);
        bool ok = false;
        const auto text = xm->text_of(std::move(bytes), s.x[4], ok);
        s.x[0] = xm->document(s.x[0], text, s.x[3], s.x[4]);
    }, "@@:@Q^@");
    support.method(nd, "initWithXMLString:options:error:", [xm](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("NSInvalidArgumentException: -[NSXMLDocument initWithXMLString:options:error:]: nil string");
        s.x[0] = xm->document(s.x[0], xm->g.utf8(s.x[2]), s.x[3], s.x[4]);
    }, "@@:@Q^@");
    support.method(nd, "initWithContentsOfURL:options:error:", [xm](CpuState& s, GuestMemory&) {
        CallRegs r;
        r.x[2] = s.x[2];
        r.x[3] = 0;
        r.x[4] = s.x[4];
        const auto data = xm->s.send(xm->s.objc.find_class("NSData"), "dataWithContentsOfURL:options:error:", r).x0;
        if (!data) { xm->g.release(s.x[0]); s.x[0] = 0; return; }
        CallRegs d;
        d.x[2] = data;
        d.x[3] = s.x[3];
        d.x[4] = s.x[4];
        s.x[0] = xm->s.send(s.x[0], "initWithData:options:error:", d).x0;
    }, "@@:@Q^@");
    support.method(nd, "rootElement", [xm](CpuState& s, GuestMemory&) {
        const auto st = xm->state(s.x[0]);
        s.x[0] = xm->wrap(st->doc, st->doc->root_element());
    }, "@@:");
    support.method(nd, "version", [xm](CpuState& s, GuestMemory&) { s.x[0] = xm->string(xm->state(s.x[0])->doc->version); }, "@@:");
    support.method(nd, "characterEncoding", [xm](CpuState& s, GuestMemory&) {
        const auto& e = xm->state(s.x[0])->doc->encoding;
        s.x[0] = e.empty() ? 0 : xm->string(e);
    }, "@@:");
    support.method(nd, "isStandalone", [xm](CpuState& s, GuestMemory&) { return_bool(s, xm->state(s.x[0])->doc->standalone); }, "B@:");
    support.method(nd, "XMLData", [xm](CpuState& s, GuestMemory&) {
        const auto text = xm->g.utf8(xm->s.send_id(s.x[0], "XMLString"));
        Guest::Scratch block(xm->g, std::max<std::size_t>(text.size(), 1));
        xm->g.memory().write_bytes(block.address, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
        CallRegs r;
        r.x[2] = block.address;
        r.x[3] = text.size();
        s.x[0] = xm->s.send(xm->s.objc.find_class("NSData"), "dataWithBytes:length:", r).x0;
    }, "@@:");

    // ---- NSXMLNode ----
    support.method(nn, "kind", [xm](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(xm->state(s.x[0])->node->kind); }, "Q@:");
    support.method(nn, "name", [xm](CpuState& s, GuestMemory&) {
        const auto* n = xm->state(s.x[0])->node;
        s.x[0] = n->name.empty() ? 0 : xm->string(n->name);
    }, "@@:");
    support.method(nn, "localName", [xm](CpuState& s, GuestMemory&) {
        const auto* n = xm->state(s.x[0])->node;
        s.x[0] = n->name.empty() ? 0 : xm->string(n->local_name());
    }, "@@:");
    support.method(nn, "prefix", [xm](CpuState& s, GuestMemory&) { s.x[0] = xm->string(xm->state(s.x[0])->node->prefix()); }, "@@:");
    support.method(nn, "URI", [xm](CpuState& s, GuestMemory&) {
        const auto* n = xm->state(s.x[0])->node;
        s.x[0] = n->uri.empty() ? 0 : xm->string(n->uri);
    }, "@@:");
    for (const auto* name : {"stringValue", "objectValue"})
        support.method(nn, name, [xm](CpuState& s, GuestMemory&) { s.x[0] = xm->string(xm->state(s.x[0])->node->string_value()); }, "@@:");
    support.method(nn, "children", [xm](CpuState& s, GuestMemory&) {
        const auto st = xm->state(s.x[0]);
        s.x[0] = st->node->children.empty() ? 0 : xm->node_list(st->doc, st->node->children);
    }, "@@:");
    support.method(nn, "childCount", [xm](CpuState& s, GuestMemory&) { s.x[0] = xm->state(s.x[0])->node->children.size(); }, "Q@:");
    support.method(nn, "childAtIndex:", [xm](CpuState& s, GuestMemory&) {
        const auto st = xm->state(s.x[0]);
        if (s.x[2] >= st->node->children.size()) fail("NSRangeException: -[NSXMLNode childAtIndex:]: index " + std::to_string(s.x[2]) + " beyond bounds " + std::to_string(st->node->children.size()));
        s.x[0] = xm->wrap(st->doc, st->node->children[static_cast<std::size_t>(s.x[2])]);
    }, "@@:Q");
    support.method(nn, "parent", [xm](CpuState& s, GuestMemory&) { const auto st = xm->state(s.x[0]); s.x[0] = xm->wrap(st->doc, st->node->parent); }, "@@:");
    support.method(nn, "nextSibling", [xm](CpuState& s, GuestMemory&) { const auto st = xm->state(s.x[0]); s.x[0] = xm->wrap(st->doc, st->node->next_sibling()); }, "@@:");
    support.method(nn, "previousSibling", [xm](CpuState& s, GuestMemory&) { const auto st = xm->state(s.x[0]); s.x[0] = xm->wrap(st->doc, st->node->previous_sibling()); }, "@@:");
    support.method(nn, "nextNode", [xm](CpuState& s, GuestMemory&) {
        // Document order: the first child, else the next sibling of the node or an ancestor.
        const auto st = xm->state(s.x[0]);
        auto* n = st->node;
        xml::Node* next = nullptr;
        if (n->kind != xml::Kind::attribute && !n->children.empty()) next = n->children.front();
        for (auto* up = n; !next && up; up = up->parent) next = up->next_sibling();
        s.x[0] = xm->wrap(st->doc, next);
    }, "@@:");
    support.method(nn, "index", [xm](CpuState& s, GuestMemory&) { s.x[0] = xm->state(s.x[0])->node->index(); }, "Q@:");
    support.method(nn, "level", [xm](CpuState& s, GuestMemory&) {
        Address level = 0;
        for (auto* n = xm->state(s.x[0])->node->parent; n; n = n->parent) ++level;
        s.x[0] = level;
    }, "Q@:");
    support.method(nn, "rootDocument", [xm](CpuState& s, GuestMemory&) { const auto st = xm->state(s.x[0]); s.x[0] = xm->wrap(st->doc, st->doc->root()); }, "@@:");
    support.method(nn, "XMLString", [xm](CpuState& s, GuestMemory&) {
        const auto st = xm->state(s.x[0]);
        auto text = xml::serialize(*st->node);
        if (st->node->kind == xml::Kind::document)
            text = "<?xml version=\"" + st->doc->version + "\"" + (st->doc->encoding.empty() ? "" : " encoding=\"" + st->doc->encoding + "\"") +
                   (st->doc->standalone ? " standalone=\"yes\"" : "") + "?>" + text;
        s.x[0] = xm->string(text);
    }, "@@:");
    support.method(nn, "description", [xm](CpuState& s, GuestMemory&) { s.x[0] = xm->s.send_id(s.x[0], "XMLString"); }, "@@:");
    support.method(nn, "nodesForXPath:error:", [xm](CpuState& s, GuestMemory&) {
        xm->g.set_error(s.x[3], 0);
        const auto st = xm->state(s.x[0]);
        xml::Status status;
        std::string message;
        const auto found = xml::select(st->node, xm->g.utf8(s.x[2]), status, message);
        if (status == xml::Status::unsupported) unsupported("NSXMLNode " + message);
        if (status == xml::Status::invalid) {
            xm->g.set_error(s.x[3], xm->g.error("NSXMLParserErrorDomain", 1, message));
            s.x[0] = 0;
            return;
        }
        s.x[0] = xm->node_list(st->doc, found);
    }, "@@:@^@");
    support.method(nn, "isEqual:", [xm](CpuState& s, GuestMemory&) {
        const auto other = s.x[2] ? xm->s.objc.find_host_state<XmlState>(s.x[2]) : nullptr;
        return_bool(s, s.x[0] == s.x[2] || (other && other->node == xm->state(s.x[0])->node));
    }, "B@:@");
    support.method(nn, "hash", [xm](CpuState& s, GuestMemory&) { s.x[0] = reinterpret_cast<std::uintptr_t>(xm->state(s.x[0])->node) >> 4; }, "Q@:");
    support.method(nn, "copyWithZone:", [xm](CpuState& s, GuestMemory&) { s.x[0] = xm->g.retain(s.x[0]); }, "@@:^v");   // read-only nodes

    // ---- NSXMLElement ----
    support.method(ne, "attributeForName:", [xm](CpuState& s, GuestMemory&) {
        const auto st = xm->state(s.x[0]);
        s.x[0] = xm->wrap(st->doc, st->node->attribute(xm->g.utf8(s.x[2])));
    }, "@@:@");
    support.method(ne, "attributeForLocalName:URI:", [xm](CpuState& s, GuestMemory&) {
        const auto st = xm->state(s.x[0]);
        const auto local = xm->g.utf8(s.x[2]), uri = xm->g.utf8(s.x[3]);
        xml::Node* found = nullptr;
        for (auto* a : st->node->attributes) if (a->local_name() == local && a->uri == uri) { found = a; break; }
        s.x[0] = xm->wrap(st->doc, found);
    }, "@@:@@");
    support.method(ne, "attributes", [xm](CpuState& s, GuestMemory&) {
        const auto st = xm->state(s.x[0]);
        s.x[0] = st->node->attributes.empty() ? 0 : xm->node_list(st->doc, st->node->attributes);
    }, "@@:");
    support.method(ne, "elementsForName:", [xm](CpuState& s, GuestMemory&) {
        const auto st = xm->state(s.x[0]);
        const auto name = xm->g.utf8(s.x[2]);
        std::vector<xml::Node*> found;
        for (auto* c : st->node->children) if (c->kind == xml::Kind::element && c->name == name) found.push_back(c);
        s.x[0] = xm->node_list(st->doc, found);
    }, "@@:@");
    support.method(ne, "elementsForLocalName:URI:", [xm](CpuState& s, GuestMemory&) {
        const auto st = xm->state(s.x[0]);
        const auto local = xm->g.utf8(s.x[2]), uri = xm->g.utf8(s.x[3]);
        std::vector<xml::Node*> found;
        for (auto* c : st->node->children) if (c->kind == xml::Kind::element && c->local_name() == local && c->uri == uri) found.push_back(c);
        s.x[0] = xm->node_list(st->doc, found);
    }, "@@:@@");
    support.method(ne, "namespaces", [xm](CpuState& s, GuestMemory&) {
        const auto st = xm->state(s.x[0]);
        if (st->node->namespaces.empty()) { s.x[0] = 0; return; }
        unsupported("-[NSXMLElement namespaces] (namespace nodes are not provided)");
    }, "@@:");
}

} // namespace alchemy::gui::completion_kit::data
