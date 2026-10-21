// SecurityInterface (src/alchemy/darwin/securityinterface/): the certificate panel is a
// class shell.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;


const Case securityinterface_case("securityinterface", [](Fixture& f) {
    require(f.send(f.cls("SFCertificatePanel"), "superclass") == f.cls("NSPanel"), "SFCertificatePanel is an NSPanel");
});

} // namespace
