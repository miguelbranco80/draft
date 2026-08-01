# Differential process test for the Draft-written folder-package boundary.
#
# The C++ oracle and draftc-next receive the same physical directory and exact
# target tag. stdout is the selected, sorted file inventory plus concrete trees;
# stderr is the normalized package/source diagnostic stream. Comparing files
# preserves every byte, while exact status comparison covers malformed and
# missing packages. Scratch is process-unique below the CMake binary directory.

foreach(required ORACLE DRAFTC_NEXT SOURCE_ROOT TEST_ROOT HOST_TARGET)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "missing required -D${required}=...")
  endif()
endforeach()
if(NOT EXISTS "${ORACLE}")
  message(FATAL_ERROR "package-syntax oracle does not exist: ${ORACLE}")
endif()
if(NOT EXISTS "${DRAFTC_NEXT}")
  message(FATAL_ERROR "Draft-written draftc-next does not exist: ${DRAFTC_NEXT}")
endif()

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef run_identity)
set(run_root "${TEST_ROOT}/${run_identity}")
file(MAKE_DIRECTORY "${run_root}")

# compare_package always executes both implementations, including for expected
# failures. An optional `success` argument also proves that a fixture did real
# package work instead of agreeing on an accidental early rejection.
function(compare_package directory target_selector)
  execute_process(
    COMMAND "${ORACLE}" package-syntax "${directory}"
      --target "${target_selector}"
    WORKING_DIRECTORY "${SOURCE_ROOT}"
    RESULT_VARIABLE oracle_result
    OUTPUT_FILE "${run_root}/oracle.stdout"
    ERROR_FILE "${run_root}/oracle.stderr"
  )
  execute_process(
    COMMAND "${DRAFTC_NEXT}" package-syntax "${directory}"
      --target "${target_selector}"
    WORKING_DIRECTORY "${SOURCE_ROOT}"
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
     NOT stdout_result EQUAL 0 OR NOT stderr_result EQUAL 0)
    file(READ "${run_root}/oracle.stdout" oracle_stdout)
    file(READ "${run_root}/next.stdout" next_stdout)
    file(READ "${run_root}/oracle.stderr" oracle_stderr)
    file(READ "${run_root}/next.stderr" next_stderr)
    message(FATAL_ERROR
      "package-syntax implementations differ for ${directory} (${target_selector})\n"
      "oracle result: ${oracle_result}\n"
      "next result: ${next_result}\n"
      "--- oracle stdout ---\n${oracle_stdout}"
      "--- next stdout ---\n${next_stdout}"
      "--- oracle stderr ---\n${oracle_stderr}"
      "--- next stderr ---\n${next_stderr}"
    )
  endif()
  if(ARGC GREATER 2 AND "${ARGV2}" STREQUAL "success" AND
     NOT oracle_result EQUAL 0)
    file(READ "${run_root}/oracle.stderr" oracle_stderr)
    message(FATAL_ERROR
      "expected package fixture to succeed: ${directory}\n${oracle_stderr}")
  endif()
endfunction()

# Every repository-owned source directory is one realistic package candidate.
# Hidden `.draft` directories are generated fragment stores rather than folder
# packages: their files are deliberately incomplete expansion results. The
# native target corpus otherwise proves the complete current build-tree
# selection without multiplying all packages across four process pairs.
file(GLOB_RECURSE source_files LIST_DIRECTORIES false
  "${SOURCE_ROOT}/core/*.draft"
  "${SOURCE_ROOT}/compiler/*.draft"
  "${SOURCE_ROOT}/examples/*.draft"
  "${SOURCE_ROOT}/lib/*.draft"
  "${SOURCE_ROOT}/tools/*.draft"
)
set(package_directories)
foreach(source IN LISTS source_files)
  get_filename_component(package_directory "${source}" DIRECTORY)
  if(package_directory MATCHES "/\\.draft(/|$)")
    continue()
  endif()
  list(APPEND package_directories "${package_directory}")
endforeach()
list(REMOVE_DUPLICATES package_directories)
list(SORT package_directories)
foreach(package_directory IN LISTS package_directories)
  compare_package("${package_directory}" "${HOST_TARGET}" success)
endforeach()

# One dense package fixes bytewise ordering, nested-directory exclusion,
# test/benchmark exclusion, assembly participation, and all four exact target
# qualifiers independently of the repository's current package mix.
set(matrix "${run_root}/matrix")
file(MAKE_DIRECTORY "${matrix}/nested")
file(WRITE "${matrix}/z_last.draft" "package matrix\nLast :: 1\n")
file(WRITE "${matrix}/a_first.draft" "package matrix\nFirst :: 2\n")
file(WRITE "${matrix}/feature_test.draft" "package wrong\n")
file(WRITE "${matrix}/feature_bench.draft" "package wrong\n")
file(WRITE "${matrix}/native.s" ".text\n")
file(WRITE "${matrix}/README.md" "ignored\n")
file(WRITE "${matrix}/nested/hidden.draft" "package wrong\n")
foreach(selector
    aarch64-macos aarch64-linux x86_64-linux x86_64-windows)
  string(REPLACE "-" "_" selector_name "${selector}")
  file(WRITE "${matrix}/platform@${selector}.draft"
    "package matrix\nSelected_${selector_name} :: 3\n")
endforeach()
foreach(selector
    aarch64-macos aarch64-linux x86_64-linux x86_64-windows)
  compare_package("${matrix}" "${selector}" success)
endforeach()

set(mismatch "${run_root}/mismatch")
file(MAKE_DIRECTORY "${mismatch}")
file(WRITE "${mismatch}/a.draft" "package expected\nA :: 1\n")
file(WRITE "${mismatch}/b.draft" "package other\nB :: 2\n")
compare_package("${mismatch}" "${HOST_TARGET}")

set(malformed "${run_root}/malformed")
file(MAKE_DIRECTORY "${malformed}")
file(WRITE "${malformed}/package.draft"
  "package malformed\nBroken ::\n")
compare_package("${malformed}" "${HOST_TARGET}")

set(contextual "${run_root}/contextual")
file(MAKE_DIRECTORY "${contextual}")
file(WRITE "${contextual}/package.draft" "package memory\nValue :: 1\n")
compare_package("${contextual}" "${HOST_TARGET}" success)

set(no_source "${run_root}/no-source")
file(MAKE_DIRECTORY "${no_source}")
file(WRITE "${no_source}/native.s" ".text\n")
compare_package("${no_source}" "${HOST_TARGET}")

set(empty "${run_root}/empty")
file(MAKE_DIRECTORY "${empty}")
compare_package("${empty}" "${HOST_TARGET}")
compare_package("${run_root}/missing" "${HOST_TARGET}")

list(LENGTH package_directories package_count)
math(EXPR comparison_count "${package_count} + 10")
message(STATUS
  "draftc-next package-syntax matched C++ for ${comparison_count} package/target inputs")
file(REMOVE_RECURSE "${run_root}")
