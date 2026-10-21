# Alchemy GUI (alchemy-fx only): AppKit, QuartzCore, Metal and the MSL
# compiler as project-authored host providers over Win32 and Vulkan
# (include/alchemy/gui/install.hpp). Included at the end of cmake/Alchemy.cmake.
#
# Nothing here is part of alchemy.exe or alchemy_core: the subsystem sources
# are collected in ALCHEMY_GUI_SOURCES and compiled into alchemy_core_fx with
# ALCHEMY_WITH_GUI by cmake/AlchemyFx.cmake. Without TRANSMUTE_WITH_FX the list
# is unused.
#
# One fragment per subsystem, each owned by one agent; a fragment edits only
# itself:
#   cmake/gui/objc.cmake         Objective-C runtime additions   src/alchemy/objc/
#   cmake/darwin/kit_export_fill.cmake  the export-fill kit (process-level gaps) src/alchemy/darwin/kit/export_fill/
#   cmake/darwin/kit_support.cmake    IOSurface, CVDisplayLink, layer contents (qt-vfb) src/alchemy/darwin/kit/support/
#   cmake/darwin/kit_fwgaps.cmake       existing-framework gaps (strict) src/alchemy/darwin/kit/fwgaps/
#
# A fragment
#   - appends its sources (relative to the source root) to ALCHEMY_GUI_SOURCES;
#   - may append to ALCHEMY_GUI_INCLUDE_DIRECTORIES and
#     ALCHEMY_GUI_COMPILE_DEFINITIONS (PRIVATE to alchemy_core_fx) and to
#     ALCHEMY_GUI_LINK_LIBRARIES (prefer LoadLibrary over link dependencies);
#   - defines a function that registers its tests and passes its name to
#     alchemy_gui_register_tests(). Registered functions run once the whole
#     project is configured (cmake_language(DEFER)), only when alchemy_core_fx
#     exists and BUILD_TESTING is on, so they may use alchemy-fx, the macOS
#     fixture pipeline (macos_dir, ALCHEMY_LD64_LLD, macos_link_options) and
#     the helpers below. Test names start with "alchemy_" so the standard
#     `ctest -R "^alchemy_"` run includes them.
#
# Helpers for registered test functions:
#   alchemy_gui_add_unit_test(<name> <source>... [TIMEOUT <s>])
#       Host test executable linked with alchemy_core_fx (fx prelude, GUI
#       definitions), registered as ctest <name>.
#   alchemy_gui_object(<name> <source> [<clang flag>...])
#       tests/fixtures/macos/<source> -> ${macos_dir}/<name>.o for
#       arm64-apple-macos against runtime/include (AppKit, Metal, QuartzCore,
#       CoreGraphics, Foundation declaration headers). .m sources get
#       -fno-objc-exceptions; pass -fobjc-arc for ARC.
#   alchemy_gui_program(<name> <object>...)
#       Links ${macos_dir}/<name> from objects and ALCHEMY_GUI_LINK_STUBS.
#   ALCHEMY_GUI_LINK_STUBS
#       Link-only identities (never mapped; Alchemy's providers replace them by
#       install name): AppKit, Metal, QuartzCore, CoreGraphics, Foundation,
#       CoreFoundation, libobjc, libSystem. Built by target
#       alchemy_gui_link_stubs: a fixture target that links them must
#       add_dependencies(<target> alchemy_gui_link_stubs) so the shared
#       custom commands never run from two parallel targets.
#   ALCHEMY_GUI_FIXTURES_ENABLED
#       ON when the macOS fixture pipeline (clang, ld64.lld) is available;
#       fixture registrations must be skipped when OFF.
#   alchemy_gui_add_fixture_test(<name> <program> PROGRESS <regex>
#       [COMPLETE <regex>] [ALLOW_UNSUPPORTED] [TIMEOUT <s>] [ARGS <arg>...])
#       Runs alchemy-fx on <program> (tests/alchemy_gui_fixture.cmake). Passes
#       when stdout matches PROGRESS and either the run exits 0 with stdout
#       matching COMPLETE, or (ALLOW_UNSUPPORTED) it exits nonzero with an
#       explicit "unsupported" error on stderr.

# ---- third-party tools and headers (referenced, never vendored) ---------------
# runtime/THIRD_PARTY_NOTICES.md lists them. All are optional at configure
# time: the subsystems that need them fail explicitly at run time instead.
set(alchemy_gui_llvm_hints)
if(CMAKE_GENERATOR_INSTANCE)
    list(APPEND alchemy_gui_llvm_hints "${CMAKE_GENERATOR_INSTANCE}/VC/Tools/Llvm/x64/bin")
endif()
if(DEFINED ENV{VCINSTALLDIR})
    list(APPEND alchemy_gui_llvm_hints "$ENV{VCINSTALLDIR}/Tools/Llvm/x64/bin")
