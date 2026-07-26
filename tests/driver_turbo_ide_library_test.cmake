# Native validation for the reusable Draft package, Turbo UI, and editor
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
file(WRITE "${TEST_ROOT}/workspace/draft.workspace" "draft-workspace-v1\n")

execute_process(
  COMMAND "${DRAFTC}" test "${TEST_ROOT}/workspace/lib/turbo_ui"
    --target "${TARGET_SELECTOR}"
  RESULT_VARIABLE ui_status
  OUTPUT_VARIABLE ui_output
  ERROR_VARIABLE ui_error
)
if(NOT ui_status EQUAL 0 OR
   NOT ui_output MATCHES "test passed: 26 selected procedures")
  message(FATAL_ERROR
    "Turbo UI tests failed (${ui_status})\n"
    "stdout:\n${ui_output}\nstderr:\n${ui_error}")
endif()

execute_process(
  COMMAND "${DRAFTC}" test "${TEST_ROOT}/workspace/lib/turbo_editor_app"
    --target "${TARGET_SELECTOR}"
  RESULT_VARIABLE editor_status
  OUTPUT_VARIABLE editor_output
  ERROR_VARIABLE editor_error
)
if(NOT editor_status EQUAL 0 OR
   NOT editor_output MATCHES "test passed: 64 selected procedures")
  message(FATAL_ERROR
    "Turbo IDE library tests failed (${editor_status})\n"
    "stdout:\n${editor_output}\nstderr:\n${editor_error}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
