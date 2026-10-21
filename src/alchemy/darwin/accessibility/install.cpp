#include "alchemy/darwin/accessibility/install.hpp"
#include "alchemy/gui/value_provider.hpp"

// Accessibility: AXCustomContent is a plain value class (label, value,
// importance).
// Chrome 155 first linked the framework (moved from
// darwin/identities/identities.cpp).
namespace alchemy::accessibility {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view accessibility = "/System/Library/Frameworks/Accessibility.framework/Versions/A/Accessibility";

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    void install();
};

void Provider::install() {
    ProcessContext::add_provider_library(std::string(accessibility));
    const auto custom_content = cls(accessibility, "AXCustomContent", "NSObject");
    object_properties(custom_content, {"label", "value"});
    integer_properties(custom_content, {"importance"});
    method(custom_content, "customContentWithLabel:value:", [this, custom_content](CpuState& s, GuestMemory&) {
        const auto object = create(custom_content);
        store(object, "label", s.x[2]);
        store(object, "value", s.x[3]);
        s.x[0] = bridge->autorelease(object);
    }, true);
}

} // namespace

void install_accessibility(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::accessibility
