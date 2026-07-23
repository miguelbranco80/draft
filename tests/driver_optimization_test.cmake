# Process-level contract test for Draft native optimization and debug selection.
#
# The LLVM adapter unit test proves the two pipelines differ internally. This
# script proves the public `build`, `resolve --build`, `test`, and `bench`
# spellings reach native emission, that omitted optimization is exactly O0, and
# that invalid or duplicate choices fail before compilation. Source workspaces
# are copied below the CMake binary tree because native commands own build and
# evidence state; tests must never write derived state into the checkout.

if(NOT DEFINED DRAFTC OR
   NOT DEFINED SOURCE_WORKSPACE OR
   NOT DEFINED SOURCE_VALIDATION OR
   NOT DEFINED TEST_ROOT)
  message(FATAL_ERROR
    "DRAFTC, SOURCE_WORKSPACE, SOURCE_VALIDATION, and TEST_ROOT are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(COPY "${SOURCE_WORKSPACE}/" DESTINATION "${TEST_ROOT}/workspace")
file(COPY "${SOURCE_VALIDATION}/" DESTINATION "${TEST_ROOT}/validation")
set(workspace "${TEST_ROOT}/workspace")
set(validation "${TEST_ROOT}/validation")

execute_process(
  COMMAND "${DRAFTC}"
  RESULT_VARIABLE usage_result
  OUTPUT_VARIABLE usage_stdout
  ERROR_VARIABLE usage_stderr
)
string(REGEX MATCHALL "\\[-O0\\|-O2\\]"
  optimization_usage_options "${usage_stderr}")
list(LENGTH optimization_usage_options optimization_usage_count)
if(NOT usage_result EQUAL 2 OR NOT optimization_usage_count EQUAL 4)
  message(FATAL_ERROR
    "usage does not expose O0/O2 on all native-producing commands\n${usage_stderr}")
endif()
string(REGEX MATCHALL "\\[--debug-symbols\\]"
  debug_usage_options "${usage_stderr}")
list(LENGTH debug_usage_options debug_usage_count)
if(NOT debug_usage_count EQUAL 2)
  message(FATAL_ERROR
    "usage does not expose opt-in debug symbols on build commands\n${usage_stderr}")
endif()

execute_process(
  COMMAND "${DRAFTC}" build "${workspace}" --root app -O1
  RESULT_VARIABLE unsupported_result
  OUTPUT_VARIABLE unsupported_stdout
  ERROR_VARIABLE unsupported_stderr
)
if(NOT unsupported_result EQUAL 2 OR
   NOT unsupported_stderr MATCHES
     "unsupported optimization level '-O1'; expected -O0 or -O2")
  message(FATAL_ERROR
    "unsupported optimization was not rejected precisely\n${unsupported_stderr}")
endif()

execute_process(
  COMMAND "${DRAFTC}" build "${workspace}" --root app -O0 -O2
  RESULT_VARIABLE duplicate_result
  OUTPUT_VARIABLE duplicate_stdout
  ERROR_VARIABLE duplicate_stderr
)
if(NOT duplicate_result EQUAL 2 OR
   NOT duplicate_stderr MATCHES
     "optimization level may be specified only once")
  message(FATAL_ERROR
    "duplicate optimization was not rejected precisely\n${duplicate_stderr}")
endif()

execute_process(
  COMMAND "${DRAFTC}" resolve "${workspace}" --root app -O2
  RESULT_VARIABLE resolve_without_build_result
  OUTPUT_VARIABLE resolve_without_build_stdout
  ERROR_VARIABLE resolve_without_build_stderr
)
if(NOT resolve_without_build_result EQUAL 2 OR
   NOT resolve_without_build_stderr MATCHES
     "-O0, -O2, and --debug-symbols require resolve --build")
  message(FATAL_ERROR
    "resolve accepted optimization without --build\n${resolve_without_build_stderr}")
endif()

execute_process(
  COMMAND "${DRAFTC}" resolve "${workspace}" --root app --debug-symbols
  RESULT_VARIABLE debug_without_build_result
  OUTPUT_VARIABLE debug_without_build_stdout
  ERROR_VARIABLE debug_without_build_stderr
)
if(NOT debug_without_build_result EQUAL 2 OR
   NOT debug_without_build_stderr MATCHES
     "-O0, -O2, and --debug-symbols require resolve --build")
  message(FATAL_ERROR
    "resolve accepted debug symbols without --build\n${debug_without_build_stderr}")
endif()

execute_process(
  COMMAND "${DRAFTC}" emit-llvm "${workspace}" --root app
  RESULT_VARIABLE fast_llvm_result
  OUTPUT_VARIABLE fast_llvm_stdout
  ERROR_VARIABLE fast_llvm_stderr
)
if(NOT fast_llvm_result EQUAL 0 OR
   fast_llvm_stdout MATCHES "!DICompileUnit|!DILocation|!dbg !")
  message(FATAL_ERROR
    "ordinary LLVM emission retained debug metadata\n${fast_llvm_stderr}")
endif()

execute_process(
  COMMAND "${DRAFTC}" emit-llvm "${workspace}" --root app -O2
  RESULT_VARIABLE emit_llvm_result
  OUTPUT_VARIABLE emit_llvm_stdout
  ERROR_VARIABLE emit_llvm_stderr
)
if(NOT emit_llvm_result EQUAL 2)
  message(FATAL_ERROR
    "emit-llvm accepted a native optimization option\n${emit_llvm_stderr}")
endif()

set(default_output "${TEST_ROOT}/default-assembly")
set(o0_output "${TEST_ROOT}/o0-assembly")
set(o2_output "${TEST_ROOT}/o2-assembly")
set(resolve_o2_output "${TEST_ROOT}/resolve-o2-assembly")

foreach(level IN ITEMS default o0 o2)
  if(level STREQUAL "default")
    set(level_argument)
    set(level_output "${default_output}")
  elseif(level STREQUAL "o0")
    set(level_argument -O0)
    set(level_output "${o0_output}")
  else()
    set(level_argument -O2)
    set(level_output "${o2_output}")
  endif()
  execute_process(
    COMMAND "${DRAFTC}" build "${workspace}" --root app
      --kind assembly ${level_argument} -o "${level_output}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr
  )
  if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
      "${level} assembly build failed (${build_result})\n${build_stderr}")
  endif()
