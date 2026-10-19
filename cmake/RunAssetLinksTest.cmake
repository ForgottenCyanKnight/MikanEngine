# The production regression requires a fresh directory on every invocation.
if(NOT DEFINED TEST_EXECUTABLE OR NOT DEFINED FIXTURE_DIR)
    message(FATAL_ERROR "TEST_EXECUTABLE and FIXTURE_DIR are required")
endif()
file(REMOVE_RECURSE "${FIXTURE_DIR}")
execute_process(COMMAND "${TEST_EXECUTABLE}" "${FIXTURE_DIR}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Project asset link regression failed: ${result}")
endif()
