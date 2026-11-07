# Guest libxml2 runtime (/usr/lib/libxml2.2.dylib) built from the vendored
# third_party/libxml2 by scripts/build_alchemy_libxml2.ps1, and its loader tests.
set(ALCHEMY_LIBXML2_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/artifacts/alchemy-libxml2-arm64" CACHE PATH
    "Independently built ARM64 libxml2 runtime directory (scripts/build_alchemy_libxml2.ps1)")
if(WIN32 AND ALCHEMY_POWERSHELL)
    add_custom_target(alchemy_libxml2_runtime
        COMMAND "${ALCHEMY_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/scripts/build_alchemy_libxml2.ps1"
            -OutputDirectory "${ALCHEMY_LIBXML2_RUNTIME_DIR}" VERBATIM)
endif()

if(BUILD_TESTING AND ALCHEMY_CLANG AND ALCHEMY_LD64_LLD AND macos_dir
   AND EXISTS "${ALCHEMY_LIBXML2_RUNTIME_DIR}/libxml2.2.dylib"
   AND EXISTS "${ALCHEMY_LIBXML2_RUNTIME_DIR}/provenance.json")
    # Compiled against libxml2's public headers as the runtime build installed them
    # (with its generated xmlversion.h) and runtime/include; no SDK.
    add_custom_command(OUTPUT "${macos_dir}/libxml2_app.o"
        COMMAND "${ALCHEMY_CLANG}" --target=arm64-apple-macos11 -nostdlibinc -fno-stack-protector -O1
            -isystem "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include" -I "${ALCHEMY_LIBXML2_RUNTIME_DIR}/include" -c
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/macos/libxml2_app.c" -o "${macos_dir}/libxml2_app.o"
        DEPENDS "tests/fixtures/macos/libxml2_app.c" "${ALCHEMY_LIBXML2_RUNTIME_DIR}/libxml2.2.dylib" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/libxml2_app"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main "${macos_dir}/libxml2_app.o"
            "${ALCHEMY_LIBXML2_RUNTIME_DIR}/libxml2.2.dylib" "${macos_dir}/link_system.dylib"
            -o "${macos_dir}/libxml2_app"
        DEPENDS "${macos_dir}/libxml2_app.o" "${macos_dir}/link_system.dylib"
            "${ALCHEMY_LIBXML2_RUNTIME_DIR}/libxml2.2.dylib" VERBATIM)
    add_custom_target(alchemy_libxml2_fixtures DEPENDS "${macos_dir}/libxml2_app")
    add_dependencies(alchemy_libxml2_fixtures alchemy_macos_fixtures)
    add_executable(alchemy_libxml2_tests tests/alchemy_libxml2_tests.cpp)
    target_link_libraries(alchemy_libxml2_tests PRIVATE alchemy_core)
    add_dependencies(alchemy_libxml2_tests alchemy_libxml2_fixtures)
    add_test(NAME alchemy_libxml2 COMMAND alchemy_libxml2_tests "${macos_dir}/libxml2_app"
        "${ALCHEMY_LIBXML2_RUNTIME_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/libxml2-scratch")
    set_tests_properties(alchemy_libxml2 PROPERTIES TIMEOUT 300)
    if(TARGET alchemy-fx)
        add_dependencies(alchemy_libxml2_tests alchemy-fx)
        add_test(NAME alchemy_libxml2_fx COMMAND alchemy_libxml2_tests "${macos_dir}/libxml2_app"
            "${ALCHEMY_LIBXML2_RUNTIME_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/libxml2-scratch" "$<TARGET_FILE:alchemy-fx>")
        set_tests_properties(alchemy_libxml2_fx PROPERTIES TIMEOUT 300 ENVIRONMENT "ALCHEMY_GUI_NO_ACTIVATE=1")
    endif()
endif()