endforeach()

execute_process(
  COMMAND "${DRAFTC}" resolve "${workspace}" --root app --build
    --kind assembly -O2 -o "${resolve_o2_output}"
  RESULT_VARIABLE resolve_o2_result
  OUTPUT_VARIABLE resolve_o2_stdout
  ERROR_VARIABLE resolve_o2_stderr
)
if(NOT resolve_o2_result EQUAL 0)
  message(FATAL_ERROR
    "resolve --build -O2 failed (${resolve_o2_result})\n${resolve_o2_stderr}")
endif()

file(GLOB default_files RELATIVE "${default_output}" "${default_output}/*.s")
file(GLOB o0_files RELATIVE "${o0_output}" "${o0_output}/*.s")
file(GLOB o2_files RELATIVE "${o2_output}" "${o2_output}/*.s")
file(GLOB resolve_o2_files
  RELATIVE "${resolve_o2_output}" "${resolve_o2_output}/*.s")
list(SORT default_files)
list(SORT o0_files)
list(SORT o2_files)
list(SORT resolve_o2_files)
if(NOT "${default_files}" STREQUAL "${o0_files}" OR
   NOT "${o0_files}" STREQUAL "${o2_files}" OR
   NOT "${o2_files}" STREQUAL "${resolve_o2_files}" OR
   NOT o0_files)
  message(FATAL_ERROR
    "optimization builds published different assembly file sets")
endif()

