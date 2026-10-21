# CoreServices

CoreServices, with UniformTypeIdentifiers and some ApplicationServices pieces.

- **Covers:** Unicode collation (UnicodeUtilities) over the host's linguistic
  comparison; UTType functions and the UTType class over one type table
  (uttype.cpp); LaunchServices answers; Apple Event descriptors; the
  accessibility client; the other system-service gaps in services.cpp.
- **Host dependencies:** host::locale collation (compare, sort_key).
- **Unsupported, failing explicitly:** there is no Launch Services registry
  of applications, no Apple Event server and no accessibility server.
  Messages to other processes fail, and registration answers
  kLSServerCommunicationErr.
- **Registration:** still called from the export-fill, qlab data and values
  aggregators.
- **Extension points:** none. A provider layering behaviour over another
  framework's export uses that framework's extension point
  (../corefoundation/README.md), never a second registration.
- **Tests:** `tests/` (alchemy_gui_coreservices_uttype); the value-provider and export-fill
  cases are still in the shared aggregator tests.

## Moved from darwin/identities/

* `carbon_core.cpp`: the fp.h aliases and FSGetVolumeInfo/FSGetVolumeParms over the host's fixed volumes (its own row; ctest `alchemy_gui_coreservices_carbon_core`).
* `launch_services.cpp`: LaunchServices registration and login items cannot change (kLSServerCommunicationErr), display names and UTI tag queries are real, Spotlight and the Component Manager have nothing (fill-only, kit/fwgaps; ctest `alchemy_gui_coreservices_launch_services`).
* `icon_refs.cpp`: ReleaseIconRef (the support kit).
* `types_and_gestalt.cpp`, `shared_file_lists.cpp`, `carbon_core_math.cpp`: headless, every build: the UTType table, Gestalt (14.0.0), Launch Services with no applications, empty LSSharedFileLists, and the CarbonCore fp.h math.
