#include "alchemy/darwin/kit/export_fill/export_fill.hpp"

#include "alchemy/host/locale.hpp"
#include "alchemy/object_store.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// CoreServices interfaces Qt Core and an installer import:
//   Unicode collation (UnicodeUtilities): LocaleRefFromLocaleString,
//     UCCreateCollator, UCCompareText, UCGetCollationKey,
//     UCCompareCollationKeys, UCDisposeCollator, over the host's linguistic
//     comparison (host::locale::compare and sort_key).
//   Uniform Type Identifiers: UTTypeEqual. (UTTypeConformsTo and the other
//     UTType functions are the headless base's, coreservices/types_and_gestalt.cpp.)
//   (FSEvents is fsevents.cpp's: real streams over the host's directory watches.)
//   LaunchServices: LSCopyDefaultApplicationURLForURL finds no macOS application
//     (NULL; no application bundles are registered on the host).
namespace alchemy::export_fill {
namespace {

constexpr std::string_view core_services_library = "/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices";
// MacErrors.h
constexpr std::int64_t no_err = 0, param_err = -50, collate_buffer_too_small = -29509, collate_invalid_collation_ref = -29510;
// UnicodeUtilities.h UCCollateOptions
constexpr std::uint32_t collate_compose_insensitive = 1U << 1, collate_width_insensitive = 1U << 2, collate_case_insensitive = 1U << 3,
    collate_diacrit_insensitive = 1U << 4, collate_punctuation_significant = 1U << 15, collate_digits_as_number = 1U << 17;

std::uint64_t status(std::int64_t value) { return static_cast<std::uint64_t>(value); }

struct Collator { std::string locale; std::uint32_t options{}; };   // locale "" is the user's default

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

} // namespace

void install_coreservices(Provider& p, ObjcRuntime& objc, CoreFoundation& cf) {
    auto& process = p.process();
    const auto function = [&](const char* name, auto handler) { p.function(core_services_library, name, std::move(handler), 0); };
    static_cast<void>(objc);

    // ---- collation ----
    // Opaque guest tokens name host state (LocaleRef, CollatorRef).
    struct Handles {
        std::mutex mutex;
        std::map<std::uint64_t, std::string> locales;
        std::map<std::uint64_t, Collator> collators;
    };
    auto handles = std::make_shared<Handles>();
    // OSStatus LocaleRefFromLocaleString(const char* localeString, LocaleRef* locale)
    function("_LocaleRefFromLocaleString", [&process, handles](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[0] || !s.x[1]) return status(param_err);
        auto name = guest_string(m, s.x[0], 256);
        for (auto& c : name) if (c == '_') c = '-';
        if (const auto at = name.find('@'); at != std::string::npos) name.resize(at);
        // An unknown locale collates as the user's.
        const auto known = host::locale::valid(name) ? name : std::string{};
        const auto token = process.allocate_guest(16, 16);
        { std::lock_guard lock(handles->mutex); handles->locales[token] = known; }
        m.write64(s.x[1], token);
        return status(no_err);
    });
    // OSStatus UCCreateCollator(LocaleRef, LocaleOperationVariant, UCCollateOptions, CollatorRef*)
    function("_UCCreateCollator", [&process, handles](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[3]) return status(param_err);
        Collator collator;
        {
            std::lock_guard lock(handles->mutex);
            if (s.x[0]) {
                const auto found = handles->locales.find(s.x[0]);
                if (found == handles->locales.end()) return status(param_err);
                collator.locale = found->second;
            }
        }
        namespace locale = host::locale;
        const auto options = static_cast<std::uint32_t>(s.x[2]);
        if (options & collate_case_insensitive) collator.options |= locale::ignore_case_linguistic;
        if (options & collate_diacrit_insensitive) collator.options |= locale::ignore_diacritics;
        if (options & collate_width_insensitive) collator.options |= locale::ignore_width;
        if (options & collate_compose_insensitive) collator.options |= locale::ignore_kana_type;
        if (!(options & collate_punctuation_significant)) collator.options |= locale::ignore_symbols;
        if (options & collate_digits_as_number) collator.options |= locale::digits_as_numbers;
        const auto token = process.allocate_guest(16, 16);
        { std::lock_guard lock(handles->mutex); handles->collators[token] = collator; }
        m.write64(s.x[3], token);
        return status(no_err);
    });
    const auto collator_of = [handles](std::uint64_t token) -> std::optional<Collator> {
        std::lock_guard lock(handles->mutex);
        const auto found = handles->collators.find(token);
        if (found == handles->collators.end()) return std::nullopt;
        return found->second;
    };
    const auto text = [](GuestMemory& m, std::uint64_t address, std::uint64_t length) {
        std::u16string value(static_cast<std::size_t>(length), u'\0');
        for (std::uint64_t i = 0; i < length; ++i) {
            value[static_cast<std::size_t>(i)] = static_cast<char16_t>(m.read8(address + i * 2) | (m.read8(address + i * 2 + 1) << 8));
        }
        return value;
    };
    // OSStatus UCCompareText(CollatorRef, const UniChar*, UniCharCount, const UniChar*, UniCharCount,
    //                        Boolean* equivalent, SInt32* order)
    function("_UCCompareText", [collator_of, text](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto collator = collator_of(s.x[0]);
        if (!collator) return status(collate_invalid_collation_ref);
        const auto left = text(m, s.x[1], s.x[2]), right = text(m, s.x[3], s.x[4]);
        const auto result = host::locale::compare(collator->locale, left, right, collator->options);
        if (!result) {
            if (result.error() == host::Error::unsupported) throw std::runtime_error("Alchemy UCCompareText requires the host collation service");
            return status(param_err);
        }
        if (s.x[5]) m.write8(s.x[5], *result == 0 ? 1 : 0);
        if (s.x[6]) m.write32(s.x[6], static_cast<std::uint32_t>(*result));
        return status(no_err);
    });
    // OSStatus UCGetCollationKey(CollatorRef, const UniChar*, UniCharCount, ItemCount maxKeySize,
    //                            ItemCount* actualKeySize, UCCollationValue* key)
    // One UCCollationValue per byte of the host sort key, so comparing keys
    // value by value orders like the text.
    function("_UCGetCollationKey", [collator_of, text](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto collator = collator_of(s.x[0]);
        if (!collator) return status(collate_invalid_collation_ref);
        if (!s.x[4] || !s.x[5]) return status(param_err);
        const auto key = host::locale::sort_key(collator->locale, text(m, s.x[1], s.x[2]), collator->options);
        if (!key) {
            if (key.error() == host::Error::unsupported) throw std::runtime_error("Alchemy UCGetCollationKey requires the host collation service");
            return status(param_err);
        }
        m.write64(s.x[4], key->size());
        if (key->size() > s.x[3]) return status(collate_buffer_too_small);
        for (std::size_t i = 0; i < key->size(); ++i) m.write32(s.x[5] + i * 4, (*key)[i]);
        return status(no_err);
    });
    // OSStatus UCCompareCollationKeys(const UCCollationValue*, ItemCount, const UCCollationValue*, ItemCount,
    //                                 Boolean* equivalent, SInt32* order)
    function("_UCCompareCollationKeys", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        std::int32_t order = 0;
        const auto count = std::min(s.x[1], s.x[3]);
        for (std::uint64_t i = 0; i < count && order == 0; ++i) {
            const auto a = m.read32(s.x[0] + i * 4), b = m.read32(s.x[2] + i * 4);
            order = a < b ? -1 : a > b ? 1 : 0;
        }
        if (order == 0 && s.x[1] != s.x[3]) order = s.x[1] < s.x[3] ? -1 : 1;
        if (s.x[4]) m.write8(s.x[4], order == 0 ? 1 : 0);
        if (s.x[5]) m.write32(s.x[5], static_cast<std::uint32_t>(order));
        return status(no_err);
    });
    // OSStatus UCDisposeCollator(CollatorRef*)
    function("_UCDisposeCollator", [&process, handles](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[0]) return status(param_err);
        const auto token = m.read64(s.x[0]);
        {
            std::lock_guard lock(handles->mutex);
            if (!handles->collators.erase(token)) return status(collate_invalid_collation_ref);
        }
        process.release_guest(token);
        m.write64(s.x[0], 0);
        return status(no_err);
    });

    // ---- Uniform Type Identifiers ----
    const auto type_string = [&cf](std::uint64_t object) { return object ? cf.utf8(object) : std::string{}; };
    function("_UTTypeEqual", [type_string](CpuState& s, GuestMemory&) -> std::uint64_t {
        return lower(type_string(s.x[0])) == lower(type_string(s.x[1])) ? 1 : 0;
    });

    // ---- LaunchServices ----
    // CFURLRef LSCopyDefaultApplicationURLForURL(CFURLRef, LSRolesMask, CFErrorRef*)
    function("_LSCopyDefaultApplicationURLForURL", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (s.x[2]) m.write64(s.x[2], 0);
        return 0;
    });
}

} // namespace alchemy::export_fill
