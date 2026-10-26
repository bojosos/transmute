if(NOT DEFINED ALCHEMY OR NOT DEFINED PROGRAM)
    message(FATAL_ERROR "ALCHEMY and PROGRAM required")
endif()
# The fixture's protocol metadata loads. Objective-C exceptions are thrown through the guest
# libc++abi, which this fixture does not load: doctor accepts the import, and the throw stops
# with a diagnostic naming the missing runtime.
execute_process(COMMAND "${ALCHEMY}" doctor --deps --json "${PROGRAM}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
if(NOT errors STREQUAL "")
    message(FATAL_ERROR "doctor failed: ${result}: ${errors}")
endif()
string(JSON verified GET "${output}" execution_verified)
string(JSON complete GET "${output}" providers 0 complete)
if(verified OR complete OR output MATCHES "unsupported Objective-C exceptions" OR
        output MATCHES "unsupported Objective-C metadata section")
    message(FATAL_ERROR "doctor misreported protocol metadata or exceptions: ${output}")
endif()
foreach(mode IN ITEMS interpreter jit)
    set(arguments)
    if(mode STREQUAL "jit")
        list(APPEND arguments --jit)
    endif()
    execute_process(COMMAND "${ALCHEMY}" ${arguments} "${PROGRAM}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
    if(NOT result EQUAL 1 OR NOT output STREQUAL "Objective-C protocols: 1 1 1 27\n" OR
            NOT errors MATCHES "Objective-C exceptions need libc\\+\\+abi")
        message(FATAL_ERROR "protocols failed or exceptions ran in ${mode}: ${result}: ${output}: ${errors}")
    endif()
endforeach()
