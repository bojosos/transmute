# Owner: fp family. Scalar and vector floating point, FPCR/FPSR.
fx_add_library(fx_emit_fp
    ${CMAKE_CURRENT_LIST_DIR}/emit_fp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/emit_fp_arith.cpp
    ${CMAKE_CURRENT_LIST_DIR}/emit_fp_convert.cpp
    ${CMAKE_CURRENT_LIST_DIR}/emit_fp_estimate.cpp
    ${CMAKE_CURRENT_LIST_DIR}/emit_fp_half.cpp
    ${CMAKE_CURRENT_LIST_DIR}/emit_fp_register.cpp
)
# The neon family provides the shared rip-relative constant pool.
target_link_libraries(fx_emit_fp PUBLIC fx_common fx_emit_neon)

fx_add_test(fx_emit_fp_tests
    SOURCES
        ${CMAKE_CURRENT_LIST_DIR}/emit_fp_test_util.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_fp_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_fp_tests_scalar.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_fp_tests_vector.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_fp_tests_ref.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_fp_tests_perf.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_fp_tests_special.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_fp_tests_fpsr.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_fp_tests_half.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_fp_tests_lowering.cpp
    LIBS fx_emit_fp
)
