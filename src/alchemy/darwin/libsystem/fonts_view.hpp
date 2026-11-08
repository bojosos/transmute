#pragma once

// /System/Library/Fonts: a read-only view of the host's system fonts (fw-ui's
// design, frameworks-wave/fw-ui/fonts-view-design.md in the wave notes).
// translate_path maps the guest directory to the host's; calls that change a
// file refuse host paths inside it with EROFS (services_impl.hpp's
// refuse_read_only, and setattrlist in fs/attrlist.cpp).

#include "alchemy/host/system.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace alchemy::libsystem {

// The host's font directory, or nullopt when the host has none.
inline const std::optional<std::filesystem::path>& host_fonts_directory() {
    static const auto value = []() -> std::optional<std::filesystem::path> {
        const auto found = host::system::fonts_directory();
        if (!found || found->empty()) return std::nullopt;
        return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(found->data()), found->size())).lexically_normal();
    }();
    return value;
}

// Whether a host path is inside the fonts view (compared without case, as the host's paths are).
inline bool in_fonts_view(const std::filesystem::path& host) {
    const auto& fonts = host_fonts_directory();
    if (!fonts) return false;
    const auto lower = [](std::u8string text) {
        for (auto& c : text) if (c >= u8'A' && c <= u8'Z') c = static_cast<char8_t>(c - u8'A' + u8'a');
        return text;
    };
    const auto base = lower(fonts->generic_u8string()), value = lower(host.lexically_normal().generic_u8string());
    return value == base || (value.size() > base.size() && value.starts_with(base) && value[base.size()] == u8'/');
}

} // namespace alchemy::libsystem
