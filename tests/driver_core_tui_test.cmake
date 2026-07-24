# Native validation for the public core/tui package.
#
# The Draft test command owns evidence below its workspace. Copying the package
# into this process-unique CMake binary directory keeps simultaneous worktrees
# and source checkouts clean while imports continue to use draftc's configured
# core distribution. Tests inspect deterministic render bytes in memory and do
# not write ANSI control sequences to CTest's terminal.

if(NOT DEFINED DRAFTC OR
   NOT DEFINED SOURCE_ROOT OR
   NOT DEFINED TEST_ROOT OR
   NOT DEFINED TARGET_SELECTOR)
  message(FATAL_ERROR
    "DRAFTC, SOURCE_ROOT, TEST_ROOT, and TARGET_SELECTOR are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/workspace/tui")
file(WRITE "${TEST_ROOT}/workspace/draft.workspace" "draft-workspace-v1\n")
file(COPY "${SOURCE_ROOT}/core/tui/"
  DESTINATION "${TEST_ROOT}/workspace/tui"
  FILES_MATCHING PATTERN "*.draft")

execute_process(
  COMMAND "${DRAFTC}" test "${TEST_ROOT}/workspace/tui"
    --target "${TARGET_SELECTOR}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE standard_output
  ERROR_VARIABLE standard_error
)
# Validation discovery includes three tui tests, core/unicode's four tests, and
# core/terminal's five tests. Keeping the total exact proves all three layers
# compose in the same selected native graph.
if(NOT status EQUAL 0 OR
   NOT standard_output MATCHES "test passed: 12 selected procedures")
  message(FATAL_ERROR
    "core/tui tests failed (${status})\n"
    "stdout:\n${standard_output}\nstderr:\n${standard_error}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
