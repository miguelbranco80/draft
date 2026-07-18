// Validation evidence key, canonical JSON, and report decoding tests.

#include "source/diagnostic.h"
#include "validation/evidence.h"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "validation_evidence_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

void append_u64_little(
    std::vector<std::uint8_t> &bytes, std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
  }
}

[[nodiscard]] draft::ValidationEvidence sample_evidence() {
  draft::ValidationEvidence evidence;
  evidence.attempt = 3;
  evidence.resolved_program = draft::sha256("program");
  evidence.kind = draft::ValidationKind::Test;
  evidence.target_identity = "aarch64-macos";
  evidence.compiler_identity = "compiler-v1";
  evidence.toolchain_identity = "clang-v1\n";
  evidence.environment_identity = "cpu=generic;features=+neon";
  evidence.runner_identity = "runner-v1";
  evidence.policy_identity = "policy-v1";
  evidence.artifact_identity = "executable-v1";
  evidence.sample_runs = 1;
  draft::ValidationEntry entry;
  entry.kind = draft::ValidationKind::Test;
  entry.package = {"workspace", "app"};
  entry.procedure = "test_json_\"escape";
  entry.state_size = 24;
  entry.state_alignment = 8;
  entry.failure_offset = 8;
  entry.report_size = 16;
  evidence.entries.push_back(entry);
  draft::ValidationObservation observation;
  observation.package = entry.package;
  observation.procedure = entry.procedure;
  observation.checks = 4;
  evidence.observations.push_back(observation);
  evidence.observations_complete = true;
  evidence.passed = true;
  evidence.key = draft::hash_validation_evidence_key(evidence);
  return evidence;
}

void test_round_trip_and_key(TestState &state) {
  const draft::ValidationEvidence evidence = sample_evidence();
  const std::string json = draft::serialize_validation_evidence(evidence);
  draft::DiagnosticSink diagnostics;
  draft::ValidationEvidence parsed;
  EXPECT(state, draft::parse_validation_evidence(json, parsed, diagnostics));
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, parsed.key == evidence.key);
  EXPECT(state, parsed.attempt == 3);
  EXPECT(state, parsed.toolchain_identity == "clang-v1\n");
  EXPECT(state, parsed.entries == evidence.entries);
  EXPECT(state, parsed.observations == evidence.observations);
  EXPECT(state, draft::serialize_validation_evidence(parsed) == json);

  draft::ValidationEvidence changed = evidence;
  changed.entries[0].procedure = "test_changed";
  EXPECT(state, draft::hash_validation_evidence_key(changed) != evidence.key);

  std::string corrupt = json;
  const std::size_t procedure = corrupt.find("test_json_");
  EXPECT(state, procedure != std::string::npos);
  if (procedure != std::string::npos) corrupt[procedure] = 'b';
  draft::DiagnosticSink corrupt_diagnostics;
  EXPECT(state, !draft::parse_validation_evidence(
      corrupt, parsed, corrupt_diagnostics));
  EXPECT(state, corrupt_diagnostics.has_errors());

  // Parsed evidence is untrusted. A maximum offset used to wrap when the
  // invariant added the eight-byte failure field width.
  std::string overflowing = json;
  const std::string original_offset = "\"failure_offset\": 8";
  const std::size_t offset = overflowing.find(original_offset);
  EXPECT(state, offset != std::string::npos);
  if (offset != std::string::npos) {
    overflowing.replace(
        offset,
        original_offset.size(),
        "\"failure_offset\": " +
            std::to_string(std::numeric_limits<std::uint64_t>::max()));
  }
  draft::DiagnosticSink overflow_diagnostics;
  EXPECT(state, !draft::parse_validation_evidence(
      overflowing, parsed, overflow_diagnostics));
  EXPECT(state, overflow_diagnostics.has_errors());
}

void test_report_decoding(TestState &state) {
  draft::ValidationEntry test_entry;
  test_entry.kind = draft::ValidationKind::Test;
  test_entry.package = {"workspace", "app"};
  test_entry.procedure = "test_one";
  test_entry.state_size = 24;
  test_entry.state_alignment = 8;
  test_entry.failure_offset = 8;
  test_entry.report_size = 16;

  draft::ValidationEntry benchmark_entry;
  benchmark_entry.kind = draft::ValidationKind::Benchmark;
  benchmark_entry.package = {"workspace", "app"};
  benchmark_entry.procedure = "bench_one";
  benchmark_entry.state_size = 40;
  benchmark_entry.state_alignment = 8;
  benchmark_entry.failure_offset = 24;
  benchmark_entry.report_size = 32;

  std::vector<std::uint8_t> report;
  append_u64_little(report, 5);
  append_u64_little(report, 1);
  append_u64_little(report, 1000);
  append_u64_little(report, 42);
  append_u64_little(report, 2);
  append_u64_little(report, 0);
  std::vector<draft::ValidationObservation> observations;
  draft::DiagnosticSink diagnostics;
  EXPECT(state, draft::decode_validation_report(
      report,
      {test_entry, benchmark_entry},
      observations,
      diagnostics));
  EXPECT(state, observations.size() == 2);
  if (observations.size() == 2) {
    EXPECT(state, observations[0].checks == 5);
    EXPECT(state, observations[0].failures == 1);
    EXPECT(state, observations[1].maximum_time_ns == 1000);
    EXPECT(state, observations[1].durations_ns.size() == 1);
    EXPECT(state, observations[1].durations_ns[0] == 42);
    EXPECT(state, observations[1].library_samples == 2);
  }

  report.pop_back();
  draft::DiagnosticSink short_diagnostics;
  EXPECT(state, !draft::decode_validation_report(
      report,
      {test_entry, benchmark_entry},
      observations,
      short_diagnostics));
  EXPECT(state, short_diagnostics.has_errors());
}

} // namespace

int main() {
  TestState state;
  test_round_trip_and_key(state);
  test_report_decoding(state);
  return state.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
