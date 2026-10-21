# Runs a Swift fixture under alchemy-fx with the given guest runtime
# directories and compares its exact output with the expected file.
if(NOT DEFINED ALCHEMY OR NOT DEFINED PROGRAM OR NOT DEFINED EXPECTED)
    message(FATAL_ERROR "ALCHEMY, PROGRAM and EXPECTED are required")
endif()
set(arguments)
foreach(directory IN LISTS RUNTIME_DIRS)
    list(APPEND arguments --runtime-dir "${directory}")
endforeach()
execute_process(COMMAND "${ALCHEMY}" ${arguments} "${PROGRAM}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 100
    # The fixture prints UTF-8; AUTO would decode it with the console's code page.
    ENCODING UTF8)
file(READ "${EXPECTED}" expected)
string(REPLACE "\r\n" "\n" expected "${expected}")
if(NOT result STREQUAL "0" OR NOT output STREQUAL expected OR NOT errors STREQUAL "")
    message(FATAL_ERROR "exit=${result}\nstdout=[${output}]\nexpected=[${expected}]\nstderr=[${errors}]")
endif()
