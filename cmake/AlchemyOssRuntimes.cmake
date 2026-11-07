# Vendored open-source /usr/lib runtime libraries (third_party/<name>, docs/ALCHEMY_OSS_RUNTIMES.md).
# Each is built by scripts/build_alchemy_<name>.ps1 into ALCHEMY_<NAME>_RUNTIME_DIR
# (artifacts/alchemy-<name>-arm64, target alchemy_<name>_runtime) and, once built, tested by
# alchemy_oss_runtime_tests: allow-list, doctor, provenance refusals, and the guest fixture
# tests/fixtures/macos/<name>_app.c in alchemy-fx processes against <name>_app.expected.
if(BUILD_TESTING AND ALCHEMY_CLANG AND ALCHEMY_LD64_LLD AND macos_dir AND TARGET alchemy-fx)
    add_executable(alchemy_oss_runtime_tests tests/alchemy_oss_runtime_tests.cpp)
    target_link_libraries(alchemy_oss_runtime_tests PRIVATE alchemy_core)
    add_dependencies(alchemy_oss_runtime_tests alchemy-fx)
endif()

# alchemy_oss_runtime(<name> INSTALL_NAME <path> UPSTREAM <url> [EXTRA_RUNTIMES <dir>...])
function(alchemy_oss_runtime name)
    cmake_parse_arguments(arg "" "INSTALL_NAME;UPSTREAM" "EXTRA_RUNTIMES" ${ARGN})
    string(TOUPPER "${name}" upper)
    set(ALCHEMY_${upper}_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/artifacts/alchemy-${name}-arm64" CACHE PATH
        "Independently built ARM64 ${name} runtime directory (scripts/build_alchemy_${name}.ps1)")
    set(dir "${ALCHEMY_${upper}_RUNTIME_DIR}")
    if(WIN32 AND ALCHEMY_POWERSHELL)
        add_custom_target(alchemy_${name}_runtime
            COMMAND "${ALCHEMY_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/scripts/build_alchemy_${name}.ps1" -OutputDirectory "${dir}" VERBATIM)
    endif()
    get_filename_component(file "${arg_INSTALL_NAME}" NAME)
    if(NOT TARGET alchemy_oss_runtime_tests OR NOT EXISTS "${dir}/${file}" OR NOT EXISTS "${dir}/provenance.json")
        return()
    endif()
    alchemy_object(${name}_app ${name}_app.c)
    add_custom_command(OUTPUT "${macos_dir}/${name}_app"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main "${macos_dir}/${name}_app.o"
            "${dir}/${file}" "${macos_dir}/link_system.dylib" -o "${macos_dir}/${name}_app"
        DEPENDS "${macos_dir}/${name}_app.o" "${macos_dir}/link_system.dylib" "${dir}/${file}" VERBATIM)
    add_custom_target(alchemy_${name}_fixtures DEPENDS "${macos_dir}/${name}_app")
    add_dependencies(alchemy_${name}_fixtures alchemy_macos_fixtures)
    add_dependencies(alchemy_oss_runtime_tests alchemy_${name}_fixtures)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${name}-scratch")
    add_test(NAME alchemy_${name}_runtime
        COMMAND alchemy_oss_runtime_tests "${arg_INSTALL_NAME}" "${arg_UPSTREAM}" "${macos_dir}/${name}_app"
            "${CMAKE_SOURCE_DIR}/tests/fixtures/macos/${name}_app.expected" "${CMAKE_CURRENT_BINARY_DIR}/${name}-scratch"
            "$<TARGET_FILE:alchemy-fx>" "${dir}" ${arg_EXTRA_RUNTIMES})
    set_tests_properties(alchemy_${name}_runtime PROPERTIES TIMEOUT 300
        ENVIRONMENT "ALCHEMY_GUI_NO_ACTIVATE=1;ALCHEMY_GUI_CLOAK=1;ALCHEMY_GUI_NO_CRASH_DIALOG=1;ALCHEMY_NETWORK=loopback")
endfunction()

alchemy_oss_runtime(libffi INSTALL_NAME /usr/lib/libffi.dylib UPSTREAM https://github.com/libffi/libffi)
alchemy_oss_runtime(sqlite INSTALL_NAME /usr/lib/libsqlite3.dylib UPSTREAM https://www.sqlite.org)
alchemy_oss_runtime(icu INSTALL_NAME /usr/lib/libicucore.A.dylib UPSTREAM https://github.com/unicode-org/icu
    EXTRA_RUNTIMES "${ALCHEMY_LLVM_RUNTIME_DIR}")
