// ScreenCaptureKit (src/alchemy/darwin/screencapturekit/): the identity, the
// constants and the picker singleton (own-content capture: own_content_tests.cpp).
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

#define LIBRARY "/System/Library/Frameworks/ScreenCaptureKit.framework/Versions/A/ScreenCaptureKit"
[[maybe_unused]] constexpr std::string_view cf_lib = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

const Case screencapturekit_case("screencapturekit", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(LIBRARY), "ScreenCaptureKit identity");
    require(f.string_constant(LIBRARY, "_SCStreamErrorDomain") == "com.apple.ScreenCaptureKit.SCStreamErrorDomain", "error domain");
    require(f.string_constant(LIBRARY, "_SCStreamFrameInfoDirtyRects") == "SCStreamFrameInfoDirtyRects" &&
                f.string_constant(LIBRARY, "_SCStreamFrameInfoPresenterOverlayContentRect") == "SCStreamFrameInfoPresenterOverlayContentRect",
            "frame info keys");
    const auto picker = f.send(f.cls("SCContentSharingPicker"), "sharedPicker");
    require(picker && picker == f.send(f.cls("SCContentSharingPicker"), "sharedPicker"), "picker singleton");
    // No window exists here: the own content lists none (own_content_tests.cpp shows windows).
    require(f.call("/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics", "_CGPreflightScreenCaptureAccess") == 1,
            "screen capture granted for own content");
});

#undef LIBRARY
} // namespace