endif()
find_file(ALCHEMY_LIBCLANG_DLL libclang.dll HINTS ${alchemy_gui_llvm_hints} NO_DEFAULT_PATH
    DOC "Host x64 libclang.dll (LLVM 19.1.5) loaded at run time by the MSL compiler")
find_path(ALCHEMY_CLANG_C_INCLUDE clang-c/Index.h
    HINTS "${PROJECT_SOURCE_DIR}/artifacts/alchemy-llvm-source/clang/include"
    DOC "Directory containing clang-c/Index.h (LLVM 19.1.5) for the MSL compiler")
file(GLOB alchemy_gui_vulkan_sdks LIST_DIRECTORIES true "C:/VulkanSDK/*")
list(SORT alchemy_gui_vulkan_sdks COMPARE NATURAL ORDER DESCENDING)
set(alchemy_gui_vulkan_hints)
if(DEFINED ENV{VULKAN_SDK})
    list(APPEND alchemy_gui_vulkan_hints "$ENV{VULKAN_SDK}")
endif()
list(APPEND alchemy_gui_vulkan_hints ${alchemy_gui_vulkan_sdks})
set(alchemy_gui_vulkan_include_hints ${alchemy_gui_vulkan_hints})
list(TRANSFORM alchemy_gui_vulkan_include_hints APPEND "/Include")
set(alchemy_gui_vulkan_bin_hints ${alchemy_gui_vulkan_hints})
list(TRANSFORM alchemy_gui_vulkan_bin_hints APPEND "/Bin")
find_path(ALCHEMY_VULKAN_INCLUDE vulkan/vulkan.h HINTS ${alchemy_gui_vulkan_include_hints}
    DOC "Vulkan-Headers include directory (vulkan-1.dll is loaded at run time)")
find_program(ALCHEMY_SPIRV_VAL spirv-val HINTS ${alchemy_gui_vulkan_bin_hints}
    DOC "spirv-val for MSL compiler tests (test-only)")
if(TRANSMUTE_WITH_FX)
    foreach(variable IN ITEMS ALCHEMY_LIBCLANG_DLL ALCHEMY_CLANG_C_INCLUDE ALCHEMY_VULKAN_INCLUDE ALCHEMY_SPIRV_VAL)
        if(${variable})
            message(STATUS "Alchemy GUI: ${variable} = ${${variable}}")
        else()
            message(STATUS "Alchemy GUI: ${variable} not found")
        endif()
    endforeach()
    if(NOT ALCHEMY_CLANG_C_INCLUDE)
        message(STATUS "Alchemy GUI: clang-c headers are fetched with "
            "`git -C artifacts/alchemy-llvm-source sparse-checkout add clang/include/clang-c`")
    endif()
endif()

# ---- sources ----------------------------------------------------------------
set(ALCHEMY_GUI_SOURCES src/alchemy/gui/install.cpp src/alchemy/gui/value_provider.cpp)
set(ALCHEMY_GUI_INCLUDE_DIRECTORIES)
set(ALCHEMY_GUI_COMPILE_DEFINITIONS)
set(ALCHEMY_GUI_LINK_LIBRARIES)
set_property(GLOBAL PROPERTY ALCHEMY_GUI_TEST_FUNCTIONS "")

function(alchemy_gui_register_tests function_name)
    set_property(GLOBAL APPEND PROPERTY ALCHEMY_GUI_TEST_FUNCTIONS "${function_name}")
endfunction()

# Framework test cases (src/alchemy/gui/tests/framework_case.hpp): a framework
# fragment calls alchemy_gui_add_framework_case(<name> <source>); every case is
# compiled into the one executable alchemy_gui_framework_cases, and registered
# as ctest alchemy_gui_<name> (that case alone, in its own process). A source
# that holds several cases is passed once per case name.
set_property(GLOBAL PROPERTY ALCHEMY_GUI_FRAMEWORK_CASES "")
set_property(GLOBAL PROPERTY ALCHEMY_GUI_FRAMEWORK_CASE_SOURCES "")
function(alchemy_gui_add_framework_case name source)
    set_property(GLOBAL APPEND PROPERTY ALCHEMY_GUI_FRAMEWORK_CASES "${name}")
    set_property(GLOBAL APPEND PROPERTY ALCHEMY_GUI_FRAMEWORK_CASE_SOURCES "${source}")
