// End-to-end public judgment-command coverage with a local Codex executable.
//
// The test crosses argument parsing, provider configuration, typed compilation,
// judgment execution, durable evidence history, and conditional manifest
// publication. No network service or release Codex installation is required.

#include "elaborator/resolution_store.h"
#include "compile/compiler.h"
#include "judgment/evidence_store.h"
#include "judgment/selection.h"
#include "judgment/verification.h"
#include "source/diagnostic.h"
#include "target/profile.h"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__unix__)
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "judgment_driver_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

struct TemporaryWorkspace {
  std::filesystem::path root;
  std::filesystem::path package;
  std::filesystem::path distribution;
  std::filesystem::path executable;

  TemporaryWorkspace(std::string_view name, bool passed) {
    std::error_code error;
    root = std::filesystem::temp_directory_path(error) /
        ("draft-judgment-driver-test-" + std::string(name));
    if (error) std::exit(EXIT_FAILURE);
    std::filesystem::remove_all(root, error);
    error.clear();
    package = root / "app";
    distribution = root / "codex-distribution";
    std::filesystem::create_directories(package, error);
    if (error) std::exit(EXIT_FAILURE);
    std::filesystem::create_directories(distribution, error);
    if (error) std::exit(EXIT_FAILURE);

    std::ofstream source(package / "package.draft", std::ios::binary);
    source << "package app\n\n"
              "judge \"The package remains coherent.\"\n\n"
              "main :: proc() {\n"
              "    value := 42\n"
              "    judge \"The typed local preserves the claim.\"\n"
              "    _ = value\n"
              "}\n";
    source.close();
    if (!source) std::exit(EXIT_FAILURE);

    executable = distribution / "fixture-codex";
    std::ofstream script(executable, std::ios::binary | std::ios::trunc);
    script <<
        "#!/bin/sh\n"
        "output=\n"
        "test \"$1\" = exec || exit 20\n"
        "shift\n"
        "while test \"$#\" -gt 0; do\n"
        "  case \"$1\" in\n"
        "    --ephemeral|--skip-git-repo-check|--ignore-user-config|--ignore-rules) shift ;;\n"
        "    --sandbox|--color|--model|--cd|--output-schema) shift 2 ;;\n"
        "    --output-last-message) output=$2; shift 2 ;;\n"
        "    -) shift ;;\n"
        "    *) exit 21 ;;\n"
        "  esac\n"
        "done\n"
        "test -n \"$output\" || exit 22\n";
    if (passed) {
      script <<
          "printf '%s' '{\"verdict\":\"pass\",\"rationale\":\"fixture pass\"}' > \"$output\"\n";
    } else {
      script <<
          "printf '%s' '{\"verdict\":\"fail\",\"rationale\":\"fixture fail\"}' > \"$output\"\n";
    }
    script.close();
    if (!script) std::exit(EXIT_FAILURE);
#if defined(__APPLE__) || defined(__unix__)
    if (::chmod(executable.c_str(), 0700) != 0) std::exit(EXIT_FAILURE);
#endif
  }

  ~TemporaryWorkspace() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
};

[[nodiscard]] int run_driver(std::vector<std::string> arguments) {
#if defined(__APPLE__) || defined(__unix__)
  std::vector<char *> raw;
  raw.reserve(arguments.size() + 1);
  for (std::string &argument : arguments) raw.push_back(argument.data());
  raw.push_back(nullptr);

  const pid_t child = ::fork();
  if (child < 0) return -1;
  if (child == 0) {
    ::execv(raw.front(), raw.data());
    ::_exit(127);
  }
  int status = 0;
  pid_t waited = -1;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != child || !WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
#else
  (void)arguments;
  return -1;
#endif
}

void append_codex_arguments(
    const TemporaryWorkspace &workspace,
    std::vector<std::string> &arguments) {
  arguments.insert(
      arguments.end(),
      {
          "--codex-distribution-root",
          workspace.distribution.string(),
          "--codex-executable",
          workspace.executable.string(),
          "--codex-model",
          "fixture-model",
      });
}

