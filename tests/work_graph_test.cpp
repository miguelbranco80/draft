// Focused contracts for deterministic dependency scheduling.
//
// These tests exercise graph validation, true concurrent execution, stable
// task-indexed output, and transitive failure blocking without involving a
// compiler subsystem. Native LLVM tests separately prove that package object
// tasks obey this base-level operation contract.

#include "base/work_graph.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct TestState {
  int failures = 0;
};

#define EXPECT(state, condition)                                                \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << __FILE__ << ':' << __LINE__                                  \
                << ": expectation failed: " #condition << '\n';                \
      ++(state).failures;                                                       \
    }                                                                           \
  } while (false)

struct DependencyContext {
  std::vector<std::atomic<bool>> completed;
  std::vector<std::uint32_t> values;

  explicit DependencyContext(std::size_t count)
      : completed(count), values(count, 0) {}
};

// Each task checks the prerequisite slot that the graph says must already be
// published. Distinct task slots are written without a lock; this is the caller
// ownership rule expected by native object emission.
bool run_dependency_task(
    void *opaque,
    draft::WorkTaskId task,
    std::string &failure) {
  auto &context = *static_cast<DependencyContext *>(opaque);
  if (task != 0 && !context.completed[task - 1].load()) {
    failure = "declared prerequisite was not completed";
    return false;
  }
  context.values[task] = task * 10U;
  context.completed[task].store(true);
  return true;
}

void test_dependency_execution(TestState &state) {
  draft::WorkGraph graph;
  graph.tasks = {{{}}, {{0}}, {{1}}, {{2}}};
  DependencyContext context(graph.tasks.size());
  draft::WorkGraphRunOptions options;
  options.worker_count = 4;
  const draft::WorkGraphRunResult run = draft::run_work_graph(
      graph, options, run_dependency_task, &context);
  EXPECT(state, run.ok);
  EXPECT(state, run.workers_used == 4);
  EXPECT(state, run.tasks.size() == 4);
  EXPECT(state, context.values[0] == 0);
  EXPECT(state, context.values[1] == 10);
  EXPECT(state, context.values[2] == 20);
  EXPECT(state, context.values[3] == 30);
}

struct ParallelContext {
  std::mutex mutex;
  std::condition_variable changed;
  std::size_t arrived = 0;
  bool timed_out = false;
};

// Four independent tasks rendezvous before returning. A falsely sequential
// implementation cannot satisfy the predicate and therefore produces a direct
// failure instead of silently passing a test that never proved concurrency.
bool run_parallel_task(
    void *opaque,
    draft::WorkTaskId,
    std::string &failure) {
  auto &context = *static_cast<ParallelContext *>(opaque);
  std::unique_lock lock(context.mutex);
  ++context.arrived;
  context.changed.notify_all();
  const bool all_arrived = context.changed.wait_for(
      lock,
      std::chrono::seconds(5),
      [&]() { return context.arrived == 4; });
  if (!all_arrived) {
    context.timed_out = true;
    failure = "independent tasks did not execute concurrently";
    context.changed.notify_all();
    return false;
  }
  return true;
}

void test_independent_tasks_run_concurrently(TestState &state) {
  draft::WorkGraph graph;
  graph.tasks.resize(4);
  ParallelContext context;
  draft::WorkGraphRunOptions options;
  options.worker_count = 4;
  const draft::WorkGraphRunResult run = draft::run_work_graph(
      graph, options, run_parallel_task, &context);
  EXPECT(state, run.ok);
  EXPECT(state, run.workers_used == 4);
  EXPECT(state, !context.timed_out);
}

struct FailureContext {
  std::vector<std::atomic<bool>> called;

  explicit FailureContext(std::size_t count) : called(count) {}
};

bool run_failing_task(
    void *opaque,
    draft::WorkTaskId task,
    std::string &failure) {
  auto &context = *static_cast<FailureContext *>(opaque);
  context.called[task].store(true);
  if (task == 0) {
    failure = "root task rejected its input";
    return false;
  }
  return true;
}

void test_failure_blocks_only_consumers(TestState &state) {
  draft::WorkGraph graph;
  graph.tasks = {{{}}, {{0}}, {{}}, {{1, 2}}};
  FailureContext context(graph.tasks.size());
  draft::WorkGraphRunOptions options;
  options.worker_count = 4;
  const draft::WorkGraphRunResult run = draft::run_work_graph(
      graph, options, run_failing_task, &context);
  EXPECT(state, !run.ok);
  EXPECT(state, run.tasks[0].state == draft::WorkTaskState::Failed);
  EXPECT(state, run.tasks[0].failure == "root task rejected its input");
  EXPECT(state,
      run.tasks[1].state == draft::WorkTaskState::SkippedDependency);
  EXPECT(state, run.tasks[1].failure == "dependency task 0 failed");
  EXPECT(state, run.tasks[2].state == draft::WorkTaskState::Succeeded);
  EXPECT(state,
      run.tasks[3].state == draft::WorkTaskState::SkippedDependency);
  EXPECT(state, run.tasks[3].failure == "dependency task 1 failed");
  EXPECT(state, context.called[0].load());
  EXPECT(state, !context.called[1].load());
  EXPECT(state, context.called[2].load());
  EXPECT(state, !context.called[3].load());
}

bool no_op_task(void *, draft::WorkTaskId, std::string &) {
  return true;
}

void test_graph_validation(TestState &state) {
  std::string reason;
  draft::WorkGraph unsorted;
  unsorted.tasks = {{{}}, {{}}, {{1, 0}}};
  EXPECT(state, !draft::validate_work_graph(unsorted, reason));
  EXPECT(state, reason ==
      "work task 2 dependencies are not strictly increasing");

  draft::WorkGraph cycle;
  cycle.tasks = {{{1}}, {{0}}};
  EXPECT(state, !draft::validate_work_graph(cycle, reason));
  EXPECT(state, reason == "work graph contains a dependency cycle");
  const draft::WorkGraphRunResult run = draft::run_work_graph(
      cycle, {}, no_op_task, nullptr);
  EXPECT(state, !run.ok);
  EXPECT(state, run.tasks[0].state == draft::WorkTaskState::Failed);
  EXPECT(state, run.tasks[0].failure ==
      "work graph contains a dependency cycle");
}

} // namespace

int main() {
  TestState state;
  test_dependency_execution(state);
  test_independent_tasks_run_concurrently(state);
  test_failure_blocks_only_consumers(state);
  test_graph_validation(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " work graph expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all work graph tests passed\n";
  return EXIT_SUCCESS;
}
