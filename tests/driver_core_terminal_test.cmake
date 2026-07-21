# Native validation for the public core/terminal package.
#
# The Draft test command owns evidence below its workspace. Copying the package
# into this process-unique CMake binary directory keeps simultaneous worktrees
# and source checkouts clean while imports continue to use draftc's configured
# core distribution. The test selects the host's exact AArch64 profile, which
# also links every target-qualified termios and poll declaration against the
# actual platform libc without mutating the runner's terminal mode.

if(NOT DEFINED DRAFTC OR
   NOT DEFINED SOURCE_ROOT OR
   NOT DEFINED TEST_ROOT OR
   NOT DEFINED TARGET_SELECTOR)
  message(FATAL_ERROR
    "DRAFTC, SOURCE_ROOT, TEST_ROOT, and TARGET_SELECTOR are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/workspace/terminal")
file(COPY "${SOURCE_ROOT}/core/terminal/"
  DESTINATION "${TEST_ROOT}/workspace/terminal"
  FILES_MATCHING PATTERN "*.draft")

execute_process(
  COMMAND "${DRAFTC}" test "${TEST_ROOT}/workspace" --root terminal
    --target "${TARGET_SELECTOR}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE standard_output
  ERROR_VARIABLE standard_error
)
if(NOT status EQUAL 0 OR
   NOT standard_output MATCHES "test passed: 4 selected procedures")
  message(FATAL_ERROR
    "core/terminal tests failed (${status})\n"
    "stdout:\n${standard_output}\nstderr:\n${standard_error}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
