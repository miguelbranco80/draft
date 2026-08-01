# Exact differential test for Draft-written canonical workspace graph loading.
#
# Both executables receive identical explicit workspace, dependency, and core
# roots. stdout contains no physical paths; stderr retains exact structured
# source diagnostics, with only native `cannot resolve` detail normalized by the
# C++ oracle to the portable core/filesystem contract. Every fixture compares
# stdout, stderr, and process status byte-for-byte. Scratch is unique below the
# caller-provided CMake binary directory and is removed after qualification.

foreach(required ORACLE DRAFTC_NEXT SOURCE_ROOT TEST_ROOT HOST_TARGET)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "missing required -D${required}=...")
  endif()
endforeach()
if(NOT EXISTS "${ORACLE}")
  message(FATAL_ERROR "workspace-syntax oracle does not exist: ${ORACLE}")
endif()
if(NOT EXISTS "${DRAFTC_NEXT}")
  message(FATAL_ERROR "Draft-written draftc-next does not exist: ${DRAFTC_NEXT}")
endif()

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef run_identity)
set(run_root "${TEST_ROOT}/${run_identity}")
file(MAKE_DIRECTORY "${run_root}")

# Additional arguments are complete `--dependency prefix root identity` groups.
# expected is `success` or `failure`; requiring the intended status prevents an
# accidental shared early rejection from satisfying the differential.
function(compare_workspace label root workspace core expected)
  set(common_arguments
    workspace-syntax "${root}"
    --workspace "${workspace}"
    --core "${core}"
    --core-identity draft-core-fixture
    --target "${HOST_TARGET}"
    ${ARGN}
  )
  execute_process(
    COMMAND "${ORACLE}" ${common_arguments}
    WORKING_DIRECTORY "${SOURCE_ROOT}"
    RESULT_VARIABLE oracle_result
    OUTPUT_FILE "${run_root}/oracle.stdout"
    ERROR_FILE "${run_root}/oracle.stderr"
  )
  execute_process(
    COMMAND "${DRAFTC_NEXT}" ${common_arguments}
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
      "workspace-syntax implementations differ for ${label}\n"
      "oracle result: ${oracle_result}\n"
      "next result: ${next_result}\n"
      "--- oracle stdout ---\n${oracle_stdout}"
      "--- next stdout ---\n${next_stdout}"
      "--- oracle stderr ---\n${oracle_stderr}"
      "--- next stderr ---\n${next_stderr}"
    )
  endif()
  if("${expected}" STREQUAL "success" AND NOT oracle_result EQUAL 0)
    file(READ "${run_root}/oracle.stderr" oracle_stderr)
    message(FATAL_ERROR "expected ${label} to succeed\n${oracle_stderr}")
  endif()
  if("${expected}" STREQUAL "failure" AND oracle_result EQUAL 0)
    message(FATAL_ERROR "expected ${label} to fail")
  endif()
endfunction()

# The real staging driver is a dense graph containing workspace and core
# packages, target-qualified core files, repeated dependencies, and aliases.
compare_workspace(
  repository-frontend
  "${SOURCE_ROOT}/tools/draftc_next"
  "${SOURCE_ROOT}"
  "${SOURCE_ROOT}/core"
  success
)

set(success_root "${run_root}/success")
set(success_workspace "${success_root}/workspace")
set(success_dependency "${success_root}/dependency")
set(success_core "${success_root}/core")
file(MAKE_DIRECTORY
  "${success_workspace}/app"
  "${success_workspace}/lib/math"
  "${success_dependency}/text"
  "${success_core}/io"
)
file(WRITE "${success_workspace}/app/package.draft"
  "package app\n"
  "import lib/math as math\n"
  "import vendor/text as text\n"
  "import core/io\n"
  "main :: proc() {}\n"
)
file(WRITE "${success_workspace}/app/second.draft"
  "package app\nimport lib/math as numbers\n"
)
file(WRITE "${success_workspace}/lib/math/package.draft"
  "package math\npub Answer :: 42\n"
)
file(WRITE "${success_dependency}/text/package.draft"
  "package text\npub Size :: 1\n"
)
file(WRITE "${success_core}/io/package.draft"
  "package io\npub Handle :: distinct u64\n"
)
compare_workspace(
  recursive-roots
  "${success_workspace}/app"
  "${success_workspace}"
  "${success_core}"
  success
  --dependency vendor "${success_dependency}" vendor-content
)

set(cycle_root "${run_root}/cycle")
file(MAKE_DIRECTORY
  "${cycle_root}/workspace/a"
  "${cycle_root}/workspace/b"
  "${cycle_root}/core"
)
file(WRITE "${cycle_root}/workspace/a/package.draft"
  "package a\nimport b\n"
)
file(WRITE "${cycle_root}/workspace/b/package.draft"
  "package b\nimport a\n"
)
compare_workspace(
  import-cycle
  "${cycle_root}/workspace/a"
  "${cycle_root}/workspace"
  "${cycle_root}/core"
  failure
)

