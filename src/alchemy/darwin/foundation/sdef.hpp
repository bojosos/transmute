#pragma once

// Scripting definitions (.sdef XML, the OSAScriptingDefinition of a bundle):
// the suites' classes, class extensions and commands, as Cocoa scripting
// reads them (host-only core, unit tested).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui::completion_kit::sdef {

struct Property { std::string name, code, type, key, access; };
struct Element { std::string type, key, access; };
struct Class {
    std::string name, code, plural, inherits, cocoa_class, suite;
    std::vector<Property> properties;
    std::vector<Element> elements;
    std::vector<std::string> responds_to;   // command names
};
struct Command { std::string name, code, suite, cocoa_class; };
struct Definition {
    std::vector<Class> classes;              // class-extensions merged into their class
    std::vector<Command> commands;
    [[nodiscard]] const Class* by_name(std::string_view name) const;
    [[nodiscard]] const Class* by_cocoa_class(std::string_view name) const;
};

// Four-character code text ("QLab") as a FourCharCode; 0 when not 4 bytes.
[[nodiscard]] std::uint32_t four_char_code(std::string_view text);
// The KVC key of a property or element without a cocoa key: the name in
// camel case ("q number" -> "qNumber"); elements use the plural.
[[nodiscard]] std::string camel_case(std::string_view name);
// Parses an sdef document; nullopt when it is not well formed.
[[nodiscard]] std::optional<Definition> parse(std::string_view xml);

} // namespace alchemy::gui::completion_kit::sdef
