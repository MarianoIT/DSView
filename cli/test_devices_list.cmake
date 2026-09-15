if(NOT DEFINED CLI)
    message(FATAL_ERROR "CLI executable path is required")
endif()

execute_process(
    COMMAND "${CLI}" devices list
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)

if(NOT result EQUAL 0)
    message(FATAL_ERROR "devices list failed: ${error}")
endif()

string(STRIP "${output}" output)
if(NOT output MATCHES "^\\{\"ok\":true,\"devices\":\\[.*\\]\\}$")
    message(FATAL_ERROR "devices list did not return valid CLI JSON: ${output}")
endif()
