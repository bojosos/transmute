// CoreML (src/alchemy/darwin/coreml/): the identity, and the documented
// "unavailable" answers.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

#define LIBRARY "/System/Library/Frameworks/CoreML.framework/Versions/A/CoreML"
[[maybe_unused]] constexpr std::string_view cf_lib = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

const Case coreml_case("coreml", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(LIBRARY), "CoreML identity");
    const auto error = f.block(8);
    require(f.send(f.cls("MLModel"), "modelWithContentsOfURL:error:", {f.cf.string("x"), error}) == 0 && f.engine.read64(error), "MLModel fails");
});

#undef LIBRARY
} // namespace
