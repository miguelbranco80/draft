# Exact differential test for the first self-hosted typed package interface.
#
# Both executables emit the established graph, declaration, target-selection,
# and public-name prefix before the canonical scalar/structural type graph.
# Imported rows are compared through structural consumer-local type shapes so
# unrelated production-only private interning cannot affect the result. Scratch
# is process-unique below the caller-provided binary directory.

foreach(required ORACLE DRAFTC_NEXT SOURCE_ROOT TEST_ROOT HOST_TARGET)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "missing required -D${required}=...")
  endif()
endforeach()
if(NOT EXISTS "${ORACLE}")
  message(FATAL_ERROR "typed-interface oracle does not exist: ${ORACLE}")
endif()
if(NOT EXISTS "${DRAFTC_NEXT}")
  message(FATAL_ERROR "Draft-written draftc-next does not exist: ${DRAFTC_NEXT}")
endif()

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef run_identity)
set(run_root "${TEST_ROOT}/${run_identity}")
file(MAKE_DIRECTORY "${run_root}")

function(compare_interfaces label root workspace core target)
  set(common_arguments
    workspace-interfaces "${root}"
    --workspace "${workspace}"
    --core "${core}"
    --core-identity draft-core-fixture
    --target "${target}"
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
  if(NOT oracle_result EQUAL 0 OR NOT next_result EQUAL 0 OR
     NOT stdout_result EQUAL 0 OR NOT stderr_result EQUAL 0)
    file(READ "${run_root}/oracle.stdout" oracle_stdout)
    file(READ "${run_root}/next.stdout" next_stdout)
    file(READ "${run_root}/oracle.stderr" oracle_stderr)
    file(READ "${run_root}/next.stderr" next_stderr)
    message(FATAL_ERROR
      "workspace-interfaces implementations differ for ${label}\n"
      "oracle result: ${oracle_result}\n"
      "next result: ${next_result}\n"
      "--- oracle stdout ---\n${oracle_stdout}"
      "--- next stdout ---\n${next_stdout}"
      "--- oracle stderr ---\n${oracle_stderr}"
      "--- next stderr ---\n${next_stderr}"
    )
  endif()
endfunction()

function(expect_next_interface_failure label source expected_message)
  set(failure_root "${run_root}/${label}")
  file(MAKE_DIRECTORY
    "${failure_root}/workspace/app"
    "${failure_root}/core"
  )
  file(WRITE "${failure_root}/workspace/app/package.draft" "${source}")
  execute_process(
    COMMAND "${DRAFTC_NEXT}"
      workspace-interfaces "${failure_root}/workspace/app"
      --workspace "${failure_root}/workspace"
      --core "${failure_root}/core"
      --core-identity draft-core-fixture
      --target "${HOST_TARGET}"
    WORKING_DIRECTORY "${SOURCE_ROOT}"
    RESULT_VARIABLE next_result
    OUTPUT_VARIABLE next_stdout
    ERROR_VARIABLE next_stderr
  )
  if(next_result EQUAL 0 OR NOT next_stderr MATCHES "${expected_message}")
    message(FATAL_ERROR
      "${label} did not fail at the typed-interface boundary (${next_result})\n"
      "stdout:\n${next_stdout}stderr:\n${next_stderr}")
  endif()
endfunction()

set(interface_root "${run_root}/supported")
set(interface_workspace "${interface_root}/workspace")
set(interface_core "${interface_root}/core")
file(MAKE_DIRECTORY
  "${interface_workspace}/app"
  "${interface_workspace}/values"
  "${interface_core}"
)
file(WRITE "${interface_workspace}/values/package.draft"
  "package values\n"
  "pub Count :: 4\n"
  "pub Index :: u32\n"
  "pub Limit :: 42\n"
  "pub Enabled :: true\n"
  "pub Label :: \"ready\"\n"
  "pub Forward :: Later\n"
  "Later :: u16\n"
  "pub Forward_View :: Later_View\n"
  "Later_View :: []Index\n"
  "pub Index_Pointer :: ^Index\n"
  "pub Index_Multi :: [^]Index\n"
  "pub Index_Slice :: []Index\n"
  "pub Index_Array :: [Count]Index\n"
  "pub Forward_Array :: [Later_Count]Index\n"
  "Later_Count :: Count\n"
  "pub Index_Tuple :: (Index_Pointer, Index_Array)\n"
  "pub Transformer :: proc(value: Index_Slice) -> Index_Pointer\n"
  "pub Page_Array :: [target.page_size]u8\n"
  "pub Storage: Index_Array\n"
  "pub transform :: proc(values: Index_Slice, cursor: Index_Multi) -> Index_Pointer { return nil; }\n"
  "pub pair :: proc(pointer: Index_Pointer, values: Index_Array) -> (Index_Pointer, Index_Array) { return (pointer, values); }\n"
)
file(WRITE "${interface_workspace}/app/package.draft"
  "package app\n"
  "import values\n"
  "pub Local_Index :: values.Index\n"
  "pub Local_Limit :: values.Limit\n"
  "pub Local_Enabled :: values.Enabled\n"
  "pub Local_Label :: values.Label\n"
  "pub Local_Pointer :: values.Index_Pointer\n"
  "pub Local_Multi :: values.Index_Multi\n"
  "pub Local_Slice :: values.Index_Slice\n"
  "pub Local_Array :: values.Index_Array\n"
  "pub Local_Tuple :: values.Index_Tuple\n"
  "pub Local_Transformer :: values.Transformer\n"
  "pub Nested_Array :: [2]values.Index_Array\n"
  "pub Imported_Count_Array :: [values.Count]values.Index\n"
  "pub Direct_Tuple :: ([]values.Index, ^values.Index_Tuple)\n"
  "pub Storage: values.Index_Array\n"
  "pub Cursor: values.Index_Multi\n"
  "pub copy :: proc(value: []values.Index, cursor: [^]values.Index) -> ^values.Index { return nil; }\n"
  "main :: proc() {}\n"
)

foreach(target
    aarch64-macos
    aarch64-linux
    x86_64-linux
    x86_64-windows)
  compare_interfaces(
    "scalar-structural-${target}"
    "${interface_workspace}/app"
    "${interface_workspace}"
    "${interface_core}"
    "${target}"
  )
endforeach()

# Unsupported canonical constructors must fail as an implementation boundary,
# not be flattened into a scalar or silently omitted from the public interface.
expect_next_interface_failure(
  unsupported-aggregate
  "package app\npub Record :: struct { value: u32; }\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar or structural declaration"
)

# Distinct types are nominal identities even when their underlying storage is a
# supported scalar, so they remain outside this structural slice.
expect_next_interface_failure(
  unsupported-distinct
  "package app\npub Handle :: distinct u32\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar or structural declaration"
)

# Array counts reuse the current scalar constant-product evaluator. Arithmetic
# remains a later constant-evaluation slice and must not be guessed or folded by
# a second array-only implementation.
expect_next_interface_failure(
  unsupported-array-count-arithmetic
  "package app\npub Bytes :: [2 + 2]u8\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar or structural declaration"
)

# A concrete uint is not an implicit usize merely because both current target
# widths are 64 bits. Only an untyped representable constant or exact usize may
# supply the fixed-array count.
expect_next_interface_failure(
  unsupported-array-count-type
  "package app\npub Bytes :: [target.pointer_bits]u8\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar or structural declaration"
)

# Local declaration cycles remain visible graph edges and receive a distinct
# cycle diagnostic instead of recursing through source declarations.
expect_next_interface_failure(
  cyclic-types
  "package app\npub First :: Second\nSecond :: First\nmain :: proc() {}\n"
  "self-hosted typed interface contains a cyclic declaration dependency"
)

message(STATUS
  "draftc-next typed interfaces matched production scalar/structural graphs")
file(REMOVE_RECURSE "${run_root}")