endfunction()
function(alchemy_gui_framework_cases_tests)
    get_property(names GLOBAL PROPERTY ALCHEMY_GUI_FRAMEWORK_CASES)
    get_property(sources GLOBAL PROPERTY ALCHEMY_GUI_FRAMEWORK_CASE_SOURCES)
    list(REMOVE_DUPLICATES sources)   # a source may hold several cases
    if(NOT names)
        return()
    endif()
    alchemy_gui_add_unit_test(alchemy_gui_framework_cases src/alchemy/gui/tests/framework_cases_main.cpp ${sources} TIMEOUT 180)
    foreach(name IN LISTS names)
        add_test(NAME alchemy_gui_${name} COMMAND alchemy_gui_framework_cases)
        set_tests_properties(alchemy_gui_${name} PROPERTIES TIMEOUT 120 ENVIRONMENT "FILL_TEST_ONLY=${name}")
    endforeach()
endfunction()
alchemy_gui_register_tests(alchemy_gui_framework_cases_tests)

foreach(fragment IN ITEMS objc)
    include("${CMAKE_CURRENT_LIST_DIR}/gui/${fragment}.cmake")
endforeach()

# The registration audit (src/alchemy/registration_audit.cpp; integrator-owned):
# duplicate registrations of the full provider set, under a ratchet.
function(alchemy_gui_registration_audit_tests)
    alchemy_gui_add_unit_test(alchemy_gui_registration_audit src/alchemy/gui/tests/registration_audit_tests.cpp TIMEOUT 180)
endfunction()
alchemy_gui_register_tests(alchemy_gui_registration_audit_tests)

# The value-class provider (src/alchemy/gui/value_provider.cpp) and the
# frameworks its row installs; chrome_imports.inc holds every strong import of
# Chrome 155 outside libSystem, which must all resolve.
function(alchemy_gui_value_provider_tests)
    alchemy_gui_add_unit_test(alchemy_gui_value_provider src/alchemy/gui/tests/value_provider_tests.cpp TIMEOUT 180)
endfunction()
alchemy_gui_register_tests(alchemy_gui_value_provider_tests)

# ---- test helpers ---------------------------------------------------------------
function(alchemy_gui_add_unit_test name)
    cmake_parse_arguments(PARSE_ARGV 1 unit "" "TIMEOUT" "")
    if(NOT unit_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "alchemy_gui_add_unit_test(${name}) needs sources")
    endif()
    if(NOT unit_TIMEOUT)
        set(unit_TIMEOUT 60)
    endif()
    add_executable(${name} ${unit_UNPARSED_ARGUMENTS})
    target_link_libraries(${name} PRIVATE alchemy_core_fx)
    target_compile_definitions(${name} PRIVATE ${ALCHEMY_GUI_COMPILE_DEFINITIONS})
    target_include_directories(${name} PRIVATE ${ALCHEMY_GUI_INCLUDE_DIRECTORIES})
    if(MSVC)
        target_compile_options(${name} PRIVATE /W3 /permissive-
            "/FI${PROJECT_SOURCE_DIR}/src/alchemy/fx/alchemy_fx_prelude.hpp")
    endif()
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT ${unit_TIMEOUT})
endfunction()

file(GLOB alchemy_gui_runtime_headers
    "${PROJECT_SOURCE_DIR}/runtime/include/AppKit/*.h" "${PROJECT_SOURCE_DIR}/runtime/include/Metal/*.h"
    "${PROJECT_SOURCE_DIR}/runtime/include/QuartzCore/*.h" "${PROJECT_SOURCE_DIR}/runtime/include/CoreGraphics/*.h"
    "${PROJECT_SOURCE_DIR}/runtime/include/Foundation/*.h" "${PROJECT_SOURCE_DIR}/runtime/include/CoreFoundation/*.h")
set(ALCHEMY_GUI_RUNTIME_HEADERS ${alchemy_gui_runtime_headers} "${PROJECT_SOURCE_DIR}/runtime/objc/AlchemyObjC.h")

function(alchemy_gui_object name source)
    set(language_flags)
    if(source MATCHES "\\.m$")
        list(APPEND language_flags -fno-objc-exceptions)
    endif()
    add_custom_command(OUTPUT "${macos_dir}/${name}.o"
        COMMAND "${ALCHEMY_CLANG}" --target=arm64-apple-macos11
            -march=armv8-a -mcpu=generic -nostdinc -fno-builtin
            -fno-stack-protector -Werror -O1 ${language_flags} ${ARGN}
            -isystem "${personality_resource_dir}/include"
            -I "${PROJECT_SOURCE_DIR}/runtime/include" -c
            "${PROJECT_SOURCE_DIR}/tests/fixtures/macos/${source}"
            -o "${macos_dir}/${name}.o"
        DEPENDS "${PROJECT_SOURCE_DIR}/tests/fixtures/macos/${source}" ${ALCHEMY_GUI_RUNTIME_HEADERS} VERBATIM)
endfunction()

