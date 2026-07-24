# Native link-and-run gate for the explicit foreign-provider example.
#
# The source workspace is copied because `draftc build` owns derived `.draft`
# state below its explicit workspace. The provider object is built independently
# by CMake's C compiler and passed through the public logical-name mapping; a
# successful launched program proves semantic declaration, object pinning,
# linker input, symbol spelling, and runtime ABI all agree.

if(NOT DEFINED DRAFTC OR
   NOT DEFINED SOURCE_PACKAGE OR
   NOT DEFINED PROVIDER_OBJECT OR
   NOT DEFINED TEST_ROOT OR
   NOT DEFINED TARGET_SELECTOR)
  message(FATAL_ERROR
    "DRAFTC, SOURCE_PACKAGE, PROVIDER_OBJECT, TEST_ROOT, and TARGET_SELECTOR are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(COPY "${SOURCE_PACKAGE}" DESTINATION "${TEST_ROOT}")
get_filename_component(workspace_name "${SOURCE_PACKAGE}" NAME)
set(workspace "${TEST_ROOT}/${workspace_name}")
file(WRITE "${workspace}/draft.workspace" "draft-workspace-v1\n")
set(program "${TEST_ROOT}/foreign-provider-program")

execute_process(
  COMMAND "${DRAFTC}" build "${workspace}"
    --target "${TARGET_SELECTOR}"
    --provider "custom_math=object:${PROVIDER_OBJECT}"
    -o "${program}"
  RESULT_VARIABLE build_status
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr
)
if(NOT build_status EQUAL 0)
  message(FATAL_ERROR
    "foreign-provider build failed (${build_status})\n"
    "stdout:\n${build_stdout}\nstderr:\n${build_stderr}")
endif()

execute_process(
  COMMAND "${program}"
  WORKING_DIRECTORY "${TEST_ROOT}"
  RESULT_VARIABLE program_status
  OUTPUT_VARIABLE program_stdout
  ERROR_VARIABLE program_stderr
)
if(NOT program_status EQUAL 0)
  message(FATAL_ERROR
    "foreign-provider program failed (${program_status})\n"
    "stdout:\n${program_stdout}\nstderr:\n${program_stderr}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
