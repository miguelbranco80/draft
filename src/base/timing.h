// Explicit hierarchical timing for compiler and driver operations.
//
// Timing is diagnostic observation, not a semantic compiler input. One
// command-owned TimingRecorder receives nested scopes from the driver,
// compiler, validation runner, and native tool adapter. The recorder owns the
// event names and durations until the command prints one report; participating
// modules only borrow its pointer through their existing option structs.
//
// The recorder itself is single-threaded. Ordinary sequential phases use one
// explicit active-scope stack and finish in last-in, first-out order. Parallel
// workers instead measure their own operation, store the duration beside their
// task result, and let the owning thread append completed sibling events after
// joining. Appending those records in stable task-ID order makes report order
// independent of scheduling without putting locks or thread IDs in this module.
//
// A clock callback can be supplied by tests. Production uses steady_clock, so
// wall-clock adjustments cannot produce negative or discontinuous durations.
// No Draft specification section applies: enabling this facility changes only
// diagnostic text written by the process driver.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

// Selects how much timing detail the final report exposes. Summary includes
// command and major phase scopes. All additionally includes package/tool-level
// scopes and each visible scope's exclusive ("self") wall time.
enum class TimingOutput {
  Disabled,
  Summary,
  All,
};

// Marks whether an event belongs in the compact report or only in the detailed
// report. Detail scopes are no-ops in Summary mode, avoiding package-level
// allocation and clock reads when exclusive time is not printed.
enum class TimingVisibility {
  Summary,
  Detail,
};

using TimingNowFunction = std::uint64_t (*)(void *state);

class TimingRecorder;

// One already measured child of a completed parallel operation. The caller
// owns name and supplies children in deterministic semantic order; the
// recorder copies the name while replaying the group on its owning thread.
// Child durations may overlap because they can describe independently
// scheduled work inside the parent. They are diagnostic observations only.
struct CompletedTimingEvent {
  std::string name;
  std::uint64_t elapsed_nanoseconds = 0;
};

// TimingScope is the movable, noncopyable lifetime token for one event. Its
// destructor closes the event, so early diagnostic returns cannot leave the
// recorder's nesting stack inconsistent. It owns no event data; the recorder
// must outlive every scope it created, which naturally follows from the
// command-owned recorder and block-local scopes used by the pipeline.
class TimingScope {
public:
  TimingScope() = default;
  TimingScope(const TimingScope &) = delete;
  TimingScope &operator=(const TimingScope &) = delete;
  TimingScope(TimingScope &&other) noexcept;
  TimingScope &operator=(TimingScope &&other) noexcept;
  ~TimingScope();

  // Closes the event now. Calling finish more than once is harmless. Explicit
  // closure is useful when later work in the same lexical block belongs to the
  // parent rather than this event.
  void finish();

private:
  friend class TimingRecorder;

  TimingScope(TimingRecorder *recorder, std::size_t event_index);

  TimingRecorder *recorder_ = nullptr;
  std::size_t event_index_ = 0;
};

// TimingRecorder owns an append-only event tree and stable, insertion-ordered
// counters for one process command. Event indices remain stable even when the
// vector reallocates, and parent indices use no pointers into that storage.
// The implicit root event is "command total" and remains open until render(),
// allowing a driver destructor to report timing on every return path.
//
// Counter names are diagnostic schema: repeated additions with the same exact
// name aggregate. Callers should use units in the name where ambiguity exists,
// for example "LLVM IR bytes" rather than a generic "size".
class TimingRecorder {
public:
  explicit TimingRecorder(
      TimingOutput output,
      TimingNowFunction now = nullptr,
      void *clock_state = nullptr);

  TimingRecorder(const TimingRecorder &) = delete;
  TimingRecorder &operator=(const TimingRecorder &) = delete;

  [[nodiscard]] bool enabled() const;
  [[nodiscard]] TimingOutput output() const;

