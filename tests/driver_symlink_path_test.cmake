# Public CLI regression for a package reached through a symlinked parent.
#
# The resolution store deliberately refuses symlink components after it accepts
# a workspace root. The driver must therefore canonicalize the existing package
# before deriving that root. An ordinary build-tree symlink reproduces the same
# boundary as macOS `/tmp` without relying on host-specific global paths. A
# provider-free check is sufficient because it opens the same resolution store
# before compiling the handwritten package.

if(NOT DEFINED DRAFTC OR NOT DEFINED SOURCE_PACKAGE OR NOT DEFINED TEST_ROOT)
  message(FATAL_ERROR "driver symlink-path test is missing an input")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/real/workspace/hello")
file(COPY "${SOURCE_PACKAGE}/" DESTINATION "${TEST_ROOT}/real/workspace/hello")
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
  COMMAND "${DRAFTC}" check "${TEST_ROOT}/workspace-link/hello"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
  message(FATAL_ERROR
    "check through symlinked parent failed (${result})\n${output}${error}")
endif()
if(NOT output MATCHES "checked package graph rooted at hello")
  message(FATAL_ERROR
    "check through symlinked parent returned unexpected output\n${output}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
