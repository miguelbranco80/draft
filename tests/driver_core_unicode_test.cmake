# Native validation for the target-independent core/unicode package.
#
# Unicode tests execute the real Draft runtime but never consult host locale or
# terminal state. Copying the package into process-unique CMake storage keeps
# evidence and native objects out of the source checkout while imports continue
# to use draftc's configured core distribution.

if(NOT DEFINED DRAFTC OR
   NOT DEFINED SOURCE_ROOT OR
   NOT DEFINED TEST_ROOT OR
   NOT DEFINED TARGET_SELECTOR)
  message(FATAL_ERROR
    "DRAFTC, SOURCE_ROOT, TEST_ROOT, and TARGET_SELECTOR are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/workspace/unicode")
file(COPY "${SOURCE_ROOT}/core/unicode/"
  DESTINATION "${TEST_ROOT}/workspace/unicode"
  FILES_MATCHING PATTERN "*.draft")

execute_process(
  COMMAND "${DRAFTC}" test "${TEST_ROOT}/workspace" --root unicode
    --target "${TARGET_SELECTOR}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE standard_output
  ERROR_VARIABLE standard_error
)
if(NOT status EQUAL 0 OR
   NOT standard_output MATCHES "test passed: 4 selected procedures")
  message(FATAL_ERROR
    "core/unicode tests failed (${status})\n"
    "stdout:\n${standard_output}\nstderr:\n${standard_error}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
