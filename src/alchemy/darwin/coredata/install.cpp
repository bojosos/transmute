#include "alchemy/darwin/coredata/install.hpp"
#include "alchemy/gui/value_provider.hpp"

// CoreData: an identity with no exports (a framework that links it without
// using it loads). Electron's Mantle framework (VS Code) first linked it.
namespace alchemy::coredata {
namespace {

constexpr std::string_view core_data = "/System/Library/Frameworks/CoreData.framework/Versions/A/CoreData";

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    void install();
};

void Provider::install() {
    ProcessContext::add_provider_library(std::string(core_data));
}

} // namespace

void install_coredata(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::coredata
