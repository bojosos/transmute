#include "alchemy/darwin/storekit/install.hpp"
#include "alchemy/gui/value_provider.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

// StoreKit: no App Store. Payments are not allowed (+canMakePayments is NO),
// so the documented failures arrive through the usual callbacks, on the
// main queue:
//   - -addPayment: adds a transaction in SKPaymentTransactionStateFailed
//     with SKErrorPaymentNotAllowed; the queue's observers hear
//     paymentQueue:updatedTransactions:, and -finishTransaction: removes it
//     (paymentQueue:removedTransactions:).
//   - restores fail: paymentQueue:restoreCompletedTransactionsFailedWithError:
//     with SKErrorUnknown ("Cannot connect to the App Store").
//   - product and receipt requests fail: the delegate hears
//     request:didFailWithError: with SKErrorUnknown.
// Payments, products and transactions are real value objects; the
// storefront is unknown (nil). Electron's update framework (VS Code) first
// linked it.
namespace alchemy::storekit {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view store_kit = "/System/Library/Frameworks/StoreKit.framework/Versions/A/StoreKit";
constexpr std::int64_t sk_error_unknown = 0, sk_error_payment_not_allowed = 3;
constexpr std::int64_t transaction_state_failed = 2;   // SKPaymentTransactionStateFailed

// The default queue's observers (unretained, as the queue keeps them) and transactions (+1).
struct QueueState {
    std::mutex mutex;
    std::vector<Address> observers, transactions;
};

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    void install();

    // Sends `selector` with (x2, x3) to each observer that implements it, on the main queue.
    void notify(const std::shared_ptr<QueueState>& state, Address queue, const char* selector, Address argument) {
        static_cast<void>(retain(argument));
        bridge->dispatch_async(bridge->main_queue(), [this, state, queue, selector, argument] {
            std::vector<Address> observers;
            {
                std::lock_guard lock(state->mutex);
                observers = state->observers;
            }
            for (const auto observer : observers) {
                CallRegs registers{};
                registers.x[2] = queue;
                registers.x[3] = argument;
                static_cast<void>(objc.send_if_responds(observer, objc.selector(selector), registers));
            }
            release(argument);
        });
    }
};

