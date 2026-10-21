# Owner: loader. PE mapping, relocation, imports, TLS, DllMain, module APIs
# and transmute-fx.exe (src/fx/loader/main.cpp, built by cmake/TransmuteFx.cmake).

set(FX_LOADER_DIR ${CMAKE_CURRENT_LIST_DIR})

fx_add_library(fx_loader
    ${FX_LOADER_DIR}/pe.cpp
    ${FX_LOADER_DIR}/image.cpp
    ${FX_LOADER_DIR}/search.cpp
    ${FX_LOADER_DIR}/loader.cpp
    ${FX_LOADER_DIR}/tls.cpp
    ${FX_LOADER_DIR}/missing.cpp
    ${FX_LOADER_DIR}/process.cpp
    ${FX_LOADER_DIR}/command_line.cpp
)
target_link_libraries(fx_loader PUBLIC Advapi32)

# ---- ARM64 fixtures (TLS, exports/forwarders/ordinals, cycles, delay load) ----
set(FX_LOADER_LLVM_HINTS)
if(CMAKE_VS_INSTALLATION_DIRECTORY)
    list(APPEND FX_LOADER_LLVM_HINTS "${CMAKE_VS_INSTALLATION_DIRECTORY}/VC/Tools/Llvm/bin")
endif()
if(CMAKE_CXX_COMPILER MATCHES "/VC/Tools/MSVC/")
    # <VS>/VC/Tools/MSVC/<version>/bin/Host<arch>/<arch>/cl.exe -> <VS>/VC/Tools/Llvm/bin
    string(REGEX REPLACE "/VC/Tools/MSVC/.*$" "/VC/Tools/Llvm/bin" fxl_llvm_from_cl "${CMAKE_CXX_COMPILER}")
    list(APPEND FX_LOADER_LLVM_HINTS "${fxl_llvm_from_cl}")
endif()
foreach(edition Enterprise Professional Community BuildTools)
    list(APPEND FX_LOADER_LLVM_HINTS "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/${edition}/VC/Tools/Llvm/bin")
endforeach()
find_program(FX_LOADER_CLANG_CL clang-cl HINTS ${FX_LOADER_LLVM_HINTS})
find_program(FX_LOADER_LLD_LINK lld-link HINTS ${FX_LOADER_LLVM_HINTS})
find_program(FX_LOADER_LLVM_LIB llvm-lib HINTS ${FX_LOADER_LLVM_HINTS})
set(FX_LOADER_FIXTURE_SRC ${FX_LOADER_DIR}/tests/fixtures)
set(FX_LOADER_FIXTURE_OUT ${CMAKE_BINARY_DIR}/fx_loader_fixtures)
set(FX_LOADER_FIXTURES)

