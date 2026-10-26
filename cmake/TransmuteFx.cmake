# fx engine build (see docs/FX_ENGINE.md).
#
# Owner: integration. Each component owns src/fx/<component>/component.cmake
# (and translator families their own *.cmake) and defines its targets there
# with fx_add_library / fx_add_test. Do not edit other components' files.

# The reference interpreter, used as the differential oracle and the
# one-instruction fallback. Excludes the Win32 bridge.
add_library(fx_oracle STATIC
    src/pe_image.cpp
    src/a64.cpp
    src/jit_ir.cpp
    src/jit_simd.cpp
    src/jit_bit_moves.cpp
    src/jit_simd_fusion.cpp
    src/jit_fp.cpp
    src/jit_atomic.cpp
    src/jit_x64_windows.cpp
)
target_include_directories(fx_oracle PUBLIC ${PROJECT_SOURCE_DIR}/include PRIVATE ${PROJECT_SOURCE_DIR}/src)
if(MSVC)
    target_compile_options(fx_oracle PRIVATE /permissive- /W3)
endif()

function(fx_component_options target)
    target_include_directories(${target} PUBLIC ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_compile_definitions(${target} PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
    if(MSVC)
        # 4324: structure padded due to alignas (GuestContext).
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:preprocessor /wd4324 ${TRANSMUTE_MSVC_DEBUG_INFO})
        target_link_options(${target} PRIVATE $<$<STREQUAL:$<TARGET_PROPERTY:${target},TYPE>,EXECUTABLE>:/DEBUG>)
    endif()
endfunction()

# Drops sources that do not exist yet (with a warning) so one component's
# in-progress file list never breaks configuration for everyone else.
function(fx_existing_sources out)
    set(result)
    foreach(source IN LISTS ARGN)
        if(IS_ABSOLUTE "${source}")
            set(path "${source}")
        else()
            set(path "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
        endif()
        if(EXISTS "${path}")
            list(APPEND result "${source}")
        else()
            message(WARNING "fx: skipping missing source ${source}")
        endif()
    endforeach()
    set(${out} ${result} PARENT_SCOPE)
endfunction()

# fx_add_library(<name> <sources...>)
function(fx_add_library name)
    fx_existing_sources(sources ${ARGN})
    add_library(${name} STATIC ${sources})
    fx_component_options(${name})
endfunction()

# fx_add_test(<name> SOURCES <files...> LIBS <targets...>)
# Test executables use src/fx/testing (test.hpp) and run without arguments;
# pass --filter <substring> to run a subset, --list to list tests.
function(fx_add_test name)
    cmake_parse_arguments(FX_TEST "" "" "SOURCES;LIBS" ${ARGN})
    fx_existing_sources(sources ${FX_TEST_SOURCES})
    add_executable(${name} ${sources})
    fx_component_options(${name})
    target_link_libraries(${name} PRIVATE ${FX_TEST_LIBS} fx_testing)
    add_test(NAME ${name} COMMAND ${name})
    # The property overrides ctest --timeout, so it has to cover a loaded machine and deep fuzzing
    # (TRANSMUTE_FX_FUZZ_SCALE): fx_emit_fp_tests took 258 s at the default scale under -j4 load.
    set_tests_properties(${name} PROPERTIES TIMEOUT 900)
endfunction()

set(FX_COMPONENT_FILES
    src/fx/host/component.cmake
    src/fx/common/component.cmake
    src/fx/testing/component.cmake
    src/fx/translator/component.cmake
    src/fx/translator/fp.cmake
    src/fx/translator/neon.cmake
    src/fx/translator/system.cmake
    src/fx/runtime/component.cmake
    src/fx/loader/component.cmake
    src/fx/abi/component.cmake
    src/fx/seh/component.cmake
    src/fx/cache/component.cmake
    tests/fx/component.cmake
)
foreach(component_file IN LISTS FX_COMPONENT_FILES)
    if(EXISTS ${PROJECT_SOURCE_DIR}/${component_file})
        include(${PROJECT_SOURCE_DIR}/${component_file})
    endif()
endforeach()

# Integration executable. Links whichever component libraries exist; the
# loader component provides src/fx/loader/main.cpp.
if(EXISTS ${PROJECT_SOURCE_DIR}/src/fx/loader/main.cpp)
    add_executable(transmute-fx src/fx/loader/main.cpp)
    fx_component_options(transmute-fx)
    foreach(lib fx_loader fx_runtime fx_abi fx_seh fx_cache fx_translator
                fx_emit_fp fx_emit_neon fx_emit_system fx_common fx_oracle)
        if(TARGET ${lib})
            target_link_libraries(transmute-fx PRIVATE ${lib})
        endif()
    endforeach()
endif()

# Static translator coverage audit (integration tool).
add_executable(fx_coverage tools/fx_coverage.cpp)
fx_component_options(fx_coverage)
foreach(lib fx_translator fx_emit_fp fx_emit_neon fx_emit_system fx_common fx_oracle)
    if(TARGET ${lib})
        target_link_libraries(fx_coverage PRIVATE ${lib})
    endif()
endforeach()
