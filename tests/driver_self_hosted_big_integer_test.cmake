# Exact differential test for the Draft-written arbitrary-precision integer.
#
# The bootstrap executable is a fixed mathematical oracle over production
# src/sema/BigInteger. This script stages ordinary Draft packages in a random
# CMake-binary subdirectory, builds the paired fixed Draft exerciser with -O2,
# and compares stdout, stderr, and process status without passing either
# implementation through shell or CMake string normalization. Derived compiler
# state therefore stays out of the source checkout and cannot collide across
# simultaneous builds or worktrees.

foreach(required DRAFTC ORACLE SOURCE_ROOT TEST_ROOT TARGET_SELECTOR)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "missing required -D${required}=...")
  endif()
endforeach()
if(NOT EXISTS "${DRAFTC}")
  message(FATAL_ERROR "bootstrap draftc does not exist: ${DRAFTC}")
endif()
if(NOT EXISTS "${ORACLE}")
  message(FATAL_ERROR "C++ big-integer oracle does not exist: ${ORACLE}")
endif()

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef run_identity)
set(run_root "${TEST_ROOT}/${run_identity}")
set(workspace "${run_root}/workspace")
set(next_executable "${run_root}/draft-big-integer-next")
if(WIN32)
  string(APPEND next_executable ".exe")
endif()
file(MAKE_DIRECTORY "${workspace}/tools")
file(WRITE "${workspace}/draft.workspace" "draft-workspace-v1\n")
file(COPY "${SOURCE_ROOT}/compiler"
  DESTINATION "${workspace}"
  FILES_MATCHING PATTERN "*.draft")
file(COPY "${SOURCE_ROOT}/tools/draft_big_integer_next"
  DESTINATION "${workspace}/tools"
  FILES_MATCHING PATTERN "*.draft")

execute_process(
  COMMAND "${DRAFTC}" build "${workspace}/tools/draft_big_integer_next"
    --target "${TARGET_SELECTOR}" -O2 -o "${next_executable}"
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr
)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR
    "Draft big-integer exerciser build failed (${build_result})\n"
    "stdout:\n${build_stdout}stderr:\n${build_stderr}")
endif()

execute_process(
  COMMAND "${ORACLE}"
  RESULT_VARIABLE oracle_result
  OUTPUT_FILE "${run_root}/oracle.stdout"
  ERROR_FILE "${run_root}/oracle.stderr"
)
execute_process(
  COMMAND "${next_executable}"
  RESULT_VARIABLE next_result
  OUTPUT_FILE "${run_root}/next.stdout"
  ERROR_FILE "${run_root}/next.stderr"
)
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files
    "${run_root}/oracle.stdout" "${run_root}/next.stdout"
  RESULT_VARIABLE stdout_result
)
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files
    "${run_root}/oracle.stderr" "${run_root}/next.stderr"
  RESULT_VARIABLE stderr_result
)

if(NOT "${oracle_result}" STREQUAL "${next_result}" OR
   NOT oracle_result EQUAL 0 OR
   NOT stdout_result EQUAL 0 OR NOT stderr_result EQUAL 0)
  file(READ "${run_root}/oracle.stdout" oracle_stdout)
  file(READ "${run_root}/next.stdout" next_stdout)
  file(READ "${run_root}/oracle.stderr" oracle_stderr)
  file(READ "${run_root}/next.stderr" next_stderr)
  message(FATAL_ERROR
    "big-integer implementations differ\n"
    "oracle result: ${oracle_result}\n"
    "next result: ${next_result}\n"
    "--- oracle stdout ---\n${oracle_stdout}"
    "--- next stdout ---\n${next_stdout}"
    "--- oracle stderr ---\n${oracle_stderr}"
    "--- next stderr ---\n${next_stderr}")
endif()

message(STATUS "Draft big integer matched the production operation matrix")
file(REMOVE_RECURSE "${run_root}")
