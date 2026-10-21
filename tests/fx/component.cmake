# Owner: corpus/bench. Define this component's targets with fx_add_library / fx_add_test.
#
# The ARM64 corpus and benchmarks are cross-compiled by PowerShell scripts
# (clang-cl + lld-link with the in-tree ARM64 CRT in tests/fx/corpus/crt), so
# these targets only drive the scripts; none is part of ALL.
#   fx_corpus_build  build tests/fx/corpus for ARM64 and x64
#   fx_corpus        build and run it natively and under transmute-fx
#   fx_bench_build   build tests/fx/bench for ARM64 and x64
#   fx_bench         run the benchmarks (writes artifacts/fx-bench/*.json)
# -DTRANSMUTE_FX_CORPUS_TESTS=ON also registers the corpus as a CTest test.

find_program(FX_POWERSHELL NAMES powershell pwsh)
option(TRANSMUTE_FX_CORPUS_TESTS "Register the fx end-to-end corpus as a CTest test" OFF)
set(TRANSMUTE_FX_CPYTHON_EXECUTABLE "" CACHE FILEPATH "Optional ARM64 CPython for dictionary iteration regression")
if(TRANSMUTE_FX_CPYTHON_EXECUTABLE)
    add_test(NAME fx_python_dict_iteration
        COMMAND $<TARGET_FILE:transmute-fx> --no-cache ${TRANSMUTE_FX_CPYTHON_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/fixtures/python_dict_iteration.py)
    set_tests_properties(fx_python_dict_iteration PROPERTIES TIMEOUT 60)
endif()

if(FX_POWERSHELL)
    set(FX_PS ${FX_POWERSHELL} -NoProfile -ExecutionPolicy Bypass -File)
    # transmute-fx from this build tree when it exists; otherwise the scripts
    # pick the newest build-fx*/Release/transmute-fx.exe or run native only.
    set(FX_ENGINE_ARG -FxExe ${CMAKE_BINARY_DIR}/$<CONFIG>/transmute-fx.exe)

    add_custom_target(fx_corpus_build
        COMMAND ${FX_PS} ${PROJECT_SOURCE_DIR}/scripts/fx_corpus.ps1 -BuildOnly
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        USES_TERMINAL VERBATIM)
    add_custom_target(fx_corpus
        COMMAND ${FX_PS} ${PROJECT_SOURCE_DIR}/scripts/fx_corpus.ps1 ${FX_ENGINE_ARG}
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        USES_TERMINAL VERBATIM)
    add_custom_target(fx_bench_build
        COMMAND ${FX_PS} ${PROJECT_SOURCE_DIR}/tests/fx/bench/build.ps1
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        USES_TERMINAL VERBATIM)
    add_custom_target(fx_bench
        COMMAND ${FX_PS} ${PROJECT_SOURCE_DIR}/scripts/fx_bench.ps1 ${FX_ENGINE_ARG}
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        USES_TERMINAL VERBATIM)

    if(TRANSMUTE_FX_CORPUS_TESTS)
        add_test(NAME fx_corpus
            COMMAND ${FX_PS} ${PROJECT_SOURCE_DIR}/scripts/fx_corpus.ps1 ${FX_ENGINE_ARG}
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
        set_tests_properties(fx_corpus PROPERTIES TIMEOUT 1800 LABELS fx_corpus)
    endif()
endif()
