# Printing

**Covers:** libcups with no scheduler (real option, destination and IPP
message data; `cups.cpp`, installed fill-only by `install.cpp`) and
PrintCore, the PM* functions of ApplicationServices (`print_core.cpp`, both
builds, the only owner of the PM names; `print_core_gaps.cpp` is the values
provider's empty entry).

**libcups for the apps wave (GTK's print backend, darktable, Inkscape):**
* Real:
  * cupsAddOption and cupsParseOptions (quoted and braced values; a bare
    name is "true");
  * the client settings (encryption, password callback);
  * IPP operation, request id and state;
  * booleans, resolutions, ippErrorString;
  * pwgMediaForPWG (the standard sizes);
  * httpAddrLength, and httpGetHostname without a connection.
* The destination and job calls fail like the other scheduler calls.
* No http_t, ppd_file_t or cups_array_t can exist: their calls answer their
  failure for NULL and stop for anything else.
* Not provided (explicit stops): IPP wire encoding (ippLength / ippRead /
  ippWrite), httpSeparateURI / httpAssembleURIf, cupsLangDefault, and
  libcupsimage's raster streams (an identity whose functions stop).

**PrintCore (real):** print settings (copies, collation, first / last page,
page range, duplex, job name, destination type / format / location, any
key's value), page formats (paper, orientation, scale, adjusted and
unadjusted page / paper rectangles), papers (custom, with margins; the
default is US Letter with 18-point margins, Alchemy's choice) and sessions
(defaults, validation, the last error). Data representations are property
lists of these values: they round-trip between Alchemy processes, not with
macOS. A document whose destination is a file is drawn into a PDF there
through CoreGraphics' CGPDFContext (pages are the paper's media box).
NSPrintInfo's -PMPrintSettings / -PMPageFormat / -PMPrintSession and
-updateFromPMPrintSettings / -updateFromPMPageFormat bridge to these
(`darwin/appkit/panels.cpp`).

**Unavailable, as documented:** no printers exist: the printer list is
empty, PMPrinterCreateFromPrinterID gives NULL, sessions have no current
printer (kPMNoDefaultPrinter), and a document for a printer, a preview or a
fax fails with kPMNoDefaultPrinter: no job is reported that did not happen.
Without CoreGraphics (the headless build) drawing fails with
kPMNotImplemented. -[NSPrintInfo printSettings] fails explicitly.

**Host dependencies:** none.

**Tests:** `tests/printing_tests.cpp` (ctest `alchemy_gui_printing`).
