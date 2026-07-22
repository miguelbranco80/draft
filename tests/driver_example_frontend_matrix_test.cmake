# Exhaustive provider-free frontend qualification for tracked examples.
#
# The checked-in TSV is intentionally data rather than a directory convention:
# every package is classified explicitly, including libraries, unresolved agent
# fixtures, foreign-provider programs, and resolved programs. This process test
# first proves that the matrix exactly covers the directories containing tracked
# Draft or assembly sources, then checks every row for every supported target.
# Expected unresolved packages must fail for the one intended reason; any other
# diagnostic is a regression rather than an acceptable negative example.

if(NOT DEFINED DRAFTC OR
   NOT DEFINED SOURCE_ROOT OR
   NOT DEFINED MATRIX)
  message(FATAL_ERROR
    "DRAFTC, SOURCE_ROOT, and MATRIX are required")
endif()

file(STRINGS "${MATRIX}" matrix_lines)
set(matrix_packages)
set(matrix_test_packages)
set(matrix_bench_packages)
set(row_count 0)

foreach(line IN LISTS matrix_lines)
  if(line STREQUAL "" OR line MATCHES "^[ \t]*#")
    continue()
  endif()

  string(REPLACE "\t" ";" fields "${line}")
  list(LENGTH fields field_count)
  if(NOT field_count EQUAL 5)
    message(FATAL_ERROR
      "example matrix row must have five tab-separated fields: ${line}")
  endif()
  list(GET fields 0 workspace)
  list(GET fields 1 root)
  list(GET fields 2 frontend)
  list(GET fields 3 native)
  list(GET fields 4 validation)

  if(IS_ABSOLUTE "${workspace}" OR
     workspace MATCHES "(^|/)\.\.(/|$)" OR
     root MATCHES "(^|/)\.\.(/|$)" OR
     root MATCHES "^/")
    message(FATAL_ERROR "example matrix paths must be normalized: ${line}")
  endif()
  if(NOT frontend MATCHES "^(pass|unresolved)$")
    message(FATAL_ERROR "unknown frontend classification: ${frontend}")
  endif()
  if(NOT native MATCHES
      "^(run|dependency|c-library|foreign-provider|resolved|none)$")
    message(FATAL_ERROR "unknown native classification: ${native}")
  endif()
  if(NOT validation MATCHES "^(none|test|test-bench)$")
    message(FATAL_ERROR "unknown validation classification: ${validation}")
  endif()

  if(root STREQUAL ".")
    set(package_relative "${workspace}")
  else()
    set(package_relative "${workspace}/${root}")
  endif()
  if(NOT IS_DIRECTORY "${SOURCE_ROOT}/${package_relative}")
    message(FATAL_ERROR
      "example matrix package does not exist: ${package_relative}")
  endif()
  list(FIND matrix_packages "${package_relative}" existing_index)
  if(NOT existing_index EQUAL -1)
    message(FATAL_ERROR
      "example matrix repeats package: ${package_relative}")
  endif()
  list(APPEND matrix_packages "${package_relative}")
  if(validation MATCHES "^(test|test-bench)$")
    list(APPEND matrix_test_packages "${package_relative}")
  endif()
  if(validation STREQUAL "test-bench")
    list(APPEND matrix_bench_packages "${package_relative}")
  endif()

  foreach(target IN ITEMS
      aarch64-macos aarch64-linux x86_64-linux x86_64-windows)
    execute_process(
      COMMAND "${DRAFTC}" check "${SOURCE_ROOT}/${workspace}"
        --root "${root}" --target "${target}"
      RESULT_VARIABLE result
      OUTPUT_VARIABLE standard_output
      ERROR_VARIABLE standard_error
    )
    if(frontend STREQUAL "pass")
      if(NOT result EQUAL 0)
        message(FATAL_ERROR
          "${package_relative} failed ${target} frontend qualification\n"
          "stdout:\n${standard_output}\nstderr:\n${standard_error}")
      endif()
    else()
      if(result EQUAL 0)
        message(FATAL_ERROR
          "${package_relative} unexpectedly resolved for ${target}")
      endif()
      string(CONCAT diagnostic "${standard_output}" "${standard_error}")
      string(FIND "${diagnostic}"
        "workspace has unresolved synthesis sites and no resolution manifest"
        unresolved_offset)
      if(unresolved_offset EQUAL -1)
        message(FATAL_ERROR
          "${package_relative} failed ${target} for an unexpected reason\n"
          "stdout:\n${standard_output}\nstderr:\n${standard_error}")
      endif()
    endif()
  endforeach()
  math(EXPR row_count "${row_count} + 1")
endforeach()

# Git supplies the repository-owned set in a checkout, excluding unrelated
# untracked experiments. A source archive has no `.git` entry, so it falls back
# to its complete physical source set. A worktree's `.git` file selects the Git
# branch just like a primary checkout's `.git` directory.
if(GIT_EXECUTABLE AND EXISTS "${SOURCE_ROOT}/.git")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_ROOT}" ls-files -- examples
    RESULT_VARIABLE git_result
    OUTPUT_VARIABLE tracked_output
    ERROR_VARIABLE git_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(NOT git_result EQUAL 0)
    message(FATAL_ERROR "cannot enumerate tracked examples: ${git_error}")
  endif()
  string(REPLACE "\n" ";" tracked_files "${tracked_output}")
else()
  file(GLOB_RECURSE tracked_files
    RELATIVE "${SOURCE_ROOT}"
    LIST_DIRECTORIES FALSE
    "${SOURCE_ROOT}/examples/*.draft"
    "${SOURCE_ROOT}/examples/*.s"
    "${SOURCE_ROOT}/examples/*.S"
    "${SOURCE_ROOT}/examples/*.asm"
  )
endif()
set(tracked_packages)
set(tracked_test_packages)
set(tracked_bench_packages)
foreach(path IN LISTS tracked_files)
  if(path MATCHES "(^|/)\.draft/" OR
     NOT path MATCHES "\.(draft|s|S|asm)$")
    continue()
  endif()
  get_filename_component(package "${path}" DIRECTORY)
  list(APPEND tracked_packages "${package}")
  if(path MATCHES "_test\.draft$")
    list(APPEND tracked_test_packages "${package}")
  endif()
  if(path MATCHES "_bench\.draft$")
    list(APPEND tracked_bench_packages "${package}")
  endif()
endforeach()
list(REMOVE_DUPLICATES tracked_packages)
list(REMOVE_DUPLICATES tracked_test_packages)
list(REMOVE_DUPLICATES tracked_bench_packages)
list(SORT tracked_packages)
list(SORT tracked_test_packages)
list(SORT tracked_bench_packages)
list(SORT matrix_packages)
list(SORT matrix_test_packages)
list(SORT matrix_bench_packages)

if(NOT tracked_packages STREQUAL matrix_packages)
  message(FATAL_ERROR
    "example qualification matrix does not match tracked package directories\n"
    "tracked: ${tracked_packages}\nclassified: ${matrix_packages}")
endif()
if(NOT tracked_test_packages STREQUAL matrix_test_packages)
  message(FATAL_ERROR
    "example test classification does not match tracked *_test.draft files\n"
    "tracked: ${tracked_test_packages}\nclassified: ${matrix_test_packages}")
endif()
if(NOT tracked_bench_packages STREQUAL matrix_bench_packages)
  message(FATAL_ERROR
    "example benchmark classification does not match tracked *_bench.draft files\n"
    "tracked: ${tracked_bench_packages}\nclassified: ${matrix_bench_packages}")
endif()

message(STATUS
  "qualified ${row_count} example packages for all supported frontends")
