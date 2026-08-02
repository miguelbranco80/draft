# Exact differential test for target-selected package declarations.
#
# Both executables first emit the established graph and unconditional
# declaration prefix. The production oracle then replays its published
# ConditionalSelections through the source-level collector; Draft evaluates the
# target-only expression subset and must emit identical decisions, appended
# SymbolIds, selected public names, qualified alias members, diagnostics, and
# status. Scratch is process-unique below the caller-provided binary directory.

foreach(required ORACLE DRAFTC_NEXT SOURCE_ROOT TEST_ROOT HOST_TARGET)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "missing required -D${required}=...")
  endif()
endforeach()
if(NOT EXISTS "${ORACLE}")
  message(FATAL_ERROR "target-declarations oracle does not exist: ${ORACLE}")
endif()
if(NOT EXISTS "${DRAFTC_NEXT}")
  message(FATAL_ERROR "Draft-written draftc-next does not exist: ${DRAFTC_NEXT}")
endif()

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef run_identity)
set(run_root "${TEST_ROOT}/${run_identity}")
file(MAKE_DIRECTORY "${run_root}")

function(compare_target_declarations label root workspace core target expected)
  set(common_arguments
    workspace-target-declarations "${root}"
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
      "workspace-target-declarations implementations differ for ${label}\n"
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

# The real staging graph currently exercises core/c_abi's ABI-selected public
# aliases and proves that the new phase composes with the complete prior graph.
foreach(target
    aarch64-macos
    aarch64-linux
    x86_64-linux
    x86_64-windows)
  compare_target_declarations(
    "repository-frontend-${target}"
    "${SOURCE_ROOT}/tools/draftc_next"
    "${SOURCE_ROOT}"
    "${SOURCE_ROOT}/core"
    "${target}"
    success
  )
endforeach()

# One root package exercises every supported expression form. The real graph
# above already proves selected public names replace an imported alias span;
# this fixture isolates branch behavior from workspace scheduling.
set(matrix_root "${run_root}/matrix")
set(matrix_workspace "${matrix_root}/workspace")
set(matrix_core "${matrix_root}/core")
file(MAKE_DIRECTORY
  "${matrix_workspace}/platform"
  "${matrix_core}"
)
file(WRITE "${matrix_workspace}/platform/package.draft"
  "package platform\n"
  "pub Unconditional :: distinct u8\n"
  "main :: proc() {}\n"
)
file(WRITE "${matrix_workspace}/platform/selected@aarch64-macos.draft"
  "package platform\n"
  "when true && target.pointer_bits == 64 && target.page_size >= 16384 &&\n"
  "    target.byte_order == .little && .macos == target.os &&\n"
  "    target.arch == .aarch64 && target.abi != .win64 &&\n"
  "    target.object_format == .macho && target.has_feature(\"neon\") &&\n"
  "    !target.has_feature(\"aes\") {\n"
  "  pub Selected_Type :: distinct u16\n"
  "  pub Selected_Value :: 1\n"
  "  Selected_Private :: 1\n"
  "  foreign fixture {\n"
  "    pub native_value :: c proc(value: u32) -> u32\n"
  "  }\n"
  "  export exported_value as \"draft_selected_export\" :: c proc(value: u32) -> u32 { return value; }\n"
  "} else {\n"
  "  pub Selected_Type :: distinct u32\n"
  "  pub Selected_Value :: 2\n"
  "  Selected_Private :: 2\n"
  "  foreign fixture {\n"
  "    pub native_value :: c proc(value: u64) -> u64\n"
  "  }\n"
  "  export exported_value as \"draft_selected_export\" :: c proc(value: u64) -> u64 { return value; }\n"
  "}\n"
)
file(WRITE "${matrix_workspace}/platform/selected@aarch64-linux.draft"
  "package platform\n"
  "when target.pointer_bits == 64 && target.page_size >= 4096 &&\n"
  "    target.byte_order == .little && target.os == .linux &&\n"
  "    target.arch == .aarch64 && target.abi == .aapcs64_gnu &&\n"
  "    target.object_format == .elf && target.has_feature(\"neon\") &&\n"
  "    !target.has_feature(\"aes\") {\n"
  "  pub Selected_Type :: distinct u16\n"
  "  pub Selected_Value :: 1\n"
  "  Selected_Private :: 1\n"
  "} else {\n"
  "  pub Selected_Type :: distinct u32\n"
  "  pub Selected_Value :: 2\n"
  "  Selected_Private :: 2\n"
  "}\n"
)
file(WRITE "${matrix_workspace}/platform/selected@x86_64-linux.draft"
  "package platform\n"
  "when target.pointer_bits == 64 && target.page_size >= 4096 &&\n"
  "    target.byte_order == .little && target.os == .linux &&\n"
  "    target.arch == .x86_64 && target.abi == .sysv_amd64 &&\n"
  "    target.object_format == .elf && target.has_feature(\"sse2\") &&\n"
  "    !target.has_feature(\"avx\") {\n"
  "  pub Selected_Type :: distinct u16\n"
  "  pub Selected_Value :: 1\n"
  "  Selected_Private :: 1\n"
  "} else {\n"
  "  pub Selected_Type :: distinct u32\n"
  "  pub Selected_Value :: 2\n"
  "  Selected_Private :: 2\n"
  "}\n"
)
file(WRITE "${matrix_workspace}/platform/selected@x86_64-windows.draft"
  "package platform\n"
  "when target.pointer_bits == 64 && target.page_size >= 4096 &&\n"
  "    target.byte_order == .little && target.os == .windows &&\n"
  "    target.arch == .x86_64 && target.abi == .win64 &&\n"
  "    target.object_format == .coff && target.has_feature(\"sse2\") &&\n"
  "    !target.has_feature(\"avx\") {\n"
  "  pub Selected_Type :: distinct u16\n"
  "  pub Selected_Value :: 1\n"
  "  Selected_Private :: 1\n"
  "} else {\n"
  "  pub Selected_Type :: distinct u32\n"
  "  pub Selected_Value :: 2\n"
  "  Selected_Private :: 2\n"
  "}\n"
)
foreach(target
    aarch64-macos
    aarch64-linux
    x86_64-linux
    x86_64-windows)
  compare_target_declarations(
    "expression-${target}"
    "${matrix_workspace}/platform"
    "${matrix_workspace}"
    "${matrix_core}"
    "${target}"
    success
  )
endforeach()

# An `else when` is not collected until its false predecessor is selected.
# Exercise that growing fixed-point frontier independently from the expression
# matrix so the expected condition order remains obvious.
set(chain_root "${run_root}/else-when")
file(MAKE_DIRECTORY
  "${chain_root}/workspace/app"
  "${chain_root}/core"
)
file(WRITE "${chain_root}/workspace/app/package.draft"
  "package app\n"
  "when target.os == .windows {\n"
  "  pub Chain_Value :: distinct u16\n"
  "} else when target.os == .linux {\n"
  "  pub Chain_Value :: distinct u32\n"
  "} else {\n"
  "  pub Chain_Value :: distinct u64\n"
  "}\n"
  "main :: proc() {}\n"
)
foreach(target
    aarch64-macos
    aarch64-linux
    x86_64-linux
    x86_64-windows)
  compare_target_declarations(
    "else-when-${target}"
    "${chain_root}/workspace/app"
    "${chain_root}/workspace"
    "${chain_root}/core"
    "${target}"
    success
  )
endforeach()

# Earlier graph and declaration failures retain their exact previous boundary
# and never attempt target selection.
set(duplicate_root "${run_root}/duplicate")
file(MAKE_DIRECTORY
  "${duplicate_root}/workspace/app"
  "${duplicate_root}/core"
)
file(WRITE "${duplicate_root}/workspace/app/a.draft"
  "package app\npub Value :: 1\n"
)
file(WRITE "${duplicate_root}/workspace/app/b.draft"
  "package app\npub Value :: 2\n"
)
compare_target_declarations(
  declaration-duplicate
  "${duplicate_root}/workspace/app"
  "${duplicate_root}/workspace"
  "${duplicate_root}/core"
  "${HOST_TARGET}"
  failure
)

set(missing_root "${run_root}/missing-import")
file(MAKE_DIRECTORY
  "${missing_root}/workspace/app"
  "${missing_root}/core"
)
file(WRITE "${missing_root}/workspace/app/package.draft"
  "package app\nimport absent/package\n"
)
compare_target_declarations(
  missing-import
  "${missing_root}/workspace/app"
  "${missing_root}/workspace"
  "${missing_root}/core"
  "${HOST_TARGET}"
  failure
)

# A selected declaration is inserted into the existing package scope. Duplicate
# lookup must therefore see the unconditional prefix and preserve the earlier
# declaration diagnostic rather than treating a branch as an isolated scope.
set(selected_duplicate_root "${run_root}/selected-duplicate")
file(MAKE_DIRECTORY
  "${selected_duplicate_root}/workspace/app"
  "${selected_duplicate_root}/core"
)
file(WRITE "${selected_duplicate_root}/workspace/app/package.draft"
  "package app\n"
  "pub Value :: 1\n"
  "when true { pub Value :: 2; }\n"
  "main :: proc() {}\n"
)
compare_target_declarations(
  selected-duplicate
  "${selected_duplicate_root}/workspace/app"
  "${selected_duplicate_root}/workspace"
  "${selected_duplicate_root}/core"
  "${HOST_TARGET}"
  failure
)

# Named constants require the later general compile-time product graph. The
# staging command must fail explicitly rather than selecting a guessed branch.
set(unsupported_root "${run_root}/unsupported-condition")
file(MAKE_DIRECTORY
  "${unsupported_root}/workspace/app"
  "${unsupported_root}/core"
)
file(WRITE "${unsupported_root}/workspace/app/package.draft"
  "package app\n"
  "Enabled :: true\n"
  "when Enabled { pub Selected :: 1; }\n"
  "main :: proc() {}\n"
)
execute_process(
  COMMAND "${DRAFTC_NEXT}"
    workspace-target-declarations "${unsupported_root}/workspace/app"
    --workspace "${unsupported_root}/workspace"
    --core "${unsupported_root}/core"
    --core-identity draft-core-fixture
    --target "${HOST_TARGET}"
  WORKING_DIRECTORY "${SOURCE_ROOT}"
  RESULT_VARIABLE unsupported_result
  OUTPUT_VARIABLE unsupported_stdout
  ERROR_VARIABLE unsupported_stderr
)
if(unsupported_result EQUAL 0 OR NOT unsupported_stderr MATCHES
   "self-hosted target 'when' requires a supported target-fact boolean expression")
  message(FATAL_ERROR
    "unsupported target condition did not fail explicitly (${unsupported_result})\n"
    "stdout:\n${unsupported_stdout}stderr:\n${unsupported_stderr}")
endif()

# Logical evaluation may skip the dead operand's value, but both operands still
# require valid Draft types. The staging subset likewise must not accept a
# not-yet-moved named dependency merely because it occurs behind `false &&`.
file(WRITE "${unsupported_root}/workspace/app/package.draft"
  "package app\n"
  "Enabled :: true\n"
  "when false && Enabled { pub Selected :: 1; }\n"
  "main :: proc() {}\n"
)
execute_process(
  COMMAND "${DRAFTC_NEXT}"
    workspace-target-declarations "${unsupported_root}/workspace/app"
    --workspace "${unsupported_root}/workspace"
    --core "${unsupported_root}/core"
    --core-identity draft-core-fixture
    --target "${HOST_TARGET}"
  WORKING_DIRECTORY "${SOURCE_ROOT}"
  RESULT_VARIABLE short_circuit_result
  ERROR_VARIABLE short_circuit_stderr
)
if(short_circuit_result EQUAL 0 OR NOT short_circuit_stderr MATCHES
   "self-hosted target 'when' requires a supported target-fact boolean expression")
  message(FATAL_ERROR
    "dead unsupported target operand did not fail (${short_circuit_result})\n"
    "stderr:\n${short_circuit_stderr}")
endif()

# has_feature distinguishes a known disabled feature from an unknown spelling.
# The latter is a language error and cannot be treated as ordinary false.
file(WRITE "${unsupported_root}/workspace/app/package.draft"
  "package app\n"
  "when target.has_feature(\"not-a-profile-feature\") { pub Selected :: 1; }\n"
  "main :: proc() {}\n"
)
execute_process(
  COMMAND "${DRAFTC_NEXT}"
    workspace-target-declarations "${unsupported_root}/workspace/app"
    --workspace "${unsupported_root}/workspace"
    --core "${unsupported_root}/core"
    --core-identity draft-core-fixture
    --target "${HOST_TARGET}"
  WORKING_DIRECTORY "${SOURCE_ROOT}"
  RESULT_VARIABLE unknown_feature_result
  ERROR_VARIABLE unknown_feature_stderr
)
if(unknown_feature_result EQUAL 0 OR NOT unknown_feature_stderr MATCHES
   "self-hosted target 'when' requires a supported target-fact boolean expression")
  message(FATAL_ERROR
    "unknown target feature did not fail (${unknown_feature_result})\n"
    "stderr:\n${unknown_feature_stderr}")
endif()

# Contextual target alternatives remain type-specific. The target-only
# evaluator must not turn a cross-category comparison into an ordinary false
# branch merely because the underlying spellings differ.
set(invalid_category_root "${run_root}/invalid-category")
file(MAKE_DIRECTORY
  "${invalid_category_root}/workspace/app"
  "${invalid_category_root}/core"
)
file(WRITE "${invalid_category_root}/workspace/app/package.draft"
  "package app\n"
  "when target.os == .aarch64 { pub Invalid :: 1; }\n"
  "main :: proc() {}\n"
)
execute_process(
  COMMAND "${DRAFTC_NEXT}"
    workspace-target-declarations "${invalid_category_root}/workspace/app"
    --workspace "${invalid_category_root}/workspace"
    --core "${invalid_category_root}/core"
    --core-identity draft-core-fixture
    --target "${HOST_TARGET}"
  WORKING_DIRECTORY "${SOURCE_ROOT}"
  RESULT_VARIABLE invalid_category_result
  ERROR_VARIABLE invalid_category_stderr
)
if(invalid_category_result EQUAL 0 OR NOT invalid_category_stderr MATCHES
   "self-hosted target 'when' requires a supported target-fact boolean expression")
  message(FATAL_ERROR
    "cross-category target alternative did not fail (${invalid_category_result})\n"
    "stderr:\n${invalid_category_stderr}")
endif()

message(STATUS
  "draftc-next target declarations matched production condition selections")
file(REMOVE_RECURSE "${run_root}")
