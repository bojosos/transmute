# CoreFoundation (src/alchemy/darwin/corefoundation/; group r2 foundation).
# Included by cmake/AlchemyDarwin.cmake.
#
# Headless (alchemy_core, so alchemy.exe and alchemy-fx): the object store and
# the CoreFoundation provider. GUI set (alchemy-fx only): the CF additions
# that sit on Foundation's classes (CFSet and CFNotificationCenter bridges,
# CFBundle over NSBundle, CFLocale, property lists, preferences,
# CFFileDescriptor, CFString clusters, CFCharacterSet). Headless tests stay in
# tests/alchemy_core_foundation_tests.cpp (cmake/Alchemy.cmake); the GUI-set
# tests are in tests/.
target_sources(alchemy_core PRIVATE
    src/alchemy/darwin/corefoundation/object_store.cpp
    src/alchemy/darwin/corefoundation/core_foundation.cpp
    src/alchemy/darwin/corefoundation/cf_encodings.cpp)
# Headless shells (darwin/foundation/headless_shells.hpp): alchemy.exe only; the
# _fx twins in the GUI set below are empty, because there cf_preferences.cpp
# and cf_locale.cpp own the names.
set(alchemy_corefoundation_headless_shells
    src/alchemy/darwin/corefoundation/user_defaults_headless.cpp
    src/alchemy/darwin/corefoundation/cf_date_formatter_headless.cpp)
target_sources(alchemy_core PRIVATE ${alchemy_corefoundation_headless_shells})
list(APPEND ALCHEMY_HEADLESS_ONLY_SOURCES ${alchemy_corefoundation_headless_shells})
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/corefoundation/cf_sets_notifications.cpp
    src/alchemy/darwin/corefoundation/charset.cpp
    src/alchemy/darwin/corefoundation/cf_collections.cpp
    src/alchemy/darwin/corefoundation/cf_urls.cpp
    src/alchemy/darwin/corefoundation/cf_extra.cpp
    src/alchemy/darwin/corefoundation/cf_system.cpp
    src/alchemy/darwin/corefoundation/cf_plist.cpp
    src/alchemy/darwin/corefoundation/cf_preferences.cpp
    src/alchemy/darwin/corefoundation/cf_locale.cpp
    src/alchemy/darwin/corefoundation/cf_file_descriptors.cpp
    src/alchemy/darwin/corefoundation/cf_string_clusters.cpp
    src/alchemy/darwin/corefoundation/install.cpp
    src/alchemy/darwin/corefoundation/user_defaults_fx.cpp
    src/alchemy/darwin/corefoundation/cf_date_formatter_fx.cpp)

function(alchemy_darwin_corefoundation_tests)
    # The CF gaps (sets, character sets, notification centers, strings, arrays,
    # UUIDs, user notifications) over the full provider set (fill_fixture.hpp).
    alchemy_gui_add_unit_test(alchemy_gui_corefoundation_gaps src/alchemy/darwin/corefoundation/tests/cf_gaps_tests.cpp TIMEOUT 180)
    # Property lists, and the CF additions over the export-fill fixture.
    alchemy_gui_add_unit_test(alchemy_gui_corefoundation_additions src/alchemy/darwin/corefoundation/tests/cf_additions_tests.cpp TIMEOUT 120)
    # Chrome's CF additions (cf_string_clusters.cpp) over the full provider set.
    alchemy_gui_add_unit_test(alchemy_gui_corefoundation_string_clusters src/alchemy/darwin/corefoundation/tests/cf_string_clusters_tests.cpp TIMEOUT 180)
    # CF strings (cf_strings.cpp) and the ICU encodings (cf_encodings.cpp) over the full provider set.
    alchemy_gui_add_unit_test(alchemy_gui_corefoundation_strings src/alchemy/darwin/corefoundation/tests/cf_strings_tests.cpp TIMEOUT 180)
    # CFBinaryHeap, CFBitVector, CFTree, CFBag, the CFSet/CFArray/CFDictionary additions and the
    # CFCharacterSet constructors (cf_collections.cpp, cf_sets_notifications.cpp, charset.cpp).
    alchemy_gui_add_unit_test(alchemy_gui_corefoundation_collections src/alchemy/darwin/corefoundation/tests/cf_collections_tests.cpp TIMEOUT 180)
    if(NOT ALCHEMY_GUI_FIXTURES_ENABLED)
        return()
    endif()
    # A guest program for the results in x1 and d0 (CFRange lengths, doubles) and
    # variadic CFStringAppendFormat, linked against its own CoreFoundation stub.
    set(cf_install_name /System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation)
    alchemy_gui_object(cf_strings_fixture ../../../src/alchemy/darwin/corefoundation/tests/cf_strings_fixture.c)
    alchemy_gui_object(cf_strings_link ../../../src/alchemy/darwin/corefoundation/tests/cf_strings_link.c)
    add_custom_command(OUTPUT "${macos_dir}/cf_strings_link.dylib"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib -install_name ${cf_install_name}
            "${macos_dir}/cf_strings_link.o" -o "${macos_dir}/cf_strings_link.dylib"
        DEPENDS "${macos_dir}/cf_strings_link.o" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/cf_strings_fixture"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main "${macos_dir}/cf_strings_fixture.o"
            "${macos_dir}/cf_strings_link.dylib" "${macos_dir}/link_system.dylib" -o "${macos_dir}/cf_strings_fixture"
        DEPENDS "${macos_dir}/cf_strings_fixture.o" "${macos_dir}/cf_strings_link.dylib" "${macos_dir}/link_system.dylib" VERBATIM)
    add_custom_target(alchemy_gui_corefoundation_string_fixtures ALL DEPENDS "${macos_dir}/cf_strings_fixture")
    add_dependencies(alchemy_gui_corefoundation_string_fixtures alchemy_personality_fixtures)
    alchemy_gui_add_fixture_test(alchemy_gui_corefoundation_strings_fixture "${macos_dir}/cf_strings_fixture"
        PROGRESS "Alchemy CF strings fixture: launched" COMPLETE "Alchemy CF strings fixture: complete")
endfunction()
alchemy_gui_register_tests(alchemy_darwin_corefoundation_tests)

# Moved from darwin/identities/framework_gaps.cpp: CFTimeZone, CFUUID,
# CFStringHasPrefix/HasSuffix and kCFAllocatorNull (its own installer-table row).
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/corefoundation/time_zones_and_uuids.cpp)
alchemy_gui_add_framework_case(corefoundation_time_zones_and_uuids src/alchemy/darwin/corefoundation/tests/time_zones_and_uuids_tests.cpp)

# Headless additions (both builds; darwin/headless_additions.hpp), moved from
# darwin/identities/installer_frameworks.cpp:
target_sources(alchemy_core PRIVATE
    src/alchemy/darwin/corefoundation/strings_and_bundles.cpp
    src/alchemy/darwin/corefoundation/cf_strings.cpp)
