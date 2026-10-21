#include "alchemy/darwin/foundation/sdef.hpp"

#include <cctype>
#include <map>

namespace alchemy::gui::completion_kit::sdef {
namespace {

struct Tag {
    std::string name;
    std::map<std::string, std::string> attributes;
    bool closing{}, empty{};
};

std::string decode(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') { out.push_back(text[i]); continue; }
        const auto end = text.find(';', i);
        if (end == std::string_view::npos) { out.push_back('&'); continue; }
        const auto entity = text.substr(i + 1, end - i - 1);
        if (entity == "amp") out.push_back('&');
        else if (entity == "lt") out.push_back('<');
        else if (entity == "gt") out.push_back('>');
        else if (entity == "quot") out.push_back('"');
        else if (entity == "apos") out.push_back('\'');
        else if (!entity.empty() && entity[0] == '#') {
            const auto value = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X')
                ? std::stoul(std::string(entity.substr(2)), nullptr, 16) : std::stoul(std::string(entity.substr(1)), nullptr, 10);
            if (value < 0x80) out.push_back(static_cast<char>(value));
            else if (value < 0x800) { out.push_back(static_cast<char>(0xc0 | (value >> 6))); out.push_back(static_cast<char>(0x80 | (value & 0x3f))); }
            else { out.push_back(static_cast<char>(0xe0 | (value >> 12))); out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f))); out.push_back(static_cast<char>(0x80 | (value & 0x3f))); }
        } else { out.append(text.substr(i, end - i + 1)); }
        i = end;
    }
    return out;
}

// The tags of a document in order (text, comments, processing instructions
// and declarations skipped); nullopt when malformed.
std::optional<std::vector<Tag>> tags(std::string_view xml) {
    std::vector<Tag> result;
    std::size_t at = 0;
    while ((at = xml.find('<', at)) != std::string_view::npos) {
        if (xml.substr(at, 4) == "<!--") {
            const auto end = xml.find("-->", at + 4);
            if (end == std::string_view::npos) return std::nullopt;
            at = end + 3;
            continue;
        }
        if (xml.substr(at, 2) == "<?" || xml.substr(at, 2) == "<!") {
            const auto end = xml.find('>', at);
            if (end == std::string_view::npos) return std::nullopt;
            at = end + 1;
            continue;
        }
        Tag tag;
        std::size_t i = at + 1;
        if (i < xml.size() && xml[i] == '/') { tag.closing = true; ++i; }
        const auto name_start = i;
        while (i < xml.size() && !std::isspace(static_cast<unsigned char>(xml[i])) && xml[i] != '>' && xml[i] != '/') ++i;
        tag.name = std::string(xml.substr(name_start, i - name_start));
        for (;;) {
            while (i < xml.size() && std::isspace(static_cast<unsigned char>(xml[i]))) ++i;
            if (i >= xml.size()) return std::nullopt;
            if (xml[i] == '>') { ++i; break; }
            if (xml[i] == '/' && i + 1 < xml.size() && xml[i + 1] == '>') { tag.empty = true; i += 2; break; }
            const auto key_start = i;
            while (i < xml.size() && xml[i] != '=' && xml[i] != '>' && !std::isspace(static_cast<unsigned char>(xml[i]))) ++i;
            const auto key = std::string(xml.substr(key_start, i - key_start));
            while (i < xml.size() && std::isspace(static_cast<unsigned char>(xml[i]))) ++i;
            if (i >= xml.size() || xml[i] != '=') return std::nullopt;
            ++i;
            while (i < xml.size() && std::isspace(static_cast<unsigned char>(xml[i]))) ++i;
            if (i >= xml.size() || (xml[i] != '"' && xml[i] != '\'')) return std::nullopt;
            const char quote = xml[i++];
            const auto end = xml.find(quote, i);
            if (end == std::string_view::npos) return std::nullopt;
            tag.attributes[key] = decode(xml.substr(i, end - i));
            i = end + 1;
        }
        result.push_back(std::move(tag));
        at = i;
    }
    return result;
}

std::string attribute(const Tag& tag, const char* name) {
    const auto found = tag.attributes.find(name);
    return found == tag.attributes.end() ? std::string{} : found->second;
}

} // namespace

const Class* Definition::by_name(std::string_view name) const {
    for (const auto& value : classes) if (value.name == name) return &value;
    return nullptr;
}

const Class* Definition::by_cocoa_class(std::string_view name) const {
    for (const auto& value : classes) if (value.cocoa_class == name) return &value;
    return nullptr;
}

std::uint32_t four_char_code(std::string_view text) {
    if (text.size() != 4) return 0;
    std::uint32_t value = 0;
    for (const char c : text) value = (value << 8) | static_cast<std::uint8_t>(c);
    return value;
}

