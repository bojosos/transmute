# Owner: runtime. Code arena, lookup table, stubs, dispatcher, linking,
# threads, call_guest, vectored exception handler, SMC, interpreter fallback.
fx_add_library(fx_runtime
    ${CMAKE_CURRENT_LIST_DIR}/arena.cpp
    ${CMAKE_CURRENT_LIST_DIR}/code_memory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/dispatcher.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lookup.cpp
    ${CMAKE_CURRENT_LIST_DIR}/profiler.cpp
    ${CMAKE_CURRENT_LIST_DIR}/regions.cpp
    ${CMAKE_CURRENT_LIST_DIR}/runtime.cpp
    ${CMAKE_CURRENT_LIST_DIR}/shared_memory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/stubs.cpp
    ${CMAKE_CURRENT_LIST_DIR}/threads.cpp
    ${CMAKE_CURRENT_LIST_DIR}/veh.cpp
)
# fx_emit_fp: the FPCR/FPSR <-> MXCSR protocol (fp_mxcsr_for_state, fp_read_fpsr, ...).
target_link_libraries(fx_runtime PUBLIC fx_common fx_emit_fp)

# Tests provide stub definitions of the other components' functions
# (translate_region, cache_*, loader_*, bind_unknown_host_target,
# dispatch_guest_exception) in runtime_test_stubs.cpp.
fx_add_test(fx_runtime_tests
    SOURCES
        ${CMAKE_CURRENT_LIST_DIR}/tests/runtime_test_stubs.cpp
        ${CMAKE_CURRENT_LIST_DIR}/tests/runtime_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/tests/runtime_region_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/tests/runtime_invalidation_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/tests/runtime_reclaim_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/tests/runtime_thread_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/tests/runtime_bench_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/tests/runtime_fp_boundary_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/tests/runtime_profiler_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/tests/runtime_shared_memory_tests.cpp
    LIBS fx_runtime
)
