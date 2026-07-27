# Native Windows qualification through the public Draft driver.
#
# This script is intentionally independent of the POSIX-oriented C++ native
# harnesses. A Windows CI host supplies a freshly built draftc, the matching
# LLVM Clang, and a private scratch root. The script copies repository examples
# into that root, builds and launches every ordinary runnable example selected
# by qualification.tsv, exercises target-specific package assembly, then closes
# the independent-C-consumer loop around a Draft DLL. It also publishes a
# multi-package static library and a one-module COFF object so every initial
# Windows artifact family reaches a real tool rather than only an argument
# recorder.
#
# Runtime-traps is the one deliberate exclusion: its success condition is a
# POSIX target-specific signal classification. Windows exception-code matching
# belongs in the later cross-platform validation runner, not in a shell-shaped
# smoke script. Draft tests/benchmarks remain outside this gate for the same
# reason. Every invoked program otherwise must launch and return zero. Derived
# state is confined to TEST_ROOT so simultaneous worktrees and CI jobs never
# share it.

if(NOT DEFINED DRAFTC OR
   NOT DEFINED CLANG OR
   NOT DEFINED SOURCE_ROOT OR
   NOT DEFINED TEST_ROOT)
  message(FATAL_ERROR "DRAFTC, CLANG, SOURCE_ROOT, and TEST_ROOT are required")
endif()

set(target_selector x86_64-windows)

# Runs one command and preserves its complete output in the failure diagnostic.
# Commands are passed as an argument list; no source path or option is evaluated
# by a shell. A successful command may print anything because program exit is
# the native conformance contract for the general example matrix.
function(run_checked label)
  execute_process(
    COMMAND ${ARGN}
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status STREQUAL "0")
    message(FATAL_ERROR
      "${label} failed (${status})\nstdout:\n${stdout}\nstderr:\n${stderr}")
  endif()
endfunction()

# Launches one built example from its private directory. Editor input uses a
# real file as stdin so the program sees the same q/newline bytes as the POSIX
# native harness without requiring an interactive console.
function(run_example label executable working_directory argument input_file)
  if(input_file)
    execute_process(
      COMMAND "${executable}" ${argument}
      WORKING_DIRECTORY "${working_directory}"
      INPUT_FILE "${input_file}"
      RESULT_VARIABLE status
      OUTPUT_VARIABLE stdout
      ERROR_VARIABLE stderr
    )
  else()
    execute_process(
      COMMAND "${executable}" ${argument}
      WORKING_DIRECTORY "${working_directory}"
      RESULT_VARIABLE status
      OUTPUT_VARIABLE stdout
      ERROR_VARIABLE stderr
    )
  endif()
  if(NOT status STREQUAL "0")
    message(FATAL_ERROR
      "${label} launch failed (${status})\nstdout:\n${stdout}\nstderr:\n${stderr}")
  endif()
endfunction()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(COPY "${SOURCE_ROOT}/examples" DESTINATION "${TEST_ROOT}")
file(COPY "${SOURCE_ROOT}/lib" DESTINATION "${TEST_ROOT}")
# Mirror the source repository's one top-level workspace boundary. This keeps
# every copied package and top-level library in one import namespace without
# inheriting a marker from the CMake build directory above TEST_ROOT.
file(WRITE "${TEST_ROOT}/draft.workspace" "draft-workspace-v1\n")

run_checked(
  "Windows target report"
  "${DRAFTC}" target --target "${target_selector}"
)

# qualification.tsv is the one repository inventory for runnable examples.
# Preserve row order so a failure is stable and matches every other native
# host. Library/dependency/provider rows are handled by focused sections below.
file(STRINGS "${SOURCE_ROOT}/examples/qualification.tsv" qualification_rows)
foreach(row IN LISTS qualification_rows)
  if(row STREQUAL "" OR row MATCHES "^#")
    continue()
  endif()
  string(REPLACE "\t" ";" fields "${row}")
  list(LENGTH fields field_count)
  if(NOT field_count EQUAL 5)
    message(FATAL_ERROR "malformed qualification row: ${row}")
  endif()
  list(GET fields 0 workspace_relative)
  list(GET fields 1 root_package)
  list(GET fields 3 native_class)
  if(NOT native_class STREQUAL "run")
    continue()
  endif()

  if(workspace_relative STREQUAL "examples" AND
     root_package STREQUAL "runtime-traps")
    continue()
  endif()

  set(workspace "${TEST_ROOT}/${workspace_relative}")
  string(REPLACE "/" "-" case_name "${root_package}")
  string(REPLACE "." "root" case_name "${case_name}")
  get_filename_component(workspace_name "${workspace_relative}" NAME)
  set(case_directory "${TEST_ROOT}/native/${workspace_name}-${case_name}")
  file(MAKE_DIRECTORY "${case_directory}")
  set(program "${case_directory}/program.exe")

  run_checked(
    "${workspace_relative}:${root_package} build"
    "${DRAFTC}" build "${workspace}/${root_package}"
      --target "${target_selector}" -o "${program}"
  )

  set(argument)
  set(input_file)
  if(workspace_relative STREQUAL "examples" AND
     root_package STREQUAL "simple-editor")
    set(argument document.txt)
    set(input_file "${case_directory}/editor-input.txt")
    file(WRITE "${input_file}" "q\n")
  elseif((workspace_relative STREQUAL "examples" AND
         root_package STREQUAL "tetris") OR
         (workspace_relative STREQUAL "." AND
          root_package STREQUAL "examples/turbo-editor") OR
         (workspace_relative STREQUAL "." AND
          root_package STREQUAL "examples/turbo-ui-gallery"))
    set(argument --smoke)
  endif()
  run_example(
    "${workspace_relative}:${root_package}"
    "${program}"
    "${case_directory}"
    "${argument}"
    "${input_file}"
  )
