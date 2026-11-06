# CoreServices

CoreServices, with UniformTypeIdentifiers and some ApplicationServices pieces.

- **Covers:** Unicode collation (UnicodeUtilities) over the host's linguistic
  comparison; UTType functions and the UTType class over one type table
  (uttype.cpp); LaunchServices answers, with an application registry over the
  guest root's Applications folders (launch_services.cpp); Apple Event descriptors, and
  events a process sends itself (its own handlers); the
  accessibility client; the other system-service gaps in services.cpp.
- **Host dependencies:** host::locale collation (compare, sort_key).
- **Unsupported, failing explicitly:** there is no Apple Event server (an
  event for another process is connectionInvalid) and no
  accessibility server. LaunchServices keeps its own store (the
  com.alchemy.LaunchServices preferences domain: LSRegisterURL's bundles and
  the default handlers set), not Apple's database. LSOpenFromURLSpec opens
  items with the host's handlers but launches no guest application. Messages to other processes fail. A headless process has no
  application registry (launch_services_headless.cpp).
- **Registration:** still called from the export-fill, qlab data and values
  aggregators.
- **Extension points:** none. A provider layering behaviour over another
  framework's export uses that framework's extension point
  (../corefoundation/README.md), never a second registration.
- **Tests:** `tests/` (alchemy_gui_coreservices_uttype); the value-provider and export-fill
  cases are still in the shared aggregator tests.

## Moved from darwin/identities/

* `carbon_core.cpp`: the fp.h aliases and FSGetVolumeInfo/FSGetVolumeParms over the host's fixed volumes (its own row; ctest `alchemy_gui_coreservices_carbon_core`).
* `launch_services.cpp`: the application registry, LSRegisterURL and the default handlers (the store above), LSOpenFromURLSpec (items open with the host's handlers, as NSWorkspace opens them; launching a guest application answers kLSServerCommunicationErr), login items cannot change, display names and UTI tag queries are real, Spotlight and the Component Manager have nothing (fill-only, kit/fwgaps; ctest `alchemy_gui_coreservices_launch_services`).
* `icon_refs.cpp`: ReleaseIconRef (the support kit).
* `types_and_gestalt.cpp`, `shared_file_lists.cpp`, `carbon_core_math.cpp`: headless, every build: the UTType table, Gestalt (14.0.0), Launch Services answers that need no applications, empty LSSharedFileLists, and the CarbonCore fp.h math.
