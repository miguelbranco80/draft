# Public direct-run qualification for every ordinary executable example.
#
# qualification.tsv remains the exhaustive inventory. Each `run` row is copied
# into one private repository-shaped workspace and launched through the public
# `draftc run` command. A standalone package is addressed by its directory; a
# dedicated multi-package workspace is addressed by its workspace directory so
# the checked-in `default` selection is exercised. The repository-root Turbo
# programs are addressed exactly because that workspace intentionally contains
# more than one program. Provider, raylib, resolved-agent, and library classes
# retain their specialized native gates.

if(NOT DEFINED DRAFTC OR
   NOT DEFINED SOURCE_ROOT OR
   NOT DEFINED MATRIX OR
   NOT DEFINED TEST_ROOT OR
   NOT DEFINED TARGET_SELECTOR)
  message(FATAL_ERROR
    "DRAFTC, SOURCE_ROOT, MATRIX, TEST_ROOT, and TARGET_SELECTOR are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(COPY "${SOURCE_ROOT}/examples" DESTINATION "${TEST_ROOT}")
file(COPY "${SOURCE_ROOT}/lib" DESTINATION "${TEST_ROOT}")
file(COPY "${SOURCE_ROOT}/draft.workspace" DESTINATION "${TEST_ROOT}")
file(WRITE "${TEST_ROOT}/input.txt" "q\n")
file(MAKE_DIRECTORY "${TEST_ROOT}/artifacts")

file(STRINGS "${MATRIX}" matrix_lines)
set(run_count 0)
foreach(line IN LISTS matrix_lines)
  if(line STREQUAL "" OR line MATCHES "^[ \t]*#")
    continue()
  endif()
  string(REPLACE "\t" ";" fields "${line}")
  list(GET fields 0 workspace)
  list(GET fields 1 root)
  list(GET fields 3 native)
  if(NOT native STREQUAL "run" AND NOT native STREQUAL "c-library")
    continue()
  endif()

  if(workspace STREQUAL ".")
    set(package_relative "${root}")
    set(command_relative "${root}")
  elseif(root STREQUAL ".")
    set(package_relative "${workspace}")
    set(command_relative "${workspace}")
  else()
    set(package_relative "${workspace}/${root}")
    # A dedicated example workspace has one named executable and must make its
    # root directory the natural run command through `default`. The shared
    # examples/ inventory is not itself such a workspace.
    if(workspace STREQUAL "examples")
      set(command_relative "${package_relative}")
    else()
      set(command_relative "${workspace}")
    endif()
  endif()

  string(REPLACE "/" "-" artifact_name "${package_relative}")
  set(build_only FALSE)
  if(native STREQUAL "c-library")
    # No --kind appears here: the checked-in workspace is useful precisely
    # because its durable dynamic-library policy makes the direct build valid.
    set(build_only TRUE)
  elseif(package_relative STREQUAL "examples/runtime-traps")
    # Every meaningful runtime-traps invocation is expected to terminate
    # through its selected trap. Its specialized conformance gate launches all
    # selectors; this public-command proof only needs to build the package.
    set(build_only TRUE)
  endif()
  if(build_only)
    set(command
      "${DRAFTC}" build "${TEST_ROOT}/${command_relative}"
      --target "${TARGET_SELECTOR}"
      -o "${TEST_ROOT}/artifacts/${artifact_name}"
    )
  else()
    set(command
      "${DRAFTC}" run "${TEST_ROOT}/${command_relative}"
      --target "${TARGET_SELECTOR}"
      -o "${TEST_ROOT}/artifacts/${artifact_name}"
    )
    if(package_relative STREQUAL "examples/simple-editor")
      list(APPEND command -- document.txt)
    elseif(package_relative STREQUAL "examples/tetris" OR
           package_relative STREQUAL "examples/turbo-editor" OR
           package_relative STREQUAL "examples/turbo-ui-gallery")
      list(APPEND command -- --smoke)
    endif()
  endif()

  execute_process(
    COMMAND ${command}
    WORKING_DIRECTORY "${TEST_ROOT}"
    INPUT_FILE "${TEST_ROOT}/input.txt"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE standard_output
    ERROR_VARIABLE standard_error
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR
      "direct run failed for ${package_relative} via ${command_relative} "
      "(${result})\nstdout:\n${standard_output}\nstderr:\n${standard_error}")
  endif()
  math(EXPR run_count "${run_count} + 1")
endforeach()

file(REMOVE_RECURSE "${TEST_ROOT}")
message(STATUS
  "qualified ${run_count} direct executable/library example commands")