set(ambiguous_root "${run_root}/ambiguous")
file(MAKE_DIRECTORY
  "${ambiguous_root}/workspace/app"
  "${ambiguous_root}/dependency"
  "${ambiguous_root}/core"
)
file(WRITE "${ambiguous_root}/workspace/app/package.draft" "package app\n")
compare_workspace(
  ambiguous-prefixes
  "${ambiguous_root}/workspace/app"
  "${ambiguous_root}/workspace"
  "${ambiguous_root}/core"
  failure
  --dependency vendor "${ambiguous_root}/dependency" vendor-content
  --dependency vendor/nested "${ambiguous_root}/dependency" nested-content
)

set(core_alone_root "${run_root}/core-alone")
file(MAKE_DIRECTORY
  "${core_alone_root}/workspace/app"
  "${core_alone_root}/core"
)
file(WRITE "${core_alone_root}/workspace/app/package.draft"
  "package app\nimport core\n"
)
compare_workspace(
  core-must-name-package
  "${core_alone_root}/workspace/app"
  "${core_alone_root}/workspace"
  "${core_alone_root}/core"
  failure
)

# A missing imported folder exercises portable canonicalization failure at a
# source edge. The oracle strips only its platform-specific strerror suffix.
set(missing_root "${run_root}/missing")
file(MAKE_DIRECTORY "${missing_root}/workspace/app" "${missing_root}/core")
file(WRITE "${missing_root}/workspace/app/package.draft"
  "package app\nimport absent/package\n"
)
compare_workspace(
  missing-import
  "${missing_root}/workspace/app"
  "${missing_root}/workspace"
  "${missing_root}/core"
  failure
)

# A malformed imported package is canonical and contained, then fails at the
# nested package parser. This proves unpublished package diagnostics retain
# exact source rendering and ordering.
set(malformed_root "${run_root}/malformed")
file(MAKE_DIRECTORY
  "${malformed_root}/workspace/app"
  "${malformed_root}/workspace/broken"
  "${malformed_root}/core"
)
file(WRITE "${malformed_root}/workspace/app/package.draft"
  "package app\nimport broken\n"
)
file(WRITE "${malformed_root}/workspace/broken/package.draft"
  "package broken\nValue ::\n"
)
compare_workspace(
  malformed-imported-package
  "${malformed_root}/workspace/app"
  "${malformed_root}/workspace"
  "${malformed_root}/core"
  failure
)

set(outside_root "${run_root}/outside-root")
file(MAKE_DIRECTORY
  "${outside_root}/workspace"
  "${outside_root}/selected"
  "${outside_root}/core"
)
file(WRITE "${outside_root}/selected/package.draft" "package selected\n")
compare_workspace(
  command-root-escape
  "${outside_root}/selected"
  "${outside_root}/workspace"
  "${outside_root}/core"
  failure
)

# Symlink creation is not guaranteed on every Windows test account. When the
# host permits it, an in-root alias must deduplicate to the canonical identity,
# while an out-of-root target must diagnose containment at the import range.
set(alias_root "${run_root}/aliases")
file(MAKE_DIRECTORY
  "${alias_root}/workspace/app"
  "${alias_root}/workspace/lib/math"
  "${alias_root}/outside/escaped"
  "${alias_root}/core"
)
file(WRITE "${alias_root}/workspace/app/package.draft"
  "package app\nimport lib/math\nimport alias/math\n"
)
file(WRITE "${alias_root}/workspace/lib/math/package.draft" "package math\n")
file(WRITE "${alias_root}/outside/escaped/package.draft" "package escaped\n")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E create_symlink
    "${alias_root}/workspace/lib" "${alias_root}/workspace/alias"
  RESULT_VARIABLE alias_link_result
)
if(alias_link_result EQUAL 0)
  compare_workspace(
    canonical-alias-deduplication
    "${alias_root}/workspace/app"
    "${alias_root}/workspace"
    "${alias_root}/core"
    success
  )
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E create_symlink
    "${alias_root}/outside" "${alias_root}/workspace/escape"
  RESULT_VARIABLE escape_link_result
)
if(escape_link_result EQUAL 0)
  file(WRITE "${alias_root}/workspace/app/package.draft"
    "package app\nimport escape/escaped\n"
  )
  compare_workspace(
    canonical-symlink-escape
    "${alias_root}/workspace/app"
    "${alias_root}/workspace"
    "${alias_root}/core"
    failure
  )
endif()

# Package recursion has its own 256-edge host-stack resource bound, independent
# of the parser's nesting limit. The failing edge originates in package_255.
set(depth_root "${run_root}/depth")
file(MAKE_DIRECTORY "${depth_root}/workspace" "${depth_root}/core")
foreach(index RANGE 0 256)
  file(MAKE_DIRECTORY "${depth_root}/workspace/package_${index}")
  if(index LESS 256)
    math(EXPR next_index "${index} + 1")
    file(WRITE "${depth_root}/workspace/package_${index}/package.draft"
      "package package_${index}\nimport package_${next_index}\n"
    )
  else()
    file(WRITE "${depth_root}/workspace/package_${index}/package.draft"
      "package package_${index}\n"
    )
  endif()
endforeach()
compare_workspace(
  import-depth-limit
  "${depth_root}/workspace/package_0"
  "${depth_root}/workspace"
  "${depth_root}/core"
  failure
)

message(STATUS
  "draftc-next workspace-syntax matched C++ across canonical graph fixtures")
file(REMOVE_RECURSE "${run_root}")
