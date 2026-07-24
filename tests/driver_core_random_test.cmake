# Native validation for the public core/random package.
#
# Tests replace the active Context provider with deterministic local callbacks,
# so they exercise forwarding and failure without depending on OS entropy. The
# copied workspace keeps evidence and native objects out of the source checkout.

if(NOT DEFINED DRAFTC OR
   NOT DEFINED SOURCE_ROOT OR
   NOT DEFINED TEST_ROOT OR
   NOT DEFINED TARGET_SELECTOR)
  message(FATAL_ERROR
    "DRAFTC, SOURCE_ROOT, TEST_ROOT, and TARGET_SELECTOR are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/workspace/random")
file(WRITE "${TEST_ROOT}/workspace/draft.workspace" "draft-workspace-v1\n")
file(COPY "${SOURCE_ROOT}/core/random/"
  DESTINATION "${TEST_ROOT}/workspace/random"
  FILES_MATCHING PATTERN "*.draft")

execute_process(
  COMMAND "${DRAFTC}" test "${TEST_ROOT}/workspace/random"
    --target "${TARGET_SELECTOR}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE standard_output
  ERROR_VARIABLE standard_error
)
if(NOT status EQUAL 0 OR
   NOT standard_output MATCHES "test passed: 3 selected procedures")
  message(FATAL_ERROR
    "core/random tests failed (${status})\n"
    "stdout:\n${standard_output}\nstderr:\n${standard_error}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
