// StoreKit (src/alchemy/darwin/storekit/): the identity, and the documented
// "unavailable" answers.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

#define LIBRARY "/System/Library/Frameworks/StoreKit.framework/Versions/A/StoreKit"
[[maybe_unused]] constexpr std::string_view cf_lib = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

const Case storekit_case("storekit", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(LIBRARY), "StoreKit identity");
    const auto queue = f.send(f.cls("SKPaymentQueue"), "defaultQueue");
    require(f.send(f.cls("SKPaymentQueue"), "canMakePayments") == 0 && queue == f.send(f.cls("SKPaymentQueue"), "defaultQueue") &&
            f.call(cf_lib, "_CFArrayGetCount", {f.send(queue, "transactions")}) == 0, "no payments, no transactions");
    const auto payment = f.send(f.send(f.cls("SKMutablePayment"), "alloc"), "init");
    static_cast<void>(f.send(payment, "setQuantity:", {2}));
    require(f.send(payment, "quantity") == 2, "SKMutablePayment is a value object");
    bool stopped{};
    try { static_cast<void>(f.send(queue, "addPayment:", {payment})); }
    catch (const std::exception& error) { stopped = std::string(error.what()).find("no App Store") != std::string::npos; }
    require(stopped, "addPayment: stops");
});

#undef LIBRARY
} // namespace
