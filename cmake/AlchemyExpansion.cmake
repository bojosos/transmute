if(BUILD_TESTING AND ALCHEMY_CLANG AND ALCHEMY_LD64_LLD)
    foreach(name IN ITEMS services_app threaded threaded_tls_app threaded_tls_plugin loader_leaf loader_bridge loader_plugin loader_app)
        alchemy_object(${name} ${name}.c)
    endforeach()
    foreach(name IN ITEMS services_app threaded threaded_tls_app)
        add_custom_command(OUTPUT "${macos_dir}/${name}"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
                "${macos_dir}/${name}.o" "${macos_dir}/link_system.dylib" -o "${macos_dir}/${name}"
            DEPENDS "${macos_dir}/${name}.o" "${macos_dir}/link_system.dylib" VERBATIM)
    endforeach()
    add_custom_command(OUTPUT "${macos_dir}/threaded_tls_plugin.bundle"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -bundle -undefined dynamic_lookup
            "${macos_dir}/threaded_tls_plugin.o" -o "${macos_dir}/threaded_tls_plugin.bundle"
        DEPENDS "${macos_dir}/threaded_tls_plugin.o" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/loader_leaf.dylib"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib
            -install_name @rpath/loader_leaf.dylib "${macos_dir}/loader_leaf.o"
            "${macos_dir}/link_system.dylib"
            -o "${macos_dir}/loader_leaf.dylib"
        DEPENDS "${macos_dir}/loader_leaf.o" "${macos_dir}/link_system.dylib" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/loader_bridge.dylib"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib
            -install_name @rpath/loader_bridge.dylib -rpath @loader_path -reexport_library "${macos_dir}/loader_leaf.dylib"
            "${macos_dir}/loader_bridge.o" -o "${macos_dir}/loader_bridge.dylib"
        DEPENDS "${macos_dir}/loader_bridge.o" "${macos_dir}/loader_leaf.dylib" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/loader_plugin.bundle"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -bundle -undefined dynamic_lookup "-L${macos_dir}"
            -rpath @loader_path "${macos_dir}/loader_plugin.o" "${macos_dir}/loader_bridge.dylib"
            -o "${macos_dir}/loader_plugin.bundle"
        DEPENDS "${macos_dir}/loader_plugin.o" "${macos_dir}/loader_bridge.dylib" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/loader_app"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main -rpath @loader_path "-L${macos_dir}"
            "${macos_dir}/loader_app.o" "${macos_dir}/loader_bridge.dylib" "${macos_dir}/link_system.dylib"
            -o "${macos_dir}/loader_app"
        DEPENDS "${macos_dir}/loader_app.o" "${macos_dir}/loader_bridge.dylib" "${macos_dir}/link_system.dylib" VERBATIM)
    add_custom_target(alchemy_expanded_fixtures DEPENDS
        "${macos_dir}/services_app" "${macos_dir}/threaded" "${macos_dir}/threaded_tls_app"
        "${macos_dir}/threaded_tls_plugin.bundle" "${macos_dir}/loader_app" "${macos_dir}/loader_plugin.bundle")
    add_dependencies(alchemy_expanded_fixtures alchemy_macos_fixtures)
    add_dependencies(alchemy_services_tests alchemy_expanded_fixtures)
    add_executable(alchemy_loader_expansion_tests tests/alchemy_loader_expansion_tests.cpp)
    target_link_libraries(alchemy_loader_expansion_tests PRIVATE alchemy_core)
    add_dependencies(alchemy_loader_expansion_tests alchemy_expanded_fixtures)
    add_test(NAME alchemy_loader_expansion COMMAND alchemy_loader_expansion_tests
        "${macos_dir}/loader_app" "${macos_dir}/loader_plugin.bundle")
    add_test(NAME alchemy_services_process COMMAND alchemy_services_tests "${macos_dir}/services_app")
    add_test(NAME alchemy_threaded_cli COMMAND alchemy --jit-stats "${macos_dir}/threaded")
    set_tests_properties(alchemy_threaded_cli PROPERTIES PASS_REGULAR_EXPRESSION "Darwin threads: 27 42" TIMEOUT 60)
    add_executable(alchemy_thread_process_tests tests/alchemy_thread_process_tests.cpp)
    target_link_libraries(alchemy_thread_process_tests PRIVATE alchemy_core)
    add_dependencies(alchemy_thread_process_tests alchemy_expanded_fixtures)
    add_test(NAME alchemy_thread_process COMMAND alchemy_thread_process_tests
        "${macos_dir}/threaded" "${macos_dir}/threaded_tls_app" "${macos_dir}/threaded_tls_plugin.bundle")
    set_tests_properties(alchemy_thread_process PROPERTIES TIMEOUT 60)
    add_executable(alchemy_bundle_tests tests/alchemy_bundle_tests.cpp)
    target_link_libraries(alchemy_bundle_tests PRIVATE alchemy_core)
    add_dependencies(alchemy_bundle_tests alchemy_macos_fixtures)
    add_test(NAME alchemy_bundle COMMAND alchemy_bundle_tests "${macos_dir}/hello" "${CMAKE_CURRENT_BINARY_DIR}/bundle-fixture")
    add_test(NAME alchemy_doctor_json COMMAND "${CMAKE_COMMAND}"
        "-DALCHEMY=$<TARGET_FILE:alchemy>" "-DPROGRAM=${macos_dir}/hello"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/alchemy_doctor_smoke.cmake")
    set_tests_properties(alchemy_services_process alchemy_bundle alchemy_doctor_json alchemy_loader_expansion PROPERTIES TIMEOUT 60)
endif()

set(ALCHEMY_LLVM_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/artifacts/alchemy-llvm-arm64" CACHE PATH
    "Independently built ARM64 LLVM runtime directory")
if(WIN32)
    find_program(ALCHEMY_POWERSHELL NAMES pwsh powershell)
    if(ALCHEMY_POWERSHELL)
        add_custom_target(alchemy_llvm_runtimes
            COMMAND "${ALCHEMY_POWERSHELL}" -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/build_alchemy_llvm.ps1"
                -Fetch -Runtime all -OutputDirectory "${ALCHEMY_LLVM_RUNTIME_DIR}" VERBATIM)
    endif()
endif()
if(BUILD_TESTING AND ALCHEMY_CLANG AND ALCHEMY_LD64_LLD
   AND EXISTS "${ALCHEMY_LLVM_RUNTIME_DIR}/libc++abi.dylib"
   AND EXISTS "${ALCHEMY_LLVM_RUNTIME_DIR}/libunwind.dylib")
    foreach(name IN ITEMS cpp_exception_helper cpp_exception_app)
        add_custom_command(OUTPUT "${macos_dir}/${name}.o"
            COMMAND "${ALCHEMY_CLANG}" --target=arm64-apple-macos11 -march=armv8-a -mcpu=generic -nostdinc
                -fno-stack-protector -O1 -fexceptions -frtti -std=c++17 -c
                "${CMAKE_SOURCE_DIR}/tests/fixtures/macos/${name}.cpp" -o "${macos_dir}/${name}.o"
            DEPENDS "tests/fixtures/macos/${name}.cpp" tests/fixtures/macos/cpp_exception_shared.hpp VERBATIM)
    endforeach()
    add_custom_command(OUTPUT "${macos_dir}/cpp_exception_helper.dylib"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib -rpath @loader_path "-L${ALCHEMY_LLVM_RUNTIME_DIR}"
            -install_name @loader_path/cpp_exception_helper.dylib "${macos_dir}/cpp_exception_helper.o"
            "${ALCHEMY_LLVM_RUNTIME_DIR}/libc++abi.dylib" "${ALCHEMY_LLVM_RUNTIME_DIR}/libunwind.dylib" "${macos_dir}/link_system.dylib"
            -o "${macos_dir}/cpp_exception_helper.dylib"
        DEPENDS "${macos_dir}/cpp_exception_helper.o" "${macos_dir}/link_system.dylib"
            "${ALCHEMY_LLVM_RUNTIME_DIR}/libc++abi.dylib" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/cpp_exception_app"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main -rpath @loader_path "-L${ALCHEMY_LLVM_RUNTIME_DIR}"
            "${macos_dir}/cpp_exception_app.o" "${macos_dir}/cpp_exception_helper.dylib"
            "${ALCHEMY_LLVM_RUNTIME_DIR}/libc++abi.dylib" "${ALCHEMY_LLVM_RUNTIME_DIR}/libunwind.dylib" "${macos_dir}/link_system.dylib"
            -o "${macos_dir}/cpp_exception_app"
        DEPENDS "${macos_dir}/cpp_exception_app.o" "${macos_dir}/cpp_exception_helper.dylib" VERBATIM)
    add_custom_target(alchemy_cpp_fixtures DEPENDS "${macos_dir}/cpp_exception_app")
    add_dependencies(alchemy_cpp_fixtures alchemy_expanded_fixtures)
    add_executable(alchemy_cpp_tests tests/alchemy_cpp_tests.cpp)
    target_link_libraries(alchemy_cpp_tests PRIVATE alchemy_core)
    add_dependencies(alchemy_cpp_tests alchemy_cpp_fixtures)
    add_test(NAME alchemy_cpp_exceptions COMMAND alchemy_cpp_tests
        "${macos_dir}/cpp_exception_app" "${ALCHEMY_LLVM_RUNTIME_DIR}")
    set_tests_properties(alchemy_cpp_exceptions PROPERTIES TIMEOUT 120)
    if(EXISTS "${ALCHEMY_LLVM_RUNTIME_DIR}/libc++.1.dylib")
        execute_process(COMMAND "${ALCHEMY_CLANG}" -print-resource-dir
            OUTPUT_VARIABLE alchemy_resource_dir OUTPUT_STRIP_TRAILING_WHITESPACE)
        set(alchemy_llvm_source "${CMAKE_SOURCE_DIR}/artifacts/alchemy-llvm-source")
        foreach(name IN ITEMS cpp_runtime_helper cpp_runtime_app)
            add_custom_command(OUTPUT "${macos_dir}/${name}.o"
                COMMAND "${ALCHEMY_CLANG}" --target=arm64-apple-macos11 -march=armv8-a -mcpu=generic -nostdinc -fno-stack-protector
                    -isystem "${alchemy_llvm_source}/libcxx/include" -isystem "${CMAKE_SOURCE_DIR}/runtime/include"
                    -isystem "${alchemy_resource_dir}/include" -O1 -fexceptions -frtti -std=c++17 -c
                    "${CMAKE_SOURCE_DIR}/tests/fixtures/macos/${name}.cpp" -o "${macos_dir}/${name}.o"
                DEPENDS "tests/fixtures/macos/${name}.cpp" tests/fixtures/macos/cpp_runtime_abi.hpp VERBATIM)
        endforeach()
        add_custom_command(OUTPUT "${macos_dir}/cpp_runtime_helper.dylib"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib -rpath @loader_path -undefined dynamic_lookup "-L${ALCHEMY_LLVM_RUNTIME_DIR}"
                -install_name @loader_path/cpp_runtime_helper.dylib "${macos_dir}/cpp_runtime_helper.o"
                "${ALCHEMY_LLVM_RUNTIME_DIR}/libc++.1.dylib" "${ALCHEMY_LLVM_RUNTIME_DIR}/libc++abi.dylib"
                "${ALCHEMY_LLVM_RUNTIME_DIR}/libunwind.dylib" "${macos_dir}/link_system.dylib"
                -o "${macos_dir}/cpp_runtime_helper.dylib"
            DEPENDS "${macos_dir}/cpp_runtime_helper.o" "${ALCHEMY_LLVM_RUNTIME_DIR}/libc++.1.dylib"
                "${macos_dir}/link_system.dylib" VERBATIM)
        add_custom_command(OUTPUT "${macos_dir}/cpp_runtime_app"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main -rpath @loader_path -undefined dynamic_lookup "-L${ALCHEMY_LLVM_RUNTIME_DIR}"
                "${macos_dir}/cpp_runtime_app.o" "${macos_dir}/cpp_runtime_helper.dylib"
                "${ALCHEMY_LLVM_RUNTIME_DIR}/libc++.1.dylib" "${ALCHEMY_LLVM_RUNTIME_DIR}/libc++abi.dylib"
                "${ALCHEMY_LLVM_RUNTIME_DIR}/libunwind.dylib" "${macos_dir}/link_system.dylib"
                -o "${macos_dir}/cpp_runtime_app"
            DEPENDS "${macos_dir}/cpp_runtime_app.o" "${macos_dir}/cpp_runtime_helper.dylib" VERBATIM)
        add_custom_target(alchemy_cpp_runtime_fixture DEPENDS "${macos_dir}/cpp_runtime_app")
        add_dependencies(alchemy_cpp_runtime_fixture alchemy_cpp_fixtures)
        add_dependencies(alchemy_cpp_tests alchemy_cpp_runtime_fixture)
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/cpp-output")
        add_test(NAME alchemy_cpp_runtime COMMAND alchemy_cpp_tests
            "${macos_dir}/cpp_exception_app" "${ALCHEMY_LLVM_RUNTIME_DIR}"
            "${macos_dir}/cpp_runtime_app" "${CMAKE_CURRENT_BINARY_DIR}/cpp-output")
        set_tests_properties(alchemy_cpp_runtime PROPERTIES TIMEOUT 120)
    endif()
endif()
