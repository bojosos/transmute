#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"
#include "alchemy/darwin_frameworks.hpp"

// QuartzCore: CALayer and CAMetalLayer, the classes the headless Metal set
// needs.
// Moved from darwin/identities/frameworks.cpp (the headless framework set);
// registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

class BaseLayers final : public Provider {
public:
    using Provider::Provider;
    void install() {
        static_cast<void>(declare_class(quartz_core, "CALayer", "NSObject"));
        static_cast<void>(declare_class(quartz_core, "CAMetalLayer", "CALayer"));
    }
};

} // namespace

void install_quartzcore_base_layers(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<BaseLayers>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