endforeach()

# Exercise the remaining COFF publication forms. The one-package hello graph
# is legal as one `.obj` and exposes one generated assembly module; c-library
# imports core/runtime and therefore proves deterministic llvm-lib archiving of
# a multi-package object set.
set(artifact_root "${TEST_ROOT}/artifacts")
file(MAKE_DIRECTORY "${artifact_root}")
run_checked(
  "COFF object publication"
  "${DRAFTC}" build "${TEST_ROOT}/examples/hello"
    --target "${target_selector}" --kind object
    -o "${artifact_root}/hello.obj"
)
set(assembly_bundle "${artifact_root}/hello-assembly")
run_checked(
  "Windows assembly-bundle publication"
  "${DRAFTC}" build "${TEST_ROOT}/examples/hello"
    --target "${target_selector}" --kind assembly
    -o "${assembly_bundle}"
)
if(NOT EXISTS "${assembly_bundle}/package-0-unit-0.s")
  message(FATAL_ERROR
    "Windows assembly build did not publish its package LLVM unit")
endif()
run_checked(
  "COFF static-library publication"
  "${DRAFTC}" build "${TEST_ROOT}/examples/c-library"
    --target "${target_selector}" --kind static-library
    -o "${artifact_root}/draft-c-library-static.lib"
)

# Build the Draft DLL and its generated target-selected C11 header, then compile
# and launch the checked-in C client with the independent Clang frontend. The
# client covers Win64 aggregates, callbacks, package TLS, ordinary Draft reentry,
# and lazy attachment/destruction on a Windows-created foreign thread.
set(header "${artifact_root}/draft-c-library.h")
set(dll "${artifact_root}/draft-c-library.dll")
set(import_library "${artifact_root}/draft-c-library.lib")
set(client "${artifact_root}/draft-c-library-client.exe")
run_checked(
  "Windows C header emission"
  "${DRAFTC}" emit-c-header "${TEST_ROOT}/examples/c-library"
    --target "${target_selector}" -o "${header}"
)
run_checked(
  "Windows Draft DLL build"
  "${DRAFTC}" build "${TEST_ROOT}/examples/c-library"
    --target "${target_selector}" --kind dynamic-library --debug-symbols
    -o "${dll}"
)
# This invocation deliberately opts into debug information. The ordinary fast
# path omits a PDB by contract; requiring one without --debug-symbols would test
# the opposite of the public CLI policy rather than qualify Windows publication.
foreach(companion IN ITEMS "${dll}" "${import_library}" "${artifact_root}/draft-c-library.pdb")
  if(NOT EXISTS "${companion}")
    message(FATAL_ERROR "Windows DLL build did not publish ${companion}")
  endif()
endforeach()
run_checked(
  "Windows C client compile"
  "${CLANG}" -target x86_64-pc-windows-msvc -std=c11
    -Wall -Wextra -Werror "-I${artifact_root}"
    "${SOURCE_ROOT}/examples/c-library/client.c" "${import_library}"
    -o "${client}"
)
run_example(
  "Windows C client"
  "${client}"
  "${artifact_root}"
  ""
  ""
)

# Finally compile a logical C provider independently and bind its exact COFF
# object through Draft's public provider mapping. This keeps Windows support for
# downloaded-and-included native code in the same qualification gate.
set(provider_object "${artifact_root}/custom-math.obj")
set(provider_program "${artifact_root}/foreign-provider.exe")
run_checked(
  "Windows foreign provider compile"
  "${CLANG}" -target x86_64-pc-windows-msvc -std=c11
    -Wall -Wextra -Werror -c
    "${SOURCE_ROOT}/tests/foreign_provider_fixture.c"
    -o "${provider_object}"
)
run_checked(
  "Windows foreign provider build"
  "${DRAFTC}" build "${TEST_ROOT}/examples/foreign-provider"
    --target "${target_selector}"
    --provider "custom_math=object:${provider_object}"
    -o "${provider_program}"
)
run_example(
  "Windows foreign provider"
  "${provider_program}"
  "${artifact_root}"
  ""
  ""
)

# The graphical example uses raylib's platform-independent memory backend for
# CI. That nested gate builds the vendored source as a DLL/import-library pair,
# maps the import library through Draft's provider option, copies the runtime
# DLL beside the executable, and renders one complete software frame.
run_checked(
  "Windows raylib Asteroids integration"
  "${CMAKE_COMMAND}"
    "-DDRAFTC=${DRAFTC}"
    "-DSOURCE_ROOT=${SOURCE_ROOT}"
    "-DTEST_ROOT=${TEST_ROOT}/raylib-asteroids"
    "-DTARGET_SELECTOR=${target_selector}"
    -P "${SOURCE_ROOT}/tests/driver_raylib_asteroids_test.cmake"
)

message(STATUS "Windows native Draft smoke qualification passed")