std::string camel_case(std::string_view name) {
    std::string out;
    bool upper = false;
    for (const char c : name) {
        if (c == ' ' || c == '-' || c == '_') { upper = !out.empty(); continue; }
        if (upper && c >= 'a' && c <= 'z') out.push_back(static_cast<char>(c - 'a' + 'A'));
        else out.push_back(c);
        upper = false;
    }
    return out;
}

std::optional<Definition> parse(std::string_view xml) {
    const auto list = tags(xml);
    if (!list) return std::nullopt;
    Definition definition;
    std::string suite;
    Class* current = nullptr;          // the class (or extended class) being read
    Property* property = nullptr;
    Element* element = nullptr;
    Command* command = nullptr;
    std::vector<Class> extensions;
    for (const auto& tag : *list) {
        if (tag.name == "suite") { if (!tag.closing) suite = attribute(tag, "name"); continue; }
        if (tag.name == "class" || tag.name == "class-extension") {
            if (tag.closing) { current = nullptr; continue; }
            Class value;
            value.suite = suite;
            if (tag.name == "class") {
                value.name = attribute(tag, "name");
                value.code = attribute(tag, "code");
                value.plural = attribute(tag, "plural");
                value.inherits = attribute(tag, "inherits");
                definition.classes.push_back(std::move(value));
                current = tag.empty ? nullptr : &definition.classes.back();
            } else {
                value.name = attribute(tag, "extends");
                extensions.push_back(std::move(value));
                current = tag.empty ? nullptr : &extensions.back();
            }
            property = nullptr; element = nullptr;
            continue;
        }
        if (tag.name == "property" && current) {
            if (tag.closing) { property = nullptr; continue; }
            current->properties.push_back({attribute(tag, "name"), attribute(tag, "code"), attribute(tag, "type"), {}, attribute(tag, "access")});
            property = tag.empty ? nullptr : &current->properties.back();
            if (tag.empty) current->properties.back().key = camel_case(current->properties.back().name);
            continue;
        }
        if (tag.name == "element" && current) {
            if (tag.closing) { element = nullptr; continue; }
            current->elements.push_back({attribute(tag, "type"), {}, attribute(tag, "access")});
            element = tag.empty ? nullptr : &current->elements.back();
            continue;
        }
        if (tag.name == "responds-to" && current && !tag.closing) {
            auto name = attribute(tag, "command");
            if (name.empty()) name = attribute(tag, "name");
            current->responds_to.push_back(name);
            continue;
        }
        if (tag.name == "command") {
            if (tag.closing) { command = nullptr; continue; }
            definition.commands.push_back({attribute(tag, "name"), attribute(tag, "code"), suite, {}});
            command = tag.empty ? nullptr : &definition.commands.back();
            continue;
        }
        if (tag.name == "cocoa" && !tag.closing) {
            if (property) property->key = attribute(tag, "key");
            else if (element) element->key = attribute(tag, "key");
            else if (command) command->cocoa_class = attribute(tag, "class");
            else if (current) current->cocoa_class = attribute(tag, "class");
            continue;
        }
    }
    // Defaults: keys from names (properties) and from plural type names (elements).
    for (auto* group : {&definition.classes, &extensions})
        for (auto& value : *group)
            for (auto& item : value.properties) if (item.key.empty()) item.key = camel_case(item.name);
    const auto plural_of = [&](const std::string& type) {
        const auto* found = definition.by_name(type);
        return found && !found->plural.empty() ? found->plural : type + "s";
    };
    for (auto* group : {&definition.classes, &extensions})
        for (auto& value : *group)
            for (auto& item : value.elements) if (item.key.empty()) item.key = camel_case(plural_of(item.type));
    for (auto& extension : extensions) {
        for (auto& value : definition.classes) {
            if (value.name != extension.name) continue;
            value.properties.insert(value.properties.end(), extension.properties.begin(), extension.properties.end());
            value.elements.insert(value.elements.end(), extension.elements.begin(), extension.elements.end());
            value.responds_to.insert(value.responds_to.end(), extension.responds_to.begin(), extension.responds_to.end());
            if (value.cocoa_class.empty()) value.cocoa_class = extension.cocoa_class;
        }
    }
    for (auto& value : definition.classes) {
        if (value.plural.empty()) value.plural = value.name + "s";
        if (value.cocoa_class.empty()) {   // Cocoa's default: NS + the capitalized name ("application" -> NSApplication)
            auto name = camel_case(value.name);
            if (!name.empty() && name[0] >= 'a' && name[0] <= 'z') name[0] = static_cast<char>(name[0] - 'a' + 'A');
            value.cocoa_class = "NS" + name;
        }
    }
    return definition;
}

} // namespace alchemy::gui::completion_kit::sdef
