# Capture the native oracle without relying on shell redirection or quoting.
execute_process(COMMAND "${PROGRAM}" OUTPUT_FILE "${OUTPUT}" RESULT_VARIABLE result)
if (NOT result EQUAL 0)
    file(REMOVE "${OUTPUT}")
    message(FATAL_ERROR "The native demo reference failed: ${result}")
endif()
