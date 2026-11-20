file(READ "${EXPECTED_OUTPUT}" expected)
string(REPLACE "\r\n" "\n" expected "${expected}")

foreach(run RANGE 1 2)
    execute_process(COMMAND "${DEMO}" ENCODING UTF-8
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(NOT status STREQUAL "0" OR NOT errors STREQUAL "")
        message(FATAL_ERROR "S08 demo run ${run} failed (status ${status}): ${errors}")
    endif()
    string(REPLACE "\r\n" "\n" output "${output}")
    if(NOT output STREQUAL expected)
        message(FATAL_ERROR "S08 demo run ${run} output differs from its fixture.\nExpected:\n${expected}\nActual:\n${output}")
    endif()
endforeach()
