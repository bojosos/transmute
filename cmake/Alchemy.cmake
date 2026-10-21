add_library(alchemy_core
    src/alchemy/macho.cpp
    src/alchemy/process_context.cpp
    src/alchemy/darwin_providers.cpp
    src/alchemy/registration_audit.cpp
    src/alchemy/objc_runtime.cpp
    src/alchemy/objc_metadata.cpp
    src/alchemy/bundle.cpp
    src/alchemy/module_loader.cpp
    src/alchemy/runtime_libraries.cpp
    src/alchemy/inspection.cpp
    src/alchemy/install/alchemy_root.cpp
    src/alchemy/install/install.cpp
    src/alchemy/perf.cpp
    src/alchemy/process.cpp)
target_link_libraries(alchemy_core PUBLIC transmute_core)
if(WIN32)
    target_link_libraries(alchemy_core PRIVATE mincore bcrypt advapi32 ws2_32 iphlpapi normaliz)
endif()
# src: provider sources include each other as "alchemy/<path>" (src-rooted).
target_include_directories(alchemy_core PUBLIC include src)
# alchemy::hash_map / hash_set (include/alchemy/hash_map.hpp): tsl::robin_map
# (third_party/robin-map, MIT, header-only) or std::unordered_map when OFF.
option(ALCHEMY_ROBIN_MAP "alchemy::hash_map is tsl::robin_map (OFF: std::unordered_map)" ON)
function(alchemy_hash_map_options target)
    target_include_directories(${target} SYSTEM PUBLIC "${PROJECT_SOURCE_DIR}/third_party/robin-map/include")
    target_compile_definitions(${target} PUBLIC ALCHEMY_ROBIN_MAP=$<BOOL:${ALCHEMY_ROBIN_MAP}>)