if(FX_LOADER_CLANG_CL AND FX_LOADER_LLD_LINK AND FX_LOADER_LLVM_LIB)
    file(MAKE_DIRECTORY ${FX_LOADER_FIXTURE_OUT})
    set(fxl_cc "${FX_LOADER_CLANG_CL}" --target=arm64-pc-windows-msvc /c /O2 /GS-)
    set(fxl_link "${FX_LOADER_LLD_LINK}" /machine:arm64 /nodefaultlib)
    set(fxl_lib "${FX_LOADER_LLVM_LIB}" /machine:arm64)
    set(o ${FX_LOADER_FIXTURE_OUT})
    set(s ${FX_LOADER_FIXTURE_SRC})

    # Import libraries generated from .def files (cycles, fake KERNEL32 names,
    # a DLL that does not exist).
    foreach(def fxl_cycle_a fxl_cycle_b fxl_kernel32 fxl_missing fxl_exitwait_k32)
        add_custom_command(OUTPUT ${o}/${def}_imp.lib
            COMMAND ${fxl_lib} /def:${s}/${def}.def /out:${o}/${def}_imp.lib
            DEPENDS ${s}/${def}.def VERBATIM)
    endforeach()

    add_custom_command(OUTPUT ${o}/fxl_fwd_target.dll
        COMMAND ${fxl_cc} /Fo${o}/fxl_fwd_target.obj ${s}/fxl_fwd_target.c
        COMMAND ${fxl_link} /dll /noentry /def:${s}/fxl_fwd_target.def /base:0x180000000
                /out:${o}/fxl_fwd_target.dll /implib:${o}/fxl_fwd_target.lib ${o}/fxl_fwd_target.obj
        DEPENDS ${s}/fxl_fwd_target.c ${s}/fxl_fwd_target.def VERBATIM)

    add_custom_command(OUTPUT ${o}/fxl_exports.dll ${o}/fxl_exports.lib
        COMMAND ${fxl_cc} /Fo${o}/fxl_exports.obj ${s}/fxl_exports.c
        COMMAND ${fxl_link} /dll /noentry /def:${s}/fxl_exports.def /section:.rwx,RWE
                /out:${o}/fxl_exports.dll /implib:${o}/fxl_exports.lib ${o}/fxl_exports.obj
        DEPENDS ${s}/fxl_exports.c ${s}/fxl_exports.def VERBATIM)

    add_custom_command(OUTPUT ${o}/fxl_cycle_a.dll
        COMMAND ${fxl_cc} /Fo${o}/fxl_cycle_a.obj ${s}/fxl_cycle_a.c
        COMMAND ${fxl_link} /dll /noentry /noimplib /out:${o}/fxl_cycle_a.dll
                ${o}/fxl_cycle_a.obj ${o}/fxl_cycle_b_imp.lib
        DEPENDS ${s}/fxl_cycle_a.c ${o}/fxl_cycle_b_imp.lib VERBATIM)

    add_custom_command(OUTPUT ${o}/fxl_cycle_b.dll
        COMMAND ${fxl_cc} /Fo${o}/fxl_cycle_b.obj ${s}/fxl_cycle_b.c
        COMMAND ${fxl_link} /dll /noentry /noimplib /out:${o}/fxl_cycle_b.dll
                ${o}/fxl_cycle_b.obj ${o}/fxl_cycle_a_imp.lib
        DEPENDS ${s}/fxl_cycle_b.c ${o}/fxl_cycle_a_imp.lib VERBATIM)

    add_custom_command(OUTPUT ${o}/fxl_delay.dll ${o}/fxl_delay.lib
        COMMAND ${fxl_cc} /Fo${o}/fxl_delay.obj ${s}/fxl_delay.c
        COMMAND ${fxl_link} /dll /noentry /out:${o}/fxl_delay.dll /implib:${o}/fxl_delay.lib
                ${o}/fxl_delay.obj
        DEPENDS ${s}/fxl_delay.c VERBATIM)

    add_custom_command(OUTPUT ${o}/fxl_tls.dll ${o}/fxl_tls.lib
        COMMAND ${fxl_cc} /Fo${o}/fxl_tls.obj ${s}/fxl_tls.c
        COMMAND ${fxl_link} /dll /entry:DllMain /out:${o}/fxl_tls.dll /implib:${o}/fxl_tls.lib
                ${o}/fxl_tls.obj
        DEPENDS ${s}/fxl_tls.c ${s}/fxl_tlssup.h VERBATIM)

    add_custom_command(OUTPUT ${o}/fxl_importer.exe
        COMMAND ${fxl_cc} /Fo${o}/fxl_importer.obj ${s}/fxl_importer.c
        COMMAND ${fxl_link} /entry:fxl_entry /subsystem:console /out:${o}/fxl_importer.exe
                /implib:${o}/fxl_importer.lib ${o}/fxl_importer.obj
                ${o}/fxl_exports.lib ${o}/fxl_cycle_a_imp.lib ${o}/fxl_tls.lib
                ${o}/fxl_kernel32_imp.lib ${o}/fxl_missing_imp.lib ${o}/fxl_delay.lib
                /delayload:fxl_delay.dll
        DEPENDS ${s}/fxl_importer.c ${s}/fxl_tlssup.h ${o}/fxl_exports.lib ${o}/fxl_cycle_a_imp.lib
                ${o}/fxl_tls.lib ${o}/fxl_kernel32_imp.lib ${o}/fxl_missing_imp.lib ${o}/fxl_delay.lib
        VERBATIM)

    add_custom_command(OUTPUT ${o}/fxl_exitwait.dll
        COMMAND ${fxl_cc} /Fo${o}/fxl_exitwait.obj ${s}/fxl_exitwait.c
        COMMAND ${fxl_link} /dll /entry:DllMain /noimplib /out:${o}/fxl_exitwait.dll
                ${o}/fxl_exitwait.obj ${o}/fxl_exitwait_k32_imp.lib
        DEPENDS ${s}/fxl_exitwait.c ${o}/fxl_exitwait_k32_imp.lib VERBATIM)

    set(FX_LOADER_FIXTURES
        ${o}/fxl_fwd_target.dll ${o}/fxl_exports.dll ${o}/fxl_cycle_a.dll ${o}/fxl_cycle_b.dll
        ${o}/fxl_delay.dll ${o}/fxl_tls.dll ${o}/fxl_importer.exe ${o}/fxl_exitwait.dll)
    add_custom_target(fx_loader_fixtures DEPENDS ${FX_LOADER_FIXTURES})
else()
    message(WARNING "fx loader: clang-cl/lld-link/llvm-lib not found; ARM64 loader fixtures skipped")
endif()

fx_add_test(fx_loader_tests
    SOURCES
        ${FX_LOADER_DIR}/tests/runtime_stubs.cpp
        ${FX_LOADER_DIR}/tests/loader_tests.cpp
        ${FX_LOADER_DIR}/tests/tls_tests.cpp
        ${FX_LOADER_DIR}/tests/blender_tests.cpp
    LIBS fx_loader)
target_compile_definitions(fx_loader_tests PRIVATE
    FXL_FIXTURE_DIR="${FX_LOADER_FIXTURE_OUT}"
    FXL_LEGACY_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/build"
    FXL_BLENDER_DIR="${PROJECT_SOURCE_DIR}/artifacts/arm64-stage/Blender Foundation/Blender 4.5")
target_link_libraries(fx_loader_tests PRIVATE Shell32 Dbghelp)
if(TARGET fx_loader_fixtures)
    add_dependencies(fx_loader_tests fx_loader_fixtures)
endif()
