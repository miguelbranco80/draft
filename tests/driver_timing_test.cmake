# Process-level contract test for Draft compiler timing reports.
#
# The base recorder has a fake-clock unit test. This script instead runs the
# real driver against one stable package and verifies only structural facts:
# timing is written to stderr, summary mode exposes real compiler passes but
# hides package/self detail, all mode exposes both, and duplicate selection is
# rejected as a usage error. It does not compare durations because host load is
# intentionally outside the deterministic compiler contract.

if(NOT DEFINED DRAFTC OR NOT DEFINED SOURCE_PACKAGE)
  message(FATAL_ERROR "DRAFTC and SOURCE_PACKAGE are required")
endif()

execute_process(
  COMMAND "${DRAFTC}" check "${SOURCE_PACKAGE}" --timings
  RESULT_VARIABLE summary_result
  OUTPUT_VARIABLE summary_stdout
  ERROR_VARIABLE summary_stderr
)
if(NOT summary_result EQUAL 0)
  message(FATAL_ERROR
    "summary timing command failed (${summary_result})\n${summary_stderr}")
endif()
if(NOT summary_stdout MATCHES "checked package graph rooted at hello")
  message(FATAL_ERROR "summary command lost ordinary stdout")
endif()
if(NOT summary_stderr MATCHES "timings \\(wall clock\\):" OR
   NOT summary_stderr MATCHES "interface discovery round 1" OR
   NOT summary_stderr MATCHES "compiler passes: 2")
  message(FATAL_ERROR "summary report lacks required phase/counter rows\n${summary_stderr}")
endif()
if(summary_stderr MATCHES "package declarations:" OR
   summary_stderr MATCHES "\\(self ")
  message(FATAL_ERROR "summary report leaked all-mode detail\n${summary_stderr}")
endif()

execute_process(
  COMMAND "${DRAFTC}" check "${SOURCE_PACKAGE}" --timings=all
  RESULT_VARIABLE all_result
  OUTPUT_VARIABLE all_stdout
  ERROR_VARIABLE all_stderr
)
if(NOT all_result EQUAL 0)
  message(FATAL_ERROR "all timing command failed (${all_result})\n${all_stderr}")
endif()
if(NOT all_stderr MATCHES "source file I/O: package.draft" OR
   NOT all_stderr MATCHES "lex and parse: package.draft" OR
   NOT all_stderr MATCHES "import graph resolution: workspace:hello" OR
   NOT all_stderr MATCHES "package declarations: workspace:hello" OR
   NOT all_stderr MATCHES "\\(self ")
  message(FATAL_ERROR
    "all report lacks source/import/package/exclusive detail\n${all_stderr}")
endif()

execute_process(
  COMMAND "${DRAFTC}" check "${SOURCE_PACKAGE}" --timings --timings=all
  RESULT_VARIABLE duplicate_result
  OUTPUT_VARIABLE duplicate_stdout
  ERROR_VARIABLE duplicate_stderr
)
if(NOT duplicate_result EQUAL 2 OR
   NOT duplicate_stderr MATCHES "--timings may be specified only once")
  message(FATAL_ERROR
    "duplicate timing option was not rejected as usage error\n${duplicate_stderr}")
endif()
