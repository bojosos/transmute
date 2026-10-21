#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"

// CoreGraphics (headless-correct): CGEventSourceKeyState (no keyboard).
// Moved from darwin/identities/installer_frameworks.cpp (an installer
// binds them first); registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

class EventSource final : public Provider {
public:
    using Provider::Provider;
    void install() {
        // The CGDisplay* queries are CoreGraphics' headless shell (darwin/shells.hpp).
        function(core_graphics, "_CGEventSourceKeyState", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }); // no keyboard
    }
};

} // namespace

void install_coregraphics_event_source(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<EventSource>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
