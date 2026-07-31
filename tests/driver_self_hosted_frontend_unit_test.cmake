# Isolated native test runner for the Draft-written frontend packages.
#
# `draftc test` stores native artifacts and content-addressed evidence below the
# selected workspace. Copying compiler sources into process-unique CMake binary
# storage keeps the checkout clean and lets simultaneous worktrees run this
# test without sharing state. Core imports continue to use draftc's embedded
# distribution; all sibling compiler packages are copied together.

foreach(required DRAFTC SOURCE_ROOT TEST_ROOT TARGET_SELECTOR)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "missing required -D${required}=...")
  endif()
endforeach()

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef run_identity)
set(run_root "${TEST_ROOT}/${run_identity}")
set(workspace "${run_root}/workspace")
file(MAKE_DIRECTORY "${workspace}")
file(WRITE "${workspace}/draft.workspace" "draft-workspace-v1\n")
file(COPY "${SOURCE_ROOT}/compiler"
  DESTINATION "${workspace}"
  FILES_MATCHING PATTERN "*.draft")

execute_process(
  COMMAND "${DRAFTC}" test "${workspace}/compiler/syntax"
    --target "${TARGET_SELECTOR}" -O2
  RESULT_VARIABLE status
  OUTPUT_VARIABLE standard_output
  ERROR_VARIABLE standard_error
)
if(NOT status EQUAL 0 OR
   NOT standard_output MATCHES "test passed: 9 selected procedures")
  message(FATAL_ERROR
    "self-hosted frontend unit tests failed (${status})\n"
    "stdout:\n${standard_output}stderr:\n${standard_error}")
endif()

file(REMOVE_RECURSE "${run_root}")
