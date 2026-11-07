# The session launchd (src/alchemy/darwin/launchd/; owner: launchd-xpc): the
# per-user job table, its server (`alchemy launchd`), launchctl as a built-in
# tool child, and the host-side property-list reader. Headless: part of
# alchemy.exe and alchemy-fx. See its README.md and cmake/AlchemyDarwin.cmake
# for the fragment pattern.
target_sources(alchemy_core PRIVATE
    src/alchemy/darwin/launchd/plist.cpp
    src/alchemy/darwin/launchd/job.cpp
    src/alchemy/darwin/launchd/client.cpp
    src/alchemy/darwin/launchd/server.cpp
    src/alchemy/darwin/launchd/launchctl.cpp)
# GUI (alchemy-fx only): ServiceManagement's job calls and launch_activate_socket
# (installed by libsystem/misc/system_c.cpp's install_system_functions).
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/launchd/service_management.cpp)
# GUI (alchemy-fx only): the MachServices ports (the cross-process Mach layer),
# passed to the server by main().
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/launchd/mach_services.cpp)
# GUI (alchemy-fx only): XPC connections, endpoints, replies and xpc_main (installed by
# libsystem/misc/xpc_objects.cpp's install_libsystem_xpc after the XPC objects).
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/launchd/xpc_connections.cpp)
# GUI (alchemy-fx only): NSXPCConnection, NSXPCListener, NSXPCInterface (moved from
# libsystem/misc/xpc.cpp; installed by kit/completion/runtime/install.cpp's install_xpc).
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/launchd/nsxpc.cpp)
alchemy_gui_add_framework_case(launchd_service_management src/alchemy/darwin/launchd/tests/service_management_tests.cpp)

if(BUILD_TESTING)
    add_executable(alchemy_launchd_tests src/alchemy/darwin/launchd/tests/launchd_tests.cpp)
    target_link_libraries(alchemy_launchd_tests PRIVATE alchemy_core)
    add_test(NAME alchemy_launchd_units COMMAND alchemy_launchd_tests)
    set_tests_properties(alchemy_launchd_units PROPERTIES TIMEOUT 60)
endif()

