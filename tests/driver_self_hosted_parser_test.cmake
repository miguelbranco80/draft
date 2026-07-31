# Differential process test for the Draft-written concrete parser.
#
# Every source is passed to bootstrap `draftc syntax` and Draft-written
# `draftc-next syntax` under the same working directory. stdout contains the
# deterministic concrete-tree dump; stderr contains lexical/parser diagnostics.
# The test compares both byte streams and the exact status, including malformed
# inputs where a rooted recovery tree is still expected.

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

function(compare_syntax source)
  execute_process(
    COMMAND "${DRAFTC}" syntax "${source}"
    WORKING_DIRECTORY "${SOURCE_ROOT}"
    RESULT_VARIABLE bootstrap_result
    OUTPUT_FILE "${run_root}/bootstrap.stdout"
    ERROR_FILE "${run_root}/bootstrap.stderr"
  )
  execute_process(
    COMMAND "${DRAFTC_NEXT}" syntax "${source}"
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
      "syntax drivers differ for ${source}\n"
      "bootstrap result: ${bootstrap_result}\n"
      "next result: ${next_result}\n"
      "--- bootstrap stdout ---\n${bootstrap_stdout}"
      "--- next stdout ---\n${next_stdout}"
      "--- bootstrap stderr ---\n${bootstrap_stderr}"
      "--- next stderr ---\n${next_stderr}"
    )
  endif()
  if(ARGC GREATER 1 AND "${ARGV1}" STREQUAL "success" AND
     NOT bootstrap_result EQUAL 0)
    file(READ "${run_root}/bootstrap.stderr" bootstrap_stderr)
    message(FATAL_ERROR
      "expected valid syntax fixture to succeed: ${source}\n"
      "${bootstrap_stderr}")
  endif()
endfunction()

# Real packages are the primary compatibility corpus. Sorted enumeration keeps
# failures deterministic across filesystems and includes the parser's own Draft
# source in the next self-hosting generation.
file(GLOB_RECURSE source_files LIST_DIRECTORIES false
  "${SOURCE_ROOT}/core/*.draft"
  "${SOURCE_ROOT}/compiler/*.draft"
  "${SOURCE_ROOT}/examples/*.draft"
  "${SOURCE_ROOT}/lib/*.draft"
  "${SOURCE_ROOT}/tools/*.draft"
)
list(SORT source_files)
foreach(source IN LISTS source_files)
  compare_syntax("${source}")
endforeach()

# This compact valid fixture deliberately combines declaration/member,
# expression/statement, synthesis, denial, and assembly grammar categories.
set(matrix "${run_root}/matrix.draft")
file(WRITE "${matrix}" [=[package matrix
docs "package intent" file "DESIGN.md"
import core/memory as memory
Box[T: type, N: usize] :: c align(16) struct {
    packed value: u32,
    bits(3) mode: u8,
    when true { selected: [N]T, } else { fallback: []T, }
    deny asm {
        ... "member"
    }
}
foreign libc { printf :: c proc(format: cstring, ..) -> int; }
run :: proc(values: [2]u32) -> u32 {
    result := 0
    for value, index in values {
        result += value + cast[u32](index)
    }
    switch .some(result) {
    case .some(value):
        result = value
    case:
        result = 0
    }
    guarded := deny memory, asm { result if result > 0 else 1 }
    asm aarch64 {
        in x0 = guarded
        out x0
        add x0, x0, #1
    }
    return ... "result"
}
]=])
compare_syntax("${matrix}" success)

# Independent malformed regions exercise local recovery boundaries and exact
# parser diagnostic source ranges without one early error hiding every later
# grammar family.
set(bad_declarations "${run_root}/bad-declarations.draft")
file(WRITE "${bad_declarations}" [=[package
import core/
docs file README
Record :: align() struct { packed field u32 other: [] }
Value :: union { packed word: u32, bits(3) other: u32, }
Callback :: c proc(value u32, .., suffix: u32)
]=])
compare_syntax("${bad_declarations}")

set(bad_statements "${run_root}/bad-statements.draft")
file(WRITE "${bad_statements}" [=[package bad_statements
run :: proc(value: u32) {
    if true return
    for i := 0; i < 4 {}
    switch value { value: return }
    asm aarch64 -> u64 { in x0 value
}
]=])
compare_syntax("${bad_statements}")

# Lexical failures flow through the same parse command and must preserve both
# the partial token stream and subsequent parser recovery diagnostics.
set(bad_lexical "${run_root}/bad-lexical.draft")
file(WRITE "${bad_lexical}"
  "package bad_lexical\nvalue := 0b102\nbroken := \"bad\\q\"\n@\n")
compare_syntax("${bad_lexical}")

# The exact numeric limit is intentionally exercised here because both parser
# implementations currently publish it in the diagnostic spelling.
string(REPEAT "+" 600 unary_prefix)
set(deep "${run_root}/deep.draft")
file(WRITE "${deep}" "package deep\nvalue := ${unary_prefix}1\n")
compare_syntax("${deep}")

# Pre-source I/O remains part of the command contract.
compare_syntax("${run_root}/missing.draft")

list(LENGTH source_files source_count)
math(EXPR comparison_count "${source_count} + 6")
message(STATUS
  "draftc-next syntax matched bootstrap for ${comparison_count} inputs")
file(REMOVE_RECURSE "${run_root}")
