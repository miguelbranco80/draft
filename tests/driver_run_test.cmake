# End-to-end contract for `draftc run` and `draft.workspace` run defaults.
#
# The fixture is constructed below the CMake binary tree so both native
# artifacts and workspace state remain isolated across worktrees. The first
# launch selects the manifest's default program and configured argument. The
# second names the package directly and proves that bytes after `--` replace
# those defaults, including a spelling which would otherwise be a compiler
# option. Both launches also inherit the manifest environment override and use
# its workspace-relative working directory.

if(NOT DEFINED DRAFTC OR
   NOT DEFINED TEST_ROOT OR
   NOT DEFINED TARGET_SELECTOR)
  message(FATAL_ERROR "DRAFTC, TEST_ROOT, and TARGET_SELECTOR are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/workspace/app")
file(MAKE_DIRECTORY "${TEST_ROOT}/workspace/run-directory")
file(MAKE_DIRECTORY "${TEST_ROOT}/workspace/assets")
file(WRITE "${TEST_ROOT}/workspace/assets/selected.txt" "selected\n")

if(TARGET_SELECTOR STREQUAL "x86_64-windows")
  set(inactive_target "aarch64-macos")
else()
  set(inactive_target "x86_64-windows")
endif()
string(CONCAT workspace_manifest
  "draft-workspace-v1\n"
  "default = contract\n"
  "\n"
  "[build]\n"
  "target = ${inactive_target}\n"
  "runtime-asset[${inactive_target}] = absent:assets/absent.txt\n"
  "runtime-asset[${TARGET_SELECTOR}] = selected:assets/selected.txt\n"
  "\n"
  "[program contract]\n"
  "root = app\n"
  "argument = configured\n"
  "working-directory = run-directory\n"
  "environment = DRAFT_RUN_CONTRACT=yes\n"
)
file(WRITE "${TEST_ROOT}/workspace/draft.workspace" "${workspace_manifest}")

# The checked-in target differs from the actual host. Each launch's explicit
# --target must replace that scalar before selecting qualified native inputs;
# otherwise the absent inactive asset makes native emission fail.

file(WRITE "${TEST_ROOT}/workspace/app/package.draft" [=[
// Native run-contract fixture. It observes only the portable process views;
// launch mechanics and manifest parsing remain driver responsibilities.
package run_contract

import core/os

text_equal :: proc(left: string, right: string) -> bool {
    if len(left) != len(right) {
        return false
    }
    for index: usize = 0; index < len(left); index += 1 {
        if left[index] != right[index] {
            return false
        }
    }
    return true
}

main :: proc() {
    arguments := os.args()
    assert(len(arguments) == 2 || len(arguments) == 3)
    if len(arguments) == 2 {
        assert(text_equal(arguments[1], "configured"))
    } else {
        assert(text_equal(arguments[1], "first"))
        assert(text_equal(arguments[2], "--timings"))
    }

    found_environment := false
    for row in os.environment() {
        if text_equal(row, "DRAFT_RUN_CONTRACT=yes") {
            found_environment = true
        }
    }
    assert(found_environment)
}
]=])

execute_process(
  COMMAND "${DRAFTC}" run "${TEST_ROOT}/workspace"
    --target "${TARGET_SELECTOR}"
    -o "${TEST_ROOT}/configured-program"
  RESULT_VARIABLE configured_status
  OUTPUT_VARIABLE configured_output
  ERROR_VARIABLE configured_error
)
if(NOT configured_status EQUAL 0)
  message(FATAL_ERROR
    "manifest-configured run failed (${configured_status})\n"
    "stdout:\n${configured_output}\nstderr:\n${configured_error}")
endif()

execute_process(
  COMMAND "${DRAFTC}" run "${TEST_ROOT}/workspace/app"
    --target "${TARGET_SELECTOR}"
    -o "${TEST_ROOT}/explicit-program"
    -- first --timings
  RESULT_VARIABLE explicit_status
  OUTPUT_VARIABLE explicit_output
  ERROR_VARIABLE explicit_error
)
if(NOT explicit_status EQUAL 0)
  message(FATAL_ERROR
    "explicit-argument run failed (${explicit_status})\n"
    "stdout:\n${explicit_output}\nstderr:\n${explicit_error}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
