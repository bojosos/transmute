#include "alchemy/darwin/coremediaio/install.hpp"
#include "alchemy/gui/value_provider.hpp"

// CoreMediaIO: no DAL plug-in objects exist (kCMIOHardwareBadObjectError).
// Chrome 155 first linked the framework (moved from
// darwin/identities/identities.cpp).
namespace alchemy::coremediaio {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view core_media_io = "/System/Library/Frameworks/CoreMediaIO.framework/Versions/A/CoreMediaIO";
constexpr std::int32_t cmio_hardware_bad_object = static_cast<std::int32_t>(fill::fourcc("!obj"));

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    void install();
};

void Provider::install() {
    ProcessContext::add_provider_library(std::string(core_media_io));
    failure(core_media_io, "_CMIOObjectGetPropertyData", cmio_hardware_bad_object);
    failure(core_media_io, "_CMIOObjectGetPropertyDataSize", cmio_hardware_bad_object);
    failure(core_media_io, "_CMIOObjectSetPropertyData", cmio_hardware_bad_object);
    constant(core_media_io, "_CMIOObjectHasProperty", 0);
}

} // namespace

void install_coremediaio(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::coremediaio
