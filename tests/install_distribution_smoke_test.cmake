# Install the current CMake build and pass the isolated prefix to the common
# distribution smoke. Keeping installation and execution separate lets release
# CI reuse the exact same execution contract against an extracted CPack archive.

foreach(required IN ITEMS
    BUILD_DIRECTORY SOURCE_ROOT TEST_ROOT DRAFT_TARGET DRAFT_RELEASE_VERSION)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

# Keep the installed prefix outside the disposable execution directory: the
# common smoke intentionally clears TEST_ROOT before copying its example.
set(prefix "${TEST_ROOT}-root")
file(REMOVE_RECURSE "${TEST_ROOT}" "${prefix}")

set(install_command
  "${CMAKE_COMMAND}" --install "${BUILD_DIRECTORY}" --prefix "${prefix}"
)
if(
  DEFINED CMAKE_INSTALL_CONFIG_NAME
  AND NOT "${CMAKE_INSTALL_CONFIG_NAME}" STREQUAL ""
)
  list(APPEND install_command --config "${CMAKE_INSTALL_CONFIG_NAME}")
endif()
execute_process(
  COMMAND ${install_command}
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error
)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR
    "Distribution install failed (${install_result})\n"
    "stdout:\n${install_output}\nstderr:\n${install_error}"
  )
endif()

set(DRAFT_ROOT "${prefix}")
include("${SOURCE_ROOT}/tests/distribution_smoke_test.cmake")