  // Starts one event beneath the currently active event. Disabled recorders
  // return a no-op scope, so call sites need no timing-specific branch.
  [[nodiscard]] TimingScope scope(
      std::string_view name,
      TimingVisibility visibility = TimingVisibility::Summary);

  // Adds a deterministic work counter. Counters describe how much work was
  // processed across repeated compiler passes; they are not deduplicated by
  // package or file identity unless the caller explicitly does so.
  void add_counter(std::string_view name, std::uint64_t amount = 1);

  // Records resource usage reported by one completed child process. The CPU
  // duration is attached to the active timing event and accumulated for the
  // report footer. It is intentionally separate from wall time: Clang and
  // linker CPU can overlap I/O or parent wait time.
  void record_child_process(
      std::uint64_t user_nanoseconds,
      std::uint64_t system_nanoseconds);

  // Appends an already completed event beneath the active scope. This is the
  // boundary used after joining parallel work: elapsed_nanoseconds was measured
  // by the worker, but the owning thread supplies events in semantic order.
  // The method must not be called by a worker or while another thread touches
  // this recorder. Hidden detail events remain allocation-free in Summary mode.
  void record_completed_event(
      std::string_view name,
      std::uint64_t elapsed_nanoseconds,
      TimingVisibility visibility);

  // Appends one completed parent plus its completed direct children beneath
  // the active scope. Parallel workers use this after joining to preserve a
  // useful hierarchy without ever mutating the recorder themselves. Parent
  // and child insertion follows caller order and is therefore independent of
  // worker completion order.
  void record_completed_event_group(
      std::string_view name,
      std::uint64_t elapsed_nanoseconds,
      TimingVisibility visibility,
      std::span<const CompletedTimingEvent> children);

  // Appends one completed external-process event and accounts its child CPU.
  // Keeping this spelling separate avoids a boolean whose meaning would be
  // unclear at call sites. When a detail row is hidden in Summary mode, child
  // usage is attached to the active parent exactly as for a no-op detail scope.
  void record_completed_process_event(
      std::string_view name,
      std::uint64_t elapsed_nanoseconds,
      std::uint64_t user_nanoseconds,
      std::uint64_t system_nanoseconds,
      TimingVisibility visibility);

  // Renders a snapshot without closing active scopes. All durations use one
  // clock reading, so parent and child rows are internally comparable even
  // when a caller requests a report during an error path.
  [[nodiscard]] std::string render() const;

private:
  friend class TimingScope;

  struct Event {
    std::string name;
    std::size_t parent = 0;
    TimingVisibility visibility = TimingVisibility::Summary;
    std::uint64_t started_nanoseconds = 0;
    std::uint64_t elapsed_nanoseconds = 0;
    std::uint64_t child_user_nanoseconds = 0;
    std::uint64_t child_system_nanoseconds = 0;
    bool finished = false;
  };

  struct Counter {
    std::string name;
    std::uint64_t value = 0;
  };

  [[nodiscard]] std::uint64_t now_nanoseconds() const;
  [[nodiscard]] std::size_t append_completed_event(
      std::string_view name,
      std::uint64_t elapsed_nanoseconds,
      TimingVisibility visibility);
  [[nodiscard]] std::size_t append_completed_child_event(
      std::size_t parent,
      std::string_view name,
      std::uint64_t elapsed_nanoseconds,
      TimingVisibility visibility);
  void finish_event(std::size_t event_index);

  TimingOutput output_ = TimingOutput::Disabled;
  TimingNowFunction now_ = nullptr;
  void *clock_state_ = nullptr;
  std::vector<Event> events_;
  std::vector<std::size_t> active_events_;
  std::vector<Counter> counters_;
  std::uint64_t child_process_count_ = 0;
  std::uint64_t child_user_nanoseconds_ = 0;
  std::uint64_t child_system_nanoseconds_ = 0;
};

} // namespace draft
