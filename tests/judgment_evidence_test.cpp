// Judgment evidence identity and strict canonical JSON tests.

#include "judgment/evidence.h"

#include "base/sha256.h"
#include "source/diagnostic.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "judgment_evidence_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

[[nodiscard]] draft::JudgmentEvidence sample_evidence() {
  draft::JudgmentEvidence evidence;
  evidence.attempt = 2;
  evidence.resolved_program = draft::sha256("complete resolved program");
  evidence.target_identity = "draft-aarch64-macos-v6";
  evidence.compiler_identity = "draft-bootstrap-cpp-v100";
  evidence.policy_identity = "draft-judgment-policy-v1:all-pass";
  evidence.claim.site_identity = "site-" + draft::sha256("judgment site").hex();
  evidence.claim.root_identity = "workspace";
  evidence.claim.root_relative_path = "codec/jpeg";
  evidence.claim.source_relative_path = "decode.draft";
  evidence.claim.anchor_name = "decode";
  evidence.claim.occurrence = 1;
  evidence.claim.input_digest = draft::sha256("typed point facts");
  evidence.claim.record_digest = draft::sha256("claim and attachments");

  // Supply artifact rows in reverse order. Serialization and key construction
  // both treat this vector as a map and establish canonical kind order.
  evidence.artifacts.push_back({"object", draft::sha256("Mach-O object")});
  evidence.artifacts.push_back({"llvm-ir", draft::sha256("LLVM module")});

  draft::JudgmentValidatorResult first;
  first.validator_identity = "review-0";
  first.provider_identity = "codex-cli-v1";
  first.model_identity = "gpt-5.4";
  first.configuration_identity = "configuration-one";
  first.passed = true;
  first.rationale = "The checked branches preserve the invariant.\nEvidence: ø.";
  evidence.validators.push_back(first);

  draft::JudgmentValidatorResult second = first;
  second.validator_identity = "review-1";
  second.configuration_identity = "configuration-two";
  second.rationale = "An independent review reaches the same conclusion.";
  evidence.validators.push_back(second);
  evidence.passed = true;
  evidence.key = draft::hash_judgment_evidence_key(evidence);
  return evidence;
}

void test_round_trip_and_static_key(TestState &state) {
  const draft::JudgmentEvidence evidence = sample_evidence();
  const std::string encoded = draft::serialize_judgment_evidence(evidence);
  EXPECT(state, encoded.ends_with('\n'));
  EXPECT(state, encoded.find("llvm-ir") < encoded.find("object"));
  EXPECT(state, encoded.find("Evidence: ø.") != std::string::npos);

  draft::DiagnosticSink diagnostics;
  draft::JudgmentEvidence parsed;
  EXPECT(state,
      draft::parse_judgment_evidence(encoded, parsed, diagnostics));
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, parsed.key == evidence.key);
  EXPECT(state, parsed.attempt == 2);
  EXPECT(state, parsed.claim == evidence.claim);
  EXPECT(state, parsed.artifacts.size() == 2);
  EXPECT(state, parsed.validators == evidence.validators);
  EXPECT(state, parsed.passed);
  EXPECT(state, draft::serialize_judgment_evidence(parsed) == encoded);

  // Attempts and outcomes do not create new keys. A failing later invocation
  // must revoke this exact claim/environment key.
  draft::JudgmentEvidence failed = evidence;
  failed.attempt = 3;
  failed.validators[1].passed = false;
  failed.validators[1].rationale = "The second review found a counterexample.";
  failed.passed = false;
  EXPECT(state,
      draft::hash_judgment_evidence_key(failed) == evidence.key);

  draft::JudgmentEvidence changed = evidence;
  changed.claim.input_digest = draft::sha256("different typed point facts");
  EXPECT(state,
      draft::hash_judgment_evidence_key(changed) != evidence.key);
  changed = evidence;
  changed.validators[0].model_identity = "different-model";
  EXPECT(state,
      draft::hash_judgment_evidence_key(changed) != evidence.key);
}

void expect_rejected(
    TestState &state,
    std::string input) {
  draft::DiagnosticSink diagnostics;
  draft::JudgmentEvidence evidence;
  EXPECT(state,
      !draft::parse_judgment_evidence(input, evidence, diagnostics));
  EXPECT(state, diagnostics.error_count() == 1);
}

void test_malformed_and_inconsistent_inputs(TestState &state) {
  const draft::JudgmentEvidence evidence = sample_evidence();
  const std::string encoded = draft::serialize_judgment_evidence(evidence);

  std::string bad_key = encoded;
  const std::size_t key = bad_key.find(evidence.key.hex());
  EXPECT(state, key != std::string::npos);
  if (key != std::string::npos) bad_key[key] = bad_key[key] == '0' ? '1' : '0';
  expect_rejected(state, std::move(bad_key));

  std::string traversal = encoded;
  const std::size_t package = traversal.find("codec/jpeg");
  EXPECT(state, package != std::string::npos);
  if (package != std::string::npos) {
    traversal.replace(package, std::string_view("codec/jpeg").size(), "../jpeg");
  }
  expect_rejected(state, std::move(traversal));

  std::string false_aggregate = encoded;
  const std::string final_verdict = "\"verdict\": \"pass\"\n}";
  const std::size_t verdict = false_aggregate.rfind(final_verdict);
  EXPECT(state, verdict != std::string::npos);
  if (verdict != std::string::npos) {
    false_aggregate.replace(
        verdict,
        final_verdict.size(),
        "\"verdict\": \"fail\"\n}");
  }
  expect_rejected(state, std::move(false_aggregate));

  std::string duplicate_validator = encoded;
  const std::size_t second = duplicate_validator.find("review-1");
  EXPECT(state, second != std::string::npos);
  if (second != std::string::npos) {
    duplicate_validator.replace(
        second, std::string_view("review-1").size(), "review-0");
  }
  expect_rejected(state, std::move(duplicate_validator));

  std::string invalid_utf8 = encoded;
  const std::size_t rationale = invalid_utf8.find("An independent review");
  EXPECT(state, rationale != std::string::npos);
  if (rationale != std::string::npos) {
    invalid_utf8[rationale] = static_cast<char>(0xc0);
  }
  expect_rejected(state, std::move(invalid_utf8));
}

} // namespace

int main() {
  TestState state;
  test_round_trip_and_static_key(state);
  test_malformed_and_inconsistent_inputs(state);
  return state.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
