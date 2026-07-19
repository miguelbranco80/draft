# Public CLI regression for a package reached through a symlinked parent.
#
# The resolution store deliberately refuses symlink components after it accepts
# a workspace root. The driver must therefore canonicalize the existing package
# before deriving that root. An ordinary build-tree symlink reproduces the same
# boundary as macOS `/tmp` without relying on host-specific global paths.

if(NOT DEFINED DRAFTC OR NOT DEFINED RECORDING_TOOL OR
   NOT DEFINED SOURCE_PACKAGE OR NOT DEFINED TEST_ROOT)
  message(FATAL_ERROR "driver symlink-path test is missing an input")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/real/workspace/hello")
file(COPY "${SOURCE_PACKAGE}/" DESTINATION "${TEST_ROOT}/real/workspace/hello")
file(MAKE_DIRECTORY "${TEST_ROOT}/toolchain/bin")
file(MAKE_DIRECTORY "${TEST_ROOT}/sdk/usr/lib")
foreach(tool IN ITEMS clang ld ld-classic llvm-ar dsymutil)
  file(COPY_FILE
    "${RECORDING_TOOL}"
    "${TEST_ROOT}/toolchain/bin/${tool}"
  )
  file(CHMOD "${TEST_ROOT}/toolchain/bin/${tool}"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
endforeach()
file(WRITE "${TEST_ROOT}/sdk/usr/lib/libSystem.tbd" "test SDK bytes\n")
file(CREATE_LINK
  "${TEST_ROOT}/real/workspace"
  "${TEST_ROOT}/workspace-link"
  SYMBOLIC
  RESULT link_result
)
if(NOT link_result STREQUAL "0")
  message(FATAL_ERROR "cannot create driver test symlink: ${link_result}")
endif()

execute_process(
  COMMAND "${DRAFTC}" resolve "${TEST_ROOT}/workspace-link/hello"
    --toolchain-root "${TEST_ROOT}/toolchain"
    --sdk-root "${TEST_ROOT}/sdk"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
  message(FATAL_ERROR
    "resolve through symlinked parent failed (${result})\n${output}${error}")
endif()
if(NOT EXISTS "${TEST_ROOT}/real/workspace/.draft/resolution.json")
  message(FATAL_ERROR
    "resolve did not publish its manifest under the canonical workspace")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