[[nodiscard]] int run_judge(
    const TemporaryWorkspace &workspace,
    const std::vector<std::string> &selectors = {},
    bool list = false) {
  std::vector<std::string> arguments{
      DRAFT_DRIVER_PATH,
      "judge",
      workspace.package.string(),
  };
  for (const std::string &selector : selectors) {
    arguments.push_back(selector);
  }
  if (list) {
    arguments.push_back("--list");
  } else {
    append_codex_arguments(workspace, arguments);
  }
  return run_driver(std::move(arguments));
}

[[nodiscard]] int run_resolve(
    const TemporaryWorkspace &workspace,
    bool judge,
    const std::vector<std::string> &selectors = {}) {
  std::vector<std::string> arguments{
      DRAFT_DRIVER_PATH,
      "resolve",
      workspace.package.string(),
  };
  if (judge) {
    if (selectors.empty()) {
      arguments.push_back("--judge");
    } else {
      for (const std::string &selector : selectors) {
        arguments.push_back("--judge-select");
        arguments.push_back(selector);
      }
    }
    append_codex_arguments(workspace, arguments);
  }
  return run_driver(std::move(arguments));
}

[[nodiscard]] draft::CompileWorkspaceResult compile_resolved(
    const TemporaryWorkspace &workspace,
    draft::SourceManager &sources,
    draft::DiagnosticSink &diagnostics) {
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = workspace.root.string();
  options.workspace.core_directory = DRAFT_CORE_DIRECTORY;
  options.workspace.core_content_identity = DRAFT_CORE_CONTENT_IDENTITY;
  return draft::compile_workspace_with_resolution(
      sources, workspace.package.string(), std::move(options), diagnostics);
}