# launchctl end to end: a guest program bootstraps, lists, prints,
# kickstarts, signals and boots out a job of the session launchd, submits
# and removes another, and uses the session environment. Run by the process
# model driver (its job object must be empty soon after the test: the session
# launchd ends with the test's process tree, at once with LINGER and GRACE 0). Registered for after configuration
# (alchemy-fx and the process model driver are defined later).
function(alchemy_launchd_session_tests)
    if(NOT (ALCHEMY_CLANG AND ALCHEMY_LD64_LLD AND macos_dir AND TARGET alchemy_process_model_tests))
        return()
    endif()
    alchemy_object(launchd_system launchd_system.c)
    alchemy_object(launchd_model launchd_model.c)
    add_custom_command(OUTPUT "${macos_dir}/launchd_system.dylib"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib
            -install_name /usr/lib/libSystem.B.dylib "${macos_dir}/launchd_system.o"
            -o "${macos_dir}/launchd_system.dylib"
        DEPENDS "${macos_dir}/launchd_system.o" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/launchd_model"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
            "${macos_dir}/launchd_model.o" "${macos_dir}/launchd_system.dylib" -o "${macos_dir}/launchd_model"
        DEPENDS "${macos_dir}/launchd_model.o" "${macos_dir}/launchd_system.dylib" VERBATIM)
    # LaunchdModel.app: launchd_model with a bundled XPC service (L4).
    alchemy_object(launchd_xpc_service launchd_xpc_service.c)
    add_custom_command(OUTPUT "${macos_dir}/launchd_xpc_service"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
            "${macos_dir}/launchd_xpc_service.o" "${macos_dir}/launchd_system.dylib" -o "${macos_dir}/launchd_xpc_service"
        DEPENDS "${macos_dir}/launchd_xpc_service.o" "${macos_dir}/launchd_system.dylib" VERBATIM)
    set(app "${macos_dir}/LaunchdModel.app/Contents")
    set(service "${app}/XPCServices/com.alchemy.test.echo.xpc/Contents")
    file(MAKE_DIRECTORY "${app}/MacOS" "${service}/MacOS")
    file(WRITE "${app}/Info.plist" "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict>\n<key>CFBundleIdentifier</key><string>com.alchemy.test.launchd-model</string>\n<key>CFBundleExecutable</key><string>launchd_model</string>\n</dict></plist>\n")
    file(WRITE "${service}/Info.plist" "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict>\n<key>CFBundleIdentifier</key><string>com.alchemy.test.echo</string>\n<key>CFBundleExecutable</key><string>echo_service</string>\n<key>CFBundlePackageType</key><string>XPC!</string>\n<key>XPCService</key><dict><key>ServiceType</key><string>Application</string>\n<key>EnvironmentVariables</key><dict><key>ALCHEMY_TEST_XPC_ENV</key><string>from-info-plist</string></dict></dict>\n</dict></plist>\n")
    add_custom_command(OUTPUT "${app}/MacOS/launchd_model" "${service}/MacOS/echo_service"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${macos_dir}/launchd_model" "${app}/MacOS/launchd_model"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${macos_dir}/launchd_xpc_service" "${service}/MacOS/echo_service"
        DEPENDS "${macos_dir}/launchd_model" "${macos_dir}/launchd_xpc_service" VERBATIM)
    add_custom_target(alchemy_launchd_fixtures DEPENDS "${macos_dir}/launchd_model" "${app}/MacOS/launchd_model" "${service}/MacOS/echo_service")
    add_dependencies(alchemy_process_model_tests alchemy_launchd_fixtures)
    add_test(NAME alchemy_launchd_session_fx COMMAND alchemy_process_model_tests "$<TARGET_FILE:alchemy-fx>" "${macos_dir}/launchd_model" launchctl mach sockets xpc)
    set_tests_properties(alchemy_launchd_session_fx PROPERTIES TIMEOUT 600
        ENVIRONMENT "ALCHEMY_GUI_NO_ACTIVATE=1;ALCHEMY_GUI_CLOAK=1;ALCHEMY_GUI_NO_CRASH_DIALOG=1;ALCHEMY_NETWORK=loopback;ALCHEMY_LAUNCHD_LINGER=0;ALCHEMY_LAUNCHD_GRACE=0")
    # Bundled XPC services: the model run from inside its application bundle.
    add_test(NAME alchemy_launchd_xpc_bundle_fx COMMAND alchemy_process_model_tests "$<TARGET_FILE:alchemy-fx>" "${app}/MacOS/launchd_model" xpc-bundle)
    set_tests_properties(alchemy_launchd_xpc_bundle_fx PROPERTIES TIMEOUT 600
        ENVIRONMENT "ALCHEMY_GUI_NO_ACTIVATE=1;ALCHEMY_GUI_CLOAK=1;ALCHEMY_GUI_NO_CRASH_DIALOG=1;ALCHEMY_NETWORK=loopback;ALCHEMY_LAUNCHD_LINGER=0;ALCHEMY_LAUNCHD_GRACE=0")
    # NsxpcModel.app (L5): an Objective-C client and its bundled NSXPC service, one source
    # (launchd_nsxpc.m) built twice; Foundation and the runtime resolve by flat lookup.
    if(NOT ALCHEMY_GUI_FIXTURES_ENABLED)
        return()
    endif()
    alchemy_gui_object(launchd_nsxpc_client launchd_nsxpc.m)
    alchemy_gui_object(launchd_nsxpc_service launchd_nsxpc.m -DNSXPC_SERVICE)
    set(nsxpc_app "${macos_dir}/NsxpcModel.app/Contents")
    set(nsxpc_service "${nsxpc_app}/XPCServices/com.alchemy.test.nsxpc.xpc/Contents")
    file(MAKE_DIRECTORY "${nsxpc_app}/MacOS" "${nsxpc_service}/MacOS")
    file(WRITE "${nsxpc_app}/Info.plist" "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict>\n<key>CFBundleIdentifier</key><string>com.alchemy.test.nsxpc-model</string>\n<key>CFBundleExecutable</key><string>nsxpc_client</string>\n</dict></plist>\n")
    file(WRITE "${nsxpc_service}/Info.plist" "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict>\n<key>CFBundleIdentifier</key><string>com.alchemy.test.nsxpc</string>\n<key>CFBundleExecutable</key><string>nsxpc_service</string>\n<key>CFBundlePackageType</key><string>XPC!</string>\n<key>XPCService</key><dict><key>ServiceType</key><string>Application</string></dict>\n</dict></plist>\n")
    foreach(pair IN ITEMS "launchd_nsxpc_client|${nsxpc_app}/MacOS/nsxpc_client" "launchd_nsxpc_service|${nsxpc_service}/MacOS/nsxpc_service")
        string(REPLACE "|" ";" parts "${pair}")
        list(GET parts 0 object)
        list(GET parts 1 output)
        add_custom_command(OUTPUT "${output}"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main -undefined dynamic_lookup
                "${macos_dir}/${object}.o" ${ALCHEMY_GUI_LINK_STUBS} -o "${output}"
            DEPENDS "${macos_dir}/${object}.o" ${ALCHEMY_GUI_LINK_STUBS} VERBATIM)
    endforeach()
    add_custom_target(alchemy_launchd_nsxpc_fixtures DEPENDS "${nsxpc_app}/MacOS/nsxpc_client" "${nsxpc_service}/MacOS/nsxpc_service")
    add_dependencies(alchemy_launchd_nsxpc_fixtures alchemy_gui_link_stubs)
    add_dependencies(alchemy_process_model_tests alchemy_launchd_nsxpc_fixtures)
    add_test(NAME alchemy_launchd_nsxpc_fx COMMAND alchemy_process_model_tests "$<TARGET_FILE:alchemy-fx>" "${nsxpc_app}/MacOS/nsxpc_client" nsxpc)
    set_tests_properties(alchemy_launchd_nsxpc_fx PROPERTIES TIMEOUT 600
        ENVIRONMENT "ALCHEMY_GUI_NO_ACTIVATE=1;ALCHEMY_GUI_CLOAK=1;ALCHEMY_GUI_NO_CRASH_DIALOG=1;ALCHEMY_NETWORK=loopback;ALCHEMY_LAUNCHD_LINGER=0;ALCHEMY_LAUNCHD_GRACE=0")
endfunction()
alchemy_gui_register_tests(alchemy_launchd_session_tests)
