# MSL -> SPIR-V compiler. Owner: A6 (src/alchemy/darwin/msl/, runtime/msl/).
# Contract: include/alchemy/msl/compiler.hpp. The front end is the pinned
# clang.exe (ALCHEMY_CLANG, LLVM 19.1.5) run as a child process; ALCHEMY_MSL_CLANG
# overrides it at run time. runtime/msl (metal_stdlib) is embedded.
set(alchemy_msl_embed "${CMAKE_BINARY_DIR}/alchemy_msl/msl_stdlib_embed.cpp")
file(GLOB alchemy_msl_stdlib_inputs CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/runtime/msl/metal_stdlib" "${PROJECT_SOURCE_DIR}/runtime/msl/__msl/*")
# Generated at configure time (editing the stdlib re-runs CMake), so the file is
# never produced by two targets' build rules at once.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${alchemy_msl_stdlib_inputs}
    "${PROJECT_SOURCE_DIR}/src/alchemy/darwin/msl/embed_stdlib.cmake")
function(alchemy_msl_embed_stdlib)
    set(SOURCE_DIR "${PROJECT_SOURCE_DIR}/runtime/msl")
    set(OUTPUT "${alchemy_msl_embed}")
    include("${PROJECT_SOURCE_DIR}/src/alchemy/darwin/msl/embed_stdlib.cmake")
endfunction()
alchemy_msl_embed_stdlib()
set(ALCHEMY_MSL_SOURCES
    src/alchemy/darwin/msl/compiler.cpp src/alchemy/darwin/msl/dump.cpp src/alchemy/darwin/msl/frontend.cpp
    src/alchemy/darwin/msl/rewriter.cpp src/alchemy/darwin/msl/llvm_ir.cpp src/alchemy/darwin/msl/spirv_builder.cpp
    src/alchemy/darwin/msl/lower.cpp src/alchemy/darwin/msl/structurize.cpp src/alchemy/darwin/msl/cache.cpp
    src/alchemy/darwin/msl/preprocess.cpp src/alchemy/darwin/msl/prelude.cpp
    "${alchemy_msl_embed}")
list(APPEND ALCHEMY_GUI_SOURCES ${ALCHEMY_MSL_SOURCES})
set(ALCHEMY_MSL_DEFINITIONS)
if(ALCHEMY_CLANG)
    list(APPEND ALCHEMY_MSL_DEFINITIONS "ALCHEMY_MSL_CLANG_DEFAULT_PATH=\"${ALCHEMY_CLANG}\"")
endif()
list(APPEND ALCHEMY_GUI_COMPILE_DEFINITIONS ${ALCHEMY_MSL_DEFINITIONS})
list(APPEND ALCHEMY_GUI_INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src/alchemy/darwin/msl")

function(alchemy_gui_msl_tests)
    # The MSL tests link the compiler alone (a static copy of the same
    # sources), not all of alchemy_core_fx; alchemy_core gives it the host
    # layer and perf.
    add_library(alchemy_msl_test_lib STATIC ${ALCHEMY_MSL_SOURCES})
    target_link_libraries(alchemy_msl_test_lib PUBLIC alchemy_core)
    alchemy_hash_map_options(alchemy_msl_test_lib)
    target_include_directories(alchemy_msl_test_lib PUBLIC "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src/alchemy/darwin/msl")
    target_compile_definitions(alchemy_msl_test_lib PUBLIC ${ALCHEMY_MSL_DEFINITIONS})
    target_compile_features(alchemy_msl_test_lib PUBLIC cxx_std_20)
    if(MSVC)
        target_compile_options(alchemy_msl_test_lib PRIVATE /W3 /permissive- /utf-8 /bigobj)
    endif()
    add_executable(alchemy_msl_tests src/alchemy/darwin/msl/tests/msl_tests.cpp src/alchemy/darwin/msl/tests/msl_vulkan_test.cpp)
    target_link_libraries(alchemy_msl_tests PRIVATE alchemy_msl_test_lib)
    if(ALCHEMY_SPIRV_VAL)
        target_compile_definitions(alchemy_msl_tests PRIVATE "ALCHEMY_SPIRV_VAL=\"${ALCHEMY_SPIRV_VAL}\"")
    endif()
    # The dumped application shader corpus (ALCHEMY_MSL_DUMP output) lives
    # outside the repository: Blender's generated MSL is GPL. Empty or missing
    # makes the corpus group skip with a message.
    set(alchemy_msl_corpus_default "")
    if(EXISTS "${PROJECT_SOURCE_DIR}/artifacts/alchemy-blender/msl-corpus")
        set(alchemy_msl_corpus_default "${PROJECT_SOURCE_DIR}/artifacts/alchemy-blender/msl-corpus")
    endif()
    set(ALCHEMY_MSL_CORPUS "${alchemy_msl_corpus_default}" CACHE PATH
        "Directory of dumped MSL libraries (.metal + .options) for the alchemy_msl_corpus test")
    if(ALCHEMY_MSL_CORPUS)
        target_compile_definitions(alchemy_msl_tests PRIVATE "ALCHEMY_MSL_CORPUS_DIR=\"${ALCHEMY_MSL_CORPUS}\"")
    endif()
    # Chrome's dumped MSL (Dawn/Tint, Skia Graphite, ANGLE), one ALCHEMY_MSL_DUMP
    # directory per run below this root; never in the repository either.
    set(alchemy_msl_chrome_corpus_default "")
    if(EXISTS "${PROJECT_SOURCE_DIR}/artifacts/chrome-msl-corpus")
        set(alchemy_msl_chrome_corpus_default "${PROJECT_SOURCE_DIR}/artifacts/chrome-msl-corpus")
    endif()
    set(ALCHEMY_MSL_CHROME_CORPUS "${alchemy_msl_chrome_corpus_default}" CACHE PATH
        "Root of Chrome's dumped MSL (one ALCHEMY_MSL_DUMP directory per run) for the alchemy_msl_chrome_corpus test")
    if(ALCHEMY_MSL_CHROME_CORPUS)
        target_compile_definitions(alchemy_msl_tests PRIVATE
            "ALCHEMY_MSL_CHROME_CORPUS_DIR=\"${ALCHEMY_MSL_CHROME_CORPUS}\"")
    endif()
    if(ALCHEMY_VULKAN_INCLUDE)
        target_include_directories(alchemy_msl_tests PRIVATE "${ALCHEMY_VULKAN_INCLUDE}")
        target_compile_definitions(alchemy_msl_tests PRIVATE ALCHEMY_MSL_HAVE_VULKAN_HEADERS=1)
    endif()
    if(MSVC)
        target_compile_options(alchemy_msl_tests PRIVATE /W3 /permissive- /utf-8)
    endif()
    foreach(group IN ITEMS rewriter preprocessor fixtures reflection link cache dump corpus chrome_corpus vulkan)
        add_test(NAME alchemy_msl_${group} COMMAND alchemy_msl_tests ${group})
        # The fixture directory goes through the environment, not a compile
        # definition (see fixtures_directory() in msl_test_support.hpp).
        set_tests_properties(alchemy_msl_${group} PROPERTIES TIMEOUT 600
            ENVIRONMENT "ALCHEMY_MSL_FIXTURES=${PROJECT_SOURCE_DIR}/src/alchemy/darwin/msl/tests/fixtures")
    endforeach()
    alchemy_gui_add_unit_test(alchemy_msl_smoke src/alchemy/darwin/msl/tests/msl_smoke_tests.cpp)
endfunction()
alchemy_gui_register_tests(alchemy_gui_msl_tests)
