// Accessibility (src/alchemy/darwin/accessibility/): the identity, and the documented
// "unavailable" answers.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

#define LIBRARY "/System/Library/Frameworks/Accessibility.framework/Versions/A/Accessibility"
[[maybe_unused]] constexpr std::string_view cf_lib = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

const Case accessibility_case("accessibility", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(LIBRARY), "Accessibility identity");
    const auto content = f.send(f.cls("AXCustomContent"), "customContentWithLabel:value:", {f.cf.string("Label"), f.cf.string("Value")});
    require(f.cf.utf8(f.send(content, "label")) == "Label" && f.cf.utf8(f.send(content, "value")) == "Value", "custom content");
});

#undef LIBRARY
} // namespace
