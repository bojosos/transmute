# Owner: integration. Test framework, differential oracle and snippet runner.
fx_add_library(fx_testing
    ${CMAKE_CURRENT_LIST_DIR}/test_main.cpp
)
target_link_libraries(fx_testing PUBLIC fx_common)
target_sources(fx_testing PRIVATE ${CMAKE_CURRENT_LIST_DIR}/harness.cpp)
add_executable(fx_testing_selftest ${CMAKE_CURRENT_LIST_DIR}/harness_selftest.cpp
    ${CMAKE_CURRENT_LIST_DIR}/engine_stack_tests.cpp)
fx_component_options(fx_testing_selftest)
target_link_libraries(fx_testing_selftest PRIVATE fx_testing)
add_test(NAME fx_testing_selftest COMMAND fx_testing_selftest)
