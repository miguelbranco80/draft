// Provider-neutral judgment execution over a real checked Draft package.

#include "judgment/command.h"

#include "compile/compiler.h"
#include "judgment/evidence_store.h"
#include "judgment/selection.h"
#include "judgment/verification.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "judgment_command_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

struct FakeProviderState {
  std::size_t calls = 0;
  std::size_t failing_call = 1;
  std::vector<draft::JudgmentRequest> requests;
};

bool fake_judge(
    void *opaque,
    const draft::JudgmentRequest &request,
    draft::JudgmentResponse &response,
    draft::DiagnosticSink &) {
  auto *state = static_cast<FakeProviderState *>(opaque);
  state->requests.push_back(request);
  response.passed = state->calls != state->failing_call;
  response.rationale = response.passed
      ? "The supplied typed program facts support the claim."
      : "The supplied typed program facts contain a counterexample.";
  ++state->calls;
  return true;
}

[[nodiscard]] draft::CompileWorkspaceResult compile_fixture(
    const std::filesystem::path &root,
    draft::SourceManager &sources,
    draft::DiagnosticSink &diagnostics) {
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = root.string();
  return draft::compile_workspace_with_resolution(
      sources,
      (root / "app").string(),
      std::move(options),
      diagnostics);
}

[[nodiscard]] draft::JudgmentCommandOptions command_options(
    const std::filesystem::path &root,
    FakeProviderState &provider_state) {
  draft::JudgmentCommandOptions options;
  options.workspace_directory = root;
  options.target = draft::make_aarch64_macos_profile();
  draft::JudgmentProvider provider;
  provider.provider_identity = "fake-judge-v1";
  provider.model_identity = "deterministic-model-v1";
  provider.configuration_identity = "fake-configuration-v1";
  provider.state = &provider_state;
  provider.judge = fake_judge;
  options.validators.push_back({"validator-0", std::move(provider)});
  return options;
}

