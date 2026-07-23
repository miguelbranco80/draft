// Deterministic command-lifetime execution of explicit dependency graphs.
//
// WorkExecutor owns one bounded set of operating-system workers and reuses them
// for every synchronous graph run in a compiler command. Each run constructs
// its own canonical reverse edges, dependency counters, smallest-ID ready heap,
// and result slots. Workers borrow that stack-owned state only until run
// returns and sleep without retaining compiler data between runs. This module
// therefore amortizes thread creation without becoming a compiler cache or
// learning about packages, semantics, targets, or LLVM.
//
// A graph operation writes only its task-owned caller slot. Scheduling order
// may differ, but dependency failure, result identity, and later publication
// order remain functions only of stable task IDs. One-worker qualification
// executes the identical dependency algorithm on the calling thread.

#include "base/work_graph.h"

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <limits>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace draft {
namespace {

// Compiler tasks legitimately recurse over authored syntax trees. An explicit
// eight-MiB stack gives every background worker the same practical recursion
// budget on supported hosts instead of accepting platform thread defaults.
constexpr std::size_t kWorkerStackBytes = 8U * 1024U * 1024U;

// Builds reverse rows in ascending consumer order. Iterating tasks by stable ID
// means push_back already produces canonical rows; no hash table or post-sort
// is needed. Validation has proved every dependency is in range.
[[nodiscard]] std::vector<std::vector<WorkTaskId>>
build_consumers(const WorkGraph &graph) {
  std::vector<std::vector<WorkTaskId>> consumers(graph.tasks.size());
  for (std::size_t task_index = 0; task_index < graph.tasks.size();
       ++task_index) {
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
  for (std::size_t task_index = 0; task_index < graph.tasks.size();
       ++task_index) {
    const WorkTask &task = graph.tasks[task_index];
    for (std::size_t dependency_index = 0;
         dependency_index < task.dependencies.size(); ++dependency_index) {
      const WorkTaskId dependency = task.dependencies[dependency_index];
      if (static_cast<std::size_t>(dependency) >= graph.tasks.size()) {
        reason = "work task " + std::to_string(task_index) +
                 " depends on out-of-range task " + std::to_string(dependency);
        return false;
      }
      if (static_cast<std::size_t>(dependency) == task_index) {
        reason =
            "work task " + std::to_string(task_index) + " depends on itself";
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
  // concurrent runner may start ready work in timing-dependent order, but it
  // never needs timing or a timeout to discover a malformed cycle.
  const std::vector<std::vector<WorkTaskId>> consumers = build_consumers(graph);
  std::vector<std::size_t> remaining_dependencies(graph.tasks.size(), 0);
  std::priority_queue<WorkTaskId, std::vector<WorkTaskId>,
                      std::greater<WorkTaskId>>
      ready;
  for (std::size_t task_index = 0; task_index < graph.tasks.size();
       ++task_index) {
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
      if (remaining == 0)
        ready.push(consumer);
    }
  }
  if (visited != graph.tasks.size()) {
    reason = "work graph contains a dependency cycle";
    return false;
  }
  return true;
}

struct WorkExecutor::Implementation {
  // RunState owns every mutable fact for one synchronous graph execution. The
  // WorkExecutor::run stack owns this value, and run waits for participating
  // workers to leave before clearing active. Background workers therefore never
  // retain a pointer into a completed call.
  struct RunState {
    const WorkGraph *graph = nullptr;
    WorkTaskOperation operation = nullptr;
    void *context = nullptr;
    WorkGraphRunResult result;
    std::vector<std::vector<WorkTaskId>> consumers;
    std::vector<std::size_t> completed_dependencies;
    std::vector<WorkTaskId> lowest_failed_dependency;
    std::priority_queue<WorkTaskId, std::vector<WorkTaskId>,
                        std::greater<WorkTaskId>>
        ready;
    std::size_t unfinished = 0;
    // worker_limit is immutable for the run and selects worker indices.
    // workers_remaining counts only participating workers which have not yet
    // left execute_active_run; keeping the two facts separate prevents an early
    // finisher from accidentally excluding a higher-index worker which has not
    // observed this generation yet.
    std::size_t worker_limit = 0;
    std::size_t workers_remaining = 0;
  };

  struct WorkerStart {
    Implementation *implementation = nullptr;
    std::size_t index = 0;
  };

  explicit Implementation(std::size_t requested_workers)
      : maximum_workers(requested_workers) {
    if (maximum_workers == 0) {
      maximum_workers =
          static_cast<std::size_t>(std::thread::hardware_concurrency());
      if (maximum_workers == 0)
        maximum_workers = 1;
    }
  }

  ~Implementation() {
    {
      std::lock_guard lock(mutex);
      stopping = true;
      ++generation;
    }
    changed.notify_all();
#if defined(_WIN32)
    for (HANDLE worker : workers) {
      if (worker == nullptr)
        continue;
      (void)WaitForSingleObject(worker, INFINITE);
      (void)CloseHandle(worker);
    }
#else
    for (pthread_t worker : workers)
      (void)pthread_join(worker, nullptr);
#endif
  }

  // Grows the pool only to the capacity requested by this run. Starting all
  // host cores for an early two-task wave would merely move avoidable setup
  // cost into the first compiler phase. A creation failure fixes the available
  // capacity for the remainder of the executor lifetime; every successfully
  // created worker remains usable and workers_used reports that degradation.
  void ensure_workers(std::size_t requested_workers) {
    requested_workers = std::min(requested_workers, maximum_workers);
    if (worker_creation_failed || workers.size() >= requested_workers)
      return;
    if (starts.empty())
      starts.resize(maximum_workers);
#if defined(_WIN32)
    if (workers.capacity() < maximum_workers)
      workers.reserve(maximum_workers);
    for (std::size_t index = workers.size(); index < requested_workers;
         ++index) {
      starts[index] = {this, index};
      const uintptr_t handle =
          _beginthreadex(nullptr, static_cast<unsigned>(kWorkerStackBytes),
                         &Implementation::windows_worker_entry, &starts[index],
                         STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
      if (handle == 0) {
        worker_creation_failed = true;
        break;
      }
      workers.push_back(reinterpret_cast<HANDLE>(handle));
    }
#else
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) {
      worker_creation_failed = true;
      return;
    }
    if (pthread_attr_setstacksize(&attributes, kWorkerStackBytes) != 0) {
      (void)pthread_attr_destroy(&attributes);
      worker_creation_failed = true;
      return;
    }
    if (workers.capacity() < maximum_workers)
      workers.reserve(maximum_workers);
    for (std::size_t index = workers.size(); index < requested_workers;
         ++index) {
      starts[index] = {this, index};
      pthread_t worker{};
      if (pthread_create(&worker, &attributes,
                         &Implementation::posix_worker_entry,
                         &starts[index]) != 0) {
        worker_creation_failed = true;
        break;
      }
      workers.push_back(worker);
    }
    (void)pthread_attr_destroy(&attributes);
#endif
  }

  // Marks one invoked or transitively skipped task complete. The caller holds
  // mutex. Consumer rows are stable-ID ordered, so the lowest failed dependency
  // and the resulting diagnostic are independent of worker completion order.
  void publish_completion(RunState &run, WorkTaskId completed) {
    std::vector<WorkTaskId> propagation{completed};
    while (!propagation.empty()) {
      const WorkTaskId task = propagation.back();
      propagation.pop_back();
      --run.unfinished;
      const bool succeeded =
          run.result.tasks[task].state == WorkTaskState::Succeeded;
      for (const WorkTaskId consumer : run.consumers[task]) {
        ++run.completed_dependencies[consumer];
        if (!succeeded) {
          run.lowest_failed_dependency[consumer] =
              std::min(run.lowest_failed_dependency[consumer], task);
        }
        if (run.completed_dependencies[consumer] !=
            run.graph->tasks[consumer].dependencies.size()) {
          continue;
        }
        if (run.lowest_failed_dependency[consumer] !=
            std::numeric_limits<WorkTaskId>::max()) {
          run.result.tasks[consumer].state = WorkTaskState::SkippedDependency;
          run.result.tasks[consumer].failure =
              "dependency task " +
              std::to_string(run.lowest_failed_dependency[consumer]) +
              " failed";
          propagation.push_back(consumer);
        } else {
          run.ready.push(consumer);
        }
      }
    }
  }

  // Drains available tasks until the graph is terminal. A worker may wait while
  // another participant owns the only runnable task; completion either exposes
  // more ready work or terminalizes the graph and wakes every waiter.
  void execute_active_run() {
    while (true) {
      WorkTaskId task = 0;
      WorkTaskOperation operation = nullptr;
      void *context = nullptr;
      {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&]() {
          return active == nullptr || active->unfinished == 0 ||
                 !active->ready.empty();
        });
        if (active == nullptr || active->unfinished == 0)
          return;
        task = active->ready.top();
        active->ready.pop();
        active->result.tasks[task].state = WorkTaskState::Running;
        operation = active->operation;
        context = active->context;
      }

      std::string failure;
      const bool succeeded = operation(context, task, failure);

      {
        std::lock_guard lock(mutex);
        assert(active != nullptr);
        WorkTaskResult &task_result = active->result.tasks[task];
        if (succeeded) {
          task_result.state = WorkTaskState::Succeeded;
          task_result.failure.clear();
        } else {
          task_result.state = WorkTaskState::Failed;
          task_result.failure =
              failure.empty() ? "work task " + std::to_string(task) + " failed"
                              : std::move(failure);
        }
        publish_completion(*active, task);
      }
      changed.notify_all();
    }
  }

  // Each persistent worker observes a monotonically increasing run generation.
  // Workers above the current run's requested count remain asleep until the
  // next generation; participating workers all leave before run clears active.
  void worker_loop(std::size_t index) {
    std::uint64_t observed_generation = 0;
    while (true) {
      {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&]() {
          return stopping || generation != observed_generation;
        });
        if (stopping)
          return;
        observed_generation = generation;
        if (active == nullptr || index >= active->worker_limit) {
          continue;
        }
      }

      execute_active_run();

      {
        std::lock_guard lock(mutex);
        assert(active != nullptr);
        assert(active->workers_remaining != 0);
        --active->workers_remaining;
      }
      run_completed.notify_one();
    }
  }

#if defined(_WIN32)
  static unsigned __stdcall windows_worker_entry(void *opaque) {
    const auto *start = static_cast<const WorkerStart *>(opaque);
    assert(start != nullptr && start->implementation != nullptr);
    start->implementation->worker_loop(start->index);
    return 0;
  }
  std::vector<HANDLE> workers;
#else
  static void *posix_worker_entry(void *opaque) {
    const auto *start = static_cast<const WorkerStart *>(opaque);
    assert(start != nullptr && start->implementation != nullptr);
    start->implementation->worker_loop(start->index);
    return nullptr;
  }
  std::vector<pthread_t> workers;
#endif

  std::size_t maximum_workers = 1;
  bool worker_creation_failed = false;
  bool stopping = false;
  std::uint64_t generation = 0;
  std::mutex mutex;
  std::condition_variable changed;
  std::condition_variable run_completed;
  RunState *active = nullptr;
  std::vector<WorkerStart> starts;
};

WorkExecutor::WorkExecutor(std::size_t maximum_workers)
    : implementation_(std::make_unique<Implementation>(maximum_workers)) {}

WorkExecutor::~WorkExecutor() = default;

std::size_t WorkExecutor::maximum_workers() const {
  return implementation_->maximum_workers;
}

WorkGraphRunResult WorkExecutor::run(const WorkGraph &graph,
                                     WorkGraphRunOptions options,
                                     WorkTaskOperation operation,
                                     void *context) {
  WorkGraphRunResult early_result;
  early_result.tasks.resize(graph.tasks.size());
  std::string validation_failure;
  if (!validate_work_graph(graph, validation_failure)) {
    if (!early_result.tasks.empty()) {
      early_result.tasks.front().state = WorkTaskState::Failed;
      early_result.tasks.front().failure = std::move(validation_failure);
    }
    return early_result;
  }
  if (graph.tasks.empty()) {
    early_result.ok = true;
    return early_result;
  }
  if (operation == nullptr) {
    early_result.tasks.front().state = WorkTaskState::Failed;
    early_result.tasks.front().failure = "work graph operation is null";
    return early_result;
  }

  std::size_t worker_count = options.worker_count;
  if (worker_count == 0)
    worker_count = implementation_->maximum_workers;
  worker_count = std::min(
      {worker_count, implementation_->maximum_workers, graph.tasks.size()});

  Implementation::RunState run;
  run.graph = &graph;
  run.operation = operation;
  run.context = context;
  run.result.tasks.resize(graph.tasks.size());
  run.result.workers_used = worker_count;
  run.consumers = build_consumers(graph);
  run.completed_dependencies.resize(graph.tasks.size(), 0);
  run.lowest_failed_dependency.assign(graph.tasks.size(),
                                      std::numeric_limits<WorkTaskId>::max());
  run.unfinished = graph.tasks.size();
  for (std::size_t task_index = 0; task_index < graph.tasks.size();
       ++task_index) {
    if (graph.tasks[task_index].dependencies.empty()) {
      run.ready.push(static_cast<WorkTaskId>(task_index));
    }
  }

  if (worker_count > 1) {
    implementation_->ensure_workers(worker_count);
    worker_count = std::min(worker_count, implementation_->workers.size());
    if (worker_count == 0)
      worker_count = 1;
    run.result.workers_used = worker_count;
  }

  {
    std::lock_guard lock(implementation_->mutex);
    assert(implementation_->active == nullptr &&
           "one command executor cannot run two graphs concurrently");
    implementation_->active = &run;
    if (worker_count > 1) {
      run.worker_limit = worker_count;
      run.workers_remaining = worker_count;
      ++implementation_->generation;
    }
  }

  if (worker_count == 1) {
    implementation_->execute_active_run();
  } else {
    implementation_->changed.notify_all();
    std::unique_lock lock(implementation_->mutex);
    implementation_->run_completed.wait(
        lock, [&]() { return run.workers_remaining == 0; });
  }

  {
    std::lock_guard lock(implementation_->mutex);
    assert(implementation_->active == &run);
    implementation_->active = nullptr;
  }
  implementation_->changed.notify_all();

  run.result.ok = std::all_of(run.result.tasks.begin(), run.result.tasks.end(),
                              [](const WorkTaskResult &task) {
                                return task.state == WorkTaskState::Succeeded;
                              });
  return std::move(run.result);
}

} // namespace draft
