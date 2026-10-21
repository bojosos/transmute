#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"

// AppKit (headless-correct): +[NSColor setIgnoresAlpha:] and +ignoresAlpha, a
// process-wide flag that only colour panels read.
// Moved from darwin/identities/installer_frameworks.cpp (an installer
// binds them first); registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

class ColorAlpha final : public Provider {
public:
    using Provider::Provider;
    bool ignores_alpha{true};
    void install() {
        // +[NSColor setIgnoresAlpha:]/+ignoresAlpha: a process-wide flag that
        // only colour panels read.
        method("NSColor", "setIgnoresAlpha:", [this](CpuState& s, GuestMemory&) { ignores_alpha = (s.x[2] & 0xff) != 0; },
               true, "v@:c");
        method("NSColor", "ignoresAlpha", [this](CpuState& s, GuestMemory&) { s.x[0] = ignores_alpha; }, true, "c@:");
    }
};

} // namespace

void install_appkit_color_alpha(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<ColorAlpha>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
