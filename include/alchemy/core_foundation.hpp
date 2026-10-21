#pragma once

#include "alchemy/object_store.hpp"
#include "alchemy/process_context.hpp"

#include <functional>
#include <optional>
#include <source_location>
#include <utility>

namespace alchemy {
inline constexpr auto core_foundation_library = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
// Clang constant-literal records (@N, @[...], @{...}) in guest Mach-O storage.
enum class LiteralClass { integer, floating, double_precision, array, dictionary };
class CoreFoundation {
public:
    explicit CoreFoundation(ProcessContext& process);
    ~CoreFoundation();
    CoreFoundation(const CoreFoundation&) = delete;
    CoreFoundation& operator=(const CoreFoundation&) = delete;
    void set_class(ObjectKind kind, std::uint64_t address);
    // Bridge class for mutable arrays, dictionaries and data (NSMutable*).
    void set_mutable_class(ObjectKind kind, std::uint64_t address);
    void set_literal_class(LiteralClass literal, std::uint64_t address);
    // Called for an address that is neither an object-store object nor a
    // constant CF string or literal (Objective-C class objects held by
    // collections); returns true after adopting it into the object store.
    void set_foreign_adopter(std::function<bool(std::uint64_t)> adopter);
    // Another adopter tried before the foreign adopter (Blocks, ...).
    void add_foreign_adopter(std::function<bool(std::uint64_t)> adopter);
    [[nodiscard]] std::uint64_t string(std::string_view utf8);
    [[nodiscard]] std::uint64_t string_utf16(std::span<const std::uint16_t> units);
    [[nodiscard]] std::uint64_t constant_string(std::string_view utf8);
    [[nodiscard]] std::string utf8(std::uint64_t object);
    [[nodiscard]] std::vector<std::uint16_t> units(std::uint64_t object);
    // Stable inner pointer (lifetime of the string) or 0 when not representable.
    [[nodiscard]] std::uint64_t c_string(std::uint64_t object, std::uint32_t encoding);
    [[nodiscard]] std::uint64_t length(std::uint64_t object);
    [[nodiscard]] std::uint64_t element(std::uint64_t object, std::uint64_t index);
    [[nodiscard]] std::uint64_t dictionary_value(std::uint64_t object, std::uint64_t key);
    [[nodiscard]] bool equal(std::uint64_t left, std::uint64_t right);
    [[nodiscard]] std::uint64_t hash(std::uint64_t object);
    [[nodiscard]] std::uint64_t copy(std::uint64_t object);
    [[nodiscard]] std::uint64_t mutable_copy(std::uint64_t object);
    // Replaces the characters of a mutable string (one made by mutable_copy).
    // Inner pointers obtained earlier stay valid until the string is freed.
    void replace_string_units(std::uint64_t object, std::vector<std::uint16_t> units);
    [[nodiscard]] std::uint64_t description(std::uint64_t object);
    [[nodiscard]] std::optional<ObjectKind> kind(std::uint64_t object);
    [[nodiscard]] bool is_mutable(std::uint64_t object);
    // Host-side constructors return +1 references with the standard CFType callbacks.
    [[nodiscard]] std::uint64_t array(std::span<const std::uint64_t> values, bool mutable_array = false);
    [[nodiscard]] std::uint64_t dictionary(std::span<const std::pair<std::uint64_t, std::uint64_t>> values, bool mutable_dictionary = false);
    [[nodiscard]] std::vector<std::uint64_t> array_values(std::uint64_t object);
    [[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint64_t>> dictionary_entries(std::uint64_t object);
    [[nodiscard]] std::uint64_t integer(std::int64_t value, unsigned type = 4);
    [[nodiscard]] std::uint64_t real(double value);
    [[nodiscard]] std::uint64_t boolean(bool value) const;
    [[nodiscard]] std::optional<double> number_value(std::uint64_t object);
    [[nodiscard]] std::uint64_t date(double absolute_time, bool immortal = false);
    [[nodiscard]] double date_time(std::uint64_t object);
    [[nodiscard]] static double absolute_time_now();
    [[nodiscard]] std::uint64_t file_url(std::string_view guest_path, bool directory);
    [[nodiscard]] std::string url_path(std::uint64_t object);
    [[nodiscard]] bool url_is_directory(std::uint64_t object);
    // CFURLCopyResourcePropertyForKey: nullopt when the item is unreachable, else
    // a +1 value or 0 when the property has no value for this item.
    [[nodiscard]] std::optional<std::uint64_t> resource_value(std::uint64_t url, std::uint64_t key);
    [[nodiscard]] std::uint64_t locale(std::string_view identifier);
    [[nodiscard]] std::uint64_t current_locale();
    [[nodiscard]] std::uint64_t invoke(std::string_view symbol, std::span<const std::uint64_t> arguments);

    // Extends CoreFoundation function `function` (its name without the leading
    // underscore, "CFGetTypeID") for the objects of one `domain` that another
    // provider owns ("Security CF types", "network URLs"): `handler` answers
    // the result for a call in its domain and nullopt for any other, which
    // CoreFoundation's own implementation then answers. The export keeps one
    // owner; the registration audit lists each extension (kind "extension").
    // Dispatch does not depend on install order: a second extension of the
    // same domain is refused when registered, and a call that two extensions
    // claim fails explicitly. Install time only; invoke() does not consult
    // extensions.
    using Extension = std::function<std::optional<std::uint64_t>(transmute::CpuState&, transmute::GuestMemory&)>;
    void extend(std::string_view function, std::string_view domain, Extension handler,
                const std::source_location& where = std::source_location::current());
    // True when CoreFoundation implements `function` and its implementation is
    // still the bound export (so extend() reaches the guest's calls).
    [[nodiscard]] bool implements(std::string_view function) const;
    // URL strings CoreFoundation and Foundation do not represent themselves
    // (they keep file URLs only), extended by the same rules as extend():
    // Foundation's +[NSURL URLWithString:] / -initWithString: ask every
    // handler; the one whose `domain` the string belongs to ("URL schemes other
    // than file:", the network provider) returns a +1 URL or 0 for nil, the
    // others nullopt, and a string no handler claims stays Foundation's.
    using UrlStringHandler = std::function<std::optional<std::uint64_t>(std::uint64_t string)>;
    void extend_url_strings(std::string_view domain, UrlStringHandler handler,
                            const std::source_location& where = std::source_location::current());
    [[nodiscard]] std::optional<std::uint64_t> handle_url_string(std::uint64_t string);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace alchemy