void test_passing_command_selects_evidence(TestState &state) {
  TemporaryWorkspace workspace("pass", true);
  EXPECT(state, run_judge(workspace, {}, true) == 0);

  draft::DiagnosticSink diagnostics;
  draft::ResolutionManifestLoadResult loaded =
      draft::load_resolution_manifest(workspace.root, diagnostics);
  EXPECT(state,
      loaded.state == draft::ResolutionManifestLoadState::Missing);

  draft::SourceManager discovery_sources;
  draft::DiagnosticSink discovery_diagnostics;
  const draft::CompileWorkspaceResult discovered_program = compile_resolved(
      workspace, discovery_sources, discovery_diagnostics);
  EXPECT(state, discovered_program.ok);
  const std::vector<draft::JudgmentSiteDescription> sites =
      draft::discover_judgment_sites(discovered_program);
  EXPECT(state, sites.size() == 2);
  if (sites.size() != 2) return;

  // Exact stable identity selects one site and publishes only its row. Listing
  // and selection both operate before provider configuration.
  EXPECT(state, run_judge(workspace, {sites.front().site_identity}) == 0);
  diagnostics = {};
  loaded = draft::load_resolution_manifest(workspace.root, diagnostics);
  EXPECT(state,
      loaded.state == draft::ResolutionManifestLoadState::Loaded);
  EXPECT(state, loaded.manifest.pins.empty());
  EXPECT(state, loaded.manifest.evidence.size() == 1);
  for (const draft::ResolutionEvidencePin &pin : loaded.manifest.evidence) {
    EXPECT(state, pin.kind == "judgment");
    draft::JudgmentEvidenceState evidence;
    EXPECT(state,
        draft::load_judgment_evidence_state(
            workspace.root, pin.key, evidence, diagnostics));
    EXPECT(state,
        evidence.status == draft::JudgmentEvidenceStateStatus::Active);
    EXPECT(state, evidence.active_digest == pin.content_digest);
    EXPECT(state, evidence.active_evidence.has_value());
  }
  EXPECT(state, !diagnostics.has_errors());

  draft::SourceManager partial_sources;
  draft::DiagnosticSink partial_diagnostics;
  const draft::CompileWorkspaceResult partial = compile_resolved(
      workspace, partial_sources, partial_diagnostics);
  std::vector<draft::Sha256Digest> active;
  EXPECT(state,
      !draft::verify_active_judgment_evidence(
          partial, workspace.root, active, partial_diagnostics));
  EXPECT(state, partial_diagnostics.has_errors());

  // The default package command selects all sites. It preserves the existing
  // selected row until its fresh attempt is durable, adds the missing row, and
  // atomically publishes the complete two-site selection.
  EXPECT(state, run_judge(workspace) == 0);
  diagnostics = {};
  loaded = draft::load_resolution_manifest(workspace.root, diagnostics);
  EXPECT(state,
      loaded.state == draft::ResolutionManifestLoadState::Loaded);
  EXPECT(state, loaded.manifest.evidence.size() == 2);
  for (const draft::ResolutionEvidencePin &pin : loaded.manifest.evidence) {
    draft::JudgmentEvidenceState evidence;
    EXPECT(state,
        draft::load_judgment_evidence_state(
            workspace.root, pin.key, evidence, diagnostics));
    EXPECT(state, evidence.active_evidence.has_value());
    if (evidence.active_evidence.has_value() &&
        evidence.active_evidence->claim.site_identity ==
            sites.front().site_identity) {
      EXPECT(state, evidence.attempts.size() == 2);
    } else {
      EXPECT(state, evidence.attempts.size() == 1);
    }
    EXPECT(state, evidence.active_digest == pin.content_digest);
  }
  EXPECT(state, !diagnostics.has_errors());

  // Locked verification consumes only the compiled obligations, selected
  // manifest rows, and typed local evidence store. It has no provider object.
  draft::SourceManager sources;
  draft::DiagnosticSink verification_diagnostics;
  const draft::CompileWorkspaceResult compiled = compile_resolved(
      workspace, sources, verification_diagnostics);
  EXPECT(state, compiled.ok);
  active.clear();
  EXPECT(state,
      draft::verify_active_judgment_evidence(
          compiled,
          workspace.root,
          active,
          verification_diagnostics));
  EXPECT(state, active.size() == 2);
  EXPECT(state, !verification_diagnostics.has_errors());

  // A new failing attempt revokes one exact selected key. The manifest bytes
  // remain unchanged, but an offline locked verifier must reject that stale
  // selection without invoking the provider that produced either attempt.
  if (!loaded.manifest.evidence.empty()) {
    draft::JudgmentEvidenceState evidence;
    EXPECT(state,
        draft::load_judgment_evidence_state(
            workspace.root,
            loaded.manifest.evidence.front().key,
            evidence,
            verification_diagnostics));
    if (evidence.active_evidence.has_value()) {
      draft::JudgmentEvidence failed = *evidence.active_evidence;
      failed.passed = false;
      failed.validators.front().passed = false;
      failed.validators.front().rationale = "fixture revocation";
      const draft::JudgmentEvidenceCommitResult revoked =
          draft::commit_judgment_evidence(
              workspace.root, std::move(failed), verification_diagnostics);
      EXPECT(state, revoked.ok);
      EXPECT(state, !revoked.active);
    }
  }
  draft::DiagnosticSink revoked_diagnostics;
  active.clear();
  EXPECT(state,
      !draft::verify_active_judgment_evidence(
          compiled, workspace.root, active, revoked_diagnostics));
  EXPECT(state, revoked_diagnostics.has_errors());
}

void test_failing_command_leaves_manifest_missing(TestState &state) {
  TemporaryWorkspace workspace("fail", false);
  EXPECT(state, run_judge(workspace) == 1);
  draft::DiagnosticSink diagnostics;
  const draft::ResolutionManifestLoadResult loaded =
      draft::load_resolution_manifest(workspace.root, diagnostics);
  EXPECT(state,
      loaded.state == draft::ResolutionManifestLoadState::Missing);
  EXPECT(state, !diagnostics.has_errors());
}

