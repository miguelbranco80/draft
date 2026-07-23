// Deterministic execution of command-local dependency work.
//
// This base module owns no compiler, package, target, or LLVM concepts. Its
// input is a closed graph whose stable integer task IDs index dependency rows;
// its output is one terminal result in that same ID domain. WorkExecutor owns a
// fixed command-lifetime worker pool and may execute many synchronous graphs in
// sequence. A caller owns all task-specific inputs and output slots for each
// run. The scheduler may change when ready tasks start, but it never changes
// which slot receives a result, which failed dependency blocks a consumer, or
// the order in which a caller later publishes results.
//
// Dependencies are immutable during a run. Each operation may mutate only the
// caller-owned state assigned to its task ID, and independent operations may be
// called concurrently. The module depends only on the C++ runtime and
// base-level data; higher compiler phases may use it, but it cannot call into
// them.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace draft {

// WorkTaskId is a stable zero-based index for one WorkGraph invocation. It may
// be copied into phase-owned side tables but is never serialized or compared
// across commands.
using WorkTaskId = std::uint32_t;

// One row describes the tasks that must succeed before this task may run.
// Task IDs are zero-based indices into WorkGraph::tasks. Dependencies must be
// strictly increasing, which makes graph validation and reverse-edge creation
// independent of insertion or hash-table order.
struct WorkTask {
  std::vector<WorkTaskId> dependencies;
};

// A WorkGraph is a command-local scheduling value, not semantic compiler state
// and not a serializable cache. Its vector index is the stable task identity
// for the run. The graph must be acyclic and every dependency must name a row
// in the same vector.
struct WorkGraph {
  std::vector<WorkTask> tasks;
};

// WorkTaskState records the visible lifecycle of one slot. Pending and Running
// are internal transient states; every returned task is Succeeded, Failed, or
// SkippedDependency. Enum order has no semantic or scheduling meaning.
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
using WorkTaskOperation = bool (*)(void *context, WorkTaskId task,
                                   std::string &failure);

// WorkGraphRunResult contains only scheduling outcomes. Task-specific products
// remain in caller-owned slots so this base module does not need callbacks,
// variants, or type erasure for compiler data. workers_used is the
// deterministic selected pool size and is useful for timing evidence and
// focused tests.
struct WorkGraphRunResult {
  bool ok = false;
  std::size_t workers_used = 0;
  std::vector<WorkTaskResult> tasks;
};

// WorkExecutor owns the one bounded worker pool used by a compiler command.
// Constructing it records the host concurrency bound; workers are started on
// the first parallel run and then sleep between graphs. Reusing those workers
// matters because semantic compilation intentionally has many small dependency-
// ready waves, for which creating and joining a fresh operating-system thread
// set would cost more than the semantic work itself.
//
// A run is synchronous and the executor is deliberately non-reentrant. The
// graph, operation, context, and task-owned result storage may therefore remain
// ordinary stack values at the call site. worker_count == 0 uses the executor's
// host bound; a smaller explicit count is useful for deterministic sequential
// qualification. A one-worker run stays on the calling thread and does not wake
// the pool. The executor contains no compiler products and retains no result
// between runs, so sharing one across phases is execution reuse rather than a
// compiler cache.
class WorkExecutor {
public:
  explicit WorkExecutor(std::size_t maximum_workers = 0);
  ~WorkExecutor();

  WorkExecutor(const WorkExecutor &) = delete;
  WorkExecutor &operator=(const WorkExecutor &) = delete;
  WorkExecutor(WorkExecutor &&) = delete;
  WorkExecutor &operator=(WorkExecutor &&) = delete;

  [[nodiscard]] std::size_t maximum_workers() const;

  // Executes one validated graph and returns only after every started worker
  // has stopped borrowing the call-owned graph, operation, context, and result.
  // Failed tasks remain in their stable slots; consumers of a failed task are
  // skipped transitively while independent work still reaches a terminal state.
  [[nodiscard]] WorkGraphRunResult run(const WorkGraph &graph,
                                       WorkGraphRunOptions options,
                                       WorkTaskOperation operation,
                                       void *context);

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

// Validates ID domains, canonical dependency rows, and acyclicity without
// executing work. The first error is selected by ascending task/dependency ID,
// so malformed graphs produce the same reason on every host.
[[nodiscard]] bool validate_work_graph(const WorkGraph &graph,
                                       std::string &reason);

} // namespace draft
