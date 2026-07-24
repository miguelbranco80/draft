# Native Draft test-and-benchmark gate for every classified example package.
#
# Each row is exercised from a private copy of its declared workspace so imports
# and root identity match ordinary user commands while evidence and native build
# artifacts remain outside the checkout. The same matrix used by frontend and
# native-executable qualification therefore owns validation coverage as well;
# adding a test file without the corresponding classification cannot quietly
# disappear from CI.

if(NOT DEFINED DRAFTC OR
   NOT DEFINED SOURCE_ROOT OR
   NOT DEFINED MATRIX OR
   NOT DEFINED TEST_ROOT OR
   NOT DEFINED TARGET_SELECTOR)
  message(FATAL_ERROR
    "DRAFTC, SOURCE_ROOT, MATRIX, TEST_ROOT, and TARGET_SELECTOR are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(STRINGS "${MATRIX}" matrix_lines)
set(validation_count 0)

foreach(line IN LISTS matrix_lines)
  if(line STREQUAL "" OR line MATCHES "^[ \t]*#")
    continue()
  endif()
  string(REPLACE "\t" ";" fields "${line}")
  list(LENGTH fields field_count)
  if(NOT field_count EQUAL 5)
    message(FATAL_ERROR "malformed example matrix row: ${line}")
  endif()
  list(GET fields 0 workspace)
  list(GET fields 1 root)
  list(GET fields 4 validation)
  if(validation STREQUAL "none")
    continue()
  endif()

  string(SHA256 row_identity "${workspace}\t${root}")
  set(row_directory "${TEST_ROOT}/${row_identity}")
  file(MAKE_DIRECTORY "${row_directory}")
  file(COPY "${SOURCE_ROOT}/${workspace}" DESTINATION "${row_directory}")
  get_filename_component(workspace_name "${workspace}" NAME)
  set(copied_workspace "${row_directory}/${workspace_name}")
  if(NOT EXISTS "${copied_workspace}/draft.workspace")
    file(WRITE "${copied_workspace}/draft.workspace" "draft-workspace-v1\n")
  endif()

  execute_process(
    COMMAND "${DRAFTC}" test "${copied_workspace}/${root}"
      --target "${TARGET_SELECTOR}"
    RESULT_VARIABLE test_status
    OUTPUT_VARIABLE test_stdout
    ERROR_VARIABLE test_stderr
  )
  if(NOT test_status EQUAL 0 OR
     NOT test_stdout MATCHES "test passed: [1-9][0-9]* selected procedures")
    message(FATAL_ERROR
      "example tests failed for ${workspace}/${root} (${test_status})\n"
      "stdout:\n${test_stdout}\nstderr:\n${test_stderr}")
  endif()

  if(validation STREQUAL "test-bench")
    execute_process(
      COMMAND "${DRAFTC}" bench "${copied_workspace}/${root}"
        --target "${TARGET_SELECTOR}" --verify
      RESULT_VARIABLE bench_status
      OUTPUT_VARIABLE bench_stdout
      ERROR_VARIABLE bench_stderr
    )
    if(NOT bench_status EQUAL 0 OR
       NOT bench_stdout MATCHES
         "benchmark passed: [1-9][0-9]* selected procedures")
      message(FATAL_ERROR
        "example benchmarks failed for ${workspace}/${root} (${bench_status})\n"
        "stdout:\n${bench_stdout}\nstderr:\n${bench_stderr}")
    endif()
  endif()

  file(GLOB evidence "${copied_workspace}/.draft/evidence/*.json")
  if(NOT evidence)
    message(FATAL_ERROR
      "example validation published no workspace evidence for ${workspace}/${root}")
  endif()
  math(EXPR validation_count "${validation_count} + 1")
endforeach()

file(REMOVE_RECURSE "${TEST_ROOT}")
message(STATUS "validated ${validation_count} example packages")
