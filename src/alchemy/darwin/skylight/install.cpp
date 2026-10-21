#include "alchemy/darwin/skylight/install.hpp"
#include "alchemy/gui/value_provider.hpp"

// SkyLight (private, weakly linked): an identity with no exports, so its weak
// imports bind to NULL and callers take their public-API path (Chrome
// takes its CVDisplayLink path).
// Chrome 155 first linked the framework (moved from
// darwin/identities/identities.cpp).
namespace alchemy::skylight {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view sky_light = "/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/SkyLight";

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    void install();
};

void Provider::install() {
    ProcessContext::add_provider_library(std::string(sky_light));
}

} // namespace

void install_skylight(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::skylight
