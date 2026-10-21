// CoreData (src/alchemy/darwin/coredata/): the identity, with no exports.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

#define LIBRARY "/System/Library/Frameworks/CoreData.framework/Versions/A/CoreData"

const Case coredata_case("coredata", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(LIBRARY), "CoreData identity");
    require(!f.process.resolve(LIBRARY, "_NSSQLiteStoreType"), "no exports");
});

#undef LIBRARY
} // namespace