void test_execution_revocation_and_reactivation(TestState &state) {
  std::error_code error;
  const std::filesystem::path root =
      std::filesystem::temp_directory_path(error) /
      "draft-judgment-command-test";
  EXPECT(state, !error);
  std::filesystem::remove_all(root, error);
  error.clear();
  std::filesystem::create_directories(root / "app", error);
  EXPECT(state, !error);
  if (error) return;

  std::ofstream source(root / "app" / "package.draft", std::ios::binary);
  source << "package app\n\n"
            "judge \"The package has one ordinary entry.\"\n\n"
            "main :: proc() {\n"
            "    value := 42\n"
            "    if value == 42 {\n"
            "        judge \"The local value is well typed at this point.\"\n"
            "    }\n"
            "    _ = value\n"
            "}\n";
  source.close();
  EXPECT(state, source.good());

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceResult compiled =
      compile_fixture(root, sources, diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, compiled.ok);
  EXPECT(state, compiled.resolved_program_digest.has_value());
  EXPECT(state, !diagnostics.has_errors());
  if (!compiled.ok) {
    std::filesystem::remove_all(root, error);
    return;
  }

  FakeProviderState provider;
  const draft::JudgmentCommandResult first =
      draft::execute_judgment_command(
          compiled, command_options(root, provider), diagnostics);
  EXPECT(state, first.completed);
  EXPECT(state, !first.passed);
  EXPECT(state, first.selected_judgments == 2);
  EXPECT(state, first.evidence.size() == 2);
  EXPECT(state, provider.calls == 2);
  EXPECT(state, provider.requests.size() == 2);
  if (provider.requests.size() == 2) {
    EXPECT(state, provider.requests[0].format == "draft-judgment-request-v3");
    EXPECT(state, provider.requests[0].obligation.syntax == draft::SyntaxReference{});
    EXPECT(state, provider.requests[0].resolved_program ==
        *compiled.resolved_program_digest);
    EXPECT(state, provider.requests[0].claim ==
        "The package has one ordinary entry.");
    EXPECT(state, provider.requests[1].claim ==
        "The local value is well typed at this point.");
    EXPECT(state, provider.requests[1].obligation.visible_bindings.size() >= 1);
    EXPECT(state,
        provider.requests[1].obligation.branch_refinements.size() == 1);
    if (provider.requests[1].obligation.branch_refinements.size() == 1) {
      const draft::AgentBranchRefinement &refinement =
          provider.requests[1].obligation.branch_refinements.front();
      EXPECT(state,
          refinement.kind ==
              draft::AgentBranchRefinementKind::ConditionTrue);
      EXPECT(state, refinement.subject == "value == 42");
      EXPECT(state, refinement.type_text == "bool");
    }
  }

  for (std::size_t index = 0; index < first.evidence.size(); ++index) {
    draft::JudgmentEvidenceState loaded;
    EXPECT(state, draft::load_judgment_evidence_state(
        root, first.evidence[index].key, loaded, diagnostics));
    EXPECT(state, loaded.attempts.size() == 1);
    EXPECT(state, loaded.latest_evidence.has_value());
    EXPECT(state, loaded.status ==
        (index == 0
            ? draft::JudgmentEvidenceStateStatus::Active
            : draft::JudgmentEvidenceStateStatus::Revoked));
  }

  provider.calls = 0;
  provider.failing_call = 99;
  provider.requests.clear();
  const draft::JudgmentCommandResult second =
      draft::execute_judgment_command(
          compiled, command_options(root, provider), diagnostics);
  EXPECT(state, second.completed);
  EXPECT(state, second.passed);
  EXPECT(state, second.evidence.size() == 2);
  if (first.evidence.size() == 2 && second.evidence.size() == 2) {
    EXPECT(state, first.evidence[0].key == second.evidence[0].key);
    EXPECT(state, first.evidence[1].key == second.evidence[1].key);
  }
  for (const draft::ResolutionEvidencePin &pin : second.evidence) {
    draft::JudgmentEvidenceState loaded;
    EXPECT(state, draft::load_judgment_evidence_state(
        root, pin.key, loaded, diagnostics));
    EXPECT(state, loaded.status == draft::JudgmentEvidenceStateStatus::Active);
    EXPECT(state, loaded.attempts.size() == 2);
    EXPECT(state, loaded.active_digest == pin.content_digest);
    EXPECT(state, loaded.active_evidence.has_value());
    if (loaded.active_evidence.has_value()) {
      EXPECT(state, loaded.active_evidence->attempt == 2);
      EXPECT(state, loaded.active_evidence->validators.size() == 1);
    }
  }
  EXPECT(state, !diagnostics.has_errors());

  const std::vector<draft::JudgmentSiteDescription> sites =
      draft::discover_judgment_sites(compiled);
  EXPECT(state, sites.size() == 2);

  // The provider-neutral command executes every validator for every selected
  // site, aggregates only after all typed verdicts arrive, and commits one
  // evidence object containing the exact requested artifact identities.
  FakeProviderState primary_validator;
  primary_validator.failing_call = 99;
  FakeProviderState secondary_validator;
  secondary_validator.failing_call = 99;
  draft::JudgmentCommandOptions multiple =
      command_options(root, primary_validator);
  multiple.policy_identity =
      "draft-judgment-policy-v2:validators=2:aggregate=all-pass:artifacts=object";
  multiple.validators.front().identity = "review-primary";
  draft::JudgmentProvider secondary = multiple.validators.front().provider;
  secondary.provider_identity = "independent-fake-judge-v1";
  secondary.configuration_identity = "independent-configuration-v1";
  secondary.state = &secondary_validator;
  multiple.validators.push_back(
      {"review-secondary", std::move(secondary)});
  draft::JudgmentRequestArtifact artifact;
  artifact.kind = "object";
  artifact.contents = "exact object bytes";
  artifact.digest = draft::sha256(artifact.contents);
  multiple.artifacts.push_back(artifact);
  const draft::JudgmentCommandResult multiple_result =
      draft::execute_judgment_command(
          compiled, std::move(multiple), diagnostics);
  EXPECT(state, multiple_result.completed);
  EXPECT(state, multiple_result.passed);
  EXPECT(state, multiple_result.evidence.size() == 2);
  EXPECT(state, primary_validator.calls == 2);
  EXPECT(state, secondary_validator.calls == 2);
  if (!primary_validator.requests.empty() &&
      !secondary_validator.requests.empty()) {
    EXPECT(state,
        primary_validator.requests.front().validator_identity ==
            "review-primary");
    EXPECT(state,
        secondary_validator.requests.front().validator_identity ==
            "review-secondary");
    EXPECT(state,
        primary_validator.requests.front().artifacts.size() == 1);
    EXPECT(state,
        primary_validator.requests.front().artifacts.front().contents ==
            artifact.contents);
  }
  for (const draft::ResolutionEvidencePin &pin : multiple_result.evidence) {
    draft::JudgmentEvidenceState loaded;
    EXPECT(state, draft::load_judgment_evidence_state(
        root, pin.key, loaded, diagnostics));
    EXPECT(state, loaded.active_evidence.has_value());
    if (loaded.active_evidence.has_value()) {
      EXPECT(state, loaded.active_evidence->validators.size() == 2);
      EXPECT(state, loaded.active_evidence->artifacts.size() == 1);
      EXPECT(state, loaded.active_evidence->passed);
    }
  }

  // Offline verification consumes an explicit policy shape. It needs neither
  // provider and proves validator order plus exact artifact content identity.
  draft::ResolutionManifest policy_manifest;
  policy_manifest.target_identity =
      draft::make_aarch64_macos_profile().facts.identity;
  policy_manifest.resolved_program_digest =
      *compiled.resolved_program_digest;
  policy_manifest.evidence = multiple_result.evidence;
  compiled.resolution_manifest = std::move(policy_manifest);
  draft::JudgmentVerificationPolicy verification_policy;
  verification_policy.identity =
      "draft-judgment-policy-v2:validators=2:aggregate=all-pass:artifacts=object";
  verification_policy.validator_identities = {
      "review-primary", "review-secondary"};
  verification_policy.artifacts.push_back(
      {artifact.kind, artifact.digest});
  std::vector<draft::Sha256Digest> active_evidence;
  EXPECT(state, draft::verify_active_judgment_evidence(
      compiled,
      root,
      active_evidence,
      diagnostics,
      verification_policy));
  EXPECT(state, active_evidence.size() == 2);
  draft::DiagnosticSink wrong_policy_diagnostics;
  active_evidence.clear();
  EXPECT(state, !draft::verify_active_judgment_evidence(
      compiled,
      root,
      active_evidence,
      wrong_policy_diagnostics));
  EXPECT(state, wrong_policy_diagnostics.has_errors());

  if (sites.size() == 2) {
    provider.calls = 0;
    provider.requests.clear();
    draft::JudgmentCommandOptions selected = command_options(root, provider);
    selected.selectors.push_back(
        sites[1].package_selector + ":" + sites[1].anchor_name);
    const draft::JudgmentCommandResult declaration =
        draft::execute_judgment_command(
            compiled, std::move(selected), diagnostics);
    EXPECT(state, declaration.completed);
    EXPECT(state, declaration.passed);
    EXPECT(state, declaration.selected_judgments == 1);
    EXPECT(state, declaration.selected_site_identities.size() == 1);
    EXPECT(state,
        declaration.selected_site_identities.front() ==
            sites[1].site_identity);
    EXPECT(state, provider.requests.size() == 1);
    if (provider.requests.size() == 1) {
      EXPECT(state, provider.requests.front().claim ==
          "The local value is well typed at this point.");
    }

    provider.calls = 0;
    provider.requests.clear();
    draft::JudgmentCommandOptions exact = command_options(root, provider);
    exact.selectors.push_back(sites[0].site_identity);
    const draft::JudgmentCommandResult one =
        draft::execute_judgment_command(
            compiled, std::move(exact), diagnostics);
    EXPECT(state, one.completed);
    EXPECT(state, one.selected_judgments == 1);
    EXPECT(state, provider.requests.size() == 1);
  }

  draft::DiagnosticSink unmatched_diagnostics;
  provider.calls = 0;
  provider.requests.clear();
  draft::JudgmentCommandOptions unmatched = command_options(root, provider);
  unmatched.selectors.push_back("no/such/package:missing");
  const draft::JudgmentCommandResult no_match =
      draft::execute_judgment_command(
          compiled, std::move(unmatched), unmatched_diagnostics);
  EXPECT(state, !no_match.completed);
  EXPECT(state, unmatched_diagnostics.has_errors());
  EXPECT(state, provider.calls == 0);
  std::filesystem::remove_all(root, error);
}

} // namespace

int main() {
  TestState state;
  test_execution_revocation_and_reactivation(state);
  return state.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
