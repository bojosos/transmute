// ScreenCaptureKit (src/alchemy/darwin/screencapturekit/): the identity, and the documented
// "unavailable" answers.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

#define LIBRARY "/System/Library/Frameworks/ScreenCaptureKit.framework/Versions/A/ScreenCaptureKit"
[[maybe_unused]] constexpr std::string_view cf_lib = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

const Case screencapturekit_case("screencapturekit", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(LIBRARY), "ScreenCaptureKit identity");
    require(f.string_constant(LIBRARY, "_SCStreamErrorDomain") == "com.apple.ScreenCaptureKit.SCStreamErrorDomain", "error domain");
    const auto picker = f.send(f.cls("SCContentSharingPicker"), "sharedPicker");
    require(picker && picker == f.send(f.cls("SCContentSharingPicker"), "sharedPicker"), "picker singleton");
});

#undef LIBRARY
} // namespace
