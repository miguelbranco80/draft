// Deterministic unit tests for the compiler timing recorder.
//
// These tests replace the monotonic production clock with an explicitly
// advanced integer. They verify nesting, summary filtering, exclusive time,
// counters, and child-process accounting without sleeps or scheduler
// assumptions. Compiler and driver tests cover the phase boundaries that feed
// the recorder; this file isolates the data structure and rendering contract.

#include "base/timing.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] std::uint64_t fake_now(void *state) {
  return *static_cast<std::uint64_t *>(state);
}

void expect_contains(
    std::string_view text,
    std::string_view expected,
    std::string_view description) {
  if (text.find(expected) != std::string_view::npos) return;
  std::cerr << "missing " << description << ": " << expected << '\n'
            << "report was:\n" << text;
  std::exit(1);
}

void expect_absent(
    std::string_view text,
    std::string_view unexpected,
    std::string_view description) {
  if (text.find(unexpected) == std::string_view::npos) return;
  std::cerr << "unexpected " << description << ": " << unexpected << '\n'
            << "report was:\n" << text;
  std::exit(1);
}

void test_summary_filters_detail_but_keeps_accounting() {
  std::uint64_t now = 0;
  draft::TimingRecorder recorder(
      draft::TimingOutput::Summary, fake_now, &now);
  {
    draft::TimingScope compiler = recorder.scope("compiler pipeline");
    now += 2'000'000;
    {
      draft::TimingScope package = recorder.scope(
          "package semantics: app", draft::TimingVisibility::Detail);
      now += 3'000'000;
    }
    now += 1'000'000;
  }
  recorder.add_counter("packages processed", 1);
  now += 4'000'000;

  const std::string report = recorder.render();
  expect_contains(report, "command total: 10.000 ms", "command duration");
  expect_contains(report, "compiler pipeline: 6.000 ms", "phase duration");
  expect_absent(report, "package semantics", "detail event in summary mode");
  expect_absent(report, "self ", "exclusive time in summary mode");
  expect_contains(report, "packages processed: 1", "work counter");
}

void test_all_reports_exclusive_and_child_cpu_time() {
  std::uint64_t now = 5'000'000;
  draft::TimingRecorder recorder(draft::TimingOutput::All, fake_now, &now);
  {
    draft::TimingScope native = recorder.scope("native artifact");
    now += 1'000'000;
    {
      draft::TimingScope clang = recorder.scope(
          "clang compile", draft::TimingVisibility::Detail);
      now += 4'000'000;
      recorder.record_child_process(2'000'000, 1'000'000);
    }
    now += 2'000'000;
  }

  const std::string report = recorder.render();
  expect_contains(
      report,
      "native artifact: 7.000 ms (self 3.000 ms)",
      "parent exclusive duration");
  expect_contains(
      report,
      "clang compile: 4.000 ms (self 4.000 ms)",
      "detail duration");
  expect_contains(report, "external processes: 1", "process count");
  expect_contains(report, "user: 2.000 ms", "child user CPU");
  expect_contains(report, "system: 1.000 ms", "child system CPU");
}

void test_disabled_recorder_is_a_no_op() {
  std::uint64_t now = 0;
  draft::TimingRecorder recorder(
      draft::TimingOutput::Disabled, fake_now, &now);
  {
    draft::TimingScope ignored = recorder.scope("ignored");
    now += 1;
    recorder.add_counter("ignored", 1);
    recorder.record_child_process(1, 1);
  }
  if (!recorder.render().empty()) {
    std::cerr << "disabled recorder produced a report\n";
    std::exit(1);
  }
}

} // namespace

int main() {
  test_summary_filters_detail_but_keeps_accounting();
  test_all_reports_exclusive_and_child_cpu_time();
  test_disabled_recorder_is_a_no_op();
  return 0;
}
