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
# model driver (its job object must be empty afterwards: the session launchd
# ends with the test's process tree). Registered for after configuration
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
    add_custom_target(alchemy_launchd_fixtures DEPENDS "${macos_dir}/launchd_model")
    add_dependencies(alchemy_process_model_tests alchemy_launchd_fixtures)
    add_test(NAME alchemy_launchd_session_fx COMMAND alchemy_process_model_tests "$<TARGET_FILE:alchemy-fx>" "${macos_dir}/launchd_model" launchctl mach sockets)
    set_tests_properties(alchemy_launchd_session_fx PROPERTIES TIMEOUT 600
        ENVIRONMENT "ALCHEMY_GUI_NO_ACTIVATE=1;ALCHEMY_GUI_CLOAK=1;ALCHEMY_GUI_NO_CRASH_DIALOG=1;ALCHEMY_NETWORK=loopback")
endfunction()
alchemy_gui_register_tests(alchemy_launchd_session_tests)
