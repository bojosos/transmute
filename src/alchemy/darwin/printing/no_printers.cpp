#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"

// PrintCore (through ApplicationServices): no printers. The server's printer
// list is empty, no session has a current printer, and the page-format,
// print-settings and session calls fail explicitly.
// Moved from darwin/identities/installer_frameworks.cpp (an installer
// binds them first); registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

class NoPrinters final : public Provider {
public:
    using Provider::Provider;
    void install() {
        // Printing: no printers. The server's printer list is empty.
        function(application_services, "_PMServerCreatePrinterList", [this](CpuState& s, GuestMemory& m) {
            if (!s.x[1]) { s.x[0] = static_cast<std::uint32_t>(-50); return; }
            m.write64(s.x[1], cf.array({}));
            s.x[0] = 0;
        });
        function(application_services, "_PMRelease", [this](CpuState& s, GuestMemory&) {
            if (s.x[0]) context.objects().release(s.x[0]);
            s.x[0] = 0;
        });
        function(application_services, "_PMSessionGetCurrentPrinter", [](CpuState& s, GuestMemory&) {
            s.x[0] = static_cast<std::uint32_t>(-30872); // kPMNoDefaultPrinter
        });
        const auto many = [this](std::string_view library, std::initializer_list<std::string_view> names, std::string_view why) {
            for (const auto name : names) unsupported(library, name, why);
        };
        many(application_services, {"_PMCopyPageFormat", "_PMCreatePageFormatWithPMPaper", "_PMGetAdjustedPageRect",
            "_PMGetAdjustedPaperRect", "_PMGetCollate", "_PMGetCopies", "_PMGetDuplex", "_PMGetFirstPage", "_PMGetLastPage",
            "_PMGetOrientation", "_PMGetPageFormatPaper", "_PMGetPageRange", "_PMGetUnadjustedPageRect",
            "_PMGetUnadjustedPaperRect", "_PMPaperCreateCustom", "_PMPaperGetHeight", "_PMPaperGetWidth",
            "_PMPrintSettingsSetJobName", "_PMPrinterGetID", "_PMPrinterGetIndexedPrinterResolution",
            "_PMPrinterGetOutputResolution", "_PMPrinterGetPaperList", "_PMPrinterGetPrinterResolutionCount",
            "_PMPrinterIsDefault", "_PMPrinterSetOutputResolution", "_PMSessionBeginCGDocumentNoDialog",
            "_PMSessionBeginPageNoDialog", "_PMSessionEndDocumentNoDialog", "_PMSessionEndPageNoDialog",
            "_PMSessionGetCGGraphicsContext", "_PMSessionSetCurrentPMPrinter", "_PMSessionValidatePageFormat",
            "_PMSessionValidatePrintSettings", "_PMSetCollate", "_PMSetCopies", "_PMSetDuplex", "_PMSetFirstPage",
            "_PMSetLastPage", "_PMSetOrientation", "_PMSetPageRange"}, "printing is not implemented (no printers)");
    }
};

} // namespace

void install_printing_no_printers(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<NoPrinters>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
