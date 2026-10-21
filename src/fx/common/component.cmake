# Owner: integration.
fx_add_library(fx_common
    ${CMAKE_CURRENT_LIST_DIR}/cpu_features.cpp
    ${CMAKE_CURRENT_LIST_DIR}/interpreter.cpp
)
# fx_host: the engine's host seams (src/fx/host/), for every fx component.
target_link_libraries(fx_common PUBLIC fx_oracle fx_host)
add_executable(fx_assembler_dump ${CMAKE_CURRENT_LIST_DIR}/assembler_dump.cpp)
fx_component_options(fx_assembler_dump)

# Interpreter fallback atomicity (multithreaded LSE/CAS/CASP/exclusives, mixed
# with translated atomics) and its differential against the reference engine.
fx_add_test(fx_interpreter_tests
    SOURCES
        ${CMAKE_CURRENT_LIST_DIR}/interpreter_tests.cpp
    LIBS fx_emit_system fx_common
)
