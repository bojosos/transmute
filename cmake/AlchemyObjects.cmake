if(BUILD_TESTING AND ALCHEMY_CLANG AND ALCHEMY_LD64_LLD)
    execute_process(COMMAND "${ALCHEMY_CLANG}" -print-resource-dir
        OUTPUT_VARIABLE personality_resource_dir OUTPUT_STRIP_TRAILING_WHITESPACE)
    function(alchemy_personality_object name source)
        set(personality_language_flags)
        if(source MATCHES "\\.m$")
            list(APPEND personality_language_flags -fno-objc-exceptions)
        endif()
        add_custom_command(OUTPUT "${macos_dir}/${name}.o"
            COMMAND "${ALCHEMY_CLANG}" --target=arm64-apple-macos11
                -march=armv8-a -mcpu=generic -nostdinc -fno-builtin
                -fno-stack-protector -Werror -O1 ${personality_language_flags} ${ARGN}
                -isystem "${personality_resource_dir}/include"
                -I "${CMAKE_SOURCE_DIR}/runtime/include" -c
                "${CMAKE_SOURCE_DIR}/tests/fixtures/macos/${source}"
                -o "${macos_dir}/${name}.o"
            DEPENDS "tests/fixtures/macos/${source}"
                tests/fixtures/macos/alchemy_objc_fixture.h
                tests/fixtures/macos/objc_leaf.h
                tests/fixtures/macos/objc_runtime_shared.h
                runtime/objc/AlchemyObjC.h
                runtime/include/CoreFoundation/CoreFoundation.h
                runtime/include/stdio.h runtime/include/string.h VERBATIM)
    endfunction()
    foreach(name IN ITEMS link_objc link_foundation objc_basic objc_leaf objc_bridge_main
            objc_plugin objc_plugin_main objc_thread_pools objc_unsupported cf_bridge)
        alchemy_personality_object(${name} ${name}.m)
    endforeach()
    foreach(name IN ITEMS objc_arc objc_runtime objc_runtime_dylib)
        alchemy_personality_object(${name} ${name}.m -fobjc-arc)
    endforeach()
    foreach(name IN ITEMS link_core_foundation cf_workload)
        alchemy_personality_object(${name} ${name}.c)
    endforeach()
    add_custom_command(OUTPUT "${macos_dir}/link_objc.dylib"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib
            -install_name /usr/lib/libobjc.A.dylib "${macos_dir}/link_objc.o"
            -o "${macos_dir}/link_objc.dylib"
        DEPENDS "${macos_dir}/link_objc.o" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/link_core_foundation.dylib"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib
            -install_name /System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation
            "${macos_dir}/link_core_foundation.o" -o "${macos_dir}/link_core_foundation.dylib"
        DEPENDS "${macos_dir}/link_core_foundation.o" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/link_foundation.dylib"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib
            -install_name /System/Library/Frameworks/Foundation.framework/Versions/C/Foundation
            "${macos_dir}/link_foundation.o" "${macos_dir}/link_objc.dylib"
            -o "${macos_dir}/link_foundation.dylib"
        DEPENDS "${macos_dir}/link_foundation.o" "${macos_dir}/link_objc.dylib" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/objc_leaf.dylib"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib
            -install_name @rpath/objc_leaf.dylib "${macos_dir}/objc_leaf.o"
            "${macos_dir}/link_objc.dylib" -o "${macos_dir}/objc_leaf.dylib"
        DEPENDS "${macos_dir}/objc_leaf.o" "${macos_dir}/link_objc.dylib" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/libobjc_runtime.dylib"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib
            -install_name @loader_path/libobjc_runtime.dylib "${macos_dir}/objc_runtime_dylib.o"
            "${macos_dir}/link_objc.dylib" -o "${macos_dir}/libobjc_runtime.dylib"
        DEPENDS "${macos_dir}/objc_runtime_dylib.o" "${macos_dir}/link_objc.dylib" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/objc_plugin.bundle"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -bundle -rpath @loader_path
            "${macos_dir}/objc_plugin.o" "${macos_dir}/objc_leaf.dylib"
            "${macos_dir}/link_objc.dylib" -o "${macos_dir}/objc_plugin.bundle"
        DEPENDS "${macos_dir}/objc_plugin.o" "${macos_dir}/objc_leaf.dylib"
            "${macos_dir}/link_objc.dylib" VERBATIM)
    foreach(name IN ITEMS objc_basic objc_arc objc_thread_pools objc_runtime
            objc_bridge_main objc_plugin_main objc_unsupported cf_bridge cf_workload)
        set(personality_libraries "${macos_dir}/link_system.dylib")
        if(NOT name STREQUAL "cf_workload")
            list(APPEND personality_libraries "${macos_dir}/link_objc.dylib")
        endif()
        if(name STREQUAL "objc_bridge_main" OR name STREQUAL "objc_plugin_main")
            list(APPEND personality_libraries "${macos_dir}/objc_leaf.dylib")
        elseif(name STREQUAL "objc_runtime")
            list(APPEND personality_libraries "${macos_dir}/libobjc_runtime.dylib")
        elseif(name STREQUAL "cf_bridge")
            list(APPEND personality_libraries "${macos_dir}/link_foundation.dylib")
        endif()
        if(name STREQUAL "cf_bridge" OR name STREQUAL "cf_workload")
            list(APPEND personality_libraries "${macos_dir}/link_core_foundation.dylib")
        endif()
        add_custom_command(OUTPUT "${macos_dir}/${name}"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main -rpath @loader_path
                "${macos_dir}/${name}.o" ${personality_libraries} -o "${macos_dir}/${name}"
            DEPENDS "${macos_dir}/${name}.o" ${personality_libraries} VERBATIM)
        list(APPEND personality_programs "${macos_dir}/${name}")
    endforeach()
    add_custom_target(alchemy_personality_fixtures DEPENDS ${personality_programs}
        "${macos_dir}/objc_plugin.bundle")
    add_dependencies(alchemy_personality_fixtures alchemy_macos_fixtures)
    add_executable(alchemy_personality_tests tests/alchemy_personality_tests.cpp)
    target_link_libraries(alchemy_personality_tests PRIVATE alchemy_core)
    add_dependencies(alchemy_personality_tests alchemy_personality_fixtures)
    add_test(NAME alchemy_objc_dispatch COMMAND alchemy_personality_tests
        "${macos_dir}/objc_basic" "Objective-C dispatch: 42 27")
    add_test(NAME alchemy_objc_dylib COMMAND alchemy_personality_tests
        "${macos_dir}/objc_bridge_main" "Objective-C dylib: 12 27")
    add_test(NAME alchemy_objc_plugin COMMAND alchemy_personality_tests
        "${macos_dir}/objc_plugin_main" "Objective-C plugin: 42 27" "${macos_dir}/objc_plugin.bundle")
    add_test(NAME alchemy_objc_arc COMMAND alchemy_personality_tests
        "${macos_dir}/objc_arc" "Objective-C ARC: 1")
    add_test(NAME alchemy_objc_abi COMMAND alchemy_personality_tests
        "${macos_dir}/objc_runtime" "Objective-C runtime: 27 42")
    add_test(NAME alchemy_objc_worker_pools COMMAND alchemy_personality_tests
        "${macos_dir}/objc_thread_pools" "Objective-C worker pools: 41")
    add_test(NAME alchemy_cf_bridge COMMAND alchemy_personality_tests
        "${macos_dir}/cf_bridge" "CoreFoundation Objective-C bridge: 27")
    add_test(NAME alchemy_cf_workload COMMAND alchemy_personality_tests
        "${macos_dir}/cf_workload" "CoreFoundation objects: 27 4 6")
    add_test(NAME alchemy_objc_doctor COMMAND "${CMAKE_COMMAND}"
        "-DALCHEMY=$<TARGET_FILE:alchemy>" "-DPROGRAM=${macos_dir}/cf_bridge"
        -P "${CMAKE_SOURCE_DIR}/tests/alchemy_doctor_smoke.cmake")
    add_test(NAME alchemy_objc_unsupported COMMAND "${CMAKE_COMMAND}"
        "-DALCHEMY=$<TARGET_FILE:alchemy>" "-DPROGRAM=${macos_dir}/objc_unsupported"
        -P "${CMAKE_SOURCE_DIR}/tests/alchemy_objc_negative.cmake")
    set_tests_properties(alchemy_objc_dispatch alchemy_objc_dylib alchemy_objc_plugin
        alchemy_objc_arc alchemy_objc_abi alchemy_objc_worker_pools alchemy_cf_bridge
        alchemy_cf_workload alchemy_objc_doctor alchemy_objc_unsupported PROPERTIES TIMEOUT 120)
endif()
