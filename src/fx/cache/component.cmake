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
