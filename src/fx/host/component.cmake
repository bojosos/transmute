# Owner: r4 / runtime. fx host seams (src/fx/host/README.md, docs/FX_HOST_SEAMS.md).
#
# TRANSMUTE_FX_HOST picks the backend at compile time: src/fx/host/<backend>/
# provides the definitions and TRANSMUTE_FX_HOST_<BACKEND>=1 selects its
# inline headers. Only win32 exists.
set(TRANSMUTE_FX_HOST "win32" CACHE STRING "fx engine host backend (win32)")
set_property(CACHE TRANSMUTE_FX_HOST PROPERTY STRINGS win32)
if(NOT IS_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/${TRANSMUTE_FX_HOST}")
    message(FATAL_ERROR "TRANSMUTE_FX_HOST=${TRANSMUTE_FX_HOST}: no backend src/fx/host/${TRANSMUTE_FX_HOST}/")
endif()
file(GLOB fx_host_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/${TRANSMUTE_FX_HOST}/*.cpp")
fx_add_library(fx_host ${fx_host_sources})
string(TOUPPER "${TRANSMUTE_FX_HOST}" fx_host_upper)
target_compile_definitions(fx_host PUBLIC TRANSMUTE_FX_HOST_${fx_host_upper}=1)

fx_add_test(fx_host_tests
    SOURCES
        ${CMAKE_CURRENT_LIST_DIR}/tests/host_tests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/tests/host_win32_tests.cpp
    LIBS fx_host
)