set(saw_optimized_difference FALSE)
foreach(relative_file IN LISTS o0_files)
  file(READ "${default_output}/${relative_file}" default_bytes)
  file(READ "${o0_output}/${relative_file}" o0_bytes)
  file(READ "${o2_output}/${relative_file}" o2_bytes)
  file(READ "${resolve_o2_output}/${relative_file}" resolve_o2_bytes)
  if(NOT "${default_bytes}" STREQUAL "${o0_bytes}")
    message(FATAL_ERROR
      "omitted optimization does not match -O0 for ${relative_file}")
  endif()
  if(NOT "${o2_bytes}" STREQUAL "${resolve_o2_bytes}")
    message(FATAL_ERROR
      "build and resolve --build disagree at -O2 for ${relative_file}")
  endif()
  if(NOT "${o0_bytes}" STREQUAL "${o2_bytes}")
    set(saw_optimized_difference TRUE)
  endif()
endforeach()
if(NOT saw_optimized_difference)
  message(FATAL_ERROR "-O2 did not change any emitted package assembly")
endif()

# Validation owns a native harness too. Exercise both command paths rather than
# merely accepting the spelling: benchmarks at fixed O0 would make their timing
# policy actively misleading. The committed evidence policy must distinguish
# O0 from O2 even though both runs consume the same resolved Draft program.
execute_process(
  COMMAND "${DRAFTC}" test "${validation}" -O0
  RESULT_VARIABLE test_o0_result
  OUTPUT_VARIABLE test_o0_stdout
  ERROR_VARIABLE test_o0_stderr
)
if(NOT test_o0_result EQUAL 0 OR
   NOT test_o0_stdout MATCHES "test passed")
  message(FATAL_ERROR
    "test -O0 failed (${test_o0_result})\n${test_o0_stderr}")
endif()

execute_process(
  COMMAND "${DRAFTC}" test "${validation}" -O2
  RESULT_VARIABLE test_o2_result
  OUTPUT_VARIABLE test_o2_stdout
  ERROR_VARIABLE test_o2_stderr
)
if(NOT test_o2_result EQUAL 0 OR
   NOT test_o2_stdout MATCHES "test passed")
  message(FATAL_ERROR
    "test -O2 failed (${test_o2_result})\n${test_o2_stderr}")
endif()

execute_process(
  COMMAND "${DRAFTC}" bench "${validation}" --verify -O2
  RESULT_VARIABLE bench_o2_result
  OUTPUT_VARIABLE bench_o2_stdout
  ERROR_VARIABLE bench_o2_stderr
)
if(NOT bench_o2_result EQUAL 0 OR
   NOT bench_o2_stdout MATCHES "benchmark passed")
  message(FATAL_ERROR
    "bench -O2 failed (${bench_o2_result})\n${bench_o2_stderr}")
endif()

file(GLOB evidence_files "${validation}/.draft/evidence/*.json")
set(saw_o0_policy FALSE)
set(saw_o2_policy FALSE)
set(test_o0_program)
set(test_o2_program)
foreach(evidence_file IN LISTS evidence_files)
  file(READ "${evidence_file}" evidence)
  if(evidence MATCHES "native-optimization=O0")
    set(saw_o0_policy TRUE)
    if(evidence MATCHES "\"kind\": \"test\"")
      string(REGEX MATCH
        "\"resolved_program\": \"([0-9a-f]+)\""
        resolved_program_field "${evidence}")
      set(test_o0_program "${CMAKE_MATCH_1}")
    endif()
  endif()
  if(evidence MATCHES "native-optimization=O2")
    set(saw_o2_policy TRUE)
    if(evidence MATCHES "\"kind\": \"test\"")
      string(REGEX MATCH
        "\"resolved_program\": \"([0-9a-f]+)\""
        resolved_program_field "${evidence}")
      set(test_o2_program "${CMAKE_MATCH_1}")
    endif()
  endif()
endforeach()
if(NOT saw_o0_policy OR NOT saw_o2_policy)
  message(FATAL_ERROR
    "validation evidence does not distinguish O0 and O2 policies")
endif()
if(test_o0_program STREQUAL "" OR
   test_o2_program STREQUAL "" OR
   NOT test_o0_program STREQUAL test_o2_program)
  message(FATAL_ERROR
    "O0 and O2 test evidence does not retain one resolved-program identity")
endif()
