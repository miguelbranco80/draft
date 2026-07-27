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

run_checked("draftide --version" "${draftide}" --version)
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
run_checked("DraftIDE smoke" "${draftide}" "${example}" --smoke)
