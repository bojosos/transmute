#include "alchemy/gui/fill_provider.hpp"

// SecurityInterface: the certificate panel is a class shell (a real NSPanel
// subclass with no methods of its own); asking for the shared trust panel stops.
// The Qt WebEngine images first imported it (moved from
// darwin/identities/system_gaps.cpp); installed by the fill-only kit
// (kit/fwgaps/install.cpp) at the old position.
namespace alchemy::gui::fill {
namespace {

class SecurityInterface final : public Provider {
public:
    using Provider::Provider;
    void install();
};

void SecurityInterface::install() {
    // SecurityInterface: the certificate panel is a class shell.
    static_cast<void>(cls(security_interface, "SFCertificatePanel", "NSPanel"));
    // SFCertificateTrustPanel: a real subclass; the shared panel cannot be
    // shown (no certificate UI), so asking for it stops.
    const auto trust_panel = cls(security_interface, "SFCertificateTrustPanel", "SFCertificatePanel");
    method(trust_panel, "sharedCertificateTrustPanel", [](CpuState&, GuestMemory&) {
        throw std::runtime_error("Alchemy: +[SFCertificateTrustPanel sharedCertificateTrustPanel] is unsupported (no certificate panels)");
    }, true);
}

} // namespace

void install_securityinterface(InstallContext& context) { std::make_shared<SecurityInterface>(context)->install(); }

} // namespace alchemy::gui::fill
