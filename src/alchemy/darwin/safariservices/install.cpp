#include "alchemy/darwin/safariservices/install.hpp"
#include "alchemy/gui/value_provider.hpp"

// SafariServices: no application claims a universal link (the initializer
// returns nil, as documented).
// Chrome 155 first linked the framework (moved from
// darwin/identities/identities.cpp).
namespace alchemy::safariservices {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view safari_services = "/System/Library/Frameworks/SafariServices.framework/Versions/A/SafariServices";

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    void install();
};

void Provider::install() {
    ProcessContext::add_provider_library(std::string(safari_services));
    const auto universal_link = cls(safari_services, "SFUniversalLink", "NSObject");
    method(universal_link, "initWithWebpageURL:", [this](CpuState& s, GuestMemory&) {
        release(s.x[0]);
        s.x[0] = 0;
    });
}

} // namespace

void install_safariservices(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::safariservices
