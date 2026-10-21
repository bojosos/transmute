# Guest libexpat runtime (/usr/lib/libexpat.1.dylib) built from pinned upstream
# expat by scripts/build_alchemy_expat.ps1, and its loader/provenance tests.
set(ALCHEMY_EXPAT_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/artifacts/alchemy-expat-arm64" CACHE PATH
    "Independently built ARM64 libexpat runtime directory (scripts/build_alchemy_expat.ps1)")
if(WIN32 AND ALCHEMY_POWERSHELL)
    add_custom_target(alchemy_expat_runtime
        COMMAND "${ALCHEMY_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/scripts/build_alchemy_expat.ps1"
            -Fetch -OutputDirectory "${ALCHEMY_EXPAT_RUNTIME_DIR}" VERBATIM)
endif()

if(BUILD_TESTING AND ALCHEMY_CLANG AND ALCHEMY_LD64_LLD AND macos_dir
   AND EXISTS "${ALCHEMY_EXPAT_RUNTIME_DIR}/libexpat.1.dylib"
   AND EXISTS "${ALCHEMY_EXPAT_RUNTIME_DIR}/provenance.json")
    alchemy_object(expat_app expat_app.c)
    add_custom_command(OUTPUT "${macos_dir}/expat_app"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main "${macos_dir}/expat_app.o"
            "${ALCHEMY_EXPAT_RUNTIME_DIR}/libexpat.1.dylib" "${macos_dir}/link_system.dylib"
            -o "${macos_dir}/expat_app"
        DEPENDS "${macos_dir}/expat_app.o" "${macos_dir}/link_system.dylib"
            "${ALCHEMY_EXPAT_RUNTIME_DIR}/libexpat.1.dylib" VERBATIM)
    add_custom_target(alchemy_expat_fixtures DEPENDS "${macos_dir}/expat_app")
    add_dependencies(alchemy_expat_fixtures alchemy_macos_fixtures)
    add_executable(alchemy_expat_tests tests/alchemy_expat_tests.cpp)
    target_link_libraries(alchemy_expat_tests PRIVATE alchemy_core)
    add_dependencies(alchemy_expat_tests alchemy_expat_fixtures)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/expat-scratch")
    add_test(NAME alchemy_expat COMMAND alchemy_expat_tests "${macos_dir}/expat_app"
        "${ALCHEMY_EXPAT_RUNTIME_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/expat-scratch")
    set_tests_properties(alchemy_expat PROPERTIES TIMEOUT 300)
    set(alchemy_expat_fixture_ready ON)
endif()

# alchemy-fx processes, at the default and a rebased load address.
if(alchemy_expat_fixture_ready AND TARGET alchemy-fx)
    add_dependencies(alchemy_expat_tests alchemy-fx)
    add_test(NAME alchemy_expat_fx COMMAND alchemy_expat_tests "${macos_dir}/expat_app"
        "${ALCHEMY_EXPAT_RUNTIME_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/expat-scratch" "$<TARGET_FILE:alchemy-fx>")
    set_tests_properties(alchemy_expat_fx PROPERTIES TIMEOUT 300 ENVIRONMENT "ALCHEMY_GUI_NO_ACTIVATE=1")
endif()