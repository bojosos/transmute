// Foundation pieces moved from darwin/identities/framework_gaps.cpp
// (file_attribute_keys.cpp): NSFileModificationDate.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

constexpr std::string_view foundation = "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation";

const Case foundation_file_attribute_keys_case("foundation_file_attribute_keys", [](Fixture& f) {
    require(f.string_constant(foundation, "_NSFileModificationDate") == "NSFileModificationDate", "NSFileModificationDate");
});

} // namespace
