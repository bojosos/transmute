#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"

// QuartzCore: kCAContentsFormatRGBA8Uint.
// Moved from darwin/identities/installer_frameworks.cpp (an installer
// binds them first); registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

class ContentsFormats final : public Provider {
public:
    using Provider::Provider;
    void install() {
        string_symbol(quartz_core, "_kCAContentsFormatRGBA8Uint", "RGBA8");
    }
};

} // namespace

void install_quartzcore_contents_formats(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<ContentsFormats>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
