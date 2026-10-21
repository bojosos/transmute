// ServiceManagement (src/alchemy/darwin/servicemanagement/): SMJobBless fails (no launchd).
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

constexpr std::string_view sm = "/System/Library/Frameworks/ServiceManagement.framework/Versions/A/ServiceManagement";

const Case servicemanagement_case("servicemanagement", [](Fixture& f) {
    const auto error = f.block(8);
    require(f.call(sm, "_SMJobBless", {0, f.cf.string("com.example.helper"), 0, error}) == 0 && f.engine.read64(error), "SMJobBless fails with an error");
});

} // namespace
