# Native equivalence contract for `resolve --build` graph continuation.
#
# Resolution is source-changing even when it revalidates identical pins, so the
# checked-in acceptance fixture is first copied into the build tree. The first
# command revalidates and continues its returned semantic graph into assembly;
# the second performs a later provider-free build of the committed source. Both
# requested output trees must contain byte-identical files. TEST_ROOT owns all
# mutations and is removed on success.

if(NOT DEFINED DRAFTC OR NOT DEFINED SOURCE_WORKSPACE OR
   NOT DEFINED TEST_ROOT OR NOT DEFINED TARGET_SELECTOR)
  message(FATAL_ERROR
    "DRAFTC, SOURCE_WORKSPACE, TEST_ROOT, and TARGET_SELECTOR are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(COPY "${SOURCE_WORKSPACE}" DESTINATION "${TEST_ROOT}")
get_filename_component(workspace_name "${SOURCE_WORKSPACE}" NAME)
set(package "${TEST_ROOT}/${workspace_name}/app")
set(resolve_output "${TEST_ROOT}/resolve-assembly")
set(build_output "${TEST_ROOT}/later-assembly")

execute_process(
  COMMAND "${DRAFTC}" resolve "${package}" --revalidate --build
    --target "${TARGET_SELECTOR}" --kind assembly -o "${resolve_output}"
  RESULT_VARIABLE resolve_status
  OUTPUT_VARIABLE resolve_stdout
  ERROR_VARIABLE resolve_stderr
)
if(NOT resolve_status EQUAL 0)
  message(FATAL_ERROR
    "resolve --build failed (${resolve_status})\nstdout:\n${resolve_stdout}\nstderr:\n${resolve_stderr}")
endif()
if(NOT resolve_stdout MATCHES "resolved 4 synthesis sites" OR
   NOT resolve_stdout MATCHES "built ${resolve_output}")
  message(FATAL_ERROR "resolve --build lost its reports: ${resolve_stdout}")
endif()

execute_process(
  COMMAND "${DRAFTC}" build "${package}"
    --target "${TARGET_SELECTOR}" --kind assembly -o "${build_output}"
  RESULT_VARIABLE build_status
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr
)
if(NOT build_status EQUAL 0)
  message(FATAL_ERROR
    "later build failed (${build_status})\nstdout:\n${build_stdout}\nstderr:\n${build_stderr}")
endif()

file(GLOB_RECURSE resolve_files
  RELATIVE "${resolve_output}" "${resolve_output}/*")
file(GLOB_RECURSE build_files
  RELATIVE "${build_output}" "${build_output}/*")
list(SORT resolve_files)
list(SORT build_files)
if(NOT resolve_files STREQUAL build_files)
  message(FATAL_ERROR
    "resolve-build and later-build file sets differ:\n${resolve_files}\n${build_files}")
endif()
foreach(relative IN LISTS resolve_files)
  if(NOT IS_DIRECTORY "${resolve_output}/${relative}")
    file(SHA256 "${resolve_output}/${relative}" resolve_digest)
    file(SHA256 "${build_output}/${relative}" build_digest)
    if(NOT resolve_digest STREQUAL build_digest)
      message(FATAL_ERROR "native output differs at ${relative}")
    endif()
  endif()
endforeach()

file(REMOVE_RECURSE "${TEST_ROOT}")
