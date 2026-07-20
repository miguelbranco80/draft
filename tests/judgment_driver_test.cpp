// End-to-end public judgment-command coverage with a local Codex executable.
//
// The test crosses argument parsing, provider configuration, typed compilation,
// judgment execution, and durable evidence history. Judgment evidence is
// deliberately independent from source resolution: running this command must
// never create or modify its root/target resolution manifest. No network service or
// release Codex installation is required.

#include "elaborator/resolution_store.h"
#include "compile/compiler.h"
#include "judgment/evidence_store.h"
#include "judgment/selection.h"
#include "source/diagnostic.h"
#include "target/profile.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
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
  std::filesystem::path codex_directory;
  std::filesystem::path executable;
  std::filesystem::path artifact;

  TemporaryWorkspace(std::string_view name, bool passed) {
    std::error_code error;
    root = std::filesystem::temp_directory_path(error) /
        ("draft-judgment-driver-test-" + std::string(name));
    if (error) std::exit(EXIT_FAILURE);
    std::filesystem::remove_all(root, error);
    error.clear();
    package = root / "app";
    codex_directory = root / "codex-bin";
    std::filesystem::create_directories(package, error);
    if (error) std::exit(EXIT_FAILURE);
    std::filesystem::create_directories(codex_directory, error);
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

    artifact = root / "review-object.bin";
    std::ofstream artifact_stream(
        artifact, std::ios::binary | std::ios::trunc);
    artifact_stream << "public judgment artifact bytes";
    artifact_stream.close();
    if (!artifact_stream) std::exit(EXIT_FAILURE);

    executable = codex_directory / "codex";
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

[[nodiscard]] int run_driver(
    std::vector<std::string> arguments,
    const std::filesystem::path &codex_directory = {}) {
#if defined(__APPLE__) || defined(__unix__)
  std::vector<char *> raw;
  raw.reserve(arguments.size() + 1);
  for (std::string &argument : arguments) raw.push_back(argument.data());
  raw.push_back(nullptr);

  const pid_t child = ::fork();
  if (child < 0) return -1;
  if (child == 0) {
    if (!codex_directory.empty()) {
      std::string path = codex_directory.string();
      if (const char *existing = std::getenv("PATH");
          existing != nullptr && *existing != '\0') {
        path += ':';
        path += existing;
      }
      if (::setenv("PATH", path.c_str(), 1) != 0) ::_exit(126);
    }
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

void append_codex_arguments(std::vector<std::string> &arguments) {
  arguments.insert(
      arguments.end(),
      {
          "--model",
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
      workspace.root.string(),
      "--root",
      "app",
  };
  for (const std::string &selector : selectors) {
    arguments.push_back(selector);
  }
  if (list) {
    arguments.push_back("--list");
  } else {
    append_codex_arguments(arguments);
  }
  return run_driver(std::move(arguments), workspace.codex_directory);
}

[[nodiscard]] int run_multi_judge(
    const TemporaryWorkspace &workspace) {
  return run_driver({
      DRAFT_DRIVER_PATH,
      "judge",
      workspace.root.string(),
      "--root",
      "app",
      "--judge-validator",
      "review-primary:fixture-primary",
      "--judge-validator",
      "review-secondary:fixture-secondary",
      "--judge-artifact",
      "object:" + workspace.artifact.string(),
  }, workspace.codex_directory);
}

[[nodiscard]] draft::ResolutionStoreKey app_store_key() {
  return {
      draft::make_aarch64_macos_profile().facts.identity,
      {"workspace", "app"},
  };
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

// The public evidence store intentionally has no "selected evidence" index:
// evidence is append-only history keyed by the exact program, claim, target,
// policy, artifacts, and validator implementations. This test enumerates the
// store's state files only to discover those opaque keys after exercising the
// public command. Production code already knows each key from the command
// result and must not depend on filesystem enumeration order.
[[nodiscard]] std::vector<draft::Sha256Digest> judgment_evidence_keys(
    const TemporaryWorkspace &workspace) {
  std::vector<draft::Sha256Digest> keys;
  const std::filesystem::path directory =
      workspace.root / ".draft" / "evidence";
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error) || error) return keys;
  for (const std::filesystem::directory_entry &entry :
       std::filesystem::directory_iterator(directory, error)) {
    if (error) return {};
    if (!entry.is_regular_file(error) || error ||
        entry.path().extension() != ".state") {
      error.clear();
      continue;
    }
    const std::optional<draft::Sha256Digest> key =
        draft::Sha256Digest::from_hex(entry.path().stem().string());
    if (!key.has_value()) return {};
    keys.push_back(*key);
  }
  std::sort(
      keys.begin(), keys.end(),
      [](const draft::Sha256Digest &left,
         const draft::Sha256Digest &right) {
        return left.bytes < right.bytes;
      });
  return keys;
}

void test_passing_command_selects_evidence(TestState &state) {
  TemporaryWorkspace workspace("pass", true);
  EXPECT(state, run_judge(workspace, {}, true) == 0);

  draft::DiagnosticSink diagnostics;
  draft::ResolutionManifestLoadResult loaded =
      draft::load_resolution_manifest(
          workspace.root, app_store_key(), diagnostics);
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

  // Exact stable identity selects one site. Listing and selection both operate
  // before provider configuration, and neither command mutates resolution.
  EXPECT(state, run_judge(workspace, {sites.front().site_identity}) == 0);
  diagnostics = {};
  loaded = draft::load_resolution_manifest(
      workspace.root, app_store_key(), diagnostics);
  EXPECT(state,
      loaded.state == draft::ResolutionManifestLoadState::Missing);
  std::vector<draft::Sha256Digest> keys = judgment_evidence_keys(workspace);
  EXPECT(state, keys.size() == 1);
  for (const draft::Sha256Digest &key : keys) {
    draft::JudgmentEvidenceState evidence;
    EXPECT(state,
        draft::load_judgment_evidence_state(
            workspace.root, key, evidence, diagnostics));
    EXPECT(state,
        evidence.status == draft::JudgmentEvidenceStateStatus::Active);
    EXPECT(state, evidence.active_evidence.has_value());
  }
  EXPECT(state, !diagnostics.has_errors());

  // The default package command selects all sites. It appends a second attempt
  // for the existing exact key and creates the other site's independent key.
  EXPECT(state, run_judge(workspace) == 0);
  diagnostics = {};
  loaded = draft::load_resolution_manifest(
      workspace.root, app_store_key(), diagnostics);
  EXPECT(state,
      loaded.state == draft::ResolutionManifestLoadState::Missing);
  keys = judgment_evidence_keys(workspace);
  EXPECT(state, keys.size() == 2);
  for (const draft::Sha256Digest &key : keys) {
    draft::JudgmentEvidenceState evidence;
    EXPECT(state,
        draft::load_judgment_evidence_state(
            workspace.root, key, evidence, diagnostics));
    EXPECT(state, evidence.active_evidence.has_value());
    if (evidence.active_evidence.has_value() &&
        evidence.active_evidence->claim.site_identity ==
            sites.front().site_identity) {
      EXPECT(state, evidence.attempts.size() == 2);
    } else {
      EXPECT(state, evidence.attempts.size() == 1);
    }
    EXPECT(state, evidence.active_digest.has_value());
  }
  EXPECT(state, !diagnostics.has_errors());

  // A new failing attempt revokes one exact selected key. Evidence history is
  // useful to the judgment command and release tooling even though ordinary
  // builds do not consume it as a prerequisite.
  draft::DiagnosticSink revocation_diagnostics;
  if (!keys.empty()) {
    draft::JudgmentEvidenceState evidence;
    EXPECT(state,
        draft::load_judgment_evidence_state(
            workspace.root,
            keys.front(),
            evidence,
            revocation_diagnostics));
    if (evidence.active_evidence.has_value()) {
      draft::JudgmentEvidence failed = *evidence.active_evidence;
      failed.passed = false;
      failed.validators.front().passed = false;
      failed.validators.front().rationale = "fixture revocation";
      const draft::JudgmentEvidenceCommitResult revoked =
          draft::commit_judgment_evidence(
              workspace.root, std::move(failed), revocation_diagnostics);
      EXPECT(state, revoked.ok);
      EXPECT(state, !revoked.active);
    }
    draft::JudgmentEvidenceState revoked_state;
    EXPECT(state, draft::load_judgment_evidence_state(
        workspace.root,
        keys.front(),
        revoked_state,
        revocation_diagnostics));
    EXPECT(state,
        revoked_state.status == draft::JudgmentEvidenceStateStatus::Revoked);
  }
  EXPECT(state, !revocation_diagnostics.has_errors());
}

void test_failing_command_records_independent_evidence(TestState &state) {
  TemporaryWorkspace workspace("fail", false);
  EXPECT(state, run_judge(workspace) == 1);
  draft::DiagnosticSink diagnostics;
  const draft::ResolutionManifestLoadResult loaded =
      draft::load_resolution_manifest(
          workspace.root, app_store_key(), diagnostics);
  EXPECT(state,
      loaded.state == draft::ResolutionManifestLoadState::Missing);
  const std::vector<draft::Sha256Digest> keys =
      judgment_evidence_keys(workspace);
  EXPECT(state, keys.size() == 2);
  for (const draft::Sha256Digest &key : keys) {
    draft::JudgmentEvidenceState evidence;
    EXPECT(state, draft::load_judgment_evidence_state(
        workspace.root, key, evidence, diagnostics));
    EXPECT(state,
        evidence.status == draft::JudgmentEvidenceStateStatus::Revoked);
  }
  EXPECT(state, !diagnostics.has_errors());
}

void test_public_multi_validator_artifact_policy(TestState &state) {
  TemporaryWorkspace workspace("multi-policy", true);
  EXPECT(state, run_multi_judge(workspace) == 0);

  draft::DiagnosticSink diagnostics;
  const draft::ResolutionManifestLoadResult loaded =
      draft::load_resolution_manifest(
          workspace.root, app_store_key(), diagnostics);
  EXPECT(state,
      loaded.state == draft::ResolutionManifestLoadState::Missing);
  const std::vector<draft::Sha256Digest> keys =
      judgment_evidence_keys(workspace);
  EXPECT(state, keys.size() == 2);

  const draft::Sha256Digest artifact_digest =
      draft::sha256("public judgment artifact bytes");
  for (const draft::Sha256Digest &key : keys) {
    draft::JudgmentEvidenceState evidence;
    EXPECT(state, draft::load_judgment_evidence_state(
        workspace.root, key, evidence, diagnostics));
    EXPECT(state, evidence.active_evidence.has_value());
    if (evidence.active_evidence.has_value()) {
      EXPECT(state, evidence.active_evidence->validators.size() == 2);
      if (evidence.active_evidence->validators.size() == 2) {
        EXPECT(state,
            evidence.active_evidence->validators[0].validator_identity ==
                "review-primary");
        EXPECT(state,
            evidence.active_evidence->validators[0].model_identity ==
                "fixture-primary");
        EXPECT(state,
            evidence.active_evidence->validators[1].validator_identity ==
                "review-secondary");
        EXPECT(state,
            evidence.active_evidence->validators[1].model_identity ==
                "fixture-secondary");
      }
      EXPECT(state, evidence.active_evidence->artifacts.size() == 1);
      if (evidence.active_evidence->artifacts.size() == 1) {
        EXPECT(state,
            evidence.active_evidence->artifacts.front().kind == "object");
        EXPECT(state,
            evidence.active_evidence->artifacts.front().content_digest ==
                artifact_digest);
      }
    }
  }

  EXPECT(state, !diagnostics.has_errors());
}

} // namespace

int main() {
  TestState state;
  test_passing_command_selects_evidence(state);
  test_failing_command_records_independent_evidence(state);
  test_public_multi_validator_artifact_policy(state);
  if (state.failures != 0) {
    std::cerr << state.failures
              << " judgment driver expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all judgment driver tests passed\n";
  return EXIT_SUCCESS;
}
