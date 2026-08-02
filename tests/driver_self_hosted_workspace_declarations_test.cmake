# Exact differential test for Draft-written declaration-name collection.
#
# Both executables load one identical explicit workspace graph, emit its stable
# graph view, then emit package/file scopes, accepted declarations, source-only
# classification flags, and file-local aliases bound to canonical target package
# IDs. Semantic failures compare the partial accepted name set, exact diagnostics,
# and exit status. Scratch is unique below the caller-provided CMake binary
# directory and is removed after qualification.

foreach(required ORACLE DRAFTC_NEXT SOURCE_ROOT TEST_ROOT HOST_TARGET)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "missing required -D${required}=...")
  endif()
endforeach()
if(NOT EXISTS "${ORACLE}")
  message(FATAL_ERROR "workspace-declarations oracle does not exist: ${ORACLE}")
endif()
if(NOT EXISTS "${DRAFTC_NEXT}")
  message(FATAL_ERROR "Draft-written draftc-next does not exist: ${DRAFTC_NEXT}")
endif()

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef run_identity)
set(run_root "${TEST_ROOT}/${run_identity}")
file(MAKE_DIRECTORY "${run_root}")

# Additional arguments are complete `--dependency prefix root identity` groups.
# expected is `success` or `failure`; requiring the intended result prevents an
# accidental shared early rejection from satisfying the differential.
function(compare_declarations label root workspace core target expected)
  set(common_arguments
    workspace-declarations "${root}"
    --workspace "${workspace}"
    --core "${core}"
    --core-identity draft-core-fixture
    --target "${target}"
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
      "workspace-declarations implementations differ for ${label}\n"
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

# The real staging driver exercises a dense multi-package name set, contextual
# names, native bindings, repeated package imports, and multiple source files.
compare_declarations(
  repository-frontend
  "${SOURCE_ROOT}/tools/draftc_next"
  "${SOURCE_ROOT}"
  "${SOURCE_ROOT}/core"
  "${HOST_TARGET}"
  success
)

set(matrix_root "${run_root}/matrix")
set(matrix_workspace "${matrix_root}/workspace")
set(matrix_dependency "${matrix_root}/dependency")
set(matrix_core "${matrix_root}/core")
file(MAKE_DIRECTORY
  "${matrix_workspace}/app"
  "${matrix_workspace}/lib/math"
  "${matrix_dependency}/text"
  "${matrix_core}/io"
)
file(WRITE "${matrix_workspace}/app/a.draft"
  "package app\n"
  "import lib/math as math\n"
  "import vendor/text as text\n"
  "import core/io\n"
  "pub Pair[T: type] :: struct { value: T, }\n"
  "deny asm {\n"
  "  Safe_Value :: 7\n"
  "}\n"
  "when true {\n"
  "  Deferred :: u64\n"
  "} else {\n"
  "  Also_Deferred :: u32\n"
  "}\n"
  "foreign libc {\n"
  "  puts :: c proc(message: cstring) -> int\n"
  "}\n"
  "export entry as \"draft_entry\" :: c proc() -> int {\n"
  "  return 0\n"
  "}\n"
  "left, right: u32\n"
  "First, Second :: 1\n"
  "Alias :: math.Number\n"
  "Pack :: proc(values: ..type) {}\n"
)
file(WRITE "${matrix_workspace}/app/b.draft"
  "package app\n"
  "import lib/math as numbers\n"
  "math :: 3\n"
  "memory :: 4\n"
)
file(WRITE "${matrix_workspace}/lib/math/package.draft"
  "package math\npub Number :: distinct u64\n"
)
file(WRITE "${matrix_dependency}/text/package.draft"
  "package text\npub Size :: 1\n"
)
file(WRITE "${matrix_core}/io/package.draft"
  "package io\npub Handle :: distinct u64\n"
)
compare_declarations(
  declaration-matrix
  "${matrix_workspace}/app"
  "${matrix_workspace}"
  "${matrix_core}"
  "${HOST_TARGET}"
  success
  --dependency vendor "${matrix_dependency}" vendor-content
)

# Package declarations share one scope across files, so the second Value is
# rejected and the first remains the canonical binding named by the note.
set(duplicate_root "${run_root}/duplicate")
file(MAKE_DIRECTORY
  "${duplicate_root}/workspace/app"
  "${duplicate_root}/core"
)
file(WRITE "${duplicate_root}/workspace/app/a.draft"
  "package app\nValue :: 1\n"
)
file(WRITE "${duplicate_root}/workspace/app/b.draft"
  "package app\nValue :: 2\n"
)
compare_declarations(
  cross-file-duplicate
  "${duplicate_root}/workspace/app"
  "${duplicate_root}/workspace"
  "${duplicate_root}/core"
  "${HOST_TARGET}"
  failure
)

# Import aliases are ordinary names only in their file. Two different source
# edges remain in the canonical graph, but the second same-file alias publishes
# no Import_Binding after its duplicate diagnostic.
set(alias_root "${run_root}/duplicate-alias")
file(MAKE_DIRECTORY
  "${alias_root}/workspace/app"
  "${alias_root}/workspace/first"
  "${alias_root}/workspace/second"
  "${alias_root}/core"
)
file(WRITE "${alias_root}/workspace/app/package.draft"
  "package app\n"
  "import first as dependency\n"
  "import second as dependency\n"
)
file(WRITE "${alias_root}/workspace/first/package.draft" "package first\n")
file(WRITE "${alias_root}/workspace/second/package.draft" "package second\n")
compare_declarations(
  duplicate-file-alias
  "${alias_root}/workspace/app"
  "${alias_root}/workspace"
  "${alias_root}/core"
  "${HOST_TARGET}"
  failure
)

# These are semantic collection diagnostics rather than parser errors. The
# collector continues and retains every otherwise valid binding in source order.
set(modifier_root "${run_root}/modifier-errors")
file(MAKE_DIRECTORY
  "${modifier_root}/workspace/app"
  "${modifier_root}/core"
)
file(WRITE "${modifier_root}/workspace/app/package.draft"
  "package app\n"
  "thread_local Type :: struct { value: u32, }\n"
  "thread_local Procedure :: proc() {}\n"
  "thread_local Constant :: 1\n"
  "Value[T: type]: u32\n"
  "Compile_Time[N: usize] :: 1\n"
  "ordinary as \"native_ordinary\" :: proc() {}\n"
  "Type_A, Type_B :: struct { value: u8, }\n"
  "Proc_A, Proc_B :: proc() {}\n"
)
compare_declarations(
  declaration-modifier-errors
  "${modifier_root}/workspace/app"
  "${modifier_root}/workspace"
  "${modifier_root}/core"
  "${HOST_TARGET}"
  failure
)

# Exact target selection changes the declaration name set but not the package or
# scope contract. Exercise every built-in selector independently of host target.
set(target_root "${run_root}/target-selection")
file(MAKE_DIRECTORY
  "${target_root}/workspace/app"
  "${target_root}/core"
)
file(WRITE "${target_root}/workspace/app/package.draft"
  "package app\nShared :: 1\n"
)
file(WRITE "${target_root}/workspace/app/platform@aarch64-macos.draft"
  "package app\nSelected_Aarch64_Macos :: 1\n"
)
file(WRITE "${target_root}/workspace/app/platform@aarch64-linux.draft"
  "package app\nSelected_Aarch64_Linux :: 1\n"
)
file(WRITE "${target_root}/workspace/app/platform@x86_64-linux.draft"
  "package app\nSelected_X86_64_Linux :: 1\n"
)
file(WRITE "${target_root}/workspace/app/platform@x86_64-windows.draft"
  "package app\nSelected_X86_64_Windows :: 1\n"
)
foreach(target
    aarch64-macos
    aarch64-linux
    x86_64-linux
    x86_64-windows)
  compare_declarations(
    "target-${target}"
    "${target_root}/workspace/app"
    "${target_root}/workspace"
    "${target_root}/core"
    "${target}"
    success
  )
endforeach()

# A graph failure must not enter the semantic collector; both commands retain
# the partial graph, graph diagnostic, and nonzero status unchanged.
set(missing_root "${run_root}/missing-import")
file(MAKE_DIRECTORY
  "${missing_root}/workspace/app"
  "${missing_root}/core"
)
file(WRITE "${missing_root}/workspace/app/package.draft"
  "package app\nimport absent/package\n"
)
compare_declarations(
  missing-import
  "${missing_root}/workspace/app"
  "${missing_root}/workspace"
  "${missing_root}/core"
  "${HOST_TARGET}"
  failure
)

message(STATUS
  "draftc-next workspace-declarations matched C++ across name-set fixtures")
file(REMOVE_RECURSE "${run_root}")
