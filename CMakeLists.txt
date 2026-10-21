cmake_minimum_required(VERSION 3.24)
project(transmute VERSION 0.1.0 LANGUAGES CXX)

# MSBuild only builds projects in parallel; /MP also compiles the sources of
# one project in parallel (alchemy_core alone has well over 100).
option(TRANSMUTE_MSVC_MP "Compile the sources of each MSVC project in parallel (/MP)" ON)
if(MSVC AND TRANSMUTE_MSVC_MP AND CMAKE_GENERATOR MATCHES "Visual Studio")
    add_compile_options($<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/MP>)
endif()

# Shared compiler cache (docs/ALCHEMY_BUILD.md). Off by default. It needs a
# Ninja (or Makefile) generator, because the Visual Studio generator ignores
# compiler launchers, and debug information embedded in the objects (/Z7):
# sccache cannot cache objects that write a shared PDB (/Zi).
option(TRANSMUTE_COMPILER_CACHE "Compile through sccache (Ninja generator; /Z7 debug information)" OFF)
set(TRANSMUTE_MSVC_DEBUG_INFO /Zi)
if(TRANSMUTE_COMPILER_CACHE)
    if(CMAKE_GENERATOR MATCHES "Visual Studio")
        message(FATAL_ERROR "TRANSMUTE_COMPILER_CACHE needs the Ninja generator: configure with -G Ninja "
            "from a Visual Studio developer environment (scripts/dev/build_env.ps1).")
    endif()
    find_program(TRANSMUTE_SCCACHE NAMES sccache
        HINTS "${CMAKE_SOURCE_DIR}/artifacts/tools/sccache" ENV TRANSMUTE_SCCACHE_DIR
        DOC "sccache executable used as the compiler launcher")
    if(NOT TRANSMUTE_SCCACHE)
        message(FATAL_ERROR "TRANSMUTE_COMPILER_CACHE is ON but sccache was not found; "
            "set TRANSMUTE_SCCACHE or TRANSMUTE_SCCACHE_DIR.")
    endif()
    # sccache replays the /showIncludes notes of the compile that filled the
    # cache, which name that compile's checkout; tools/sccache_shim.cpp rewrites
    # them to this checkout so Ninja's header dependencies stay right.
    if(NOT CMAKE_CL_SHOWINCLUDES_PREFIX)
        message(FATAL_ERROR "TRANSMUTE_COMPILER_CACHE: the compiler's /showIncludes prefix is unknown.")
    endif()
    set(TRANSMUTE_SCCACHE_SHIM "${CMAKE_BINARY_DIR}/tools/sccache_shim.exe")
    try_compile(TRANSMUTE_SCCACHE_SHIM_BUILT "${CMAKE_BINARY_DIR}/tools/sccache_shim_build"
        SOURCES "${CMAKE_SOURCE_DIR}/tools/sccache_shim.cpp"
        CXX_STANDARD 20
        COPY_FILE "${TRANSMUTE_SCCACHE_SHIM}"
        OUTPUT_VARIABLE TRANSMUTE_SCCACHE_SHIM_OUTPUT)
    if(NOT TRANSMUTE_SCCACHE_SHIM_BUILT)
        message(FATAL_ERROR "TRANSMUTE_COMPILER_CACHE: building tools/sccache_shim.cpp failed:\n${TRANSMUTE_SCCACHE_SHIM_OUTPUT}")
    endif()
    file(TO_NATIVE_PATH "${TRANSMUTE_SCCACHE}" sccache_native)
    file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}" source_native)
    file(WRITE "${CMAKE_BINARY_DIR}/tools/sccache_shim.cfg"
        "${sccache_native}\n${source_native}\n${CMAKE_CL_SHOWINCLUDES_PREFIX}\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/sccache_shim.cpp")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${TRANSMUTE_SCCACHE_SHIM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${TRANSMUTE_SCCACHE_SHIM}")
    set(TRANSMUTE_MSVC_DEBUG_INFO /Z7)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The fx engine (docs/FX_ENGINE.md). TRANSMUTE_FX_ONLY builds just fx, its
# oracle and tests; TRANSMUTE_WITH_FX adds fx to the full build.
option(TRANSMUTE_FX_ONLY "Build only the fx engine, its oracle and its tests" OFF)
option(TRANSMUTE_WITH_FX "Also build the fx engine in the full build" OFF)
if(TRANSMUTE_FX_ONLY)
    enable_testing()
    include(cmake/TransmuteFx.cmake)
    return()
endif()

add_library(transmute_core
    src/pe_image.cpp
    src/a64.cpp
    src/guest_process.cpp
    src/jit_ir.cpp
    src/jit_simd.cpp
    src/jit_bit_moves.cpp
    src/jit_simd_fusion.cpp
    src/jit_fp.cpp
    src/jit_atomic.cpp
)
if(WIN32)
    target_sources(transmute_core PRIVATE src/win32_bridge.cpp src/crt_scan.cpp
                                         src/win32_native_imports.cpp)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|X86_64)$")
        target_sources(transmute_core PRIVATE src/jit_x64_windows.cpp)
    endif()
