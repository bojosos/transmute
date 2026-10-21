# Owner: seh. ARM64 unwinder, exception dispatch, Rtl* entry points, longjmp.
fx_add_library(fx_seh
    ${CMAKE_CURRENT_LIST_DIR}/memory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/unwind.cpp
    ${CMAKE_CURRENT_LIST_DIR}/function_table.cpp
    ${CMAKE_CURRENT_LIST_DIR}/context.cpp
    ${CMAKE_CURRENT_LIST_DIR}/guest_call.cpp
    ${CMAKE_CURRENT_LIST_DIR}/vectored.cpp
    ${CMAKE_CURRENT_LIST_DIR}/dispatch.cpp
    ${CMAKE_CURRENT_LIST_DIR}/diag.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rtl.cpp
    ${CMAKE_CURRENT_LIST_DIR}/longjmp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/stack.cpp
)
target_link_libraries(fx_seh PUBLIC fx_common)

fx_add_test(fx_seh_tests
    SOURCES
        ${CMAKE_CURRENT_LIST_DIR}/tests/test_unwind.cpp
        ${CMAKE_CURRENT_LIST_DIR}/tests/test_function_table.cpp
        ${CMAKE_CURRENT_LIST_DIR}/tests/test_context.cpp
        ${CMAKE_CURRENT_LIST_DIR}/tests/test_dispatch.cpp
        ${CMAKE_CURRENT_LIST_DIR}/tests/test_corpus.cpp
        ${CMAKE_CURRENT_LIST_DIR}/tests/call_guest_stub.cpp
    LIBS fx_seh
)
