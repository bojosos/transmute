# Guest libcurl runtime (/usr/lib/libcurl.4.dylib) built from the vendored
# third_party/curl by scripts/build_alchemy_libcurl.ps1, and its loader tests.
set(ALCHEMY_LIBCURL_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/artifacts/alchemy-libcurl-arm64" CACHE PATH
    "Independently built ARM64 libcurl runtime directory (scripts/build_alchemy_libcurl.ps1)")
if(WIN32 AND ALCHEMY_POWERSHELL)
    add_custom_target(alchemy_libcurl_runtime
        COMMAND "${ALCHEMY_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/scripts/build_alchemy_libcurl.ps1"
            -OutputDirectory "${ALCHEMY_LIBCURL_RUNTIME_DIR}" VERBATIM)
endif()

if(BUILD_TESTING AND ALCHEMY_CLANG AND ALCHEMY_LD64_LLD AND macos_dir
   AND EXISTS "${ALCHEMY_LIBCURL_RUNTIME_DIR}/libcurl.4.dylib"
   AND EXISTS "${ALCHEMY_LIBCURL_RUNTIME_DIR}/provenance.json")
    # Compiled against curl's public headers as the runtime build installed them
    # and runtime/include; no SDK. libcurl_app_system.tbd lists the libSystem
    # calls the fixture makes (its loopback server), for two-level binding.
    add_custom_command(OUTPUT "${macos_dir}/libcurl_app.o"
        COMMAND "${ALCHEMY_CLANG}" --target=arm64-apple-macos11 -nostdlibinc -fno-stack-protector -O1
            -isystem "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include" -I "${ALCHEMY_LIBCURL_RUNTIME_DIR}/include" -c
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/macos/libcurl_app.c" -o "${macos_dir}/libcurl_app.o"
        DEPENDS "tests/fixtures/macos/libcurl_app.c" "${ALCHEMY_LIBCURL_RUNTIME_DIR}/libcurl.4.dylib" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/libcurl_app"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main "${macos_dir}/libcurl_app.o"
            "${ALCHEMY_LIBCURL_RUNTIME_DIR}/libcurl.4.dylib"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/macos/libcurl_app_system.tbd" -o "${macos_dir}/libcurl_app"
        DEPENDS "${macos_dir}/libcurl_app.o" "tests/fixtures/macos/libcurl_app_system.tbd"
            "${ALCHEMY_LIBCURL_RUNTIME_DIR}/libcurl.4.dylib" VERBATIM)
    add_custom_target(alchemy_libcurl_fixtures DEPENDS "${macos_dir}/libcurl_app")
    add_dependencies(alchemy_libcurl_fixtures alchemy_macos_fixtures)
    add_executable(alchemy_libcurl_tests tests/alchemy_libcurl_tests.cpp)
    target_link_libraries(alchemy_libcurl_tests PRIVATE alchemy_core)
    add_dependencies(alchemy_libcurl_tests alchemy_libcurl_fixtures)
    add_test(NAME alchemy_libcurl COMMAND alchemy_libcurl_tests "${macos_dir}/libcurl_app"
        "${ALCHEMY_LIBCURL_RUNTIME_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/libcurl-scratch")
    set_tests_properties(alchemy_libcurl PROPERTIES TIMEOUT 300)
    if(TARGET alchemy-fx)
        add_dependencies(alchemy_libcurl_tests alchemy-fx)
        add_test(NAME alchemy_libcurl_fx COMMAND alchemy_libcurl_tests "${macos_dir}/libcurl_app"
            "${ALCHEMY_LIBCURL_RUNTIME_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/libcurl-scratch" "$<TARGET_FILE:alchemy-fx>")
        set_tests_properties(alchemy_libcurl_fx PROPERTIES TIMEOUT 300 ENVIRONMENT "ALCHEMY_GUI_NO_ACTIVATE=1")
    endif()
endif()
