#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"
#include "alchemy/darwin_frameworks.hpp"

// Carbon (HIToolbox) Text Input Sources: the property keys (no keyboard
// input source exists).
// Moved from darwin/identities/frameworks.cpp (the headless framework set);
// registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

class InputSources final : public Provider {
public:
    using Provider::Provider;
    void install() {
        // Event state, input sources and UCKeyTranslate are AppKit's headless
        // shell (darwin/shells.hpp).
        string_symbol(carbon, "_kTISPropertyInputSourceIsASCIICapable", "TISPropertyInputSourceIsASCIICapable");
        string_symbol(carbon, "_kTISPropertyUnicodeKeyLayoutData", "TISPropertyUnicodeKeyLayoutData");
    }
};

} // namespace

void install_appkit_input_sources(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<InputSources>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
