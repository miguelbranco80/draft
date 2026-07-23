# Native validation for the reusable Draft project, Turbo UI, and editor
# packages. The copied workspace preserves normal `lib/...` import identities
# while keeping validation evidence and native artifacts out of the checkout.

if(NOT DEFINED DRAFTC OR
   NOT DEFINED SOURCE_ROOT OR
   NOT DEFINED TEST_ROOT OR
   NOT DEFINED TARGET_SELECTOR)
  message(FATAL_ERROR
    "DRAFTC, SOURCE_ROOT, TEST_ROOT, and TARGET_SELECTOR are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/workspace")
file(COPY "${SOURCE_ROOT}/lib"
  DESTINATION "${TEST_ROOT}/workspace"
  FILES_MATCHING PATTERN "*.draft")

execute_process(
  COMMAND "${DRAFTC}" test "${TEST_ROOT}/workspace"
    --root lib/draft_project --target "${TARGET_SELECTOR}"
  RESULT_VARIABLE project_status
  OUTPUT_VARIABLE project_output
  ERROR_VARIABLE project_error
)
if(NOT project_status EQUAL 0 OR
   NOT project_output MATCHES "test passed: 3 selected procedures")
  message(FATAL_ERROR
    "draft_project tests failed (${project_status})\n"
    "stdout:\n${project_output}\nstderr:\n${project_error}")
endif()

execute_process(
  COMMAND "${DRAFTC}" test "${TEST_ROOT}/workspace"
    --root lib/turbo_editor_app --target "${TARGET_SELECTOR}"
  RESULT_VARIABLE editor_status
  OUTPUT_VARIABLE editor_output
  ERROR_VARIABLE editor_error
)
if(NOT editor_status EQUAL 0 OR
   NOT editor_output MATCHES "test passed: 28 selected procedures")
  message(FATAL_ERROR
    "Turbo IDE library tests failed (${editor_status})\n"
    "stdout:\n${editor_output}\nstderr:\n${editor_error}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
