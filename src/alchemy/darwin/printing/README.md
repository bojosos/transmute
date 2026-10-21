# Printing

**Covers:** libcups with no scheduler (real option, destination and IPP
message data) and PrintCore (PM*) (`install.cpp`, fill-only), and Chrome's
PrintCore gaps (`print_core_gaps.cpp`).

**Host dependencies:** none: no printers exist.

**Unsupported:** printing itself; PrintCore sessions fail with PrintCore's
error codes.

## Moved from darwin/identities/

* `no_printers.cpp`: headless, every build: PrintCore through ApplicationServices with no printers; page-format, settings and session calls fail explicitly.
