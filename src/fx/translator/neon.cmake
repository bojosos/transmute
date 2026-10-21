# Owner: neon family. Integer SIMD, permutes and all SIMD&FP loads/stores.
fx_add_library(fx_emit_neon
    ${CMAKE_CURRENT_LIST_DIR}/emit_neon_common.cpp
    ${CMAKE_CURRENT_LIST_DIR}/emit_neon_arith.cpp
    ${CMAKE_CURRENT_LIST_DIR}/emit_neon_shift.cpp
    ${CMAKE_CURRENT_LIST_DIR}/emit_neon_perm.cpp
    ${CMAKE_CURRENT_LIST_DIR}/emit_neon_dispatch.cpp
    ${CMAKE_CURRENT_LIST_DIR}/emit_neon_fusion.cpp
    ${CMAKE_CURRENT_LIST_DIR}/emit_vector_memory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/emit_neon_register.cpp
)
target_link_libraries(fx_emit_neon PUBLIC fx_common)

fx_add_test(fx_emit_neon_tests
    SOURCES
        ${CMAKE_CURRENT_LIST_DIR}/emit_neon_test_util.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_neon_test_reference.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_neon_tests_arith.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_neon_tests_shift.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_neon_tests_perm.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_vector_memory_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_neon_tests_misc.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_neon_tests_lowering.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_neon_tests_fusion.cpp
    LIBS fx_emit_neon
)

# End-to-end tests through the translator core (built only when fx_translator
# exists; uses the core's DiffRunner from core_test_machine.cpp).
if(TARGET fx_translator)
    fx_add_test(fx_emit_neon_region_tests
        SOURCES
            ${CMAKE_CURRENT_LIST_DIR}/emit_neon_region_tests.cpp
            ${CMAKE_CURRENT_LIST_DIR}/core_test_machine.cpp
        LIBS fx_emit_neon fx_emit_fp fx_translator
    )
    # SIMD&FP host-code cost audit (tools/fx_neon_cost.cpp, tools/fx_neon_cost.py).
    add_executable(fx_neon_cost ${CMAKE_SOURCE_DIR}/tools/fx_neon_cost.cpp)
    fx_component_options(fx_neon_cost)
    # fx_emit_system is defined later (system.cmake): link it by name.
    target_link_libraries(fx_neon_cost PRIVATE fx_translator fx_emit_fp fx_emit_neon fx_emit_system fx_common)
    if(TARGET fx_oracle)
        target_link_libraries(fx_neon_cost PRIVATE fx_oracle)
    endif()
endif()
