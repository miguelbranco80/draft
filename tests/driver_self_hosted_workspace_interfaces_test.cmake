# Exact differential test for the first self-hosted typed package interface.
#
# Both executables emit the established graph, declaration, target-selection,
# and public-name prefix before the new canonical scalar/procedure type graph.
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
  "pub Index :: u32\n"
  "pub Limit :: 42\n"
  "pub Enabled :: true\n"
  "pub Label :: \"ready\"\n"
  "pub Forward :: Later\n"
  "Later :: u16\n"
  "pub Storage: Index\n"
  "pub transform :: proc(value: Index) -> bool { return Enabled; }\n"
)
file(WRITE "${interface_workspace}/app/package.draft"
  "package app\n"
  "import values\n"
  "pub Local_Index :: values.Index\n"
  "pub Local_Limit :: values.Limit\n"
  "pub Local_Enabled :: values.Enabled\n"
  "pub Local_Label :: values.Label\n"
  "pub Storage: values.Index\n"
  "pub copy :: proc(value: values.Index) -> bool { return values.Enabled; }\n"
  "main :: proc() {}\n"
)

foreach(target
    aarch64-macos
    aarch64-linux
    x86_64-linux
    x86_64-windows)
  compare_interfaces(
    "scalar-procedure-${target}"
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
  "self-hosted typed interface requires a supported scalar or procedure declaration"
)

# Local declaration cycles remain visible graph edges and receive a distinct
# cycle diagnostic instead of recursing through source declarations.
expect_next_interface_failure(
  cyclic-types
  "package app\npub First :: Second\nSecond :: First\nmain :: proc() {}\n"
  "self-hosted typed interface contains a cyclic declaration dependency"
)

message(STATUS
  "draftc-next typed interfaces matched production scalar/procedure graphs")
file(REMOVE_RECURSE "${run_root}")
