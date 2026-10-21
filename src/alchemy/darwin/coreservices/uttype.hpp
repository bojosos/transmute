#pragma once

// Uniform Type Identifiers (host-only): a table of system-declared types
// (identifier, direct supertypes, filename extensions, MIME types, HFS
// OSType codes) with conformance, tag lookup and dynamic identifiers for
// tags no declared type claims ("dyn.alchemy-ext-<ext>" / "dyn.alchemy-mime-<type>",
// the scheme coreservices.cpp uses for the C API; macOS's "dyn."
// identifiers are opaque too). Dynamic types conform to public.data.
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui::completion_kit::uttype {

struct Declaration {
    const char* identifier;
    std::vector<const char*> parents, extensions, mime, ostypes;
    const char* description;
};

[[nodiscard]] const std::vector<Declaration>& declarations();
[[nodiscard]] const Declaration* find(std::string_view identifier);   // case-insensitive
[[nodiscard]] bool is_dynamic(std::string_view identifier);
// Every supertype (transitively), without the type itself.
[[nodiscard]] std::vector<std::string> supertypes(std::string_view identifier);
[[nodiscard]] bool conforms(std::string_view type, std::string_view to);

enum class Tag { filename_extension, mime_type, ostype };
// The preferred type for a tag (the first declared one conforming to
// `conforming_to` when given), else a dynamic type (nullopt when even that
// would not conform: a dynamic type conforms only to public.data / public.item).
[[nodiscard]] std::optional<std::string> for_tag(Tag tag, std::string_view value, std::string_view conforming_to = {});
[[nodiscard]] std::optional<std::string> preferred_tag(std::string_view identifier, Tag tag);
[[nodiscard]] std::vector<std::string> tags(std::string_view identifier, Tag tag);
[[nodiscard]] std::string description(std::string_view identifier);

} // namespace alchemy::gui::completion_kit::uttype
