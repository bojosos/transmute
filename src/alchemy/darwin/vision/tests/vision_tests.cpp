// Vision (src/alchemy/darwin/vision/): the identity, and the documented
// "unavailable" answers.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

#define LIBRARY "/System/Library/Frameworks/Vision.framework/Versions/A/Vision"
[[maybe_unused]] constexpr std::string_view cf_lib = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

const Case vision_case("vision", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(LIBRARY), "Vision identity");
    const auto error = f.block(8);
    const auto handler = f.send(f.send(f.cls("VNImageRequestHandler"), "alloc"), "initWithCGImage:options:", {0, 0});
    require(f.send(handler, "performRequests:error:", {f.cf.array({}), error}) == 0 && f.engine.read64(error), "Vision fails");
    require(f.string_constant(LIBRARY, "_VNErrorDomain") == "com.apple.Vision", "error domain");
});

#undef LIBRARY
} // namespace
