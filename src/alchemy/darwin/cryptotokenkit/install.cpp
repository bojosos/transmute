#include "alchemy/darwin/cryptotokenkit/install.hpp"
#include "alchemy/gui/value_provider.hpp"

// CryptoTokenKit: no smart-card token is ever present.
// Chrome 155 first linked the framework (moved from
// darwin/identities/identities.cpp).
namespace alchemy::cryptotokenkit {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view crypto_token_kit = "/System/Library/Frameworks/CryptoTokenKit.framework/Versions/A/CryptoTokenKit";

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    void install();
};

void Provider::install() {
    ProcessContext::add_provider_library(std::string(crypto_token_kit));
    const auto empty_array = [this](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(cf.array({})); };
    const auto nothing = [](CpuState&, GuestMemory&) {};
    const auto token_watcher = cls(crypto_token_kit, "TKTokenWatcher", "NSObject");
    method(token_watcher, "tokenIDs", empty_array);
    method(token_watcher, "initWithInsertionHandler:", [](CpuState&, GuestMemory&) {});   // never called: no token appears
    method(token_watcher, "setInsertionHandler:", nothing, false, "v@:@?");
    method(token_watcher, "addRemovalHandler:forTokenID:", nothing, false, "v@:@?@");   // the token does not exist
}

} // namespace

void install_cryptotokenkit(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::cryptotokenkit
