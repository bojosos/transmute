#pragma once

// NSXMLDocument's tree (host-only, UTF-8): a non-validating XML 1.0 parser
// (elements, attributes, character data, CDATA, comments, processing
// instructions, the five predefined entities and character references,
// namespace prefixes resolved to URIs; a DOCTYPE is skipped, so custom
// entities are an error) and an XPath subset for -nodesForXPath:error:.
// XPath: absolute and relative location paths with '/' and '//', name
// tests (name, prefix:name, *), '.', '..', '@attribute', '@*', text(),
// node(), and predicates [n], [last()], [@a], [@a='v'], [name='v'].
// Other XPath (functions, axes::, unions, arithmetic) is Status::unsupported.
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui::completion_kit::xml {

// NSXMLNodeKind values.
enum class Kind { invalid = 0, document = 1, element = 2, attribute = 3, namespace_ = 4, processing_instruction = 5, comment = 6, text = 7 };

struct Node {
    Kind kind{Kind::invalid};
    std::string name;          // qualified name (elements, attributes, PI targets)
    std::string value;         // text, comment, PI data, attribute value
    std::string uri;           // namespace URI of the name (elements, attributes)
    Node* parent{};
    std::size_t order{};       // document order
    std::vector<Node*> children;     // elements, text, comments, PIs
    std::vector<Node*> attributes;   // elements (xmlns declarations are kept out)
    std::vector<std::pair<std::string, std::string>> namespaces;   // prefix ("" default) -> URI declared here

    [[nodiscard]] std::string local_name() const;
    [[nodiscard]] std::string prefix() const;
    [[nodiscard]] std::string string_value() const;   // text content (descendants for elements)
    [[nodiscard]] Node* attribute(std::string_view name) const;
    [[nodiscard]] std::size_t index() const;          // among the parent's children (attributes: among attributes)
    [[nodiscard]] Node* next_sibling() const;
    [[nodiscard]] Node* previous_sibling() const;
};

struct Document {
    std::vector<std::unique_ptr<Node>> nodes;   // owner of every node; nodes[0] is the document
    std::string version{"1.0"}, encoding;
    bool standalone{};
    [[nodiscard]] Node* root() const { return nodes.empty() ? nullptr : nodes.front().get(); }
    [[nodiscard]] Node* root_element() const;
};

struct ParseError { std::string message; std::size_t line{}, column{}; };
// Whitespace-only text between markup is dropped unless `preserve_whitespace`
// (NSXMLNodePreserveWhitespace).
[[nodiscard]] std::shared_ptr<Document> parse(std::string_view text, ParseError& error, bool preserve_whitespace = false);

enum class Status { ok, invalid, unsupported };
[[nodiscard]] std::vector<Node*> select(Node* context, std::string_view path, Status& status, std::string& message);

// -XMLString of a node (compact, no pretty printing).
[[nodiscard]] std::string serialize(const Node& node);

} // namespace alchemy::gui::completion_kit::xml
