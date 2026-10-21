# Objective-C runtime for the GUI. Owner: A1 (src/alchemy/objc/,
# src/alchemy/objc_runtime.cpp, runtime/objc/dispatch.S, object_store.cpp).
# The runtime itself (method cache, host->guest send API, host_state, Blocks
# classes, weak references) lives in objc_runtime.cpp, part of alchemy_core
# for both engines; this fragment holds fx-only additions (none yet) and the
# tests.
#
# Method cache coverage: every Objective-C guest fixture runs with the cache
# (default) and with ALCHEMY_OBJC_CACHE=0, on the reference engine
# (alchemy_personality_tests: interpreter, JIT, rebased) and on alchemy-fx.
function(alchemy_gui_objc_tests)
    alchemy_gui_add_unit_test(alchemy_gui_objc_smoke src/alchemy/objc/tests/objc_smoke_tests.cpp)
    alchemy_gui_add_unit_test(alchemy_gui_objc_runtime src/alchemy/objc/tests/objc_runtime_tests.cpp TIMEOUT 300)
    if(NOT TARGET alchemy_personality_tests)
        return()
    endif()
    # Fixtures of this fragment: src/alchemy/objc/tests/fixtures/<name>.m.
    # Runtime symbols the shared link stubs lack resolve by flat lookup.
    set(objc_fixture_dir "${PROJECT_SOURCE_DIR}/src/alchemy/objc/tests/fixtures")
    set(objc_programs)
    foreach(entry IN ITEMS "objc_blocks|" "objc_weak|-fobjc-arc")
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 name)
        list(GET parts 1 flags)
        add_custom_command(OUTPUT "${macos_dir}/${name}.o"
            COMMAND "${ALCHEMY_CLANG}" --target=arm64-apple-macos11
                -march=armv8-a -mcpu=generic -nostdinc -fno-builtin
                -fno-stack-protector -Werror -O1 -fno-objc-exceptions ${flags}
                -isystem "${personality_resource_dir}/include"
                -I "${PROJECT_SOURCE_DIR}/runtime/include" -I "${PROJECT_SOURCE_DIR}/tests/fixtures/macos" -c
                "${objc_fixture_dir}/${name}.m" -o "${macos_dir}/${name}.o"
            DEPENDS "${objc_fixture_dir}/${name}.m" "${PROJECT_SOURCE_DIR}/tests/fixtures/macos/alchemy_objc_fixture.h" VERBATIM)
        add_custom_command(OUTPUT "${macos_dir}/${name}"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main -undefined dynamic_lookup
                "${macos_dir}/${name}.o" "${macos_dir}/link_system.dylib" "${macos_dir}/link_objc.dylib"
                -o "${macos_dir}/${name}"
            DEPENDS "${macos_dir}/${name}.o" "${macos_dir}/link_system.dylib" "${macos_dir}/link_objc.dylib" VERBATIM)
        list(APPEND objc_programs "${macos_dir}/${name}")
    endforeach()
    # The hand-encoded dispatcher in objc_runtime.cpp must equal dispatch.S.
    add_custom_command(OUTPUT "${macos_dir}/objc_dispatch.o"
        COMMAND "${ALCHEMY_CLANG}" --target=arm64-apple-macos11 -c "${PROJECT_SOURCE_DIR}/runtime/objc/dispatch.S"
            -o "${macos_dir}/objc_dispatch.o"
        DEPENDS "${PROJECT_SOURCE_DIR}/runtime/objc/dispatch.S" VERBATIM)
    add_custom_target(alchemy_gui_objc_fixtures ALL DEPENDS ${objc_programs} "${macos_dir}/objc_dispatch.o")
    add_dependencies(alchemy_gui_objc_fixtures alchemy_personality_fixtures)
    add_test(NAME alchemy_gui_objc_dispatch_encoding COMMAND alchemy_gui_objc_runtime --encoding "${macos_dir}/objc_dispatch.o")

    # name | program | expected stdout line
    set(objc_cases
        "dispatch|objc_basic|Objective-C dispatch: 42 27"
        "abi|objc_runtime|Objective-C runtime: 27 42"
        "arc|objc_arc|Objective-C ARC: 1"
        "worker_pools|objc_thread_pools|Objective-C worker pools: 41"
        "cf_bridge|cf_bridge|CoreFoundation Objective-C bridge: 27"
        "blocks|objc_blocks|Objective-C blocks: 27 1"
        "weak|objc_weak|Objective-C weak: 42 1 1")
    foreach(entry IN LISTS objc_cases)
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 name)
        list(GET parts 1 program)
        list(GET parts 2 expected)
        foreach(mode IN ITEMS cache nocache)
            set(environment "ALCHEMY_OBJC_CACHE=1")
            if(mode STREQUAL "nocache")
                set(environment "ALCHEMY_OBJC_CACHE=0")
            endif()
            # The shared fixtures already run cached as alchemy_objc_* tests.
            if(mode STREQUAL "nocache" OR name STREQUAL "blocks" OR name STREQUAL "weak")
                add_test(NAME alchemy_objc_${mode}_${name} COMMAND alchemy_personality_tests
                    "${macos_dir}/${program}" "${expected}")
                set_tests_properties(alchemy_objc_${mode}_${name} PROPERTIES TIMEOUT 120 ENVIRONMENT "${environment}")
            endif()
            # The expected lines contain no regex metacharacters.
            alchemy_gui_add_fixture_test(alchemy_gui_objc_fx_${mode}_${name} "${macos_dir}/${program}"
                PROGRESS "${expected}" COMPLETE "${expected}")
            set_tests_properties(alchemy_gui_objc_fx_${mode}_${name} PROPERTIES ENVIRONMENT "${environment}")
        endforeach()
    endforeach()
endfunction()
alchemy_gui_register_tests(alchemy_gui_objc_tests)

# libobjc's exception type info and personality (exception_types.cpp; moved
# from darwin/identities/system_gaps.cpp; fill-only, kit/fwgaps).
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/objc/exception_types.cpp)
alchemy_gui_add_framework_case(objc_exception_types src/alchemy/objc/tests/exception_types_tests.cpp)
