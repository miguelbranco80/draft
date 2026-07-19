# Source-tree-safe process test for public address instrumentation.
#
# `draftc test` intentionally records evidence beside its package. CTest must
# therefore run the command against a private copy rather than mutate the
# checked-in example. TEST_ROOT lives in the build tree and is recreated for
# every invocation; a failed command may leave only disposable diagnostic state
# there, never a source-tree `.draft` directory.

if(NOT DEFINED DRAFTC OR NOT DEFINED SOURCE_PACKAGE OR NOT DEFINED TEST_ROOT)
  message(FATAL_ERROR "DRAFTC, SOURCE_PACKAGE, and TEST_ROOT are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(COPY "${SOURCE_PACKAGE}" DESTINATION "${TEST_ROOT}")
get_filename_component(package_name "${SOURCE_PACKAGE}" NAME)

execute_process(
  COMMAND "${DRAFTC}" test "${TEST_ROOT}/${package_name}"
    --instrument address
  RESULT_VARIABLE test_status
  OUTPUT_VARIABLE test_stdout
  ERROR_VARIABLE test_stderr
)
if(NOT test_status EQUAL 0)
  message(FATAL_ERROR
    "address instrumentation failed (${test_status})\nstdout:\n${test_stdout}\nstderr:\n${test_stderr}")
endif()
if(NOT test_stdout MATCHES "test passed")
  message(FATAL_ERROR
    "address instrumentation lost its success report: ${test_stdout}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
