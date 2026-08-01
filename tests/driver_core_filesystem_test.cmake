# Native validation for the public core/filesystem package.
#
# Canonicalization and enumeration act on the process filesystem. The copied
# package and explicit working directory keep `.` deterministic enough for the
# API invariants while validation evidence and native artifacts remain below
# the CMake binary directory.

if(NOT DEFINED DRAFTC OR
   NOT DEFINED SOURCE_ROOT OR
   NOT DEFINED TEST_ROOT OR
   NOT DEFINED TARGET_SELECTOR)
  message(FATAL_ERROR
    "DRAFTC, SOURCE_ROOT, TEST_ROOT, and TARGET_SELECTOR are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/workspace/filesystem")
file(WRITE "${TEST_ROOT}/workspace/draft.workspace" "draft-workspace-v1\n")
file(COPY "${SOURCE_ROOT}/core/filesystem/"
  DESTINATION "${TEST_ROOT}/workspace/filesystem"
  FILES_MATCHING PATTERN "*.draft")
file(MAKE_DIRECTORY "${TEST_ROOT}/workspace/canonicalize-target")
if(UNIX)
  file(CREATE_LINK
    "canonicalize-target"
    "${TEST_ROOT}/workspace/canonicalize-link"
    SYMBOLIC
    RESULT link_result)
  if(NOT link_result STREQUAL "0")
    message(FATAL_ERROR "cannot create canonicalization symlink: ${link_result}")
  endif()
endif()

execute_process(
  COMMAND "${DRAFTC}" test "${TEST_ROOT}/workspace/filesystem"
    --target "${TARGET_SELECTOR}"
  WORKING_DIRECTORY "${TEST_ROOT}/workspace"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE standard_output
  ERROR_VARIABLE standard_error
)
if(NOT status EQUAL 0 OR
   NOT standard_output MATCHES "test passed: 5 selected procedures")
  message(FATAL_ERROR
    "core/filesystem tests failed (${status})\n"
    "stdout:\n${standard_output}\nstderr:\n${standard_error}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
