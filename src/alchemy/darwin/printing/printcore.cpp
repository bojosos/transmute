#include "alchemy/darwin/printing/printing.hpp"

// PrintCore (ApplicationServices PM* functions). The macOS printing system is
// not implemented under Alchemy: sessions, print settings and page formats
// cannot be created (kPMNotImplemented) and no printer exists, so every call
// on a printing object answers kPMInvalidParameter (paramErr) with its
// outputs cleared.
namespace alchemy::gui::fill {
namespace {

constexpr std::int32_t pm_not_implemented = -30873, pm_invalid_parameter = -50;

class PrintCore final : public Provider {
public:
    using Provider::Provider;
    void install() {
        for (const auto* name : {"_PMCreateSession", "_PMCreatePrintSettings", "_PMCreatePageFormat"})
            failure(application_services, name, pm_not_implemented, {0});
        constant(application_services, "_PMPrinterCreateFromPrinterID", 0);
        for (const auto* name : {"_PMPrinterGetName", "_PMPrinterGetLocation"}) constant(application_services, name, 0);
        failure(application_services, "_PMRetain", pm_invalid_parameter);
        failure(application_services, "_PMPrinterGetMakeAndModelName", pm_invalid_parameter, {1});
        failure(application_services, "_PMPrinterGetMimeTypes", pm_invalid_parameter, {2});
        failure(application_services, "_PMPrinterGetState", pm_invalid_parameter);
        failure(application_services, "_PMPrinterIsRemote", pm_invalid_parameter);
        failure(application_services, "_PMPrinterCopyDescriptionURL", pm_invalid_parameter, {2});
        failure(application_services, "_PMPaperCreateLocalizedName", pm_invalid_parameter, {2});
        failure(application_services, "_PMPaperGetMargins", pm_invalid_parameter);
        failure(application_services, "_PMPaperGetPPDPaperName", pm_invalid_parameter, {1});
        failure(application_services, "_PMPrintSettingsGetJobName", pm_invalid_parameter, {1});
        failure(application_services, "_PMPrintSettingsSetValue", pm_invalid_parameter);
        failure(application_services, "_PMSessionCopyDestinationLocation", pm_invalid_parameter, {2});
        for (const auto* name : {"_PMSessionDefaultPageFormat", "_PMSessionDefaultPrintSettings", "_PMSessionError",
                                 "_PMSessionGetDestinationType", "_PMSessionSetDestination"})
            failure(application_services, name, pm_invalid_parameter);
    }
};

} // namespace

void install_print_core(InstallContext& context) { std::make_shared<PrintCore>(context)->install(); }

} // namespace alchemy::gui::fill
