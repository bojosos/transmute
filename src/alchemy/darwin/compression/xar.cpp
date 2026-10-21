#include "alchemy/darwin/compression/xar.hpp"
#include "alchemy/darwin/compression/bzip2.hpp"
#include "alchemy/darwin/foundation/xml.hpp"
#include "alchemy/darwin/compression/zlib_codec.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace alchemy::gui::completion_kit::xar {
namespace {

constexpr std::uint32_t magic = 0x78617221;   // "xar!"
constexpr std::uint16_t header_size = 28;

std::uint64_t big(std::span<const std::uint8_t> b, std::size_t at, int n) {
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | b[at + static_cast<std::size_t>(i)];
    return v;
}
void put_big(std::vector<std::uint8_t>& out, std::uint64_t v, int n) {
    for (int i = n - 1; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
const std::string* find(const Properties& p, std::string_view key) {
    for (const auto& [k, v] : p) if (k == key) return &v;
    return nullptr;
}
void set_in(Properties& p, std::string_view key, std::string value) {
    for (auto& [k, v] : p) if (k == key) { v = std::move(value); return; }
    p.emplace_back(std::string(key), std::move(value));
}
std::string escape(std::string_view v) {
    std::string out;
    for (const auto c : v) {
        if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '&') out += "&amp;";
        else if (c == '"') out += "&quot;";
        else out.push_back(c);
    }
    return out;
}
std::string hex(std::span<const std::uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (const auto b : bytes) { out.push_back(digits[b >> 4]); out.push_back(digits[b & 15]); }
    return out;
}

// Child elements of `element` (except nested files) become properties.
void flatten(const xml::Node* element, const std::string& prefix, Properties& properties, Attributes& attributes) {
    for (const auto* c : element->children) {
        if (c->kind != xml::Kind::element || c->name == "file") continue;
        const auto key = prefix + c->name;
        if (!c->attributes.empty()) {
            Properties values;
            for (const auto* a : c->attributes) values.emplace_back(a->name, a->value);
            attributes.emplace_back(key, std::move(values));
        }
        const bool nested = std::any_of(c->children.begin(), c->children.end(), [](const xml::Node* n) { return n->kind == xml::Kind::element; });
        set_in(properties, key, nested ? std::string() : c->string_value());
        if (nested) flatten(c, key + "/", properties, attributes);
    }
}

void read_files(const xml::Node* element, Node* parent, std::vector<std::unique_ptr<Node>>& into) {
    for (const auto* c : element->children) {
        if (c->kind != xml::Kind::element || c->name != "file") continue;
        auto node = std::make_unique<Node>();
        node->parent = parent;
        if (const auto* id = c->attribute("id")) node->id = std::strtoull(id->value.c_str(), nullptr, 10);
        flatten(c, "", node->properties, node->attributes);
        read_files(c, node.get(), node->children);
        into.push_back(std::move(node));
    }
}

// Properties as nested elements: "a/b" opens <a><b>.
void emit_properties(std::string& out, const Properties& properties, bool skip_data) {
    std::vector<std::string> open;
    const auto close_to = [&](std::size_t depth) {
        while (open.size() > depth) { out += "</" + open.back() + ">"; open.pop_back(); }
    };
    for (const auto& [key, value] : properties) {
        if (skip_data && (key == "data" || key.rfind("data/", 0) == 0)) continue;
        std::vector<std::string> parts;
        for (std::size_t at = 0; at <= key.size();) {
            const auto slash = std::min(key.find('/', at), key.size());
            parts.push_back(key.substr(at, slash - at));
            at = slash + 1;
        }
        std::size_t common = 0;
        while (common < open.size() && common + 1 < parts.size() && open[common] == parts[common]) ++common;
        close_to(common);
        for (std::size_t i = common; i + 1 < parts.size(); ++i) { out += "<" + parts[i] + ">"; open.push_back(parts[i]); }
        out += "<" + parts.back() + ">" + escape(value) + "</" + parts.back() + ">";
    }
    close_to(0);
}

} // namespace

const std::string* Node::property(std::string_view key) const { return find(properties, key); }
void Node::set(std::string_view key, std::string value) { set_in(properties, key, std::move(value)); }
const std::string* Node::attribute(std::string_view key, std::string_view name) const {
    for (const auto& [k, values] : attributes) if (k == key) return find(values, name);
    return nullptr;
}
std::string Node::path() const {
    const auto* name = property("name");
    std::string own = name ? *name : std::string();
    return parent ? parent->path() + "/" + own : own;
}
const std::string* Subdoc::property(std::string_view key) const { return find(properties, key); }
void Subdoc::set(std::string_view key, std::string value) { set_in(properties, key, std::move(value)); }

std::vector<Node*> Archive::all() const {
    std::vector<Node*> out;
    const std::function<void(const std::vector<std::unique_ptr<Node>>&)> walk = [&](const std::vector<std::unique_ptr<Node>>& list) {
        for (const auto& n : list) { out.push_back(n.get()); walk(n->children); }
    };
    walk(files);
    return out;
}

Node& Archive::add(Node* parent, std::string name) {
    auto node = std::make_unique<Node>();
    node->parent = parent;
    node->id = next_id++;
    node->set("name", std::move(name));
    auto& list = parent ? parent->children : files;
    list.push_back(std::move(node));
    return *list.back();
}

std::unique_ptr<Archive> read(std::span<const std::uint8_t> bytes, std::string& error) {
    if (bytes.size() < header_size || big(bytes, 0, 4) != magic) { error = "not a xar archive"; return nullptr; }
    const auto size = static_cast<std::size_t>(big(bytes, 4, 2));
    const auto compressed = big(bytes, 8, 8), uncompressed = big(bytes, 16, 8);
    if (size < header_size || size + compressed > bytes.size() || uncompressed > (1u << 30)) { error = "a truncated xar header or table of contents"; return nullptr; }
    const auto toc = export_fill::zlib_decompress(bytes.subspan(size, static_cast<std::size_t>(compressed)), static_cast<std::size_t>(uncompressed));
    if (toc.status != export_fill::InflateStatus::ok) { error = "the table of contents does not decompress"; return nullptr; }
    xml::ParseError parse_error;
    const auto doc = xml::parse(std::string_view(reinterpret_cast<const char*>(toc.output.data()), toc.output.size()), parse_error);
    const auto* root = doc ? doc->root_element() : nullptr;
    if (!root || root->name != "xar") { error = "the table of contents is not a <xar> document" + (doc ? std::string() : ": " + parse_error.message); return nullptr; }
    auto archive = std::make_unique<Archive>();
    for (const auto* c : root->children) {
        if (c->kind != xml::Kind::element) continue;
        if (c->name == "subdoc") {
            auto sub = std::make_unique<Subdoc>();
            if (const auto* name = c->attribute("subdoc_name")) sub->name = name->value;
            Attributes ignored;
            flatten(c, "", sub->properties, ignored);
            archive->subdocs.push_back(std::move(sub));
        } else if (c->name == "toc") {
            read_files(c, nullptr, archive->files);
            for (const auto* s : c->children)
                if (s->kind == xml::Kind::element && s->name == "subdoc") {
                    auto sub = std::make_unique<Subdoc>();
                    if (const auto* name = s->attribute("subdoc_name")) sub->name = name->value;
                    Attributes ignored;
                    flatten(s, "", sub->properties, ignored);
                    archive->subdocs.push_back(std::move(sub));
                }
        }
    }
    archive->heap.assign(bytes.begin() + static_cast<std::ptrdiff_t>(size + compressed), bytes.end());
    for (const auto* n : archive->all()) archive->next_id = std::max(archive->next_id, n->id + 1);
    return archive;
}

std::optional<std::vector<std::uint8_t>> extract(const Archive& archive, const Node& node, std::string& error, bool& unsupported) {
    unsupported = false;
    if (node.contents) return *node.contents;
    const auto* offset = node.property("data/offset");
    const auto* length = node.property("data/length");
    if (!offset || !length) return std::vector<std::uint8_t>{};   // no data: an empty file
    const auto at = std::strtoull(offset->c_str(), nullptr, 10), n = std::strtoull(length->c_str(), nullptr, 10);
    if (at + n > archive.heap.size()) { error = "file data beyond the heap"; return std::nullopt; }
    const auto raw = std::span(archive.heap).subspan(static_cast<std::size_t>(at), static_cast<std::size_t>(n));
    const auto* style = node.attribute("data/encoding", "style");
    const std::string encoding = style ? *style : "application/octet-stream";
    const auto* size_text = node.property("data/size");
    const auto expected = size_text ? std::strtoull(size_text->c_str(), nullptr, 10) : 0;
    if (encoding == "application/octet-stream") return std::vector<std::uint8_t>(raw.begin(), raw.end());
    if (encoding == "application/x-gzip") {
        auto r = export_fill::zlib_decompress(raw, static_cast<std::size_t>(std::max<std::uint64_t>(expected, 1)) + (1u << 20));
        if (r.status != export_fill::InflateStatus::ok) { error = "file data does not decompress (zlib)"; return std::nullopt; }
        return std::move(r.output);
    }
    if (encoding == "application/x-bzip2") {
        auto r = bzip2::decode(raw);
        if (r.error != bzip2::Error::none) { error = "file data does not decompress (bzip2): " + r.message; unsupported = r.error == bzip2::Error::unsupported; return std::nullopt; }
        return std::move(r.output);
    }
    error = "file data encoding " + encoding + " is not provided";
    unsupported = true;
    return std::nullopt;
}

std::vector<std::uint8_t> write(Archive& archive, const Digest& sha1) {
    std::vector<std::uint8_t> heap(20, 0);   // the TOC checksum comes first
    std::string files_xml;
    const std::function<void(const Node&)> emit_file = [&](const Node& n) {
        files_xml += "<file id=\"" + std::to_string(n.id) + "\">";
        emit_properties(files_xml, n.properties, n.contents.has_value());
        if (n.contents) {
            std::vector<std::uint8_t> stored;
            std::string encoding = "application/octet-stream";
            if (archive.compression == "gzip") { stored = export_fill::zlib_compress(*n.contents, 6); encoding = "application/x-gzip"; }
            else if (archive.compression == "bzip2") { stored = bzip2::encode(*n.contents); encoding = "application/x-bzip2"; }
            else stored = *n.contents;
            const auto offset = heap.size();
            heap.insert(heap.end(), stored.begin(), stored.end());
            files_xml += "<data><length>" + std::to_string(stored.size()) + "</length><offset>" + std::to_string(offset) + "</offset><size>" +
                         std::to_string(n.contents->size()) + "</size><encoding style=\"" + encoding + "\"/>" +
                         "<archived-checksum style=\"sha1\">" + hex(sha1(stored)) + "</archived-checksum>" +
                         "<extracted-checksum style=\"sha1\">" + hex(sha1(*n.contents)) + "</extracted-checksum></data>";
        }
        for (const auto& c : n.children) emit_file(*c);
        files_xml += "</file>";
    };
    for (const auto& f : archive.files) emit_file(*f);
    std::string toc = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<xar>";
    for (const auto& s : archive.subdocs) {
        toc += "<subdoc subdoc_name=\"" + escape(s->name) + "\">";
        emit_properties(toc, s->properties, false);
        toc += "</subdoc>";
    }
    toc += "<toc><checksum style=\"sha1\"><offset>0</offset><size>20</size></checksum>" + files_xml + "</toc></xar>\n";
    const auto compressed = export_fill::zlib_compress(std::span(reinterpret_cast<const std::uint8_t*>(toc.data()), toc.size()), 6);
    const auto toc_sum = sha1(compressed);
    std::copy(toc_sum.begin(), toc_sum.begin() + std::min<std::size_t>(20, toc_sum.size()), heap.begin());
    std::vector<std::uint8_t> out;
    put_big(out, magic, 4);
    put_big(out, header_size, 2);
    put_big(out, 1, 2);
    put_big(out, compressed.size(), 8);
    put_big(out, toc.size(), 8);
    put_big(out, 1, 4);   // SHA-1
    out.insert(out.end(), compressed.begin(), compressed.end());
    out.insert(out.end(), heap.begin(), heap.end());
    return out;
}

} // namespace alchemy::gui::completion_kit::xar
