#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"
#include "alchemy/darwin_frameworks.hpp"

// CoreGraphics (headless-correct): zero online displays, so per-display
// queries answer for no display.
// Moved from darwin/identities/frameworks.cpp (the headless framework set);
// registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

class NoDisplays final : public Provider {
public:
    using Provider::Provider;
    void install() {
        // Named color spaces, the display list and the cursor are CoreGraphics'
        // and AppKit's headless shells (darwin/shells.hpp). Zero online
        // displays: per-display queries answer for no display.
        function(core_graphics, "_CGDisplayIOServicePort", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
        function(core_graphics, "_CGDisplayUnitNumber", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    }
};

} // namespace

void install_coregraphics_no_displays(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<NoDisplays>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
