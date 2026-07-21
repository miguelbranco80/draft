// Deterministic execution engine for explicit command-local work graphs.
//
// See work_graph.h for the public ownership and determinism contract. This file
// owns only per-run scheduling state: canonical reverse edges, dependency
// counters, a smallest-ID ready heap, bounded worker loops, and synchronization.
// A one-worker run executes the same loop on the calling thread to avoid paying
// thread creation and join overhead for the common one-task wave. All state dies
// after the synchronous run and no result is cached across compiler commands.

#include "base/work_graph.h"

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <limits>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <pthread.h>
#endif

namespace draft {
namespace {

#if !defined(_WIN32)

// Apple creates ordinary pthreads with a substantially smaller stack than the
// process main thread. Compiler tasks legitimately recurse over authored syntax
// trees, so using std::thread's platform default made a large expression safe
// in a one-worker run but capable of crossing a worker's guard page. Eight MiB
// matches the ordinary main-thread budget on the supported POSIX hosts without
// making stack size depend on semantic input or worker completion order.
constexpr std::size_t kPosixWorkerStackBytes = 8U * 1024U * 1024U;

struct PosixWorkerStart {
  const std::function<void()> *operation = nullptr;
};

void *run_posix_worker(void *opaque) {
  const auto *start = static_cast<const PosixWorkerStart *>(opaque);
  assert(start != nullptr);
  assert(start->operation != nullptr);
  (*start->operation)();
  return nullptr;
}

#endif

// Builds reverse rows in ascending consumer order. Iterating tasks by stable ID
// means push_back already produces canonical rows; no hash table or post-sort is
// needed. Validation has proved every dependency is in range.
[[nodiscard]] std::vector<std::vector<WorkTaskId>> build_consumers(
    const WorkGraph &graph) {
  std::vector<std::vector<WorkTaskId>> consumers(graph.tasks.size());
  for (std::size_t task_index = 0; task_index < graph.tasks.size(); ++task_index) {
    const WorkTaskId task = static_cast<WorkTaskId>(task_index);
    for (const WorkTaskId dependency : graph.tasks[task_index].dependencies) {
      consumers[dependency].push_back(task);
    }
  }
  return consumers;
}

// Work IDs are uint32_t so compiler-owned tables can use compact stable slots.
// Rejecting a larger vector before any narrowing conversion prevents an
// unrepresentable last ID from aliasing an earlier task.
[[nodiscard]] bool task_count_fits_id_domain(std::size_t task_count) {
  return task_count <=
      static_cast<std::size_t>(std::numeric_limits<WorkTaskId>::max()) + 1U;
}

} // namespace

bool validate_work_graph(const WorkGraph &graph, std::string &reason) {
  reason.clear();
  if (!task_count_fits_id_domain(graph.tasks.size())) {
    reason = "work graph has more tasks than the WorkTaskId domain";
    return false;
  }

  // Check each authored row before constructing reverse edges. The row contract
  // gives one canonical representation to equivalent graphs and makes duplicate
  // prerequisites an explicit construction error rather than an indegree bug.
  for (std::size_t task_index = 0; task_index < graph.tasks.size(); ++task_index) {
    const WorkTask &task = graph.tasks[task_index];
    for (std::size_t dependency_index = 0;
         dependency_index < task.dependencies.size();
         ++dependency_index) {
      const WorkTaskId dependency = task.dependencies[dependency_index];
      if (static_cast<std::size_t>(dependency) >= graph.tasks.size()) {
        reason = "work task " + std::to_string(task_index) +
            " depends on out-of-range task " + std::to_string(dependency);
        return false;
      }
      if (static_cast<std::size_t>(dependency) == task_index) {
        reason = "work task " + std::to_string(task_index) +
            " depends on itself";
        return false;
      }
      if (dependency_index != 0 &&
          task.dependencies[dependency_index - 1] >= dependency) {
        reason = "work task " + std::to_string(task_index) +
            " dependencies are not strictly increasing";
        return false;
      }
    }
  }

  // A sequential smallest-ID Kahn traversal is the validation oracle. The
  // concurrent runner is allowed to start ready work in timing-dependent order,
  // but it never needs timing or a timeout to discover a malformed cycle.
  const std::vector<std::vector<WorkTaskId>> consumers = build_consumers(graph);
  std::vector<std::size_t> remaining_dependencies(graph.tasks.size(), 0);
  std::priority_queue<
      WorkTaskId,
      std::vector<WorkTaskId>,
      std::greater<WorkTaskId>> ready;
  for (std::size_t task_index = 0; task_index < graph.tasks.size(); ++task_index) {
    remaining_dependencies[task_index] =
        graph.tasks[task_index].dependencies.size();
    if (remaining_dependencies[task_index] == 0) {
      ready.push(static_cast<WorkTaskId>(task_index));
    }
  }
  std::size_t visited = 0;
  while (!ready.empty()) {
    const WorkTaskId task = ready.top();
    ready.pop();
    ++visited;
    for (const WorkTaskId consumer : consumers[task]) {
      std::size_t &remaining = remaining_dependencies[consumer];
      --remaining;
      if (remaining == 0) ready.push(consumer);
    }
  }
  if (visited != graph.tasks.size()) {
    reason = "work graph contains a dependency cycle";
    return false;
  }
  return true;
}

WorkGraphRunResult run_work_graph(
    const WorkGraph &graph,
    WorkGraphRunOptions options,
    WorkTaskOperation operation,
    void *context) {
  WorkGraphRunResult result;
  result.tasks.resize(graph.tasks.size());
  std::string validation_failure;
  if (!validate_work_graph(graph, validation_failure)) {
    if (!result.tasks.empty()) {
      result.tasks.front().state = WorkTaskState::Failed;
      result.tasks.front().failure = std::move(validation_failure);
    }
    return result;
  }
  // Empty work is complete without an operation. This lets a higher phase use
  // one code path for an empty selected package/task set without fabricating a
  // callback that can never be invoked.
  if (graph.tasks.empty()) {
    result.ok = true;
    return result;
  }
  if (operation == nullptr) {
    if (!result.tasks.empty()) {
      result.tasks.front().state = WorkTaskState::Failed;
      result.tasks.front().failure = "work graph operation is null";
    }
    return result;
  }

  std::size_t worker_count = options.worker_count;
  if (worker_count == 0) {
    worker_count = static_cast<std::size_t>(std::thread::hardware_concurrency());
    if (worker_count == 0) worker_count = 1;
  }
  worker_count = std::min(worker_count, graph.tasks.size());
  result.workers_used = worker_count;

  const std::vector<std::vector<WorkTaskId>> consumers = build_consumers(graph);
  std::vector<std::size_t> completed_dependencies(graph.tasks.size(), 0);
  std::vector<WorkTaskId> lowest_failed_dependency(
      graph.tasks.size(), std::numeric_limits<WorkTaskId>::max());
  std::priority_queue<
      WorkTaskId,
      std::vector<WorkTaskId>,
      std::greater<WorkTaskId>> ready;
  for (std::size_t task_index = 0; task_index < graph.tasks.size(); ++task_index) {
    if (graph.tasks[task_index].dependencies.empty()) {
      ready.push(static_cast<WorkTaskId>(task_index));
    }
  }

  std::mutex mutex;
  std::condition_variable changed;
  std::size_t unfinished = graph.tasks.size();

  // Complete one invoked or transitively skipped task while holding mutex.
  // Newly blocked consumers are processed through an explicit stack so a deep
  // failure chain cannot overflow the C++ call stack. Every consumer row is
  // already sorted; the minimum failed dependency is therefore stable even
  // when prerequisite operations finish in different orders.
  const auto publish_completion = [&](WorkTaskId completed) {
    std::vector<WorkTaskId> propagation;
    propagation.push_back(completed);
    while (!propagation.empty()) {
      const WorkTaskId task = propagation.back();
      propagation.pop_back();
      --unfinished;
      const bool succeeded =
          result.tasks[task].state == WorkTaskState::Succeeded;
      for (const WorkTaskId consumer : consumers[task]) {
        ++completed_dependencies[consumer];
        if (!succeeded) {
          lowest_failed_dependency[consumer] = std::min(
              lowest_failed_dependency[consumer], task);
        }
        if (completed_dependencies[consumer] !=
            graph.tasks[consumer].dependencies.size()) {
          continue;
        }
        if (lowest_failed_dependency[consumer] !=
            std::numeric_limits<WorkTaskId>::max()) {
          result.tasks[consumer].state = WorkTaskState::SkippedDependency;
          result.tasks[consumer].failure =
              "dependency task " +
              std::to_string(lowest_failed_dependency[consumer]) + " failed";
          propagation.push_back(consumer);
        } else {
          ready.push(consumer);
        }
      }
    }
  };

  const auto worker = [&]() {
    while (true) {
      WorkTaskId task = 0;
      {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&]() {
          return unfinished == 0 || !ready.empty();
        });
        if (unfinished == 0) return;
        task = ready.top();
        ready.pop();
        result.tasks[task].state = WorkTaskState::Running;
      }

      std::string failure;
      const bool succeeded = operation(context, task, failure);

      {
        std::lock_guard lock(mutex);
        if (succeeded) {
          result.tasks[task].state = WorkTaskState::Succeeded;
          result.tasks[task].failure.clear();
        } else {
          result.tasks[task].state = WorkTaskState::Failed;
          result.tasks[task].failure = failure.empty()
              ? "work task " + std::to_string(task) + " failed"
              : std::move(failure);
        }
        publish_completion(task);
      }
      changed.notify_all();
    }
  };

  if (worker_count == 1) {
    // The scheduling state is identical to the threaded path. Running the loop
    // directly is an execution optimization only and preserves the same task
    // states, failure propagation, and workers_used evidence.
    worker();
  } else {
#if defined(_WIN32)
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (std::size_t worker_index = 0;
         worker_index < worker_count;
         ++worker_index) {
      workers.emplace_back(worker);
    }
    for (std::thread &thread : workers) thread.join();
#else
    // pthread_attr_setstacksize is the one platform operation intentionally
    // kept at this low-level executor boundary. Semantic callers remain fully
    // portable and cannot vary the stack budget. No worker starts with a
    // pointer to temporary task data: start and worker_operation live until all
    // pthreads below have joined.
    const std::function<void()> worker_operation = worker;
    PosixWorkerStart start{&worker_operation};
    pthread_attr_t attributes;
    const int initialized = pthread_attr_init(&attributes);
    const int stack_configured = initialized == 0
        ? pthread_attr_setstacksize(&attributes, kPosixWorkerStackBytes)
        : initialized;
    std::vector<pthread_t> posix_workers;
    bool launch_failed = stack_configured != 0;
    if (!launch_failed) {
      posix_workers.reserve(worker_count);
      for (std::size_t worker_index = 0;
           worker_index < worker_count;
           ++worker_index) {
        pthread_t thread{};
        if (pthread_create(
                &thread,
                &attributes,
                run_posix_worker,
                &start) != 0) {
          launch_failed = true;
          break;
        }
        posix_workers.push_back(thread);
      }
    }
    if (initialized == 0) (void)pthread_attr_destroy(&attributes);

    // A partial pool can still drain the complete graph. If no pthread started,
    // use the caller loop so all task-owned operations reach a terminal state
    // before reporting the launch failure; callers never observe a half-running
    // graph or live thread after this synchronous function returns.
    result.workers_used = posix_workers.empty() ? 1 : posix_workers.size();
    if (posix_workers.empty()) worker();
    for (pthread_t thread : posix_workers) (void)pthread_join(thread, nullptr);
    if (launch_failed) {
      result.tasks.front().state = WorkTaskState::Failed;
      result.tasks.front().failure =
          "cannot start requested work graph worker pool";
      result.ok = false;
      return result;
    }
#endif
  }

  result.ok = std::all_of(
      result.tasks.begin(),
      result.tasks.end(),
      [](const WorkTaskResult &task) {
        return task.state == WorkTaskState::Succeeded;
      });
  return result;
}

} // namespace draft
