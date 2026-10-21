# Run loop, timers, main pump and completion thread. Owner: A2
# (src/alchemy/darwin/runloop/). Contract: include/alchemy/gui/runloop.hpp.
set(alchemy_gui_runloop_sources
    src/alchemy/darwin/runloop/install.cpp
    src/alchemy/darwin/runloop/runloop.cpp
    src/alchemy/darwin/runloop/cf_runloop.cpp
    src/alchemy/darwin/runloop/ns_runloop.cpp
    src/alchemy/darwin/runloop/timers.cpp
    src/alchemy/darwin/runloop/completion_thread.cpp)
list(APPEND ALCHEMY_GUI_SOURCES ${alchemy_gui_runloop_sources})
# ALCHEMY_WAIT_DUMP: the blocking-wait trace (installed by install_support_kit).
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/runloop/wait_trace.cpp)

# NSRunLoop's headless shell (darwin/foundation/headless_shells.hpp): alchemy.exe
# only; in alchemy-fx timers.cpp registers those methods and the _fx twin is
# empty. foundation_extra.cpp calls it.
target_sources(alchemy_core PRIVATE src/alchemy/darwin/runloop/run_loop_headless.cpp)
list(APPEND ALCHEMY_HEADLESS_ONLY_SOURCES src/alchemy/darwin/runloop/run_loop_headless.cpp)
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/runloop/run_loop_fx.cpp)

function(alchemy_gui_runloop_tests)
    alchemy_gui_add_unit_test(alchemy_gui_runloop_smoke src/alchemy/darwin/runloop/tests/runloop_smoke_tests.cpp)
    # Behaviour tests on the reference engine: synthetic guest calls run as
    # host handlers, so run loop, CF/NS providers, completion thread and
    # dispatch are exercised without the fx runtime. The run loop sources are
    # compiled in directly (they are not part of alchemy_core), with the host
    # backend's run-loop calls (host::ui, GUI-only).
    list(TRANSFORM alchemy_gui_runloop_sources PREPEND "${PROJECT_SOURCE_DIR}/" OUTPUT_VARIABLE sources)
    list(APPEND sources "${PROJECT_SOURCE_DIR}/src/alchemy/host/${ALCHEMY_HOST}/gui/run_loop.cpp")
    add_executable(alchemy_gui_runloop_tests src/alchemy/darwin/runloop/tests/runloop_tests.cpp ${sources})
    target_link_libraries(alchemy_gui_runloop_tests PRIVATE alchemy_core)
    target_include_directories(alchemy_gui_runloop_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/alchemy/darwin/runloop")
    if(MSVC)
        target_compile_options(alchemy_gui_runloop_tests PRIVATE /W3 /permissive-)
    endif()
    add_test(NAME alchemy_gui_runloop COMMAND alchemy_gui_runloop_tests)
    set_tests_properties(alchemy_gui_runloop PROPERTIES TIMEOUT 120)
    # The same tests on FxEngine (alchemy_core_fx), where synthetic calls,
    # register-image calls and guest pthreads run through fx.
    alchemy_gui_add_unit_test(alchemy_gui_runloop_fx src/alchemy/darwin/runloop/tests/runloop_tests.cpp TIMEOUT 120)
    target_include_directories(alchemy_gui_runloop_fx PRIVATE "${PROJECT_SOURCE_DIR}/src/alchemy/darwin/runloop")
    # Run-loop behaviour QCocoaEventDispatcher depends on (host-only).
    alchemy_gui_add_unit_test(alchemy_gui_runloop_probe src/alchemy/darwin/runloop/tests/runloop_probe_tests.cpp TIMEOUT 60)
    target_include_directories(alchemy_gui_runloop_probe PRIVATE "${PROJECT_SOURCE_DIR}/src/alchemy/darwin/runloop")

    # Guest fixture on alchemy-fx: CFRunLoop timers/sources/observers, main
    # queue wakes, NSTimer and NSRunLoop, dispatch groups, barriers, apply and
    # sources. Link-only stubs for the interfaces it imports live beside it.
    if(NOT ALCHEMY_GUI_FIXTURES_ENABLED)
        return()
    endif()
    set(fixture_dir "${PROJECT_SOURCE_DIR}/src/alchemy/darwin/runloop/tests")
    set(objects)
    foreach(entry IN ITEMS "runloop_fixture|runloop_fixture.c|-fblocks"
            "runloop_link_system|runloop_link_system.c|"
            "runloop_link_cf|runloop_link_cf.c|"
            "runloop_link_objc|runloop_link_objc.c|")
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 name)
        list(GET parts 1 source)
        list(GET parts 2 flags)
        add_custom_command(OUTPUT "${macos_dir}/${name}.o"
            COMMAND "${ALCHEMY_CLANG}" --target=arm64-apple-macos11 -march=armv8-a -mcpu=generic
                -ffreestanding -fno-builtin -fno-stack-protector -Werror -O1 ${flags}
                -c "${fixture_dir}/${source}" -o "${macos_dir}/${name}.o"
            DEPENDS "${fixture_dir}/${source}" "${fixture_dir}/runloop_fixture.h" VERBATIM)
    endforeach()
    set(stubs)
    foreach(entry IN ITEMS "runloop_link_system|/usr/lib/libSystem.B.dylib"
            "runloop_link_cf|/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation"
            "runloop_link_objc|/usr/lib/libobjc.A.dylib")
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 stub)
        list(GET parts 1 install_name)
        add_custom_command(OUTPUT "${macos_dir}/${stub}.dylib"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib -install_name "${install_name}"
                "${macos_dir}/${stub}.o" -o "${macos_dir}/${stub}.dylib"
            DEPENDS "${macos_dir}/${stub}.o" VERBATIM)
        list(APPEND stubs "${macos_dir}/${stub}.dylib")
    endforeach()
    add_custom_command(OUTPUT "${macos_dir}/runloop_fixture"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
            "${macos_dir}/runloop_fixture.o" ${stubs} -o "${macos_dir}/runloop_fixture"
        DEPENDS "${macos_dir}/runloop_fixture.o" ${stubs} VERBATIM)
    add_custom_target(alchemy_gui_runloop_fixture ALL DEPENDS "${macos_dir}/runloop_fixture")
    add_dependencies(alchemy_gui_runloop_fixture alchemy-fx)
    alchemy_gui_add_fixture_test(alchemy_gui_runloop_fixture "${macos_dir}/runloop_fixture"
        PROGRESS "runloop fixture: start" COMPLETE "runloop fixture: passed" TIMEOUT 120)
endfunction()
alchemy_gui_register_tests(alchemy_gui_runloop_tests)
