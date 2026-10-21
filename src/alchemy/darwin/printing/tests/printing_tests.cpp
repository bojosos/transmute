// printing provider tests: the libcups and PrintCore imports resolve; every
// scheduler call fails with IPP_STATUS_ERROR_SERVICE_UNAVAILABLE; option,
// destination and IPP message data functions are real; PPD files are not
// parsed; PrintCore objects cannot be created.
#include "alchemy/gui/tests/fill_fixture.hpp"

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
    const auto session = f.block(8, 0xff);
    require(f.status(as, "_PMCreateSession", {session}) == -30873 && f.engine.read64(session) == 0, "PMCreateSession");
    require(f.call(as, "_PMPrinterCreateFromPrinterID", {f.cf.string("Office")}) == 0, "no printers");
    require(f.status(as, "_PMSessionError", {0}) == -50, "no sessions");
}

} // namespace

int main() {
    return run("printing", {{"imports", imports}, {"scheduler", scheduler}, {"options and destinations", options_and_destinations},
                            {"ipp", ipp}, {"print core", print_core}});
}
