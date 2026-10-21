// QuickLookUI (src/alchemy/darwin/quicklookui/): the identity, and the documented
// "unavailable" answers.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

#define LIBRARY "/System/Library/Frameworks/QuickLookUI.framework/Versions/A/QuickLookUI"

const Case quicklookui_case("quicklookui", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(LIBRARY), "QuickLookUI identity");
    const auto panel = f.cls("QLPreviewPanel");
    require(f.send(panel, "superclass") == f.cls("NSPanel"), "QLPreviewPanel is an NSPanel");
    require(f.send(panel, "sharedPreviewPanelExists") == 0, "no shared preview panel");
    bool stopped{};
    try { static_cast<void>(f.send(panel, "sharedPreviewPanel")); }
    catch (const std::exception& error) { stopped = std::string(error.what()).find("no Quick Look previews") != std::string::npos; }
    require(stopped, "+sharedPreviewPanel stops");
});

#undef LIBRARY
} // namespace
