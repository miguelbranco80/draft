// Deterministic execution of command-local dependency work.
//
// This base module owns no compiler, package, target, or LLVM concepts. Its
// input is a closed graph whose stable integer task IDs index dependency rows;
// its output is one terminal result in that same ID domain. A caller owns all
// task-specific inputs and output slots for the complete synchronous run. The
// scheduler may change when ready tasks start, but it never changes which slot
// receives a result, which failed dependency blocks a consumer, or the order in
// which a caller later publishes results.
//
// Dependencies are immutable during a run. Each operation may mutate only the
// caller-owned state assigned to its task ID, and independent operations may be
// called concurrently. The module depends only on the C++ runtime and base-level
// data; higher compiler phases may use it, but it cannot call into them.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace draft {

using WorkTaskId = std::uint32_t;

// One row describes the tasks that must succeed before this task may run.
// Task IDs are zero-based indices into WorkGraph::tasks. Dependencies must be
// strictly increasing, which makes graph validation and reverse-edge creation
// independent of insertion or hash-table order.
struct WorkTask {
  std::vector<WorkTaskId> dependencies;
};

// A WorkGraph is a command-local scheduling value, not semantic compiler state
// and not a serializable cache. Its vector index is the stable task identity for
// the run. The graph must be acyclic and every dependency must name a row in the
// same vector.
struct WorkGraph {
  std::vector<WorkTask> tasks;
};

enum class WorkTaskState {
  Pending,
  Running,
  Succeeded,
  Failed,
  SkippedDependency,
};

// A terminal result is stored at the same index as its task. failure is empty
// for success and contains either the operation's direct reason or the stable
// lowest failed dependency ID that prevented this task from running. Pending
// and Running exist for internal state transitions and are never returned by a
// successful scheduler invocation.
struct WorkTaskResult {
  WorkTaskState state = WorkTaskState::Pending;
  std::string failure;
};

// worker_count == 0 selects the host's reported hardware concurrency, with one
// worker as the fallback. The scheduler always caps the selected count to the
// number of tasks. Tests and embedding callers may request one worker to retain
// the exact sequential oracle without changing the graph or operation.
struct WorkGraphRunOptions {
  std::size_t worker_count = 0;
};

// The operation receives one stable ID and a context pointer whose concrete
// type is owned by the caller. It returns true on success. On false it writes a
// direct failure reason; an empty reason is replaced with a deterministic
// scheduler diagnostic. The operation must not throw, retain the context after
// returning, or read/write another task's mutable output slot.
using WorkTaskOperation = bool (*)(
    void *context,
    WorkTaskId task,
    std::string &failure);

// WorkGraphRunResult contains only scheduling outcomes. Task-specific products
// remain in caller-owned slots so this base module does not need callbacks,
// variants, or type erasure for compiler data. workers_used is the deterministic
// selected pool size and is useful for timing evidence and focused tests.
struct WorkGraphRunResult {
  bool ok = false;
  std::size_t workers_used = 0;
  std::vector<WorkTaskResult> tasks;
};

// Validates ID domains, canonical dependency rows, and acyclicity without
// executing work. The first error is selected by ascending task/dependency ID,
// so malformed graphs produce the same reason on every host.
[[nodiscard]] bool validate_work_graph(
    const WorkGraph &graph,
    std::string &reason);

// Executes the validated ready graph synchronously. Failed tasks are retained
// in their own result slots. A consumer whose dependency failed is marked
// SkippedDependency without invoking the operation, and that state propagates
// transitively. Independent ready work still completes, allowing the caller to
// report the stable lowest-ID failure after joining every started operation.
[[nodiscard]] WorkGraphRunResult run_work_graph(
    const WorkGraph &graph,
    WorkGraphRunOptions options,
    WorkTaskOperation operation,
    void *context);

} // namespace draft
