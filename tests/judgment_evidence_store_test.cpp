// Persistent judgment history, revocation, and typed corruption tests.

#include "judgment/evidence_store.h"

#include "base/sha256.h"
#include "source/diagnostic.h"

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
      std::cerr << "judgment_evidence_store_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

[[nodiscard]] draft::JudgmentEvidence evidence_attempt(bool passed) {
  draft::JudgmentEvidence evidence;
  evidence.resolved_program = draft::sha256("judged program");
  evidence.target_identity = "draft-aarch64-macos-v5";
  evidence.compiler_identity = "compiler-v1";
  evidence.policy_identity = "single-validator-all-pass-v1";
  evidence.claim.site_identity = "site-" + draft::sha256("store site").hex();
  evidence.claim.root_identity = "workspace";
  evidence.claim.root_relative_path = "app";
  evidence.claim.source_relative_path = "package.draft";
  evidence.claim.anchor_name = "run";
  evidence.claim.input_digest = draft::sha256("typed judgment context");
  evidence.claim.record_digest = draft::sha256("claim");
  draft::JudgmentValidatorResult validator;
  validator.validator_identity = "validator-0";
  validator.provider_identity = "provider-v1";
  validator.model_identity = "model-v1";
  validator.configuration_identity = "configuration-v1";
  validator.passed = passed;
  validator.rationale = passed
      ? "The reviewed invariant holds."
      : "A reviewed path violates the invariant.";
  evidence.validators.push_back(std::move(validator));
  evidence.passed = passed;
  evidence.key = draft::hash_judgment_evidence_key(evidence);
  return evidence;
}

void test_history_and_revocation(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-judgment-evidence-store-test"};
  const std::filesystem::path &root = temporary_directory.path();

  draft::JudgmentEvidence passing = evidence_attempt(true);
  const draft::Sha256Digest key = passing.key;
  draft::DiagnosticSink diagnostics;
  const draft::JudgmentEvidenceCommitResult first =
      draft::commit_judgment_evidence(root, passing, diagnostics);
  EXPECT(state, first.ok);
  EXPECT(state, first.key == key);
  EXPECT(state, first.active);
  EXPECT(state, first.attempt == 1);
  EXPECT(state, std::filesystem::exists(first.evidence_path));

  draft::JudgmentEvidenceState loaded;
  EXPECT(state, draft::load_judgment_evidence_state(
      root, key, loaded, diagnostics));
  EXPECT(state, loaded.status == draft::JudgmentEvidenceStateStatus::Active);
  EXPECT(state, loaded.attempts.size() == 1);
  EXPECT(state, loaded.active_evidence.has_value());
  if (loaded.active_evidence.has_value()) {
    EXPECT(state, loaded.active_evidence->attempt == 1);
    EXPECT(state, loaded.active_evidence->passed);
  }

  const draft::JudgmentEvidenceCommitResult second =
      draft::commit_judgment_evidence(
          root, evidence_attempt(false), diagnostics);
  EXPECT(state, second.ok);
  EXPECT(state, second.key == key);
  EXPECT(state, !second.active);
  EXPECT(state, second.attempt == 2);
  EXPECT(state, draft::load_judgment_evidence_state(
      root, key, loaded, diagnostics));
  EXPECT(state, loaded.status == draft::JudgmentEvidenceStateStatus::Revoked);
  EXPECT(state, loaded.attempts.size() == 2);
  EXPECT(state, !loaded.active_evidence.has_value());

  const draft::JudgmentEvidenceCommitResult third =
      draft::commit_judgment_evidence(root, passing, diagnostics);
  EXPECT(state, third.ok);
  EXPECT(state, third.active);
  EXPECT(state, third.attempt == 3);
  EXPECT(state, draft::load_judgment_evidence_state(
      root, key, loaded, diagnostics));
  EXPECT(state, loaded.status == draft::JudgmentEvidenceStateStatus::Active);
  EXPECT(state, loaded.active_evidence.has_value());
  if (loaded.active_evidence.has_value()) {
    EXPECT(state, loaded.active_evidence->attempt == 3);
  }
  EXPECT(state, !diagnostics.has_errors());

  // The shared store verifies the content address before the judgment codec
  // sees the bytes. Neither a forged rationale nor arbitrary JSON can replace
  // a previously committed immutable attempt.
  std::ofstream corrupt(third.evidence_path, std::ios::binary | std::ios::trunc);
  corrupt << "{}\n";
  corrupt.close();
  draft::DiagnosticSink corrupt_diagnostics;
  EXPECT(state, !draft::load_judgment_evidence_state(
      root, key, loaded, corrupt_diagnostics));
  EXPECT(state, corrupt_diagnostics.has_errors());

}

void test_missing_state(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-judgment-evidence-missing-test"};
  const std::filesystem::path &root = temporary_directory.path();
  draft::JudgmentEvidenceState loaded;
  draft::DiagnosticSink diagnostics;
  EXPECT(state, draft::load_judgment_evidence_state(
      root, draft::sha256("missing judgment key"), loaded, diagnostics));
  EXPECT(state, loaded.status == draft::JudgmentEvidenceStateStatus::Missing);
  EXPECT(state, !diagnostics.has_errors());
}

} // namespace

int main() {
  TestState state;
  test_history_and_revocation(state);
  test_missing_state(state);
  return state.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
