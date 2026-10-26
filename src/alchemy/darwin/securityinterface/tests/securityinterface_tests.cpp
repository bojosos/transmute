// SecurityInterface (src/alchemy/darwin/securityinterface/): the certificate,
// trust and identity panels keep their settings; showing one stops.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

bool stops(Fixture& f, Address receiver, std::string_view selector) {
    try { static_cast<void>(f.send(receiver, selector)); }
    catch (const std::exception& error) { return std::string(error.what()).find("no certificate panels") != std::string::npos; }
    return false;
}

const Case securityinterface_case("securityinterface", [](Fixture& f) {
    const auto panel = f.cls("SFCertificatePanel"), trust = f.cls("SFCertificateTrustPanel"), choose = f.cls("SFChooseIdentityPanel");
    require(f.send(panel, "superclass") == f.cls("NSPanel") && f.send(trust, "superclass") == panel &&
                f.send(choose, "superclass") == f.cls("NSPanel"), "the panel classes");
    require(stops(f, panel, "sharedCertificatePanel") && stops(f, trust, "sharedCertificateTrustPanel") &&
                stops(f, choose, "sharedChooseIdentityPanel"), "the shared panels stop");
    const auto chooser = f.send(choose, "alloc");   // settings need no window
    static_cast<void>(f.send(chooser, "setInformativeText:", {f.cf.string("Pick one")}));
    require(f.cf.utf8(f.send(chooser, "informativeText")) == "Pick one", "panel settings are kept");
    require(f.send(chooser, "identity") == 0, "no identity was chosen");
    require(stops(f, chooser, "runModalForIdentities:message:"), "running the panel stops");
});

} // namespace
