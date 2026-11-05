// printing provider tests: the libcups and PrintCore imports resolve; every
// scheduler call fails with IPP_STATUS_ERROR_SERVICE_UNAVAILABLE; option,
// destination and IPP message data functions are real; PPD files are not
// parsed; PrintCore's settings, page formats, papers and sessions are real,
// with no printers (a file destination prints a PDF).
#include "alchemy/gui/tests/fill_fixture.hpp"

#include <array>
#include <windows.h>
#include <fstream>
#include <filesystem>
#include <bit>

namespace {
using namespace fill_test;

constexpr std::string_view cups = "/usr/lib/libcups.2.dylib";
constexpr std::string_view as = "/System/Library/Frameworks/ApplicationServices.framework/Versions/A/ApplicationServices";

void imports(Fixture& f) { imports_resolve(f, {cups}); }

void scheduler(Fixture& f) {
    const auto dests = f.block(8, 0xff);
    require(f.call(cups, "_cupsGetDests2", {0, dests}) == 0 && f.engine.read64(dests) == 0, "no destinations");
    require(f.call(cups, "_cupsLastError") == 0x0503, "service unavailable");
    require(f.read_text(f.call(cups, "_cupsLastErrorString")) == "server-error-service-unavailable", "error string");
    require(f.call(cups, "_cupsGetNamedDest", {0, f.text("printer"), 0}) == 0, "no named destination");
    const auto job = f.block(4, 0xff);
    require(f.call(cups, "_cupsCreateDestJob", {0, 0x1000, 0, job, f.text("title"), 0, 0}) == 0x0503 && f.engine.read32(job) == 0, "no jobs");
    require(f.call(cups, "_httpConnect2", {f.text("localhost"), 631, 0, 0, 0, 0, 0, 0}) == 0, "no HTTP connection");
    require(f.read_text(f.call(cups, "_cupsServer")) == "/private/var/run/cupsd", "cupsServer");
    require(f.call(cups, "_ppdOpenFile", {f.text("/tmp/printer.ppd")}) == 0 && f.call(cups, "_ppdLastError", {0}) == 6, "PPD not parsed");
}

void options_and_destinations(Fixture& f) {
    // Two options.
    const auto options = f.block(32);
    f.engine.write64(options, f.text("media")); f.engine.write64(options + 8, f.text("A4"));
    f.engine.write64(options + 16, f.text("sides")); f.engine.write64(options + 24, f.text("two-sided-long-edge"));
    require(f.read_text(f.call(cups, "_cupsGetOption", {f.text("sides"), 2, options})) == "two-sided-long-edge", "cupsGetOption");
    require(f.call(cups, "_cupsGetOption", {f.text("color"), 2, options}) == 0, "missing option");
    // A destination copied into an empty array, found, then removed.
    const auto dest = f.block(32);
    f.engine.write64(dest, f.text("Office")); f.engine.write32(dest + 16, 1);
    f.engine.write32(dest + 20, 2); f.engine.write64(dest + 24, options);
    const auto array = f.block(8);
    require(f.call(cups, "_cupsCopyDest", {dest, 0, array}) == 1, "cupsCopyDest");
    const auto copied = f.engine.read64(array);
    require(f.read_text(f.engine.read64(copied)) == "Office" && f.engine.read64(copied) != f.engine.read64(dest), "deep copy");
    require(f.call(cups, "_cupsGetDest", {0, 0, 1, copied}) == copied, "default destination");
    require(f.call(cups, "_cupsGetDest", {f.text("Office"), 0, 1, copied}) == copied, "named destination");
    require(f.call(cups, "_cupsRemoveDest", {f.text("Office"), 0, 1, array}) == 0, "cupsRemoveDest");
    static_cast<void>(f.call(cups, "_cupsFreeDests", {0, f.engine.read64(array)}));
}

void ipp(Fixture& f) {
    const auto request = f.call(cups, "_ippNewRequest", {0x000b});   // IPP_OP_GET_PRINTER_ATTRIBUTES
    require(request != 0, "ippNewRequest");
    const auto first = f.call(cups, "_ippFirstAttribute", {request});
    require(f.read_text(f.call(cups, "_ippGetName", {first})) == "attributes-charset", "charset attribute");
    require(f.read_text(f.call(cups, "_ippGetString", {first, 0, 0})) == "utf-8", "charset value");
    const auto uri = f.call(cups, "_ippAddString", {request, 1, 0x45, f.text("printer-uri"), 0, f.text("ipp://localhost/printers/x")});
    require(f.call(cups, "_ippGetValueTag", {uri}) == 0x45 && f.call(cups, "_ippGetGroupTag", {uri}) == 1, "tags");
    const auto values = f.block(16);
    f.engine.write64(values, f.text("printer-name")); f.engine.write64(values + 8, f.text("printer-state"));
    const auto requested = f.call(cups, "_ippAddStrings", {request, 1, 0x44, f.text("requested-attributes"), 2, 0, values});
    require(f.call(cups, "_ippGetCount", {requested}) == 2 && f.read_text(f.call(cups, "_ippGetString", {requested, 1, 0})) == "printer-state", "strings");
    const auto copies = f.call(cups, "_ippAddInteger", {request, 2, 0x21, f.text("copies"), 3});
    require(f.call(cups, "_ippGetInteger", {copies, 0}) == 3, "integer");
    std::size_t count = 0;
    for (auto at = f.call(cups, "_ippFirstAttribute", {request}); at; at = f.call(cups, "_ippNextAttribute", {request})) ++count;
    require(count == 5, "five attributes");
    require(f.call(cups, "_ippValidateAttributes", {request}) == 1, "valid request");
    require(f.call(cups, "_ippPort") == 631, "ippPort");
    // cupsDoRequest frees the request and fails.
    require(f.call(cups, "_cupsDoRequest", {0, request, f.text("/")}) == 0, "cupsDoRequest");
    bool refused = false;
    try { static_cast<void>(f.call(cups, "_ippDelete", {request})); } catch (const std::exception&) { refused = true; }
    require(refused, "the request was already freed");
}

void print_core(Fixture& f) {
    const auto object = [&f](const char* function, std::initializer_list<Address> before = {}) {
        const auto out = f.block(8, 0xff);
        std::vector<Address> arguments(before);
        arguments.push_back(out);
        alchemy::CallRegs registers;
        for (std::size_t i = 0; i < arguments.size(); ++i) registers.x[i] = arguments[i];
        require(static_cast<std::int32_t>(f.regs(as, function, registers).x0) == 0, function);
        return f.engine.read64(out);
    };
    const auto u32 = [&f](const char* function, Address target) {
        const auto out = f.block(4);
        require(f.status(as, function, {target, out}) == 0, function);
        return f.engine.read32(out);
    };
    const auto rect = [&f](const char* function, Address format) {
        const auto out = f.block(32);
        require(f.status(as, function, {format, out}) == 0, function);
        std::array<double, 4> r{};
        for (unsigned i = 0; i < 4; ++i) r[i] = std::bit_cast<double>(f.engine.read64(out + i * 8));
        return r;   // top, left, bottom, right
    };
    // Print settings: values, a page range, a job name, any key, and their data representation.
    const auto settings = object("_PMCreatePrintSettings");
    require(settings != 0 && u32("_PMGetCopies", settings) == 1, "new print settings");
    require(f.status(as, "_PMSetCopies", {settings, 3, 0}) == 0 && u32("_PMGetCopies", settings) == 3, "copies");
    require(f.status(as, "_PMSetFirstPage", {settings, 1, 0}) == 0 && f.status(as, "_PMSetPageRange", {settings, 2, 5}) == 0 &&
                u32("_PMGetFirstPage", settings) == 2 && u32("_PMGetLastPage", settings) == 5, "a page range brings the first and last pages in");
    require(f.status(as, "_PMSetCopies", {settings, 0, 0}) == -50, "zero copies are refused");
    require(f.status(as, "_PMPrintSettingsSetJobName", {settings, f.cf.string("Cue list")}) == 0, "set job name");
    require(f.cf.utf8(object("_PMPrintSettingsGetJobName", {settings})) == "Cue list", "job name");
    const auto key = f.cf.string("com.example.colour");
    require(f.status(as, "_PMPrintSettingsSetValue", {settings, key, f.cf.string("grey"), 0}) == 0 &&
                f.cf.utf8(object("_PMPrintSettingsGetValue", {settings, key})) == "grey", "any key's value");
    const auto data_out = f.block(8);
    require(f.status(as, "_PMPrintSettingsCreateDataRepresentation", {settings, data_out, 0}) == 0, "settings data representation");
    const auto data = f.engine.read64(data_out);
    require(data != 0, "settings data");
    const auto revived = f.engine.read64([&] {
        const auto out = f.block(8);
        require(f.status(as, "_PMPrintSettingsCreateWithDataRepresentation", {data, out}) == 0, "settings from data");
        return out;
    }());
    require(u32("_PMGetCopies", revived) == 3 && u32("_PMGetLastPage", revived) == 5 &&
                f.cf.utf8(object("_PMPrintSettingsGetJobName", {revived})) == "Cue list" &&
                f.cf.utf8(object("_PMPrintSettingsGetValue", {revived, key})) == "grey",
            "the data representation round-trips");
    const auto copy = object("_PMCreatePrintSettings");
    require(f.status(as, "_PMCopyPrintSettings", {settings, copy}) == 0 && u32("_PMGetCopies", copy) == 3, "PMCopyPrintSettings");

    // Page formats: US Letter with 18-point margins by default; landscape swaps the rectangles.
    const auto format = object("_PMCreatePageFormat");
    const auto page = rect("_PMGetAdjustedPageRect", format);
    const auto paper_rect = rect("_PMGetAdjustedPaperRect", format);
    require(page[0] == 0 && page[1] == 0 && page[2] == 756 && page[3] == 576, "the default page rect");
    require(paper_rect[0] == -18 && paper_rect[1] == -18 && paper_rect[2] == 774 && paper_rect[3] == 594, "the default paper rect");
    require(f.status(as, "_PMSetOrientation", {format, 2, 0}) == 0 && u32("_PMGetOrientation", format) == 2 &&
                rect("_PMGetAdjustedPageRect", format)[2] == 576, "landscape");
    const auto paper = object("_PMGetPageFormatPaper", {format});
    const auto width = f.block(8);
    require(f.status(as, "_PMPaperGetWidth", {paper, width}) == 0 && std::bit_cast<double>(f.engine.read64(width)) == 612 &&
                f.cf.utf8(object("_PMPaperGetPPDPaperName", {paper})) == "na-letter", "the default paper");
    // A custom paper (A4, 10-point margins) and a format made with it, through its data representation.
    const auto margins = f.block(32);
    for (unsigned i = 0; i < 4; ++i) f.engine.write64(margins + i * 8, std::bit_cast<std::uint64_t>(10.0));
    alchemy::CallRegs custom;
    const auto out = f.block(8);
    custom.x[0] = 0; custom.x[1] = f.cf.string("iso-a4"); custom.x[2] = f.cf.string("A4"); custom.x[3] = margins; custom.x[4] = out;
    custom.set_d(0, 595); custom.set_d(1, 842);
    require(static_cast<std::int32_t>(f.regs(as, "_PMPaperCreateCustom", custom).x0) == 0, "PMPaperCreateCustom");
    const auto a4_format_out = f.block(8);
    require(f.status(as, "_PMCreatePageFormatWithPMPaper", {a4_format_out, f.engine.read64(out)}) == 0, "a format with the paper");
    const auto a4_format = f.engine.read64(a4_format_out);
    const auto format_data_out = f.block(8);
    require(f.status(as, "_PMPageFormatCreateDataRepresentation", {a4_format, format_data_out, 0}) == 0, "format data representation");
    const auto format_data = f.engine.read64(format_data_out);
    const auto a4_again_out = f.block(8);
    require(f.status(as, "_PMPageFormatCreateWithDataRepresentation", {format_data, a4_again_out}) == 0, "format from data");
    require(rect("_PMGetAdjustedPageRect", f.engine.read64(a4_again_out))[3] == 575, "the A4 format round-trips (595 - 2 x 10)");

    // Sessions: no printer, so printing to the default destination fails; a file destination writes a PDF.
    const auto session = object("_PMCreateSession");
    const auto printer = f.block(8, 0xff);
    require(f.status(as, "_PMSessionGetCurrentPrinter", {session, printer}) == -30872 && f.engine.read64(printer) == 0, "no current printer");
    require(f.call(as, "_PMPrinterCreateFromPrinterID", {f.cf.string("Office")}) == 0, "no printers");
    require(f.cf.utf8(object("_PMSessionCopyDestinationFormat", {session, settings})) == "com.apple.documentformat.default", "default format");
    require(f.status(as, "_PMSessionBeginCGDocumentNoDialog", {session, settings, format}) == -30872 &&
                f.status(as, "_PMSessionError", {session}) == -30872, "printing to a printer fails: none exists");
    const auto host = std::filesystem::temp_directory_path() / ("alchemy-pm-" + std::to_string(GetCurrentProcessId()) + ".pdf");
    std::filesystem::remove(host);
    const auto generic = host.generic_string();
    const auto url = f.cf.file_url("/Volumes/" + generic.substr(0, 1) + generic.substr(2), false);
    require(f.status(as, "_PMSessionSetDestination", {session, settings, 2, f.cf.string("application/pdf"), url}) == 0, "set destination");
    const auto type = f.block(4);
    require(f.status(as, "_PMSessionGetDestinationType", {session, settings, type}) == 0 && f.engine.read32(type) == 2, "a file destination");
    const bool drawing = f.process.resolve("/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics", "_CGPDFContextCreateWithURL").has_value();
    const auto begun = f.status(as, "_PMSessionBeginCGDocumentNoDialog", {session, settings, format});
    if (!drawing) {
        require(begun == -30873, "without CoreGraphics drawing is not implemented");
        return;
    }
    require(begun == 0, "a document to a file: " + std::to_string(begun));
    for (int i = 0; i < 2; ++i) {
        require(f.status(as, "_PMSessionBeginPageNoDialog", {session, format, 0}) == 0, "begin page");
        require(object("_PMSessionGetCGGraphicsContext", {session}) != 0, "the page's context");
        require(f.status(as, "_PMSessionEndPageNoDialog", {session}) == 0, "end page");
    }
    require(f.status(as, "_PMSessionEndDocumentNoDialog", {session}) == 0, "end document");
    std::ifstream file(host, std::ios::binary);
    std::string head(4, ' ');
    file.read(head.data(), 4);
    file.close();
    constexpr std::string_view cg = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";
    const auto document = f.call(cg, "_CGPDFDocumentCreateWithURL", {url});
    const auto pages = document ? f.call(cg, "_CGPDFDocumentGetNumberOfPages", {document}) : 0;
    std::filesystem::remove(host);
    require(head == "%PDF" && pages == 2, "a two-page PDF at the destination: " + std::to_string(pages));
    require(f.call(as, "_PMRelease", {session}) == 0 && f.call(as, "_PMRelease", {settings}) == 0, "PMRelease");

    // NSPrintInfo's PrintCore objects, set from it and read back into it.
    if (!f.objc.find_class("NSPrintInfo")) return;
    const auto info = f.send(f.send(f.cls("NSPrintInfo"), "alloc"), "init");
    static_cast<void>(f.send(info, "setOrientation:", {1}));   // landscape
    const auto info_format = f.send(info, "PMPageFormat");
    const auto info_settings = f.send(info, "PMPrintSettings");
    require(info_format && info_settings && f.send(info, "PMPrintSession"), "the print info's PrintCore objects");
    require(u32("_PMGetOrientation", info_format) == 2 && u32("_PMGetCopies", info_settings) == 1, "landscape, one copy");
    require(f.status(as, "_PMSetCopies", {info_settings, 4, 0}) == 0 && f.status(as, "_PMSetOrientation", {info_format, 1, 0}) == 0, "PM edits");
    static_cast<void>(f.send(info, "updateFromPMPrintSettings"));
    static_cast<void>(f.send(info, "updateFromPMPageFormat"));
    const auto copies = f.send(f.send(info, "dictionary"), "objectForKey:", {f.cf.string("NSCopies")});
    require(copies && f.send(copies, "intValue") == 4 && f.send(info, "orientation") == 0, "updateFromPMPrintSettings / updateFromPMPageFormat");
}

} // namespace

int main() {
    return run("printing", {{"imports", imports}, {"scheduler", scheduler}, {"options and destinations", options_and_destinations},
                            {"ipp", ipp}, {"print core", print_core}});
}
