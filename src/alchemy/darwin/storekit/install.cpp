#include "alchemy/darwin/storekit/install.hpp"
#include "alchemy/gui/value_provider.hpp"

#include <stdexcept>

// StoreKit: no App Store. Payments are not allowed (+canMakePayments is NO),
// the payment queue never has a transaction, so its observers are never
// called; payments, restores and product requests stop. Payments are real
// value objects. Electron's update framework (VS Code) first linked it.
namespace alchemy::storekit {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view store_kit = "/System/Library/Frameworks/StoreKit.framework/Versions/A/StoreKit";

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    void install();
};

void Provider::install() {
    ProcessContext::add_provider_library(std::string(store_kit));
    const auto no = [](CpuState& s, GuestMemory&) { s.x[0] = 0; };
    const auto nothing = [](CpuState&, GuestMemory&) {};
    const auto empty_array = [this](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(cf.array({})); };
    const auto stop = [](const char* what) {
        return [what](CpuState&, GuestMemory&) { throw std::runtime_error(std::string("Alchemy: ") + what + " is unsupported (no App Store)"); };
    };
    string(store_kit, "_SKErrorDomain", "SKErrorDomain");
    const auto payment_queue = cls(store_kit, "SKPaymentQueue", "NSObject");
    singleton(payment_queue, "defaultQueue");
    method(payment_queue, "canMakePayments", no, true, "B@:");
    method(payment_queue, "transactions", empty_array);
    method(payment_queue, "addTransactionObserver:", nothing, false, "v@:@");
    method(payment_queue, "removeTransactionObserver:", nothing, false, "v@:@");
    method(payment_queue, "addPayment:", stop("-[SKPaymentQueue addPayment:]"), false, "v@:@");
    method(payment_queue, "restoreCompletedTransactions", stop("-[SKPaymentQueue restoreCompletedTransactions]"), false, "v@:");
    method(payment_queue, "restoreCompletedTransactionsWithApplicationUsername:",
           stop("-[SKPaymentQueue restoreCompletedTransactionsWithApplicationUsername:]"), false, "v@:@");
    const auto store_request = cls(store_kit, "SKRequest", "NSObject");
    weak_properties(store_request, {"delegate"});
    method(store_request, "start", stop("-[SKRequest start]"), false, "v@:");
    method(store_request, "cancel", nothing, false, "v@:");
    const auto products_request = cls(store_kit, "SKProductsRequest", "SKRequest");
    method(products_request, "initWithProductIdentifiers:", [this](CpuState& s, GuestMemory&) {
        store(s.x[0], "productIdentifiers", s.x[2]);
    });
    for (const auto* name : {"SKPayment", "SKMutablePayment"}) {
        const auto payment = cls(store_kit, name, std::string_view(name) == "SKPayment" ? "NSObject" : "SKPayment");
        object_properties(payment, {"productIdentifier", "applicationUsername", "requestData"});
        integer_properties(payment, {"quantity", "simulatesAskToBuyInSandbox"});
    }
}

} // namespace

void install_storekit(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::storekit
