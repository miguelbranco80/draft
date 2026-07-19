// Implementation of the command-owned compiler timing recorder.
//
// This module converts nested lifetime scopes into a compact diagnostic tree.
// It owns no compiler phase policy: callers decide where semantic boundaries
// begin and whether a boundary belongs in the summary. The implementation is
// deliberately linear and inspectable. Recording and rendering are linear in
// the number of events plus counters, including detailed reports for large
// package graphs. Compilation never consults the result.
//
// See timing.h for ownership, nesting, clock, and determinism invariants.

#include "base/timing.h"

#include <cassert>
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace draft {

namespace {

constexpr std::size_t no_parent = std::numeric_limits<std::size_t>::max();

// steady_now_nanoseconds is the only production clock boundary. Keeping the
// conversion here lets unit tests replace the complete notion of time without
// sleeping or depending on scheduler precision.
[[nodiscard]] std::uint64_t steady_now_nanoseconds(void *state) {
  (void)state;
  const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
  const auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
  assert(nanoseconds >= 0 && "steady_clock epoch duration must be nonnegative");
  return static_cast<std::uint64_t>(nanoseconds);
}

[[nodiscard]] double milliseconds(std::uint64_t nanoseconds) {
  return static_cast<double>(nanoseconds) / 1'000'000.0;
}

} // namespace

TimingScope::TimingScope(TimingRecorder *recorder, std::size_t event_index)
    : recorder_(recorder), event_index_(event_index) {}

TimingScope::TimingScope(TimingScope &&other) noexcept
    : recorder_(other.recorder_), event_index_(other.event_index_) {
  other.recorder_ = nullptr;
}

TimingScope &TimingScope::operator=(TimingScope &&other) noexcept {
  if (this == &other) return *this;
  finish();
  recorder_ = other.recorder_;
  event_index_ = other.event_index_;
  other.recorder_ = nullptr;
  return *this;
}

TimingScope::~TimingScope() {
  finish();
}

void TimingScope::finish() {
  if (recorder_ == nullptr) return;
  recorder_->finish_event(event_index_);
  recorder_ = nullptr;
}

TimingRecorder::TimingRecorder(
    TimingOutput output,
    TimingNowFunction now,
    void *clock_state)
    : output_(output),
      now_(now == nullptr ? steady_now_nanoseconds : now),
      clock_state_(clock_state) {
  if (!enabled()) return;

  Event root;
  root.name = "command total";
  root.parent = no_parent;
  root.started_nanoseconds = now_nanoseconds();
  events_.push_back(std::move(root));
  active_events_.push_back(0);
}

bool TimingRecorder::enabled() const {
  return output_ != TimingOutput::Disabled;
}

TimingOutput TimingRecorder::output() const {
  return output_;
}

TimingScope TimingRecorder::scope(
    std::string_view name,
    TimingVisibility visibility) {
  if (!enabled() ||
      (output_ == TimingOutput::Summary &&
       visibility == TimingVisibility::Detail)) {
    return {};
  }

  assert(!active_events_.empty() && "enabled timing recorder must have a root");
  Event event;
  event.name = std::string(name);
  event.parent = active_events_.back();
  event.visibility = visibility;
  event.started_nanoseconds = now_nanoseconds();
  const std::size_t index = events_.size();
  events_.push_back(std::move(event));
  active_events_.push_back(index);
  return TimingScope(this, index);
}

void TimingRecorder::add_counter(std::string_view name, std::uint64_t amount) {
  if (!enabled()) return;
  for (Counter &counter : counters_) {
    if (counter.name == name) {
      counter.value += amount;
      return;
    }
  }
  counters_.push_back(Counter{std::string(name), amount});
}

void TimingRecorder::record_child_process(
    std::uint64_t user_nanoseconds,
    std::uint64_t system_nanoseconds) {
  if (!enabled()) return;
  assert(!active_events_.empty() && "child process must belong to an event");
  Event &event = events_[active_events_.back()];
  event.child_user_nanoseconds += user_nanoseconds;
  event.child_system_nanoseconds += system_nanoseconds;
  child_user_nanoseconds_ += user_nanoseconds;
  child_system_nanoseconds_ += system_nanoseconds;
  child_process_count_ += 1;
  add_counter("external processes", 1);
}

std::string TimingRecorder::render() const {
  if (!enabled()) return {};

  const std::uint64_t snapshot = now_nanoseconds();
  std::vector<std::uint64_t> elapsed;
  elapsed.reserve(events_.size());
  for (const Event &event : events_) {
    elapsed.push_back(event.finished
        ? event.elapsed_nanoseconds
        : snapshot - event.started_nanoseconds);
  }

  // Parent events are always appended before their children. One forward pass
  // can therefore accumulate direct-child wall time and visible indentation
  // without repeatedly walking the event tree for every rendered row.
  std::vector<std::uint64_t> child_elapsed(events_.size(), 0);
  std::vector<std::size_t> visible_depth(events_.size(), 0);
  for (std::size_t index = 0; index < events_.size(); ++index) {
    const std::size_t parent = events_[index].parent;
    if (parent == no_parent) continue;
    assert(parent < index && "timing parent must precede its child");
    child_elapsed[parent] += elapsed[index];
    const bool parent_visible = output_ == TimingOutput::All ||
        events_[parent].visibility == TimingVisibility::Summary;
    visible_depth[index] = visible_depth[parent] + (parent_visible ? 1U : 0U);
  }

  std::ostringstream stream;
  stream << "timings (wall clock):\n" << std::fixed << std::setprecision(3);
  for (std::size_t index = 0; index < events_.size(); ++index) {
    const Event &event = events_[index];
    if (output_ == TimingOutput::Summary &&
        event.visibility == TimingVisibility::Detail) {
      continue;
    }

    // Properly nested scopes cannot exceed their parent. Clamp defensively so
    // a clock with coarse resolution or a future partially recorded event can
    // never wrap an unsigned exclusive duration into a huge diagnostic value.
    const std::uint64_t self = child_elapsed[index] <= elapsed[index]
        ? elapsed[index] - child_elapsed[index]
        : 0;

    stream << std::string((visible_depth[index] + 1) * 2, ' ')
           << event.name << ": " << milliseconds(elapsed[index]) << " ms";
    if (output_ == TimingOutput::All) {
      stream << " (self " << milliseconds(self) << " ms)";
      if (event.child_user_nanoseconds != 0 ||
          event.child_system_nanoseconds != 0) {
        stream << " [child user "
               << milliseconds(event.child_user_nanoseconds)
               << " ms, system "
               << milliseconds(event.child_system_nanoseconds) << " ms]";
      }
    }
    stream << '\n';
  }

  if (!counters_.empty()) {
    stream << "timing counters:\n";
    for (const Counter &counter : counters_) {
      stream << "  " << counter.name << ": " << counter.value << '\n';
    }
  }
  if (child_process_count_ != 0) {
    stream << "child process CPU:\n"
           << "  user: " << milliseconds(child_user_nanoseconds_) << " ms\n"
           << "  system: " << milliseconds(child_system_nanoseconds_) << " ms\n";
  }
  return stream.str();
}

std::uint64_t TimingRecorder::now_nanoseconds() const {
  assert(now_ != nullptr && "timing recorder clock must be configured");
  return now_(clock_state_);
}

void TimingRecorder::finish_event(std::size_t event_index) {
  assert(event_index < events_.size() && "timing event index must be valid");
  Event &event = events_[event_index];
  if (event.finished) return;
  assert(!active_events_.empty() && active_events_.back() == event_index &&
         "timing scopes must finish in last-in, first-out order");
  event.elapsed_nanoseconds = now_nanoseconds() - event.started_nanoseconds;
  event.finished = true;
  active_events_.pop_back();
}

} // namespace draft
