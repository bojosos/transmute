#include "alchemy/darwin/localauthenticationembeddedui/install.hpp"
#include "alchemy/gui/value_provider.hpp"

// LocalAuthenticationEmbeddedUI: LAAuthenticationView is a plain NSView
// that keeps its LAContext.
// Chrome 155 first linked the framework (moved from
// darwin/identities/identities.cpp).
namespace alchemy::localauthenticationembeddedui {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view local_authentication_embedded_ui = "/System/Library/Frameworks/LocalAuthenticationEmbeddedUI.framework/Versions/A/LocalAuthenticationEmbeddedUI";

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    void install();
};

void Provider::install() {
    ProcessContext::add_provider_library(std::string(local_authentication_embedded_ui));
    const auto authentication_view = cls(local_authentication_embedded_ui, "LAAuthenticationView", "NSView");
    object_properties(authentication_view, {"context"});
    method(authentication_view, "initWithContext:", [this](CpuState& s, GuestMemory&) {
        CallRegs registers{};
        const auto self = s.x[0], context = s.x[2];
        const auto view = objc.send_super(self, objc.find_class("LAAuthenticationView"), objc.selector("initWithFrame:"), registers).x0;
        if (view) store(view, "context", context);
        s.x[0] = view;
    });
}

} // namespace

void install_localauthenticationembeddedui(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::localauthenticationembeddedui
