#include "alchemy/darwin/quicklookui/install.hpp"
#include "alchemy/gui/value_provider.hpp"

#include <stdexcept>

// QuickLookUI: no Quick Look previews. QLPreviewPanel is a real NSPanel
// subclass; the shared panel never exists, and asking for it stops.
// Electron's framework (VS Code) first linked it.
namespace alchemy::quicklookui {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view quick_look_ui = "/System/Library/Frameworks/QuickLookUI.framework/Versions/A/QuickLookUI";

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    void install();
};

void Provider::install() {
    ProcessContext::add_provider_library(std::string(quick_look_ui));
    const auto no = [](CpuState& s, GuestMemory&) { s.x[0] = 0; };
    const auto preview_panel = cls(quick_look_ui, "QLPreviewPanel", "NSPanel");
    method(preview_panel, "sharedPreviewPanelExists", no, true, "B@:");
    method(preview_panel, "sharedPreviewPanel", [](CpuState&, GuestMemory&) {
        throw std::runtime_error("Alchemy: +[QLPreviewPanel sharedPreviewPanel] is unsupported (no Quick Look previews)");
    }, true);
}

} // namespace

void install_quicklookui(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::quicklookui
