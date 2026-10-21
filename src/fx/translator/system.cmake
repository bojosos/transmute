# Owner: atomics/system family. Define this component's targets with fx_add_library / fx_add_test.
fx_add_library(fx_emit_system
    ${CMAKE_CURRENT_LIST_DIR}/emit_system.cpp
    ${CMAKE_CURRENT_LIST_DIR}/emit_atomic.cpp
    ${CMAKE_CURRENT_LIST_DIR}/emit_system_crypto.cpp
    ${CMAKE_CURRENT_LIST_DIR}/emit_system_reference.cpp
    ${CMAKE_CURRENT_LIST_DIR}/emit_system_timer.cpp
    ${CMAKE_CURRENT_LIST_DIR}/emit_system_helpers.cpp
    ${CMAKE_CURRENT_LIST_DIR}/emit_system_register.cpp
)
target_link_libraries(fx_emit_system PUBLIC fx_common)

fx_add_test(fx_emit_system_tests
    SOURCES
        ${CMAKE_CURRENT_LIST_DIR}/emit_system_test_util.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_system_core_like.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_system_core_like_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_atomic_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_atomic_fusion_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_system_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_system_crypto_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_system_sha512_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/emit_atomic_stress_tests.cpp
    LIBS fx_emit_system
)

# Cross-check through the real translator core and its test dispatcher
# (only when the core is part of the build).
if(TARGET fx_translator)
    fx_add_test(fx_emit_system_core_tests
        SOURCES
            ${CMAKE_CURRENT_LIST_DIR}/emit_system_core_tests.cpp
            ${CMAKE_CURRENT_LIST_DIR}/emit_system_test_util.cpp
            ${CMAKE_CURRENT_LIST_DIR}/emit_system_core_like.cpp
            ${CMAKE_CURRENT_LIST_DIR}/core_test_machine.cpp
        LIBS fx_translator fx_emit_system
    )
endif()
