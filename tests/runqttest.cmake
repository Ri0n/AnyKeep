if(NOT DEFINED ANYKEEP_TEST_EXECUTABLE OR ANYKEEP_TEST_EXECUTABLE STREQUAL "")
  message(FATAL_ERROR "ANYKEEP_TEST_EXECUTABLE is required")
endif()
if(NOT DEFINED ANYKEEP_TEST_TEXT_REPORT OR ANYKEEP_TEST_TEXT_REPORT STREQUAL "")
  message(FATAL_ERROR "ANYKEEP_TEST_TEXT_REPORT is required")
endif()
if(NOT DEFINED ANYKEEP_TEST_JUNIT_REPORT OR ANYKEEP_TEST_JUNIT_REPORT STREQUAL "")
  message(FATAL_ERROR "ANYKEEP_TEST_JUNIT_REPORT is required")
endif()

get_filename_component(anykeep_test_report_directory "${ANYKEEP_TEST_TEXT_REPORT}" DIRECTORY)
file(MAKE_DIRECTORY "${anykeep_test_report_directory}")
file(REMOVE "${ANYKEEP_TEST_TEXT_REPORT}" "${ANYKEEP_TEST_JUNIT_REPORT}")

execute_process(COMMAND "${ANYKEEP_TEST_EXECUTABLE}" -v1 -o "${ANYKEEP_TEST_TEXT_REPORT},txt" -o
                        "${ANYKEEP_TEST_JUNIT_REPORT},junitxml" RESULT_VARIABLE anykeep_test_result)

if(EXISTS "${ANYKEEP_TEST_TEXT_REPORT}")
  file(READ "${ANYKEEP_TEST_TEXT_REPORT}" anykeep_test_output)
  message("${anykeep_test_output}")
else()
  message("QtTest did not create ${ANYKEEP_TEST_TEXT_REPORT}")
endif()

if(NOT "${anykeep_test_result}" STREQUAL "0")
  message(FATAL_ERROR "QtTest process failed with result: ${anykeep_test_result}")
endif()
