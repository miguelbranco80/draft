# Exercise an already installed or extracted Draft distribution.
#
# The caller provides a distribution root, a disposable directory, the native
# target, and the expected public version. This script deliberately invokes
# only files inside the distribution. Passing proves that the compiler finds
# its bundled LLVM tools and embedded core after relocation, that DraftIDE finds
# its sibling service library, and that a real Draft program builds and runs.

foreach(required IN ITEMS DRAFT_ROOT TEST_ROOT DRAFT_TARGET DRAFT_RELEASE_VERSION)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

# A Windows build job adds its development LLVM bin directory to PATH before
# configuration. The installed tools are intentionally self-contained, so an
# archive smoke must not let that ambient entry satisfy a missing public DLL or
# private tool. Remove only the exact caller-supplied directory and retain the
# Visual Studio, Windows SDK, and system entries established by VsDevCmd.
if(WIN32 AND DEFINED DRAFT_AMBIENT_LLVM_BIN
    AND NOT DRAFT_AMBIENT_LLVM_BIN STREQUAL "")
  cmake_path(
    CONVERT "$ENV{PATH}" TO_CMAKE_PATH_LIST path_entries NORMALIZE
  )
  set(ambient_llvm_bin "${DRAFT_AMBIENT_LLVM_BIN}")
  cmake_path(NORMAL_PATH ambient_llvm_bin)
  string(TOLOWER "${ambient_llvm_bin}" ambient_llvm_bin_key)
  set(isolated_path_entries)
  foreach(path_entry IN LISTS path_entries)
    set(normalized_path_entry "${path_entry}")
    cmake_path(NORMAL_PATH normalized_path_entry)
    string(TOLOWER "${normalized_path_entry}" path_entry_key)
    if(NOT path_entry_key STREQUAL ambient_llvm_bin_key)
      list(APPEND isolated_path_entries "${path_entry}")
    endif()
  endforeach()
  cmake_path(
    CONVERT "${isolated_path_entries}" TO_NATIVE_PATH_LIST isolated_path
    NORMALIZE
  )
  set(ENV{PATH} "${isolated_path}")
endif()

if(WIN32)
  set(executable_suffix ".exe")
else()
  set(executable_suffix "")
endif()
set(draftc "${DRAFT_ROOT}/bin/draftc${executable_suffix}")
set(draftide "${DRAFT_ROOT}/bin/draftide${executable_suffix}")
set(example_source "${DRAFT_ROOT}/share/draft/examples/hello")
set(draft_license "${DRAFT_ROOT}/LICENSE")
set(third_party_notices "${DRAFT_ROOT}/THIRD_PARTY_NOTICES.md")
set(llvm_license
    "${DRAFT_ROOT}/share/draft/licenses/llvm/LLVM-LICENSE.txt")
set(example "${TEST_ROOT}/hello")
set(program "${TEST_ROOT}/hello-program${executable_suffix}")

# Sanitizer qualification installs an instrumented C++ service beside an
# ordinary DraftIDE executable. On ELF, ASan must be present before that service
# is loaded, so the sanitizer build passes its exact runtime for only the two
# DraftIDE launches below. Release archives leave this unset because their
# service is not instrumented.
set(draftide_command "${draftide}")
if(DEFINED DRAFT_ASAN_PRELOAD AND NOT DRAFT_ASAN_PRELOAD STREQUAL "")
  set(
    draftide_command
    "${CMAKE_COMMAND}" -E env "LD_PRELOAD=${DRAFT_ASAN_PRELOAD}" "${draftide}"
  )
endif()

foreach(path IN ITEMS
    "${draftc}"
    "${draftide}"
    "${example_source}"
    "${draft_license}"
    "${third_party_notices}"
    "${llvm_license}"
)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "Distribution is missing ${path}")
  endif()
endforeach()

# A relocatable distribution cannot contain a link that reaches back into the
# build host. This is checked before executing draftc because the ELF loader may
# otherwise ignore a broken private link and satisfy the same SONAME from an
# installed system LLVM package, giving the smoke test a false success.
file(
  GLOB_RECURSE distribution_entries
  LIST_DIRECTORIES TRUE
  "${DRAFT_ROOT}/*"
)
foreach(distribution_entry IN LISTS distribution_entries)
  if(IS_SYMLINK "${distribution_entry}" AND NOT EXISTS "${distribution_entry}")
    file(READ_SYMLINK "${distribution_entry}" link_target)
    message(FATAL_ERROR
      "Distribution contains broken symlink ${distribution_entry} -> ${link_target}"
    )
  endif()
endforeach()

# LLVM 22.1's Linux SONAMEs are direct runtime dependencies of draftc and its
# private Clang driver. Requiring their resolved paths makes the intended
# runtime closure explicit even if a future install layout stops using links.
if(DRAFT_TARGET MATCHES "-linux$")
  foreach(runtime_name IN ITEMS libLLVM.so.22.1 libclang-cpp.so.22.1)
    set(runtime_path "${DRAFT_ROOT}/libexec/draft/lib/${runtime_name}")
    if(NOT EXISTS "${runtime_path}")
      message(FATAL_ERROR
        "Linux distribution is missing resolved runtime ${runtime_path}"
      )
    endif()
  endforeach()
endif()

if(EXISTS "${example_source}/.draft")
  message(FATAL_ERROR
    "Distribution contains derived example state at ${example_source}/.draft"
  )
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(COPY "${example_source}" DESTINATION "${TEST_ROOT}")

function(run_checked label)
  execute_process(
    COMMAND ${ARGN}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    WORKING_DIRECTORY "${TEST_ROOT}"
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR
      "${label} failed (${result})\nstdout:\n${output}\nstderr:\n${error}"
    )
  endif()
  set(DRAFT_LAST_OUTPUT "${output}${error}" PARENT_SCOPE)
endfunction()

run_checked("draftc --version" "${draftc}" --version)
if(NOT DRAFT_LAST_OUTPUT MATCHES "draftc ${DRAFT_RELEASE_VERSION}")
  message(FATAL_ERROR "draftc reported the wrong version:\n${DRAFT_LAST_OUTPUT}")
endif()
if(NOT DRAFT_LAST_OUTPUT MATCHES "toolchain: bundled")
  message(FATAL_ERROR "draftc did not select bundled tools:\n${DRAFT_LAST_OUTPUT}")
endif()

run_checked("draftide --version" ${draftide_command} --version)
if(NOT DRAFT_LAST_OUTPUT MATCHES "draftide ${DRAFT_RELEASE_VERSION}")
  message(FATAL_ERROR "draftide reported the wrong version:\n${DRAFT_LAST_OUTPUT}")
endif()
if(NOT DRAFT_LAST_OUTPUT MATCHES "toolchain: bundled")
  message(FATAL_ERROR "draftide did not select bundled tools:\n${DRAFT_LAST_OUTPUT}")
endif()

run_checked("Draft check" "${draftc}" check "${example}" --target "${DRAFT_TARGET}")
run_checked(
  "Draft build"
  "${draftc}" build "${example}" --target "${DRAFT_TARGET}" -o "${program}"
)
run_checked("built Draft program" "${program}")
run_checked("DraftIDE smoke" ${draftide_command} "${example}" --smoke)
