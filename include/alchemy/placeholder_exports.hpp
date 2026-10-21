#pragma once

// Placeholder exports: symbols a provider binds only so images link, whose
// call fails explicitly ("unsupported in a headless process"). A provider that
// implements the symbol for real may replace a placeholder; it never replaces
// a real export. Providers that only fill gaps (Provider::function in
// src/alchemy/darwin/kit/export_fill, ...) consult is_placeholder_export before skipping an
// existing export.

#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <tuple>

namespace alchemy {

class ProcessContext;

namespace detail {
struct PlaceholderExports {
    std::mutex mutex;
    std::set<std::tuple<const ProcessContext*, std::string, std::string>, std::less<>> names;
};
inline PlaceholderExports& placeholder_exports() {
    static PlaceholderExports value;
    return value;
}
} // namespace detail

inline void mark_placeholder_export(const ProcessContext& process, std::string_view library, std::string_view symbol) {
    auto& registry = detail::placeholder_exports();
    std::lock_guard lock(registry.mutex);
    registry.names.emplace(&process, std::string(library), std::string(symbol));
}

// True once for a placeholder: the caller is about to replace it.
inline bool take_placeholder_export(const ProcessContext& process, std::string_view library, std::string_view symbol) {
    auto& registry = detail::placeholder_exports();
    std::lock_guard lock(registry.mutex);
    const auto found = registry.names.find(std::make_tuple(&process, std::string(library), std::string(symbol)));
    if (found == registry.names.end()) return false;
    registry.names.erase(found);
    return true;
}

// Drops every placeholder record of a process (its exports die with it).
inline void forget_placeholder_exports(const ProcessContext& process) {
    auto& registry = detail::placeholder_exports();
    std::lock_guard lock(registry.mutex);
    for (auto it = registry.names.begin(); it != registry.names.end();)
        it = std::get<0>(*it) == &process ? registry.names.erase(it) : std::next(it);
}

} // namespace alchemy
