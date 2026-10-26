#include "alchemy/gui/value_provider.hpp"

#include <stdexcept>
#include <string>

// SecurityInterface: the certificate, trust and identity panels. Their
// settings are real values; no certificate UI exists here, so the shared
// panels and every way of running a panel stop with an explicit
// "unsupported" naming the API. The Qt WebEngine images first imported the
// framework (moved from darwin/identities/system_gaps.cpp), VS Code's
// Electron framework imports SFChooseIdentityPanel; installed by the
// fill-only kit (kit/fwgaps/install.cpp) at the old position.
namespace alchemy::gui::fill {
namespace {

class SecurityInterface final : public values::Provider {
public:
    using values::Provider::Provider;
    void install();
};

[[noreturn]] void no_panels(const std::string& api) {
    throw std::runtime_error("Alchemy: " + api + " is unsupported (no certificate panels)");
}

void SecurityInterface::install() {
    const auto stop = [this](Address target, const char* selector, bool class_method, const char* name, const char* types) {
        const auto api = std::string(class_method ? "+[" : "-[") + name + " " + selector + "]";
        method(target, selector, [api](CpuState&, GuestMemory&) { no_panels(api); }, class_method, types);
    };
    // SFCertificatePanel: the settings every panel shares.
    const auto panel = cls(security_interface, "SFCertificatePanel", "NSPanel");
    cleanup_on_dealloc(panel);
    object_properties(panel, {"policies", "helpAnchor", "defaultButtonTitle", "alternateButtonTitle", "informativeText"});
    integer_properties(panel, {"showsHelp"});
    stop(panel, "sharedCertificatePanel", true, "SFCertificatePanel", "@@:");
    stop(panel, "runModalForCertificates:showGroup:", false, "SFCertificatePanel", "q@:@B");
    stop(panel, "beginSheetForWindow:modalDelegate:didEndSelector:contextInfo:certificates:showGroup:", false, "SFCertificatePanel",
         "v@:@@:^v@B");
    // SFCertificateTrustPanel: a trust evaluation's certificates.
    const auto trust_panel = cls(security_interface, "SFCertificateTrustPanel", "SFCertificatePanel");
    stop(trust_panel, "sharedCertificateTrustPanel", true, "SFCertificateTrustPanel", "@@:");
    stop(trust_panel, "runModalForTrust:message:", false, "SFCertificateTrustPanel", "q@:^{__SecTrust=}@");
    stop(trust_panel, "beginSheetForWindow:modalDelegate:didEndSelector:contextInfo:trust:message:", false, "SFCertificateTrustPanel",
         "v@:@@:^v^{__SecTrust=}@");
    // SFChooseIdentityPanel: no identity is chosen until a panel runs.
    const auto choose = cls(security_interface, "SFChooseIdentityPanel", "NSPanel");
    cleanup_on_dealloc(choose);
    object_properties(choose, {"policies", "helpAnchor", "alternateButtonTitle", "informativeText", "domain"});
    integer_properties(choose, {"showsHelp"});
    method(choose, "identity", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "^{OpaqueSecIdentityRef=}@:");
    stop(choose, "sharedChooseIdentityPanel", true, "SFChooseIdentityPanel", "@@:");
    stop(choose, "runModalForIdentities:message:", false, "SFChooseIdentityPanel", "q@:@@");
    stop(choose, "beginSheetForWindow:modalDelegate:didEndSelector:contextInfo:identities:message:", false, "SFChooseIdentityPanel",
         "v@:@@:^v@@");
}

} // namespace

void install_securityinterface(InstallContext& context) { std::make_shared<SecurityInterface>(context)->install(); }

} // namespace alchemy::gui::fill