function(alchemy_gui_program name)
    set(objects)
    foreach(object IN LISTS ARGN)
        list(APPEND objects "${macos_dir}/${object}.o")
    endforeach()
    add_custom_command(OUTPUT "${macos_dir}/${name}"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main -rpath @loader_path
            ${objects} ${ALCHEMY_GUI_LINK_STUBS} -o "${macos_dir}/${name}"
        DEPENDS ${objects} ${ALCHEMY_GUI_LINK_STUBS} VERBATIM)
endfunction()

function(alchemy_gui_add_fixture_test name program)
    cmake_parse_arguments(PARSE_ARGV 2 fixture "ALLOW_UNSUPPORTED" "PROGRESS;COMPLETE;TIMEOUT" "ARGS")
    if(NOT fixture_PROGRESS)
        message(FATAL_ERROR "alchemy_gui_add_fixture_test(${name}) needs PROGRESS")
    endif()
    if(NOT fixture_TIMEOUT)
        set(fixture_TIMEOUT 120)
    endif()
    string(REPLACE ";" "\\;" fixture_arguments "${fixture_ARGS}")
    add_test(NAME ${name} COMMAND "${CMAKE_COMMAND}"
        "-DALCHEMY_FX=$<TARGET_FILE:alchemy-fx>" "-DPROGRAM=${program}"
        "-DPROGRESS=${fixture_PROGRESS}" "-DCOMPLETE=${fixture_COMPLETE}"
        "-DALLOW_UNSUPPORTED=${fixture_ALLOW_UNSUPPORTED}" "-DARGUMENTS=${fixture_arguments}"
        -P "${PROJECT_SOURCE_DIR}/tests/alchemy_gui_fixture.cmake")
    set_tests_properties(${name} PROPERTIES TIMEOUT ${fixture_TIMEOUT})
endfunction()

# Link-only framework stubs, built once for every GUI fixture.
function(alchemy_gui_link_stubs)
    alchemy_gui_object(link_appkit link_appkit.m)
    alchemy_gui_object(link_metal link_metal.m)
    alchemy_gui_object(link_quartzcore link_quartzcore.m)
    alchemy_gui_object(link_coregraphics link_coregraphics.c)
    set(frameworks /System/Library/Frameworks)
    set(base "${macos_dir}/link_objc.dylib" "${macos_dir}/link_foundation.dylib")
    foreach(entry IN ITEMS "link_appkit|AppKit.framework/Versions/C/AppKit"
            "link_metal|Metal.framework/Versions/A/Metal"
            "link_quartzcore|QuartzCore.framework/Versions/A/QuartzCore"
            "link_coregraphics|CoreGraphics.framework/Versions/A/CoreGraphics")
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 stub)
        list(GET parts 1 install_name)
        set(libraries ${base})
        if(stub STREQUAL "link_coregraphics")
            set(libraries)   # plain C, no imports
        endif()
        add_custom_command(OUTPUT "${macos_dir}/${stub}.dylib"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib
                -install_name "${frameworks}/${install_name}" "${macos_dir}/${stub}.o" ${libraries}
                -o "${macos_dir}/${stub}.dylib"
            DEPENDS "${macos_dir}/${stub}.o" ${libraries} VERBATIM)
    endforeach()
endfunction()

function(alchemy_gui_register_all_tests)
    if(NOT TARGET alchemy_core_fx OR NOT BUILD_TESTING)
        return()
    endif()
    if(TARGET alchemy_personality_fixtures)
        alchemy_gui_link_stubs()
        set(ALCHEMY_GUI_LINK_STUBS "${macos_dir}/link_appkit.dylib" "${macos_dir}/link_metal.dylib"
            "${macos_dir}/link_quartzcore.dylib" "${macos_dir}/link_coregraphics.dylib"
            "${macos_dir}/link_foundation.dylib" "${macos_dir}/link_core_foundation.dylib"
            "${macos_dir}/link_objc.dylib" "${macos_dir}/link_system.dylib")
        add_custom_target(alchemy_gui_link_stubs DEPENDS "${macos_dir}/link_appkit.dylib" "${macos_dir}/link_metal.dylib"
            "${macos_dir}/link_quartzcore.dylib" "${macos_dir}/link_coregraphics.dylib")
        add_dependencies(alchemy_gui_link_stubs alchemy_personality_fixtures)
        set(ALCHEMY_GUI_FIXTURES_ENABLED ON)
    else()
        set(ALCHEMY_GUI_LINK_STUBS)
        set(ALCHEMY_GUI_FIXTURES_ENABLED OFF)
        message(STATUS "Alchemy GUI fixtures disabled: the macOS fixture pipeline is unavailable")
    endif()
    get_property(functions GLOBAL PROPERTY ALCHEMY_GUI_TEST_FUNCTIONS)
    foreach(function_name IN LISTS functions)
        cmake_language(CALL ${function_name})
    endforeach()
endfunction()
cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL alchemy_gui_register_all_tests)