endfunction()
alchemy_hash_map_options(alchemy_core)
if(MSVC)
    target_compile_options(alchemy_core PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(alchemy_core PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_executable(alchemy src/alchemy/main.cpp)
if(MSVC)
    # longPathAware: installed bundles (Chrome's helpers) exceed MAX_PATH.
    target_sources(alchemy PRIVATE src/alchemy/alchemy.manifest)
endif()
target_link_libraries(alchemy PRIVATE alchemy_core)
if(MSVC)
    target_compile_options(alchemy PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(alchemy PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

if(BUILD_TESTING)
    foreach(group IN ITEMS engine macho darwin threads services objects objc objc_metadata core_foundation mach_vm native numeric network frameworks install)
        add_executable(alchemy_${group}_tests tests/alchemy_${group}_tests.cpp)
        if(group STREQUAL "engine")
            target_link_libraries(alchemy_${group}_tests PRIVATE transmute_core)
        else()
            target_link_libraries(alchemy_${group}_tests PRIVATE alchemy_core)
        endif()
        add_test(NAME alchemy_${group} COMMAND alchemy_${group}_tests)
        set_tests_properties(alchemy_${group} PROPERTIES TIMEOUT 60)
    endforeach()
    set(ALCHEMY_EXTERNAL_SMOKE_BINARY "" CACHE FILEPATH "Optional user-owned Mach-O smoke binary; never copied or distributed")
    get_filename_component(alchemy_llvm_bin "${TRANS_MUTE_CLANG_CL}" DIRECTORY)
    find_program(ALCHEMY_CLANG clang HINTS "${alchemy_llvm_bin}"
        "${TRANS_MUTE_LLVM_HINT}" "${CMAKE_GENERATOR_INSTANCE}/VC/Tools/Llvm/bin")
    find_program(ALCHEMY_LD64_LLD ld64.lld HINTS "${alchemy_llvm_bin}"
        "${TRANS_MUTE_LLVM_HINT}" "${CMAKE_GENERATOR_INSTANCE}/VC/Tools/Llvm/bin")
    if(ALCHEMY_CLANG AND ALCHEMY_LD64_LLD)
        set(macos_dir "${CMAKE_CURRENT_BINARY_DIR}/macos")
        file(MAKE_DIRECTORY "${macos_dir}")
        function(alchemy_object name source)
            add_custom_command(OUTPUT "${macos_dir}/${name}.o"
                COMMAND "${ALCHEMY_CLANG}" --target=arm64-apple-macos11
                    -ffreestanding -fno-builtin -fno-stack-protector -O1 -c
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/macos/${source}"
                    -o "${macos_dir}/${name}.o"
                DEPENDS "tests/fixtures/macos/${source}" VERBATIM)
        endfunction()
        alchemy_object(link_system link_system.c)
        alchemy_object(link_services link_services.c)
        alchemy_object(link_cpp link_cpp.c)
        alchemy_object(hello_helper hello_helper.c)
        foreach(name IN ITEMS hello dylib_app startup constructor_exit unsupported_format strong_import weak_import)
            alchemy_object(${name} ${name}.c)
        endforeach()
        alchemy_object(syscalls syscalls.s)
        set(macos_link_options -arch arm64 -platform_version macos 11.0 11.0 -fixup_chains)
        add_custom_command(OUTPUT "${macos_dir}/link_system.dylib"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib
                -install_name /usr/lib/libSystem.B.dylib "${macos_dir}/link_system.o" "${macos_dir}/link_services.o"
                -o "${macos_dir}/link_system.dylib"
            DEPENDS "${macos_dir}/link_system.o" "${macos_dir}/link_services.o" VERBATIM)
        add_custom_command(OUTPUT "${macos_dir}/link_cpp.dylib"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib
                -install_name /usr/lib/libc++.1.dylib "${macos_dir}/link_cpp.o"
                -o "${macos_dir}/link_cpp.dylib"
            DEPENDS "${macos_dir}/link_cpp.o" VERBATIM)
        add_custom_command(OUTPUT "${macos_dir}/hello_helper.dylib"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib
                -install_name @loader_path/hello_helper.dylib "${macos_dir}/hello_helper.o"
                -o "${macos_dir}/hello_helper.dylib"
            DEPENDS "${macos_dir}/hello_helper.o" VERBATIM)
        foreach(name IN ITEMS hello startup unsupported_format strong_import weak_import)
            add_custom_command(OUTPUT "${macos_dir}/${name}"
                COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
                    "${macos_dir}/${name}.o" -needed_library "${macos_dir}/link_cpp.dylib"
                    "${macos_dir}/link_system.dylib" -o "${macos_dir}/${name}"
                DEPENDS "${macos_dir}/${name}.o" "${macos_dir}/link_system.dylib"
                    "${macos_dir}/link_cpp.dylib" VERBATIM)
        endforeach()
        foreach(name IN ITEMS syscalls constructor_exit)
            add_custom_command(OUTPUT "${macos_dir}/${name}"
                COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
                    "${macos_dir}/${name}.o" -o "${macos_dir}/${name}"
                DEPENDS "${macos_dir}/${name}.o" VERBATIM)
        endforeach()
        add_custom_command(OUTPUT "${macos_dir}/dylib_app"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
                "${macos_dir}/dylib_app.o" "${macos_dir}/hello_helper.dylib"
                "${macos_dir}/link_system.dylib" -o "${macos_dir}/dylib_app"
            DEPENDS "${macos_dir}/dylib_app.o" "${macos_dir}/hello_helper.dylib"
                "${macos_dir}/link_system.dylib" VERBATIM)
        set(macos_fixture_arguments "${macos_dir}/hello" "${macos_dir}/dylib_app"
            "${macos_dir}/syscalls" "${macos_dir}/startup"
            "${macos_dir}/constructor_exit" "${macos_dir}/unsupported_format"
            "${macos_dir}/strong_import" "${macos_dir}/weak_import")
        add_custom_target(alchemy_macos_fixtures DEPENDS ${macos_fixture_arguments})
        add_executable(alchemy_process_tests tests/alchemy_process_tests.cpp)
        target_link_libraries(alchemy_process_tests PRIVATE alchemy_core)
        add_dependencies(alchemy_process_tests alchemy_macos_fixtures)
        if(ALCHEMY_EXTERNAL_SMOKE_BINARY)
            if(NOT EXISTS "${ALCHEMY_EXTERNAL_SMOKE_BINARY}")
                message(FATAL_ERROR "ALCHEMY_EXTERNAL_SMOKE_BINARY does not exist")
            endif()
            list(APPEND macos_fixture_arguments "${ALCHEMY_EXTERNAL_SMOKE_BINARY}")
            add_test(NAME alchemy_external_cli COMMAND "${CMAKE_COMMAND}"
                "-DALCHEMY=$<TARGET_FILE:alchemy>" "-DPROGRAM=${ALCHEMY_EXTERNAL_SMOKE_BINARY}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/alchemy_cli_smoke.cmake")
            set_tests_properties(alchemy_external_cli PROPERTIES TIMEOUT 60)
        endif()
        add_test(NAME alchemy_process COMMAND alchemy_process_tests ${macos_fixture_arguments})
        add_test(NAME alchemy_cli COMMAND "${CMAKE_COMMAND}"
            "-DALCHEMY=$<TARGET_FILE:alchemy>" "-DPROGRAM=${macos_dir}/hello"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/alchemy_cli_smoke.cmake")
        set_tests_properties(alchemy_process alchemy_cli PROPERTIES TIMEOUT 60)
    else()
        message(STATUS "Alchemy compiler fixtures disabled: clang and ld64.lld are required")
    endif()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/AlchemyExpansion.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/AlchemyObjects.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/AlchemyProviderLookup.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/AlchemyPython.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/AlchemyContinuation.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/AlchemyGui.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/AlchemyDarwin.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/AlchemyHost.cmake")
