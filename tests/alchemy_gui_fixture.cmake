# Runs a GUI fixture under alchemy-fx (see alchemy_gui_add_fixture_test in
# cmake/AlchemyGui.cmake). Inputs: ALCHEMY_FX, PROGRAM, PROGRESS (regex stdout
# must match), COMPLETE (regex stdout must match on exit 0), ALLOW_UNSUPPORTED
# (accept a nonzero exit whose stderr names an explicit "unsupported" stop),
# ARGUMENTS (guest arguments).
if(NOT DEFINED ALCHEMY_FX OR NOT DEFINED PROGRAM OR NOT DEFINED PROGRESS)
    message(FATAL_ERROR "ALCHEMY_FX, PROGRAM and PROGRESS are required")
endif()
# alchemy_gui_add_fixture_test escapes the list separators of ARGS ("\;")
# to keep them out of add_test's own list; restore the list here.
string(REPLACE "\\;" ";" ARGUMENTS "${ARGUMENTS}")
execute_process(COMMAND "${ALCHEMY_FX}" "${PROGRAM}" ${ARGUMENTS}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 110)
message(STATUS "exit: ${result}\nstdout:\n${output}\nstderr:\n${errors}")
if(NOT output MATCHES "${PROGRESS}")
    message(FATAL_ERROR "fixture did not reach its progress marker: ${PROGRESS}")
endif()
if(result EQUAL 0)
    if(COMPLETE AND NOT output MATCHES "${COMPLETE}")
        message(FATAL_ERROR "fixture exited 0 without its completion marker: ${COMPLETE}")
    endif()
elseif(ALLOW_UNSUPPORTED AND errors MATCHES "unsupported")
    string(REGEX MATCH "[^\n]*unsupported[^\n]*" stop "${errors}")
    message(STATUS "fixture stopped explicitly: ${stop}")
else()
    message(FATAL_ERROR "fixture failed (exit ${result})")
endif()
