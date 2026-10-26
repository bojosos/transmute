# Foundation (src/alchemy/darwin/foundation/; group r2 foundation). Included by
# cmake/AlchemyDarwin.cmake.
#
# Headless (alchemy_core, so alchemy.exe and alchemy-fx): the Foundation
# bridge and its extra classes. GUI set (alchemy-fx only): value classes,
# collections, strings, scanners, predicates, regular expressions, decimal
# numbers, calendars, formatters, JSON, property lists, keyed archives and
# coding, XML, KVC and KVO, notification queues and
# distributed notifications, undo, invocations, exceptions, bundles, file
# manager additions, Cocoa scripting, and libobjc associated objects.
target_sources(alchemy_core PRIVATE
    src/alchemy/darwin/foundation/foundation_bridge.cpp
    src/alchemy/darwin/foundation/foundation_extra.cpp)
# Headless shells (src/alchemy/darwin/foundation/headless_shells.hpp): alchemy.exe
# only; in alchemy-fx the GUI provider owns these names, and the _fx twins
# (below) define the same entry points empty.
set(alchemy_foundation_headless_shells
    src/alchemy/darwin/foundation/notification_center_headless.cpp)
target_sources(alchemy_core PRIVATE ${alchemy_foundation_headless_shells})
list(APPEND ALCHEMY_HEADLESS_ONLY_SOURCES ${alchemy_foundation_headless_shells})
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/foundation/distributed_notifications.cpp
    src/alchemy/darwin/foundation/kvc.cpp
    src/alchemy/darwin/foundation/number_format.cpp
    src/alchemy/darwin/foundation/date_format.cpp
    src/alchemy/darwin/foundation/formatters.cpp
    src/alchemy/darwin/foundation/collections.cpp
    src/alchemy/darwin/foundation/json.cpp
    src/alchemy/darwin/foundation/json_serialization.cpp
    src/alchemy/darwin/foundation/constants.cpp
    src/alchemy/darwin/foundation/file_attributes.cpp
    src/alchemy/darwin/foundation/immutable_copies.cpp
    src/alchemy/darwin/foundation/keyed_archive.cpp
    src/alchemy/darwin/foundation/data_files.cpp
    src/alchemy/darwin/foundation/text.cpp
    src/alchemy/darwin/foundation/regex.cpp
    src/alchemy/darwin/foundation/decimal.cpp
    src/alchemy/darwin/foundation/scan.cpp
    src/alchemy/darwin/foundation/predicate.cpp
    src/alchemy/darwin/foundation/calendar.cpp
    src/alchemy/darwin/foundation/byte_count.cpp
    src/alchemy/darwin/foundation/xml.cpp
    src/alchemy/darwin/foundation/guest.cpp
    src/alchemy/darwin/foundation/class_objects.cpp
    src/alchemy/darwin/foundation/sorting.cpp
    src/alchemy/darwin/foundation/collections_extra.cpp
    src/alchemy/darwin/foundation/strings.cpp
    src/alchemy/darwin/foundation/strings_more.cpp
    src/alchemy/darwin/foundation/collections_more.cpp
    src/alchemy/darwin/foundation/foundation_more.cpp
    src/alchemy/darwin/foundation/scanner.cpp
    src/alchemy/darwin/foundation/regex_provider.cpp
    src/alchemy/darwin/foundation/predicate_provider.cpp
    src/alchemy/darwin/foundation/pointer_collections.cpp
    src/alchemy/darwin/foundation/decimal_provider.cpp
    src/alchemy/darwin/foundation/calendar_provider.cpp
    src/alchemy/darwin/foundation/formatters_extra.cpp
    src/alchemy/darwin/foundation/data_extra.cpp
    src/alchemy/darwin/foundation/xml_provider.cpp
    src/alchemy/darwin/foundation/url_extra.cpp
    src/alchemy/darwin/foundation/coding.cpp
    src/alchemy/darwin/foundation/archives_provider.cpp
    src/alchemy/darwin/foundation/exceptions.cpp
    src/alchemy/darwin/foundation/value_transformers.cpp
    src/alchemy/darwin/foundation/invocation_core.cpp
    src/alchemy/darwin/foundation/invocation.cpp
    src/alchemy/darwin/foundation/undo_core.cpp
    src/alchemy/darwin/foundation/undo.cpp
    src/alchemy/darwin/foundation/kvo.cpp
    src/alchemy/darwin/foundation/kvc_collections.cpp
    src/alchemy/darwin/foundation/notification_queue.cpp
    src/alchemy/darwin/foundation/streams.cpp
    src/alchemy/darwin/foundation/url_components.cpp
    src/alchemy/darwin/foundation/files.cpp
    src/alchemy/darwin/foundation/bundles.cpp
    src/alchemy/darwin/foundation/sdef.cpp
    src/alchemy/darwin/foundation/scripting.cpp
    src/alchemy/darwin/foundation/objc_assoc.cpp
    src/alchemy/darwin/foundation/mutable_string.cpp
    src/alchemy/darwin/foundation/notification_blocks.cpp
    src/alchemy/darwin/foundation/property_list_serialization.cpp
    src/alchemy/darwin/foundation/value_classes.cpp
    src/alchemy/darwin/foundation/notification_center_fx.cpp
    src/alchemy/darwin/foundation/install.cpp)

