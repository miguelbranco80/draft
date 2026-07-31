# Differential process test for the Draft-written lexer driver.
#
# Each invocation receives the same pathname and working directory. The test
# compares stdout and stderr as files so CMake string semantics cannot normalize
# or truncate bytes, then compares the exact process result. Scratch filenames
# live below the current binary directory and include a random component, so
# concurrent worktrees and test processes never share derived state.

foreach(required DRAFTC DRAFTC_NEXT SOURCE_ROOT TEST_ROOT)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "missing required -D${required}=...")
  endif()
endforeach()
if(NOT EXISTS "${DRAFTC}")
  message(FATAL_ERROR "bootstrap draftc does not exist: ${DRAFTC}")
endif()
if(NOT EXISTS "${DRAFTC_NEXT}")
  message(FATAL_ERROR "Draft-written draftc-next does not exist: ${DRAFTC_NEXT}")
endif()

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef run_identity)
set(run_root "${TEST_ROOT}/${run_identity}")
file(MAKE_DIRECTORY "${run_root}")

# compare_lex executes both drivers even when the source is intentionally bad.
# A lexical error is an expected nonzero result provided both drivers agree on
# the result and every output byte.
function(compare_lex source)
  execute_process(
    COMMAND "${DRAFTC}" lex "${source}"
    WORKING_DIRECTORY "${SOURCE_ROOT}"
    RESULT_VARIABLE bootstrap_result
    OUTPUT_FILE "${run_root}/bootstrap.stdout"
    ERROR_FILE "${run_root}/bootstrap.stderr"
  )
  execute_process(
    COMMAND "${DRAFTC_NEXT}" lex "${source}"
    WORKING_DIRECTORY "${SOURCE_ROOT}"
    RESULT_VARIABLE next_result
    OUTPUT_FILE "${run_root}/next.stdout"
    ERROR_FILE "${run_root}/next.stderr"
  )
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
      "${run_root}/bootstrap.stdout" "${run_root}/next.stdout"
    RESULT_VARIABLE stdout_result
  )
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
      "${run_root}/bootstrap.stderr" "${run_root}/next.stderr"
    RESULT_VARIABLE stderr_result
  )
  if(NOT "${bootstrap_result}" STREQUAL "${next_result}" OR
     NOT stdout_result EQUAL 0 OR NOT stderr_result EQUAL 0)
    file(READ "${run_root}/bootstrap.stdout" bootstrap_stdout)
    file(READ "${run_root}/next.stdout" next_stdout)
    file(READ "${run_root}/bootstrap.stderr" bootstrap_stderr)
    file(READ "${run_root}/next.stderr" next_stderr)
    message(FATAL_ERROR
      "lexer drivers differ for ${source}\n"
      "bootstrap result: ${bootstrap_result}\n"
      "next result: ${next_result}\n"
      "--- bootstrap stdout ---\n${bootstrap_stdout}"
      "--- next stdout ---\n${next_stdout}"
      "--- bootstrap stderr ---\n${bootstrap_stderr}"
      "--- next stderr ---\n${next_stderr}"
    )
  endif()
endfunction()

# This inventory is deterministic and deliberately broader than compiler-only
# sources: examples and tools supply realistic comments, raw strings, agent
# attachments, target-qualified files, and punctuation mixtures.
file(GLOB_RECURSE source_files LIST_DIRECTORIES false
  "${SOURCE_ROOT}/core/*.draft"
  "${SOURCE_ROOT}/compiler/*.draft"
  "${SOURCE_ROOT}/examples/*.draft"
  "${SOURCE_ROOT}/lib/*.draft"
  "${SOURCE_ROOT}/tools/*.draft"
)
list(SORT source_files)
foreach(source IN LISTS source_files)
  compare_lex("${source}")
endforeach()

# A dense valid fixture covers longest-match punctuation, each literal family,
# CRLF normalization, contextual keyword alternatives, and attachment newline
# suppression independently of what happens to appear in current packages.
set(matrix "${run_root}/matrix.draft")
file(WRITE "${matrix}"
  "package matrix\r\n"
  "variant union simd packed bits raw\r\n"
  "0 0b1010 0o755 0xCA_FE 1_000 1.25 2e10 3.5e-2\r\n"
  "\"text\\n\\x00\\u{1f642}\" 'é' '\\n' `raw\ntext`\r\n"
  ". .. ... :: := -> --- <<= >>= += -= *= /= %= &= |= ~= == != <= >= << >> && ||\r\n"
  "first := .struct\r\n"
  "docs \"intent\"\r\n    file \"DESIGN.md\"\r\n")
compare_lex("${matrix}")

# Invalid spellings exercise diagnostic rendering and the nonzero status.
set(malformed "${run_root}/malformed.draft")
file(WRITE "${malformed}"
  "package malformed\n"
  "binary := 0b102\nunderscores := 1__2\nfraction := 1.\n"
  "surrogate := '\\u{d800}'\nrunes := 'ab'\n"
  "broken := \"bad\\q\"\n@\n")
compare_lex("${malformed}")

# A valid non-ASCII scalar is still illegal in an identifier, while an
# unfinished block comment proves an EOF-spanning diagnostic range.
set(unicode_identifier "${run_root}/unicode-identifier.draft")
file(WRITE "${unicode_identifier}" "café := 1\n")
compare_lex("${unicode_identifier}")
set(unterminated_comment "${run_root}/unterminated-comment.draft")
file(WRITE "${unterminated_comment}" "value := 1\n/* unfinished")
compare_lex("${unterminated_comment}")

# A missing path separately fixes the pre-source I/O error contract.
compare_lex("${run_root}/missing.draft")

list(LENGTH source_files source_count)
math(EXPR comparison_count "${source_count} + 5")
message(STATUS
  "draftc-next lex matched bootstrap for ${comparison_count} inputs")
file(REMOVE_RECURSE "${run_root}")