target_link_libraries(transmute_core PRIVATE Shlwapi Pathcch Shell32 Ole32 OleAut32 Bcrypt Advapi32 Shcore User32 Comctl32 Gdi32 OpenGL32 Dwmapi Imm32 Ws2_32 Version)
endif()
target_include_directories(transmute_core PUBLIC include)
if(MSVC)
    target_compile_options(transmute_core PRIVATE /W4 /WX /permissive-)
    # Keep optimized profiling builds on the same inlining policy as Release.
    target_compile_options(transmute_core PRIVATE $<$<CONFIG:RelWithDebInfo>:/Ob2>)
else()
    target_compile_options(transmute_core PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

add_executable(transmute src/main.cpp)
target_link_libraries(transmute PRIVATE transmute_core)
if(MSVC)
    target_compile_options(transmute PRIVATE $<$<CONFIG:RelWithDebInfo>:/Ob2>)
endif()
if(WIN32)
    target_link_libraries(transmute PRIVATE Shell32)
    add_executable(transmute_debug_host_crash scripts/debug_host_crash.cpp)
    target_link_libraries(transmute_debug_host_crash PRIVATE Dbghelp Advapi32)
endif()

include(CTest)
if(BUILD_TESTING)
    if(WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|X86_64)$")
        add_executable(transmute_concurrent_region_bench tests/concurrent_region_bench.cpp)
        target_link_libraries(transmute_concurrent_region_bench PRIVATE transmute_core)
    endif()
    add_executable(transmute_tests tests/test_main.cpp)
    target_link_libraries(transmute_tests PRIVATE transmute_core)
    if(WIN32)
        add_library(thread_lifecycle_native SHARED
            tests/fixtures/thread_lifecycle_native.cpp)
        target_link_options(thread_lifecycle_native PRIVATE
            /ENTRY:DllMain /NODEFAULTLIB /INCLUDE:_tls_used)
        target_link_libraries(thread_lifecycle_native PRIVATE Kernel32)
        add_dependencies(transmute_tests thread_lifecycle_native)
        target_compile_definitions(transmute_tests PRIVATE
            TRANS_MUTE_NATIVE_THREAD_FIXTURE="$<TARGET_FILE:thread_lifecycle_native>")
        set(TRANS_MUTE_LLVM_HINT
            "${CMAKE_VS_INSTALLATION_DIRECTORY}/VC/Tools/Llvm/bin")
        find_program(TRANS_MUTE_CLANG_CL clang-cl HINTS "${TRANS_MUTE_LLVM_HINT}")
        find_program(TRANS_MUTE_LLD_LINK lld-link HINTS "${TRANS_MUTE_LLVM_HINT}")
    endif()
    if(TRANS_MUTE_CLANG_CL AND TRANS_MUTE_LLD_LINK)
        set(ARM64_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/return42_arm64.exe")
        set(ARM64_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/return42_arm64.obj")
        add_custom_command(
            OUTPUT "${ARM64_FIXTURE}"
            COMMAND "${TRANS_MUTE_CLANG_CL}" --target=arm64-pc-windows-msvc
                    /c /O2 "/Fo${ARM64_OBJECT}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/return42.c"
            COMMAND "${TRANS_MUTE_LLD_LINK}" /machine:arm64 /entry:guest_entry
                    /subsystem:console /nodefaultlib "/out:${ARM64_FIXTURE}"
                    "${ARM64_OBJECT}"
            DEPENDS tests/fixtures/return42.c
            VERBATIM
        )
        set(ARM64_FIXTURE_OUTPUTS "${ARM64_FIXTURE}")
        set(ARM64_TEST_ARGUMENTS "${ARM64_FIXTURE}")
        set(ARM64_RELOC_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/relocated_pointer_arm64.exe")
        set(ARM64_RELOC_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/relocated_pointer_arm64.obj")
        add_custom_command(
            OUTPUT "${ARM64_RELOC_FIXTURE}"
            COMMAND "${TRANS_MUTE_CLANG_CL}" --target=arm64-pc-windows-msvc
                    /c /O2 "/Fo${ARM64_RELOC_OBJECT}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/relocated_pointer.c"
            COMMAND "${TRANS_MUTE_LLD_LINK}" /machine:arm64 /entry:guest_entry
                    /subsystem:console /nodefaultlib "/out:${ARM64_RELOC_FIXTURE}"
                    "${ARM64_RELOC_OBJECT}"
            DEPENDS tests/fixtures/relocated_pointer.c
            VERBATIM
        )
        find_file(ARM64_KERNEL32_LIB kernel32.lib
            PATHS "$ENV{SystemDrive}/Program Files (x86)/Windows Kits/10/Lib/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/um/arm64"
            NO_DEFAULT_PATH)
        if(ARM64_KERNEL32_LIB)
            set(ARM64_IMPORT_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/win32_import_arm64.exe")
            set(ARM64_IMPORT_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/win32_import_arm64.obj")
            add_custom_command(
                OUTPUT "${ARM64_IMPORT_FIXTURE}"
                COMMAND "${TRANS_MUTE_CLANG_CL}" --target=arm64-pc-windows-msvc
                        /c /O2 "/Fo${ARM64_IMPORT_OBJECT}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/win32_import.c"
                COMMAND "${TRANS_MUTE_LLD_LINK}" /machine:arm64 /entry:guest_entry
                        /subsystem:console /nodefaultlib
                        "/out:${ARM64_IMPORT_FIXTURE}" "${ARM64_IMPORT_OBJECT}"
                        "${ARM64_KERNEL32_LIB}"
                DEPENDS tests/fixtures/win32_import.c
                VERBATIM
            )
            list(APPEND ARM64_FIXTURE_OUTPUTS "${ARM64_IMPORT_FIXTURE}")
            list(APPEND ARM64_TEST_ARGUMENTS "${ARM64_IMPORT_FIXTURE}")

            set(ARM64_CHILD_FIXTURE
                "${CMAKE_CURRENT_BINARY_DIR}/guest_child_process_arm64.exe")
            set(ARM64_CHILD_OBJECT
                "${CMAKE_CURRENT_BINARY_DIR}/guest_child_process_arm64.obj")
            add_custom_command(
                OUTPUT "${ARM64_CHILD_FIXTURE}"
                COMMAND "${TRANS_MUTE_CLANG_CL}" --target=arm64-pc-windows-msvc
                        /c /O2 /GS- "/Fo${ARM64_CHILD_OBJECT}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/guest_child_process.c"
                COMMAND "${TRANS_MUTE_LLD_LINK}" /machine:arm64 /entry:guest_entry
                        /subsystem:console /nodefaultlib
                        "/out:${ARM64_CHILD_FIXTURE}" "${ARM64_CHILD_OBJECT}"
                        "${ARM64_KERNEL32_LIB}"
                DEPENDS tests/fixtures/guest_child_process.c
                VERBATIM
            )
            list(APPEND ARM64_FIXTURE_OUTPUTS "${ARM64_CHILD_FIXTURE}")
            add_test(NAME transmute_arm64_child_process
                     COMMAND $<TARGET_FILE:transmute> "${ARM64_CHILD_FIXTURE}")
            set_tests_properties(transmute_arm64_child_process PROPERTIES TIMEOUT 30)
            get_filename_component(ARM64_SYSTEM_LIB_DIR "${ARM64_KERNEL32_LIB}" DIRECTORY)
            if(EXISTS "${ARM64_SYSTEM_LIB_DIR}/user32.lib" AND
               EXISTS "${ARM64_SYSTEM_LIB_DIR}/gdi32.lib" AND
               EXISTS "${ARM64_SYSTEM_LIB_DIR}/opengl32.lib")
                set(ARM64_GUI_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/gui_opengl_arm64.exe")
                set(ARM64_GUI_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/gui_opengl_arm64.obj")
                add_custom_command(
                    OUTPUT "${ARM64_GUI_FIXTURE}"
                    COMMAND "${TRANS_MUTE_CLANG_CL}" --target=arm64-pc-windows-msvc
                            /c /O2 /GS- "/Fo${ARM64_GUI_OBJECT}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gui_opengl.c"
                    COMMAND "${TRANS_MUTE_LLD_LINK}" /machine:arm64 /entry:guest_entry
                            /subsystem:console /nodefaultlib "/out:${ARM64_GUI_FIXTURE}"
                            "${ARM64_GUI_OBJECT}" "${ARM64_KERNEL32_LIB}"
                            "${ARM64_SYSTEM_LIB_DIR}/user32.lib"
                            "${ARM64_SYSTEM_LIB_DIR}/gdi32.lib"
                            "${ARM64_SYSTEM_LIB_DIR}/opengl32.lib"
                    DEPENDS tests/fixtures/gui_opengl.c
                    VERBATIM
                )
                list(APPEND ARM64_FIXTURE_OUTPUTS "${ARM64_GUI_FIXTURE}")
                foreach(engine IN ITEMS interpreter jit)
                    if(engine STREQUAL "jit")
                        set(GUI_ENGINE_ARGUMENTS --jit)
                    else()
                        set(GUI_ENGINE_ARGUMENTS "")
                    endif()
                    add_test(NAME transmute_arm64_gui_${engine}
                             COMMAND $<TARGET_FILE:transmute> ${GUI_ENGINE_ARGUMENTS} "${ARM64_GUI_FIXTURE}")
                    set_tests_properties(transmute_arm64_gui_${engine} PROPERTIES TIMEOUT 30)
                    add_test(NAME transmute_arm64_gui_teardown_${engine}
                             COMMAND $<TARGET_FILE:transmute> ${GUI_ENGINE_ARGUMENTS} "${ARM64_GUI_FIXTURE}" --abandon)
                    set_tests_properties(transmute_arm64_gui_teardown_${engine} PROPERTIES TIMEOUT 30)
                endforeach()
            endif()
        else()
            list(APPEND ARM64_TEST_ARGUMENTS "-")
        endif()
        list(APPEND ARM64_FIXTURE_OUTPUTS "${ARM64_RELOC_FIXTURE}")
        list(APPEND ARM64_TEST_ARGUMENTS "${ARM64_RELOC_FIXTURE}")
        set(ARM64_LEAF_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/guest_leaf.dll")
        set(ARM64_LEAF_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/guest_leaf_arm64.obj")
        set(ARM64_LEAF_IMPORT_LIBRARY "${CMAKE_CURRENT_BINARY_DIR}/guest_leaf.lib")
        add_custom_command(
            OUTPUT "${ARM64_LEAF_FIXTURE}" "${ARM64_LEAF_IMPORT_LIBRARY}"
            COMMAND "${TRANS_MUTE_CLANG_CL}" --target=arm64-pc-windows-msvc
                    /c /O2 "/Fo${ARM64_LEAF_OBJECT}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/guest_leaf.c"
            COMMAND "${TRANS_MUTE_LLD_LINK}" /machine:arm64 /dll /noentry /nodefaultlib
                    /base:0x190000000
                    "/out:${ARM64_LEAF_FIXTURE}" "/implib:${ARM64_LEAF_IMPORT_LIBRARY}"
                    "${ARM64_LEAF_OBJECT}"
            DEPENDS tests/fixtures/guest_leaf.c
            VERBATIM
        )
        set(ARM64_DLL_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/guest_library.dll")
        set(ARM64_DLL_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/guest_library_arm64.obj")
        set(ARM64_DLL_IMPORT_LIBRARY "${CMAKE_CURRENT_BINARY_DIR}/guest_library.lib")
        add_custom_command(
            OUTPUT "${ARM64_DLL_FIXTURE}" "${ARM64_DLL_IMPORT_LIBRARY}"
            COMMAND "${TRANS_MUTE_CLANG_CL}" --target=arm64-pc-windows-msvc
                    /c /O2 "/Fo${ARM64_DLL_OBJECT}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/guest_library.c"
            COMMAND "${TRANS_MUTE_LLD_LINK}" /machine:arm64 /dll /entry:DllMain /nodefaultlib
                    /base:0x140000000
                    "/out:${ARM64_DLL_FIXTURE}" "/implib:${ARM64_DLL_IMPORT_LIBRARY}"
                    "${ARM64_DLL_OBJECT}" "${ARM64_LEAF_IMPORT_LIBRARY}"
            DEPENDS tests/fixtures/guest_library.c "${ARM64_LEAF_IMPORT_LIBRARY}"
            VERBATIM
        )
        set(ARM64_APP_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/guest_app.exe")
        set(ARM64_FAIL_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/guest_fail.dll")
        set(ARM64_FAIL_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/guest_fail_arm64.obj")
        add_custom_command(
            OUTPUT "${ARM64_FAIL_FIXTURE}"
            COMMAND "${TRANS_MUTE_CLANG_CL}" --target=arm64-pc-windows-msvc
                    /c /O2 "/Fo${ARM64_FAIL_OBJECT}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/guest_fail.c"
            COMMAND "${TRANS_MUTE_LLD_LINK}" /machine:arm64 /dll /entry:DllMain
                    /nodefaultlib /base:0x1d0000000
                    "/out:${ARM64_FAIL_FIXTURE}" "${ARM64_FAIL_OBJECT}"
            DEPENDS tests/fixtures/guest_fail.c
            VERBATIM
        )
        set(ARM64_APP_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/guest_app_arm64.obj")
        add_custom_command(
            OUTPUT "${ARM64_APP_FIXTURE}"
            COMMAND "${TRANS_MUTE_CLANG_CL}" --target=arm64-pc-windows-msvc
                    /c /O2 "/Fo${ARM64_APP_OBJECT}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/guest_app.c"
            COMMAND "${TRANS_MUTE_LLD_LINK}" /machine:arm64 /entry:guest_entry
                    /subsystem:console /nodefaultlib "/out:${ARM64_APP_FIXTURE}"
                    "${ARM64_APP_OBJECT}" "${ARM64_DLL_IMPORT_LIBRARY}"
            DEPENDS tests/fixtures/guest_app.c "${ARM64_DLL_IMPORT_LIBRARY}"
            VERBATIM
        )
        list(APPEND ARM64_FIXTURE_OUTPUTS "${ARM64_LEAF_FIXTURE}" "${ARM64_DLL_FIXTURE}"
                                          "${ARM64_FAIL_FIXTURE}"
                                          "${ARM64_APP_FIXTURE}")
        list(APPEND ARM64_TEST_ARGUMENTS "${ARM64_APP_FIXTURE}")
        add_custom_target(arm64_fixture DEPENDS ${ARM64_FIXTURE_OUTPUTS})
        add_dependencies(transmute_tests arm64_fixture)
    endif()
    set(TRANSMUTE_TEST_SHARDS 4 CACHE STRING "Number of independent core test processes")
    if(NOT TRANSMUTE_TEST_SHARDS MATCHES "^[1-9][0-9]*$" OR TRANSMUTE_TEST_SHARDS GREATER 256)
        message(FATAL_ERROR "TRANSMUTE_TEST_SHARDS must be between 1 and 256")
    endif()
    math(EXPR TRANSMUTE_LAST_TEST_SHARD "${TRANSMUTE_TEST_SHARDS} - 1")
    foreach(shard RANGE 0 ${TRANSMUTE_LAST_TEST_SHARD})
        add_test(NAME transmute_core_tests_${shard}
                 COMMAND transmute_tests --shard ${shard} ${TRANSMUTE_TEST_SHARDS}
                         ${ARM64_TEST_ARGUMENTS})
        set_tests_properties(transmute_core_tests_${shard} PROPERTIES TIMEOUT 120)
    endforeach()
endif()

include(cmake/Alchemy.cmake)

if(TRANSMUTE_WITH_FX)
    include(cmake/TransmuteFx.cmake)
    include(cmake/AlchemyFx.cmake)
endif()
include(cmake/AlchemyExpat.cmake)
include(cmake/AlchemySwift.cmake)
include(cmake/AlchemyProcess.cmake)
