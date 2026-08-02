# Exact differential test for target-selected package declarations.
#
# Both executables first emit the established graph and unconditional
# declaration prefix. The production oracle then replays its published
# ConditionalSelections through the source-level collector; Draft evaluates the
# target/scalar subset through explicit local constant products and must emit
# identical decisions, appended SymbolIds, selected public names, qualified
# alias members, diagnostics, and status. Scratch is process-unique below the
# caller-provided binary directory.

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

# Current staging limitations intentionally cannot be compared byte-for-byte
# with the complete production evaluator: production may accept the expression
# or diagnose its full-language reason. Require the Draft boundary to fail
# closed with its exact self-hosting message instead of guessing a branch.
function(expect_next_condition_failure label root workspace core)
  execute_process(
    COMMAND "${DRAFTC_NEXT}"
      workspace-target-declarations "${root}"
      --workspace "${workspace}"
      --core "${core}"
      --core-identity draft-core-fixture
      --target "${HOST_TARGET}"
    WORKING_DIRECTORY "${SOURCE_ROOT}"
    RESULT_VARIABLE next_result
    OUTPUT_VARIABLE next_stdout
    ERROR_VARIABLE next_stderr
  )
  if(next_result EQUAL 0 OR NOT next_stderr MATCHES
     "self-hosted package 'when' requires a supported scalar constant expression")
    message(FATAL_ERROR
      "${label} did not fail at the self-hosting boundary (${next_result})\n"
      "stdout:\n${next_stdout}stderr:\n${next_stderr}")
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

# A package condition consumes a named scalar constant through an explicit
# product dependency. This smallest case is kept separate from the chained
# scheduler fixture below so a direct regression is easy to diagnose.
set(named_root "${run_root}/named-condition")
file(MAKE_DIRECTORY
  "${named_root}/workspace/app"
  "${named_root}/core"
)
file(WRITE "${named_root}/workspace/app/package.draft"
  "package app\n"
  "Enabled :: true\n"
  "when Enabled { pub Selected :: 1; }\n"
  "main :: proc() {}\n"
)
compare_target_declarations(
  named-constant
  "${named_root}/workspace/app"
  "${named_root}/workspace"
  "${named_root}/core"
  "${HOST_TARGET}"
  success
)

# Logical evaluation may skip the dead operand's value, but both operands still
# require valid Draft types. The condition product must therefore retain its
# named dependency even though the selected value is already known to be false.
file(WRITE "${named_root}/workspace/app/package.draft"
  "package app\n"
  "Enabled :: true\n"
  "when false && Enabled { pub Selected :: 1; }\n"
  "main :: proc() {}\n"
)
compare_target_declarations(
  named-constant-behind-short-circuit
  "${named_root}/workspace/app"
  "${named_root}/workspace"
  "${named_root}/core"
  "${HOST_TARGET}"
  success
)

# Conditions are indexed before constants, while constant candidates retain
# source SymbolId order. The first condition blocks on a forward chain; the
# staging source view runs only demanded constants while that earlier condition
# waits, so a later independent condition cannot reorder the appended SymbolId
# suffix. Its selected branch reveals another named dependency and nested
# condition. Target
# categorical/numeric aliases and a named has_feature string exercise every
# scalar value kind which may cross the product boundary. Unused_Arithmetic is
# deliberately outside the staging evaluator: because no condition demands it,
# its dormant row must not turn a valid package into an eager failure.
set(named_graph_root "${run_root}/named-product-graph")
file(MAKE_DIRECTORY
  "${named_graph_root}/workspace/app"
  "${named_graph_root}/core"
)
file(WRITE "${named_graph_root}/workspace/app/package.draft"
  "package app\n"
  "Early :: true\n"
  "Late :: Base\n"
  "Base :: target.pointer_bits == 64 && target.byte_order == .little\n"
  "Selected_OS :: target.os\n"
  "Selected_Page :: target.page_size\n"
  "Unused_Arithmetic :: 1 + 2\n"
  "when Late && Selected_OS == target.os && Selected_Page >= 4096 {\n"
  "  A_Late_Selected :: 1\n"
  "} else {\n"
  "  A_Late_Selected :: 0\n"
  "}\n"
  "when Early {\n"
  "  B_Early_Selected :: 1\n"
  "  Nested_Enable :: Late\n"
  "  when Nested_Enable { D_Nested_Selected :: 1; }\n"
  "}\n"
  "when target.arch == .aarch64 {\n"
  "  Feature_Name :: \"neon\"\n"
  "  when target.has_feature(Feature_Name) { C_Feature_Selected :: 1; }\n"
  "} else {\n"
  "  Feature_Name :: \"sse2\"\n"
  "  when target.has_feature(Feature_Name) { C_Feature_Selected :: 1; }\n"
  "}\n"
  "main :: proc() {}\n"
)
foreach(target
    aarch64-macos
    aarch64-linux
    x86_64-linux
    x86_64-windows)
  compare_target_declarations(
    "named-product-graph-${target}"
    "${named_graph_root}/workspace/app"
    "${named_graph_root}/workspace"
    "${named_graph_root}/core"
    "${target}"
    success
  )
endforeach()

set(unsupported_root "${run_root}/unsupported-condition")
file(MAKE_DIRECTORY
  "${unsupported_root}/workspace/app"
  "${unsupported_root}/core"
)

# Constant products are demand driven, but their evaluator deliberately has a
# smaller expression vocabulary than the production Draft interpreter. A
# demanded arithmetic initializer must fail explicitly; the same initializer
# was accepted above when its product remained dormant and unrelated.
file(WRITE "${unsupported_root}/workspace/app/package.draft"
  "package app\n"
  "Computed :: 1 + 1\n"
  "when Computed == 2 { Selected :: 1; }\n"
  "main :: proc() {}\n"
)
expect_next_condition_failure(
  demanded-arithmetic-constant
  "${unsupported_root}/workspace/app"
  "${unsupported_root}/workspace"
  "${unsupported_root}/core"
)

# Each attempt consumes only published values. A cycle therefore leaves a
# visible waiting graph instead of recursing through declarations or publishing
# a provisional value.
file(WRITE "${unsupported_root}/workspace/app/package.draft"
  "package app\n"
  "First :: Second\n"
  "Second :: First\n"
  "when First { Selected :: 1; }\n"
  "main :: proc() {}\n"
)
expect_next_condition_failure(
  cyclic-named-constants
  "${unsupported_root}/workspace/app"
  "${unsupported_root}/workspace"
  "${unsupported_root}/core"
)

# has_feature distinguishes a known disabled feature from an unknown spelling.
# The latter is a language error and cannot be treated as ordinary false.
file(WRITE "${unsupported_root}/workspace/app/package.draft"
  "package app\n"
  "when target.has_feature(\"not-a-profile-feature\") { pub Selected :: 1; }\n"
  "main :: proc() {}\n"
)
expect_next_condition_failure(
  unknown-target-feature
  "${unsupported_root}/workspace/app"
  "${unsupported_root}/workspace"
  "${unsupported_root}/core"
)

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
expect_next_condition_failure(
  cross-category-target-alternative
  "${invalid_category_root}/workspace/app"
  "${invalid_category_root}/workspace"
  "${invalid_category_root}/core"
)

message(STATUS
  "draftc-next target declarations matched production condition selections")
file(REMOVE_RECURSE "${run_root}")
