# Owner: abi. Signature database (tools/sigdb), guest->host thunks, callbacks
# and API/CRT overrides. See src/fx/abi/NOTES.md.
fx_add_library(fx_abi
    ${CMAKE_CURRENT_LIST_DIR}/signature.cpp
    ${CMAKE_CURRENT_LIST_DIR}/sigdb.cpp
    ${CMAKE_CURRENT_LIST_DIR}/thunk_gen.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bind.cpp
    ${CMAKE_CURRENT_LIST_DIR}/overrides.cpp
    ${CMAKE_CURRENT_LIST_DIR}/overrides_crt.cpp
    ${CMAKE_CURRENT_LIST_DIR}/overrides_fiber.cpp
    ${CMAKE_CURRENT_LIST_DIR}/diagnostics.cpp
)
target_link_libraries(fx_abi PUBLIC fx_common)
if(MSVC)
    # The generated signature table is a large constant initializer.
    target_compile_options(fx_abi PRIVATE /bigobj)
endif()

# The tests stub the runtime, loader and seh entry points the library calls
# (tests/abi_test_support.cpp), so they never link other components.
fx_add_test(fx_abi_tests
    SOURCES
        ${CMAKE_CURRENT_LIST_DIR}/tests/abi_test_support.cpp
        ${CMAKE_CURRENT_LIST_DIR}/tests/abi_signature_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/tests/abi_thunk_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/tests/abi_callback_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/tests/abi_override_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/tests/abi_fiber_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/tests/abi_bench_tests.cpp
    LIBS fx_abi
)
