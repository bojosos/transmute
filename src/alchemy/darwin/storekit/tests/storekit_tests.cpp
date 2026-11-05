// StoreKit (src/alchemy/darwin/storekit/): the identity, and the documented
// "unavailable" answers through the usual callbacks.
#include "alchemy/gui/tests/framework_case.hpp"
#include "alchemy/darwin/security/bridge.hpp"

#include <atomic>

namespace {
using namespace fill_test;
using transmute::CpuState;
using transmute::GuestMemory;

#define LIBRARY "/System/Library/Frameworks/StoreKit.framework/Versions/A/StoreKit"
constexpr std::string_view cf_lib = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

const Case storekit_case("storekit", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(LIBRARY), "StoreKit identity");
    const auto queue = f.send(f.cls("SKPaymentQueue"), "defaultQueue");
    require(f.send(f.cls("SKPaymentQueue"), "canMakePayments") == 0 && queue == f.send(f.cls("SKPaymentQueue"), "defaultQueue") &&
            f.call(cf_lib, "_CFArrayGetCount", {f.send(queue, "transactions")}) == 0, "no payments, no transactions");
    const auto payment = f.send(f.send(f.cls("SKMutablePayment"), "alloc"), "init");
    static_cast<void>(f.send(payment, "setQuantity:", {2}));
    static_cast<void>(f.send(payment, "setProductIdentifier:", {f.cf.string("com.example.item")}));
    require(f.send(payment, "quantity") == 2, "SKMutablePayment is a value object");

    // An observer and a request delegate (guest classes over the Security bridge).
    const auto bridge = alchemy::gui::security::bridge();
    static std::atomic<Address> updated{}, removed{}, restore_error{}, request_error{};
    const auto observer_class = bridge->define_class("AlchemyTestStoreObserver", bridge->object_class, LIBRARY);
    bridge->method(observer_class, "paymentQueue:updatedTransactions:", [](CpuState& s, GuestMemory&) { updated = alchemy::gui::security::bridge()->retain(s.x[3]); }, false, "v@:@@");
    bridge->method(observer_class, "paymentQueue:removedTransactions:", [](CpuState& s, GuestMemory&) { removed = alchemy::gui::security::bridge()->retain(s.x[3]); }, false, "v@:@@");
    bridge->method(observer_class, "paymentQueue:restoreCompletedTransactionsFailedWithError:",
                   [](CpuState& s, GuestMemory&) { restore_error = alchemy::gui::security::bridge()->retain(s.x[3]); }, false, "v@:@@");
    bridge->method(observer_class, "request:didFailWithError:", [](CpuState& s, GuestMemory&) { request_error = alchemy::gui::security::bridge()->retain(s.x[3]); }, false, "v@:@@");
    const auto observer = f.send(f.send(observer_class, "alloc"), "init");
    static_cast<void>(f.send(queue, "addTransactionObserver:", {observer}));
    const auto mode = f.engine.read64(f.symbol(cf_lib, "_kCFRunLoopDefaultMode"));
    const auto spin = [&](const std::atomic<Address>& until) {
        for (int round = 0; round < 50 && !until.load(); ++round) {
            alchemy::CallRegs run;
            run.x[0] = mode;
            run.set_d(0, 0.02);
            run.x[1] = 1;
            static_cast<void>(f.regs(cf_lib, "_CFRunLoopRunInMode", run));
        }
        return until.load();
    };

    // A payment fails as not allowed.
    static_cast<void>(f.send(queue, "addPayment:", {payment}));
    const auto list = spin(updated);
    require(list && f.call(cf_lib, "_CFArrayGetCount", {list}) == 1, "paymentQueue:updatedTransactions:");
    const auto transaction = f.call(cf_lib, "_CFArrayGetValueAtIndex", {list, 0});
    require(f.send(transaction, "transactionState") == 2 && f.send(transaction, "payment") != 0, "a failed transaction");
    const auto error = f.send(transaction, "error");
    require(f.send(error, "code") == 3 && f.cf.utf8(f.send(error, "domain")) == "SKErrorDomain", "SKErrorPaymentNotAllowed");
    require(f.call(cf_lib, "_CFArrayGetCount", {f.send(queue, "transactions")}) == 1, "the queue holds it");
    static_cast<void>(f.send(queue, "finishTransaction:", {transaction}));
    require(spin(removed) && f.call(cf_lib, "_CFArrayGetCount", {f.send(queue, "transactions")}) == 0, "finishTransaction:");
    // Restores fail.
    static_cast<void>(f.send(queue, "restoreCompletedTransactions"));
    require(spin(restore_error) && f.cf.utf8(f.send(restore_error.load(), "domain")) == "SKErrorDomain", "restore fails");
    // Product requests fail through the delegate.
    const auto identifiers = f.cf.array(std::vector<Address>{f.cf.string("com.example.item")});
    const auto request = f.send(f.send(f.cls("SKProductsRequest"), "alloc"), "initWithProductIdentifiers:", {identifiers});
    static_cast<void>(f.send(request, "setDelegate:", {observer}));
    static_cast<void>(f.send(request, "start"));
    require(spin(request_error) && f.send(request_error.load(), "code") == 0, "request:didFailWithError:");
    static_cast<void>(f.send(queue, "removeTransactionObserver:", {observer}));
    require(f.call(cf_lib, "_CFArrayGetCount", {f.send(queue, "transactionObservers")}) == 0, "removeTransactionObserver:");
    // +paymentWithProduct:
    const auto product = f.send(f.send(f.cls("SKProduct"), "alloc"), "init");
    static_cast<void>(f.send(product, "setProductIdentifier:", {f.cf.string("com.example.other")}));
    const auto from_product = f.send(f.cls("SKPayment"), "paymentWithProduct:", {product});
    require(f.cf.utf8(f.send(from_product, "productIdentifier")) == "com.example.other" && f.send(from_product, "quantity") == 1, "paymentWithProduct:");
});

#undef LIBRARY
} // namespace
