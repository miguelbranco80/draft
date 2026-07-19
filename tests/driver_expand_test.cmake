# Process-level contract for the read-only expanded-source command.
#
# The C++ projection test owns format and transactional edge cases. This small
# driver test proves public option parsing reaches that implementation, saved
# generated fragments are applied without Codex, and a second invocation cannot
# overwrite an existing projection. TEST_ROOT is build-tree state owned and
# removed by this script, so the source fixture remains immutable.

if(NOT DEFINED DRAFTC OR NOT DEFINED SOURCE_PACKAGE OR NOT DEFINED TEST_ROOT)
  message(FATAL_ERROR "DRAFTC, SOURCE_PACKAGE, and TEST_ROOT are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}" "${TEST_ROOT}.tmp")
execute_process(
  COMMAND "${DRAFTC}" expand "${SOURCE_PACKAGE}" --out "${TEST_ROOT}"
  RESULT_VARIABLE first_status
  OUTPUT_VARIABLE first_stdout
  ERROR_VARIABLE first_stderr
)
if(NOT first_status EQUAL 0)
  message(FATAL_ERROR
    "expand failed (${first_status})\nstdout:\n${first_stdout}\nstderr:\n${first_stderr}")
endif()
if(NOT first_stdout MATCHES "expanded [0-9]+ source files")
  message(FATAL_ERROR "expand did not report its output: ${first_stdout}")
endif()
if(NOT EXISTS "${TEST_ROOT}/draft-expanded-source.map")
  message(FATAL_ERROR "expand did not write the root manifest")
endif()

execute_process(
  COMMAND "${DRAFTC}" expand "${SOURCE_PACKAGE}" --out "${TEST_ROOT}"
  RESULT_VARIABLE second_status
  OUTPUT_VARIABLE second_stdout
  ERROR_VARIABLE second_stderr
)
if(second_status EQUAL 0)
  message(FATAL_ERROR "expand unexpectedly replaced an existing projection")
endif()
if(NOT second_stderr MATCHES "output directory already exists")
  message(FATAL_ERROR "expand reported the wrong collision: ${second_stderr}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}" "${TEST_ROOT}.tmp")
