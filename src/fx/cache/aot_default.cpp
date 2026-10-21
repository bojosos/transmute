// aot_translate_image() bound to the real translator (owner: cache). Kept in
// its own object file so code that never calls it (the cache runtime, the
// stub-translator tests) does not need translate_region at link time.

#include "fx/cache/aot.hpp"
#include "fx/cache/cache_internal.hpp"

namespace transmute::fx {

std::size_t aot_translate_image(const std::wstring& path, const AotOptions& options, AotStats* stats,
                                std::wstring* error) {
    return aot_detail::translate_image(path, options, &translate_region, stats, nullptr, error);
}

std::size_t aot_translate_image(const std::wstring& path, std::wstring* error) {
    return aot_translate_image(path, AotOptions{}, nullptr, error);
}

void aot_external_module(const ExternalAotRequest& request) {
    cache_detail::external_aot_start(request, [](std::uint64_t pc, const CodeSource& code, const TranslateOptions& o) {
        return translate_region(pc, code, o);
    });
}

} // namespace transmute::fx
