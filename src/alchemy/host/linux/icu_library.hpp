#pragma once

// Linux host backend: the distribution's ICU4C, loaded at run time (areas "icu" and
// "locale"). libicuuc.so.NN and libicui18n.so.NN are opened by soname, newest first, so the
// build needs no ICU headers and a host without ICU still runs (its areas answer
// "unavailable"). The C API's names carry the major version as a suffix (u_strToUpper_78)
// unless the distribution built ICU with U_DISABLE_RENAMING; symbol() tries both forms.
// Linux-only: no other backend includes this.

#include <dlfcn.h>

#include <cstdio>
#include <string>

namespace alchemy::host::linux_icu {

struct Library {
    void* common{};   // libicuuc: strings, converters, break iterators, locales, normalization
    void* i18n{};     // libicui18n: collation, search, transliteration, formats, calendars, currencies
    int major{};
    std::string suffix;   // "_78"
};

inline const Library& library() noexcept {
    static const Library value = [] {
        Library result;
        // Oldest supported is the area's minimum (icu.hpp minimum_major_version, 64).
        for (int major = 99; major >= 64 && !result.common; --major) {
            char name[32];
            std::snprintf(name, sizeof(name), "libicuuc.so.%d", major);
            if (void* common = dlopen(name, RTLD_NOW | RTLD_LOCAL)) {
                std::snprintf(name, sizeof(name), "libicui18n.so.%d", major);
                void* i18n = dlopen(name, RTLD_NOW | RTLD_LOCAL);
                if (!i18n) { dlclose(common); continue; }
                result.common = common;
                result.i18n = i18n;
                result.major = major;
                result.suffix = "_" + std::to_string(major);
            }
        }
        return result;   // the libraries stay loaded for the process's lifetime
    }();
    return value;
}

// The address of ICU's C function `name`, or nullptr.
inline void* symbol(const char* name) noexcept {
    const auto& icu = library();
    if (!icu.common) return nullptr;
    const auto suffixed = std::string(name) + icu.suffix;
    for (void* handle : {icu.common, icu.i18n}) {
        if (void* found = dlsym(handle, suffixed.c_str())) return found;
        if (void* found = dlsym(handle, name)) return found;
    }
    return nullptr;
}

} // namespace alchemy::host::linux_icu
