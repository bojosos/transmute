// CryptoTokenKit (src/alchemy/darwin/cryptotokenkit/): the identity, and the documented
// "unavailable" answers.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

#define LIBRARY "/System/Library/Frameworks/CryptoTokenKit.framework/Versions/A/CryptoTokenKit"
[[maybe_unused]] constexpr std::string_view cf_lib = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

const Case cryptotokenkit_case("cryptotokenkit", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(LIBRARY), "CryptoTokenKit identity");
    const auto watcher = f.send(f.send(f.cls("TKTokenWatcher"), "alloc"), "init");
    require(f.call(cf_lib, "_CFArrayGetCount", {f.send(watcher, "tokenIDs")}) == 0, "no tokens");
});

#undef LIBRARY
} // namespace
