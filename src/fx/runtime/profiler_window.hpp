#pragma once

// Sampling profiler configuration and report files (owner: runtime;
// profiler.cpp). Separate from runtime_internal.hpp so tests can check the
// TRANSMUTE_FX_SAMPLE syntax and the atomic report replacement directly.

#include "fx/host/file.hpp"

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <string_view>

namespace transmute::fx::rt {

// TRANSMUTE_FX_SAMPLE=<file|1>[,<interval_us>][,main][,stacks][,bg]
//                     [,delay=<seconds>][,duration=<seconds>][,paused][,regions=<n>]
struct SampleWindow {
    std::wstring path;          // "1" = %TEMP%\fx-profile-<pid>.txt (resolved by the profiler)
    unsigned interval_us = 1000;
    bool main_only = false;
    bool stacks = false;
    bool background = false;
    double delay_seconds = 0;     // sampling starts this long after runtime_initialize
    double duration_seconds = 0;  // and stops this long after it started (0 = until exit)
    bool paused = false;          // start with sampling off (fx::profiler_set_enabled turns it on)
    unsigned dump_regions = 24;   // hottest regions whose host code is dumped (<report>.r<N>.bin), 1..4096
};

// False for an empty value or "0" (profiler off). Unknown flags are ignored.
inline bool parse_sample_setting(std::wstring_view text, SampleWindow& out) {
    out = SampleWindow{};
    if (text.empty() || text == L"0") return false;
    bool first = true;
    while (!text.empty()) {
        const std::size_t comma = text.find(L',');
        const std::wstring item(text.substr(0, comma));
        text = comma == std::wstring_view::npos ? std::wstring_view{} : text.substr(comma + 1);
        if (first) {
            out.path = item;
            first = false;
            continue;
        }
        const auto seconds = [&](std::size_t prefix) { return std::max(0.0, std::wcstod(item.c_str() + prefix, nullptr)); };
        if (item == L"main") out.main_only = true;
        else if (item == L"stacks") out.stacks = true;
        else if (item == L"bg") out.background = true;
        else if (item == L"paused") out.paused = true;
        else if (item.rfind(L"delay=", 0) == 0) out.delay_seconds = seconds(6);
        else if (item.rfind(L"duration=", 0) == 0) out.duration_seconds = seconds(9);
        else if (item.rfind(L"regions=", 0) == 0)
            out.dump_regions = std::clamp(static_cast<unsigned>(std::wcstoul(item.c_str() + 8, nullptr, 10)), 1u, 4096u);
        else if (!item.empty() && item[0] >= L'0' && item[0] <= L'9')
            out.interval_us = std::max(100u, static_cast<unsigned>(std::wcstoul(item.c_str(), nullptr, 10)));
    }
    return !out.path.empty() && out.path != L"0";
}

// Renames `temp` over `target` (same directory, so the same volume): readers
// see the old file or the new one, never a partial one. POSIX rename
// semantics first, so a reader that opened the old report with delete
// sharing keeps reading it while the new one takes its name; otherwise
// MoveFileEx, retried briefly while a reader holds the target without delete
// sharing. False (temp left for the caller to remove) if it cannot be replaced.
inline bool replace_file_atomically(const std::wstring& temp, const std::wstring& target) {
    return host::file::replace_atomically(temp, target);
}

} // namespace transmute::fx::rt
