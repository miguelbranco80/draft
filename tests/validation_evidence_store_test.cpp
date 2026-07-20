// Persistent validation evidence history, revocation, and corruption tests.

#include "source/diagnostic.h"
#include "validation/evidence_store.h"

#include "test_directory.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "validation_evidence_store_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

[[nodiscard]] draft::ValidationEvidence evidence_attempt(bool passed) {
  draft::ValidationEvidence evidence;
  evidence.resolved_program = draft::sha256("store-program");
  evidence.kind = draft::ValidationKind::Test;
  evidence.target_identity = "aarch64-macos";
  evidence.compiler_identity = "compiler-v1";
  evidence.toolchain_identity = "toolchain-v1";
  evidence.environment_identity = "cpu=generic;features=+neon";
  evidence.runner_identity = "runner-v1";
  evidence.policy_identity = "policy-v1";
  evidence.instrumentation_identity =
      "draft-validation-instrumentation-v1:none";
  evidence.artifact_identity = "executable-v1";
  evidence.sample_runs = 1;
  draft::ValidationEntry entry;
  entry.kind = draft::ValidationKind::Test;
  entry.package = {"workspace", "app"};
  entry.procedure = "test_store";
  entry.state_size = 24;
  entry.state_alignment = 8;
  entry.failure_offset = 8;
  entry.report_size = 16;
  evidence.entries.push_back(entry);
  draft::ValidationObservation observation;
  observation.package = entry.package;
  observation.procedure = entry.procedure;
  observation.checks = 1;
  observation.failures = passed ? 0 : 1;
  evidence.observations.push_back(observation);
  evidence.observations_complete = true;
  evidence.passed = passed;
  evidence.exit_code = passed ? 0 : 1;
  evidence.key = draft::hash_validation_evidence_key(evidence);
  return evidence;
}

void test_history_and_revocation(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-validation-evidence-store-test"};
  const std::filesystem::path &root = temporary_directory.path();

  draft::ValidationEvidence passing = evidence_attempt(true);
  const draft::Sha256Digest key = passing.key;
  draft::DiagnosticSink diagnostics;
  const draft::ValidationEvidenceCommitResult first =
      draft::commit_validation_evidence(root, passing, diagnostics);
  EXPECT(state, first.ok);
  EXPECT(state, first.key == key);
  EXPECT(state, first.active);
  EXPECT(state, first.attempt == 1);
  EXPECT(state, std::filesystem::exists(first.evidence_path));

  draft::ValidationEvidenceState loaded;
  EXPECT(state, draft::load_validation_evidence_state(
      root, key, loaded, diagnostics));
  EXPECT(state,
      loaded.status == draft::ValidationEvidenceStateStatus::Active);
  EXPECT(state, loaded.attempts.size() == 1);
  EXPECT(state, loaded.active_evidence.has_value());
  if (loaded.active_evidence.has_value()) {
    EXPECT(state, loaded.active_evidence->attempt == 1);
    EXPECT(state, loaded.active_evidence->passed);
  }

  draft::ValidationEvidence failing = evidence_attempt(false);
  const draft::ValidationEvidenceCommitResult second =
      draft::commit_validation_evidence(root, failing, diagnostics);
  EXPECT(state, second.ok);
  EXPECT(state, second.key == key);
  EXPECT(state, !second.active);
  EXPECT(state, second.attempt == 2);
  EXPECT(state, draft::load_validation_evidence_state(
      root, key, loaded, diagnostics));
  EXPECT(state,
      loaded.status == draft::ValidationEvidenceStateStatus::Revoked);
  EXPECT(state, loaded.attempts.size() == 2);
  EXPECT(state, !loaded.active_evidence.has_value());

  const draft::ValidationEvidenceCommitResult third =
      draft::commit_validation_evidence(root, passing, diagnostics);
  EXPECT(state, third.ok);
  EXPECT(state, third.key == key);
  EXPECT(state, third.active);
  EXPECT(state, third.attempt == 3);
  EXPECT(state, draft::load_validation_evidence_state(
      root, key, loaded, diagnostics));
  EXPECT(state,
      loaded.status == draft::ValidationEvidenceStateStatus::Active);
  EXPECT(state, loaded.attempts.size() == 3);
  EXPECT(state, loaded.active_evidence.has_value());
  if (loaded.active_evidence.has_value()) {
    EXPECT(state, loaded.active_evidence->attempt == 3);
  }
  EXPECT(state, !diagnostics.has_errors());

  // An object is immutable and content-addressed. Corruption is detected both
  // by its filename digest and by the strict canonical evidence parser.
  std::ofstream corrupt(third.evidence_path, std::ios::binary | std::ios::trunc);
  corrupt << "corrupt\n";
  corrupt.close();
  draft::DiagnosticSink corrupt_diagnostics;
  EXPECT(state, !draft::load_validation_evidence_state(
      root, key, loaded, corrupt_diagnostics));
  EXPECT(state, corrupt_diagnostics.has_errors());

}

void test_missing_state(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-validation-evidence-missing-test"};
  const std::filesystem::path &root = temporary_directory.path();
  draft::ValidationEvidenceState loaded;
  draft::DiagnosticSink diagnostics;
  EXPECT(state, draft::load_validation_evidence_state(
      root, draft::sha256("missing-key"), loaded, diagnostics));
  EXPECT(state,
      loaded.status == draft::ValidationEvidenceStateStatus::Missing);
  EXPECT(state, !diagnostics.has_errors());
}

void test_symlink_store_is_rejected(TestState &state) {
#if !defined(_WIN32)
  draft::test::TemporaryDirectory root_directory{
      "draft-validation-evidence-symlink-test"};
  draft::test::TemporaryDirectory outside_directory{
      "draft-validation-evidence-symlink-outside"};
  const std::filesystem::path &root = root_directory.path();
  const std::filesystem::path &outside = outside_directory.path();
  std::error_code error;
  std::filesystem::create_directories(root / ".draft", error);
  EXPECT(state, !error);
  if (error) return;
  std::filesystem::create_directory_symlink(
      outside, root / ".draft" / "evidence", error);
  EXPECT(state, !error);
  if (!error) {
    draft::DiagnosticSink diagnostics;
    const draft::ValidationEvidenceCommitResult committed =
        draft::commit_validation_evidence(
            root, evidence_attempt(true), diagnostics);
    EXPECT(state, !committed.ok);
    EXPECT(state, diagnostics.has_errors());
    EXPECT(state, std::filesystem::is_empty(outside));
  }
#else
  (void)state;
#endif
}

} // namespace

int main() {
  TestState state;
  test_history_and_revocation(state);
  test_missing_state(state);
  test_symlink_store_is_rejected(state);
  return state.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
