// Provider-neutral judgment execution over a real checked Draft package.

#include "judgment/command.h"

#include "compile/compiler.h"
#include "judgment/evidence_store.h"
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
  options.provider.provider_identity = "fake-judge-v1";
  options.provider.model_identity = "deterministic-model-v1";
  options.provider.configuration_identity = "fake-configuration-v1";
  options.provider.state = &provider_state;
  options.provider.judge = fake_judge;
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
            "    judge \"The local value is well typed at this point.\"\n"
            "    _ = value\n"
            "}\n";
  source.close();
  EXPECT(state, source.good());

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::CompileWorkspaceResult compiled =
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
    EXPECT(state, provider.requests[0].format == "draft-judgment-request-v1");
    EXPECT(state, provider.requests[0].obligation.syntax == draft::SyntaxReference{});
    EXPECT(state, provider.requests[0].resolved_program ==
        *compiled.resolved_program_digest);
    EXPECT(state, provider.requests[0].claim ==
        "The package has one ordinary entry.");
    EXPECT(state, provider.requests[1].claim ==
        "The local value is well typed at this point.");
    EXPECT(state, provider.requests[1].obligation.visible_bindings.size() >= 1);
  }

  for (std::size_t index = 0; index < first.evidence.size(); ++index) {
    draft::JudgmentEvidenceState loaded;
    EXPECT(state, draft::load_judgment_evidence_state(
        root, first.evidence[index].key, loaded, diagnostics));
    EXPECT(state, loaded.attempts.size() == 1);
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
  std::filesystem::remove_all(root, error);
}

} // namespace

int main() {
  TestState state;
  test_execution_revocation_and_reactivation(state);
  return state.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
