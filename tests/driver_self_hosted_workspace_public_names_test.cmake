# Exact differential test for unconditional public names and import lookup.
#
# Both executables emit the established graph/declaration view followed by each
# direct unconditional public definition and every file-local alias member. The
# C++ oracle obtains acceptance/order from final production PackageInterfaces,
# then intersects them with source collector symbols because conditional `when`
# materialization is explicitly outside this Draft boundary. Scratch is unique
# below the caller-provided binary directory and removed after qualification.

foreach(required ORACLE DRAFTC_NEXT SOURCE_ROOT TEST_ROOT HOST_TARGET)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "missing required -D${required}=...")
  endif()
endforeach()
if(NOT EXISTS "${ORACLE}")
  message(FATAL_ERROR "workspace-public-names oracle does not exist: ${ORACLE}")
endif()
if(NOT EXISTS "${DRAFTC_NEXT}")
  message(FATAL_ERROR "Draft-written draftc-next does not exist: ${DRAFTC_NEXT}")
endif()

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef run_identity)
set(run_root "${TEST_ROOT}/${run_identity}")
file(MAKE_DIRECTORY "${run_root}")

# Additional arguments are complete `--dependency prefix root identity` groups.
# expected prevents a shared early rejection from satisfying the differential.
function(compare_public_names label root workspace core target expected)
  set(common_arguments
    workspace-public-names "${root}"
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
      "workspace-public-names implementations differ for ${label}\n"
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

# The real staging graph includes repeated aliases, every compiler package, and
# core/c_abi's conditional public long aliases. The latter remain absent from
# this earlier source-only view on both sides of the differential.
compare_public_names(
  repository-frontend
  "${SOURCE_ROOT}/tools/draftc_next"
  "${SOURCE_ROOT}"
  "${SOURCE_ROOT}/core"
  "${HOST_TARGET}"
  success
)

# Public names cross package boundaries; private declarations and imported
# aliases do not. The same local alias spelling may name different packages in
# different files, while repeated imports of math get independent member spans
# that point to the same defining SymbolIds.
set(matrix_root "${run_root}/matrix")
set(matrix_workspace "${matrix_root}/workspace")
set(matrix_dependency "${matrix_root}/dependency")
set(matrix_core "${matrix_root}/core")
file(MAKE_DIRECTORY
  "${matrix_workspace}/app"
  "${matrix_workspace}/lib/base"
  "${matrix_workspace}/lib/math"
  "${matrix_dependency}/text"
  "${matrix_core}/io"
)
file(WRITE "${matrix_workspace}/app/a.draft"
  "package app\n"
  "import lib/math as dependency\n"
  "import vendor/text as text\n"
  "import core/io\n"
  "pub App_Result :: distinct u64\n"
  "consume :: proc(value: dependency.Number, size: text.Size, handle: io.Handle) -> dependency.Number {\n"
  "  return value\n"
  "}\n"
)
file(WRITE "${matrix_workspace}/app/b.draft"
  "package app\n"
  "import lib/base as dependency\n"
  "import lib/math\n"
  "mirror :: proc(value: math.Number) -> math.Number {\n"
  "  return value\n"
  "}\n"
)
file(WRITE "${matrix_workspace}/lib/base/package.draft"
  "package base\n"
  "pub Flag :: distinct u8\n"
  "Hidden_Base :: distinct u16\n"
)
file(WRITE "${matrix_workspace}/lib/math/package.draft"
  "package math\n"
  "import lib/base\n"
  "pub Number :: distinct u64\n"
  "pub Alias :: Number\n"
  "pub First, Second :: 1\n"
  "Private_Number :: distinct u32\n"
  "when target.os == .windows {\n"
  "  pub Conditional_Number :: distinct u32\n"
  "} else {\n"
  "  pub Conditional_Number :: distinct u64\n"
  "}\n"
)
file(WRITE "${matrix_dependency}/text/package.draft"
  "package text\n"
  "pub Size :: distinct usize\n"
  "Private_Text :: 1\n"
)
file(WRITE "${matrix_core}/io/package.draft"
  "package io\n"
  "pub Handle :: distinct uintptr\n"
  "Private_Handle :: distinct uintptr\n"
)
compare_public_names(
  visibility-and-alias-locality
  "${matrix_workspace}/app"
  "${matrix_workspace}"
  "${matrix_core}"
  "${HOST_TARGET}"
  success
  --dependency vendor "${matrix_dependency}" vendor-content
)

# Target-qualified files change both a package's direct public view and every
# imported alias span. Exercise all selectors rather than assuming host facts.
set(target_root "${run_root}/target-selection")
file(MAKE_DIRECTORY
  "${target_root}/workspace/app"
  "${target_root}/workspace/platform"
  "${target_root}/core"
)
file(WRITE "${target_root}/workspace/app/package.draft"
  "package app\nimport platform\nmain :: proc() {}\n"
)
file(WRITE "${target_root}/workspace/platform/package.draft"
  "package platform\npub Shared :: distinct u8\nPrivate :: 1\n"
)
file(WRITE "${target_root}/workspace/platform/value@aarch64-macos.draft"
  "package platform\npub Selected_Aarch64_Macos :: distinct u16\n"
)
file(WRITE "${target_root}/workspace/platform/value@aarch64-linux.draft"
  "package platform\npub Selected_Aarch64_Linux :: distinct u16\n"
)
file(WRITE "${target_root}/workspace/platform/value@x86_64-linux.draft"
  "package platform\npub Selected_X86_64_Linux :: distinct u16\n"
)
file(WRITE "${target_root}/workspace/platform/value@x86_64-windows.draft"
  "package platform\npub Selected_X86_64_Windows :: distinct u16\n"
)
foreach(target
    aarch64-macos
    aarch64-linux
    x86_64-linux
    x86_64-windows)
  compare_public_names(
    "target-${target}"
    "${target_root}/workspace/app"
    "${target_root}/workspace"
    "${target_root}/core"
    "${target}"
    success
  )
endforeach()

# Declaration failure leaves the exact partial declaration view and never
# attempts public-name binding or full compilation.
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
compare_public_names(
  declaration-duplicate
  "${duplicate_root}/workspace/app"
  "${duplicate_root}/workspace"
  "${duplicate_root}/core"
  "${HOST_TARGET}"
  failure
)

# Graph failure likewise stops before declarations/public names and preserves
# the established graph diagnostic and nonzero status.
set(missing_root "${run_root}/missing-import")
file(MAKE_DIRECTORY
  "${missing_root}/workspace/app"
  "${missing_root}/core"
)
file(WRITE "${missing_root}/workspace/app/package.draft"
  "package app\nimport absent/package\n"
)
compare_public_names(
  missing-import
  "${missing_root}/workspace/app"
  "${missing_root}/workspace"
  "${missing_root}/core"
  "${HOST_TARGET}"
  failure
)

message(STATUS
  "draftc-next public-name lookup matched production interface binding")
file(REMOVE_RECURSE "${run_root}")
