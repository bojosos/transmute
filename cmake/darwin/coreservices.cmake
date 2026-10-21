# CoreServices (src/alchemy/darwin/coreservices/; group r2 foundation), with
# ApplicationServices pieces and UniformTypeIdentifiers. Included by
# cmake/AlchemyDarwin.cmake.
#
# Unicode collation (UnicodeUtilities) over the host's linguistic comparison,
# UTType functions and the UTType class over one type table (uttype.cpp),
# LaunchServices answers (no application registry), Apple Event descriptors
# without an Apple Event server, the accessibility client without an
# accessibility server, and the other system-service gaps in services.cpp.
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/coreservices/coreservices.cpp
    src/alchemy/darwin/coreservices/services.cpp
    src/alchemy/darwin/coreservices/fsevents.cpp
    src/alchemy/darwin/coreservices/uttype.cpp
    src/alchemy/darwin/coreservices/uttype_provider.cpp
    src/alchemy/darwin/coreservices/install.cpp)

function(alchemy_darwin_coreservices_tests)
    # Host-only: the UTType table.
    alchemy_gui_add_unit_test(alchemy_gui_coreservices_uttype src/alchemy/darwin/coreservices/tests/uttype_tests.cpp)
    # Collation, UTType functions, Launch Services over the export-fill fixture.
    alchemy_gui_add_unit_test(alchemy_gui_coreservices src/alchemy/darwin/coreservices/tests/coreservices_tests.cpp TIMEOUT 120)
    # Chrome's system-service gaps in r2's frameworks over the full provider set.
    alchemy_gui_add_unit_test(alchemy_gui_coreservices_system_services src/alchemy/darwin/coreservices/tests/system_services_tests.cpp TIMEOUT 180)
endfunction()
alchemy_gui_register_tests(alchemy_darwin_coreservices_tests)

# services.cpp's pieces in other groups' directories (install_services calls
# them in place). The libSystem and identities pieces (misc/sandbox.cpp,
# open_directory.cpp, service_management.cpp) are listed in r1's fragments,
# the printing and IOKit pieces in r3's.

# Moved from darwin/identities/: carbon_core.cpp (fp aliases, FSGetVolumeInfo;
# its own installer-table row), launch_services.cpp (LaunchServices, Spotlight,
# Time Machine, Component Manager, UTI queries; fill-only, kit/fwgaps) and
# icon_refs.cpp (ReleaseIconRef; the support kit).
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/coreservices/carbon_core.cpp
    src/alchemy/darwin/coreservices/launch_services.cpp
    src/alchemy/darwin/coreservices/icon_refs.cpp)
alchemy_gui_add_framework_case(coreservices_launch_services src/alchemy/darwin/coreservices/tests/coreservices_cases_tests.cpp)
alchemy_gui_add_framework_case(coreservices_carbon_core src/alchemy/darwin/coreservices/tests/coreservices_cases_tests.cpp)

# Headless additions (both builds; darwin/headless_additions.hpp), moved from
# darwin/identities/installer_frameworks.cpp:
target_sources(alchemy_core PRIVATE
    src/alchemy/darwin/coreservices/types_and_gestalt.cpp)

# Headless framework set (both builds; darwin/headless_additions.hpp), moved
# from darwin/identities/frameworks.cpp:
target_sources(alchemy_core PRIVATE
    src/alchemy/darwin/coreservices/shared_file_lists.cpp
    src/alchemy/darwin/coreservices/carbon_core_math.cpp)
