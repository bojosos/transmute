#pragma once

// A small BER / DER reader for the code-signing code (CMS blobs, certificate
// names and extensions): definite and indefinite lengths, low tag numbers.
// Host only.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace alchemy::gui::security::ber {

struct Element {
    std::uint8_t tag{};
    std::size_t start{}, content{}, end{}, next{};   // element start, content start/end, the element after it
};

inline std::optional<Element> at(std::span<const std::uint8_t> data, std::size_t offset, int depth = 0) {
    if (depth > 32 || offset + 2 > data.size()) return std::nullopt;
    Element element;
    element.start = offset;
    element.tag = data[offset++];
    if ((element.tag & 0x1f) == 0x1f) return std::nullopt;   // high tag numbers: not used here
    const auto first = data[offset++];
    if (first == 0x80) {   // indefinite: up to the end-of-contents octets
        element.content = offset;
        while (offset + 2 <= data.size() && !(data[offset] == 0 && data[offset + 1] == 0)) {
            const auto inner = at(data, offset, depth + 1);
            if (!inner) return std::nullopt;
            offset = inner->next;
        }
        if (offset + 2 > data.size()) return std::nullopt;
        element.end = offset;
        element.next = offset + 2;
        return element;
    }
    std::size_t length = first;
    if (first & 0x80) {
        const auto count = first & 0x7f;
        if (count > 4 || offset + count > data.size()) return std::nullopt;
        length = 0;
        for (unsigned index = 0; index < count; ++index) length = (length << 8) | data[offset++];
    }
    if (offset + length > data.size()) return std::nullopt;
    element.content = offset;
    element.end = offset + length;
    element.next = element.end;
    return element;
}

inline std::vector<Element> children(std::span<const std::uint8_t> data, const Element& parent) {
    std::vector<Element> items;
    for (auto offset = parent.content; offset < parent.end;) {
        const auto child = at(data, offset);
        if (!child) return {};
        items.push_back(*child);
        offset = child->next;
    }
    return items;
}

inline std::vector<std::uint8_t> contents(std::span<const std::uint8_t> data, const Element& element) {
    return {data.begin() + static_cast<std::ptrdiff_t>(element.content), data.begin() + static_cast<std::ptrdiff_t>(element.end)};
}
inline std::vector<std::uint8_t> whole(std::span<const std::uint8_t> data, const Element& element) {
    return {data.begin() + static_cast<std::ptrdiff_t>(element.start), data.begin() + static_cast<std::ptrdiff_t>(element.next)};
}

// The dotted text of OBJECT IDENTIFIER content bytes.
inline std::string oid_text(std::span<const std::uint8_t> content) {
    std::string text;
    std::uint64_t value = 0;
    bool first = true;
    for (const auto byte : content) {
        value = (value << 7) | (byte & 0x7f);
        if (byte & 0x80) continue;
        if (first) {
            const auto head = value < 40 ? 0 : value < 80 ? 1 : 2;
            text = std::to_string(head) + "." + std::to_string(value - head * 40);
            first = false;
        } else {
            text += "." + std::to_string(value);
        }
        value = 0;
    }
    return text;
}
inline std::string oid_text(std::span<const std::uint8_t> data, const Element& element) {
    return oid_text(data.subspan(element.content, element.end - element.content));
}

// The content bytes of a dotted OBJECT IDENTIFIER (nullopt when malformed).
inline std::optional<std::vector<std::uint8_t>> oid_bytes(std::string_view text) {
    std::vector<std::uint64_t> arcs;
    std::uint64_t value = 0;
    bool digits = false;
    for (const auto c : text) {
        if (c == '.') {
            if (!digits) return std::nullopt;
            arcs.push_back(value);
            value = 0;
            digits = false;
        } else if (c >= '0' && c <= '9') {
            value = value * 10 + static_cast<std::uint64_t>(c - '0');
            digits = true;
        } else {
            return std::nullopt;
        }
    }
    if (!digits) return std::nullopt;
    arcs.push_back(value);
    if (arcs.size() < 2 || arcs[0] > 2) return std::nullopt;
    std::vector<std::uint8_t> out;
    const auto put = [&](std::uint64_t arc) {
        std::uint8_t buffer[10];
        int count = 0;
        do { buffer[count++] = static_cast<std::uint8_t>(arc & 0x7f); arc >>= 7; } while (arc);
        while (count--) out.push_back(static_cast<std::uint8_t>(buffer[count] | (count ? 0x80 : 0)));
    };
    put(arcs[0] * 40 + arcs[1]);
    for (std::size_t index = 2; index < arcs.size(); ++index) put(arcs[index]);
    return out;
}

} // namespace alchemy::gui::security::ber
