#pragma once

// xar archives (host-only), from the public format: a big-endian header
// (magic "xar!", header size, version 1, compressed and uncompressed TOC
// lengths, TOC checksum algorithm), the zlib-compressed XML table of contents
// and the heap. The TOC lists <file> elements (nested for directories) whose
// child elements are the file's properties ("name", "type", "mode",
// "data/length", ...); <subdoc subdoc_name="..."> elements beside the <toc>
// carry archive-level properties. File data in the heap is stored
// ("application/octet-stream"), zlib ("application/x-gzip") or bzip2
// ("application/x-bzip2") encoded; LZMA ("application/x-lzma") is not
// provided. Written archives use SHA-1 checksums (TOC and file data) through
// the host digest; reading does not verify checksums.
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace alchemy::gui::completion_kit::xar {

using Properties = std::vector<std::pair<std::string, std::string>>;   // key path ("data/size") -> text
using Attributes = std::vector<std::pair<std::string, Properties>>;    // key path -> attributes

struct Node {
    Properties properties;
    Attributes attributes;
    std::vector<std::unique_ptr<Node>> children;
    Node* parent{};
    std::uint64_t id{};
    std::optional<std::vector<std::uint8_t>> contents;   // written archives: the file's data

    [[nodiscard]] const std::string* property(std::string_view key) const;
    void set(std::string_view key, std::string value);
    [[nodiscard]] const std::string* attribute(std::string_view key, std::string_view name) const;
    [[nodiscard]] std::string path() const;   // names from the top, '/'-separated
};

struct Subdoc {
    std::string name;
    Properties properties;
    [[nodiscard]] const std::string* property(std::string_view key) const;
    void set(std::string_view key, std::string value);
};

struct Archive {
    std::vector<std::unique_ptr<Node>> files;   // top level
    std::vector<std::unique_ptr<Subdoc>> subdocs;
    std::vector<std::uint8_t> heap;             // read archives
    std::string compression{"gzip"};            // written archives: gzip, bzip2 or none
    std::uint64_t next_id{1};

    // Every file, depth first (xar_file_first / xar_file_next order).
    [[nodiscard]] std::vector<Node*> all() const;
    Node& add(Node* parent, std::string name);
};

[[nodiscard]] std::unique_ptr<Archive> read(std::span<const std::uint8_t> bytes, std::string& error);
// The file's data, decoded; nullopt with `error` for missing or bad data
// (`unsupported` is set for an encoding that is not provided).
[[nodiscard]] std::optional<std::vector<std::uint8_t>> extract(const Archive& archive, const Node& node, std::string& error, bool& unsupported);

// SHA-1 for written archives (injected: the host digest).
using Digest = std::function<std::vector<std::uint8_t>(std::span<const std::uint8_t>)>;
[[nodiscard]] std::vector<std::uint8_t> write(Archive& archive, const Digest& sha1);

} // namespace alchemy::gui::completion_kit::xar