void Provider::install() {
    ProcessContext::add_provider_library(std::string(store_kit));
    const auto no = [](CpuState& s, GuestMemory&) { s.x[0] = 0; };
    const auto nothing = [](CpuState&, GuestMemory&) {};
    string(store_kit, "_SKErrorDomain", "SKErrorDomain");
    auto state = std::make_shared<QueueState>();

    // Value classes.
    const auto product = cls(store_kit, "SKProduct", "NSObject");
    object_properties(product, {"productIdentifier", "localizedTitle", "localizedDescription", "price", "priceLocale",
                                "subscriptionPeriod", "introductoryPrice", "subscriptionGroupIdentifier", "discounts"});
    integer_properties(product, {"isDownloadable", "isFamilyShareable"});
    for (const auto* name : {"SKPayment", "SKMutablePayment"}) {
        const auto payment = cls(store_kit, name, std::string_view(name) == "SKPayment" ? "NSObject" : "SKPayment");
        object_properties(payment, {"productIdentifier", "applicationUsername", "requestData", "paymentDiscount"});
        integer_properties(payment, {"quantity", "simulatesAskToBuyInSandbox"});
    }
    // +[SKPayment paymentWithProduct:] / +[SKMutablePayment paymentWithProduct:]: quantity 1.
    for (const auto* name : {"SKPayment", "SKMutablePayment"}) {
        const auto target = objc.find_class(name);
        method(target, "paymentWithProduct:", [this, target](CpuState& s, GuestMemory&) {
            const auto payment = create(target);
            if (s.x[2]) store(payment, "productIdentifier", stored(s.x[2], "productIdentifier"));
            store_integer(payment, "quantity", 1);
            s.x[0] = bridge->autorelease(payment);
        }, true);
    }
    const auto transaction_class = cls(store_kit, "SKPaymentTransaction", "NSObject");
    object_properties(transaction_class, {"payment", "error", "transactionIdentifier", "transactionDate", "originalTransaction", "downloads"});
    integer_properties(transaction_class, {"transactionState"});
    const auto response = cls(store_kit, "SKProductsResponse", "NSObject");
    object_properties(response, {"products", "invalidProductIdentifiers"});

    // The payment queue.
    const auto payment_queue = cls(store_kit, "SKPaymentQueue", "NSObject");
    singleton(payment_queue, "defaultQueue");
    method(payment_queue, "canMakePayments", no, true, "B@:");
    method(payment_queue, "storefront", no);   // no App Store account: unknown
    method(payment_queue, "transactions", [this, state](CpuState& s, GuestMemory&) {
        std::vector<Address> transactions;
        {
            std::lock_guard lock(state->mutex);
            transactions = state->transactions;
        }
        s.x[0] = bridge->autorelease(cf.array(transactions));
    });
    method(payment_queue, "addTransactionObserver:", [this, state](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        if (s.x[2] && std::find(state->observers.begin(), state->observers.end(), s.x[2]) == state->observers.end())
            state->observers.push_back(s.x[2]);
    }, false, "v@:@");
    method(payment_queue, "removeTransactionObserver:", [this, state](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        std::erase(state->observers, s.x[2]);
    }, false, "v@:@");
    method(payment_queue, "transactionObservers", [this, state](CpuState& s, GuestMemory&) {
        std::vector<Address> observers;
        {
            std::lock_guard lock(state->mutex);
            observers = state->observers;
        }
        s.x[0] = bridge->autorelease(cf.array(observers));
    });
    // -addPayment: a failed transaction (payments are not allowed).
    method(payment_queue, "addPayment:", [this, state, transaction_class](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;
        const auto queue = s.x[0];
        const auto transaction = create(transaction_class);
        store(transaction, "payment", s.x[2]);
        const auto error = bridge->error("SKErrorDomain", sk_error_payment_not_allowed, "Payments are not allowed (no App Store)");
        store(transaction, "error", error);
        release(error);
        store_integer(transaction, "transactionState", transaction_state_failed);
        {
            std::lock_guard lock(state->mutex);
            state->transactions.push_back(transaction);   // the queue's reference
        }
        const auto list = cf.array(std::vector<Address>{transaction});
        notify(state, queue, "paymentQueue:updatedTransactions:", list);
        release(list);
    }, false, "v@:@");
    method(payment_queue, "finishTransaction:", [this, state](CpuState& s, GuestMemory&) {
        bool found{};
        {
            std::lock_guard lock(state->mutex);
            const auto at = std::find(state->transactions.begin(), state->transactions.end(), s.x[2]);
            if (at != state->transactions.end()) { state->transactions.erase(at); found = true; }
        }
        if (!found) return;
        const auto list = cf.array(std::vector<Address>{s.x[2]});
        notify(state, s.x[0], "paymentQueue:removedTransactions:", list);
        release(list);
        release(s.x[2]);   // the queue's reference
    }, false, "v@:@");
    const auto restore = [this, state](CpuState& s, GuestMemory&) {
        const auto error = bridge->error("SKErrorDomain", sk_error_unknown, "Cannot connect to the App Store");
        notify(state, s.x[0], "paymentQueue:restoreCompletedTransactionsFailedWithError:", error);
        release(error);
    };
    method(payment_queue, "restoreCompletedTransactions", restore, false, "v@:");
    method(payment_queue, "restoreCompletedTransactionsWithApplicationUsername:", restore, false, "v@:@");

    // Requests fail: request:didFailWithError: on the delegate.
    const auto store_request = cls(store_kit, "SKRequest", "NSObject");
    weak_properties(store_request, {"delegate"});
    method(store_request, "start", [this](CpuState& s, GuestMemory&) {
        const auto request = retain(s.x[0]);
        bridge->dispatch_async(bridge->main_queue(), [this, request] {
            if (const auto delegate = static_cast<Address>(stored_integer(request, "delegate"))) {
                const auto error = bridge->error("SKErrorDomain", sk_error_unknown, "Cannot connect to the App Store");
                CallRegs registers{};
                registers.x[2] = request;
                registers.x[3] = error;
                static_cast<void>(objc.send_if_responds(delegate, objc.selector("request:didFailWithError:"), registers));
                release(error);
            }
            release(request);
        });
    }, false, "v@:");
    method(store_request, "cancel", nothing, false, "v@:");
    const auto products_request = cls(store_kit, "SKProductsRequest", "SKRequest");
    method(products_request, "initWithProductIdentifiers:", [this](CpuState& s, GuestMemory&) {
        store(s.x[0], "productIdentifiers", s.x[2]);
    });
    const auto receipt_request = cls(store_kit, "SKReceiptRefreshRequest", "SKRequest");
    object_properties(receipt_request, {"receiptProperties"});
    method(receipt_request, "initWithReceiptProperties:", [this](CpuState& s, GuestMemory&) {
        store(s.x[0], "receiptProperties", s.x[2]);
    });
}

} // namespace

void install_storekit(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::storekit