function(alchemy_darwin_foundation_tests)
    # Host-only cores (no guest): number and date formats, JSON.
    alchemy_gui_add_unit_test(alchemy_gui_foundation_support_unit src/alchemy/darwin/foundation/tests/formats_tests.cpp)
    # Host-only data cores: text, regular expressions, decimals, scanning,
    # predicate formats, calendars, byte counts, XML.
    alchemy_gui_add_unit_test(alchemy_gui_foundation_data_unit src/alchemy/darwin/foundation/tests/data_tests.cpp)
    # Runtime providers over a reference-engine process (exceptions, invocations,
    # undo, KVO, notification queues, operations, files, scripting, CF extras).
    alchemy_gui_add_unit_test(alchemy_gui_foundation_runtime src/alchemy/darwin/foundation/tests/runtime_tests.cpp TIMEOUT 120)
    # The fwgaps Foundation gaps over the full provider set (fill_fixture.hpp).
    alchemy_gui_add_unit_test(alchemy_gui_foundation_gaps src/alchemy/darwin/foundation/tests/foundation_gaps_tests.cpp TIMEOUT 180)
    # Value classes (NSTimeZone, NSUUID, ...) over the export-fill fixture.
    alchemy_gui_add_unit_test(alchemy_gui_foundation_value_classes src/alchemy/darwin/foundation/tests/value_classes_tests.cpp TIMEOUT 120)
    # NSStream / CFStream (streams.cpp): data, memory, buffers, files, bound pairs, loopback sockets and events.
    alchemy_gui_add_unit_test(alchemy_gui_foundation_streams src/alchemy/darwin/foundation/tests/streams_tests.cpp TIMEOUT 120)
    # NSURLComponents, the CFURL additions and NSURL's file-URL additions.
    alchemy_gui_add_unit_test(alchemy_gui_foundation_urls src/alchemy/darwin/foundation/tests/urls_tests.cpp TIMEOUT 120)
    # Core-class additions: strings_more, collections_more, foundation_more, plist streams, NSUserDefaults volatile domains.
    alchemy_gui_add_unit_test(alchemy_gui_foundation_core_classes src/alchemy/darwin/foundation/tests/core_classes_tests.cpp TIMEOUT 120)
    if(NOT ALCHEMY_GUI_FIXTURES_ENABLED)
        return()
    endif()
    # Guest keyed-archive round trips (NSKeyedArchiver -> NSKeyedUnarchiver),
    # a guest NSSecureCoding class included.
    alchemy_gui_object(qlab_archive_fixture ../../../src/alchemy/darwin/foundation/tests/archive_fixture.m)
    alchemy_gui_program(qlab_archive_fixture qlab_archive_fixture)
    add_custom_target(alchemy_gui_foundation_data_fixtures ALL DEPENDS "${macos_dir}/qlab_archive_fixture")
    add_dependencies(alchemy_gui_foundation_data_fixtures alchemy_gui_link_stubs)
    alchemy_gui_add_fixture_test(alchemy_gui_foundation_archive_fixture "${macos_dir}/qlab_archive_fixture"
        PROGRESS "Alchemy keyed archive fixture: launched" COMPLETE "Alchemy keyed archive fixture: complete")
    # Foundation's exceptions and forwarding (exceptions.cpp, invocation.cpp): @catch of -raise and
    # +raise:format:, and -forwardInvocation:. Throwing unwinds through the guest LLVM libc++abi and
    # libunwind, which only a --runtime-dir supplies, so the fixture links them and runs with it; the
    # runtime and class symbols the link stubs lack resolve by flat lookup.
    if(EXISTS "${ALCHEMY_LLVM_RUNTIME_DIR}/libc++abi.dylib" AND EXISTS "${ALCHEMY_LLVM_RUNTIME_DIR}/libunwind.dylib")
        alchemy_gui_object(foundation_exceptions_fixture ../../../src/alchemy/darwin/foundation/tests/exceptions_fixture.m
            -fobjc-exceptions -fexceptions)
        add_custom_command(OUTPUT "${macos_dir}/foundation_exceptions_fixture"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main -undefined dynamic_lookup -rpath @loader_path "-L${ALCHEMY_LLVM_RUNTIME_DIR}"
                "${macos_dir}/foundation_exceptions_fixture.o" "${ALCHEMY_LLVM_RUNTIME_DIR}/libc++abi.dylib"
                "${ALCHEMY_LLVM_RUNTIME_DIR}/libunwind.dylib" ${ALCHEMY_GUI_LINK_STUBS} -o "${macos_dir}/foundation_exceptions_fixture"
            DEPENDS "${macos_dir}/foundation_exceptions_fixture.o" ${ALCHEMY_GUI_LINK_STUBS} VERBATIM)
        add_custom_target(alchemy_gui_foundation_exceptions_fixtures ALL DEPENDS "${macos_dir}/foundation_exceptions_fixture")
        add_dependencies(alchemy_gui_foundation_exceptions_fixtures alchemy_gui_link_stubs)
        add_test(NAME alchemy_gui_foundation_exceptions_fixture
            COMMAND alchemy-fx --runtime-dir "${ALCHEMY_LLVM_RUNTIME_DIR}" "${macos_dir}/foundation_exceptions_fixture")
        set_tests_properties(alchemy_gui_foundation_exceptions_fixture PROPERTIES TIMEOUT 120
            PASS_REGULAR_EXPRESSION "Alchemy Foundation exceptions fixture: 111 1")
    endif()
endfunction()
alchemy_gui_register_tests(alchemy_darwin_foundation_tests)


# Moved from darwin/identities/: file_attribute_keys.cpp (NSFileModificationDate;
# its own installer-table row) and threads_and_localizations.cpp (NSThread,
# NSBundle localizations, class shells; the support kit).
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/foundation/file_attribute_keys.cpp
    src/alchemy/darwin/foundation/threads_and_localizations.cpp)
alchemy_gui_add_framework_case(foundation_file_attribute_keys src/alchemy/darwin/foundation/tests/file_attribute_keys_tests.cpp)

# Headless additions (both builds; darwin/headless_additions.hpp), moved from
# darwin/identities/installer_frameworks.cpp:
target_sources(alchemy_core PRIVATE
    src/alchemy/darwin/foundation/class_shells_and_events.cpp
    src/alchemy/darwin/foundation/geometry.cpp)
