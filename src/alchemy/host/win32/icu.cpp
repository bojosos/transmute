// Win32 host backend, area "icu" (include/alchemy/host/icu.hpp): the system ICU
// of Windows 10 1903 and later, System32\icu.dll. Its exports carry ICU's
// unversioned C names.
#include "alchemy/host/icu.hpp"

#include "win32.hpp"

namespace alchemy::host::icu {
namespace {

Api load() noexcept {
    Api table;
    const auto library = LoadLibraryExW(L"icu.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library) return table;
    bool complete = true;
#define ALCHEMY_HOST_ICU_RESOLVE(name, result, parameters) \
    table.name = reinterpret_cast<result(*) parameters>(reinterpret_cast<void*>(GetProcAddress(library, #name))); \
    complete = complete && table.name != nullptr;
    ALCHEMY_HOST_ICU_FUNCTIONS(ALCHEMY_HOST_ICU_RESOLVE)
#undef ALCHEMY_HOST_ICU_RESOLVE
    if (table.u_getVersion) table.u_getVersion(table.version);
    // An older ICU that lacks a listed function, or predates the minimum, is no ICU
    // at all for the callers: they answer "unavailable" instead of calling a null entry.
    if (!complete || table.version[0] < minimum_major_version) table.ucnv_open = nullptr;
    return table;
}

} // namespace

const Api* api() noexcept {
    // The library stays loaded for the process's lifetime.
    static const Api table = load();
    return table.ucnv_open ? &table : nullptr;
}

} // namespace alchemy::host::icu
