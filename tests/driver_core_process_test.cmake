# Native validation for the public core/process package.
#
# The test launches exact host executables and therefore runs only under the
# matching native target selected by the enclosing CTest configuration. A
# copied one-package workspace keeps validation evidence and native artifacts
# below the CMake binary directory rather than changing the source checkout.

if(NOT DEFINED DRAFTC OR
   NOT DEFINED SOURCE_ROOT OR
   NOT DEFINED TEST_ROOT OR
   NOT DEFINED TARGET_SELECTOR)
  message(FATAL_ERROR
    "DRAFTC, SOURCE_ROOT, TEST_ROOT, and TARGET_SELECTOR are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/workspace/process")
file(WRITE "${TEST_ROOT}/workspace/draft.workspace" "draft-workspace-v1\n")
file(COPY "${SOURCE_ROOT}/core/process/"
  DESTINATION "${TEST_ROOT}/workspace/process"
  FILES_MATCHING PATTERN "*.draft")

execute_process(
  COMMAND "${DRAFTC}" test "${TEST_ROOT}/workspace/process"
    --target "${TARGET_SELECTOR}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE standard_output
  ERROR_VARIABLE standard_error
)
if(NOT status EQUAL 0 OR
   NOT standard_output MATCHES "test passed: 1 selected procedures")
  message(FATAL_ERROR
    "core/process tests failed (${status})\n"
    "stdout:\n${standard_output}\nstderr:\n${standard_error}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
