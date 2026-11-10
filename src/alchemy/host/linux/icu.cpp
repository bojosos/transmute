// Linux host backend, area "icu" (include/alchemy/host/icu.hpp): the distribution's ICU4C,
// libicuuc.so.NN and libicui18n.so.NN opened at run time (icu_library.hpp), its C names
// resolved with the library's version suffix.
#include "alchemy/host/icu.hpp"

#include "icu_library.hpp"

namespace alchemy::host::icu {
namespace {

Api load() noexcept {
    Api table;
    if (!linux_icu::library().common) return table;
    bool complete = true;
#define ALCHEMY_HOST_ICU_RESOLVE(name, result, parameters) \
    table.name = reinterpret_cast<result(*) parameters>(linux_icu::symbol(#name)); \
    complete = complete && table.name != nullptr;
    ALCHEMY_HOST_ICU_FUNCTIONS(ALCHEMY_HOST_ICU_RESOLVE)
#undef ALCHEMY_HOST_ICU_RESOLVE
    if (table.u_getVersion) table.u_getVersion(table.version);
    // As on Windows: an ICU missing a listed function, or older than the minimum, is no ICU.
    if (!complete || table.version[0] < minimum_major_version) table.ucnv_open = nullptr;
    return table;
}

} // namespace

const Api* api() noexcept {
    static const Api table = load();
    return table.ucnv_open ? &table : nullptr;
}

} // namespace alchemy::host::icu
