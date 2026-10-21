# Owner: cache/aot. Define this component's targets with fx_add_library / fx_add_test.

fx_add_library(fx_cache
    ${CMAKE_CURRENT_LIST_DIR}/format.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pe_view.cpp
    ${CMAKE_CURRENT_LIST_DIR}/cache.cpp
    ${CMAKE_CURRENT_LIST_DIR}/merge.cpp
    ${CMAKE_CURRENT_LIST_DIR}/aot.cpp
    ${CMAKE_CURRENT_LIST_DIR}/background.cpp
    ${CMAKE_CURRENT_LIST_DIR}/external_aot.cpp
    # References translate_region; only linked into programs that call aot_translate_image.
    ${CMAKE_CURRENT_LIST_DIR}/aot_default.cpp
)

# Translator build fingerprint (cmake/FxTranslatorFingerprint.cmake): a hash of
# the sources that decide the emitted code, folded into every cache file key,
# so a build with different code generation never reuses translations made by
# another (kTranslatorAbiVersion stays for deliberate format and convention
# changes). A generated source of fx_cache, so Ninja tracks it directly.
file(GLOB fx_translator_fingerprint_sources CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/src/fx/translator/*.cpp
    ${PROJECT_SOURCE_DIR}/src/fx/translator/*.hpp
    ${PROJECT_SOURCE_DIR}/src/fx/translator/*.inc)
list(FILTER fx_translator_fingerprint_sources EXCLUDE REGEX "(_tests?[_.]|test_|tests_)[^/]*$")
list(APPEND fx_translator_fingerprint_sources
    ${PROJECT_SOURCE_DIR}/src/fx/common/x64_assembler.hpp
    ${PROJECT_SOURCE_DIR}/include/transmute/fx/context.hpp
    ${PROJECT_SOURCE_DIR}/src/fx/cache/abi_version.hpp
    ${PROJECT_SOURCE_DIR}/src/fx/cache/format.hpp
    ${PROJECT_SOURCE_DIR}/src/fx/cache/format.cpp)
list(SORT fx_translator_fingerprint_sources)
string(REPLACE ";" "\n" fx_translator_fingerprint_lines "${fx_translator_fingerprint_sources}")
# Rewritten only when the list changes (a source added or removed).
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fx_translator_fingerprint.list
    CONTENT "${fx_translator_fingerprint_lines}\n" @ONLY)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fx_translator_fingerprint.cpp
    COMMAND ${CMAKE_COMMAND} -DLIST=${CMAKE_CURRENT_BINARY_DIR}/fx_translator_fingerprint.list
            -DROOT=${PROJECT_SOURCE_DIR} -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/fx_translator_fingerprint.cpp
            -P ${PROJECT_SOURCE_DIR}/cmake/FxTranslatorFingerprint.cmake
    DEPENDS ${fx_translator_fingerprint_sources} ${CMAKE_CURRENT_BINARY_DIR}/fx_translator_fingerprint.list
            ${PROJECT_SOURCE_DIR}/cmake/FxTranslatorFingerprint.cmake
    COMMENT "fx translator build fingerprint"
    VERBATIM)
target_sources(fx_cache PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/fx_translator_fingerprint.cpp)
target_link_libraries(fx_cache PUBLIC fx_common)

set(FX_CACHE_BLENDER_DIR "${PROJECT_SOURCE_DIR}/artifacts/arm64-stage/Blender Foundation/Blender 4.5")

# Stub translator only: never depends on the state of the translator component.
fx_add_test(fx_cache_tests
    SOURCES
        ${CMAKE_CURRENT_LIST_DIR}/cache_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/aot_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/external_aot_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_support.cpp
    LIBS fx_cache
)
target_compile_definitions(fx_cache_tests PRIVATE FX_CACHE_BLENDER_DIR=L"${FX_CACHE_BLENDER_DIR}")

# AOT and cache round trip with the real translator, once it exists.
if(TARGET fx_translator)
    set(fx_cache_real_libs fx_cache fx_translator)
    set(fx_cache_real_defs FX_CACHE_BLENDER_DIR=L"${FX_CACHE_BLENDER_DIR}")
    foreach(family fp neon system)
        if(TARGET fx_emit_${family})
            list(APPEND fx_cache_real_libs fx_emit_${family})
            string(TOUPPER ${family} family_upper)
            list(APPEND fx_cache_real_defs FX_CACHE_HAVE_${family_upper}=1)
        endif()
    endforeach()
    fx_add_test(fx_cache_aot_tests
        SOURCES
            ${CMAKE_CURRENT_LIST_DIR}/aot_real_tests.cpp
            ${CMAKE_CURRENT_LIST_DIR}/test_support.cpp
        LIBS ${fx_cache_real_libs}
    )
    target_compile_definitions(fx_cache_aot_tests PRIVATE ${fx_cache_real_defs})
endif()
