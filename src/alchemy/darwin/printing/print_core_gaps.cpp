#include "alchemy/gui/value_provider.hpp"

// PrintCore (ApplicationServices) gaps of Chrome's images, cut from
// darwin/coreservices/services.cpp (install_services calls it in place):
// like the printing provider, no print session, settings or page format
// exists (kPMInvalidParameter with outputs cleared; creating from data is
// kPMNotImplemented).
namespace alchemy::gui::values {
namespace {

using fill::application_services;

constexpr std::int32_t pm_not_implemented = -30873, pm_invalid_parameter = -50;

} // namespace

void Provider::install_print_core() {
    failure(application_services, "_PMCopyPrintSettings", pm_invalid_parameter);
    failure(application_services, "_PMPageFormatCreateDataRepresentation", pm_invalid_parameter, {1});
    failure(application_services, "_PMPrintSettingsCreateDataRepresentation", pm_invalid_parameter, {1});
    failure(application_services, "_PMSessionCopyDestinationFormat", pm_invalid_parameter, {2});
    failure(application_services, "_PMPageFormatCreateWithDataRepresentation", pm_not_implemented, {1});
    failure(application_services, "_PMPrintSettingsCreateWithDataRepresentation", pm_not_implemented, {1});
}

} // namespace alchemy::gui::values
