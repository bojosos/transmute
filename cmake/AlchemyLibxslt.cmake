# Guest libxslt and libexslt runtimes (/usr/lib/libxslt.1.dylib, /usr/lib/libexslt.0.dylib)
# built from the vendored third_party/libxslt by scripts/build_alchemy_libxslt.ps1 on top
# of the libxml2 runtime, and their loader tests.
set(ALCHEMY_LIBXSLT_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/artifacts/alchemy-libxslt-arm64" CACHE PATH
    "Independently built ARM64 libxslt/libexslt runtime directory (scripts/build_alchemy_libxslt.ps1)")
if(WIN32 AND ALCHEMY_POWERSHELL)
    add_custom_target(alchemy_libxslt_runtime
        COMMAND "${ALCHEMY_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/scripts/build_alchemy_libxslt.ps1"
            -LibXml2Directory "${ALCHEMY_LIBXML2_RUNTIME_DIR}" -OutputDirectory "${ALCHEMY_LIBXSLT_RUNTIME_DIR}" VERBATIM)
endif()

if(BUILD_TESTING AND ALCHEMY_CLANG AND ALCHEMY_LD64_LLD AND macos_dir
   AND EXISTS "${ALCHEMY_LIBXML2_RUNTIME_DIR}/provenance.json"
   AND EXISTS "${ALCHEMY_LIBXSLT_RUNTIME_DIR}/libxslt.1.dylib"
   AND EXISTS "${ALCHEMY_LIBXSLT_RUNTIME_DIR}/provenance.json")
    add_custom_command(OUTPUT "${macos_dir}/libxslt_app.o"
        COMMAND "${ALCHEMY_CLANG}" --target=arm64-apple-macos11 -nostdlibinc -fno-stack-protector -O1
            -isystem "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include" -I "${ALCHEMY_LIBXSLT_RUNTIME_DIR}/include"
            -I "${ALCHEMY_LIBXML2_RUNTIME_DIR}/include" -c
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/macos/libxslt_app.c" -o "${macos_dir}/libxslt_app.o"
        DEPENDS "tests/fixtures/macos/libxslt_app.c" "${ALCHEMY_LIBXSLT_RUNTIME_DIR}/libxslt.1.dylib" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/libxslt_app"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main "${macos_dir}/libxslt_app.o"
            "${ALCHEMY_LIBXSLT_RUNTIME_DIR}/libexslt.0.dylib" "${ALCHEMY_LIBXSLT_RUNTIME_DIR}/libxslt.1.dylib"
            "${ALCHEMY_LIBXML2_RUNTIME_DIR}/libxml2.2.dylib" "${macos_dir}/link_system.dylib" -o "${macos_dir}/libxslt_app"
        DEPENDS "${macos_dir}/libxslt_app.o" "${macos_dir}/link_system.dylib"
            "${ALCHEMY_LIBXSLT_RUNTIME_DIR}/libxslt.1.dylib" VERBATIM)
    add_custom_target(alchemy_libxslt_fixtures DEPENDS "${macos_dir}/libxslt_app")
    add_dependencies(alchemy_libxslt_fixtures alchemy_macos_fixtures)
    add_executable(alchemy_libxslt_tests tests/alchemy_libxslt_tests.cpp)
    target_link_libraries(alchemy_libxslt_tests PRIVATE alchemy_core)
    add_dependencies(alchemy_libxslt_tests alchemy_libxslt_fixtures)
    add_test(NAME alchemy_libxslt COMMAND alchemy_libxslt_tests "${macos_dir}/libxslt_app"
        "${ALCHEMY_LIBXSLT_RUNTIME_DIR}" "${ALCHEMY_LIBXML2_RUNTIME_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/libxslt-scratch")
    set_tests_properties(alchemy_libxslt PROPERTIES TIMEOUT 300)
    if(TARGET alchemy-fx)
        add_dependencies(alchemy_libxslt_tests alchemy-fx)
        add_test(NAME alchemy_libxslt_fx COMMAND alchemy_libxslt_tests "${macos_dir}/libxslt_app"
            "${ALCHEMY_LIBXSLT_RUNTIME_DIR}" "${ALCHEMY_LIBXML2_RUNTIME_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/libxslt-scratch"
            "$<TARGET_FILE:alchemy-fx>")
        set_tests_properties(alchemy_libxslt_fx PROPERTIES TIMEOUT 300 ENVIRONMENT "ALCHEMY_GUI_NO_ACTIVATE=1")
    endif()
endif()
