# Owner: translator core. Define this component's targets with fx_add_library / fx_add_test.
fx_add_library(fx_translator
    ${CMAKE_CURRENT_LIST_DIR}/core_decode.cpp
    ${CMAKE_CURRENT_LIST_DIR}/region_plan.cpp
    ${CMAKE_CURRENT_LIST_DIR}/region_emitter.cpp
    ${CMAKE_CURRENT_LIST_DIR}/region_alloc.cpp
    ${CMAKE_CURRENT_LIST_DIR}/emit_integer.cpp
    ${CMAKE_CURRENT_LIST_DIR}/emit_load_store.cpp
    ${CMAKE_CURRENT_LIST_DIR}/emit_branch.cpp
    ${CMAKE_CURRENT_LIST_DIR}/core_translator.cpp
)
target_link_libraries(fx_translator PUBLIC fx_common)

fx_add_test(fx_translator_tests
    SOURCES
        ${CMAKE_CURRENT_LIST_DIR}/core_test_machine.cpp
        ${CMAKE_CURRENT_LIST_DIR}/core_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/core_fuzz_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/core_family_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/core_perf_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/core_isel_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/core_inline_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/core_table_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/core_convention_tests.cpp
    LIBS fx_translator
)

# Translator core + the real families (deferred: the family targets are defined
# by fp.cmake / neon.cmake / system.cmake after this file).
set(FX_TRANSLATOR_CORE_DIR ${CMAKE_CURRENT_LIST_DIR})
function(fx_translator_core_family_tests)
    if(TARGET fx_emit_fp AND TARGET fx_emit_neon AND TARGET fx_emit_system)
        fx_add_test(fx_translator_family_tests
            SOURCES
                ${FX_TRANSLATOR_CORE_DIR}/core_test_machine.cpp
                ${FX_TRANSLATOR_CORE_DIR}/core_integration_tests.cpp
                ${FX_TRANSLATOR_CORE_DIR}/core_tier2_tests.cpp
            LIBS fx_translator fx_emit_fp fx_emit_neon fx_emit_system
        )
    endif()
endfunction()
cmake_language(DEFER CALL fx_translator_core_family_tests)

# Diagnostic: relocation audit and translation timing over a real image
# (fx_translator_image_audit <image> [dump-dir]); not a CTest test.
function(fx_translator_core_image_audit)
    if(TARGET fx_cache AND TARGET fx_emit_fp AND TARGET fx_emit_neon AND TARGET fx_emit_system)
        add_executable(fx_translator_image_audit ${FX_TRANSLATOR_CORE_DIR}/core_image_audit.cpp)
        fx_component_options(fx_translator_image_audit)
        target_link_libraries(fx_translator_image_audit PRIVATE fx_translator fx_cache fx_emit_fp fx_emit_neon fx_emit_system)
    endif()
endfunction()
cmake_language(DEFER CALL fx_translator_core_image_audit)
