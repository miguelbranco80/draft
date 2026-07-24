# Transaction and native-equivalence contract for `resolve --build`.
#
# Resolution is source-changing even when it revalidates identical pins, so the
# checked-in acceptance fixture is first copied into the build tree. The first
# command revalidates and continues its returned semantic graph into assembly;
# the second performs a later provider-free build of the committed source. Both
# requested output trees must contain byte-identical files. TEST_ROOT owns all
# mutations and is removed on success. A final invalid-assembly case proves the
# source transaction remains committed when post-commit artifact I/O fails.

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
set(workspace "${TEST_ROOT}/${workspace_name}")
set(resolve_output "${TEST_ROOT}/resolve-assembly")
set(build_output "${TEST_ROOT}/later-assembly")

execute_process(
  COMMAND "${DRAFTC}" resolve "${workspace}/app" --revalidate --build
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
  COMMAND "${DRAFTC}" build "${workspace}/app"
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

# Backend failure occurs after resolution has accepted and committed ordinary
# source. Add one valid checked declaration, then occupy the requested assembly
# output path with a regular file. Parsed assembly is deliberately not used as
# the failure oracle: it is now a checked-body product and therefore fails
# before a source transaction can commit. The revalidated manifest must change
# despite artifact-directory creation failing, and a later provider-free check
# must consume that committed source normally.
execute_process(
  COMMAND "${DRAFTC}" target --target "${TARGET_SELECTOR}"
  RESULT_VARIABLE target_status
  OUTPUT_VARIABLE target_stdout
  ERROR_VARIABLE target_stderr
)
if(NOT target_status EQUAL 0 OR
   NOT target_stdout MATCHES "identity ([^\r\n ]+)")
  message(FATAL_ERROR
    "cannot identify selected target (${target_status})\n"
    "stdout:\n${target_stdout}\nstderr:\n${target_stderr}")
endif()
set(target_identity "${CMAKE_MATCH_1}")
set(manifest
  "${workspace}/.draft/resolutions/${target_identity}/packages/app/resolution.json")
if(NOT EXISTS "${manifest}")
  message(FATAL_ERROR
    "selected app resolution manifest does not exist: ${manifest}")
endif()
file(SHA256 "${manifest}" manifest_before_backend_failure)
file(WRITE "${package}/committed_source_change.draft"
  "package app\n\n"
  "committed_after_backend_failure :: proc(value: u64) -> u64 {\n"
  "    return value + 1\n"
  "}\n")
set(blocked_output "${TEST_ROOT}/occupied-assembly-output")
file(WRITE "${blocked_output}" "not a directory\n")

execute_process(
  COMMAND "${DRAFTC}" resolve "${workspace}/app" --revalidate --build
    --target "${TARGET_SELECTOR}" --kind assembly
    -o "${blocked_output}"
  RESULT_VARIABLE failed_build_status
  OUTPUT_VARIABLE failed_build_stdout
  ERROR_VARIABLE failed_build_stderr
)
if(failed_build_status EQUAL 0)
  message(FATAL_ERROR "occupied output path unexpectedly produced an artifact")
endif()
if(NOT failed_build_stderr MATCHES
   "assembly output path must be a non-symlink directory")
  message(FATAL_ERROR
    "resolve --build failed for the wrong reason\n"
    "stdout:\n${failed_build_stdout}\nstderr:\n${failed_build_stderr}")
endif()
file(SHA256 "${manifest}" manifest_after_backend_failure)
if(manifest_before_backend_failure STREQUAL manifest_after_backend_failure)
  message(FATAL_ERROR
    "backend failure discarded the successful source transaction")
endif()

execute_process(
  COMMAND "${DRAFTC}" check "${workspace}/app"
    --target "${TARGET_SELECTOR}"
  RESULT_VARIABLE committed_check_status
  OUTPUT_VARIABLE committed_check_stdout
  ERROR_VARIABLE committed_check_stderr
)
if(NOT committed_check_status EQUAL 0 OR
   NOT committed_check_stdout MATCHES "checked package graph rooted at app")
  message(FATAL_ERROR
    "provider-free check could not consume the committed failed-build source\n"
    "stdout:\n${committed_check_stdout}\nstderr:\n${committed_check_stderr}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