void test_resolution_profile_commits_judgments_atomically(TestState &state) {
  TemporaryWorkspace workspace("resolve-pass", true);

  draft::SourceManager discovery_sources;
  draft::DiagnosticSink discovery_diagnostics;
  const draft::CompileWorkspaceResult discovered = compile_resolved(
      workspace, discovery_sources, discovery_diagnostics);
  EXPECT(state, discovered.ok);
  const std::vector<draft::JudgmentSiteDescription> sites =
      draft::discover_judgment_sites(discovered);
  EXPECT(state, sites.size() == 2);
  if (sites.size() != 2) return;

  // A selected resolution profile can publish a deliberately partial evidence
  // set. A later complete profile replaces it with one row for every judgment,
  // all in the resolver's single candidate-manifest transaction.
  EXPECT(state,
      run_resolve(workspace, true, {sites.front().site_identity}) == 0);
  draft::DiagnosticSink diagnostics;
  draft::ResolutionManifestLoadResult loaded =
      draft::load_resolution_manifest(workspace.root, diagnostics);
  EXPECT(state,
      loaded.state == draft::ResolutionManifestLoadState::Loaded);
  EXPECT(state, loaded.manifest.pins.empty());
  EXPECT(state, loaded.manifest.evidence.size() == 1);

  EXPECT(state, run_resolve(workspace, true) == 0);
  diagnostics = {};
  loaded = draft::load_resolution_manifest(workspace.root, diagnostics);
  EXPECT(state,
      loaded.state == draft::ResolutionManifestLoadState::Loaded);
  EXPECT(state, loaded.manifest.evidence.size() == 2);

  draft::SourceManager sources;
  draft::DiagnosticSink verification_diagnostics;
  const draft::CompileWorkspaceResult compiled = compile_resolved(
      workspace, sources, verification_diagnostics);
  std::vector<draft::Sha256Digest> active;
  EXPECT(state, compiled.ok);
  EXPECT(state,
      draft::verify_active_judgment_evidence(
          compiled,
          workspace.root,
          active,
          verification_diagnostics));
  EXPECT(state, active.size() == 2);

  // Ordinary resolution keeps expensive judgment rows when the complete
  // resolved-program digest is unchanged.
  EXPECT(state, run_resolve(workspace, false) == 0);
  diagnostics = {};
  loaded = draft::load_resolution_manifest(workspace.root, diagnostics);
  EXPECT(state, loaded.manifest.evidence.size() == 2);

  // A source edit changes that digest. Provider-free resolution must drop the
  // stale rows instead of carrying qualitative claims onto another program.
  std::ofstream changed(
      workspace.package / "package.draft",
      std::ios::binary | std::ios::trunc);
  changed << "package app\n\n"
             "judge \"The package remains coherent.\"\n\n"
             "main :: proc() {\n"
             "    value := 43\n"
             "    judge \"The typed local preserves the claim.\"\n"
             "    _ = value\n"
             "}\n";
  changed.close();
  EXPECT(state, static_cast<bool>(changed));
  EXPECT(state, run_resolve(workspace, false) == 0);
  diagnostics = {};
  loaded = draft::load_resolution_manifest(workspace.root, diagnostics);
  EXPECT(state,
      loaded.state == draft::ResolutionManifestLoadState::Loaded);
  EXPECT(state, loaded.manifest.evidence.empty());
  EXPECT(state, !diagnostics.has_errors());
}

void test_failing_resolution_profile_leaves_manifest_missing(
    TestState &state) {
  TemporaryWorkspace workspace("resolve-fail", false);
  EXPECT(state, run_resolve(workspace, true) == 1);
  draft::DiagnosticSink diagnostics;
  const draft::ResolutionManifestLoadResult loaded =
      draft::load_resolution_manifest(workspace.root, diagnostics);
  EXPECT(state,
      loaded.state == draft::ResolutionManifestLoadState::Missing);
  EXPECT(state, !diagnostics.has_errors());
}

} // namespace

int main() {
  TestState state;
  test_passing_command_selects_evidence(state);
  test_failing_command_leaves_manifest_missing(state);
  test_resolution_profile_commits_judgments_atomically(state);
  test_failing_resolution_profile_leaves_manifest_missing(state);
  if (state.failures != 0) {
    std::cerr << state.failures
              << " judgment driver expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all judgment driver tests passed\n";
  return EXIT_SUCCESS;
}
